/*
 * nxperf's tests but the GPU's (perf_gpu.c): docs/apps/bench.md says what each measures. A test
 * makes its data first, times only its work (perf_begin, perf_end), and then checks what the work
 * made: an exact sum where the work is in integers or in floats that hold whole numbers, a
 * residual or an error bound where it is not. Sizes are set for a few hundred milliseconds of
 * the card's time each.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mcw.h"
#include "mcwx.h"
#include "media.h"
#include "perf.h"

int perf_workers;

/* ---------------------------------------------------------------- the frame */

uint64_t perf_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void perf_begin(struct perf_run *r)
{
	r->t0 = perf_now_ns();
	r->c0 = perf_cycles();
}

void perf_end(struct perf_run *r)
{
	r->cycles = perf_cycles() - r->c0;
	r->ns = perf_now_ns() - r->t0;
	if (!r->ns) r->ns = 1;
}

void perf_rate(struct perf_run *r, float amount, float scale)
{
	r->value = amount / ((float)r->ns * 1e-9f) / scale;
}

void perf_each_ns(struct perf_run *r, float amount)
{
	r->value = (float)r->ns / amount;
}

void perf_cpo(struct perf_run *r, float ops)
{
	r->cpo = (float)r->cycles / ops;
}

void perf_check(struct perf_run *r, int ok, const char *format, ...)
{
	va_list a;

	va_start(a, format);
	vsnprintf(r->note, sizeof r->note, format, a);
	va_end(a);
	r->ok = ok ? 1 : 0;
}

/* memory kept between tests: slot k, at least `bytes`, on 16 bytes */
static void *bufs[12];
static uint32_t buf_bytes[12];

static void *buf(int k, uint32_t bytes)
{
	if (buf_bytes[k] < bytes) {
		free(bufs[k]);
		bufs[k] = aligned_alloc(16, (bytes + 15) & ~15u);
		buf_bytes[k] = bufs[k] ? bytes : 0;
	}
	return bufs[k];
}

#define NEED(p) do { if (!(p)) { perf_check(r, 0, "no memory"); return; } } while (0)

static volatile uint32_t sink;
static volatile float fsink;
static volatile uint32_t vone = 1;
static volatile float fone = 1.0f;

static uint32_t lcg(uint32_t *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s;
}

/* 1 MB of words 0, 1, 2, ...: what the loads read (made once) */
#define WORDS_A (1u << 18)
static uint32_t *words_a(void)
{
	static int made;
	uint32_t *a = buf(0, WORDS_A * 4), i;

	if (a && !made) {
		for (i = 0; i < WORDS_A; i++) a[i] = i;
		made = 1;
	}
	return a;
}

/* 0 + 1 + ... + (n - 1), as 32-bit sums make it */
static uint32_t sum_to(uint32_t n)
{
	return (uint32_t)((uint64_t)n * (n - 1) / 2);
}

/* ---------------------------------------------------------------- the processor */

/* eight operations, add xor sll srl or sub srl xor, four times a turn */
#define ALU8 a += b; b ^= a; a = (a << 5) | (a >> 27); b -= a; a ^= b >> 3;
static uint32_t __attribute__((noipa)) alu_loop(uint32_t a, uint32_t b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		ALU8 ALU8 ALU8 ALU8
	}
	return a ^ b;
}

static void t_int_alu(struct perf_run *r)
{
	uint32_t n = 25000, got;

	perf_begin(r);
	got = alu_loop(0x12345678u * vone, 0x9abcdef0u, n);
	perf_end(r);
	perf_rate(r, 32.0f * n, 1e6f);
	perf_cpo(r, 32.0f * n);
	perf_check(r, got == 0xc77245e5u, "result %08x", got);
}

/* x = x * a + c, four a turn: f to the n is (A, C) by squaring */
static uint32_t __attribute__((noipa)) mul_loop(uint32_t x, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		x = x * 1664525u + 1013904223u;
		x = x * 1664525u + 1013904223u;
		x = x * 1664525u + 1013904223u;
		x = x * 1664525u + 1013904223u;
	}
	return x;
}

static uint32_t affine_power(uint32_t x, uint32_t a, uint32_t c, uint32_t n)
{
	uint32_t ra = 1, rc = 0;	/* the map so far: ra x + rc */

	while (n) {
		if (n & 1) ra = a * ra, rc = a * rc + c;
		c = a * c + c, a = a * a, n >>= 1;
	}
	return ra * x + rc;
}

