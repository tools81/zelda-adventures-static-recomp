/* Emulator snapshots: quicksave slots and run-ahead.

   A snapshot is taken only at a frame boundary, when no process is running:
   then the CPU state of every process is in the kernel's process table and
   nothing lives on native stacks.  It holds the RAM in use, the CPU registers
   and the state of every runtime module (kernel, CD file manager, video,
   audio).  Host-side state (window, input, wall clock) is not part of it. */
#include "recomp.h"
#include "runtime.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC   0x5A534E50u   /* "ZSNP" */
#define STATE_VERSION 4

void st_begin(StateBuf *s, int loading) {
    s->loading = loading;
    s->pos = 0;
    s->error = 0;
    if (!loading) s->len = 0;
}

void st_io(StateBuf *s, void *p, size_t n) {
    if (s->loading) {
        if (s->pos + n > s->len) { s->error = 1; memset(p, 0, n); return; }
        memcpy(p, s->buf + s->pos, n);
        s->pos += n;
        return;
    }
    if (s->len + n > s->cap) {
        size_t c = s->cap ? s->cap : 1 << 20;
        while (c < s->len + n) c *= 2;
        uint8_t *nb = (uint8_t *)realloc(s->buf, c);
        if (!nb) fatal("out of memory saving a snapshot");
        s->buf = nb;
        s->cap = c;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
}

/* the RAM below ram_top (program, heap) and the kernel scratch area */
#define KERNEL_AREA 0x00F00000u
#define KERNEL_SIZE 0x00020000u

static void emu_state(StateBuf *s) {
    uint32_t magic = STATE_MAGIC, version = STATE_VERSION, top = ram_top;
    ST_VAR(s, magic);
    ST_VAR(s, version);
    if (s->loading && (magic != STATE_MAGIC || version != STATE_VERSION)) { s->error = 1; return; }
    ST_VAR(s, top);
    if (top > KERNEL_AREA) { s->error = 1; return; }
    ST_VAR(s, m68k);
    st_io(s, RAM, top);
    st_io(s, RAM + KERNEL_AREA, KERNEL_SIZE);
    os9_state(s);
    cdfm_state(s);
    video_state(s);
    audio_state(s);
    sprites_state(s);
}

void emu_save(StateBuf *s) {
    st_begin(s, 0);
    emu_state(s);
}

int emu_load(StateBuf *s) {
    st_begin(s, 1);
    emu_state(s);
    return s->error;
}

/* ---------------------------------------------------------------- files */
typedef struct {
    char id[16];        /* "CDI-RECOMP-SNAP" */
    char game[48];      /* GAME_TITLE: refuse snapshots of another game */
    uint32_t size;
} FileHeader;

int emu_save_file(const char *path) {
    static StateBuf sb;
    emu_save(&sb);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    FileHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.id, "CDI-RECOMP-SNAP", 15);
    snprintf(h.game, sizeof h.game, "%s", GAME_TITLE);
    h.size = (uint32_t)sb.len;
    int ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(sb.buf, 1, sb.len, f) == sb.len;
    fclose(f);
    return ok ? 0 : -1;
}

int emu_load_file(const char *path) {
    static StateBuf sb;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    FileHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.id, "CDI-RECOMP-SNAP", 15) ||
        strncmp(h.game, GAME_TITLE, sizeof h.game)) {
        fclose(f);
        return -2;
    }
    if (h.size > sb.cap) {
        uint8_t *nb = (uint8_t *)realloc(sb.buf, h.size);
        if (!nb) { fclose(f); return -1; }
        sb.buf = nb;
        sb.cap = h.size;
    }
    sb.len = fread(sb.buf, 1, h.size, f);
    fclose(f);
    if (sb.len != h.size) return -2;
    /* validate before touching the running emulator: a bad file must not
       leave it half loaded */
    StateBuf backup = {0};
    emu_save(&backup);
    int err = emu_load(&sb);
    if (err) emu_load(&backup);
    free(backup.buf);
    return err ? -2 : 0;
}
