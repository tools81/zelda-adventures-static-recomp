/* Sprite blitter observation (emulation side of sprite interpolation).

   The game draws its characters in software into the background picture
   with a few run-list blitters (game.json "sprites"): d0 points at a
   descriptor {run list, destination, background}.  Hooks at their entry
   points record every call, without changing what the game does.  When the
   blits of a game update become visible on screen (they were drawn into the
   buffer the display now shows), they form an "update": the list of sprites
   drawn, in order, plus where the game keeps its clean background copy.
   motion.c turns consecutive updates into interpolated frames.

   This state is part of emulator snapshots (run-ahead, quicksaves). */
#include "recomp.h"
#include "runtime.h"
#include "game_config.h"
#include "sprites.h"
#include <string.h>

#define MAXPEND 512
#define PEND_FIELDS 12      /* drop blits that never became visible */

static struct {
    SpriteBlit pend[MAXPEND];
    int npend;
    SpriteUpdate upd;       /* the last update that became visible */
    uint32_t fields;
} sp;

const SpriteUpdate *sprites_last_update(void) { return &sp.upd; }
uint32_t sprites_fields(void) { return sp.fields; }

static int in_list(const uint32_t *l, uint32_t a) {
    for (; *l; l++) if (*l == a) return 1;
    return 0;
}

/* called from game_skip_insn() when a blitter is entered */
void sprite_hook(uint32_t addr) {
#if GAME_HAS_SPRITES
    int restore = in_list(game_sprite_restore, addr);
    if (!restore && !in_list(game_sprite_draw, addr)) return;
    if (sp.npend >= MAXPEND) return;
    uint32_t d = D(0);
    SpriteBlit *b = &sp.pend[sp.npend++];
    b->routine = addr;
    b->restore = restore;
    b->runlist = RL(d);
    b->dest = RL(d + 4);
    b->bg = restore ? RL(d + 8) : 0;
    b->field = sp.fields;
#else
    (void)addr;
#endif
}

/* After every field: blits into the lines on screen now are visible. */
void sprites_field(void) {
    sp.fields++;
#if GAME_HAS_SPRITES
    /* each plane's picture buffer: the longest run of display lines fetched
       from consecutive 384-byte lines */
    extern uint32_t plane_line[2][SCREEN_LINES];
    extern int plane_mode[2][SCREEN_LINES];
    uint32_t rlo[2] = {0, 0}, rhi[2] = {0, 0};
    for (int p = 0; p < 2; p++) {
        int best = 0, run = 0;
        for (int y = 0; y < SCREEN_LINES; y++) {
            int cont = y > 0 && plane_mode[p][y] && plane_mode[p][y - 1] &&
                       plane_line[p][y] == plane_line[p][y - 1] + LINE_BYTES;
            run = cont ? run + 1 : (plane_mode[p][y] ? 1 : 0);
            if (run > best) {
                best = run;
                rhi[p] = plane_line[p][y] + LINE_BYTES;
                rlo[p] = plane_line[p][y] - (uint32_t)(run - 1) * LINE_BYTES;
            }
        }
        if (best < 16) rlo[p] = rhi[p] = 0;     /* no picture buffer on this plane */
    }
    if (log_level >= 3 && sp.npend)
        TRACE("sprites: %d pending (first dest %06x), plane A %06x-%06x, plane B %06x-%06x", sp.npend,
              sp.pend[0].dest, rlo[0], rhi[0], rlo[1], rhi[1]);
    /* the plane the blits go to: the one whose buffer holds the first visible blit */
    int plane = -1;
    for (int i = 0; i < sp.npend && plane < 0; i++)
        for (int p = 0; p < 2; p++)
            if (!sp.pend[i].restore && sp.pend[i].dest >= rlo[p] && sp.pend[i].dest < rhi[p]) { plane = p; break; }
    uint32_t lo = plane >= 0 ? rlo[plane] : 0, hi = plane >= 0 ? rhi[plane] : 0;
    int ndraw = 0;
    for (int i = 0; i < sp.npend; i++)
        if (!sp.pend[i].restore && sp.pend[i].dest >= lo && sp.pend[i].dest < hi) ndraw++;
    /* keep what is not visible yet; drop very old entries */
    int k = 0;
    SpriteUpdate u;
    memset(&u, 0, sizeof u);
    for (int i = 0; i < sp.npend; i++) {
        SpriteBlit *b = &sp.pend[i];
        int vis = b->dest >= lo && b->dest < hi;
        if (vis && ndraw) {
            if (b->restore) {
                u.bg_delta = (int32_t)(b->bg - b->dest);
                u.have_bg = 1;
            } else if (u.n < MAX_UPDATE_SPRITES) {
                u.s[u.n++] = *b;
            }
            continue;
        }
        if (sp.fields - b->field > PEND_FIELDS) continue;
        sp.pend[k++] = *b;
    }
    sp.npend = k;
    if (!ndraw) return;
    u.serial = sp.upd.serial + 1;
    u.field = sp.fields;
    u.plane = plane;
    u.buf_lo = lo;
    u.buf_hi = hi;
    if (!u.have_bg && sp.upd.have_bg && sp.upd.buf_lo == lo) {   /* same buffer as before */
        u.bg_delta = sp.upd.bg_delta;
        u.have_bg = 1;
    }
    sp.upd = u;
    TRACE("sprite update %u at field %u: %d sprites, plane %c buffer %06x-%06x, clean copy %s %+d", u.serial,
          u.field, u.n, 'A' + plane, lo, hi, u.have_bg ? "at" : "unknown", u.bg_delta);
#endif
}

void sprites_state(StateBuf *s) {
    ST_VAR(s, sp);
}
