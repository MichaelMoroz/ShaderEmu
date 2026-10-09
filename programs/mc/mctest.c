// mctest: the worker cores' test (docs/multicore.md). A Linux program on core 0 that puts
// worker.c's code in the arena, starts every worker the machine has and gives them work:
//
//   primes   counting primes, nothing stored: core 0 alone, then split with the workers
//   fill     each worker fills a buffer of its own; core 0 checks every word
//   stripe   three workers fill one buffer between them, every third 16 bytes each
//   sum      the workers read back what the others wrote
//
// Prints one line a test and "mctest: PASS" or "mctest: FAIL".
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include "mc.h"
#include "mc_worker.h"   // worker.c built for the arena: mc_worker_code[], made by build.bat

static uint8_t* arena;
static volatile uint32_t* host_ms;   // the host's clock, a control word of the GPU device
static int workers, failed;
static uint32_t seq[MC_MAX_CORES];

static mc_job* job_of(int k) { return (mc_job*)(arena + MC_JOB_AT(k)); }
static mc_answer* answer_of(int k) { return (mc_answer*)(arena + MC_ANSWER_AT(k)); }

static uint32_t primes(uint32_t from, uint32_t to) {
    uint32_t count = 0;
    for (uint32_t n = from; n < to; n++) {
        if (n < 2) continue;
        int prime = 1;
        for (uint32_t d = 2; d * d <= n; d++)
            if (n % d == 0) { prime = 0; break; }
        count += prime;
    }
    return count;
}

static uint32_t pattern(uint32_t seed, uint32_t i) { return (seed + i) * 2654435761u ^ (i >> 3); }

static uint32_t guest_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)tv.tv_sec * 1000u + (uint32_t)tv.tv_usec / 1000u;
}

static void post(int k, uint32_t fn, uint32_t a0, uint32_t a1) {
    mc_job* j = job_of(k);
    j->fn = fn;
    j->a0 = a0;
    j->a1 = a1;
    j->seq = ++seq[k];
}

// Passes until worker k has answered its last job (0: it did not in `limit` passes).
static int wait_for(int k, int limit) {
    int passes = 0;
    while (answer_of(k)->done != seq[k]) {
        if (++passes > limit) return 0;
        mc_next_pass();
    }
    return passes + 1;
}

static void check(const char* what, int ok) {
    if (!ok) failed = 1;
    printf("mctest: %-7s %s\n", what, ok ? "ok" : "FAILED");
}

