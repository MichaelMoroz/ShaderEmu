// isabench's loops as RISC-V writes them, timed by the guest's clock: millions of instructions a second each.
//   isaloops [MILLIONS]   (instructions a loop, 16 by default)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint32_t data[2048] __attribute__((aligned(64)));
static uint32_t far_data[1 << 20] __attribute__((aligned(64)));

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void report(const char *name, uint32_t trips, uint32_t per_trip, double t0) {
    double s = now() - t0, n = (double)trips * per_trip;
    printf("%-8s %10.0f instructions in %7.3f s: %6.2f M a second\n", name, n, s, n / s / 1e6);
}

int main(int argc, char **argv) {
    uint32_t want = (argc > 1 ? atoi(argv[1]) : 16) * 1000000u, n, *p = data, *f = far_data;
    double t;
    for (int i = 0; i < 2048; i++) data[i] = 0x9e3779b9u * (i + 1);

    n = want / 17; t = now();
    __asm__ volatile(
        "1: addi t1, t1, 1\n xor t2, t2, t1\n add t3, t3, t2\n slli t4, t3, 3\n"
        "   sub t5, t4, t1\n or t6, t5, t2\n andi t1, t1, 0x7ff\n srli t2, t6, 2\n"
        "   add t3, t3, t5\n xori t4, t4, 0x55\n sltu t5, t3, t4\n add t6, t6, t5\n"
        "   slli t1, t1, 1\n srai t2, t2, 1\n and t3, t3, t6\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : : "t1", "t2", "t3", "t4", "t5", "t6");
    report("arith", want / 17, 17, t);

    n = want / 16; t = now();
    __asm__ volatile(
        "1: lw t1, 0(%1)\n lw t2, 4(%1)\n lw t3, 8(%1)\n lw t4, 12(%1)\n"
        "   lw t5, 16(%1)\n lw t6, 20(%1)\n lw t1, 24(%1)\n lw t2, 28(%1)\n"
        "   lw t3, 32(%1)\n lw t4, 36(%1)\n lw t5, 40(%1)\n lw t6, 44(%1)\n"
        "   lw t1, 48(%1)\n lw t2, 52(%1)\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : "r"(p) : "t1", "t2", "t3", "t4", "t5", "t6");
    report("loads", want / 16, 16, t);

    n = want / 16; t = now();
    __asm__ volatile(
        "1: sw %0, 0(%1)\n sw %0, 4(%1)\n sw %0, 8(%1)\n sw %0, 12(%1)\n"
        "   sw %0, 16(%1)\n sw %0, 20(%1)\n sw %0, 24(%1)\n sw %0, 28(%1)\n"
        "   sw %0, 32(%1)\n sw %0, 36(%1)\n sw %0, 40(%1)\n sw %0, 44(%1)\n"
        "   sw %0, 48(%1)\n sw %0, 52(%1)\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : "r"(p) : "memory");
    report("stores", want / 16, 16, t);

    n = want / 16; t = now();
    __asm__ volatile(
        "1: add t1, t1, %0\n add t1, t1, %0\n add t1, t1, %0\n add t1, t1, %0\n"
        "   add t1, t1, %0\n add t1, t1, %0\n add t1, t1, %0\n add t1, t1, %0\n"
        "   add t1, t1, %0\n add t1, t1, %0\n add t1, t1, %0\n add t1, t1, %0\n"
        "   add t1, t1, %0\n add t1, t1, %0\n"
        "   addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n) : : "t1");
    report("adds", want / 16, 16, t);

    n = want / 2; t = now();
    __asm__ volatile("1: addi %0, %0, -1\n bnez %0, 1b\n" : "+r"(n));
    report("tight", want / 2, 2, t);

    // mix: isabench's, with the two base-plus-index addresses RISC-V adds apart (12 for its 10)
    n = want / 12; t = now();
    __asm__ volatile(
        "   li t0, 0\n li a4, 0\n"
        "1: add t4, %1, t0\n lw t1, 0(t4)\n add a4, a4, t1\n addi t0, t0, 4\n"
        "   xor t2, t1, a4\n andi t0, t0, 0x7c\n add t5, %2, t0\n sw t2, 0(t5)\n"
        "   slli t3, t2, 1\n add a4, a4, t3\n addi %0, %0, -1\n bnez %0, 1b\n"
        : "+r"(n) : "r"(p), "r"(p + 1024) : "t0", "t1", "t2", "t3", "t4", "t5", "a4", "memory");
    report("mix", want / 12, 12, t);

    // stride: loads striding through 4 MB (7: the address added apart)
    n = want / 7; t = now();
    __asm__ volatile(
        "   li t0, 0\n li a4, 0\n"
        "1: add t4, %1, t0\n lw t1, 0(t4)\n add a4, a4, t1\n addi t0, t0, 1040\n"
        "   and t0, t0, %2\n addi %0, %0, -1\n bnez %0, 1b\n"
        : "+r"(n) : "r"(f), "r"(0x3ffffc) : "t0", "t1", "t4", "a4");
    report("stride", want / 7, 7, t);
    return 0;
}
