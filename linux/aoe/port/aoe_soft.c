/* Double precision's commonest two calls, for a game that compares floats with double constants at every turn: libgcc's
 * conversion and comparison were 10% of the first core's instructions in play. The same results for every input,
 * NaNs and subnormals too; libgcc's general paths are not needed for them (docs/games/aoe.md, "Speed in play"). */
#include <stdint.h>

typedef union { double d; uint64_t u; struct { uint32_t lo, hi; } w; } D;

/* A float in a register of the soft-float convention, a double in a pair. */
uint64_t __extendsfdf2(uint32_t f)
{
    uint32_t sign = f & 0x80000000u, exp = (f >> 23) & 255, frac = f & 0x7fffff;
    D r;
    if (__builtin_expect(exp - 1 < 254, 1)) {   /* the normal numbers */
        r.w.hi = sign | ((exp + 896) << 20) | (frac >> 3);
        r.w.lo = frac << 29;
        return r.u;
    }
    if (exp == 255) {   /* infinity and NaN: a signalling one comes out quiet, as from libgcc */
        r.w.hi = sign | 0x7ff00000u | (frac >> 3) | (frac ? 0x80000u : 0);
        r.w.lo = frac << 29;
        return r.u;
    }
    if (!frac) { r.w.hi = sign; r.w.lo = 0; return r.u; }
    int shift = __builtin_clz(frac) - 8;   /* a subnormal float is a normal double */
    frac = (frac << shift) & 0x7fffff;
    r.w.hi = sign | ((uint32_t) (897 - shift) << 20) | (frac >> 3);
    r.w.lo = frac << 29;
    return r.u;
}

/* -1, 0, 1, or `unordered` for a NaN. */
static inline int compare(uint32_t ah, uint32_t al, uint32_t bh, uint32_t bl, int unordered)
{
    uint32_t am = ah & 0x7fffffffu, bm = bh & 0x7fffffffu;
    if (__builtin_expect(am >= 0x7ff00000u || bm >= 0x7ff00000u, 0)) {
        if ((am > 0x7ff00000u || (am == 0x7ff00000u && al)) || (bm > 0x7ff00000u || (bm == 0x7ff00000u && bl)))
            return unordered;
    }
    if (!(am | al | bm | bl)) return 0;   /* +0 and -0 */
    if ((ah ^ bh) >> 31) return ah >> 31 ? -1 : 1;
    int less = am < bm || (am == bm && al < bl);
    if (am == bm && al == bl) return 0;
    return (less ^ (ah >> 31)) ? -1 : 1;
}

int __ltdf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, 1); }
int __ledf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, 1); }
int __eqdf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, 1); }
int __nedf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, 1); }
int __gtdf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, -1); }
int __gedf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, -1); }
int __cmpdf2(uint32_t al, uint32_t ah, uint32_t bl, uint32_t bh) { return compare(ah, al, bh, bl, 1); }
