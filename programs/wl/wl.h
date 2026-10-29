/*
 * Workloads: pieces of the machine's games and programs, each a kernel that does one unit of
 * the work again and again, for measuring the machine and changes to it on the same C
 * (programs/wl/README.md). wl.c runs a kernel for some seconds and prints its units a second.
 *
 * A kernel:
 *   setup()   once, on the first core: makes the data the units read (from a fixed seed: no
 *             files). It must write every byte of memory that run() will store to (a worker
 *             stops at a page not yet written: calloc's pages are not), and may allocate.
 *   inst()    optional: a new scratch of the kernel's own for one core (what run() stores to),
 *             written whole before it returns; NULL when the kernel keeps its scratch in
 *             statics (then it runs on one core at a time only).
 *   run(ctx)  one unit (about 50 thousand to 2 million instructions), the same work every
 *             time; returns a word of its result, which wl.c folds into the run's sum. It may
 *             not allocate, print, call the clock or make any other system call.
 */
#ifndef WL_H
#define WL_H

#include <stdint.h>

typedef struct wl_kernel {
    const char *name;    /* short: q3_trace, doom_cols, ... */
    const char *from;    /* what of which program it stands for, a few words */
    void (*setup)(void);
    void *(*inst)(void);
    uint32_t (*run)(void *ctx);
} wl_kernel;

/* a fixed generator for the setups */
static inline uint32_t wl_rand(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static inline float wl_randf(uint32_t *s) { return (wl_rand(s) >> 8) * (1.0f / 16777216.0f); }   /* [0, 1) */

/* memory of 16-byte texels for a core's own scratch, written (touched) before it is returned */
void *wl_alloc(unsigned bytes);

/* a float folded into a sum by its value to 1/256, so that both machines' small differences mostly agree */
static inline uint32_t wl_fold(float v) { return (uint32_t)(int32_t)(v * 256.0f); }

#endif
