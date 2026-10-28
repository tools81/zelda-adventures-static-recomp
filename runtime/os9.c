/* High-level emulation of the OS-9/68K (CD-RTOS) kernel: processes, signals,
   events, alarms, trap handler modules, modules and I/O paths. */
#include "recomp.h"
#include "runtime.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#endif

/* OS-9 error codes */
#define E_PTHFUL 0xC8
#define E_BPNUM_ 0xC9
#define E_BMODE_ 0xCB
#define E_MEMFUL 0xCF
#define E_UNKSVC_ 0xD0
#define E_EOF_   0xD3
#define E_PNNF_  0xD8
#define E_MNF    0xDD
#define E_NOCHLD 0xE2
#define E_IPRCID 0xE0

#define MODULE_BASE MAIN_MODULE_BASE   /* fixed load address of the recompiled main program */

/* modules that were statically recompiled for a fixed address (game.json) */
static uint32_t fixed_module_base(const char *name) {
    for (int i = 0; game_fixed_modules[i].name; i++)
        if (!_stricmp(game_fixed_modules[i].name, name)) return game_fixed_modules[i].base;
    return 0;
}

/* ------------------------------------------------------------ allocator */
typedef struct Block { uint32_t addr, size; struct Block *next; } Block;
static Block *free_list;
uint32_t ram_top = HEAP_BASE;       /* heap high-water mark: snapshots save the RAM below it */

static void heap_init(void) {
    free_list = (Block *)calloc(1, sizeof(Block));
    free_list->addr = HEAP_BASE;
    free_list->size = HEAP_END - HEAP_BASE;
}

uint32_t mem_alloc(uint32_t size, uint32_t *actual) {
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;
    for (Block **pp = &free_list; *pp; pp = &(*pp)->next) {
        Block *b = *pp;
        if (b->size >= size) {
            uint32_t a = b->addr;
            b->addr += size;
            b->size -= size;
            if (b->size == 0) { *pp = b->next; free(b); }
            memset(RAM + a, 0, size);
            if (a + size > ram_top) ram_top = a + size;
            if (actual) *actual = size;
            return a;
        }
    }
    return 0;
}

void mem_free(uint32_t addr, uint32_t size) {
    size = (size + 15) & ~15u;
    if (!addr || !size) return;
    Block *nb = (Block *)calloc(1, sizeof(Block));
    nb->addr = addr; nb->size = size;
    Block **pp = &free_list;
    while (*pp && (*pp)->addr < addr) pp = &(*pp)->next;
    nb->next = *pp;
    *pp = nb;
    /* coalesce */
    for (Block *b = free_list; b && b->next;) {
        if (b->addr + b->size == b->next->addr) {
            Block *n = b->next;
            b->size += n->size;
            b->next = n->next;
            free(n);
        } else b = b->next;
    }
}

/* ------------------------------------------------------------ processes */
#define MAXSIG 64
#define MAXPROC 8

typedef struct {
    uint32_t d[8], a[8], pc;
    uint8_t ccr, srh;
} Context;

enum { PS_FREE, PS_READY, PS_SLEEP, PS_EVWAIT, PS_CHWAIT, PS_DEAD };

/* Per-process kernel state.  A process gives up the CPU only at the top of
   the run loop: at a yield point of translated code (backward branch), or in
   a system call that unwinds the native calls back to the run loop
   (proc_block) and restarts at the os9 instruction when the process runs
   again.  So whenever the scheduler runs, every process is fully described
   by this struct (no native stacks to keep), which makes emulator snapshots
   possible, and execution stays deterministic. */
typedef struct {
    int used, pid, ppid, prio, age, state;
    M68K regs;                      /* CPU state while the process is not running */
    int64_t wake_at;                /* F$Sleep timeout */
    int sleep_active, woken;        /* F$Sleep restarted after blocking; woken by S$Wake */
    int ev_active;                  /* Ev$Wait restarted after blocking */
    int ev_idx, ev_woken;           /* Ev$Wait */
    int32_t ev_min, ev_max, ev_val;
    uint32_t ev_seq;
    uint32_t status;                /* F$Exit status (zombie until the parent waits) */
    uint32_t data, datasz;          /* static storage + stack */
    int sys;                        /* >0 while a system-state routine runs on behalf of it */
    /* signals */
    uint32_t q[MAXSIG];
    int nq;
    uint32_t handler, handler_a6;
    int mask;                       /* F$SigMask level + in-handler */
    Context saved[16];
    int nsaved;
    /* trap handler modules (F$TLink) */
    uint32_t trap_entry[16], trap_a6[16];
} Proc;
static Proc procs[MAXPROC];
static Proc *cur;
static int64_t slice_start;
static volatile int sched_stop;     /* the running process must return to the scheduler */
static int fields_pending;          /* fields rendered since the last frame boundary */
static unsigned sched_rr;           /* round-robin start for equal priorities */
static int in_alarm;                /* a system-state alarm routine is running */
#define SLICE_CYC (CPU_IPS / 50)    /* 20 ms time slice between processes of equal priority */

#define sigq (cur->q)
#define sig_pending_count (cur->nq)
#define icpt_pc (cur->handler)
#define icpt_a6 (cur->handler_a6)
#define sig_masked (cur->mask)
#define ctx_stack (cur->saved)
#define ctx_depth (cur->nsaved)
/* signals are not delivered while kernel code runs a system-state routine */
#define CAN_DELIVER() (cur && !in_alarm && !cur->sys && cur->nq && !cur->mask && cur->handler)


int os9_cur_pid(void) { return cur ? cur->pid : 1; }

static Proc *proc_by_pid(int pid) {
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].used && procs[i].pid == pid) return &procs[i];
    return NULL;
}

static int blocked(const Proc *p) {
    return p->state == PS_SLEEP || p->state == PS_EVWAIT || p->state == PS_CHWAIT;
}

static void proc_kill(Proc *p, uint32_t status);

void sig_send_pid(int pid, uint32_t sig) {
    Proc *p = proc_by_pid(pid);
    if (!p || p->state == PS_DEAD) return;
    if (sig == 0) {                     /* S$Kill */
        LOG("pid %d killed", pid);
        proc_kill(p, 0);
        return;
    }
    if (sig == 1) {                     /* S$Wake: wake up, no intercept routine */
        if (blocked(p)) { p->state = PS_READY; p->woken = 1; }
        return;
    }
    if (p->nq < MAXSIG) p->q[p->nq++] = sig;
    /* a blocked process that can take the signal wakes up to run its handler */
    if (blocked(p) && (!p->mask || p->state == PS_SLEEP)) p->state = PS_READY;
    TRACE("signal %u queued for pid %d", sig, pid);
}

void sig_send(uint32_t sig) { sig_send_pid(os9_cur_pid(), sig); }

static void push_context(uint32_t resume_pc) {
    if (ctx_depth >= 16) fatal("signal context overflow");
    Context *c = &ctx_stack[ctx_depth++];
    memcpy(c->d, m68k.d, sizeof c->d);
    memcpy(c->a, m68k.a, sizeof c->a);
    c->pc = resume_pc;
    c->ccr = GET_CCR();
    c->srh = m68k.srh;
}

/* Deliver one pending signal: m68k.pc must hold the resume address. */
int sig_deliver(void) {
    if (!CAN_DELIVER()) return 0;
    uint32_t sig = sigq[0];
    memmove(sigq, sigq + 1, (--sig_pending_count) * sizeof(uint32_t));
    push_context(m68k.pc);
    D(1) = sig;
    A(6) = icpt_a6;
    A(7) -= 0x50;           /* the kernel builds a frame on the user stack */
    m68k.pc = icpt_pc;
    sig_masked++;
    TRACE("pid %d: deliver signal %u -> %06x", cur->pid, sig, icpt_pc);
    return 1;
}

static uint32_t do_rte(void) {
    if (!ctx_depth) fatal("F$RTE without signal context");
    Context *c = &ctx_stack[--ctx_depth];
    memcpy(m68k.d, c->d, sizeof c->d);
    memcpy(m68k.a, c->a, sizeof c->a);
    SET_CCR(c->ccr);
    m68k.srh = c->srh;
    if (sig_masked > 0) sig_masked--;
    return c->pc;
}

