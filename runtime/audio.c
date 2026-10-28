/* CD-i audio processor HLE: ADPCM decoding, soundmaps, attenuation, mixing. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define OUT_RATE 44100
#define E_UNKSVC 0xD0
#define E_UNID   0xBC

/* ---------------------------------------------------------------- ADPCM */
static const int K0[4] = {0, 60, 115, 98};
static const int K1[4] = {0, 0, -52, -55};

typedef struct { int p1[2], p2[2]; } AdpcmState;

static FORCEINLINE int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* Decode one 128-byte sound group. out receives interleaved samples per
   channel; returns number of sample frames written (per channel). */
static int decode_group(const uint8_t *g, int coding, AdpcmState *st, int16_t *outL, int16_t *outR) {
    int stereo = (coding & 3) == 1;
    int eight = ((coding >> 4) & 3) == 1;
    int units = eight ? 4 : 8;
    int frames = 0;
    int16_t tmp[8][28];
    for (int u = 0; u < units; u++) {
        uint8_t prm = eight ? g[4 + u] : g[4 + u];
        int range = prm & 15, filt = (prm >> 4) & 3;
        int ch = stereo ? (u & 1) : 0;
        int p1 = st->p1[ch], p2 = st->p2[ch];
        for (int i = 0; i < 28; i++) {
            int s;
            if (eight) {
                s = (int8_t)g[16 + i * 4 + u];
                s = (s * 256) >> range;
            } else {
                uint8_t b = g[16 + i * 4 + (u >> 1)];
                int n = (u & 1) ? (b >> 4) : (b & 15);
                if (n & 8) n -= 16;
                s = (n * 4096) >> range;
            }
            int v = s + ((p1 * K0[filt] + p2 * K1[filt] + 32) >> 6);
            int16_t o = clamp16(v);
            p2 = p1; p1 = o;
            tmp[u][i] = o;
        }
        st->p1[ch] = p1; st->p2[ch] = p2;
    }
    if (stereo) {
        for (int u = 0; u < units; u += 2)
            for (int i = 0; i < 28; i++) { outL[frames] = tmp[u][i]; outR[frames] = tmp[u + 1][i]; frames++; }
    } else {
        for (int u = 0; u < units; u++)
            for (int i = 0; i < 28; i++) { outL[frames] = outR[frames] = tmp[u][i]; frames++; }
    }
    return frames;
}

static int coding_rate(int coding) { return ((coding >> 2) & 3) == 1 ? 18900 : 37800; }

/* ---------------------------------------------------------------- streams */
typedef struct {
    int16_t *buf;       /* stereo interleaved at OUT_RATE */
    int cap, rd, wr;    /* ring */
} Ring;

static int ring_count(Ring *r) { return (r->wr - r->rd + r->cap) % r->cap; }
static void ring_put(Ring *r, int16_t l, int16_t rr) {
    int n = (r->wr + 1) % r->cap;
    if (n == r->rd) return;
    r->buf[r->wr * 2] = l; r->buf[r->wr * 2 + 1] = rr;
    r->wr = n;
}

/* resampler state per source */
typedef struct { double pos; int16_t lastL, lastR; } Resamp;

static void push_resampled(Ring *r, Resamp *rs, const int16_t *L, const int16_t *R, int n, int rate) {
    double step = (double)rate / OUT_RATE;
    double pos = rs->pos;
    while (pos < n) {
        int i = (int)pos;
        double f = pos - i;
        int16_t l0 = i == 0 ? rs->lastL : L[i - 1], r0 = i == 0 ? rs->lastR : R[i - 1];
        int16_t l1 = L[i], r1 = R[i];
        ring_put(r, (int16_t)(l0 + (l1 - l0) * f), (int16_t)(r0 + (r1 - r0) * f));
        pos += step;
    }
    rs->pos = pos - n;
    if (n) { rs->lastL = L[n - 1]; rs->lastR = R[n - 1]; }
}