static void t_int_mul(struct perf_run *r)
{
	uint32_t n = 50000, x0 = 7 * vone, got, want;

	perf_begin(r);
	got = mul_loop(x0, n);
	perf_end(r);
	perf_rate(r, 4.0f * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	want = affine_power(x0, 1664525u, 1013904223u, 4 * n);
	perf_check(r, got == want, "x %08x, the map's power %08x", got, want);
}

static uint32_t __attribute__((noipa)) div_loop(uint32_t x, uint32_t n)
{
	uint32_t acc = 0, i;

	for (i = 1; i <= n; i++) {
		acc += x / i;
		acc ^= x % (i + 7);
		x = x * 5 + 1;
	}
	return acc;
}

static void t_int_div(struct perf_run *r)
{
	uint32_t n = 40000, got;

	perf_begin(r);
	got = div_loop(0xdeadbeefu * vone, n);
	perf_end(r);
	perf_rate(r, 2.0f * n, 1e6f);
	perf_cpo(r, 2.0f * n);
	perf_check(r, got == 0xfe944a24u, "result %08x", got);
}

/* two branches a word of random bits (the asm keeps them branches) */
static void __attribute__((noipa)) branch_loop(const uint8_t *t, uint32_t n, uint32_t *c)
{
	uint32_t c1 = 0, c2 = 0, c3 = 0, i;

	for (i = 0; i < n; i++) {
		uint32_t v = t[i & 1023];

		if (v & 1) {
			c1++;
			__asm__ volatile("");
		} else {
			c2 += 3;
			__asm__ volatile("");
		}
		if (v & 2) {
			c3++;
			__asm__ volatile("");
		}
	}
	c[0] = c1, c[1] = c2, c[2] = c3;
}

static void t_branch(struct perf_run *r)
{
	static uint8_t t[1024];
	uint32_t s = 99, ones = 0, twos = 0, n = 1024 * 160, c[3], i;

	for (i = 0; i < 1024; i++) {
		t[i] = (uint8_t)(lcg(&s) >> 24);
		ones += t[i] & 1, twos += (t[i] >> 1) & 1;
	}
	perf_begin(r);
	branch_loop(t, n, c);
	perf_end(r);
	perf_rate(r, 2.0f * n, 1e6f);
	perf_cpo(r, 2.0f * n);
	perf_check(r, c[0] == ones * 160 && c[1] == 3 * (n - ones * 160) && c[2] == twos * 160, "taken %u, %u, %u", c[0], c[1], c[2]);
}

static void t_fp_add(struct perf_run *r)
{
	float s0 = 0, s1 = 0, s2 = 0, s3 = 0, one = fone;
	uint32_t n = 100000, i;

	perf_begin(r);
	for (i = 0; i < n; i++) s0 += one, s1 += one, s2 += one, s3 += one;
	perf_end(r);
	perf_rate(r, 4.0f * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	perf_check(r, s0 + s1 + s2 + s3 == 4.0f * n, "sum %.0f of %u", (double)(s0 + s1 + s2 + s3), 4 * n);
}

static int near(float got, float want, float rel)
{
	return fabsf(got - want) <= rel * fabsf(want);
}

static void t_fp_mul(struct perf_run *r)
{
	const float m[4] = {1.00001f, 0.99999f, 1.000005f, 0.999995f};
	float x0 = fone, x1 = fone, x2 = fone, x3 = fone, worst = 0, x[4];
	float m0 = m[0] * fone, m1 = m[1] * fone, m2 = m[2] * fone, m3 = m[3] * fone;
	uint32_t n = 50000, i;
	int k;

	perf_begin(r);
	for (i = 0; i < n; i++) x0 *= m0, x1 *= m1, x2 *= m2, x3 *= m3;
	perf_end(r);
	perf_rate(r, 4.0f * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	x[0] = x0, x[1] = x1, x[2] = x2, x[3] = x3;
	for (k = 0; k < 4; k++) {
		float want = expf((float)n * log1pf(m[k] - 1.0f)), e = fabsf(x[k] - want) / want;

		worst = e > worst ? e : worst;
	}
	perf_check(r, worst < 2e-4f, "worst relative error %.2g against exp", (double)worst);
}

static void t_fp_div(struct perf_run *r)
{
	const float d[4] = {1.00001f, 0.99999f, 1.000005f, 0.999995f};
	float x0 = fone, x1 = fone, x2 = fone, x3 = fone, worst = 0, x[4];
	float d0 = d[0] * fone, d1 = d[1] * fone, d2 = d[2] * fone, d3 = d[3] * fone;
	uint32_t n = 25000, i;
	int k;

	perf_begin(r);
	for (i = 0; i < n; i++) x0 /= d0, x1 /= d1, x2 /= d2, x3 /= d3;
	perf_end(r);
	perf_rate(r, 4.0f * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	x[0] = x0, x[1] = x1, x[2] = x2, x[3] = x3;
	for (k = 0; k < 4; k++) {
		float want = expf(-(float)n * log1pf(d[k] - 1.0f)), e = fabsf(x[k] - want) / want;

		worst = e > worst ? e : worst;
	}
	perf_check(r, worst < 2e-4f, "worst relative error %.2g against exp", (double)worst);
}

static void t_fp_sqrt(struct perf_run *r)
{
	uint32_t n = 65536, i, j;
	float total = 0, want;

	perf_begin(r);
	for (i = 0; i < n; i += 256) {
		float block = 0, f = (float)(i + 1) * fone;

		for (j = 0; j < 256; j++, f += 1.0f) block += sqrtf(f);
		total += block;
	}
	perf_end(r);
	perf_rate(r, (float)n, 1e6f);
	perf_cpo(r, (float)n);
	/* the sum of sqrt(1) .. sqrt(n): (2/3) n^1.5 + n^0.5 / 2 + zeta(-1/2) */
	want = 2.0f / 3.0f * (float)n * sqrtf((float)n) + 0.5f * sqrtf((float)n) - 0.2078862f;
	perf_check(r, near(total, want, 1e-4f), "sum %.1f, Euler-Maclaurin %.1f", (double)total, (double)want);
}

/* ---------------------------------------------------------------- memory on core 0 */

static uint32_t __attribute__((noipa)) sum_stride(const uint32_t *a, uint32_t words, uint32_t step)
{
	uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, o;

	for (o = 0; o < step; o++) {
		const uint32_t *p = a + o, *end = a + words;

		while (p + 3 * step < end) {
			s0 += p[0], s1 += p[step], s2 += p[2 * step], s3 += p[3 * step];
			p += 4 * step;
		}
		while (p < end) s0 += *p, p += step;
	}
	return s0 + s1 + s2 + s3;
}

static void load_test(struct perf_run *r, uint32_t step)
{
	uint32_t *a = words_a(), got;

	NEED(a);
	perf_begin(r);
	got = sum_stride(a, WORDS_A, step);
	perf_end(r);
	perf_rate(r, 4.0f * WORDS_A, 1e6f);
	perf_cpo(r, (float)WORDS_A);
	perf_check(r, got == sum_to(WORDS_A), "sum %08x of 1 MB", got);
}

static void t_load_4(struct perf_run *r) { load_test(r, 1); }
static void t_load_16(struct perf_run *r) { load_test(r, 4); }
static void t_load_64(struct perf_run *r) { load_test(r, 16); }
static void t_load_4k(struct perf_run *r) { load_test(r, 1024); }

/* a random cycle through n words (Sattolo's), walked */
static void chase_test(struct perf_run *r, uint32_t n)
{
	uint32_t *p = buf(1, n * 4), s = 5, i, at = 0, steps;

	NEED(p);
	for (i = 0; i < n; i++) p[i] = i;
	for (i = n - 1; i > 0; i--) {
		uint32_t j = lcg(&s) % i, t = p[i];

		p[i] = p[j], p[j] = t;
	}
	steps = n * ((131072 + n - 1) / n);
	perf_begin(r);
	for (i = 0; i < steps; i++) at = p[at];
	perf_end(r);
	perf_each_ns(r, (float)steps);
	perf_cpo(r, (float)steps);
	perf_check(r, at == 0, "back at %u after %u loads (whole cycles)", at, steps);
}

static void t_lat_4k(struct perf_run *r) { chase_test(r, 1024); }
static void t_lat_64k(struct perf_run *r) { chase_test(r, 16384); }
static void t_lat_256k(struct perf_run *r) { chase_test(r, 65536); }

static void t_store_seq(struct perf_run *r)
{
	uint32_t *b = buf(2, 256 * 1024), n = 65536, rep, i, bad = 0;

	NEED(b);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) {
		uint32_t *p = b, v = rep;

		for (i = 0; i < n; i += 4, p += 4, v += 4) p[0] = v, p[1] = v + 1, p[2] = v + 2, p[3] = v + 3;
	}
	perf_end(r);
	perf_rate(r, 4.0f * 4 * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	for (i = 0; i < n; i++) bad += b[i] != i + 3;
	perf_check(r, bad == 0, "%u of %u words wrong", bad, n);
}

/* stores over 1 KB (64 texels: the write cache keeps them) */
static void t_wcache_hot(struct perf_run *r)
{
	uint32_t *b = buf(2, 1024), rep, i, bad = 0, reps = 1024;

	NEED(b);
	perf_begin(r);
	for (rep = 0; rep < reps; rep++)
		for (i = 0; i < 256; i += 4) b[i] = rep + i, b[i + 1] = rep, b[i + 2] = i, b[i + 3] = rep ^ i;
	perf_end(r);
	perf_rate(r, 256.0f * reps, 1e6f);
	perf_cpo(r, 256.0f * reps);
	for (i = 0; i < 256; i += 4) bad += b[i] != reps - 1 + i || b[i + 1] != reps - 1 || b[i + 2] != i || b[i + 3] != ((reps - 1) ^ i);
	perf_check(r, bad == 0, "%u of 64 texels wrong", bad);
}

/* a word a texel over 1 MB: every store a texel the write cache does not have */
static void t_wcache_cold(struct perf_run *r)
{
	uint32_t *b = buf(2, 1u << 20), rep, i, bad = 0, n = 65536;

	NEED(b);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++)
		for (i = 0; i < n; i++) b[4 * i] = i + rep;
	perf_end(r);
	perf_rate(r, 4.0f * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	for (i = 0; i < n; i++) bad += b[4 * i] != i + 3;
	perf_check(r, bad == 0, "%u of %u texels wrong", bad, n);
}

static void __attribute__((noipa)) copy_words(uint32_t *d, const uint32_t *s, uint32_t words)
{
	uint32_t i;

	for (i = 0; i < words; i += 4) {
		uint32_t a = s[i], b = s[i + 1], c = s[i + 2], e = s[i + 3];

		d[i] = a, d[i + 1] = b, d[i + 2] = c, d[i + 3] = e;
	}
}

static void t_copy_loop(struct perf_run *r)
{
	uint32_t *a = words_a(), *d = buf(3, 256 * 1024), rep, n = 65536;

	NEED(a && d);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) copy_words(d, a + rep * n, n);
	perf_end(r);
	perf_rate(r, 4.0f * 4 * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	perf_check(r, memcmp(d, a + 3 * n, 4 * n) == 0, "256 KB as the source");
}

static void t_memcpy(struct perf_run *r)
{
	uint32_t *a = words_a(), *d = buf(3, 1u << 20), rep;

	NEED(a && d);
	memset(d, 0, 1u << 20);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) memcpy(d, a, 1u << 20);
	perf_end(r);
	perf_rate(r, 4.0f * (1u << 20), 1e6f);
	perf_check(r, memcmp(d, a, 1u << 20) == 0, "1 MB as the source");
}

static void t_memset(struct perf_run *r)
{
	uint8_t *d = buf(3, 1u << 20);
	uint32_t rep, i, bad = 0;

	NEED(d);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) memset(d, 0x40 + (int)rep, 1u << 20);
	perf_end(r);
	perf_rate(r, 4.0f * (1u << 20), 1e6f);
	for (i = 0; i < (1u << 20); i += 4) bad += *(uint32_t *)(d + i) != 0x43434343u;
	perf_check(r, bad == 0, "%u words wrong", bad);
}

static void t_gather(struct perf_run *r)
{
	uint32_t *a = words_a(), *idx = buf(4, 65536 * 4), *out = buf(5, 65536 * 4), n = 65536, s = 11, i, rep, total = 0;

	NEED(a && idx && out);
	for (i = 0; i < n; i++) idx[i] = i;
	for (i = n - 1; i > 0; i--) {
		uint32_t j = lcg(&s) % (i + 1), t = idx[i];

		idx[i] = idx[j], idx[j] = t;
	}
	perf_begin(r);
	for (rep = 0; rep < 4; rep++)
		for (i = 0; i < n; i++) out[i] = a[idx[i] + rep * n];
	perf_end(r);
	perf_rate(r, 4.0f * n, 1e6f);
	perf_cpo(r, 4.0f * n);
	for (i = 0; i < n; i++) total += out[i];
	perf_check(r, total == sum_to(n) + 3 * n * n, "sum %08x", total);
}

/* ---------------------------------------------------------------- memory on the workers */

struct span { uint32_t *dst; const uint32_t *src; uint32_t words, value; };

static void copy_piece(void *ctx, uint32_t from, uint32_t to)
{
	struct span *s = ctx;

	copy_words(s->dst + from * 1024, s->src + from * 1024, (to - from) * 1024);
}

static void fill_piece(void *ctx, uint32_t from, uint32_t to)
{
	struct span *s = ctx;
	uint32_t *p = s->dst + from * 1024, *end = s->dst + to * 1024, v = s->value;

	for (; p < end; p += 4) p[0] = v, p[1] = v, p[2] = v, p[3] = v;
}

#define MC_BYTES (2u << 20)
static void t_mc_copy(struct perf_run *r)
{
	static struct span s __attribute__((aligned(16)));
	mcw_for_opts o = {0};
	uint32_t *src = buf(6, MC_BYTES), *dst = buf(7, MC_BYTES), *a = words_a(), rep;

	NEED(src && dst && a);
	memcpy(src, a, 1u << 20), memcpy((uint8_t *)src + (1u << 20), a, 1u << 20);
	memset(dst, 0, MC_BYTES);
	s.dst = dst, s.src = src, s.words = MC_BYTES / 4;
	o.grain = 1, o.first_idle = 1;
	perf_begin(r);
	for (rep = 0; rep < 2; rep++) mcw_for_opt(MC_BYTES / 4096, copy_piece, &s, &o);
	perf_end(r);
	perf_rate(r, 2.0f * MC_BYTES, 1e6f);
	perf_check(r, memcmp(dst, src, MC_BYTES) == 0, "2 MB as the source, in 4 KB pieces");
}

static void t_mc_fill(struct perf_run *r)
{
	static struct span s __attribute__((aligned(16)));
	mcw_for_opts o = {0};
	uint32_t *dst = buf(7, MC_BYTES), rep, i, bad = 0;

	NEED(dst);
	s.dst = dst;
	o.grain = 1, o.first_idle = 1;
	perf_begin(r);
	for (rep = 0; rep < 2; rep++) {
		s.value = 0x5a5a0000u + rep;
		mcw_for_opt(MC_BYTES / 4096, fill_piece, &s, &o);
	}
	perf_end(r);
	perf_rate(r, 2.0f * MC_BYTES, 1e6f);
	for (i = 0; i < MC_BYTES / 4; i++) bad += dst[i] != 0x5a5a0001u;
	perf_check(r, bad == 0, "%u of %u words wrong", bad, MC_BYTES / 4);
}

/* ---------------------------------------------------------------- the workers */

static uint32_t mix(uint32_t i)
{
	uint32_t x = i * 0x9e3779b1u, k;

	for (k = 0; k < 64; k++) {
		x ^= x >> 15;
		x *= 0x2c1b3c6du;
		x += k;
	}
	return x;
}

struct mixes { uint32_t *out; uint32_t pad[3]; };

static void mix_piece(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t *out = ((struct mixes *)ctx)->out, i;

	for (i = from; i < to; i++) out[i] = mix(i);
}

/* items of 64 rounds of a mix, 2,048 a core: on `n` workers (0: the first core alone) */
static void scale_test(struct perf_run *r, int n)
{
	static struct mixes m __attribute__((aligned(16)));
	mcw_for_opts o = {0};
	uint32_t items = 2048u * (uint32_t)(n ? n : 1), i, bad = 0, step;

	if (n > perf_workers) {
		r->ok = -1;
		snprintf(r->note, sizeof r->note, "%d workers here", perf_workers);
		return;
	}
	NEED(m.out = buf(8, items * 4));
	memset(m.out, 0, items * 4);
	o.workers = n, o.first_idle = n > 0, o.out_bytes = 4, o.grain = 2048;
	perf_begin(r);
	if (n) mcw_for_opt(items, mix_piece, &m, &o);
	else mix_piece(&m, 0, items);
	perf_end(r);
	perf_rate(r, 64.0f * items, 1e6f);
	if (!n) perf_cpo(r, 64.0f * items);
	step = items / 256;
	for (i = step / 2; i < items; i += step) bad += m.out[i] != mix(i);
	perf_check(r, bad == 0, "%u of 256 items wrong", bad);
}

static void t_mc_0(struct perf_run *r) { scale_test(r, 0); }
static void t_mc_1(struct perf_run *r) { scale_test(r, 1); }
static void t_mc_4(struct perf_run *r) { scale_test(r, 4); }
static void t_mc_16(struct perf_run *r) { scale_test(r, 16); }
static void t_mc_64(struct perf_run *r) { scale_test(r, 64); }
static void t_mc_255(struct perf_run *r) { scale_test(r, perf_workers < 255 && perf_workers > 64 ? perf_workers : 255); }

struct rounds { uint32_t *out, round, pad[2]; };

static void round_piece(void *ctx, uint32_t from, uint32_t to)
{
	struct rounds *rd = ctx;
	uint32_t i;

	for (i = from; i < to; i++) rd->out[i] = i + rd->round;
}

static void t_mc_for(struct perf_run *r)
{
	static struct rounds rd __attribute__((aligned(16)));
	mcw_for_opts o = {0};
	uint32_t n = 4096, rounds = 40, i, bad = 0;

	NEED(rd.out = buf(8, n * 4));
	o.out_bytes = 4;
	perf_begin(r);
	for (rd.round = 0; rd.round < rounds; rd.round++) mcw_for_opt(n, round_piece, &rd, &o);
	perf_end(r);
	perf_rate(r, (float)rounds, 1.0f);
	for (i = 0; i < n; i++) bad += rd.out[i] != i + rounds - 1;
	perf_check(r, bad == 0, "%u of %u words of the last round wrong", bad, n);
}

static uint32_t call_fn(uint32_t a, uint32_t b)
{
	return a * 2654435761u + b;
}

static void t_mc_call(struct perf_run *r)
{
	uint32_t calls = 30, i, bad = 0;

	mcw_for_end();
	perf_begin(r);
	for (i = 0; i < calls; i++) {
		mcw_handle h = mcw_call(call_fn, i, 7);

		bad += mcw_join(h) != call_fn(i, 7);
	}
	perf_end(r);
	perf_rate(r, (float)calls, 1.0f);
	perf_check(r, bad == 0, "%u of %u answers wrong", bad, calls);
}

/* a word each way: the first core's ping, the worker's pong, each in its own texel */
static struct { volatile uint32_t v, pad[3]; } ping __attribute__((aligned(16))), pong __attribute__((aligned(16)));

static uint32_t pong_job(uint32_t times, uint32_t unused)
{
	uint32_t i;

	(void)unused;
	for (i = 1; i <= times; i++) {
		while (ping.v != i) mc_end_pass();
		pong.v = i;
	}
	return times;
}

static void t_handoff(struct perf_run *r)
{
	uint32_t times = 60, i;
	int k = 1;

	mcw_for_end();
	ping.v = 0, pong.v = 0;
	mc_publish();
	mcw_post(k, pong_job, times, 0);
	perf_begin(r);
	for (i = 1; i <= times; i++) {
		ping.v = i;
		while (pong.v != i) mcw_rest();
	}
	perf_end(r);
	mcw_wait(k);
	perf_rate(r, (float)times, 1.0f);
	perf_check(r, mcw_result(k) == times && pong.v == times, "%u round trips, the last %u", mcw_result(k), pong.v);
}

static void add_piece(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t i;

	(void)ctx;
	for (i = from; i < to; i++) mcw_add(0, 1);
}

static void t_counters(struct perf_run *r)
{
	mcw_for_opts o = {0};
	uint32_t n = 2000u * (uint32_t)perf_workers, got;

	mcw_counters();
	mcw_set(0, 0);
	o.first_idle = 1, o.grain = 2000;
	perf_begin(r);
	mcw_for_opt(n, add_piece, 0, &o);
	perf_end(r);
	perf_rate(r, (float)n, 1e6f);
	mcw_counters_settle();
	got = mcw_get(0);
	perf_check(r, got == n, "total %u of %u adds", got, n);
}

/* 128 KB stored twice by one worker */
static uint32_t store_job(uint32_t at, uint32_t words)
{
	uint32_t *p = (uint32_t *)at, rep, i;

	for (rep = 0; rep < 2; rep++)
		for (i = 0; i < words; i += 4) p[i] = i + rep, p[i + 1] = rep, p[i + 2] = i, p[i + 3] = ~i;
	return words;
}

static void hybrid_test(struct perf_run *r, int fast)
{
	uint32_t words = 32768, *b = buf(9, words * 4), i, bad = 0;
	int k, found = 0;

	NEED(b);
	mcw_for_end();
	for (k = 1; k <= perf_workers && !found; k++)
		if (mcw_bound(k) && !mcw_fast(k) == !fast) found = k;
	if (!found) {
		r->ok = -1;
		snprintf(r->note, sizeof r->note, "no %s core here", fast ? "big" : "small");
		return;
	}
	memset(b, 0, words * 4);
	mcw_touch(b, words * 4);
	perf_begin(r);
	mcw_post(found, store_job, (uint32_t)b, words);
	mcw_wait(found);
	perf_end(r);
	perf_rate(r, 2.0f * 4 * words, 1e6f);
	for (i = 0; i < words; i += 4) bad += b[i] != i + 1 || b[i + 1] != 1 || b[i + 2] != i || b[i + 3] != ~i;
	perf_check(r, bad == 0, "worker %d (core %d): %u texels wrong", found, mcw_core(found), bad);
}

static void t_hybrid_big(struct perf_run *r) { hybrid_test(r, 1); }
static void t_hybrid_small(struct perf_run *r) { hybrid_test(r, 0); }

/* ---------------------------------------------------------------- lanes */

struct lanejob { uint32_t *out; uint32_t pad[3]; };

/* an index's texel: four words of 40 rounds */
static void lane_fn(uint32_t index, void *ctx)
{
	uint32_t *o = ((struct lanejob *)ctx)->out + 4 * index, x = index * 0x9e3779b1u + 1, k;

	for (k = 0; k < 40; k++) x = (x ^ (x >> 13)) * 0x5bd1e995u + k;
	o[0] = x, o[1] = x ^ index, o[2] = x * 3, o[3] = index;
}

static int lane_wrong(const uint32_t *out, uint32_t n)
{
	static struct lanejob j __attribute__((aligned(16)));
	static uint32_t one[4] __attribute__((aligned(16)));
	uint32_t i, step = n / 256 ? n / 256 : 1;
	int bad = 0;

	for (i = step / 2; i < n; i += step) {
		j.out = one - 4 * i;
		lane_fn(i, &j);
		bad += memcmp(one, out + 4 * i, 16) != 0;
	}
	return bad;
}

static void lanes_piece(void *ctx, uint32_t from, uint32_t to)
{
	struct lanejob *j = ctx;

	mcw_lanes(lane_fn, j, from, to - from, j->out + 4 * from, 1);
}

static void t_lanes(struct perf_run *r)
{
	static struct lanejob j __attribute__((aligned(16)));
	mcw_for_opts o = {0};
	uint32_t a_worker = 256, n = a_worker * (uint32_t)perf_workers;
	int bad;

	NEED(j.out = buf(10, n * 16));
	memset(j.out, 0, n * 16);
	o.first_idle = 1, o.grain = a_worker;
	perf_begin(r);
	mcw_for_opt(n, lanes_piece, &j, &o);
	perf_end(r);
	perf_rate(r, (float)n, 1e6f);
	bad = lane_wrong(j.out, n);
	perf_check(r, bad == 0, "%d of 256 indices wrong (%d lanes a worker)", bad, mcw_lanes_here());
}

static void t_field(struct perf_run *r)
{
	static struct lanejob j __attribute__((aligned(16)));
	uint32_t n = 3968 * 4;
	int bad;

	NEED(j.out = buf(10, n * 16));
	memset(j.out, 0, n * 16);
	mcw_for_end();
	perf_begin(r);
	mcw_field_begin(lane_fn, &j, 0, n, j.out, 1);
	mcw_field_wait();
	perf_end(r);
	perf_rate(r, (float)n, 1e6f);
	bad = lane_wrong(j.out, n);
	perf_check(r, bad == 0, "%d of 256 indices wrong (%d lanes in the field)", bad, mcw_field_lanes());
}

static uint32_t shade_fn(int x, int y, void *ctx)
{
	uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u, k;

	(void)ctx;
	for (k = 0; k < 8; k++) h = (h ^ (h >> 11)) * 0x27d4eb2du;
	return h & 0xffffffu;
}

static void t_shade(struct perf_run *r)
{
	uint32_t *px = buf(10, 256 * 256 * 4), i, bad = 0;

	NEED(px);
	memset(px, 0, 256 * 256 * 4);
	mcw_for_end();
	perf_begin(r);
	mcw_shade(px, 256, 256, shade_fn, 0);
	perf_end(r);
	perf_rate(r, 256.0f * 256, 1e6f);
	for (i = 37; i < 65536; i += 257) bad += px[i] != shade_fn((int)(i & 255), (int)(i >> 8), 0);
	perf_check(r, bad == 0, "%u of 255 pixels wrong", bad);
}

/* ---------------------------------------------------------------- numbers: BLAS */

/* small whole numbers, so every sum below is exact in floats */
static float small(uint32_t i, uint32_t m)
{
	return (float)(int)((i * 2654435761u >> 16) % m) - (float)(m / 2);
}

static float __attribute__((noipa)) sdot(const float *x, const float *y, uint32_t n)
{
	float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
	uint32_t i;

	for (i = 0; i < n; i += 4) s0 += x[i] * y[i], s1 += x[i + 1] * y[i + 1], s2 += x[i + 2] * y[i + 2], s3 += x[i + 3] * y[i + 3];
	return (s0 + s1) + (s2 + s3);
}

static void t_dot(struct perf_run *r)
{
	uint32_t n = 16384, i, rep;
	float *x = buf(1, n * 4), *y = buf(2, n * 4), got = 0;
	int32_t want = 0;

	NEED(x && y);
	for (i = 0; i < n; i++) x[i] = small(i, 9), y[i] = small(i + 77, 7), want += (int32_t)x[i] * (int32_t)y[i];
	perf_begin(r);
	for (rep = 0; rep < 8; rep++) got += sdot(x, y, n);
	perf_end(r);
	perf_rate(r, 2.0f * n * 8, 1e6f);
	perf_cpo(r, 2.0f * n * 8);
	perf_check(r, got == 8.0f * (float)want, "%.0f, whole numbers %d", (double)got, 8 * want);
}

static void __attribute__((noipa)) saxpy(float a, const float *x, float *y, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i += 4) y[i] += a * x[i], y[i + 1] += a * x[i + 1], y[i + 2] += a * x[i + 2], y[i + 3] += a * x[i + 3];
}

static void t_axpy(struct perf_run *r)
{
	uint32_t n = 16384, i, rep, bad = 0;
	float *x = buf(1, n * 4), *y = buf(2, n * 4);

	NEED(x && y);
	for (i = 0; i < n; i++) x[i] = small(i, 9), y[i] = small(i + 5, 11);
	perf_begin(r);
	for (rep = 0; rep < 8; rep++) saxpy(2.0f * fone, x, y, n);
	perf_end(r);
	perf_rate(r, 2.0f * n * 8, 1e6f);
	perf_cpo(r, 2.0f * n * 8);
	for (i = 0; i < n; i++) bad += y[i] != small(i + 5, 11) + 16.0f * x[i];
	perf_check(r, bad == 0, "%u of %u wrong", bad, n);
}

static void __attribute__((noipa)) sgemv(const float *a, const float *x, float *y, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) y[i] = sdot(a + i * n, x, n);
}

