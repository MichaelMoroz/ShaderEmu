/* The RISC-V machine's cost of one instruction of a kind, by the guest's clock: a loop of 100 of them unrolled
 * (plus the loop's addi and bnez), run for about SECONDS each.
 *
 *   rvlat [SECONDS]      prints "rvlat: KIND N instructions in T s: X ns an instruction"
 *
 * add: a dependent chain (add a0, a0, a1); add4: four independent chains; lw: a pointer chase in 1 KB; lwi: loads
 * of one word, independent; sw: stores to 16 words of one 64-byte block; mix: lw, add, sw, add of a chain. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

#define R10(x) x x x x x x x x x x
#define R100(x) R10(R10(x))

static uint32_t chase[256] __attribute__((aligned(64)));
static uint32_t block[16] __attribute__((aligned(64)));

static uint32_t run(int kind, uint32_t loops)
{
	uint32_t a = 1, b = 3, c = 5, d = 7, n = loops;
	uint32_t *p = &chase[0];
	switch (kind) {
	case 0: __asm__ volatile("1:\n" R100("add %0, %0, %1\n") "addi %2, %2, -1\nbnez %2, 1b\n" : "+r"(a), "+r"(b), "+r"(n)); break;
	case 1: __asm__ volatile("1:\n" R10(R10("add %0, %0, %4\n") R10("add %1, %1, %4\n") R10("add %2, %2, %4\n") R10("add %3, %3, %4\n")) "" : "+r"(a), "+r"(b), "+r"(c), "+r"(d) : "r"(n));
		/* (25 rounds of 4 chains: 100 adds, then the loop) */
		break;
	case 2: __asm__ volatile("1:\n" R100("lw %0, 0(%0)\n") "addi %1, %1, -1\nbnez %1, 1b\n" : "+r"(p), "+r"(n)); a = (uint32_t)(uintptr_t)p; break;
	case 3: __asm__ volatile("1:\n" R100("lw %0, 0(%2)\n") "addi %1, %1, -1\nbnez %1, 1b\n" : "=&r"(a), "+r"(n) : "r"(block)); break;
	case 4: __asm__ volatile("1:\n" R10("sw %1, 0(%2)\nsw %1, 4(%2)\nsw %1, 8(%2)\nsw %1, 12(%2)\nsw %1, 16(%2)\nsw %1, 20(%2)\nsw %1, 24(%2)\nsw %1, 28(%2)\nsw %1, 32(%2)\nsw %1, 36(%2)\n") "addi %0, %0, -1\nbnez %0, 1b\n" : "+r"(n) : "r"(b), "r"(block) : "memory"); break;
	case 5: __asm__ volatile("1:\n" R10(R10("lw %1, 0(%3)\nadd %1, %1, %0\nsw %1, 4(%3)\naddi %0, %1, 1\n")) "addi %2, %2, -1\nbnez %2, 1b\n" : "+r"(a), "=&r"(b), "+r"(n) : "r"(block) : "memory"); break;
	}
	return a + b + c + d;
}

int main(int argc, char **argv)
{
	static const char *names[] = {"add", "add4", "lw", "lwi", "sw", "mix"};
	static const double per[] = {102, 100, 102, 102, 102, 402};   /* instructions a loop turn */
	double secs = argc > 1 ? atof(argv[1]) : 3;
	for (int i = 0; i < 256; i++) chase[i] = (uint32_t)(uintptr_t)&chase[(i * 97 + 31) & 255];
	for (int k = 0; k < 6; k++) {
		if (k == 1) continue;   /* (add4's loop has no counter: left out) */
		uint32_t loops = 64, sink = 0;
		double t0 = now(), t;
		while ((t = now() - t0) < 0.5) { sink += run(k, loops); loops *= 2; }   /* warm and size */
		loops = (uint32_t)(loops * secs / (t > 0 ? t : 1)) / 2 + 1;
		t0 = now();
		sink += run(k, loops);
		t = now() - t0;
		double ins = (double)loops * per[k];
		printf("rvlat: %s %.0f instructions in %.2f s: %.1f ns an instruction (%u)\n", names[k], ins, t, t * 1e9 / ins, sink & 1);
	}
	return 0;
}
