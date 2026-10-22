// The worker cores with no kernel (gpu_vm/platform.md, "Workers"): core 0, bare and semihosted, lays the workers
// out by the control words, starts them through their start words and gives them jobs through their mailboxes, as
// programs/mc/mcw.c does under Linux. Built by gpu_vm/platform/test/run.sh, run by gvm_cpu --cores N (and gvm_gpu).
//   workers [N]     N workers (3); exit 0 when every check holds
// Checks: jobs on every worker (a count split N + 1 ways equals core 0's alone); a system call made for a worker; a
// fault recorded with RISC-V's cause; a running job parked by MCST and started again; break parks, MCST and MCSU
// start it again; two workers storing two halves of one word in the same pass (on gpu_vm the higher core's word wins:
// GVM_RACES reports it as undone), and two words of one texel (both kept).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CTRL 0x01000000u
#define MBOX 0x00c00000u
#define MCSU 0x5553434du
#define MCST 0x5453434du
#define V(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define START(k) (MBOX + 16u * (k))
#define JOB(k) (MBOX + 0x4000u + 64u * (k))
#define ANSWER(k) (JOB(k) + 16)
#define FAULT(k) (JOB(k) + 32)
#define RESUME(k) (JOB(k) + 48)
#define CALL(k) (MBOX + 0x2000u + 32u * (k))
#define MAXW 16

typedef uint32_t (*job_fn)(uint32_t, uint32_t);
static uint8_t stacks[MAXW + 1][8192] __attribute__((aligned(16)));
static uint32_t seq[MAXW + 1];
static int nw;
static uint32_t share_word[4] __attribute__((aligned(16)));   // one word, two cores' halves
static uint32_t texel_words[4] __attribute__((aligned(16)));  // one texel, two cores' words

static void end_pass(void) { __asm__ volatile("sync" ::: "memory"); }   // (core 0 is in supervisor mode: sync ends its pass)
static void user_pass(void) { __asm__ volatile("wait" ::: "memory"); }  // a worker's: a user's wait ends the pass

