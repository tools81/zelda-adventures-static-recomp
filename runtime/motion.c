/* Sprite interpolation (presentation side).

   The game moves its sprites only when it updates the picture (every 3
   fields on PAL, 16.7 times a second).  For smooth motion at 60 frames per
   second, each new game update (sprites.c) is captured: the sprite layer as
   shown, the game's clean copy of it (the layer without moving sprites) and
   every sprite drawn, with the pixels its run list covered.  Sprites are
   matched with the previous update; in-between frames erase each sprite
   (clean copy) and draw it again at a position between the old and the new
   one, then compose that layer with the other plane exactly as the display
   hardware does (video_render_override).

   Interpolating means showing an update while moving towards it, so the
   picture is up to one update behind; run-ahead (frame.c) hides part of
   that.  Nothing here changes the emulated game. */
#include "recomp.h"
#include "runtime.h"
#include "sprites.h"
#include <stdlib.h>
#include <string.h>

void video_render_override(uint32_t *dst, int plane, uint32_t lo, uint32_t hi, const uint8_t *buf);

#define MAX_RUNS 16384
#define MAX_MOVE 24         /* pixels per update: more is a jump, not motion */

typedef struct { int off, len; } Run;
typedef struct {
    int x, y;               /* anchor (the blit's destination) in the layer */
    uint32_t runlist;
    int run0, nruns;        /* this sprite's runs in Capture.runs */
    int vx, vy;             /* motion since the previous update */
    int moving;
} Spr;

typedef struct {
    int valid;
    uint32_t serial, field;
    int plane;
    uint32_t lo, hi;
    int have_bg;
    uint8_t *layer, *clean;
    int n;
    Spr s[MAX_UPDATE_SPRITES];
    int nruns;
    Run runs[MAX_RUNS];
} Capture;

static Capture caps[2];
static int curi;            /* caps[curi] is the latest update, caps[curi ^ 1] the one before */
static uint8_t *synth;
static size_t synth_cap;

static void footprint(Capture *c, Spr *sp, uint32_t runlist, uint32_t dest) {
    sp->run0 = c->nruns;
    uint32_t p = runlist + 4;
    uint32_t d = dest;
    for (int guard = 0; guard < 4096 && RL(p); guard++) {
        uint32_t skip = RW(p), cnt = RW(p + 2);
        p += 4;
        d += skip;
        if (cnt) {
            uint32_t len = cnt * 4;
            if (d >= c->lo && d + len <= c->hi && c->nruns < MAX_RUNS) {
                c->runs[c->nruns].off = (int)(d - c->lo);
                c->runs[c->nruns].len = (int)len;
                c->nruns++;
            }
            d += len;
            p += len;
        }
    }
    sp->nruns = c->nruns - sp->run0;
}

/* match the sprites of the latest update with the previous one */
static void match(Capture *cur, const Capture *prev) {
    static unsigned char used[MAX_UPDATE_SPRITES];
    memset(used, 0, sizeof used);
    for (int i = 0; i < cur->n; i++) {
        Spr *s = &cur->s[i];
        s->moving = 0;
        int best = -1, bestc = 1 << 30;
        for (int j = 0; j < prev->n; j++) {
            if (used[j]) continue;
            const Spr *q = &prev->s[j];
            int dx = s->x - q->x, dy = s->y - q->y;
            if (abs(dx) > MAX_MOVE || abs(dy) > MAX_MOVE) continue;
            int cost = abs(dx) + abs(dy) + (q->runlist == s->runlist ? 0 : MAX_MOVE);
            if (cost < bestc) { bestc = cost; best = j; }
        }
        if (best < 0) continue;
        used[best] = 1;
        s->vx = s->x - prev->s[best].x;
        s->vy = s->y - prev->s[best].y;
        s->moving = s->vx || s->vy;
    }
}

