/*
 * wl: runs workloads (wl.h, programs/wl/README.md) and prints their units a second.
 *
 *   wl list
 *   wl [-s SECONDS] [-m c0|w|p] [-n WORKERS] NAME[,NAME...]|all
 *
 * Modes: c0 the first core alone; w one worker while the first core rests for it (mcw_rest_for,
 * as a game's server on a worker while its client waits); p every worker (-n of them) at once,
 * each with a scratch of its own, for the kernels that have inst(). Each kernel's unit is run
 * once on the first core before the clock starts (its code and data are then the machine's).
 * A line a kernel:
 *   WL name mode units seconds rate sum
 * (sum: one unit's result; every unit does the same work, on every core)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wl.h"
#ifdef WL_HOST   /* (a host build for trying the kernels: no workers) */
typedef uint32_t (*mcw_fn)(uint32_t, uint32_t);
static int mcw_count(void) { return 0; }
static int mcw_open(int n) { (void)n; return 0; }
static void mcw_close(void) {}
static void *mcw_alloc(unsigned n) { void *p = 0; return posix_memalign(&p, 64, n) ? 0 : p; }
static void mcw_post(int k, mcw_fn f, uint32_t a, uint32_t b) { (void)k; (void)f; (void)a; (void)b; }
static int mcw_wait(int k) { (void)k; return 0; }
static void mcw_rest_for(const int *w, int n) { (void)w; (void)n; }
static int mcw_bound(int k) { (void)k; return 0; }
static int mcw_on_worker(void) { return 0; }
#else
#include "mcw.h"
#endif

#define K(x) extern const wl_kernel x;
#include "kernels.h"
#undef K
#define K(x) &x,
static const wl_kernel *const kernels[] = {
#include "kernels.h"
};
#undef K
#define NK ((int)(sizeof(kernels) / sizeof(kernels[0])))

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* a kernel's scratches come from blocks of 4 MB (mcw_alloc's, which are never freed), used again by the next kernel */
#define ARENA_BLOCK (4u << 20)
#define ARENA_BLOCKS 64
static char *arena[ARENA_BLOCKS];
static unsigned arena_block, arena_used;
static int in_setup;   /* (what a kernel's setup allocates is kept: its tables, read by every unit) */

void *wl_alloc(unsigned bytes)
{
    bytes = (bytes + 63) & ~63u;
    if (in_setup) {
        void *p = mcw_count() ? mcw_alloc(bytes) : 0;
        if (!p && posix_memalign(&p, 64, bytes)) p = 0;
        if (!p) { printf("wl: no memory for %u bytes\n", bytes); exit(1); }
        memset(p, 0, bytes);
        return p;
    }
    if (bytes > ARENA_BLOCK) { printf("wl: %u bytes is more than a block\n", bytes); exit(1); }
    if (arena_used + bytes > ARENA_BLOCK) arena_block++, arena_used = 0;
    if (arena_block >= ARENA_BLOCKS) { printf("wl: no memory for %u bytes\n", bytes); exit(1); }
    if (!arena[arena_block]) {
        void *p = mcw_count() ? mcw_alloc(ARENA_BLOCK) : 0;
        if (!p && posix_memalign(&p, 64, ARENA_BLOCK)) p = 0;
        if (!p) { printf("wl: no memory for block %u\n", arena_block); exit(1); }
        arena[arena_block] = p;
    }
    void *p = arena[arena_block] + arena_used;
    arena_used += bytes;
    memset(p, 0, bytes);
    return p;
}

/* a worker's job: units until the first core says stop; what it did, in its own texel */
struct job {
    const wl_kernel *k;
    void *ctx;
    uint32_t units, sum;
    uint32_t pad[12];
};
static volatile uint32_t stop;   /* (on a texel of its own: the workers only read it) */
static uint32_t stop_pad[3] __attribute__((unused));