/* ------------------------------------------------------------ alarms */
typedef struct {
    int used, pid;
    uint32_t id, sig;
    uint32_t regimg;                /* system-state alarm: register image (R$pc at +66) */
    int64_t when, period;
} Alarm;
static Alarm alarms[16];
static uint32_t next_alarm_id = 1;
#define TICKS_PER_SEC 100
#define TICK_CYC (CPU_IPS / TICKS_PER_SEC)

static void run_system_routine(uint32_t pc, int alarm);

static void alarms_check(void) {
    if (in_alarm) return;
    int64_t now = m68k.cyc;
    for (int i = 0; i < 16; i++) {
        Alarm *al = &alarms[i];
        if (!al->used || now < al->when) continue;
        if (al->period > 0) al->when += al->period;
        else al->used = 0;
        if (al->regimg) {
            /* system-state alarm: run the routine with the registers of the image */
            uint32_t img = al->regimg;
            M68K saved = m68k;
            for (int k = 0; k < 8; k++) { D(k) = RL(img + k * 4); A(k) = RL(img + 32 + k * 4); }
            run_system_routine(RL(img + 66), 1);
            int64_t cyc = m68k.cyc;
            m68k = saved;
            m68k.cyc = cyc;
        } else {
            sig_send_pid(al->pid, al->sig);
        }
    }
}

static int64_t alarms_next(void) {
    int64_t n = INT64_MAX;
    for (int i = 0; i < 16; i++)
        if (alarms[i].used && alarms[i].when < n) n = alarms[i].when;
    return n;
}

/* ------------------------------------------------------------ events (F$Event) */
#define MAXEV 32
#define E_EVNF   0xA7
#define E_EVBUSY 0xA8
typedef struct {
    int used;
    uint32_t id;
    char name[32];
    int32_t value;
    int16_t winc, sinc;
    int links;
} Event;
static Event events[MAXEV];
static uint32_t ev_seq_counter;
static int line_event = -1;

static int ev_find_name(const char *nm) {
    for (int i = 0; i < MAXEV; i++)
        if (events[i].used && !_stricmp(events[i].name, nm)) return i;
    return -1;
}

static int ev_find_id(uint32_t id) {
    for (int i = 0; i < MAXEV; i++)
        if (events[i].used && events[i].id == id) return i;
    return -1;
}

static int ev_create(const char *nm, int32_t value, int16_t winc, int16_t sinc) {
    for (int i = 0; i < MAXEV; i++) {
        if (events[i].used) continue;
        Event *e = &events[i];
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->id = ((uint32_t)(i + 1) << 16) | (uint32_t)i;
        snprintf(e->name, sizeof e->name, "%s", nm);
        e->value = value; e->winc = winc; e->sinc = sinc;
        e->links = 1;
        return i;
    }
    return -1;
}

/* activate waiters whose range holds the event value: the oldest one, or all */
static void ev_activate(int idx, int all) {
    Event *e = &events[idx];
    for (;;) {
        Proc *best = NULL;
        for (int i = 0; i < MAXPROC; i++) {
            Proc *p = &procs[i];
            if (!p->used || p->state != PS_EVWAIT || p->ev_idx != idx) continue;
            if (e->value < p->ev_min || e->value > p->ev_max) continue;
            if (!best || p->ev_seq < best->ev_seq) best = p;
        }
        if (!best) break;
        best->ev_val = e->value;
        best->ev_woken = 1;
        best->state = PS_READY;
        e->value += e->winc;
        if (!all) break;
    }
}

/* ------------------------------------------------------------ scheduler */
static int64_t next_field, next_sector;
static int64_t field_period, sector_period;

void sched_init(void) {
    field_period = CPU_IPS / FIELD_RATE;
    sector_period = CPU_IPS / SECTOR_RATE;
    next_field = field_period;
    next_sector = sector_period;
    m68k.cyc_limit = next_sector < next_field ? next_sector : next_field;
}

int64_t sched_now(void) { return m68k.cyc; }

int64_t audio_next_event(void);
void audio_event(void);

static void field_tick(void) {
    /* the system's vertical-blank event: wake everything waiting for the next field */
    if (line_event >= 0) {
        Event *e = &events[line_event];
        int32_t v = e->value;
        e->value = 1;
        ev_activate(line_event, 1);
        e->value = v;
    }
}

static void run_events(void) {
    for (;;) {
        int progressed = 0;
        if (m68k.cyc >= audio_next_event()) { audio_event(); progressed = 1; }
        if (m68k.cyc >= next_sector) { next_sector += sector_period; cdfm_sector_tick(); progressed = 1; }
        if (m68k.cyc >= next_field) {
            next_field += field_period;
            video_field();
            audio_field_tick();
            field_tick();
            fields_pending++;
            if (cur) sched_stop = 1;    /* reach the frame boundary promptly */
            progressed = 1;
        }
        alarms_check();
        if (!progressed) break;
    }
    int64_t lim = next_field;
    if (next_sector < lim) lim = next_sector;
    int64_t an = alarms_next();
    if (an < lim) lim = an;
    an = audio_next_event();
    if (an < lim) lim = an;
    for (int i = 0; i < MAXPROC; i++) {
        Proc *p = &procs[i];
        if (!p->used || p->state != PS_SLEEP) continue;
        if (p->wake_at <= m68k.cyc) p->state = PS_READY;
        else if (p->wake_at < lim) lim = p->wake_at;
    }
    m68k.cyc_limit = lim;
}

/* the best runnable process other than the current one */
static Proc *best_ready(void) {
    Proc *best = NULL;
    for (int i = 0; i < MAXPROC; i++) {
        Proc *p = &procs[i];
        if (!p->used || p->state != PS_READY || p == cur) continue;
        if (!best || p->prio + p->age > best->prio + best->age) best = p;
    }
    return best;
}

/* Block the current process.  The system call returns PC_UNWIND, which makes
   every native caller return up to the run loop; the process resumes at
   restart_pc (normally the os9 instruction, so the call runs again and sees
   that it was woken up). */
static uint32_t proc_block(int state, uint32_t restart_pc) {
    if (!cur) fatal("blocking system call outside a process");
    if (in_alarm || cur->sys) fatal("blocking system call in system state");
    cur->state = state;
    m68k.pc = restart_pc;
    sched_stop = 1;
    return PC_UNWIND;
}

/* switch to a higher-priority process that became ready, or round-robin
   when the time slice is used up */
static int preempt_wanted(void) {
    if (!cur || in_alarm || cur->sys || cur->state != PS_READY) return 0;
    Proc *b = best_ready();
    if (!b) return 0;
    int mine = cur->prio;
    int slice_over = m68k.cyc - slice_start >= SLICE_CYC;
    if (b->prio + b->age > mine || (slice_over && b->prio + b->age >= mine)) return 1;
    if (slice_over) {
        /* aging lets lower-priority processes run eventually */
        for (int i = 0; i < MAXPROC; i++)
            if (procs[i].used && procs[i].state == PS_READY && &procs[i] != cur) procs[i].age++;
        slice_start = m68k.cyc;
    }
    return 0;
}

void sched_service(void) {
    run_events();
    if (CAN_DELIVER()) sig_deliver();
    if (preempt_wanted()) sched_stop = 1;
}

/* nothing can run: advance time to the next event */
void sched_wait_event(void) {
    if (m68k.cyc < m68k.cyc_limit) m68k.cyc = m68k.cyc_limit;
    run_events();
}

static void run_proc(Proc *p) {
    int64_t cyc = m68k.cyc, lim = m68k.cyc_limit;
    m68k = p->regs;
    m68k.cyc = cyc;
    m68k.cyc_limit = lim;
    cur = p;
    p->age = 0;
    slice_start = cyc;
    sched_stop = 0;
    cpu_run_until(&sched_stop);
    cur = NULL;
    if (p->used && p->state != PS_DEAD) p->regs = m68k;
}

/* one scheduling decision: run the best process until it gives up the CPU,
   or advance time when everything is blocked */
static void sched_step(void) {
    Proc *best = NULL;
    for (int k = 0; k < MAXPROC; k++) {
        Proc *p = &procs[(sched_rr + k) % MAXPROC];
        if (!p->used || p->state != PS_READY) continue;
        if (!best || p->prio + p->age > best->prio + best->age) best = p;
    }
    if (!best) {
        sched_wait_event();
        return;
    }
    sched_rr = (unsigned)(best - procs) + 1;
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].used && procs[i].state == PS_READY && &procs[i] != best) procs[i].age++;
    run_proc(best);
    /* keep the event horizon right after a switch */
    if (m68k.cyc >= m68k.cyc_limit) run_events();
}

