/* Sprite blitter observation (sprites.c) and interpolation (motion.c). */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t routine;       /* blitter entry point */
    int restore;            /* background restore (not a sprite) */
    uint32_t runlist;       /* run list: long header, then {skip.w, count.w, count longs}, ended by a 0 long */
    uint32_t dest;          /* destination in the picture buffer */
    uint32_t bg;            /* restore: source in the clean background copy */
    uint32_t field;
} SpriteBlit;

#define MAX_UPDATE_SPRITES 96
#define LINE_BYTES 384          /* normal-resolution 8-bit (CLUT8/DYUV) lines */
typedef struct {
    uint32_t serial;        /* counts visible updates */
    uint32_t field;         /* field at which it became visible */
    int plane;              /* display plane of the sprite layer (0 = A, 1 = B) */
    uint32_t buf_lo, buf_hi;    /* the sprite layer's picture buffer, LINE_BYTES per line */
    int have_bg;
    int32_t bg_delta;       /* clean background copy = picture address + bg_delta */
    int n;
    SpriteBlit s[MAX_UPDATE_SPRITES];
} SpriteUpdate;

const SpriteUpdate *sprites_last_update(void);
void sprite_hook(uint32_t addr);