int main(int argc, char** argv) {
    int fd = open("/dev/gpu", O_RDWR);
    if (fd < 0) { printf("mctest: no /dev/gpu\n"); return 1; }
    arena = mmap(0, MC_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MC_ARENA_GPU_OFFSET);
    uint8_t* regs = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
    if (arena == MAP_FAILED || regs == MAP_FAILED) { printf("mctest: mmap failed\n"); return 1; }
    host_ms = (volatile uint32_t*)(regs + 0x34);
    int want = argc > 1 ? atoi(argv[1]) : MC_MAX_CORES - 1;
    uint32_t limit = argc > 2 ? (uint32_t)atoi(argv[2]) : 60000;

    // the code, and mailboxes with nothing in them; a pass, so that every core sees them
    memset(arena, 0, MC_CODE_AT);
    memcpy(arena + MC_CODE_AT, mc_worker_code, sizeof mc_worker_code);
    mc_next_pass();
    for (int k = 1; k <= want; k++) {
        volatile uint32_t* start = (volatile uint32_t*)(arena + MC_START_AT(k));
        start[1] = MC_ARENA_PHYS + MC_CODE_AT;
        start[2] = MC_ARENA_PHYS + MC_STACK_TOP(k);
        start[3] = 0;
        start[0] = MC_START;
    }
    // a core that is there says so within a few passes
    for (int pass = 0; pass < 8; pass++) mc_next_pass();
    for (int k = 1; k <= want; k++) {
        if (answer_of(k)->alive != (MC_ALIVE | (uint32_t)k)) break;
        workers = k;
    }
    for (int k = 1; k <= want; k++) *(volatile uint32_t*)(arena + MC_START_AT(k)) = 0;
    printf("mctest: %d worker core%s\n", workers, workers == 1 ? "" : "s");
    if (workers == 0) { printf("mctest: FAIL (run the machine with --cores N)\n"); return 1; }

    // "mctest N LIMIT bench": what a pass costs with more cores busy. Each line is the same
    // work on every busy core (primes of one range, or of ranges of one length further up),
    // so the time is the pass's, not the work's.
    if (argc > 3 && !strcmp(argv[3], "bench")) {
        for (int mode = 0; mode < 6; mode++) {
            if (argc > 4 && atoi(argv[4]) != mode) continue;   // one line only
            // busy workers; whether core 0 works too; whether the workers' ranges differ
            static const int busy_of[6] = {0, 1, 3, 3, MC_MAX_CORES, 1}, main_of[6] = {1, 1, 1, 1, 0, 0}, apart_of[6] = {0, 0, 0, 1, 0, 0};
            int busy = busy_of[mode] < workers ? busy_of[mode] : workers;
            uint32_t t0 = guest_ms(), sum = 0;
            for (int k = 1; k <= busy; k++) {
                uint32_t from = apart_of[mode] ? limit * (uint32_t)k : 0;
                post(k, MC_FN_PRIMES, from, from + limit);
            }
            if (main_of[mode]) sum = primes(0, limit);
            for (int k = 1; k <= busy; k++) { wait_for(k, 1000000); sum += answer_of(k)->result; }
            printf("mctest: bench: core 0 %s, %d worker%s on %s: %u ms (%u)\n", main_of[mode] ? "working" : "waiting", busy,
                   busy == 1 ? "" : "s", apart_of[mode] ? "ranges of their own" : "the same range", guest_ms() - t0, sum);
        }
        for (int k = 1; k <= workers; k++) post(k, MC_FN_PARK, 0, 0);
        for (int k = 1; k <= workers; k++) wait_for(k, 1000);
        return 0;
    }

    // ---- primes: the same range on core 0 alone, then a share each ----
    {
        int parts = workers + 1;
        uint32_t share = limit / (uint32_t)parts, t0 = guest_ms(), h0 = *host_ms;
        uint32_t alone = primes(0, share * (uint32_t)parts);
        uint32_t t1 = guest_ms(), h1 = *host_ms;
        for (int k = 1; k <= workers; k++) post(k, MC_FN_PRIMES, share * (uint32_t)k, share * (uint32_t)(k + 1));
        uint32_t together = primes(0, share);
        int ok = 1;
        for (int k = 1; k <= workers; k++) {
            if (!wait_for(k, 100000)) ok = 0;
            together += answer_of(k)->result;
        }
        uint32_t t2 = guest_ms(), h2 = *host_ms;
        printf("mctest: primes below %u: %u alone in %u ms (%u ms of the host's), %u with %d workers in %u ms (%u ms)\n",
               share * (uint32_t)parts, alone, t1 - t0, h1 - h0, together, workers, t2 - t1, h2 - h1);
        check("primes", ok && alone == together);
    }

    // ---- fill: 64 KiB each, a buffer of its own ----
    {
        const uint32_t words = 16384;
        int ok = 1;
        uint32_t t0 = guest_ms();
        for (int k = 1; k <= workers; k++) post(k, MC_FN_FILL, MC_DATA_AT + 0x40000u * (uint32_t)k, words);
        int passes = 0;
        for (int k = 1; k <= workers; k++) {
            int p = wait_for(k, 100000);
            if (!p) ok = 0;
            if (p > passes) passes = p;
        }
        uint32_t t1 = guest_ms();
        for (int k = 1; k <= workers; k++) {
            const uint32_t* p = (const uint32_t*)(arena + MC_DATA_AT + 0x40000u * (uint32_t)k);
            uint32_t sum = 0, bad = 0;
            for (uint32_t i = 0; i < words; i++) {
                sum += p[i];
                bad += p[i] != pattern((uint32_t)k, i);
            }
            if (bad || sum != answer_of(k)->result) {
                printf("mctest: worker %d's buffer: %u wrong words, sum %08x, its own %08x\n", k, bad, sum, answer_of(k)->result);
                ok = 0;
            }
        }
        printf("mctest: fill: %u KiB a worker in %d passes, %u ms\n", words / 256, passes, t1 - t0);
        check("fill", ok);
    }

    // ---- stripe: one buffer, every third 16 bytes a worker's ----
    if (workers >= 3) {
        const uint32_t texels = 3000, at = MC_DATA_AT;
        int ok = 1;
        memset(arena + at, 0, texels * 16);
        mc_next_pass();
        for (int k = 1; k <= 3; k++) post(k, MC_FN_STRIPE, at, texels);
        for (int k = 1; k <= 3; k++) if (!wait_for(k, 100000)) ok = 0;
        const uint32_t* p = (const uint32_t*)(arena + at);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < texels * 4; i++) bad += p[i] != pattern(0x57a1, i);
        if (bad) { printf("mctest: stripe: %u wrong words of %u\n", bad, texels * 4); ok = 0; }
        check("stripe", ok);

        // ---- sum: each worker reads all of it, the other two's words among them ----
        uint32_t expect = 0;
        for (uint32_t i = 0; i < texels * 4; i++) expect += pattern(0x57a1, i);
        for (int k = 1; k <= 3; k++) post(k, MC_FN_SUM, at, texels * 4);
        for (int k = 1; k <= 3; k++) {
            if (!wait_for(k, 100000) || answer_of(k)->result != expect) {
                printf("mctest: sum: worker %d has %08x, expected %08x\n", k, answer_of(k)->result, expect);
                ok = 0;
            }
        }
        check("sum", ok);
    }

    // park them: the next program starts them again
    for (int k = 1; k <= workers; k++) post(k, MC_FN_PARK, 0, 0);
    int parked = 1;
    for (int k = 1; k <= workers; k++) if (!wait_for(k, 1000)) parked = 0;
    check("park", parked);
    printf("mctest: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