/* Main loop.  After every field the host gets a frame boundary (input,
   presentation, snapshots, run-ahead): at that point no process is running
   and the whole emulator state is in plain data. */
void os9_run(void) {
    for (;;) {
        sched_step();
        while (fields_pending) {
            fields_pending--;
            frame_boundary();
        }
    }
}

/* Run-ahead: emulate n more fields without frame boundaries. */
void os9_run_fields(int n) {
    int done = 0;
    while (done < n) {
        sched_step();
        done += fields_pending;
        fields_pending = 0;
    }
}

/* ------------------------------------------------------------ system-state routines */
/* Kernel scratch area above the heap: stub code and the system stack. */
#define KERNEL_AREA   0x00F00000u
#define K_SYSRET      (KERNEL_AREA + 0x00)    /* os9 $F0: end of a system routine */
#define K_DRV_SETSTT  (KERNEL_AREA + 0x10)    /* os9 I$SetStt; rts */
#define K_DRV_GETSTT  (KERNEL_AREA + 0x18)    /* os9 I$GetStt; rts */
#define K_DRVSTATIC   (KERNEL_AREA + 0x100)   /* fake driver static storage */
#define K_REGIMG      (KERNEL_AREA + 0x200)   /* caller register image (R$d0..R$a7, R$sr, R$pc) */
#define K_SYSSTACK    (KERNEL_AREA + 0x10000) /* system stack (grows down) */
#define K_ALARMSTACK  (KERNEL_AREA + 0x18000)

static volatile int *sysret_flag;

static void kernel_area_init(void) {
    WW(K_SYSRET, 0x4E40); WW(K_SYSRET + 2, 0x00F0);
    WW(K_DRV_SETSTT, 0x4E40); WW(K_DRV_SETSTT + 2, 0x008E); WW(K_DRV_SETSTT + 4, 0x4E75);
    WW(K_DRV_GETSTT, 0x4E40); WW(K_DRV_GETSTT + 2, 0x008D); WW(K_DRV_GETSTT + 4, 0x4E75);
    WL(K_DRVSTATIC + 70, K_DRV_SETSTT);
    WL(K_DRVSTATIC + 74, K_DRV_GETSTT);
}

/* Run 68k code at pc to its final rts, in "system state" (no signal delivery,
   no process switches).  The caller sets up the registers except a7. */
static void run_system_routine(uint32_t pc, int alarm) {
    Proc *saved_cur = cur;
    if (!cur) cur = proc_by_pid(1);
    if (alarm) in_alarm++; else cur->sys++;
    A(7) = alarm ? K_ALARMSTACK : (cur->sys > 1 ? A(7) - 0x400 : K_SYSSTACK);
    A(7) -= 4;
    WL(A(7), K_SYSRET);
    volatile int done = 0;
    volatile int *prev = sysret_flag;
    sysret_flag = &done;
    m68k.pc = pc;
    cpu_run_until(&done);
    sysret_flag = prev;
    if (alarm) in_alarm--; else cur->sys--;
    cur = saved_cur;
}

/* ------------------------------------------------------------ trap handler modules */
uint32_t m68k_trap(int n, uint32_t func, uint32_t next) {
    if (!cur || !cur->trap_entry[n])
        fatal("trap #%d (function %x) at %06x with no trap handler linked", n, func, next - 4);
    /* frame seen by the handler: caller's a6, function code, vector, return pc */
    A(7) -= 4; WL(A(7), next);
    A(7) -= 2; WW(A(7), 32 + n);
    A(7) -= 2; WW(A(7), func);
    A(7) -= 4; WL(A(7), A(6));
    A(6) = cur->trap_a6[n];
    return cur->trap_entry[n];
}

/* ------------------------------------------------------------ paths */
enum { P_NONE, P_CONS, P_CD, P_NVR, P_VIDEO, P_PTR, P_AUDIO, P_NULL, P_KB };
typedef struct {
    int type;
    int handle;          /* CDFM handle */
    int nvr_file;        /* NVR file: host path + position, opened per operation so that
                            the path table holds no host handles (emulator snapshots) */
    uint32_t pos;
    char host[300];
    char name[128];
} Path;
#define MAXPATH 32
static Path paths[MAXPATH];
char g_data_dir[512] = ".";

