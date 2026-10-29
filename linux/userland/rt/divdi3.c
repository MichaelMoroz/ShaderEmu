/* 64-bit division for RV32IM in place of compiler-rt's (toolchain.sh puts these in libcompiler_rt.a): compiler-rt's
   generic builtins divide a bit a turn, 500 to 1,100 instructions a call (a 47-bit value by 1,000: 1,028), where two
   steps of the M extension's 32-bit divide by 16-bit digits (Knuth's algorithm D, libgcc's __udiv_qrnnd_c) take some
   40 to 80. Every case was checked against the host's 64-bit division (rt/test_rt.c). */
typedef unsigned u32;
typedef unsigned long long u64;
typedef long long s64;

/* the leading zeros of x != 0 (no Zbb: __builtin_clz would be a call of __clzsi2) */
static inline __attribute__((always_inline)) unsigned clz32(u32 x)
{
	unsigned n = 0;
	if (!(x >> 16)) { n = 16; x <<= 16; }
	if (!(x >> 24)) { n += 8; x <<= 8; }
	if (!(x >> 28)) { n += 4; x <<= 4; }
	if (!(x >> 30)) { n += 2; x <<= 2; }
	return n + !(x >> 31);
}

/* (u1:u0) / d with u1 < d and d's top bit set: the quotient, and the remainder in *r */
static inline __attribute__((always_inline)) u32 qrnnd(u32 u1, u32 u0, u32 d, u32 *r)
{
	u32 d1 = d >> 16, d0 = d & 0xffff;
	u32 q1 = u1 / d1, r1 = u1 % d1, m = q1 * d0;
	r1 = (r1 << 16) | (u0 >> 16);
	if (r1 < m) {
		q1--, r1 += d;
		if (r1 >= d && r1 < m)       /* (no carry out of the add) */
			q1--, r1 += d;
	}
	r1 -= m;
	u32 q0 = r1 / d1, r0 = r1 % d1;
	m = q0 * d0;
	r0 = (r0 << 16) | (u0 & 0xffff);
	if (r0 < m) {
		q0--, r0 += d;
		if (r0 >= d && r0 < m)
			q0--, r0 += d;
	}
	*r = r0 - m;
	return (q1 << 16) | q0;
}

static inline __attribute__((always_inline)) u64 udivmod(u64 n, u64 d, u64 *rp)
{
	u32 n1 = n >> 32, n0 = (u32)n, d1 = d >> 32, d0 = (u32)d, r;
	if (!d1) {
		if (!n1) {
			if (rp) *rp = n0 % d0;
			return n0 / d0;
		}
		u32 q1 = 0;
		if (n1 >= d0) {
			q1 = n1 / d0;
			n1 = n1 % d0;
		}
		unsigned s = clz32(d0);
		if (s) {
			d0 <<= s;
			n1 = (n1 << s) | (n0 >> (32 - s));
			n0 <<= s;
		}
		u32 q0 = qrnnd(n1, n0, d0, &r);
		if (rp) *rp = r >> s;
		return ((u64)q1 << 32) | q0;
	}
	if (d1 > n1) {
		if (rp) *rp = n;
		return 0;
	}
	unsigned s = clz32(d1);
	if (!s) {                            /* d >= 2^63 and n >= d's high word: the quotient is 0 or 1 */
		u32 q = n1 > d1 || n0 >= d0;
		if (rp) *rp = q ? n - d : n;
		return q;
	}
	d1 = (d1 << s) | (d0 >> (32 - s));
	d0 <<= s;
	u32 n2 = n1 >> (32 - s);
	n1 = (n1 << s) | (n0 >> (32 - s));
	n0 <<= s;
	u32 q = qrnnd(n2, n1, d1, &r);       /* q is the quotient or one over it */
	u64 m = (u64)q * d0, rr = ((u64)r << 32) | n0;
	if (m > rr) {
		q--;
		m -= ((u64)d1 << 32) | d0;   /* (rr - m now fits: the true remainder) */
	}
	if (rp) *rp = (rr - m) >> s;
	return q;
}

u64 __udivmoddi4(u64 a, u64 b, u64 *rem) { return udivmod(a, b, rem); }
u64 __udivdi3(u64 a, u64 b) { return udivmod(a, b, 0); }
u64 __umoddi3(u64 a, u64 b) { u64 r; udivmod(a, b, &r); return r; }

s64 __divdi3(s64 a, s64 b)
{
	u64 ua = a < 0 ? -(u64)a : (u64)a, ub = b < 0 ? -(u64)b : (u64)b;
	u64 q = udivmod(ua, ub, 0);
	return (a ^ b) < 0 ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 a, s64 b)
{
	u64 ua = a < 0 ? -(u64)a : (u64)a, ub = b < 0 ? -(u64)b : (u64)b, r;
	udivmod(ua, ub, &r);
	return a < 0 ? -(s64)r : (s64)r;
}

s64 __divmoddi4(s64 a, s64 b, s64 *rem)
{
	u64 ua = a < 0 ? -(u64)a : (u64)a, ub = b < 0 ? -(u64)b : (u64)b, r;
	u64 q = udivmod(ua, ub, &r);
	*rem = a < 0 ? -(s64)r : (s64)r;
	return (a ^ b) < 0 ? -(s64)q : (s64)q;
}
