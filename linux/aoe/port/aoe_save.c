// What the game writes through rge_write (saved games, scenarios, campaigns, the players' file, the AI's learning), not
// deflated: "AOEU" and the bytes as they are (docs/games/aoe.md, "Saved games"). A folder the host keeps is packed whole by
// nxpersist's LZMA (docs/system/persist.md), which packs the bytes far smaller than after deflate: an early game's save was
// 175,663 bytes deflated, over the 96,000 a player may keep. The reader (Inf32Decode, aoe_asm.cpp) takes a file that begins
// so as it is; a deflated one, the trial's own or a save of before, is inflated as ever. AOE_SAVE=deflate is the way before:
// upstream's Deflate.c, compiled with its functions renamed (build.sh).
#include <stdlib.h>
#include <string.h>

int aoe_up_deflate_buf_size(void);
int aoe_up_deflate_init(void* state, int level, int method, int window_bits, void* buffer, int size, int (*full)(unsigned char*, int));
int aoe_up_deflate_data(void* state, void* source, int size, int flush);
void aoe_up_deflate_deinit(void* state);

struct stored {
    int deflate;                        // upstream's state follows this one
    unsigned char* buffer;
    int size, used, marked;
    int (*full)(unsigned char*, int);
};
#define UP(state) ((char*) (state) + ((sizeof(struct stored) + 15) & ~15))

static int deflating(void) {
    const char* e = getenv("AOE_SAVE");
    return e && !strcmp(e, "deflate");
}

int deflate_buf_size(void) {
    return (int) ((sizeof(struct stored) + 15) & ~15) + aoe_up_deflate_buf_size();
}

int deflate_init(void* state, int level, int method, int window_bits, void* buffer, int size, int (*full)(unsigned char*, int)) {
    struct stored* s = (struct stored*) state;
    memset(s, 0, sizeof *s);
    s->deflate = deflating();
    if (s->deflate) return aoe_up_deflate_init(UP(state), level, method, window_bits, buffer, size, full);
    s->buffer = (unsigned char*) buffer;
    s->size = size;
    s->full = full;
    return 0;
}

static void put(struct stored* s, const unsigned char* p, int n) {
    while (n > 0) {
        int here = s->size - s->used < n ? s->size - s->used : n;
        memcpy(s->buffer + s->used, p, here);
        s->used += here, p += here, n -= here;
        if (s->used == s->size) {
            s->full(s->buffer, s->used);
            s->used = 0;
        }
    }
}

// 0, as upstream's: 2 would be an error (rge_write_error)
int deflate_data(void* state, void* source, int size, int flush) {
    struct stored* s = (struct stored*) state;
    if (s->deflate) return aoe_up_deflate_data(UP(state), source, size, flush);
    if (!s->marked) {
        s->marked = 1;
        put(s, (const unsigned char*) "AOEU", 4);
    }
    if (source && size > 0) put(s, (const unsigned char*) source, size);
    if (flush && s->used) {
        s->full(s->buffer, s->used);
        s->used = 0;
    }
    return 0;
}

void deflate_deinit(void* state) {
    struct stored* s = (struct stored*) state;
    if (s->deflate) aoe_up_deflate_deinit(UP(state));
}