static void t_gemv(struct perf_run *r)
{
	uint32_t n = 128, i, j, rep, bad = 0;
	float *a = buf(1, n * n * 4), *x = buf(2, n * 4), *y = buf(3, n * 4);

	NEED(a && x && y);
	for (i = 0; i < n * n; i++) a[i] = small(i, 9);
	for (i = 0; i < n; i++) x[i] = small(i + 3, 7);
	perf_begin(r);
	for (rep = 0; rep < 8; rep++) sgemv(a, x, y, n);
	perf_end(r);
	perf_rate(r, 2.0f * n * n * 8, 1e6f);
	perf_cpo(r, 2.0f * n * n * 8);
	for (i = 0; i < n; i++) {
		int32_t s = 0;

		for (j = 0; j < n; j++) s += (int32_t)a[i * n + j] * (int32_t)x[j];
		bad += y[i] != (float)s;
	}
	perf_check(r, bad == 0, "%u of %u rows wrong", bad, n);
}

/* rows from..to of c = a b, n x n: i, k, j */
static void gemm_rows(const float *a, const float *b, float *c, uint32_t n, uint32_t from, uint32_t to)
{
	uint32_t i, j, k;

	for (i = from; i < to; i++) {
		float *ci = c + i * n;

		for (j = 0; j < n; j++) ci[j] = 0;
		for (k = 0; k < n; k++) {
			float aik = a[i * n + k];
			const float *bk = b + k * n;

			for (j = 0; j < n; j += 4) ci[j] += aik * bk[j], ci[j + 1] += aik * bk[j + 1], ci[j + 2] += aik * bk[j + 2], ci[j + 3] += aik * bk[j + 3];
		}
	}
}