static Ring disc_ring;
static Resamp disc_rs;
static AdpcmState disc_st;

void audio_feed_sector(const uint8_t *sec) {
    int coding = sec[19];
    const uint8_t *d = sec + 24;
    int16_t L[18 * 224], R[18 * 224];
    int n = 0;
    for (int g = 0; g < 18; g++) n += decode_group(d + g * 128, coding, &disc_st, L + n, R + n);
    push_resampled(&disc_ring, &disc_rs, L, R, n, coding_rate(coding));
}

/* ---------------------------------------------------------------- soundmaps */
#define MAXSM 32
typedef struct {
    int used;
    int coding, groups;
    uint32_t desc, data;     /* emulated addresses (data/groups/coding: from the descriptor) */
    uint32_t buf, bufsz;     /* sample buffer allocated by SM_Creat */
    int loop_start, loop_end, loop_count;   /* SD_Loop: sound groups, repeats */
} SoundMap;
static SoundMap sms[MAXSM];

/* Soundmap playback.  The audio processor reads a soundmap's sound groups
   from memory while it plays them, so a game can stream into a map's buffer
   during playback (Zelda's Adventure loops a small ring buffer with SD_Loop
   and keeps refilling it from the disc).  So groups are decoded from the
   emulated RAM when playback reaches them, never decoded ahead.  Times are
   output samples (OUT_RATE) on the emulated clock: cyc * OUT_RATE / CPU_IPS. */
typedef struct {
    int active;
    int id;
    uint32_t asy;
    int pid;                 /* process to signal on completion */
    uint32_t data;           /* sound groups (from the descriptor at SM_Out) */
    int groups, coding;
    double step;             /* input frames per output sample */
    int g_next, g_end;       /* next group to decode, end of this pass */
    int loop_start, loop_end, loops_left;   /* SD_Loop region (groups, end exclusive) */
    int fifo_n, fifo_pos;    /* decoded frames of the current group */
    int16_t fl[224], fr[224];
    int16_t pl, pr, ql, qr;  /* the two input frames around the output position */
    double pos;              /* output position between them (0..1) */
    int64_t start;           /* output sample where playback started */
} SmPlay;
static SmPlay cur, nxt;
static AdpcmState sm_state;
static int64_t rendered;     /* output samples rendered so far */
static int atten[4] = {0, 0xFF, 0, 0xFF};   /* LL, LR, RR, RL: stereo at full volume */

static int64_t now_sample(void) { return m68k.cyc * OUT_RATE / CPU_IPS; }

/* frames (stereo pairs, or mono samples) one 128-byte sound group decodes to */
static int frames_per_group(int coding) {
    int eight = ((coding >> 4) & 3) == 1, stereo = (coding & 3) == 1;
    return eight ? (stereo ? 56 : 112) : (stereo ? 112 : 224);
}

/* set up playback of soundmap id (the descriptor is authoritative: games
   repoint a soundmap at their own buffer by writing its data address, group
   count and coding) */
static void sm_start(SmPlay *p, int id, int continuous) {
    SoundMap *s = &sms[id];
    s->data = RL(s->desc + 10);
    s->groups = RW(s->desc + 26);
    s->coding = RB(s->desc + 30);
    if (!continuous) memset(&sm_state, 0, sizeof sm_state);
    p->id = id;
    p->data = s->data;
    p->groups = s->groups;
    p->coding = s->coding;
    p->step = (double)coding_rate(s->coding) / OUT_RATE;
    p->g_next = 0;
    p->g_end = s->groups;
    p->loops_left = 0;
    if (s->loop_count > 0 && s->loop_end >= s->loop_start && s->loop_start < s->groups) {
        p->loop_start = s->loop_start;
        p->loop_end = s->loop_end + 1 < s->groups ? s->loop_end + 1 : s->groups;
        p->g_end = p->loop_end;
        p->loops_left = s->loop_count;
    }
    p->fifo_n = p->fifo_pos = 0;
    if (!continuous) { p->pl = p->pr = p->ql = p->qr = 0; }
    p->pos = 1.0;
    TRACE("SM %d plays %d groups at %06x, loop %d-%d x%d", id, p->groups, p->data,
          p->loops_left ? p->loop_start : -1, p->loops_left ? p->loop_end - 1 : -1, p->loops_left);
}