static void read_cstr(uint32_t a, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t c = RB(a + i);
        if (c == 0 || c == '\r' || c == '\n' || c == ' ') break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static uint32_t nvr_file_size(const Path *P) {
    FILE *fp = fopen(P->host, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fclose(fp);
    return n > 0 ? (uint32_t)n : 0;
}

static int path_alloc(void) {
    for (int i = 3; i < MAXPATH; i++)
        if (paths[i].type == P_NONE) return i;
    return -1;
}

static void nvr_host_path(const char *name, char *out, size_t n) {
    /* name is the part after "/nvr/" */
    snprintf(out, n, "%s/nvr_%s", g_data_dir, name);
}

#define NVR_SIZE 8192
static uint32_t nvr_free_bytes(void) {
    /* 8 KB NVRAM minus the files the game created (the CSD is kept outside the budget) */
    long used = 0;
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s/nvr_*", g_data_dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do {
            if (_stricmp(fd.name, "nvr_csd")) used += (long)fd.size + 64;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#endif
    return used >= NVR_SIZE ? 0 : (uint32_t)(NVR_SIZE - used);
}

/* returns path number or -error */
static int do_open(const char *pname, int mode, int create) {
    char lower[128];
    size_t L = strlen(pname);
    if (L >= sizeof lower) L = sizeof lower - 1;
    for (size_t i = 0; i <= L; i++) lower[i] = (char)tolower((unsigned char)pname[i]);
    lower[L] = 0;
    int p = path_alloc();
    if (p < 0) return -E_PTHFUL;
    Path *P = &paths[p];
    memset(P, 0, sizeof *P);
    snprintf(P->name, sizeof P->name, "%s", pname);

    const char *nm = lower;
    if (!strncmp(nm, "/nvr", 4)) {
        const char *f = nm + 4;
        if (*f == '/') f++;
        if (!*f) { P->type = P_NVR; return p; }     /* directory itself */
        nvr_host_path(f, P->host, sizeof P->host);
        FILE *fp = create ? fopen(P->host, "w+b") : fopen(P->host, "rb");
        if (!fp) { P->type = P_NONE; return -E_PNNF_; }
        fclose(fp);
        P->type = P_NVR;
        P->nvr_file = 1;
        P->pos = 0;
        return p;
    }
    if (!strncmp(nm, "/vid", 4) || !strncmp(nm, "/video", 6) || !strncmp(nm, "/dev/vid", 8)) {
        P->type = P_VIDEO;
        return p;
    }
    if (!strncmp(nm, "/ptr", 4) || !strncmp(nm, "/cdikeys", 8) || !strncmp(nm, "/pt", 3)) {
        P->type = P_PTR;
        return p;
    }
    if (!strncmp(nm, "/ap", 3) || !strncmp(nm, "/audio", 6) || !strncmp(nm, "/adpcm", 6)) {
        P->type = P_AUDIO;
        return p;
    }
    if (!strncmp(nm, "/nil", 4) || !strncmp(nm, "/term", 5) || !strncmp(nm, "/t1", 3)) {
        P->type = P_NULL;
        return p;
    }
    if (!strncmp(nm, "/kb", 3)) {
        P->type = P_KB;
        return p;
    }
    /* CD files: "/cd/name" or plain "name" relative to /cd */
    const char *f = nm;
    if (!strncmp(f, "/cd", 3)) {
        f += 3;
        if (*f == '/') f++;
        if (!*f) { P->type = P_AUDIO; return p; }   /* "/cd" device itself: audio functions */
    } else if (*f == '/') {
        LOG("I$Open of unknown device '%s'", pname);
        P->type = P_NONE;
        return -E_PNNF_;
    }
    int h = cdfm_open(f, mode);
    if (h < 0) { P->type = P_NONE; return -E_PNNF_; }
    P->type = P_CD;
    P->handle = h;
    return p;
}

/* ------------------------------------------------------------ modules */
typedef struct { char name[32]; uint32_t addr; uint32_t size; int links; } ModEntry;
static ModEntry mods[32];
static int nmods;

static uint32_t mod_register(const char *name, uint32_t addr, uint32_t size) {
    if (nmods >= 32) fatal("module directory full");
    ModEntry *m = &mods[nmods++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->addr = addr; m->size = size; m->links = 1;
    return addr;
}

static ModEntry *mod_find(const char *name) {
    for (int i = 0; i < nmods; i++)
        if (!_stricmp(mods[i].name, name)) return &mods[i];
    return NULL;
}

/* a file may contain several modules: register each one */
static ModEntry *mod_register_file(uint32_t a, uint32_t len) {
    uint32_t off = 0;
    ModEntry *first = NULL;
    while (off + 0x30 <= len && RW(a + off) == 0x4AFC) {
        uint32_t sz = RL(a + off + 4);
        char nm[32];
        read_cstr(a + off + RL(a + off + 12), nm, sizeof nm);
        mod_register(nm, a + off, sz);
        if (!first) first = &mods[nmods - 1];
        LOG("module '%s' at %06x size %x type %02x", nm, a + off, sz, RB(a + off + 0x12));
        if (!sz) break;
        off += sz;              /* modules follow each other directly (sizes are even) */
    }
    return first;
}

static ModEntry *mod_load_from_disc(const char *name) {
    ModEntry *m = mod_find(name);
    if (m) { m->links++; return m; }
    const DiscFile *df = disc_find(name);
    if (!df) return NULL;
    uint8_t *buf; uint32_t len;
    if (disc_read_file(df, &buf, &len)) return NULL;
    /* modules that were statically recompiled must sit at their fixed address */
    uint32_t a = fixed_module_base(name);
    if (!a) a = mem_alloc(len, NULL);
    memcpy(RAM + a, buf, len);
    free(buf);
    return mod_register_file(a, len);
}

/* copy a module's initialized data to its static storage and apply irefs */
static void init_static(uint32_t mb, uint32_t data) {
    uint32_t idata = RL(mb + 0x40), irefs = RL(mb + 0x44);
    if (idata) {
        uint32_t doff = RL(mb + idata), cnt = RL(mb + idata + 4);
        memcpy(RAM + data + doff, RAM + mb + idata + 8, cnt);
    }
    if (!irefs) return;
    /* irefs: two tables (code-relative, data-relative) */
    uint32_t o = mb + irefs;
    for (int t = 0; t < 2; t++) {
        for (;;) {
            uint32_t hi = RW(o), n = RW(o + 2);
            o += 4;
            if (!n) break;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t off = (hi << 16) | RW(o);
                o += 2;
                WL(data + off, RL(data + off) + (t == 0 ? mb : data));
            }
        }
    }
}

/* Set up a process for program module mb as the OS-9 kernel's F$Fork does:
   allocate static storage + stack + parameters, copy idata, apply irefs and
   load the initial registers into p->regs. */
static void create_process_state(Proc *p, uint32_t mb, uint32_t params, uint32_t paramsz,
                                 uint32_t extra_mem, int npaths) {
    uint32_t execoff = RL(mb + 0x30);
    uint32_t memsize = RL(mb + 0x38), stacksize = RL(mb + 0x3C);
    uint32_t psz = (paramsz + 15) & ~15u;
    uint32_t total = memsize + stacksize + extra_mem + psz + 0x100;
    uint32_t data = mem_alloc(total, &total);
    if (!data) fatal("out of memory creating process");
    init_static(mb, data);
    M68K *r = &p->regs;
    memset(r, 0, sizeof *r);
    uint32_t top = data + total;
    uint32_t pa = top - psz - 0x10;          /* parameter area at the top */
    for (uint32_t i = 0; i < paramsz; i++) WB(pa + i, RB(params + i));
    r->d[0] = (uint32_t)p->pid;
    r->d[1] = 0;            /* group/user */
    r->d[2] = (uint32_t)p->prio;
    r->d[3] = (uint32_t)npaths;
    r->d[5] = paramsz;
    r->d[6] = total;
    r->a[1] = top;
    r->a[3] = mb;
    r->a[5] = pa;
    r->a[6] = data + 0x8000;
    r->a[7] = pa;
    r->pc = mb + execoff;
    p->data = data;
    p->datasz = total;
}

static int next_pid = 1;

static Proc *proc_new(int ppid, int prio) {
    Proc *p = NULL;
    for (int i = 0; i < MAXPROC; i++) if (!procs[i].used) { p = &procs[i]; break; }
    if (!p) {
        /* recycle a zombie nobody waited for */
        for (int i = 0; i < MAXPROC; i++) if (procs[i].state == PS_DEAD) { p = &procs[i]; break; }
        if (!p) return NULL;
    }
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->pid = next_pid++;
    p->ppid = ppid;
    p->prio = prio;
    p->state = PS_READY;
    return p;
}

/* terminate a process (F$Exit or S$Kill): it stays a zombie until the parent waits */
static void proc_kill(Proc *p, uint32_t status) {
    if (p->pid == 1) {
        LOG("F$Exit(%u) from the main program", status);
        audio_shutdown();
        exit((int)(status & 0xFFFF));
    }
    p->state = PS_DEAD;
    p->status = status & 0xFFFF;
    for (int i = 0; i < 16; i++) if (alarms[i].used && alarms[i].pid == p->pid) alarms[i].used = 0;
    if (p->data) { mem_free(p->data, p->datasz); p->data = 0; }
    Proc *parent = proc_by_pid(p->ppid);
    if (!parent || parent->state == PS_DEAD) p->used = 0;      /* orphan: no zombie */
    else if (parent->state == PS_CHWAIT) parent->state = PS_READY;
    for (int i = 0; i < MAXPROC; i++)
        if (procs[i].used && procs[i].ppid == p->pid) procs[i].ppid = 0;
    if (p == cur) sched_stop = 1;       /* the caller unwinds with PC_UNWIND */
}

void os9_load_main_module(const uint8_t *mod, uint32_t len) {
    memcpy(RAM + MODULE_BASE, mod, len);
    uint32_t mb = MODULE_BASE;
    /* the program file may carry further modules (subroutine, trap handler
       and program modules the game links or forks later) */
    mod_register_file(mb, len);
    Proc *p = proc_new(0, 128);
    create_process_state(p, mb, 0, 0, 0, 3);
    uint32_t data = p->data;
    /* MPW-style programs: the startup code sets a5 = data + 8 + idata[1].
       a6-table programs: a6 = static storage + 0x8000 (set by the kernel) */
    jt_expected = MAIN_JT_REG == 5 ? data + 8 + RL(data + 4) : MAIN_JT_REG == 6 ? data + 0x8000 : 0;
    LOG("main module at %06x, data %06x, jump table reg a%d=%06x, entry %06x", mb, data,
        MAIN_JT_REG, jt_expected, p->regs.pc);
}

static void write_csd(void) {
    char hp[600];
    nvr_host_path("csd", hp, sizeof hp);
    FILE *f = fopen(hp, "wb");
    if (!f) return;
    /* Device Status Descriptors of a typical base-case player */
#if GAME_PAL
#define CSD_LINES "625"
#else
#define CSD_LINES "525"
#endif
    static const char *dsd[] = {
        "0::RV=\"1.1\":", "1:/cd:SP#27:", "2:/ap:AD#27:", "3:/vid:PL#2:LI=\"" CSD_LINES "\":TV:",
        "4:/nvr:SZ#8:", "5:/ptr:CL=\"d\":", "6:CDDRIVE:SK#3:", "8:MONITOR:LI=\"" CSD_LINES "\":",
    };
    for (size_t i = 0; i < sizeof dsd / sizeof dsd[0]; i++) {
        fputs(dsd[i], f);
        fputc(13, f);
    }
    fclose(f);
}

void os9_init(void) {
    heap_init();
    write_csd();
    kernel_area_init();
    paths[0].type = P_CONS;
    paths[1].type = P_CONS;
    paths[2].type = P_CONS;
    /* the video driver's vertical-blank event ("line_event"): programs link it
       and Ev$Wait for value 1; it is pulsed with 1 at every field */
    line_event = ev_create("line_event", 0, 0, 0);
}

/* ------------------------------------------------------------ driver subroutine module */
/* Some games install a subroutine module into the video/pointer drivers with
   SetStat $5A (Philips "cdi_bpsys": pointer, cursor and hot-spot handling).
   It runs in system state and calls the driver back through its static
   storage (70(a2) = SetStat, 74(a2) = GetStat).  We run the module's 68k code
   with a fake driver whose entry points are plain I$SetStt / I$GetStt calls. */
static uint32_t drvsub_base;        /* module address, 0 = none installed */

static void drvsub_regs(int path) {
    D(0) = (uint32_t)path;
    A(1) = 0;                       /* path descriptor: not used by the module */
    A(2) = K_DRVSTATIC;
    A(4) = 0;
    A(5) = K_REGIMG;
    A(6) = 0;
}

static void drvsub_install(const char *name, int path) {
    ModEntry *m = mod_find(name);
    if (!m) { LOG("SetStat $5A: subroutine module '%s' not found", name); return; }
    drvsub_base = m->addr;
    uint32_t entry_tbl = m->addr + RL(m->addr + 0x30);
    uint32_t init = m->addr + RL(m->addr + 0x30) + RW(entry_tbl);
    LOG("driver subroutine module '%s' at %06x installed (init %06x)", name, m->addr, init);
    M68K saved = m68k;
    drvsub_regs(path);
    run_system_routine(init, 0);
    int64_t cyc = m68k.cyc;
    m68k = saved;
    m68k.cyc = cyc;
}

/* SetStat handled by the subroutine module; returns 0 or an error code */
static uint32_t drvsub_setstat(int path) {
    uint32_t entry_tbl = drvsub_base + RL(drvsub_base + 0x30);
    uint32_t entry = entry_tbl + RW(entry_tbl + 2);
    for (int k = 0; k < 8; k++) { WL(K_REGIMG + k * 4, D(k)); WL(K_REGIMG + 32 + k * 4, A(k)); }
    WW(K_REGIMG + 64, GET_SR());
    M68K saved = m68k;
    drvsub_regs(path);
    TRACE("drvsub setstat %x fn %x", D(1) & 0xFFFF, RL(K_REGIMG + 8) & 0xFFFF);
    run_system_routine(entry, 0);
    int err = CF ? (int)(D(1) & 0xFFFF) : 0;
    int64_t cyc = m68k.cyc;
    m68k = saved;
    m68k.cyc = cyc;
    /* the caller's registers are the register image the module updated */
    for (int k = 0; k < 8; k++) { D(k) = RL(K_REGIMG + k * 4); }
    for (int k = 0; k < 7; k++) { A(k) = RL(K_REGIMG + 32 + k * 4); }
    return (uint32_t)err;
}

/* ------------------------------------------------------------ syscalls */
static uint32_t ret_err(uint32_t err, uint32_t next) {
    D(1) = (D(1) & 0xFFFF0000u) | (err & 0xFFFF);
    CF = 1;
    return next;
}
static uint32_t ret_ok(uint32_t next) { CF = 0; return next; }

/* signal delivery and preemption happen in os9_syscall on the way out */
#define CHECK_SIG_RETURN(next) return (next)

#define E_PRCFUL 0xE5

static uint32_t io_getstt(int p, uint32_t next);
static uint32_t io_setstt(int p, uint32_t next);

static uint32_t stack_caller(void) {
    for (int i = 0; i < 24; i++) {
        uint32_t v = RL(A(7) + i * 2);
        if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) return v;
    }
    return 0;
}

static uint32_t sys_event(uint32_t next) {
    uint32_t fn = D(1) & 0xFF;
    int all = (D(1) & 0x8000) != 0;
    int idx;
    switch (fn) {
    case 0: { /* Ev$Link */
        char nm[32]; read_cstr(A(0), nm, sizeof nm);
        idx = ev_find_name(nm);
        if (idx < 0) { LOG("Ev$Link '%s': no such event", nm); return ret_err(E_EVNF, next); }
        events[idx].links++;
        D(0) = events[idx].id;
        while (RB(A(0)) > ' ') A(0)++;
        TRACE("Ev$Link '%s' -> %x", nm, D(0));
        return ret_ok(next);
    }
    case 1: /* Ev$UnLnk */
        idx = ev_find_id(D(0));
        if (idx < 0) return ret_err(E_EVNF, next);
        if (events[idx].links > 0) events[idx].links--;
        return ret_ok(next);
    case 2: { /* Ev$Creat */
        char nm[32]; read_cstr(A(0), nm, sizeof nm);
        if (ev_find_name(nm) >= 0) return ret_err(E_EVBUSY, next);
        idx = ev_create(nm, (int32_t)D(0), (int16_t)D(2), (int16_t)D(3));
        if (idx < 0) return ret_err(E_MEMFUL, next);
        D(0) = events[idx].id;
        while (RB(A(0)) > ' ') A(0)++;
        LOG("Ev$Creat '%s' value %d winc %d sinc %d -> %x", nm, (int32_t)events[idx].value,
            events[idx].winc, events[idx].sinc, D(0));
        return ret_ok(next);
    }
    case 3: { /* Ev$Delet */
        char nm[32]; read_cstr(A(0), nm, sizeof nm);
        idx = ev_find_name(nm);
        if (idx < 0) return ret_err(E_EVNF, next);
        if (events[idx].links > 0) return ret_err(E_EVBUSY, next);
        events[idx].used = 0;
        while (RB(A(0)) > ' ') A(0)++;
        return ret_ok(next);
    }
    case 4: case 5: { /* Ev$Wait / Ev$WaitR */
        idx = ev_find_id(D(0));
        if (idx < 0) return ret_err(E_EVNF, next);
        if (cur->ev_active) {           /* restarted after blocking */
            cur->ev_active = 0;
            if (cur->ev_woken) {
                cur->ev_woken = 0;
                D(1) = (uint32_t)cur->ev_val;
                return ret_ok(next);
            }
            /* woken by a signal: the intercept routine runs, then the wait starts over */
        }
        Event *e = &events[idx];
        int32_t lo = (int32_t)D(2), hi = (int32_t)D(3);
        if (fn == 5) { lo += e->value; hi += e->value; }
        if (e->value >= lo && e->value <= hi) {
            D(1) = (uint32_t)e->value;
            e->value += e->winc;
            return ret_ok(next);
        }
        if (CAN_DELIVER()) {        /* take pending signals first, then wait */
            m68k.pc = next - 4;
            sig_deliver();
            return m68k.pc;
        }
        cur->ev_idx = idx;
        cur->ev_min = lo;
        cur->ev_max = hi;
        cur->ev_woken = 0;
        cur->ev_seq = ++ev_seq_counter;
        cur->ev_active = 1;
        return proc_block(PS_EVWAIT, next - 4);
    }
    case 6: /* Ev$Read */
        idx = ev_find_id(D(0));
        if (idx < 0) return ret_err(E_EVNF, next);
        D(1) = (uint32_t)events[idx].value;
        return ret_ok(next);
    case 7: { /* Ev$Info: d0 = first index to look at, a0 = buffer */
        int from = (int)(D(0) & 0xFFFF);
        for (idx = from; idx < MAXEV; idx++) if (events[idx].used) break;
        if (idx >= MAXEV) return ret_err(E_EVNF, next);
        Event *e = &events[idx];
        uint32_t b = A(0);
        for (int i = 0; i < 32; i++) WB(b + i, 0);
        WL(b, e->id);
        for (int i = 0; i < 11 && e->name[i]; i++) WB(b + 4 + i, (uint8_t)e->name[i]);
        WL(b + 16, (uint32_t)e->value);
        WW(b + 20, (uint16_t)e->winc);
        WW(b + 22, (uint16_t)e->links);
        WW(b + 24, (uint16_t)e->sinc);
        D(0) = (uint32_t)idx;
        return ret_ok(next);
    }
    case 8: /* Ev$Signl */
        idx = ev_find_id(D(0));
        if (idx < 0) return ret_err(E_EVNF, next);
        events[idx].value += events[idx].sinc;
        ev_activate(idx, all);
        CHECK_SIG_RETURN(ret_ok(next));
    case 9: { /* Ev$Pulse */
        idx = ev_find_id(D(0));
        if (idx < 0) return ret_err(E_EVNF, next);
        int32_t v = events[idx].value;
        events[idx].value = (int32_t)D(2);
        ev_activate(idx, all);
        events[idx].value = v;
        CHECK_SIG_RETURN(ret_ok(next));
    }
    case 10: case 11: { /* Ev$Set / Ev$SetR */
        idx = ev_find_id(D(0));
        if (idx < 0) return ret_err(E_EVNF, next);
        int32_t old = events[idx].value;
        events[idx].value = fn == 10 ? (int32_t)D(2) : old + (int32_t)D(2);
        D(1) = (uint32_t)old;
        ev_activate(idx, all);
        CHECK_SIG_RETURN(ret_ok(next));
    }
    }
    LOG("F$Event function %u not implemented", fn);
    return ret_err(E_UNKSVC_, next);
}

static uint32_t syscall_inner(uint32_t code, uint32_t next);

/* OS-9 delivers pending signals on the way out of every system call */
uint32_t os9_syscall(uint32_t code, uint32_t next) {
    uint32_t r = syscall_inner(code, next);
    if (r == PC_UNWIND) return r;
    if (CF && log_level >= 2) TRACE("[%d] os9 %02x -> error %02x", os9_cur_pid(), code, D(1) & 0xFFFF);
    if (CAN_DELIVER()) {
        m68k.pc = r;
        sig_deliver();
        r = m68k.pc;
    }
    if (preempt_wanted()) {     /* e.g. a signal or event woke a higher-priority process */
        m68k.pc = r;
        sched_stop = 1;
        return PC_UNWIND;
    }
    return r;
}

static uint32_t syscall_inner(uint32_t code, uint32_t next) {
    if (log_level >= 2) {
        uint32_t c = stack_caller();
        TRACE("[%d] os9 %02x d0=%08x d1=%08x d2=%08x a0=%06x from %06x caller %06x (%s)", os9_cur_pid(), code,
              D(0), D(1), D(2), A(0), next, c, func_name(c));
        if (log_level >= 3) {
            char bt[512]; int k = 0;
            for (int i = 0; i < 64 && k < 480; i++) {
                uint32_t v = RL(A(7) + i * 2);
                if (v >= recomp_code_lo && v < recomp_code_hi && !(v & 1)) k += sprintf(bt + k, " %06x", v);
            }
            TRACE("   bt:%s", bt);
        }
    }
    switch (code) {
    case 0xF0: /* private: end of a system-state routine (see run_system_routine) */
        if (sysret_flag) *sysret_flag = 1;
        return PC_UNWIND;
    case 0x00: { /* F$Link */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) { LOG("F$Link '%s' failed", nm); return ret_err(E_MNF, next); }
        m->links++;
        A(1) = m->addr + RL(m->addr + 0x30);
        A(2) = m->addr;
        D(0) = RW(m->addr + 0x12) & 0xFFFF;
        D(1) = RW(m->addr + 0x14);
        LOG("F$Link '%s' -> %06x", nm, m->addr);
        return ret_ok(next);
    }
    case 0x01: { /* F$Load */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        const char *base = strrchr(nm, '/');
        base = base ? base + 1 : nm;
        ModEntry *m = mod_load_from_disc(base);
        if (!m) { LOG("F$Load '%s' failed", nm); return ret_err(E_PNNF_, next); }
        A(1) = m->addr + RL(m->addr + 0x30);
        A(2) = m->addr;
        D(0) = RW(m->addr + 0x12) & 0xFFFF;
        D(1) = RW(m->addr + 0x14);
        LOG("F$Load '%s' -> %06x", nm, m->addr);
        return ret_ok(next);
    }
    case 0x02: /* F$UnLink */
        return ret_ok(next);
    case 0x03: { /* F$Fork: the child runs concurrently with its parent */
        char nm[64]; read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) m = mod_load_from_disc(nm);
        if (!m) { LOG("F$Fork '%s': module not found", nm); return ret_err(E_MNF, next); }
        uint32_t entry = m->addr + RL(m->addr + 0x30);
        if (entry < recomp_code_lo || entry >= recomp_code_hi)
            LOG("F$Fork '%s': module at %06x is not recompiled, it will be interpreted", nm, m->addr);
        int prio = (int)(D(0) & 0xFFFF);
        if (!prio) prio = cur->prio;
        Proc *p = proc_new(cur->pid, prio);
        if (!p) return ret_err(E_PRCFUL, next);
        create_process_state(p, m->addr, A(1), D(2), D(1), D(3) & 0xFFFF);
        LOG("F$Fork '%s' -> pid %d priority %d", nm, p->pid, prio);
        D(0) = (uint32_t)p->pid;
        while (RB(A(0)) > ' ') A(0)++;
        CHECK_SIG_RETURN(ret_ok(next));
    }
    case 0x04: /* F$Wait */
        for (;;) {
            int children = 0;
            for (int i = 0; i < MAXPROC; i++) {
                Proc *p = &procs[i];
                if (!p->used || p->ppid != cur->pid) continue;
                children++;
                if (p->state == PS_DEAD) {
                    D(0) = (uint32_t)p->pid;
                    D(1) = p->status;
                    LOG("F$Wait: child pid %d exited (%u)", p->pid, p->status);
                    p->used = 0;
                    return ret_ok(next);
                }
            }
            if (!children) return ret_err(E_NOCHLD, next);
            if (CAN_DELIVER()) {
                D(0) = 0;
                CF = 0;
                m68k.pc = next;
                sig_deliver();
                return m68k.pc;
            }
            return proc_block(PS_CHWAIT, next - 4);
        }
    case 0x05: /* F$Chain */
        fatal("F$Chain not supported");
        return next;
    case 0x06: /* F$Exit */
        LOG("pid %d: F$Exit(%u)", cur->pid, D(1) & 0xFFFF);
        proc_kill(cur, D(1));
        return PC_UNWIND;
    case 0x1D: /* F$UnLoad */
        return ret_ok(next);
    case 0x21: { /* F$TLink: link a trap handler module to trap #d0 */
        int n = (int)(D(0) & 15);
        char nm[64];
        if (!A(0) || !RB(A(0))) { cur->trap_entry[n] = 0; return ret_ok(next); }
        read_cstr(A(0), nm, sizeof nm);
        ModEntry *m = mod_find(nm);
        if (!m) m = mod_load_from_disc(nm);
        if (!m) { LOG("F$TLink #%d '%s': module not found", n, nm); return ret_err(E_MNF, next); }
        uint32_t memsize = RL(m->addr + 0x38) + D(1);
        uint32_t data = mem_alloc(memsize + 16, NULL);
        if (!data) return ret_err(E_MEMFUL, next);
        init_static(m->addr, data);
        cur->trap_entry[n] = m->addr + RL(m->addr + 0x30);
        cur->trap_a6[n] = data + 0x8000;
        A(1) = cur->trap_entry[n];
        A(2) = m->addr;
        LOG("pid %d: F$TLink trap #%d -> '%s' entry %06x", cur->pid, n, nm, cur->trap_entry[n]);
        return ret_ok(next);
    }
    case 0x27: { /* F$SetSys: read/write a system global */
        uint32_t off = D(0) & 0xFFFF, sz = D(1);
        if (sz & 0x80000000u) {
            /* read: D_TckSec (ticks per second) and D_Ticks are what programs ask for */
            D(2) = off == 0x0E ? TICKS_PER_SEC : (uint32_t)(m68k.cyc / TICK_CYC);
            TRACE("F$SetSys read %x -> %u", off, D(2));
        } else {
            TRACE("F$SetSys write %x = %x (ignored)", off, D(2));
        }
        return ret_ok(next);
    }
    case 0x07: /* F$Mem */
        D(0) = 0;
        A(1) = 0;
        return ret_ok(next);
    case 0x08: { /* F$Send */
        int pid = D(0) & 0xFFFF;
        uint32_t sig = D(1) & 0xFFFF;
        if (pid == 0) {             /* broadcast */
            for (int i = 0; i < MAXPROC; i++)
                if (procs[i].used && &procs[i] != cur && procs[i].state != PS_DEAD) sig_send_pid(procs[i].pid, sig);
            CHECK_SIG_RETURN(ret_ok(next));
        }
        if (!proc_by_pid(pid)) return ret_err(E_IPRCID, next);
        if (pid == cur->pid && sig == 0) { proc_kill(cur, 0); return PC_UNWIND; }
        sig_send_pid(pid, sig);
        CHECK_SIG_RETURN(ret_ok(next));
    }
    case 0x09: /* F$Icpt */
        icpt_pc = A(0);
        icpt_a6 = A(6);
        LOG("pid %d: F$Icpt handler %06x a6=%06x", cur->pid, icpt_pc, icpt_a6);
        return ret_ok(next);
    case 0x0A: { /* F$Sleep (clears the signal mask, as OS-9 does) */
        uint32_t ticks = D(0);
        if (cur->sys || in_alarm) return ret_ok(next);   /* system state: just continue */
        if (cur->sleep_active) {        /* restarted after blocking */
            if (CAN_DELIVER()) {
                cur->sleep_active = 0;
                D(0) = 0;
                CF = 0;
                m68k.pc = next;
                sig_deliver();
                return m68k.pc;
            }
            if (m68k.cyc >= cur->wake_at || cur->woken) {
                cur->sleep_active = 0;
                cur->woken = 0;
                D(0) = 0;
                return ret_ok(next);
            }
            return proc_block(PS_SLEEP, next - 4);
        }
        sig_masked = 0;
        CF = 0;
        if (ticks == 1) {               /* give up the rest of the time slice */
            D(0) = 0;
            if (CAN_DELIVER()) { m68k.pc = next; sig_deliver(); return m68k.pc; }
            m68k.pc = next;
            sched_stop = 1;
            return PC_UNWIND;
        }
        if (CAN_DELIVER()) { D(0) = 0; m68k.pc = next; sig_deliver(); return m68k.pc; }
        int64_t len = (ticks & 0x80000000u) ? (int64_t)(ticks & 0x7FFFFFFF) * CPU_IPS / 256
                                            : (int64_t)ticks * TICK_CYC;
        cur->wake_at = ticks ? m68k.cyc + len : INT64_MAX;
        cur->sleep_active = 1;
        cur->woken = 0;
        if (cur->wake_at < m68k.cyc_limit) m68k.cyc_limit = cur->wake_at;
        return proc_block(PS_SLEEP, next - 4);
    }
    case 0x0C: /* F$ID */
        D(0) = (uint32_t)cur->pid;
        D(1) = 0;
        D(2) = (uint32_t)cur->prio;
        return ret_ok(next);
    case 0x0D: { /* F$SPrior */
        Proc *p = proc_by_pid(D(0) & 0xFFFF);
        if (!p) return ret_err(E_IPRCID, next);
        p->prio = (int)(D(1) & 0xFFFF);
        CHECK_SIG_RETURN(ret_ok(next));
    }
    case 0x15: { /* F$Time */
        extern int g_headless;
        /* headless runs use a fixed epoch so test runs are reproducible */
        time_t t = g_headless ? (time_t)(820454400 + m68k.cyc / CPU_IPS) : time(NULL);
        struct tm *tm = localtime(&t);
        D(0) = ((uint32_t)tm->tm_hour << 16) | (tm->tm_min << 8) | tm->tm_sec;
        D(1) = ((uint32_t)(tm->tm_year + 1900) << 16) | ((tm->tm_mon + 1) << 8) | tm->tm_mday;
        D(2) = (TICKS_PER_SEC << 16) | (uint32_t)((m68k.cyc / TICK_CYC) % TICKS_PER_SEC);
        D(3) = (uint32_t)(m68k.cyc / TICK_CYC);
        return ret_ok(next);
    }
    case 0x1E: /* F$RTE */
        return do_rte();
    case 0x28: { /* F$SRqMem */
        uint32_t sz = D(0), act;
        if (sz == 0xFFFFFFFFu) sz = 0x40000;    /* "largest block" request */
        uint32_t a = mem_alloc(sz, &act);
        if (!a) return ret_err(E_MEMFUL, next);
        D(0) = act;
        A(2) = a;
        TRACE("F$SRqMem %x -> %06x", sz, a);
        return ret_ok(next);
    }
    case 0x29: /* F$SRtMem */
        mem_free(A(2), D(0));
        return ret_ok(next);
    case 0x53: /* F$Event */
        return sys_event(next);
    case 0x56: { /* F$Alarm */
        uint32_t fn = D(1) & 0xFFFF;
        if (fn == 0) { /* A$Delete */
            for (int i = 0; i < 16; i++)
                if (alarms[i].used && (D(0) == 0 || alarms[i].id == D(0))) alarms[i].used = 0;
            return ret_ok(next);
        }
        int slot = -1;
        for (int i = 0; i < 16; i++) if (!alarms[i].used) { slot = i; break; }
        if (slot < 0) return ret_err(E_MEMFUL, next);
        Alarm *al = &alarms[slot];
        memset(al, 0, sizeof *al);
        al->used = 1;
        al->pid = os9_cur_pid();
        al->id = next_alarm_id++;
        /* in system state the alarm runs a routine given by a register image */
        if ((cur && cur->sys) || in_alarm) al->regimg = A(0);
        else al->sig = D(2) & 0xFFFF;
        uint32_t t = D(3);
        int64_t cyc;
        if (t & 0x80000000u) cyc = (int64_t)(t & 0x7FFFFFFF) * CPU_IPS / 256; /* seconds/256 */
        else cyc = (int64_t)t * TICK_CYC;
        if (cyc < 1) cyc = 1;
        al->when = m68k.cyc + cyc;
        al->period = (fn == 2) ? cyc : 0;   /* A$Cycle */
        if (al->when < m68k.cyc_limit) m68k.cyc_limit = al->when;
        D(0) = al->id;
        LOG("F$Alarm fn %u %s t %x -> id %u", fn, al->regimg ? "system routine" : "signal", t, al->id);
        return ret_ok(next);
    }
    case 0x57: /* F$SigMask */
        switch (D(1) & 0xFFFF) {
        case 0: sig_masked = 0; break;
        case 1: sig_masked++; break;
        case 0xFFFF: if (sig_masked) sig_masked--; break;
        default: sig_masked = (int)(D(1) & 0xFFFF);
        }
        CHECK_SIG_RETURN(ret_ok(next));
    case 0x5C: { /* F$SRqCMem: colored memory request */
        uint32_t sz = D(0), act;
        if (sz == 0xFFFFFFFFu) sz = 0x40000;
        uint32_t a = mem_alloc(sz, &act);
        if (!a) return ret_err(E_MEMFUL, next);
        D(0) = act;
        A(2) = a;
        TRACE("F$SRqCMem %x type %x -> %06x", sz, D(1), a);
        return ret_ok(next);
    }

    /* ---- I/O */
    case 0x83: case 0x84: { /* I$Create / I$Open */
        char nm[128]; read_cstr(A(0), nm, sizeof nm);
        int r = do_open(nm, D(0) & 0xFF, code == 0x83);
        LOG("I$%s '%s' mode %x -> %d", code == 0x83 ? "Create" : "Open", nm, D(0) & 0xFF, r);
        if (r < 0) return ret_err((uint32_t)-r, next);
        uint32_t a = A(0);
        while (RB(a) > ' ') a++;
        A(0) = a;
        D(0) = (uint32_t)r;
        return ret_ok(next);
    }
    case 0x87: { /* I$Delete */
        char nm[128]; read_cstr(A(0), nm, sizeof nm);
        LOG("I$Delete '%s'", nm);
        if (!_strnicmp(nm, "/nvr/", 5)) {
            char hp[600]; nvr_host_path(nm + 5, hp, sizeof hp);
            remove(hp);
            return ret_ok(next);
        }
        return ret_err(E_PNNF_, next);
    }
    case 0x82: { /* I$Dup */
        int p = path_alloc();
        if (p < 0) return ret_err(E_PTHFUL, next);
        paths[p] = paths[D(0) & 0xFFFF];
        D(0) = (uint32_t)p;
        return ret_ok(next);
    }
    case 0x8F: { /* I$Close */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        if (P->type == P_CD) cdfm_close(P->handle);
        if (p >= 3) memset(P, 0, sizeof *P);
        return ret_ok(next);
    }
    case 0x88: { /* I$Seek */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        if (P->type == P_CD) { cdfm_seek(P->handle, D(1)); return ret_ok(next); }
        if (P->type == P_NVR && P->nvr_file) { P->pos = D(1); return ret_ok(next); }
        return ret_ok(next);
    }
    case 0x89: case 0x8B: { /* I$Read / I$ReadLn */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        uint32_t n = D(1), got = 0;
        if (P->type == P_CD) {
            int e = cdfm_read(P->handle, A(0), n, &got);
            if (e) return ret_err((uint32_t)e, next);
            D(1) = got;
            return ret_ok(next);
        }
        if (P->type == P_NVR && P->nvr_file) {
            FILE *fp = fopen(P->host, "rb");
            if (fp && fseek(fp, (long)P->pos, SEEK_SET) == 0) {
                for (got = 0; got < n; got++) {
                    int c = fgetc(fp);
                    if (c == EOF) break;
                    WB(A(0) + got, (uint32_t)c);
                    if (code == 0x8B && c == '\r') { got++; break; }
                }
            }
            if (fp) fclose(fp);
            P->pos += got;
            if (!got && n) return ret_err(E_EOF_, next);
            D(1) = got;
            return ret_ok(next);
        }
        return ret_err(E_EOF_, next);
    }
    case 0x8A: case 0x8C: { /* I$Write / I$WritLn */
        int p = D(0) & 0xFFFF;
        if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
        Path *P = &paths[p];
        uint32_t n = D(1);
        if (P->type == P_NVR && P->nvr_file) {
            FILE *fp = fopen(P->host, "r+b");
            if (!fp) fp = fopen(P->host, "w+b");
            uint32_t k = 0;
            if (fp) {
                fseek(fp, (long)P->pos, SEEK_SET);
                for (; k < n; k++) {
                    uint8_t c = RB(A(0) + k);
                    fputc(c, fp);
                    if (code == 0x8C && c == '\r') { k++; break; }
                }
                fclose(fp);
            }
            P->pos += k;
            D(1) = k;
            return ret_ok(next);
        }
        if (P->type == P_CONS) {
            char buf[512];
            uint32_t k = 0;
            for (; k < n && k < sizeof buf - 1; k++) {
                char c = (char)RB(A(0) + k);
                buf[k] = c == '\r' ? '\n' : c;
                if (code == 0x8C && c == '\r') { k++; break; }
            }
            buf[k] = 0;
            LOG("[console] %s", buf);
            D(1) = k;
            return ret_ok(next);
        }
        D(1) = n;
        return ret_ok(next);
    }
    case 0x8D: return io_getstt(D(0) & 0xFFFF, next);
    case 0x8E: return io_setstt(D(0) & 0xFFFF, next);
    }
    {
        static uint8_t warned[256];
        if (!warned[code & 0xFF]) {
            warned[code & 0xFF] = 1;
            LOG("unimplemented OS-9 call $%02x at %06x (%s): returning E$UnkSvc", code, next - 4, func_name(next));
        }
    }
    return ret_err(E_UNKSVC_, next);
}