static uint32_t gemm_wrong(const float *a, const float *b, const float *c, uint32_t n)
{
	uint32_t s, bad = 0, k;

	for (s = 0; s < 64; s++) {
		uint32_t i = (s * 37 + 5) % n, j = (s * 101 + 11) % n;
		int32_t t = 0;

		for (k = 0; k < n; k++) t += (int32_t)a[i * n + k] * (int32_t)b[k * n + j];
		bad += c[i * n + j] != (float)t;
	}
	return bad;
}

static void t_gemm(struct perf_run *r)
{
	uint32_t n = 64, i, bad;
	float *a = buf(1, n * n * 4), *b = buf(2, n * n * 4), *c = buf(3, n * n * 4);

	NEED(a && b && c);
	for (i = 0; i < n * n; i++) a[i] = small(i, 9), b[i] = small(i + 1, 7);
	perf_begin(r);
	gemm_rows(a, b, c, n, 0, n);
	perf_end(r);
	perf_rate(r, 2.0f * n * n * n, 1e6f);
	perf_cpo(r, 2.0f * n * n * n);
	bad = gemm_wrong(a, b, c, n);
	perf_check(r, bad == 0, "%u of 64 entries wrong", bad);
}

struct gemm_job { const float *a, *b; float *c; uint32_t n; };

