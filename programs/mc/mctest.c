// mctest: the worker cores' test (docs/machine/multicore.md). A Linux program on core 0 that starts
// every worker the machine has and gives them functions of its own to run:
//
//   primes   counting primes, nothing stored: core 0 alone, then split with the workers
//   fill     each worker fills a buffer of its own; core 0 checks every word
//   stripe   three workers fill one buffer between them, every third 16 bytes each
//   sum      the workers read back what the others wrote
//   calls    a worker asks the kernel for memory, writes a line and gives the memory back:
//            core 0 makes its system calls for it
//   floats   float arithmetic over many passes gives what it gives on core 0
//   pages    a worker reads and writes memory the program has never touched: every page of it
//            stops the worker, and the library brings the page and lets it go on
//   loop     mcw_for (mcwx.h): rounds of a loop over a buffer, the team kept between them
//   async    mcw_call and mcw_join: more calls than workers, joined out of order
//   effects  a job prints, defers a call, takes scratch memory and fails; core 0 sees each
//   counters every core adds to two counters in the same passes; the totals are exact
//
// Prints one line a test and "mctest: PASS" or "mctest: FAIL".
//   mctest [WORKERS [LIMIT [bench [LINE]]]]
//   mctest WORKERS SECONDS share [lend]     (the kernel's sharing: how many of them have cores, second by second)
//   mctest WORKERS 0 race                   (two workers store to one 16 bytes: for RVC_RACES, docs/machine/multicore.md)
//   mctest WORKERS 0 near                   (the same in passes apart: for RVC_RACES_NEAR, which plain RVC_RACES misses)
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include "mcw.h"
#include "mcwx.h"

extern unsigned mcw_faults;
static int workers, failed;

// ---- the jobs: functions of this program, which a worker calls as core 0 would ----

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

struct span { uint32_t* words; uint32_t count, seed, step; };

// Fills a span with a pattern and returns the words' sum.
static uint32_t fill(uint32_t span_at, uint32_t unused) {
    const struct span* s = (const struct span*)span_at;
    uint32_t sum = 0;
    (void)unused;
    for (uint32_t i = 0; i < s->count; i++) {
        s->words[i] = pattern(s->seed, i);
        sum += s->words[i];
    }
    return sum;
}

static uint32_t sum(uint32_t words_at, uint32_t count) {
    const uint32_t* p = (const uint32_t*)words_at;
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) total += p[i];
    return total;
}

// Every third 16 bytes of a buffer that three workers fill between them, from `which` on.
static uint32_t stripe(uint32_t span_at, uint32_t which) {
    const struct span* s = (const struct span*)span_at;
    uint32_t total = 0;
    for (uint32_t t = which; t < s->count / 4; t += 3)
        for (uint32_t w = 0; w < 4; w++) {
            s->words[4 * t + w] = pattern(s->seed, 4 * t + w);
            total += s->words[4 * t + w];
        }
    return total;
}

// Writes a word into every page of memory nobody has touched, then reads them all back.
uint32_t mcw_mmu_root(void);
static uint32_t pages(uint32_t bytes_at, uint32_t bytes) {
    volatile uint32_t* p = (volatile uint32_t*)bytes_at;
    uint32_t total = 0;
    for (uint32_t i = 0; i < bytes / 4; i += 1024) p[i] = i + 7;
    for (uint32_t i = 0; i < bytes / 4; i += 1024) total += p[i] + p[i + 1];
    return total;
}

// Floats kept in registers over many passes; the sum's bits.
static uint32_t floats(uint32_t count, uint32_t seed) {
    float a = (float)seed, b = 0.5f, c = 1.0f;
    union { float f; uint32_t u; } bits;
    for (uint32_t i = 0; i < count; i++) {
        a = a * 0.99999f + b;
        b = b * 1.0001f + c * 0.001f;
        c = c + a * 0.000001f;
        if (b > 100.0f) b -= 99.5f;
    }
    bits.f = a + b + c;
    return bits.u;
}

