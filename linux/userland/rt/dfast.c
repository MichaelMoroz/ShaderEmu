/* A double's sum, difference and product in software for RV32, in place of the library's (toolchain.sh): two finite,
   normal operands and a normal result take a path of their own on 32-bit words (a sum some 45 instructions, a product
   some 60, where soft-fp's sum is 96 and compiler-rt's product 107); zeros, subnormals, infinities, NaNs, an overflow
   or an underflow go to the library's routine (__adddf3_slow, __muldf3_slow), which is the same result bit for bit.
   Round to nearest even, as the slow routines. Checked against the host's doubles by test_df.c (hundreds of millions of
   operands shaped to reach every branch) and fpcheck.c. */
typedef unsigned u32;
typedef unsigned long long u64;
typedef union { double d; u64 u; struct { u32 lo, hi; } w; } du;

double __adddf3_slow(double, double);
double __muldf3_slow(double, double);

static inline __attribute__((always_inline)) unsigned clz32(u32 x)   /* x != 0 */
{
	unsigned n = 0;
	if (!(x >> 16)) { n = 16; x <<= 16; }
	if (!(x >> 24)) { n += 8; x <<= 8; }
	if (!(x >> 28)) { n += 4; x <<= 4; }
	if (!(x >> 30)) { n += 2; x <<= 2; }
	return n + !(x >> 31);
}

static inline __attribute__((always_inline)) double make(u32 hi, u32 lo)
{
	du r;
	r.w.hi = hi;
	r.w.lo = lo;
	return r.d;
}

/* a + b, where b's sign word is bh (b's own, or flipped for a difference) */
static inline __attribute__((always_inline)) double add(u32 ah, u32 al, u32 bh, u32 bl)
{
	u32 aah = ah & 0x7fffffff, abh = bh & 0x7fffffff;
	if (aah < abh || (aah == abh && al < bl)) {          /* |a| >= |b| from here */
		u32 t;
		t = ah, ah = bh, bh = t;
		t = al, al = bl, bl = t;
		t = aah, aah = abh, abh = t;
	}
	u32 ea = aah >> 20, eb = abh >> 20;
	if (ea - 1 >= 0x7fe || eb - 1 >= 0x7fe)
		goto slow;
	/* 53 bits and three below them (guard, round, sticky): mh holds bits 32..55 */
	u32 mh = ((aah & 0xfffff) | 0x100000) << 3 | al >> 29, ml = al << 3;
	u32 nh = ((abh & 0xfffff) | 0x100000) << 3 | bl >> 29, nl = bl << 3;
	u32 d = ea - eb, e = ea;
	if (d) {
		if (d < 32) {
			u32 lost = nl << (32 - d);
			nl = (nl >> d) | (nh << (32 - d)) | (lost != 0);
			nh >>= d;
		} else if (d < 56) {
			u32 s = d - 32, lost = s ? nl | nh << (32 - s) : nl;
			nl = (nh >> s) | (lost != 0);
			nh = 0;
		} else {
			nl = 1;
			nh = 0;
		}
	}
	if ((ah ^ bh) >> 31) {
		u32 borrow = ml < nl;
		ml -= nl;
		mh -= nh + borrow;
		if (!(mh >> 23)) {
			if (!(mh | ml))
				goto slow;                   /* (an exact zero: +0) */
			unsigned s = mh ? clz32(mh) - 8 : 24 + clz32(ml);
			if (e <= s)
				goto slow;                   /* (subnormal) */
			e -= s;
			if (s >= 32) {
				mh = ml << (s - 32);
				ml = 0;
			} else {
				mh = (mh << s) | (s ? ml >> (32 - s) : 0);
				ml <<= s;
			}
		}
	} else {
		u32 l = ml + nl;
		mh += nh + (l < ml);
		ml = l;
		if (mh >> 24) {
			ml = (ml >> 1) | (mh << 31) | (ml & 1);
			mh >>= 1;
			e++;
		}
	}
	u32 g = ml & 7;
	ml = (ml >> 3) | (mh << 29);
	mh >>= 3;
	if (g > 4 || (g == 4 && (ml & 1))) {
		if (!++ml && ++mh >> 21) {
			mh >>= 1;                            /* (2^53: ml is 0, and stays so) */
			e++;
		}
	}
	if (e >= 0x7ff)
		goto slow;
	return make((ah & 0x80000000) | e << 20 | (mh & 0xfffff), ml);
slow:
	return __adddf3_slow(make(ah, al), make(bh, bl));   /* (the two in either order) */
}

double __adddf3(double a, double b)
{
	du x = {a}, y = {b};
	return add(x.w.hi, x.w.lo, y.w.hi, y.w.lo);
}

double __subdf3(double a, double b)
{
	du x = {a}, y = {b};
	return add(x.w.hi, x.w.lo, y.w.hi ^ 0x80000000, y.w.lo);
}

double __muldf3(double a, double b)
{
	du x = {a}, y = {b};
	u32 ah = x.w.hi, bh = y.w.hi;
	u32 ea = (ah >> 20) & 0x7ff, eb = (bh >> 20) & 0x7ff;
	if (ea - 1 >= 0x7fe || eb - 1 >= 0x7fe)
		return __muldf3_slow(a, b);
	u32 mah = (ah & 0xfffff) | 0x100000, mal = x.w.lo;
	u32 mbh = (bh & 0xfffff) | 0x100000, mbl = y.w.lo;
	/* the 106-bit product in four words */
	u64 p0 = (u64)mal * mbl, p1 = (u64)mah * mbl, p2 = (u64)mal * mbh;
	u32 p3 = mah * mbh, p3h = (u32)(((u64)mah * mbh) >> 32);
	u32 w0 = (u32)p0, t = (u32)(p0 >> 32), w1 = t + (u32)p1, c = w1 < t;
	t = w1 + (u32)p2;
	c += t < w1;
	w1 = t;
	u32 u = (u32)(p1 >> 32) + (u32)(p2 >> 32) + c, w2 = u + p3, w3 = p3h + (w2 < u);
	int e = (int)(ea + eb) - 1023;
	if (w3 & 0x200)
		e++;
	else {                                       /* the product under 2^105: one place up */
		w3 = (w3 << 1) | (w2 >> 31);
		w2 = (w2 << 1) | (w1 >> 31);
		w1 = (w1 << 1) | (w0 >> 31);
		w0 <<= 1;
	}
	/* bits 53..105 are the result, 52 the round bit, 0..51 the sticky ones */
	u32 mh = (w3 << 11) | (w2 >> 21), ml = (w2 << 11) | (w1 >> 21);
	if ((w1 >> 20) & 1 && ((w1 & 0xfffff) | w0 | (ml & 1))) {
		if (!++ml && ++mh >> 21) {
			mh >>= 1;
			e++;
		}
	}
	if (e <= 0 || e >= 0x7ff)
		return __muldf3_slow(a, b);
	return make(((ah ^ bh) & 0x80000000) | (u32)e << 20 | (mh & 0xfffff), ml);
}

/* an int or an unsigned as a double: always exact (compiler-rt's are some 70 instructions) */
static inline __attribute__((always_inline)) double from_u32(u32 sign, u32 m)
{
	if (!m)
		return make(sign, 0);
	unsigned z = clz32(m);
	m <<= z;                                     /* the top bit is the implicit one */
	return make(sign | (1054 - z) << 20 | (m >> 11 & 0xfffff), m << 21);
}

double __floatsidf(int i) { return i < 0 ? from_u32(0x80000000, -(u32)i) : from_u32(0, (u32)i); }
double __floatunsidf(unsigned u) { return from_u32(0, u); }