/* next input frame; 0 when the map has ended */
static int sm_frame(SmPlay *p, int16_t *l, int16_t *r) {
    if (p->fifo_pos >= p->fifo_n) {
        if (p->g_next >= p->g_end) {
            if (p->loops_left <= 0) return 0;
            p->loops_left--;
            p->g_next = p->loop_start;
        }
        uint8_t grp[128];
        for (int k = 0; k < 128; k++) grp[k] = RB(p->data + (uint32_t)p->g_next * 128 + k);
        p->fifo_n = decode_group(grp, p->coding, &sm_state, p->fl, p->fr);
        p->fifo_pos = 0;
        p->g_next++;
    }
    *l = p->fl[p->fifo_pos];
    *r = p->fr[p->fifo_pos];
    p->fifo_pos++;
    return 1;
}

static void asy_done(uint32_t asy, int pid) {
    if (!asy) return;
    WW(asy, RW(asy) | 1);
    uint16_t sig = RW(asy + 2);
    if (sig) sig_send_pid(pid, sig);
}

/* the current map ended at output sample 'at': continue with the queued one */
static void sm_finished(int64_t at) {
    uint32_t asy = cur.asy;
    int apid = cur.pid;
    cur.active = 0;
    if (nxt.active) {
        SmPlay q = nxt;
        nxt.active = 0;
        cur.id = q.id; cur.asy = q.asy; cur.pid = q.pid;
        sm_start(&cur, q.id, 1);        /* gapless: keep the decoder state */
        cur.start = at;
        cur.active = 1;
        TRACE("SM switch to %d at %lld", cur.id, (long long)at);
    }
    asy_done(asy, apid);
}

/* ---------------------------------------------------------------- mixing */
static int16_t mixbuf[2 * 8192];
static int mixn;

static double gain_of(int a) {
    if (a >= 0x80) return 0.0;
    return pow(10.0, -a / 20.0);
}

static void render_until(int64_t target) {
    if (target <= rendered) return;
    double gll = gain_of(atten[0]), glr = gain_of(atten[1]), grr = gain_of(atten[2]), grl = gain_of(atten[3]);
    while (rendered < target) {
        int l = 0, r = 0;
        while (cur.active) {
            /* advance the resampler; a map that runs out ends right here */
            while (cur.pos >= 1.0) {
                cur.pl = cur.ql; cur.pr = cur.qr;
                if (!sm_frame(&cur, &cur.ql, &cur.qr)) break;
                cur.pos -= 1.0;
            }
            if (cur.pos >= 1.0) { sm_finished(rendered); continue; }
            l = (int)(cur.pl + (cur.ql - cur.pl) * cur.pos);
            r = (int)(cur.pr + (cur.qr - cur.pr) * cur.pos);
            cur.pos += cur.step;
            break;
        }
        /* real-time audio from the disc (e.g. narration) plays over soundmaps */
        if (ring_count(&disc_ring)) {
            l += disc_ring.buf[disc_ring.rd * 2];
            r += disc_ring.buf[disc_ring.rd * 2 + 1];
            disc_ring.rd = (disc_ring.rd + 1) % disc_ring.cap;
        }
        if (mixn < 8192) {
            double ol = l * gll + r * grl, or_ = r * grr + l * glr;
            mixbuf[mixn * 2] = clamp16((int)ol);
            mixbuf[mixn * 2 + 1] = clamp16((int)or_);
            mixn++;
        }
        rendered++;
    }
}

