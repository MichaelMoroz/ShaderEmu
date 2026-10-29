/* flm, the float libm, on RISC-V's F instructions (docs/machine/fpu.md): musl's sinf, cosf, expf, logf, powf and
 * the rest work in doubles, which are compiler-rt's software here (788 to 1,898 instructions a call); these are 14 to
 * 58 float instructions. linux/userland/toolchain.sh puts them in libc.a in place of musl's. A denormal argument or
 * result goes to musl's generic code.
 *
 * Every function here uses only exact float ops (add, sub, mul and the multiply-adds, which round the product first,
 * conversions, compares), fsqrt.s and integer ops, never a division where avoidable: the results are the same bits on
 * rvc_cpu and on the card, and within about an ulp of the true value. The polynomials are minimax fits. */
#ifndef FLM_LIBM_H
#define FLM_LIBM_H

#include <stdint.h>

static inline uint32_t flm_asu(float f)
{
	union { float f; uint32_t u; } v = { f };
	return v.u;
}

static inline float flm_asf(uint32_t u)
{
	union { uint32_t u; float f; } v = { u };
	return v.f;
}

/* the square root (fsqrt.s); asm, as __builtin_sqrtf is a call of sqrtf where math errno is on */
static inline float flm_sqrtf(float x)
{
	float r;
	__asm__("fsqrt.s %0, %1" : "=f"(r) : "f"(x));
	return r;
}

/* x rounded to an integer, ties to even, for |x| < 2^31 (fcvt.w.s with its rounding in the instruction: without Zfa
 * __builtin_roundevenf is a call of roundevenf, which musl does not have) */
static inline float flm_roundevenf(float x)
{
	int i;
	__asm__("fcvt.w.s %0, %1, rne" : "=r"(i) : "f"(x));
	return (float)i;
}

/* ---- sin and cos: x = k pi/2 + r, |r| <= pi/4 (and a little), for |x| < 2^13. pi/2 in four parts: k times each of
 * the first three is exact for |k| < 2^13, so r loses nothing but the last part's rounding. */
#define FLM_TRIG_MAX 0x46000000u   /* 8192.0f: |x| beyond it (and inf, nan) go to musl's generic code */
static const float
flm_2_pi = 0x1.45f306p-1f,
flm_pio2_1 = 0x1.92p+0f,
flm_pio2_2 = 0x1.fb4p-12f,
flm_pio2_3 = 0x1.444p-24f,
flm_pio2_4 = 0x1.68c234p-39f,
/* sin r = r + r^3 S(r^2), relative error 2^-28.1 on |r| <= pi/4 */
flm_S0 = -0x1.555546p-3f,
flm_S1 = 0x1.11076p-7f,
flm_S2 = -0x1.994e9p-13f,
/* cos r = 1 - r^2 / 2 + r^4 C(r^2), error 2^-33.3 */
flm_C0 = 0x1.55554ap-5f,
flm_C1 = -0x1.6c0c8cp-10f,
flm_C2 = 0x1.9a023ap-16f;

/* r = r + rl (rl about an ulp of r or less) and the quadrant k (mod 4): x - k P1 - k P2 is exact, the last two
 * parts' products are subtracted with their rounding errors kept */
static inline float flm_trig_reduce(float x, int *k, float *rl)
{
	float kf = flm_roundevenf(x * flm_2_pi);
	*k = (int)kf;
	float a = (x - kf * flm_pio2_1) - kf * flm_pio2_2;
	float b = kf * flm_pio2_3, r1 = a - b;
	float t = kf * flm_pio2_4 - ((a - r1) - b);
	float r = r1 - t;
	*rl = (r1 - r) - t;
	return r;
}

/* sin(r + rl), z = r^2 */
static inline float flm_sin_poly(float r, float rl, float z)
{
	return r + (rl + r * z * (flm_S0 + z * (flm_S1 + z * flm_S2)));
}

/* cos(r + rl): 1 - z/2 with its rounding error kept */
static inline float flm_cos_poly(float r, float rl, float z)
{
	float hz = 0.5f * z, w = 1.0f - hz;
	return w + ((((1.0f - w) - hz) - r * rl) + z * z * (flm_C0 + z * (flm_C1 + z * flm_C2)));
}

/* ---- exp2 of r, |r| <= 0.5 (and a little): 1 + r P(r), relative error 2^-28.9 */
static const float
flm_E0 = 0x1.62e43p-1f,
flm_E1 = 0x1.ebfbdcp-3f,
flm_E2 = 0x1.c6aee8p-5f,
flm_E3 = 0x1.3b2d4cp-7f,
flm_E4 = 0x1.5f3e54p-10f,
flm_E5 = 0x1.41fbbcp-13f;