static void gemm_piece(void *ctx, uint32_t from, uint32_t to)
{
	struct gemm_job *g = ctx;

	gemm_rows(g->a, g->b, g->c, g->n, from, to);
}

static void t_gemm_mc(struct perf_run *r)
{
	static struct gemm_job g __attribute__((aligned(16)));
	mcw_for_opts o = {0};
	uint32_t n = 256, i, bad;
	float *a = buf(1, n * n * 4), *b = buf(2, n * n * 4), *c = buf(3, n * n * 4);

	NEED(a && b && c);
	for (i = 0; i < n * n; i++) a[i] = small(i, 9), b[i] = small(i + 1, 7);
	memset(c, 0, n * n * 4);
	g.a = a, g.b = b, g.c = c, g.n = n;
	o.grain = 1, o.first_idle = 1;
	perf_begin(r);
	mcw_for_opt(n, gemm_piece, &g, &o);
	perf_end(r);
	perf_rate(r, 2.0f * n * n * n, 1e6f);
	bad = gemm_wrong(a, b, c, n);
	perf_check(r, bad == 0, "%u of 64 entries wrong, rows on the workers", bad);
}

/* ---------------------------------------------------------------- numbers: LAPACK */

static float frand(uint32_t *s)
{
	return (float)(lcg(s) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

/* the largest |x - want| over the largest |want| */
static float rel_error(const float *x, const float *want, uint32_t n)
{
	float e = 0, m = 0;
	uint32_t i;

	for (i = 0; i < n; i++) {
		float d = fabsf(x[i] - want[i]);

		e = d > e ? d : e;
		m = fabsf(want[i]) > m ? fabsf(want[i]) : m;
	}
	return e / m;
}

/* b = a x, a m x n by rows */
static void matvec(const float *a, const float *x, float *b, uint32_t m, uint32_t n)
{
	uint32_t i, j;

	for (i = 0; i < m; i++) {
		float s = 0;

		for (j = 0; j < n; j++) s += a[i * n + j] * x[j];
		b[i] = s;
	}
}

/* LU with partial pivoting in place, then a solve of b in place */
static int lu_solve(float *a, float *b, uint32_t n)
{
	uint32_t i, j, k, p;

	for (k = 0; k < n; k++) {
		float best = fabsf(a[k * n + k]), t;

		for (p = k, i = k + 1; i < n; i++)
			if (fabsf(a[i * n + k]) > best) best = fabsf(a[i * n + k]), p = i;
		if (best == 0) return 0;
		if (p != k) {
			for (j = 0; j < n; j++) t = a[k * n + j], a[k * n + j] = a[p * n + j], a[p * n + j] = t;
			t = b[k], b[k] = b[p], b[p] = t;
		}
		for (i = k + 1; i < n; i++) {
			float l = a[i * n + k] / a[k * n + k];

			a[i * n + k] = l;
			for (j = k + 1; j < n; j++) a[i * n + j] -= l * a[k * n + j];
			b[i] -= l * b[k];
		}
	}
	for (i = n; i-- > 0;) {
		float s = b[i];

		for (j = i + 1; j < n; j++) s -= a[i * n + j] * b[j];
		b[i] = s / a[i * n + i];
	}
	return 1;
}

static void t_lu(struct perf_run *r)
{
	uint32_t n = 64, s = 3, i, rep;
	float *a0 = buf(1, n * n * 4), *a = buf(2, n * n * 4), *x = buf(3, n * 4), *b0 = buf(4, n * 4), *b = buf(5, n * 4), e;
	int ok = 1;

	NEED(a0 && a && x && b0 && b);
	for (i = 0; i < n * n; i++) a0[i] = frand(&s);
	for (i = 0; i < n; i++) x[i] = (float)(int)(i % 13) - 6.0f;
	matvec(a0, x, b0, n, n);
	perf_begin(r);
	for (rep = 0; rep < 2; rep++) {
		memcpy(a, a0, n * n * 4), memcpy(b, b0, n * 4);
		ok &= lu_solve(a, b, n);
	}
	perf_end(r);
	perf_rate(r, 2.0f * (2.0f / 3.0f * n * n * n + 2.0f * n * n), 1e6f);
	perf_cpo(r, 2.0f * (2.0f / 3.0f * n * n * n + 2.0f * n * n));
	e = rel_error(b, x, n);
	perf_check(r, ok && e < 1e-3f, "x's relative error %.2g (64 x 64, random)", (double)e);
}

/* a = l l^T in place (l in the lower triangle), then a solve of b in place */
static int chol_solve(float *a, float *b, uint32_t n)
{
	uint32_t i, j, k;

	for (j = 0; j < n; j++) {
		float d = a[j * n + j];

		for (k = 0; k < j; k++) d -= a[j * n + k] * a[j * n + k];
		if (d <= 0) return 0;
		d = sqrtf(d);
		a[j * n + j] = d;
		for (i = j + 1; i < n; i++) {
			float s = a[i * n + j];

			for (k = 0; k < j; k++) s -= a[i * n + k] * a[j * n + k];
			a[i * n + j] = s / d;
		}
	}
	for (i = 0; i < n; i++) {
		float s = b[i];

		for (k = 0; k < i; k++) s -= a[i * n + k] * b[k];
		b[i] = s / a[i * n + i];
	}
	for (i = n; i-- > 0;) {
		float s = b[i];

		for (k = i + 1; k < n; k++) s -= a[k * n + i] * b[k];
		b[i] = s / a[i * n + i];
	}
	return 1;
}

static void t_cholesky(struct perf_run *r)
{
	uint32_t n = 64, s = 4, i, j, k, rep;
	float *m = buf(1, n * n * 4), *a0 = buf(6, n * n * 4), *a = buf(2, n * n * 4), *x = buf(3, n * 4), *b0 = buf(4, n * 4),
	      *b = buf(5, n * 4), e;
	int ok = 1;

	NEED(m && a0 && a && x && b0 && b);
	for (i = 0; i < n * n; i++) m[i] = frand(&s);
	for (i = 0; i < n; i++)		/* m^T m + n: symmetric, positive definite */
		for (j = 0; j < n; j++) {
			float t = i == j ? (float)n : 0;

			for (k = 0; k < n; k++) t += m[k * n + i] * m[k * n + j];
			a0[i * n + j] = t;
		}
	for (i = 0; i < n; i++) x[i] = (float)(int)(i % 11) - 5.0f;
	matvec(a0, x, b0, n, n);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) {
		memcpy(a, a0, n * n * 4), memcpy(b, b0, n * 4);
		ok &= chol_solve(a, b, n);
	}
	perf_end(r);
	perf_rate(r, 4.0f * (1.0f / 3.0f * n * n * n + 2.0f * n * n), 1e6f);
	perf_cpo(r, 4.0f * (1.0f / 3.0f * n * n * n + 2.0f * n * n));
	e = rel_error(b, x, n);
	perf_check(r, ok && e < 1e-4f, "x's relative error %.2g", (double)e);
}

static void t_trsv(struct perf_run *r)
{
	uint32_t n = 256, s = 6, i, j, rep;
	float *l = buf(1, n * n * 4), *x = buf(3, n * 4), *b0 = buf(4, n * 4), *b = buf(5, n * 4), e;

	NEED(l && x && b0 && b);
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++) l[i * n + j] = j < i ? frand(&s) * 0.1f : j == i ? 1.0f + 0.5f * (frand(&s) + 1.0f) : 0;
	for (i = 0; i < n; i++) x[i] = (float)(int)(i % 9) - 4.0f;
	matvec(l, x, b0, n, n);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) {
		memcpy(b, b0, n * 4);
		for (i = 0; i < n; i++) {
			float t = b[i];
			const float *li = l + i * n;

			for (j = 0; j < i; j++) t -= li[j] * b[j];
			b[i] = t / li[i];
		}
	}
	perf_end(r);
	perf_rate(r, 4.0f * n * n, 1e6f);
	perf_cpo(r, 4.0f * n * n);
	e = rel_error(b, x, n);
	perf_check(r, e < 1e-4f, "x's relative error %.2g (256, lower)", (double)e);
}

