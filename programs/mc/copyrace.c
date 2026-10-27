// copyrace FILE [CHUNK] [ROUNDS] [WORKERS]: a read()'s buffer summed by workers that wait already, posted the moment the
// read returns, then by this core. Half-Life's packed files were inflated so (FS_InflateHere: pread, then the media
// workers at once): with a kernel that returns to user mode with its copy still queued (no TIF_GVM_DRAIN), a worker
// reads the buffer as it was, while this core's own loads wait for its copy. Every slice must sum the same on both.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mcw.h"

static uint32_t sum_job(uint32_t at, uint32_t bytes)
{
    const uint32_t *w = (const uint32_t *)at;
    uint32_t s = 2166136261u;
    for (uint32_t i = 0; i < bytes / 4; i++) s = (s ^ w[i]) * 16777619u;
    return s;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: copyrace FILE [CHUNK] [ROUNDS] [WORKERS]\n");
        return 2;
    }
    uint32_t chunk = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 262144, rounds = argc > 3 ? (uint32_t)atoi(argv[3]) : 32;
    int fd = open(argv[1], O_RDONLY), want = argc > 4 ? atoi(argv[4]) : 15;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || (uint32_t)st.st_size < 2 * chunk) {
        fprintf(stderr, "copyrace: %s: cannot read it, or under two chunks\n", argv[1]);
        return 2;
    }
    uint8_t *buf = aligned_alloc(4096, chunk);
    for (uint32_t i = 0; i < chunk; i += 4) *(uint32_t *)(buf + i) = 0;   // (a worker reads it: stored here first)
    int workers = mcw_open(want);
    if (workers < 1) {
        printf("copyrace: no workers\n");
        return 1;
    }
    uint32_t slice = (chunk / (uint32_t)workers) & ~3u, bad = 0, slices = 0;
    for (int k = 1; k <= workers; k++) mcw_post(k, sum_job, (uint32_t)buf, 4);   // (each worker waits from here on)
    for (int k = 1; k <= workers; k++) mcw_wait(k);
    for (uint32_t r = 0; r < rounds; r++) {
        off_t at = (off_t)((r * 7919u * 4096u) % ((uint32_t)st.st_size - chunk));
        if (pread(fd, buf, chunk, at) != (ssize_t)chunk) {
            fprintf(stderr, "copyrace: short read at %ld\n", (long)at);
            return 2;
        }
        for (int k = 1; k <= workers; k++) mcw_post(k, sum_job, (uint32_t)(buf + (k - 1) * slice), slice);
        for (int k = 1; k <= workers; k++) {
            mcw_wait(k);
            slices++;
            if (mcw_result(k) != sum_job((uint32_t)(buf + (k - 1) * slice), slice)) bad++;
        }
    }
    mcw_close();
    printf("copyrace: %u rounds of %u bytes on %d workers: %u of %u slices read before the copy\n", rounds, chunk, workers, bad, slices);
    return bad != 0;
}
