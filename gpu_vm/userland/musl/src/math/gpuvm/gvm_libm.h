/* gpu_vm's float libm (gpu_vm/userland/README.md, "libm"): the shared parts.
 *
 * Every function here uses only the card's exact float ops (add, sub, mul and the multiply-adds, which round the
 * product first, floor/round/trunc, conversions, compares) and integer ops, never a special function (fsin fcos
 * fexp2 flog2 frcp frsqrt, which are approximations on the card) or a division where avoidable: the results are the
 * same bits on gvm_cpu and on the card, and within about an ulp of the true value (the tables in the README).
 * The polynomials are fitted by gpu_vm/llvm/test/libm/fit.c, the accuracy measured by acc.c over every float.
 *
 * With GVM_HOST the same code is compiled for the host (acc.c: -ffp-contract=off, denormals flushed), where the
 * generic fallbacks are the host's own functions. */
#ifndef GVM_LIBM_H
#define GVM_LIBM_H

#include <stdint.h>

static inline uint32_t gvm_asu(float f)
{
	union { float f; uint32_t u; } v = { f };
	return v.u;
}

static inline float gvm_asf(uint32_t u)
{
	union { uint32_t u; float f; } v = { u };
	return v.f;
}

/* the card's square root (within 0.9 ulp on the card, correctly rounded on gvm_cpu); asm, as __builtin_sqrtf is a
 * call of sqrtf where math errno is on */
static inline float gvm_sqrtf(float x)
{
#ifdef GVM_HOST
	return __builtin_sqrtf(x);
#else
	float r;
	__asm__("fsqrt %0, %1" : "=r"(r) : "r"(x));
	return r;
#endif
}

/* ---- sin and cos: x = k pi/2 + r, |r| <= pi/4 (and a little), for |x| < 2^13. pi/2 in four parts: k times each of
 * the first three is exact for |k| < 2^13, so r loses nothing but the last part's rounding. */
#define GVM_TRIG_MAX 0x46000000u   /* 8192.0f: |x| beyond it (and inf, nan) go to musl's generic code */
static const float
gvm_2_pi = 0x1.45f306p-1f,
gvm_pio2_1 = 0x1.92p+0f,
gvm_pio2_2 = 0x1.fb4p-12f,
gvm_pio2_3 = 0x1.444p-24f,
gvm_pio2_4 = 0x1.68c234p-39f,
/* sin r = r + r^3 S(r^2), relative error 2^-28.1 on |r| <= pi/4 */
gvm_S0 = -0x1.555546p-3f,
gvm_S1 = 0x1.11076p-7f,
gvm_S2 = -0x1.994e9p-13f,
/* cos r = 1 - r^2 / 2 + r^4 C(r^2), error 2^-33.3 */
gvm_C0 = 0x1.55554ap-5f,
gvm_C1 = -0x1.6c0c8cp-10f,
gvm_C2 = 0x1.9a023ap-16f;

/* r = r + rl (rl about an ulp of r or less) and the quadrant k (mod 4): x - k P1 - k P2 is exact, the last two
 * parts' products are subtracted with their rounding errors kept */
static inline float gvm_trig_reduce(float x, int *k, float *rl)
{
	float kf = __builtin_roundevenf(x * gvm_2_pi);
	*k = (int)kf;
	float a = (x - kf * gvm_pio2_1) - kf * gvm_pio2_2;
	float b = kf * gvm_pio2_3, r1 = a - b;
	float t = kf * gvm_pio2_4 - ((a - r1) - b);
	float r = r1 - t;
	*rl = (r1 - r) - t;
	return r;
}

/* sin(r + rl), z = r^2 */
static inline float gvm_sin_poly(float r, float rl, float z)
{
	return r + (rl + r * z * (gvm_S0 + z * (gvm_S1 + z * gvm_S2)));
}

/* cos(r + rl): 1 - z/2 with its rounding error kept */
static inline float gvm_cos_poly(float r, float rl, float z)
{
	float hz = 0.5f * z, w = 1.0f - hz;
	return w + ((((1.0f - w) - hz) - r * rl) + z * z * (gvm_C0 + z * (gvm_C1 + z * gvm_C2)));
}

/* ---- exp2 of r, |r| <= 0.5 (and a little): 1 + r P(r), relative error 2^-28.9 */
static const float
gvm_E0 = 0x1.62e43p-1f,
gvm_E1 = 0x1.ebfbdcp-3f,
gvm_E2 = 0x1.c6aee8p-5f,
gvm_E3 = 0x1.3b2d4cp-7f,
gvm_E4 = 0x1.5f3e54p-10f,
gvm_E5 = 0x1.41fbbcp-13f;