// ---- what the workers run (user mode)
static long wcall3(long n, long a, long b, long c) {
    register long a7 __asm__("r12") = n;
    register long a0 __asm__("r5") = a;
    register long a1 __asm__("r6") = b;
    register long a2 __asm__("r7") = c;
    __asm__ volatile("syscall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}
void worker_main(uint32_t k) {
    uint32_t last = V(JOB(k));
    V(ANSWER(k)) = last;
    V(ANSWER(k) + 8) = 0x600d0000u | k;
    for (;;) {
        uint32_t at = V(JOB(k));
        if (at != last) {
            job_fn fn = (job_fn)(uintptr_t)V(JOB(k) + 4);
            if (!fn) {
                V(ANSWER(k)) = at;
                __asm__ volatile("break");
            }
            V(ANSWER(k) + 4) = fn(V(JOB(k) + 8), V(JOB(k) + 12));
            V(ANSWER(k)) = at;
            last = at;
            continue;
        }
        user_pass();
    }
}
static uint32_t primes(uint32_t from, uint32_t to) {
    uint32_t n = 0;
    for (uint32_t v = from; v < to; v++) {
        uint32_t d = 2;
        if (v < 2) continue;
        while (d * d <= v && v % d) d++;
        n += d * d > v;
    }
    return n;
}
static char hello[] = "workers: a line from a worker core\n";
static uint32_t say(uint32_t a, uint32_t b) { return (uint32_t)wcall3(64, 1, (long)a, (long)b); }
static uint32_t fault(uint32_t a, uint32_t b) { (void)b; return V(a); }
static uint32_t spin(uint32_t a, uint32_t b) { (void)a; (void)b; for (;;) V(ANSWER(1) + 12) = V(ANSWER(1) + 12) + 1; }
static uint32_t halves(uint32_t half, uint32_t passes) {   // a half of share_word, pass after pass
    for (uint32_t p = 0; p < passes; p++) {
        ((volatile uint16_t *)share_word)[half] = (uint16_t)(0x100 + p);
        user_pass();
    }
    return 0;
}
static uint32_t words(uint32_t w, uint32_t passes) {
    for (uint32_t p = 0; p < passes; p++) {
        ((volatile uint32_t *)texel_words)[w] = 0x1000 + p;
        user_pass();
    }
    return 0;
}

// ---- core 0
static int failures;
static void check(int ok, const char *what) {
    printf("workers: %s: %s\n", what, ok ? "ok" : "WRONG");
    failures += !ok;
}
// A worker that stopped: a system call made for it, a fault left for the caller (returns its cause, else 0)
static uint32_t serve(int k) {
    uint32_t count = V(FAULT(k)), resume = V(RESUME(k));
    if (count == resume) return 0;
    uint32_t cause = V(FAULT(k) + 4);
    if (cause != 8) return cause;
    uint32_t n = V(CALL(k)), a[6];
    for (int i = 0; i < 6; i++) a[i] = V(CALL(k) + 4 + 4 * i);
    long r = -38;
    if (n == 64) r = (long)fwrite((const void *)(uintptr_t)a[1], 1, a[2], a[0] == 2 ? stderr : stdout);
    V(RESUME(k) + 4) = (uint32_t)r;
    V(RESUME(k)) = count;
    return 0;
}
static void post(int k, job_fn fn, uint32_t a, uint32_t b) {
    V(JOB(k) + 4) = (uint32_t)(uintptr_t)fn;
    V(JOB(k) + 8) = a;
    V(JOB(k) + 12) = b;
    V(JOB(k)) = ++seq[k];
}
static int done(int k) { return V(ANSWER(k)) == seq[k]; }
// Passes until worker k has answered (serving its calls), or `most` passes; its fault's cause if it stopped at one
static uint32_t wait_for(int k, int most) {
    for (int p = 0; p < most; p++) {
        uint32_t c = serve(k);
        if (c) return c;
        if (done(k)) return 0;
        end_pass();
    }
    return 0xdead;
}
static void start(int k) {
    V(START(k) + 4) = (uint32_t)(uintptr_t)worker_main;
    V(START(k) + 8) = (uint32_t)(uintptr_t)(stacks[k] + sizeof stacks[k]);
    V(START(k) + 12) = 0;
    V(START(k)) = MCSU;
}
static int alive(int k, int most) {
    for (int p = 0; p < most; p++) {
        if (V(ANSWER(k) + 8) == (0x600d0000u | (uint32_t)k)) return 1;
        end_pass();
    }
    return 0;
}

int main(int argc, char **argv) {
    nw = argc > 1 ? atoi(argv[1]) : 3;
    if (nw < 2 || nw > MAXW) nw = 3;
    uint32_t most = V(CTRL + 0x3cc);
    printf("workers: %d workers; the machine could have %u cores\n", nw, most);
    // the layout, as the kernel's workers_layout makes it: every start word MCST, then the count and the size
    for (int k = 1; k <= nw; k++) V(START(k)) = MCST;
    V(CTRL + 0x3d0) = 1;
    end_pass();
    V(CTRL + 0x3d4) = (uint32_t)nw;
    V(CTRL + 0x3d8) = 5;
    end_pass();
    V(CTRL + 0x3d0) = 0;
    for (int p = 0; p < 4; p++) end_pass();
    check(V(CTRL + 0x3c0) == (uint32_t)nw + 1 || most == 0, "the cores counted in the control words");
    for (int k = 1; k <= nw; k++) {
        for (int i = 0; i < 16; i += 4) V(JOB(k) + i) = 0, V(ANSWER(k) + i) = 0, V(FAULT(k) + i) = 0, V(RESUME(k) + i) = 0;
        V(START(k)) = 0;
    }
    end_pass();
    for (int k = 1; k <= nw; k++) start(k);
    int up = 1;
    for (int k = 1; k <= nw; k++) up = up && alive(k, 20);
    check(up, "every worker started");

    // a count split nw + 1 ways
    uint32_t total = 0, part = 40000 / (uint32_t)(nw + 1);
    for (int k = 1; k <= nw; k++) post(k, primes, part * (uint32_t)k, part * (uint32_t)(k + 1));
    total = primes(0, part);
    for (int k = 1; k <= nw; k++) { wait_for(k, 2000); total += V(ANSWER(k) + 4); }
    uint32_t alone = primes(0, part * (uint32_t)(nw + 1));
    printf("workers: primes below %u: %u on %d cores, %u on core 0 alone\n", part * (nw + 1), total, nw + 1, alone);
    check(total == alone, "primes");

    // a system call made for a worker
    post(1, say, (uint32_t)(uintptr_t)hello, sizeof hello - 1);
    check(wait_for(1, 200) == 0 && V(ANSWER(1) + 4) == sizeof hello - 1, "a worker's write");

    // a fault: RISC-V's load fault (5), the address and the pc
    post(2, fault, 0x7ffffff0u, 0);
    uint32_t cause = wait_for(2, 200);
    printf("workers: worker 2 stopped: cause %u, address %08x, pc %08x\n", cause, V(FAULT(2) + 8), V(FAULT(2) + 12));
    check(cause == 5 && V(FAULT(2) + 8) == 0x7ffffff0u, "a fault recorded");
    // ... parked by MCST and started again
    V(START(2)) = MCST;
    end_pass(); end_pass();
    V(RESUME(2)) = V(FAULT(2));
    V(ANSWER(2) + 8) = 0;
    end_pass();
    start(2);
    check(alive(2, 20), "a stopped worker started again");

    // a job that runs for ever, parked by MCST within a pass or two
    post(1, spin, 0, 0);
    for (int p = 0; p < 3; p++) end_pass();
    V(START(1)) = MCST;
    end_pass(); end_pass(); end_pass();
    uint32_t c1 = V(ANSWER(1) + 12);
    for (int p = 0; p < 4; p++) end_pass();
    check(c1 != 0 && V(ANSWER(1) + 12) == c1, "a running job parked by MCST");
    V(ANSWER(1) + 8) = 0;
    seq[1] = V(JOB(1));
    end_pass();
    start(1);
    check(alive(1, 20), "and started again");

    // break parks (fn 0), MCSU at the same pc does not start it again, MCST then MCSU does
    post(3 <= nw ? 3 : 1, 0, 0, 0);
    int b = 3 <= nw ? 3 : 1;
    wait_for(b, 50);
    V(ANSWER(b) + 8) = 0;
    end_pass();
    start(b);
    for (int p = 0; p < 6; p++) end_pass();
    int again = V(ANSWER(b) + 8) != 0;
    V(START(b)) = MCST;
    end_pass(); end_pass();
    start(b);
    check(!again && alive(b, 20), "break parks; MCST and MCSU start it again");
    seq[b] = V(JOB(b));

    // two halves of one word, and two words of one texel, from two workers in the same passes
    share_word[0] = 0; texel_words[0] = texel_words[1] = 0;
    end_pass();
    post(1, halves, 0, 40);
    post(2, halves, 1, 40);
    wait_for(1, 200); wait_for(2, 200);
    printf("workers: one word, two halves: %08x (the higher core's word wins: 0x0127xxxx, its low half 0)\n", share_word[0]);
    check((share_word[0] >> 16) == 0x127 && (share_word[0] & 0xffff) == 0, "two cores in one word: the higher one's");
    post(1, words, 0, 40);
    post(2, words, 1, 40);
    wait_for(1, 200); wait_for(2, 200);
    check(texel_words[0] == 0x1027 && texel_words[1] == 0x1027, "two cores in one texel: both kept");

    // the workers leave: fn 0 parks them
    for (int k = 1; k <= nw; k++) post(k, 0, 0, 0);
    for (int k = 1; k <= nw; k++) wait_for(k, 50);
    printf("workers: %s\n", failures ? "FAILED" : "PASS");
    return failures != 0;
}
