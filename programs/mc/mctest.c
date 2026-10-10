// mctest: the worker cores' test (docs/multicore.md). A Linux program on core 0 that starts
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
//
// Prints one line a test and "mctest: PASS" or "mctest: FAIL".
//   mctest [WORKERS [LIMIT [bench [LINE]]]]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include "mcw.h"

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
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
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
    // mctest N LIMIT shape 6,6,5,4,4: the machine's geometry first (docs/multicore.md)
    if (argc > 4 && !strcmp(argv[3], "shape")) {
        unsigned char bits[16];
        int n = 0, answer;
        for (const char* at = argv[4]; *at && n < 15; at++)
            if (*at >= '0' && *at <= '9') bits[n++] = (unsigned char)(*at - '0');
        answer = mcw_shape(bits, n);
        printf("mctest: the geometry: %d workers (%s): %s%c", n, argv[4], answer == 0 ? "set" : "refused", 10);
        if (answer != 0) return 1;
        argc = 3;
    }

    workers = mcw_open(want);
    printf("mctest: %d worker core%s (%u pages brought for their start)\n", workers, workers == 1 ? "" : "s", mcw_faults);
    if (workers == 0) { printf("mctest: FAIL (run the machine with --cores N; or other programs have the workers)\n"); return 1; }

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
        check("pages", fresh != MAP_FAILED && mcw_result(1) == expect && mcw_faults - before >= 64);
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

    mcw_close();   // parked: the next program starts them again
    check("park", 1);
    printf("mctest: %s\n", failed ? "FAIL" : "PASS");
    return failed;
}