static inline float gvm_exp2_poly(float r)
{
	return 1.0f + r * (gvm_E0 + r * (gvm_E1 + r * (gvm_E2 + r * (gvm_E3 + r * (gvm_E4 + r * gvm_E5)))));
}

/* p 2^n for n in [-252, 254], by two exact steps: a result below 2^-126 is 0 (the card flushes), above the range
 * inf */
static inline float gvm_scale(float p, int n)
{
	int a = n >> 1;
	return p * gvm_asf((uint32_t)(a + 127) << 23) * gvm_asf((uint32_t)(n - a + 127) << 23);
}

/* ---- log: x = 2^k z, z in [0x3f330000, 0x3fb30000) as bits (0.6992 to 1.3984), 64 intervals of z by the top six
 * bits of x - 0x3f330000; each has invc, 1/z's middle rounded to 8 bits (1 for the interval round 1), so that
 * z invc - 1 is an exact pair: z's top 8 bits times invc and the rest times invc. r = z invc - 1, |r| < 0.0099. */
#define GVM_LOG_OFF 0x3f330000u
struct gvm_log_entry { float invc, hi, lo, pad; };
/* hi + lo: -log(invc) (log2 for the second table); hi on a grid of 2^-16, so that k ln2hi + hi is exact */
extern const struct gvm_log_entry __gvm_log_tab[64], __gvm_log2_tab[64];
static const float
gvm_ln2hi = 0x1.62e4p-1f,          /* 16 bits */
gvm_ln2lo = 0x1.7f7d1cp-20f,
gvm_ivln2hi = 0x1.714p+0f,         /* 1/ln2: 12 bits */
gvm_ivln2lo = 0x1.47652cp-12f,
/* log(1 + r) = r - r^2 / 2 + r^3 L(r), error 2^-40 relative on |r| < 0.0099 */
gvm_L0 = 0x1.555556p-2f,
gvm_L1 = -0x1.00025ep-2f,
gvm_L2 = 0x1.99a0b6p-3f,
/* log2(1 + r) = r / ln2 + r^2 M(r), error 2^-41 */
gvm_M0 = -0x1.715476p-1f,
gvm_M1 = 0x1.ec709ep-2f,
gvm_M2 = -0x1.71596p-2f,
gvm_M3 = 0x1.277c4ep-2f;

/* x positive and normal: k, the table's entry, rh + rl = z invc - 1 exactly (rh on a grid of 2^-17) */
static inline const struct gvm_log_entry *gvm_log_reduce(uint32_t ix, const struct gvm_log_entry *tab, int *k,
	float *rh, float *rl)
{
	uint32_t tmp = ix - GVM_LOG_OFF;
	const struct gvm_log_entry *e = &tab[(tmp >> 17) & 63];
	*k = (int32_t)tmp >> 23;
	uint32_t iz = ix - (tmp & 0xff800000u);
	float zh = gvm_asf(iz & 0xffff0000u), zl = gvm_asf(iz) - zh;
	*rh = zh * e->invc - 1.0f;
	*rl = zl * e->invc;
	return e;
}

/* s + e = a + b exactly (Knuth's two-sum) */
static inline float gvm_two_sum(float a, float b, float *e)
{
	float s = a + b, bb = s - a;
	*e = (a - (s - bb)) + (b - bb);
	return s;
}

/* log2 of a positive normal x as hi + lo, |lo| about an ulp of hi or less, with about 2^-36 relative error */
static inline float gvm_log2_hl(uint32_t ix, float *lo)
{
	int k;
	float rh, rl, rr;
	const struct gvm_log_entry *e = gvm_log_reduce(ix, __gvm_log2_tab, &k, &rh, &rl);
	float r = gvm_two_sum(rh, rl, &rr);                  /* r + rr = z invc - 1 exactly */
	/* r / ln2: r in two halves of 12 bits, each times ivln2hi (12 bits) exact */
	float r1 = gvm_asf(gvm_asu(r) & 0xfffff000u), r2 = r - r1;
	float err, s = gvm_two_sum((float)k + e->hi, r1 * gvm_ivln2hi, &err);
	float t = r2 * gvm_ivln2hi + (rr * gvm_ivln2hi + r * gvm_ivln2lo + e->lo
		+ r * r * (gvm_M0 + r * (gvm_M1 + r * (gvm_M2 + r * gvm_M3))));
	t = err + t;
	float h = s + t;
	*lo = (s - h) + t;
	return h;
}

#ifdef GVM_HOST
float gvm_host_sinf(float), gvm_host_cosf(float), gvm_host_tanf(float), gvm_host_powf(float, float);
void gvm_host_sincosf(float, float *, float *);
#endif

#endif