/* least squares by Householder QR: a m x n (m >= n) and b in place; x is b's first n */
static void qr_solve(float *a, float *b, uint32_t m, uint32_t n)
{
	uint32_t i, j, k;

	for (k = 0; k < n; k++) {
		float norm = 0, alpha, vtv, *v;

		for (i = k; i < m; i++) norm += a[i * n + k] * a[i * n + k];
		norm = sqrtf(norm);
		alpha = a[k * n + k] > 0 ? -norm : norm;
		/* v = x - alpha e1, kept in column k below the diagonal (its first word apart) */
		a[k * n + k] -= alpha;
		vtv = 0;
		for (i = k; i < m; i++) vtv += a[i * n + k] * a[i * n + k];
		v = a + k;	/* v_i = a[i * n + k] */
		if (vtv > 0) {
			for (j = k + 1; j < n; j++) {
				float d = 0;

				for (i = k; i < m; i++) d += v[i * n] * a[i * n + j];
				d = 2 * d / vtv;
				for (i = k; i < m; i++) a[i * n + j] -= d * v[i * n];
			}
			{
				float d = 0;

				for (i = k; i < m; i++) d += v[i * n] * b[i];
				d = 2 * d / vtv;
				for (i = k; i < m; i++) b[i] -= d * v[i * n];
			}
		}
		a[k * n + k] = alpha;	/* r's diagonal */
	}
	for (i = n; i-- > 0;) {
		float s = b[i];

		for (j = i + 1; j < n; j++) s -= a[i * n + j] * b[j];
		b[i] = s / a[i * n + i];
	}
}

static void t_qr(struct perf_run *r)
{
	uint32_t m = 64, n = 48, s = 8, i, rep;
	float *a0 = buf(1, m * n * 4), *a = buf(2, m * n * 4), *x = buf(3, n * 4), *b0 = buf(4, m * 4), *b = buf(5, m * 4), e;

	NEED(a0 && a && x && b0 && b);
	for (i = 0; i < m * n; i++) a0[i] = frand(&s);
	for (i = 0; i < n; i++) x[i] = (float)(int)(i % 7) - 3.0f;
	matvec(a0, x, b0, m, n);	/* b in a's range: the least squares answer is x */
	perf_begin(r);
	for (rep = 0; rep < 2; rep++) {
		memcpy(a, a0, m * n * 4), memcpy(b, b0, m * 4);
		qr_solve(a, b, m, n);
	}
	perf_end(r);
	perf_rate(r, 2.0f * (2.0f * m * n * n - 2.0f / 3.0f * n * n * n), 1e6f);
	perf_cpo(r, 2.0f * (2.0f * m * n * n - 2.0f / 3.0f * n * n * n));
	e = rel_error(b, x, n);
	perf_check(r, e < 1e-3f, "x's relative error %.2g (64 x 48 least squares)", (double)e);
}

/* ---------------------------------------------------------------- numbers: FFT, sort, hash */

#define FFT_N 1024
static float tw_re[FFT_N / 2], tw_im[FFT_N / 2];

/* in place, radix 2; `inverse` conjugates the turns (and does not divide) */
static void fft(float *re, float *im, int inverse)
{
	uint32_t n = FFT_N, i, j, k, len;

	for (i = 1, j = 0; i < n; i++) {
		uint32_t bit = n >> 1;

		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i];

			re[i] = re[j], re[j] = t, t = im[i], im[i] = im[j], im[j] = t;
		}
	}
	for (len = 2; len <= n; len <<= 1) {
		uint32_t half = len >> 1, step = n / len;

		for (i = 0; i < n; i += len)
			for (k = 0; k < half; k++) {
				float wr = tw_re[k * step], wi = inverse ? -tw_im[k * step] : tw_im[k * step];
				float *ar = re + i + k, *ai = im + i + k, *br = ar + half, *bi = ai + half;
				float tr = *br * wr - *bi * wi, ti = *br * wi + *bi * wr;

				*br = *ar - tr, *bi = *ai - ti;
				*ar += tr, *ai += ti;
			}
	}
}

static void t_fft(struct perf_run *r)
{
	float *re = buf(1, FFT_N * 4), *im = buf(2, FFT_N * 4), *x0 = buf(3, FFT_N * 4), e = 0, m = 0, peak;
	uint32_t i, rep, s = 9;

	NEED(re && im && x0);
	for (i = 0; i < FFT_N / 2; i++) {
		float a = -2.0f * 3.14159265f * (float)i / FFT_N;

		tw_re[i] = cosf(a), tw_im[i] = sinf(a);
	}
	/* a tone in bin 37 and noise */
	for (i = 0; i < FFT_N; i++) x0[i] = cosf(2.0f * 3.14159265f * 37.0f * (float)i / FFT_N) + 0.1f * frand(&s);
	memcpy(re, x0, FFT_N * 4), memset(im, 0, FFT_N * 4);
	fft(re, im, 0);
	peak = sqrtf(re[37] * re[37] + im[37] * im[37]);
	memcpy(re, x0, FFT_N * 4), memset(im, 0, FFT_N * 4);
	perf_begin(r);
	for (rep = 0; rep < 4; rep++) {
		fft(re, im, 0);
		fft(re, im, 1);
		for (i = 0; i < FFT_N; i++) re[i] *= 1.0f / FFT_N, im[i] *= 1.0f / FFT_N;
	}
	perf_end(r);
	perf_rate(r, 8.0f * 5.0f * FFT_N * 10, 1e6f);
	perf_cpo(r, 8.0f * 5.0f * FFT_N * 10);
	for (i = 0; i < FFT_N; i++) {
		float d = fabsf(re[i] - x0[i]) + fabsf(im[i]);

		e = d > e ? d : e, m = fabsf(x0[i]) > m ? fabsf(x0[i]) : m;
	}
	perf_check(r, e / m < 1e-4f && near(peak, FFT_N / 2, 0.02f), "round trip error %.2g, bin 37 %.1f of %d", (double)(e / m), (double)peak,
		   FFT_N / 2);
}

