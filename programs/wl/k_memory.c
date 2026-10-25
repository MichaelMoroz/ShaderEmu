/*
 * mem_copy: what loading and drawing spend in the C library's memcpy, memmove and memset, each machine's own
 * (musl's, with the RISC-V machine's memcpy.S for words on word boundaries). A unit, as a level's load and a
 * frame's buffers move memory: 8 blocks of 8 KB copied, 4 of them at an odd offset; 2,000 pieces of 1 to 200
 * bytes copied between places that do not line up; 4 blocks of 8 KB moved over themselves by 40 bytes; 32 KB
 * cleared and 32 KB filled with a byte.
 */
#include <string.h>
#include "wl.h"

#define BLOCK 8192
#define PIECES 2000

struct buffers { unsigned char from[8 * BLOCK + 64], to[8 * BLOCK + 64], clear[32768]; };
static unsigned short piece_size[PIECES], piece_from[PIECES], piece_to[PIECES];

static void setup(void)
{
    uint32_t seed = 0x3e3c;
    for (int i = 0; i < PIECES; i++) {
        piece_size[i] = (unsigned short)(1 + wl_rand(&seed) % 200);
        piece_from[i] = (unsigned short)(wl_rand(&seed) % (8 * BLOCK - 256));
        piece_to[i] = (unsigned short)(wl_rand(&seed) % (8 * BLOCK - 256));
    }
}

static void *inst(void)
{
    struct buffers *b = wl_alloc(sizeof(struct buffers));
    uint32_t seed = 0x77;
    for (unsigned i = 0; i < sizeof b->from; i++) b->from[i] = (unsigned char)wl_rand(&seed);
    return b;
}

static uint32_t run(void *ctx)
{
    struct buffers *b = ctx;
    uint32_t sum = 0;
    for (int k = 0; k < 8; k++) memcpy(b->to + k * BLOCK + (k & 1) * 3, b->from + k * BLOCK + (k & 1) * 5, BLOCK);
    for (int i = 0; i < PIECES; i++) memcpy(b->to + piece_to[i] + 1, b->from + piece_from[i] + 3, piece_size[i]);
    for (int k = 0; k < 4; k++) memmove(b->to + k * BLOCK + 40, b->to + k * BLOCK, BLOCK - 64);
    memset(b->clear, 0, sizeof b->clear);
    memset(b->clear, 0x5a, sizeof b->clear - 7);
    for (unsigned i = 0; i < sizeof b->to; i += 257) sum = sum * 31 + b->to[i];
    return sum + b->clear[100];
}

const wl_kernel wl_mem_copy = {"mem_copy", "libc: memcpy, memmove, memset of blocks and pieces (loading)", setup, inst, run};