// A system call, as a job makes one: the machine stops the worker at the ecall and core 0 makes
// the call for it (mcw.c).
static long call3(long number, long a, long b, long c, long d, long e, long f) {
    register long a7 __asm__("a7") = number;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    __asm__ volatile(MC_ECALL : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
    return a0;
}

// Asks the kernel for memory, fills it, says a line, gives the memory back; the words' sum.
static uint32_t calls(uint32_t words, uint32_t seed) {
    static const char line[] = {'m', 'c', 't', 'e', 's', 't', ':', ' ', 'a', ' ', 'l', 'i', 'n', 'e', ' ', 'f', 'r', 'o', 'm', ' ', 'a',
                                ' ', 'w', 'o', 'r', 'k', 'e', 'r', 10};
    uint32_t* p = (uint32_t*)call3(222, 0, (long)words * 4, 3, 0x22, -1, 0);   // mmap2: private, of no file
    uint32_t total = 0;
    if ((long)p < 0 && (long)p > -4096) return 0xdead0000u;
    for (uint32_t i = 0; i < words; i++) p[i] = pattern(seed, i);
    for (uint32_t i = 0; i < words; i++) total += p[i];
    if (seed == 1) call3(64, 1, (long)line, sizeof line, 0, 0, 0);            // write
    call3(215, (long)p, (long)words * 4, 0, 0, 0, 0);                          // munmap
    return total;
}

// ---- the second layer's jobs (mcwx.h) ----

struct loop { uint32_t* words; uint32_t seed; };

static void loop_piece(void* ctx, uint32_t from, uint32_t to) {
    const struct loop* l = (const struct loop*)ctx;
    for (uint32_t i = from; i < to; i++) l->words[i] = pattern(l->seed, i);
}

// A piece that asks for a loop of its own: run as one call wherever it is.
static void inner_piece(void* ctx, uint32_t from, uint32_t to) {
    uint32_t* row = (uint32_t*)ctx;
    for (uint32_t i = from; i < to; i++) row[i] = i + 1;
}
static void nested_piece(void* ctx, uint32_t from, uint32_t to) {
    uint32_t* rows = (uint32_t*)ctx;
    for (uint32_t r = from; r < to; r++) mcw_for(16, inner_piece, rows + 16 * r);
}

static void count_piece(void* ctx, uint32_t from, uint32_t to) {
    (void)ctx;
    for (uint32_t i = from; i < to; i++) {
        mcw_add(0, 1);
        mcw_add(1, i);
    }
}

static uint32_t deferred_sum;
static uint32_t defer_add(uint32_t a, uint32_t b) { deferred_sum += a * b; return 0; }

static uint32_t effects(uint32_t which, uint32_t fail) {
    uint32_t* m = mcw_scratch(4096);
    uint32_t total = 0;
    for (uint32_t i = 0; i < 1024; i++) m[i] = pattern(which, i);
    for (uint32_t i = 0; i < 1024; i++) total += m[i];
    mcw_print("mctest: effects: job %u on worker %d, %08x\n", which, mcw_self(), total);
    mcw_defer(defer_add, which, 3);
    if (fail) mcw_fail("mctest: asked to fail", which);
    mcw_scratch_reset();
    return total;
}

static int failures, failed_value;
static void failed_here(int worker, const char* what, uint32_t value) {
    (void)worker;
    (void)what;
    failures++;
    failed_value = (int)value;
}

static uint32_t async_job(uint32_t seed, uint32_t count) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < count; i++) total += pattern(seed, i);
    return total;
}

// Two workers store to the two halves of one 16 bytes in the same passes: on the GPU one of
// them is undone at the commit. For RVC_RACES to find.
static uint32_t race_words[4] __attribute__((aligned(16)));
static uint32_t race_job(uint32_t half, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        race_words[half * 2] = i;
        mc_end_pass();
    }
    return race_words[half * 2];
}