static void __attribute__((noipa)) quicksort(uint32_t *a, int n)
{
	while (n > 16) {
		uint32_t p = a[n / 2], t;
		int i = -1, j = n;

		/* the middle of three as the pivot; Hoare's partition */
		if ((a[0] < p) != (a[0] < a[n - 1])) p = a[0];
		else if ((a[n - 1] < p) != (a[n - 1] < a[0])) p = a[n - 1];
		for (;;) {
			do i++; while (a[i] < p);
			do j--; while (a[j] > p);
			if (i >= j) break;
			t = a[i], a[i] = a[j], a[j] = t;
		}
		/* the smaller side by a call, the larger by the loop */
		if (j + 1 < n - j - 1) quicksort(a, j + 1), a += j + 1, n -= j + 1;
		else quicksort(a + j + 1, n - j - 1), n = j + 1;
	}
	for (int i = 1; i < n; i++) {
		uint32_t v = a[i];
		int j = i - 1;

		while (j >= 0 && a[j] > v) a[j + 1] = a[j], j--;
		a[j + 1] = v;
	}
}

static void t_sort(struct perf_run *r)
{
	uint32_t n = 8192, *a = buf(1, n * 4), s = 12, i, sum = 0, x = 0, sum2 = 0, x2 = 0, order = 0;

	NEED(a);
	for (i = 0; i < n; i++) a[i] = lcg(&s), sum += a[i], x ^= a[i] * 2654435761u;
	perf_begin(r);
	quicksort(a, (int)n);
	perf_end(r);
	perf_rate(r, (float)n, 1e6f);
	perf_cpo(r, (float)n);
	for (i = 0; i < n; i++) sum2 += a[i], x2 ^= a[i] * 2654435761u, order += i && a[i - 1] > a[i];
	perf_check(r, order == 0 && sum == sum2 && x == x2, "%u out of order, the same %u words", order, n);
}

static void t_hash_table(struct perf_run *r)
{
	uint32_t slots = 16384, n = 8192, *t = buf(1, slots * 4), i, found = 0, wrong = 0, probes = 0;

	NEED(t);
	memset(t, 0, slots * 4);
	perf_begin(r);
	for (i = 1; i <= n; i++) {
		uint32_t key = i * 7919u, h = (key * 2654435761u) >> 18;

		while (t[h]) h = (h + 1) & (slots - 1), probes++;
		t[h] = key;
	}
	for (i = 1; i <= 2 * n; i++) {
		uint32_t key = i * 7919u, h = (key * 2654435761u) >> 18;

		while (t[h] && t[h] != key) h = (h + 1) & (slots - 1);
		if (t[h] == key) found++, wrong += i > n;
	}
	perf_end(r);
	perf_rate(r, 3.0f * n, 1e6f);
	perf_cpo(r, 3.0f * n);
	sink = probes;
	perf_check(r, found == n && wrong == 0, "%u of %u keys found, %u that were not put", found, n, wrong);
}

static void t_crc32(struct perf_run *r)
{
	uint32_t bytes = 128 * 1024, *a = words_a(), whole, first, second, nine;

	NEED(a);
	nine = media_crc32(0, "123456789", 9);
	perf_begin(r);
	whole = media_crc32(0, a, bytes);
	perf_end(r);
	perf_rate(r, (float)bytes, 1e6f);
	perf_cpo(r, (float)bytes);
	first = media_crc32(0, a, bytes / 2);
	second = media_crc32(0, (uint8_t *)a + bytes / 2, bytes / 2);
	perf_check(r, nine == 0xcbf43926u && whole == media_crc32_join(first, second, bytes / 2), "123456789: %08x, halves joined %s", nine,
		   whole == media_crc32_join(first, second, bytes / 2) ? "the same" : "not the same");
}

/* ---------------------------------------------------------------- libmedia */

#define PIC_W 256
#define PIC_H 192
static struct media_image pic;	/* the source picture, 3 bytes a pixel */
static uint8_t *png, *jpeg;
static uint32_t png_size, jpeg_size;

static int picture_made(void)
{
	int x, y;

	if (pic.pixels) return 1;
	pic.width = PIC_W, pic.height = PIC_H, pic.channels = 3, pic.stride = PIC_W * 3;
	if (!(pic.pixels = buf(11, PIC_W * PIC_H * 3))) return 0;
	/* rings and gradients with a little noise: a photograph's kind of content, and compressible */
	for (y = 0; y < PIC_H; y++)
		for (x = 0; x < PIC_W; x++) {
			uint8_t *p = pic.pixels + y * pic.stride + x * 3;
			int dx = x - 100, dy = y - 80, d = (dx * dx + dy * dy) >> 6, n = (int)((uint32_t)(x * 7919 + y * 104729) * 2654435761u >> 29);

			p[0] = (uint8_t)(x + n);
			p[1] = (uint8_t)(y + (d & 63));
			p[2] = (uint8_t)(128 + ((d * 5) & 127) - n);
		}
	return 1;
}

static int png_made(void)
{
	if (png) return 1;
	if (!picture_made()) return 0;
	png = media_png_encode(&pic, 6, &png_size, perf_workers);
	return png != 0;
}

/* the picture's PNG's zlib stream (its IDATs, one after another) */
static uint8_t *zlib_of(uint32_t *bytes)
{
	uint8_t *z = buf(4, png_size), *p = png + 8, *end = png + png_size;
	uint32_t n = 0;

	if (!z) return 0;
	while (p + 12 <= end) {
		uint32_t len = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];

		if (!memcmp(p + 4, "IDAT", 4)) memcpy(z + n, p + 8, len), n += len;
		p += 12 + len;
	}
	*bytes = n;
	return z;
}

static uint32_t adler32(const uint8_t *p, uint32_t n)
{
	uint32_t a = 1, b = 0;

	while (n) {
		uint32_t k = n < 5552 ? n : 5552;

		n -= k;
		while (k--) a += *p++, b += a;
		a %= 65521, b %= 65521;
	}
	return b << 16 | a;
}

static void t_inflate(struct perf_run *r)
{
	uint32_t zbytes, made = 0, used = 0, want = (uint32_t)PIC_H * (1 + PIC_W * 3), stored, got;
	uint8_t *z, *out, *room;
	const char *why;

	NEED(png_made() && (z = zlib_of(&zbytes)));
	NEED((out = buf(5, want + 64)) && (room = buf(6, media_inflate_room())));
	perf_begin(r);
	why = media_inflate_one(z + 2, zbytes - 6, out, want + 64, &made, &used, 0, room);
	perf_end(r);
	perf_rate(r, (float)made, 1e6f);
	perf_cpo(r, (float)made);
	stored = (uint32_t)z[zbytes - 4] << 24 | (uint32_t)z[zbytes - 3] << 16 | (uint32_t)z[zbytes - 2] << 8 | z[zbytes - 1];
	got = adler32(out, made);
	perf_check(r, !why && made == want && got == stored, "%s%u bytes from %u, Adler-32 %08x of %08x", why ? why : "", made, zbytes, got, stored);
}

static void t_png_decode(struct perf_run *r)
{
	struct media_image out = {0};
	const char *why;
	int y, bad = 0;

	NEED(png_made());
	out.channels = 3;
	perf_begin(r);
	why = media_image_decode(png, png_size, &out, perf_workers);
	perf_end(r);
	perf_rate(r, (float)PIC_W * PIC_H, 1e6f);
	if (!why && out.width == PIC_W && out.height == PIC_H)
		for (y = 0; y < PIC_H; y++) bad += memcmp(out.pixels + y * out.stride, pic.pixels + y * pic.stride, PIC_W * 3) != 0;
	perf_check(r, !why && out.width == PIC_W && bad == 0, "%s%d of %d rows as encoded (%u bytes)", why ? why : "", PIC_H - bad, PIC_H, png_size);
	media_image_free(&out);
}

static void t_jpeg_encode(struct perf_run *r)
{
	NEED(picture_made());
	if (jpeg) media_free(jpeg, jpeg_size), jpeg = 0;
	perf_begin(r);
	jpeg = media_jpeg_encode(&pic, 90, &jpeg_size, perf_workers);
	perf_end(r);
	perf_rate(r, (float)PIC_W * PIC_H, 1e6f);
	perf_check(r, jpeg && jpeg_size > 1000 && jpeg[0] == 0xff && jpeg[1] == 0xd8, "%u bytes at quality 90", jpeg_size);
}

