/* Soft doubles on random operands, a checksum a kind: the same program built for the host (x86-64, SSE: IEEE's
   round to nearest) and for the machine with libcompiler_rt.a must print the same lines (toolchain.sh, "64-bit division,
   doubles"): gcc -O2 -ffp-contract=off fpcheck.c; rv32-cc.mmu -O2 fpcheck.c, run under qemu-riscv32. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static uint64_t st = 0x2545F4914F6CDD1DULL;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static double rd(void)
{
	uint64_t b = rnd();
	switch (rnd() % 6) {
	case 0: b = (b & 0x800fffffffffffffULL) | ((uint64_t)(1023 + (int)(rnd() % 64) - 32) << 52); break;   /* near 1 */
	case 1: b = (b & 0x800fffffffffffffULL) | ((uint64_t)(rnd() % 3) << 52); break;   /* subnormal / tiny */
	case 2: b = (b & 0x800fffffffffffffULL) | ((uint64_t)(2045 + rnd() % 3) << 52); break;   /* huge, inf, nan */
	case 3: { double d = (double)(int32_t)rnd(); memcpy(&b, &d, 8); break; }
	case 4: b &= 0xfff0000000000000ULL | (rnd() & 0xff); break;
	default: break;
	}
	double d; memcpy(&d, &b, 8); return d;
}
static uint64_t h;
static void mixd(double d) { uint64_t b; if (d != d) b = 1; else memcpy(&b, &d, 8); h = (h ^ b) * 0x100000001b3ULL; }
static void mixf(float f) { uint32_t b; if (f != f) b = 1; else memcpy(&b, &f, 4); h = (h ^ b) * 0x100000001b3ULL; }
static void mixi(int64_t i) { h = (h ^ (uint64_t)i) * 0x100000001b3ULL; }
#define N 300000
int main(void)
{
	static double A[N], B[N]; static float F[N]; static int64_t L[N];
	for (int i = 0; i < N; i++) { A[i] = rd(); B[i] = (i % 7 == 0) ? A[i] * 1.0000001 : rd(); uint32_t fb = rnd(); if (i & 1) fb = (fb & 0x807fffff) | ((uint32_t)(100 + rnd() % 60) << 23); memcpy(&F[i], &fb, 4);
		L[i] = (int64_t)(rnd() >> (rnd() % 64)) * ((i & 2) ? -1 : 1); }
	/* (A and B as made here use the hardware's ops only through bit moves, the multiply of case i%7 aside: made again from bits) */
	for (int i = 0; i < N; i += 7) { uint64_t b; memcpy(&b, &A[i], 8); b ^= rnd() & 3; memcpy(&B[i], &b, 8); }
	volatile double *va = A, *vb = B;
	h = 1; for (int i = 0; i < N; i++) mixd(va[i] + vb[i]); printf("add %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) mixd(va[i] - vb[i]); printf("sub %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) mixd(va[i] * vb[i]); printf("mul %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) mixd(va[i] / vb[i]); printf("div %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) mixi((va[i] < vb[i]) + 2 * (va[i] <= vb[i]) + 4 * (va[i] == vb[i]) + 8 * (va[i] > vb[i]) + 16 * (va[i] >= vb[i]) + 32 * (va[i] != vb[i])); printf("cmp %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { double d = va[i]; if (d > -2e9 && d < 2e9) mixi((int)d); } printf("d2i %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { double d = va[i]; if (d > -1 && d < 4e9) mixi((unsigned)d); } printf("d2u %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { double d = va[i]; if (d > -9e18 && d < 9e18) mixi((int64_t)d); } printf("d2l %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { double d = va[i]; if (d > -1 && d < 1.8e19) mixi((int64_t)(uint64_t)d); } printf("d2ul %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { volatile int32_t x = (int32_t)L[i]; mixd((double)x); } printf("i2d %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { volatile uint32_t x = (uint32_t)L[i]; mixd((double)x); } printf("u2d %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { volatile int64_t x = L[i]; mixd((double)x); } printf("l2d %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { volatile uint64_t x = (uint64_t)L[i]; mixd((double)x); } printf("ul2d %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { volatile int64_t x = L[i]; mixf((float)x); } printf("l2f %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { volatile uint64_t x = (uint64_t)L[i]; mixf((float)x); } printf("ul2f %016llx\n", (unsigned long long)h);
	volatile float *vf = F;
	h = 1; for (int i = 0; i < N; i++) mixd((double)vf[i]); printf("f2d %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) mixf((float)va[i]); printf("d2f %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { float f = vf[i]; if (f > -9e18f && f < 9e18f) mixi((int64_t)f); } printf("f2l %016llx\n", (unsigned long long)h);
	h = 1; for (int i = 0; i < N; i++) { float f = vf[i]; if (f > -1 && f < 1.8e19f) mixi((int64_t)(uint64_t)f); } printf("f2ul %016llx\n", (unsigned long long)h);
	return 0;
}