// The same two halves stored in turns, one worker a pass, the other the next: never in one pass on rvc_cpu, where
// every core's pass ends at its budget, but on the card a pass ends where it ends, so the two may meet in one and
// the commit undo one (Half-Life's statics beside another core's, 632e982). For RVC_RACES_NEAR to find.
static uint32_t near_job(uint32_t half, uint32_t count) {
    if (half) mc_end_pass();
    for (uint32_t i = 0; i < count; i++) {
        race_words[half * 2] = i + 1;
        mc_end_pass();
        mc_end_pass();
    }
    return race_words[half * 2];
}

// ---- core 0 ----

static uint32_t guest_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)tv.tv_sec * 1000u + (uint32_t)tv.tv_usec / 1000u;
}

static void check(const char* what, int ok) {
    if (!ok) failed = 1;
    printf("mctest: %-7s %s\n", what, ok ? "ok" : "FAILED");
}

int main(int argc, char** argv) {
    int want = argc > 1 ? atoi(argv[1]) : MC_MAX_CORES - 1;
    uint32_t limit = argc > 2 ? (uint32_t)atoi(argv[2]) : 60000;
    if (argc > 4 && !strcmp(argv[3], "share") && !strcmp(argv[4], "lend")) mcw_lend();
    workers = mcw_open(want);
    printf("mctest: %d worker core%s (%u pages brought for their start)\n", workers, workers == 1 ? "" : "s", mcw_faults);
    if (workers == 0) { printf("mctest: FAIL (run the machine with --cores N; or other programs have the workers)\n"); return 1; }

    // "mctest N SECONDS share": the kernel shares the cores out between programs
    // (docs/machine/multicore.md). Every worker is kept at short jobs for so many seconds, and a line
    // a second says how many of them have cores and where the jobs were run. The primes'
    // number must come out the same wherever they were counted.
    if (argc > 3 && !strcmp(argv[3], "share")) {
        uint32_t end = guest_ms() + limit * 1000u, said = guest_ms(), on_cores = 0, here = 0, wrong = 0, jobs = 0;
        const uint32_t right = primes(100000, 104000);
        uint8_t was_bound[MC_MAX_CORES] = {0}, out[MC_MAX_CORES] = {0};
        while (guest_ms() < end) {
            for (int k = 1; k <= workers; k++) {
                if (!mcw_done(k)) continue;
                if (out[k]) {
                    if (mcw_result(k) != right) wrong++;
                    if (was_bound[k]) on_cores++; else here++;
                    jobs++;
                }
                was_bound[k] = (uint8_t)mcw_bound(k);
                // (one that has no core is not given a job here: it would be this core's, and the test is of the others)
                out[k] = was_bound[k];
                if (was_bound[k]) mcw_post(k, primes, 100000, 104000);
            }
            if (guest_ms() - said >= 1000) {
                int bound = 0;
                for (int k = 1; k <= workers; k++) bound += mcw_bound(k);
                printf("mctest: share: %d workers, %d with cores; %u jobs on cores, %u here, %u wrong\n", workers, bound, on_cores, here, wrong);
                said += 1000;
                on_cores = here = 0;
            }
            mc_next_pass();
        }
        for (int k = 1; k <= workers; k++) mcw_wait(k);
        printf("mctest: share: done, %u jobs, %u wrong\n", jobs, wrong);
        mcw_close();
        return wrong != 0;
    }

    if (argc > 3 && (!strcmp(argv[3], "race") || !strcmp(argv[3], "near"))) {
        if (workers < 2) return 1;
        uint32_t (*job)(uint32_t, uint32_t) = !strcmp(argv[3], "race") ? race_job : near_job;
        mcw_post(1, job, 0, 50);
        mcw_post(2, job, 1, 50);
        mcw_wait(1);
        mcw_wait(2);
        printf("mctest: race: %u %u %u %u\n", race_words[0], race_words[1], race_words[2], race_words[3]);
        mcw_close();
        return 0;
    }

    // "mctest N LIMIT bench": what a pass costs with more cores busy. Each line is the same
    // work on every busy core (primes of one range, or of ranges of one length further up),
    // so the time is the pass's, not the work's.
    if (argc > 3 && !strcmp(argv[3], "bench")) {
        for (int mode = 0; mode < 6; mode++) {
            if (argc > 4 && atoi(argv[4]) != mode) continue;   // one line only
            // busy workers; whether core 0 works too; whether the workers' ranges differ
            static const int busy_of[6] = {0, 1, 3, 3, MC_MAX_CORES, 1}, main_of[6] = {1, 1, 1, 1, 0, 0}, apart_of[6] = {0, 0, 0, 1, 0, 0};
            int busy = busy_of[mode] < workers ? busy_of[mode] : workers;
            uint32_t t0 = guest_ms(), total = 0;
            for (int k = 1; k <= busy; k++) {
                uint32_t from = apart_of[mode] ? limit * (uint32_t)k : 0;
                mcw_post(k, primes, from, from + limit);
            }
            if (main_of[mode]) total = primes(0, limit);
            for (int k = 1; k <= busy; k++) { mcw_wait(k); total += mcw_result(k); }
            printf("mctest: bench: core 0 %s, %d worker%s on %s: %u ms (%u)\n", main_of[mode] ? "working" : "waiting", busy,
                   busy == 1 ? "" : "s", apart_of[mode] ? "ranges of their own" : "the same range", guest_ms() - t0, total);
        }
        mcw_close();   // parked: the next program starts them again
        return 0;
    }

    // ---- primes: the same range on core 0 alone, then a share each ----
    {
        int parts = workers + 1;
        uint32_t share = limit / (uint32_t)parts, t0 = guest_ms();
        uint32_t alone = primes(0, share * (uint32_t)parts);
        uint32_t t1 = guest_ms();
        for (int k = 1; k <= workers; k++) mcw_post(k, primes, share * (uint32_t)k, share * (uint32_t)(k + 1));
        uint32_t together = primes(0, share);
        for (int k = 1; k <= workers; k++) { mcw_wait(k); together += mcw_result(k); }
        uint32_t t2 = guest_ms();
        printf("mctest: primes below %u: %u alone in %u ms, %u with %d workers in %u ms\n", share * (uint32_t)parts, alone,
               t1 - t0, together, workers, t2 - t1);
        check("primes", alone == together);
    }

    // ---- fill: 64 KiB each, a buffer of its own ----
    {
        const uint32_t words = 16384;
        static struct span spans[MC_MAX_CORES];
        int ok = 1, passes = 0;
        uint32_t t0 = guest_ms();
        for (int k = 1; k <= workers; k++) {
            spans[k] = (struct span){mcw_alloc(words * 4), words, (uint32_t)k, 0};
            if (!spans[k].words) { printf("mctest: no memory\n"); return 1; }
        }
        for (int k = 1; k <= workers; k++) mcw_post(k, fill, (uint32_t)&spans[k], 0);
        for (int k = 1; k <= workers; k++) {
            int p = mcw_wait(k);
            if (p > passes) passes = p;
        }
        uint32_t t1 = guest_ms();
        for (int k = 1; k <= workers; k++) {
            const uint32_t* p = spans[k].words;
            uint32_t total = 0, bad = 0;
            for (uint32_t i = 0; i < words; i++) {
                total += p[i];
                bad += p[i] != pattern((uint32_t)k, i);
            }
            if (bad || total != mcw_result(k)) {
                printf("mctest: worker %d's buffer: %u wrong words, sum %08x, its own %08x\n", k, bad, total, mcw_result(k));
                ok = 0;
            }
        }
        printf("mctest: fill: %u KiB a worker in %d passes, %u ms\n", words / 256, passes, t1 - t0);
        check("fill", ok);
    }

    // ---- stripe: one buffer, every third 16 bytes a worker's ----
    if (workers >= 3) {
        const uint32_t texels = 3000;
        static struct span one;
        int ok = 1;
        one = (struct span){mcw_alloc(texels * 16), texels * 4, 0x57a1, 0};
        memset(one.words, 0, texels * 16);
        for (int k = 1; k <= 3; k++) mcw_post(k, stripe, (uint32_t)&one, (uint32_t)(k - 1));
        for (int k = 1; k <= 3; k++) mcw_wait(k);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < texels * 4; i++) bad += one.words[i] != pattern(0x57a1, i);
        if (bad) { printf("mctest: stripe: %u wrong words of %u\n", bad, texels * 4); ok = 0; }
        check("stripe", ok);

        // ---- sum: each worker reads all of it, the other two's words among them ----
        uint32_t expect = 0;
        for (uint32_t i = 0; i < texels * 4; i++) expect += pattern(0x57a1, i);
        for (int k = 1; k <= 3; k++) mcw_post(k, sum, (uint32_t)one.words, texels * 4);
        for (int k = 1; k <= 3; k++) {
            mcw_wait(k);
            if (mcw_result(k) != expect) {
                printf("mctest: sum: worker %d has %08x, expected %08x\n", k, mcw_result(k), expect);
                ok = 0;
            }
        }
        check("sum", ok);
    }

    // ---- pages: memory that no core has touched, 64 pages of it ----
    {
        const uint32_t bytes = 64 * 4096;
        uint8_t* fresh = mmap(0, bytes, PROT_READ | PROT_WRITE, 0x22, -1, 0);   // private, of no file
        uint32_t before = mcw_faults, expect = 0;
        for (uint32_t i = 0; i < bytes / 4; i += 1024) expect += i + 7;
        mcw_post(1, pages, (uint32_t)fresh, bytes);
        int passes = mcw_wait(1);
        printf("mctest: pages: %u of them brought for the worker in %d passes\n", mcw_faults - before, passes);
        check("pages", fresh != MAP_FAILED && mcw_result(1) == expect && (mcw_faults - before >= 64 || !mcw_mmu_root()));
    }

    // ---- calls: a worker's system calls are made by core 0 ----
    {
        int ok = 1;
        for (int k = 1; k <= workers; k++) mcw_post(k, calls, 4096, (uint32_t)k);
        for (int k = 1; k <= workers; k++) {
            uint32_t expect = 0;
            for (uint32_t i = 0; i < 4096; i++) expect += pattern((uint32_t)k, i);
            mcw_wait(k);
            if (mcw_result(k) != expect) {
                printf("mctest: calls: worker %d has %08x, expected %08x%c", k, mcw_result(k), expect, 10);
                ok = 0;
            }
        }
        check("calls", ok);
    }

    // ---- floats: a worker's float registers are its own, and last from pass to pass ----
    {
        int ok = 1;
        for (int k = 1; k <= workers; k++) mcw_post(k, floats, limit * 4, (uint32_t)k);
        for (int k = 1; k <= workers; k++) {
            uint32_t here = floats(limit * 4, (uint32_t)k);
            mcw_wait(k);
            if (mcw_result(k) != here) {
                printf("mctest: floats: worker %d has %08x, core 0 %08x\n", k, mcw_result(k), here);
                ok = 0;
            }
        }
        check("floats", ok);
    }

    // ---- loop: rounds of mcw_for, one after another ----
    {
        const uint32_t words = 64 * 1024, rounds = 8;
        static struct loop l;
        int ok = 1;
        uint32_t t0 = guest_ms(), bad = 0;
        l.words = mcw_alloc(words * 4);
        mcw_for_opts o = {0, 4, l.words, words * 4, 0, 0};
        for (uint32_t r = 0; r < rounds; r++) {
            l.seed = 100 + r;
            mcw_for_opt(words, loop_piece, &l, &o);
            for (uint32_t i = 0; i < words; i++) bad += l.words[i] != pattern(100 + r, i);
        }
        if (bad) { printf("mctest: loop: %u wrong words\n", bad); ok = 0; }
        printf("mctest: loop: %u rounds of %u words, %u ms\n", rounds, words, guest_ms() - t0);
        // rounds back to back: the team waits on the round word between them
        uint32_t t1 = guest_ms();
        for (uint32_t r = 0; r < 64; r++) {
            l.seed = 500 + r;
            mcw_for_opt(4096, loop_piece, &l, &o);
        }
        for (uint32_t i = 0; i < 4096; i++) bad += l.words[i] != pattern(563, i);
        printf("mctest: loop: 64 rounds of 4096 words back to back, %u ms\n", guest_ms() - t1);
        // a loop asked inside a piece
        static uint32_t rows[64 * 16] __attribute__((aligned(16)));
        mcw_for(64, nested_piece, rows);
        for (uint32_t i = 0; i < 64 * 16; i++) bad += rows[i] != i % 16 + 1;
        if (bad) { printf("mctest: loop: %u wrong words after the rounds\n", bad); ok = 0; }
        check("loop", ok);
    }

    // ---- async: more calls than workers, joined last to first ----
    {
        enum { CALLS = 24 };
        mcw_handle h[CALLS];
        int ok = 1;
        fflush(stdout);
        for (int i = 0; i < CALLS; i++) h[i] = mcw_call(async_job, (uint32_t)i, 20000);
        for (int i = CALLS - 1; i >= 0; i--) {
            uint32_t got = mcw_join(h[i]), expect = async_job((uint32_t)i, 20000);
            if (got != expect) { printf("mctest: async: call %d has %08x, expected %08x\n", i, got, expect); ok = 0; }
        }
        check("async", ok);
    }

    // ---- effects: printing, a deferred call, scratch memory and a failure, from a job ----
    {
        int ok = 1;
        uint32_t expect_sum = 0;
        mcw_handle h[4];
        fflush(stdout);
        mcw_on_failure(failed_here);
        deferred_sum = 0;
        failures = 0;
        mcw_for_end();   // (the team leaves: the calls have its workers)
        for (uint32_t i = 0; i < 4; i++) h[i] = mcw_call(effects, i + 1, i == 2);
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t got = mcw_join(h[i]), expect = 0;
            for (uint32_t j = 0; j < 1024; j++) expect += pattern(i + 1, j);
            if (i != 2 && got != expect) { printf("mctest: effects: job %u has %08x, expected %08x\n", i + 1, got, expect); ok = 0; }
            expect_sum += (i + 1) * 3;
        }
        if (deferred_sum != expect_sum || failures != 1 || failed_value != 3) {
            printf("mctest: effects: deferred %u (expected %u), %d failures (value %d)\n", deferred_sum, expect_sum, failures, failed_value);
            ok = 0;
        }
        mcw_on_failure(0);
        check("effects", ok);
    }

    // ---- counters: every core adds to two of them in the same passes ----
    {
        const uint32_t n = 20000;
        int ok = 1;
        mcw_counters();
        mcw_set(0, 0);
        mcw_set(1, 7);
        mcw_for_opts o = {16, 0, 0, 0, 0, 0};
        mcw_for_opt(n, count_piece, 0, &o);
        mcw_counters_settle();
        uint32_t got = mcw_get(0), sum = mcw_get(1), expect = 7;
        for (uint32_t i = 0; i < n; i++) expect += i;
        if (got != n || sum != expect) { printf("mctest: counters: %u and %u, expected %u and %u\n", got, sum, n, expect); ok = 0; }
        printf("mctest: counters: %s\n", mcw_counters_merged() ? "merged by the machine" : "added up here");
        check("counters", ok);
    }

    mcw_close();   // parked: the next program starts them again
    check("park", 1);
    printf("mctest: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
