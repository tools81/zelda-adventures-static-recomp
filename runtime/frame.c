/* Frame boundary: everything the host does once per emulated field, at a
   point where no 68k process is running (os9_run calls it after each field):
   input, hotkeys, quicksave slots, cheats, run-ahead and presentation. */
#include "recomp.h"
#include "runtime.h"
#include "game_config.h"
#include "sprites.h"
#include <stdlib.h>
#include <string.h>

int host_next_hotkey(void);
void host_field_done(const uint32_t *fb, int w, int h);
void host_show(const uint32_t *fb, int w, int h);
void motion_capture(void);
int motion_render(uint32_t *out, double field);
uint32_t sprites_fields(void);
extern int g_turbo;
void osd_text(uint32_t *fb, int w, int h, int x, int y, const char *text);
int osd_text_width(const char *text);
extern int g_audio_muted, g_log_muted, g_headless;

/* ---------------------------------------------------------------- settings */
int g_runahead = -1;        /* fields to run ahead (-1: default, 1 with a window, 0 headless) */
int g_invincible;           /* F5 */
int g_interp = -1;          /* F7: 60 fps sprite interpolation (-1: default, on with a window) */
static int slot;            /* quicksave slot 0-9 (F3) */
int g_ramdump;              /* debug: write the RAM every N fields (--ramdump N) */
char g_load_state[512];     /* --load-state FILE: start from a snapshot */
uint32_t g_watch;           /* --watch ADDR (debug) */

/* ---------------------------------------------------------------- messages */
#if GAME_HAS_INVINCIBILITY
static void refill_health(void);
#endif

static char osd_msg[64];
static int osd_frames;

static void osd(const char *fmt, int a) {
    snprintf(osd_msg, sizeof osd_msg, fmt, a);
    osd_frames = FIELD_RATE * 2;
    LOG("%s", osd_msg);
}

/* ---------------------------------------------------------------- quicksaves */
static void slot_path(int n, char *out, size_t sz) {
    snprintf(out, sz, "%s/quicksave_%d.state", g_data_dir, n);
}