/* when the current map will end (if it ends: a loop runs until another map
   is queued), so the completion is signalled at the exact sample */
int64_t audio_next_event(void) {
    if (!cur.active || cur.loops_left > 0) return INT64_MAX;
    double frames = (double)(cur.fifo_n - cur.fifo_pos) +
                    (double)(cur.g_end - cur.g_next) * frames_per_group(cur.coding);
    int64_t end = rendered + (int64_t)((frames + 1.0 - cur.pos) / cur.step) + 1;
    return (end * CPU_IPS + OUT_RATE - 1) / OUT_RATE;
}

void audio_event(void) {
    render_until(now_sample());
}

static void sm_out(int id, uint32_t asy) {
    if (asy) WW(asy, 0);
    render_until(now_sample());
    if (cur.active) {
        /* the current map finishes first; replace any pending one */
        if (nxt.active) asy_done(nxt.asy, nxt.pid);
        nxt.active = 1; nxt.id = id; nxt.asy = asy; nxt.pid = os9_cur_pid();
        /* a looping map (SD_Loop) plays until another one is queued: it
           finishes this pass of the loop, then the queued one follows */
        cur.loops_left = 0;
        TRACE("SM_Out %d queued", id);
    } else {
        cur.id = id; cur.asy = asy; cur.pid = os9_cur_pid();
        sm_start(&cur, id, 0);
        cur.start = now_sample();
        cur.active = 1;
        TRACE("SM_Out %d start", id);
    }
    int64_t ev = audio_next_event();
    if (ev < m68k.cyc_limit) m68k.cyc_limit = ev;
}

static void sm_off(void) {
    render_until(now_sample());
    if (nxt.active) { nxt.active = 0; asy_done(nxt.asy, nxt.pid); }
    if (cur.active) { cur.active = 0; asy_done(cur.asy, cur.pid); }
}

int audio_setstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x3B) { /* SS_SM */
        switch (fn) {
        case 0: { /* SM_Creat coding, groups */
            int id = -1;
            for (int i = 1; i < MAXSM; i++) if (!sms[i].used) { id = i; break; }
            if (id < 0) return 0xBD;
            SoundMap *s = &sms[id];
            s->used = 1;
            s->loop_count = 0;
            s->coding = D(3) & 0xFF;
            s->groups = D(4) & 0xFFFF;
            s->desc = mem_alloc(48, NULL);
            s->bufsz = (uint32_t)s->groups * 128;
            s->buf = s->data = mem_alloc(s->bufsz, NULL);
            WW(s->desc + 0, (uint32_t)id);
            WL(s->desc + 10, s->data);
            WL(s->desc + 14, (uint32_t)s->groups * 128);
            WW(s->desc + 26, (uint32_t)s->groups);
            WB(s->desc + 30, (uint32_t)s->coding);
            D(0) = (uint32_t)id;
            A(0) = s->data;
            TRACE("SM_Creat coding %02x groups %d -> %d @%06x", s->coding, s->groups, id, s->data);
            return 0;
        }
        case 1: { /* SM_Out id, a0 = asy */
            int id = D(3) & 0xFFFF;
            if (id <= 0 || id >= MAXSM || !sms[id].used) return E_UNID;
            sm_out(id, A(0));
            return 0;
        }
        case 2: /* SM_Off */
            sm_off();
            return 0;
        case 3: { /* SM_Close */
            int id = D(3) & 0xFFFF;
            if (id > 0 && id < MAXSM && sms[id].used) {
                if ((cur.active && cur.id == id) || (nxt.active && nxt.id == id)) sm_off();
                mem_free(sms[id].buf, sms[id].bufsz);
                mem_free(sms[id].desc, 48);
                sms[id].used = 0;
            }
            return 0;
        }
        default:
            LOG("SM function %x not implemented", fn);
            return 0;
        }
    }
    if (code == 0x3D) { /* SS_SC: SC_Atten */
        render_until(now_sample());
        /* bytes 2 and 0 are the direct paths (L->L, R->R), bytes 3 and 1 the
           cross paths: the game's stereo attenuation writes $FF (off) to 3/1 */
        uint32_t v = D(3);
        atten[0] = (v >> 16) & 0xFF;   /* L -> L */
        atten[1] = (v >> 24) & 0xFF;   /* L -> R */
        atten[2] = v & 0xFF;           /* R -> R */
        atten[3] = (v >> 8) & 0xFF;    /* R -> L */
        TRACE("SC_Atten %08x", v);
        return 0;
    }
    if (code == 0x3C) { /* SS_SD */
        int id = D(3) & 0xFFFF;
        if (fn == 2) { /* SD_Loop id, first group, last group, count: repeat a part of a soundmap */
            if (id <= 0 || id >= MAXSM || !sms[id].used) return E_UNID;
            sms[id].loop_start = (int16_t)D(4);
            sms[id].loop_end = (int16_t)D(5);
            sms[id].loop_count = (int)(D(6) & 0xFFFF);
            TRACE("SD_Loop sm %d groups %d-%d x%d", id, sms[id].loop_start, sms[id].loop_end, sms[id].loop_count);
            return 0;
        }
        LOG("SD function %x not implemented (d3=%x d4=%x d5=%x d6=%x)", fn, D(3), D(4), D(5), D(6));
        return 0;
    }
    if (code == 0x31) { sm_off(); return 0; }
    LOG("audio setstat %x fn %x", code, fn);
    return E_UNKSVC;
}