static inline float flm_exp2_poly(float r)
{
	return 1.0f + r * (flm_E0 + r * (flm_E1 + r * (flm_E2 + r * (flm_E3 + r * (flm_E4 + r * flm_E5)))));
}

/* p 2^n for n in [-252, 254], by two exact steps: a result below 2^-126 is 0 (flushed), above the range
 * inf */
static inline float flm_scale(float p, int n)
{
	int a = n >> 1;
	return p * flm_asf((uint32_t)(a + 127) << 23) * flm_asf((uint32_t)(n - a + 127) << 23);
}

/* ---- log: x = 2^k z, z in [0x3f330000, 0x3fb30000) as bits (0.6992 to 1.3984), 64 intervals of z by the top six
 * bits of x - 0x3f330000; each has invc, 1/z's middle rounded to 8 bits (1 for the interval round 1), so that
 * z invc - 1 is an exact pair: z's top 8 bits times invc and the rest times invc. r = z invc - 1, |r| < 0.0099. */
#define FLM_LOG_OFF 0x3f330000u
struct flm_log_entry { float invc, hi, lo, pad; };
/* hi + lo: -log(invc) (log2 for the second table); hi on a grid of 2^-16, so that k ln2hi + hi is exact */
extern const struct flm_log_entry __flm_log_tab[64], __flm_log2_tab[64];
static const float
flm_ln2hi = 0x1.62e4p-1f,          /* 16 bits */
flm_ln2lo = 0x1.7f7d1cp-20f,
flm_ivln2hi = 0x1.714p+0f,         /* 1/ln2: 12 bits */
flm_ivln2lo = 0x1.47652cp-12f,
/* log(1 + r) = r - r^2 / 2 + r^3 L(r), error 2^-40 relative on |r| < 0.0099 */
flm_L0 = 0x1.555556p-2f,
flm_L1 = -0x1.00025ep-2f,
flm_L2 = 0x1.99a0b6p-3f,
/* log2(1 + r) = r / ln2 + r^2 M(r), error 2^-41 */
flm_M0 = -0x1.715476p-1f,
flm_M1 = 0x1.ec709ep-2f,
flm_M2 = -0x1.71596p-2f,
flm_M3 = 0x1.277c4ep-2f;

/* x positive and normal: k, the table's entry, rh + rl = z invc - 1 exactly (rh on a grid of 2^-17) */
static inline const struct flm_log_entry *flm_log_reduce(uint32_t ix, const struct flm_log_entry *tab, int *k,
	float *rh, float *rl)
{
	uint32_t tmp = ix - FLM_LOG_OFF;
	const struct flm_log_entry *e = &tab[(tmp >> 17) & 63];
	*k = (int32_t)tmp >> 23;
	uint32_t iz = ix - (tmp & 0xff800000u);
	float zh = flm_asf(iz & 0xffff0000u), zl = flm_asf(iz) - zh;
	*rh = zh * e->invc - 1.0f;
	*rl = zl * e->invc;
	return e;
}

/* s + e = a + b exactly (Knuth's two-sum) */
static inline float flm_two_sum(float a, float b, float *e)
{
	float s = a + b, bb = s - a;
	*e = (a - (s - bb)) + (b - bb);
	return s;
}

/* log2 of a positive normal x as hi + lo, |lo| about an ulp of hi or less, with about 2^-36 relative error */
static inline float flm_log2_hl(uint32_t ix, float *lo)
{
	int k;
	float rh, rl, rr;
	const struct flm_log_entry *e = flm_log_reduce(ix, __flm_log2_tab, &k, &rh, &rl);
	float r = flm_two_sum(rh, rl, &rr);                  /* r + rr = z invc - 1 exactly */
	/* r / ln2: r in two halves of 12 bits, each times ivln2hi (12 bits) exact */
	float r1 = flm_asf(flm_asu(r) & 0xfffff000u), r2 = r - r1;
	float err, s = flm_two_sum((float)k + e->hi, r1 * flm_ivln2hi, &err);
	float t = r2 * flm_ivln2hi + (rr * flm_ivln2hi + r * flm_ivln2lo + e->lo
		+ r * r * (flm_M0 + r * (flm_M1 + r * (flm_M2 + r * flm_M3))));
	t = err + t;
	float h = s + t;
	*lo = (s - h) + t;
	return h;
}


#endif
