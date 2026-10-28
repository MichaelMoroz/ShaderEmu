// handoff [FRAMES] [FILL]: Quake III's hand-off of a frame to its client's worker (client_shaderemu.c's ask and
// client_job), alone: core 0 writes a list's entry (two texels, each saying its frame) and then the count of frames
// asked for; the worker, seeing the count, reads the entry. Between two frames core 0 stores up to FILL words of a
// table, so that some of its passes end with the write cache full. An entry the worker reads as older than the count
// says is the card-only n < cg.latestSnapshotNum's cause (GPU-VM docs/REGRESSIONS.md): it must say 0.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mcw.h"

#define LISTS 4
static struct { uint32_t frame_a, payload[3]; uint32_t frame_b, check[3]; } entry[LISTS] __attribute__((aligned(16)));
static struct { uint32_t frames, unused[3]; } asked __attribute__((aligned(16)));    // core 0's
static struct { uint32_t frames, stale_a, stale_b, newer; } made __attribute__((aligned(16)));   // the worker's
static uint32_t table[16384] __attribute__((aligned(16)));

static uint32_t worker_job(uint32_t frames, uint32_t unused)
{
    while (made.frames < frames) {
        uint32_t f = made.frames + 1, *a, *b;
        while (asked.frames < f) mc_end_pass();
        a = &entry[f % LISTS].frame_a, b = &entry[f % LISTS].frame_b;
        if (*a < f) made.stale_a++;
        if (*b < f) made.stale_b++;
        if (*a > f || *b > f) made.newer++;
        made.frames = f;
    }
    return made.stale_a + made.stale_b;
}

int main(int argc, char **argv)
{
    uint32_t frames = argc > 1 ? (uint32_t)atoi(argv[1]) : 20000, fill = argc > 2 ? (uint32_t)atoi(argv[2]) : 3000, seed = 12345;
    int ordered = argc > 3 && atoi(argv[3]);   // (mc_publish between the entry and the count)
    for (int l = 0; l < LISTS; l++) entry[l].frame_a = entry[l].frame_b = 0;
    asked.frames = 0, made.frames = made.stale_a = made.stale_b = made.newer = 0;
    for (uint32_t i = 0; i < 16384; i++) table[i] = 0;
    if (mcw_open(1) < 1) {
        printf("handoff: no worker\n");
        return 1;
    }
    mcw_post(1, worker_job, frames, 0);
    for (uint32_t f = 1; f <= frames; f++) {
        while (f - made.frames > 3) mc_end_pass();   // (an entry is written again only once its frame is made)
        entry[f % LISTS].frame_a = f, entry[f % LISTS].payload[0] = f * 3;
        entry[f % LISTS].frame_b = f, entry[f % LISTS].check[0] = f * 5;
        if (ordered) mc_publish();
        asked.frames = f;
        seed = seed * 1103515245u + 12345u;
        for (uint32_t i = 0, n = (seed >> 8) % (fill + 1); i < n; i++) table[(seed + i * 17u) & 16383u] = f;
    }
    mcw_wait(1);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 16384; i++) sum += table[i];   // (the table's stores are kept: they fill the write cache)
    printf("handoff: %u frames: %u read older in the first texel, %u in the second, %u newer (table %08x)\n", frames,
           made.stale_a, made.stale_b, made.newer, sum);
    mcw_close();
    return made.stale_a + made.stale_b != 0;
}