static uint32_t work(uint32_t a0, uint32_t a1)
{
    struct job *j = (struct job *)(uintptr_t)a0;
    uint32_t n = 0, sum = 0;
    (void)a1;
    while (!stop) {
        sum = j->k->run(j->ctx);
        n++;
        if (!mcw_on_worker()) break;   /* (run by the core that posted it: a worker without a core) */
    }
    j->units = n;
    j->sum = sum;
    return n;
}

static void run_one(const wl_kernel *k, const char *mode, double seconds, int want)
{
    arena_block = 0, arena_used = 0;   /* (the last kernel's scratches are free again) */
    void *ctx0 = k->inst ? k->inst() : 0;
    uint32_t sum = k->run(ctx0), first = sum, units = 0;   /* (the warm unit, not counted) */
    double t0, t;
    if (!strcmp(mode, "c0")) {
        t0 = now();
        do {
            sum = k->run(ctx0);
            units++;
        } while ((t = now()) - t0 < seconds);
        if (sum != first) printf("WL %s c0: a unit gave %08x, the first %08x\n", k->name, sum, first);
    } else {
        int n = !strcmp(mode, "w") ? 1 : want;
        if (n > 1 && !k->inst) { printf("WL %s %s skipped: no inst()\n", k->name, mode); return; }
        if (n < 1 || mcw_count() < n) { printf("WL %s %s skipped: %d workers of %d\n", k->name, mode, mcw_count(), n); return; }
        struct job *jobs = wl_alloc(sizeof(struct job) * (unsigned)n);
        int ids[256];
        memset(jobs, 0, sizeof(struct job) * (unsigned)n);
        for (int i = 0; i < n; i++) {
            jobs[i].k = k;
            jobs[i].ctx = i == 0 ? ctx0 : k->inst();
            ids[i] = i + 1;
        }
        stop = 0;
        t0 = now();
        int bound = 0;
        for (int i = 0; i < n; i++)
            if (mcw_bound(i + 1)) ids[bound++] = i + 1;
        if (bound < n) printf("WL %s %s: %d of %d workers have a core\n", k->name, mode, bound, n);
        for (int i = 0; i < bound; i++) mcw_post(ids[i], work, (uint32_t)(uintptr_t)&jobs[ids[i] - 1], 0);
        while (now() - t0 < seconds) mcw_rest_for(ids, bound);
        stop = 1;
        for (int i = 0; i < bound; i++) mcw_wait(ids[i]);
        t = now();
        for (int i = 0; i < n; i++) {
            units += jobs[i].units;
            if (jobs[i].units && jobs[i].sum != sum) printf("WL %s %s: worker %d's unit gave %08x, not %08x\n", k->name, mode, i + 1, jobs[i].sum, sum);
        }
    }
    printf("WL %s %s %u %.3f %.2f %08x\n", k->name, mode, units, t - t0, units / (t - t0), sum);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    double seconds = 3;
    const char *mode = "c0";
    int want = 255, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) want = atoi(argv[++i]);
        else break;
    }
    if (i >= argc || !strcmp(argv[i], "list")) {
        for (int k = 0; k < NK; k++) printf("%-14s %s\n", kernels[k]->name, kernels[k]->from);
        return 0;
    }
    if (strcmp(mode, "c0")) {
        int got = mcw_open(!strcmp(mode, "w") ? 1 : want);
        if (got < want && strcmp(mode, "w")) want = got;
    }
    char *names = strdup(argv[i]);
    for (char *name = strtok(names, ","); name; name = strtok(0, ",")) {
        int found = 0;
        for (int k = 0; k < NK; k++) {
            if (strcmp(name, "all") && strcmp(name, kernels[k]->name)) continue;
            found = 1;
            static unsigned char done[256];
            if (!done[k]) { in_setup = 1, kernels[k]->setup(), in_setup = 0, done[k] = 1; }
            run_one(kernels[k], mode, seconds, want);
        }
        if (!found) printf("wl: no kernel %s\n", name);
    }
    if (strcmp(mode, "c0")) mcw_close();
    printf("WL done\n");
    return 0;
}