int audio_getstat(uint32_t code) {
    uint32_t fn = D(2) & 0xFFFF;
    if (code == 0x3B) {
        int id = D(3) & 0xFFFF;
        if (fn == 0) { /* SM_Info */
            if (id <= 0 || id >= MAXSM || !sms[id].used) return E_UNID;
            A(0) = sms[id].desc;
            return 0;
        }
        if (fn == 1) { /* SM_Stat: position and length in sound groups */
            WW(A(0), cur.active ? (uint32_t)cur.g_next : 0);
            WW(A(0) + 2, cur.active ? (uint32_t)cur.groups : 0);
            WW(A(0) + 4, 0);
            return cur.active ? 0 : 0xBE;
        }
    }
    LOG("audio getstat %x fn %x", code, fn);
    return E_UNKSVC;
}

/* ---------------------------------------------------------------- output */
void audio_init(void) {
    disc_ring.cap = OUT_RATE * 4;
    disc_ring.buf = (int16_t *)calloc((size_t)disc_ring.cap * 2, sizeof(int16_t));
    host_audio_init(OUT_RATE);
}

void audio_field_tick(void) {
    audio_event();
    /* keep disc audio latency bounded */
    while (ring_count(&disc_ring) > OUT_RATE / 2) disc_ring.rd = (disc_ring.rd + 1) % disc_ring.cap;
    render_until(now_sample());
    host_audio_push(mixbuf, mixn);
    mixn = 0;
}

void audio_shutdown(void) {}

/* ---------------------------------------------------------------- snapshots */
void audio_state(StateBuf *s) {
    ST_VAR(s, sms);
    ST_VAR(s, cur);
    ST_VAR(s, nxt);
    ST_VAR(s, sm_state);
    ST_VAR(s, rendered);
    ST_VAR(s, atten);
    ST_VAR(s, disc_rs);
    ST_VAR(s, disc_st);
    ST_VAR(s, disc_ring.rd);
    ST_VAR(s, disc_ring.wr);
    st_io(s, disc_ring.buf, sizeof(int16_t) * 2 * (size_t)disc_ring.cap);
    ST_VAR(s, mixn);
    st_io(s, mixbuf, sizeof(int16_t) * 2 * (size_t)mixn);
}