static uint32_t io_getstt(int p, uint32_t next) {
    if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
    Path *P = &paths[p];
    uint32_t fn = D(1) & 0xFFFF;
    int e;
    switch (P->type) {
    case P_CD:
        if (fn == 2) { D(2) = cdfm_size(P->handle); return ret_ok(next); }   /* SS_Size */
        if (fn == 5) { D(2) = cdfm_getpos(P->handle); return ret_ok(next); } /* SS_Pos */
        if (fn == 6) { /* SS_EOF */
            if (cdfm_getpos(P->handle) >= cdfm_size(P->handle)) return ret_err(E_EOF_, next);
            D(1) = 0; return ret_ok(next);
        }
        e = cdfm_getstat(P->handle, fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_NVR:
        if (fn == 2 && P->nvr_file) { D(2) = nvr_file_size(P); return ret_ok(next); }   /* SS_Size */
        if (fn == 5 && P->nvr_file) { D(2) = P->pos; return ret_ok(next); }              /* SS_Pos */
        if (fn == 6 && P->nvr_file) {                                                   /* SS_EOF */
            if (P->pos >= nvr_file_size(P)) return ret_err(E_EOF_, next);
            return ret_ok(next);
        }
        if (fn == 0) { /* SS_Opt */
            for (int i = 0; i < 128; i++) WB(A(0) + i, 0);
            return ret_ok(next);
        }
        if (fn == 0x43) { /* NVRAM free space */
            D(2) = nvr_free_bytes();
            return ret_ok(next);
        }
        LOG("GetStt %x on nvr path", fn);
        return ret_err(E_UNKSVC_, next);
    case P_VIDEO:
        e = ucm_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_PTR:
        e = ucm_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_AUDIO:
        e = audio_getstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_CONS: case P_NULL: case P_KB:
        if (fn == 0) { for (int i = 0; i < 128; i++) WB(A(0) + i, 0); return ret_ok(next); }
        if (fn == 1) return ret_err(0xF6, next); /* SS_Ready: not ready */
        return ret_err(E_UNKSVC_, next);
    }
    return ret_err(E_UNKSVC_, next);
}

static uint32_t io_setstt(int p, uint32_t next) {
    if (p >= MAXPATH || paths[p].type == P_NONE) return ret_err(E_BPNUM_, next);
    Path *P = &paths[p];
    uint32_t fn = D(1) & 0xFFFF;
    int e;
    if (P->type == P_VIDEO || P->type == P_PTR) {
        if (fn == 0x5A) {   /* install a driver subroutine module (a0 = name) */
            char nm[32]; read_cstr(A(0), nm, sizeof nm);
            drvsub_install(nm, p);
            return ret_ok(next);
        }
        uint32_t f2 = D(2) & 0xFFFF;
        if (drvsub_base && f2 >= 0x4200 && f2 < 0x4214) {
            uint32_t err = drvsub_setstat(p);
            if (err) return ret_err(err, next);
            CHECK_SIG_RETURN(ret_ok(next));
        }
    }
    switch (P->type) {
    case P_CD:
        e = cdfm_setstat(P->handle, fn);
        if (e) return ret_err((uint32_t)e, next);
        CHECK_SIG_RETURN(ret_ok(next));
    case P_VIDEO: case P_PTR:
        e = ucm_setstat(fn);
        return e ? ret_err((uint32_t)e, next) : ret_ok(next);
    case P_AUDIO:
        e = audio_setstat(fn);
        if (e) return ret_err((uint32_t)e, next);
        CHECK_SIG_RETURN(ret_ok(next));
    case P_NVR:
        if (fn == 2) return ret_ok(next);   /* SS_Size */
        if (fn == 0) return ret_ok(next);
        LOG("SetStt %x on nvr path", fn);
        return ret_ok(next);
    default:
        return ret_ok(next);
    }
}

/* ------------------------------------------------------------ snapshots */
void os9_state(StateBuf *s) {
    /* taken at frame boundaries only: no process running, no system routine */
    if (cur || in_alarm || sysret_flag) fatal("snapshot while a process is running");
    ST_VAR(s, procs);
    ST_VAR(s, slice_start);
    ST_VAR(s, fields_pending);
    ST_VAR(s, sched_rr);
    ST_VAR(s, alarms);
    ST_VAR(s, next_alarm_id);
    ST_VAR(s, events);
    ST_VAR(s, ev_seq_counter);
    ST_VAR(s, line_event);
    ST_VAR(s, next_field);
    ST_VAR(s, next_sector);
    ST_VAR(s, paths);
    ST_VAR(s, mods);
    ST_VAR(s, nmods);
    ST_VAR(s, drvsub_base);
    ST_VAR(s, next_pid);
    ST_VAR(s, ram_top);
    /* free list */
    uint32_t n = 0;
    for (Block *b = free_list; b; b = b->next) n++;
    ST_VAR(s, n);
    if (!s->loading) {
        for (Block *b = free_list; b; b = b->next) { ST_VAR(s, b->addr); ST_VAR(s, b->size); }
        return;
    }
    while (free_list) { Block *b = free_list; free_list = b->next; free(b); }
    Block **tail = &free_list;
    for (uint32_t i = 0; i < n && !s->error; i++) {
        Block *b = (Block *)calloc(1, sizeof(Block));
        ST_VAR(s, b->addr);
        ST_VAR(s, b->size);
        *tail = b;
        tail = &b->next;
    }
}