/* at a frame boundary: take a new game update if one became visible */
void motion_capture(void) {
    const SpriteUpdate *u = sprites_last_update();
    Capture *c = &caps[curi];
    if (!u->serial || (c->valid && u->serial == c->serial)) return;
    curi ^= 1;
    c = &caps[curi];
    size_t sz = u->buf_hi - u->buf_lo;
    if (!u->have_bg || sz == 0 || sz > 0x40000) { c->valid = 0; c->serial = u->serial; return; }
    c->layer = (uint8_t *)realloc(c->layer, sz);
    c->clean = (uint8_t *)realloc(c->clean, sz);
    memcpy(c->layer, RAM + u->buf_lo, sz);
    memcpy(c->clean, RAM + ((u->buf_lo + (uint32_t)u->bg_delta) & MEM_MASK), sz);
    c->valid = 1;
    c->serial = u->serial;
    c->field = u->field;
    c->plane = u->plane;
    c->lo = u->buf_lo;
    c->hi = u->buf_hi;
    c->have_bg = u->have_bg;
    c->n = 0;
    c->nruns = 0;
    for (int i = 0; i < u->n; i++) {
        const SpriteBlit *b = &u->s[i];
        Spr *s = &c->s[c->n++];
        int off = (int)(b->dest - c->lo);
        s->x = ((off % LINE_BYTES) + LINE_BYTES) % LINE_BYTES;
        s->y = off >= 0 ? off / LINE_BYTES : -((-off + LINE_BYTES - 1) / LINE_BYTES);
        s->runlist = b->runlist;
        s->moving = 0;
        footprint(c, s, b->runlist, b->dest);
    }
    Capture *p = &caps[curi ^ 1];
    if (p->valid && p->plane == c->plane && c->field - p->field <= 6) match(c, p);
}

/* Draw the frame for presented time 'field' (in fields, fractional).
   Returns 0 if there is nothing to interpolate (show the plain frame). */
int motion_render(uint32_t *out, double field) {
    Capture *c = &caps[curi], *p = &caps[curi ^ 1];
    if (!c->valid || !p->valid || c->plane != p->plane) return 0;
    double period = (double)(c->field - p->field);
    if (period <= 0 || period > 6 || field - c->field > 2 * period) return 0;
    double t = (field - c->field) / period;
    if (t >= 1.0) return 0;
    if (t < 0) t = 0;
    size_t sz = c->hi - c->lo;
    if (synth_cap < sz) { synth = (uint8_t *)realloc(synth, sz); synth_cap = sz; }
    memcpy(synth, c->layer, sz);
    int any = 0;
    /* erase every moving sprite */
    for (int i = 0; i < c->n; i++) {
        const Spr *s = &c->s[i];
        if (!s->moving) continue;
        any = 1;
        for (int r = s->run0; r < s->run0 + s->nruns; r++)
            memcpy(synth + c->runs[r].off, c->clean + c->runs[r].off, (size_t)c->runs[r].len);
    }
    if (!any) return 0;
    /* draw sprites back in the game's order, moving ones on their way */
    for (int i = 0; i < c->n; i++) {
        const Spr *s = &c->s[i];
        int ox = 0, oy = 0;
        if (s->moving) {
            double k = t - 1.0;
            ox = (int)(s->vx * k + (s->vx * k < 0 ? -0.5 : 0.5));
            oy = (int)(s->vy * k + (s->vy * k < 0 ? -0.5 : 0.5));
        }
        for (int r = s->run0; r < s->run0 + s->nruns; r++) {
            int off = c->runs[r].off;
            for (int k2 = 0; k2 < c->runs[r].len; k2++) {
                int o = off + k2;
                uint8_t v = c->layer[o];
                if (v == c->clean[o]) continue;         /* not drawn by the sprite */
                int x = o % LINE_BYTES + ox, y = o / LINE_BYTES + oy;
                if (x < 0 || x >= LINE_BYTES || y < 0) continue;
                size_t d = (size_t)y * LINE_BYTES + (size_t)x;
                if (d < sz) synth[d] = v;
            }
        }
    }
    video_render_override(out, c->plane, c->lo, c->hi, synth);
    return 1;
}