static int slot_used(int n) {
    char p[600];
    slot_path(n, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void hotkey(int vk) {
    char p[600];
    switch (vk) {
    case 0x71: /* F2: save */
        slot_path(slot, p, sizeof p);
        if (emu_save_file(p) == 0) osd("SAVED TO SLOT %d", slot);
        else osd("SLOT %d: SAVE FAILED", slot);
        break;
    case 0x72: /* F3: next slot */
        slot = (slot + 1) % 10;
        osd(slot_used(slot) ? "SLOT %d" : "SLOT %d (EMPTY)", slot);
        break;
    case 0x73: { /* F4: load */
        slot_path(slot, p, sizeof p);
        int r = emu_load_file(p);
        if (r == 0) osd("LOADED SLOT %d", slot);
        else if (r == -1) osd("SLOT %d IS EMPTY", slot);
        else osd("SLOT %d: NOT A SNAPSHOT OF THIS GAME", slot);
        break;
    }
    case 0x74: /* F5: invincibility */
#if GAME_HAS_INVINCIBILITY
        g_invincible = !g_invincible;
        if (g_invincible) refill_health();
        osd(g_invincible ? "INVINCIBILITY ON" : "INVINCIBILITY OFF", 0);
#else
        osd("NO INVINCIBILITY FOR THIS GAME", 0);
#endif
        break;
    case 0x76: /* F7: sprite interpolation */
        g_interp = !g_interp;
        osd(g_interp ? "60 FPS SPRITE INTERPOLATION ON" : "SPRITE INTERPOLATION OFF", 0);
        { void host_settings_changed(void); host_settings_changed(); }
        break;
    case 0x75: /* F6: run-ahead 0 / 1 / 2 fields */
        g_runahead = (g_runahead + 1) % 3;
        osd(g_runahead ? "RUN-AHEAD %d" : "RUN-AHEAD OFF", g_runahead);
        { void host_settings_changed(void); host_settings_changed(); }
        break;
    }
}

/* ---------------------------------------------------------------- cheats */
/* Invincibility.  The game takes health off any character in one
   instruction (game.json "invincibility": damage_insn); the recompiled code
   and the interpreter ask here before executing it, and it is skipped when
   the character being hit is the player.  Enemies take damage as usual. */
#if GAME_HAS_INVINCIBILITY
static uint32_t player_object(void) {
    uint32_t globals = RL(GAME_GLOBALS);
    return globals ? RL(globals + GAME_PLAYER_OFF) : 0;
}

static void refill_health(void) {
    uint32_t globals = RL(GAME_GLOBALS), player = player_object();
    if (globals && player) WW(player + GAME_HEALTH_OFF, RW(globals + GAME_MAX_HEALTH_OFF));
}
#endif

int game_skip_insn(uint32_t addr) {
    if (log_level >= 2)
        for (const uint32_t *h = game_trace_hooks; *h; h++)
            if (*h == addr) {
                TRACE("hook %06x: d0=%08x d1=%08x d2=%08x a0=%06x a1=%06x a2=%06x sp=%06x ret=%06x", addr,
                      D(0), D(1), D(2), A(0), A(1), A(2), A(7), RL(A(7)));
            }
#if GAME_HAS_SPRITES
    sprite_hook(addr);              /* sprite blitters: observed only */
#endif
#if GAME_HAS_INVINCIBILITY
    if (addr == GAME_DAMAGE_INSN && g_invincible) {
        uint32_t victim = GAME_DAMAGE_REG < 8 ? D(GAME_DAMAGE_REG) : A(GAME_DAMAGE_REG - 8);
        return victim && victim == player_object();
    }
#endif
    (void)addr;
    return 0;
}

/* ---------------------------------------------------------------- frame */
static uint32_t *present_fb, *show_fb;

/* one picture: the field, or an in-between frame with the sprites moved */
static void show(double field) {
    if (!show_fb) show_fb = (uint32_t *)malloc((size_t)video_w * video_h * 4);
    if (!(g_interp && motion_render(show_fb, field)))
        memcpy(show_fb, present_fb, (size_t)video_w * video_h * 4);
    if (osd_frames > 0)
        osd_text(show_fb, video_w, video_h, (video_w - osd_text_width(osd_msg)) / 2, 8, osd_msg);
    host_show(show_fb, video_w, video_h);
}

/* Pictures for one field interval.  Without interpolation: one per field
   (50 Hz on PAL).  With it: SHOW_RATE per second, each showing the sprites
   where they are at that moment; the field clock and the wall clock stay
   locked together.  Headless runs use a virtual clock (no waiting). */
#define SHOW_RATE 60.0
static double field_wall = -1, next_show = -1, virt_clock;

static void present(double field) {
    if (osd_frames > 0) osd_frames--;
    int smooth = g_interp && !g_turbo;
    double fs, fe;
    if (g_headless) {
        fs = virt_clock;
        virt_clock += 1.0 / FIELD_RATE;
        fe = virt_clock;
    } else {
        double now = host_time();
        if (field_wall < 0 || now - field_wall > 0.25) field_wall = next_show = now;
        fs = field_wall;
        fe = fs + 1.0 / FIELD_RATE;
    }
    if (!smooth) {
        show(field);
        if (!g_headless) {
            if (!g_turbo) host_sleep_until(fe);
            field_wall = g_turbo ? host_time() : fe;
        }
        return;
    }
    if (next_show < fs - 0.05) next_show = fs;
    while (next_show < fe - 1e-9) {
        if (!g_headless) host_sleep_until(next_show);
        show(field + (next_show - fs) * FIELD_RATE);
        next_show += 1.0 / SHOW_RATE;
    }
    if (!g_headless) {
        host_sleep_until(fe);
        field_wall = fe;
    }
}

static StateBuf ahead_state;
static unsigned long long nframes;

void frame_boundary(void) {
    nframes++;
    if (g_runahead < 0) g_runahead = g_headless ? 0 : 1;
    if (g_interp < 0) g_interp = g_headless ? 0 : 1;
    if (!present_fb) present_fb = (uint32_t *)malloc((size_t)video_w * video_h * 4);

    if (g_load_state[0]) {
        int r = emu_load_file(g_load_state);
        LOG("--load-state %s: %s", g_load_state, r ? "failed" : "ok");
        g_load_state[0] = 0;
    }
    host_poll();
    for (int k; (k = host_next_hotkey()) != 0;) hotkey(k);

    if (g_watch) {                      /* debug (--watch ADDR): report changes of a byte */
        static int last = -1;
        int v = RB(g_watch);
        if (v != last) LOG("watch %06x: %02x -> %02x at field %llu", g_watch, last & 0xFF, v, nframes);
        last = v;
    }
    if (g_ramdump && nframes % (unsigned)g_ramdump == 0) {
        char p[600];
        snprintf(p, sizeof p, "%s/ram_%06llu.bin", g_data_dir, nframes);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(RAM, 1, ram_top, f); fclose(f); }
    }

    /* Run-ahead: emulate the next fields with the current input, show the
       last of them, then roll back.  The game sees the input as early as
       its own reaction time allows, at the cost of emulating extra fields. */
    uint32_t shown_field;
    if (g_runahead > 0) {
        emu_save(&ahead_state);
        g_audio_muted = g_log_muted = 1;
        os9_run_fields(g_runahead);
        memcpy(present_fb, video_fb, (size_t)video_w * video_h * 4);
        motion_capture();               /* the timeline shown is the one ahead */
        shown_field = sprites_fields();
        g_audio_muted = g_log_muted = 0;
        if (emu_load(&ahead_state)) fatal("run-ahead: snapshot restore failed");
    } else {
        memcpy(present_fb, video_fb, (size_t)video_w * video_h * 4);
        motion_capture();
        shown_field = sprites_fields();
    }
    host_field_done(present_fb, video_w, video_h);
    present((double)shown_field);
}