static void t_jpeg_decode(struct perf_run *r)
{
	struct media_image out = {0};
	const char *why;
	float se = 0, psnr = 0;
	int x, y;

	NEED(picture_made());
	if (!jpeg) jpeg = media_jpeg_encode(&pic, 90, &jpeg_size, perf_workers);
	NEED(jpeg);
	out.channels = 3;
	perf_begin(r);
	why = media_image_decode(jpeg, jpeg_size, &out, perf_workers);
	perf_end(r);
	perf_rate(r, (float)PIC_W * PIC_H, 1e6f);
	if (!why && out.width == PIC_W && out.height == PIC_H) {
		for (y = 0; y < PIC_H; y++) {
			float row = 0;

			for (x = 0; x < PIC_W * 3; x++) {
				float d = (float)out.pixels[y * out.stride + x] - (float)pic.pixels[y * pic.stride + x];

				row += d * d;
			}
			se += row;
		}
		psnr = 10.0f * log10f(255.0f * 255.0f / (se / (PIC_W * PIC_H * 3) + 1e-6f));
	}
	perf_check(r, !why && psnr > 30.0f, "%sPSNR %.1f dB against the source", why ? why : "", (double)psnr);
	media_image_free(&out);
}

/* ---------------------------------------------------------------- the list */

const char *const perf_groups[] = {"cpu", "memory", "cores", "lanes", "numbers", "media", "gpu"};
const int perf_group_count = 7;

#define W PERF_WORKERS
const struct perf_test perf_tests[] = {
	{"int-alu", "cpu", "Mop/s", t_int_alu, 0, "add, xor, shifts: a chain of 8 operations"},
	{"int-mul", "cpu", "Mop/s", t_int_mul, 0, "mul and add, a chain"},
	{"int-div", "cpu", "Mop/s", t_int_div, 0, "divu and remu"},
	{"branch", "cpu", "Mop/s", t_branch, 0, "branches on random bits"},
	{"fp-add", "cpu", "Mflop/s", t_fp_add, 0, "fadd.s, four chains"},
	{"fp-mul", "cpu", "Mflop/s", t_fp_mul, 0, "fmul.s, four chains"},
	{"fp-div", "cpu", "Mflop/s", t_fp_div, 0, "fdiv.s, four chains"},
	{"fp-sqrt", "cpu", "Mop/s", t_fp_sqrt, 0, "fsqrt.s and a sum"},
	{"load-4", "memory", "MB/s", t_load_4, 0, "loads of every word of 1 MB"},
	{"load-16", "memory", "MB/s", t_load_16, 0, "loads 16 bytes apart (a texel each)"},
	{"load-64", "memory", "MB/s", t_load_64, 0, "loads 64 bytes apart"},
	{"load-4k", "memory", "MB/s", t_load_4k, 0, "loads 4 KB apart"},
	{"lat-4k", "memory", "ns/load", t_lat_4k, 0, "a pointer chase in 4 KB"},
	{"lat-64k", "memory", "ns/load", t_lat_64k, 0, "a pointer chase in 64 KB"},
	{"lat-256k", "memory", "ns/load", t_lat_256k, 0, "a pointer chase in 256 KB"},
	{"store-seq", "memory", "MB/s", t_store_seq, 0, "stores of every word of 256 KB"},
	{"wcache-hot", "memory", "M/s", t_wcache_hot, 0, "stores over 1 KB: the write cache keeps it"},
	{"wcache-cold", "memory", "M/s", t_wcache_cold, 0, "a store a texel over 1 MB: the write cache fills"},
	{"copy-loop", "memory", "MB/s", t_copy_loop, 0, "a copy by words"},
	{"memcpy", "memory", "MB/s", t_memcpy, 0, "memcpy of 1 MB: the machine's copy"},
	{"memset", "memory", "MB/s", t_memset, 0, "memset of 1 MB: the machine's fill"},
	{"gather", "memory", "M/s", t_gather, 0, "loads at random indices"},
	{"mc-copy", "memory", "MB/s", t_mc_copy, W, "a copy of 2 MB on the workers"},
	{"mc-fill", "memory", "MB/s", t_mc_fill, W, "a fill of 2 MB on the workers"},
	{"mc-0", "cores", "Mop/s", t_mc_0, 0, "the mix on the first core alone"},
	{"mc-1", "cores", "Mop/s", t_mc_1, W, "the mix on 1 worker"},
	{"mc-4", "cores", "Mop/s", t_mc_4, W, "the mix on 4 workers"},
	{"mc-16", "cores", "Mop/s", t_mc_16, W, "the mix on 16 workers"},
	{"mc-64", "cores", "Mop/s", t_mc_64, W, "the mix on 64 workers"},
	{"mc-255", "cores", "Mop/s", t_mc_255, W, "the mix on every worker"},
	{"mc-for", "cores", "rounds/s", t_mc_for, W, "rounds of mcw_for: a team's turn"},
	{"mc-call", "cores", "calls/s", t_mc_call, W, "mcw_call and mcw_join, one at a time"},
	{"handoff", "cores", "trips/s", t_handoff, W, "a word to a worker and back"},
	{"counters", "cores", "M/s", t_counters, W, "mcw_add from every worker"},
	{"hybrid-big", "cores", "MB/s", t_hybrid_big, W, "stores on a big core (the first 16)"},
	{"hybrid-small", "cores", "MB/s", t_hybrid_small, W, "stores on a small core"},
	{"lanes", "lanes", "M/s", t_lanes, W, "indices on the workers' lanes"},
	{"field", "lanes", "M/s", t_field, W, "indices on the field"},
	{"shade", "lanes", "Mpx/s", t_shade, W, "mcw_shade of 256 x 256"},
	{"dot", "numbers", "Mflop/s", t_dot, 0, "sdot, 16,384"},
	{"axpy", "numbers", "Mflop/s", t_axpy, 0, "saxpy, 16,384"},
	{"gemv", "numbers", "Mflop/s", t_gemv, 0, "sgemv, 128 x 128"},
	{"gemm", "numbers", "Mflop/s", t_gemm, 0, "sgemm, 64 x 64"},
	{"gemm-mc", "numbers", "Mflop/s", t_gemm_mc, W, "sgemm, 256 x 256, a row a worker"},
	{"lu", "numbers", "Mflop/s", t_lu, 0, "LU with pivoting and a solve, 64"},
	{"cholesky", "numbers", "Mflop/s", t_cholesky, 0, "Cholesky and a solve, 64"},
	{"trsv", "numbers", "Mflop/s", t_trsv, 0, "a triangular solve, 256"},
	{"qr", "numbers", "Mflop/s", t_qr, 0, "Householder QR least squares, 64 x 48"},
	{"fft", "numbers", "Mflop/s", t_fft, 0, "complex FFT of 1,024 and back"},
	{"sort", "numbers", "M/s", t_sort, 0, "quicksort of 8,192 words"},
	{"hash", "numbers", "M/s", t_hash_table, 0, "a hash table: puts and gets"},
	{"crc32", "numbers", "MB/s", t_crc32, 0, "CRC-32 of 128 KB"},
	{"inflate", "media", "MB/s", t_inflate, 0, "a deflate stream on one core"},
	{"png-dec", "media", "Mpx/s", t_png_decode, W, "a PNG of 256 x 192 on the workers"},
	{"jpeg-enc", "media", "Mpx/s", t_jpeg_encode, W, "a JPEG of 256 x 192 on the workers"},
	{"jpeg-dec", "media", "Mpx/s", t_jpeg_decode, W, "a JPEG of 256 x 192 on the workers"},
	{"gpu-list", "gpu", "lists/s", perf_gpu_lists, PERF_GPU, "a list that clears, drawn and waited for"},
	{"gpu-tris", "gpu", "Mtri/s", perf_gpu_tris, PERF_GPU, "lists of 2,000 small triangles"},
	{"gpu-quads", "gpu", "M/s", perf_gpu_quads, PERF_GPU, "lists of 400 textured quads"},
	{"gpu-fill", "gpu", "Mpx/s", perf_gpu_fill, PERF_GPU, "lists of 16 quads over the picture"},
	{"gpu-copy", "gpu", "MB/s", perf_gpu_memcpy, PERF_GPU, "memcpy in GPU memory"},
};
const int perf_count = sizeof perf_tests / sizeof perf_tests[0];
