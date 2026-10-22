// Step x12 (gpu_vm/tick/steps/README.md): every fast path in one tick: integers (path 1), loads and stores (2, 3),
// floats (4, 5) and a memory op with a float sum (6), gpu_vm/isa.h's paths. Memory as s21 makes it (x11): both ops'
// addresses, texels and write cache entries read at once, words and slots chosen by selects; a store with no room
// ends the pass before its bundle (op 0 undone when it was op 1's). Floats as the card makes them (gvm_ref.h's
// gvm_fop: the one NaN, -0 below +0, the special functions approximate). A bundle of path 0 halts the core.
//
// Variants (gvmbench --variants; each switch at its use): PREFETCH, WCN, ARR, Q1_LATE, INT_FIRST, DISPATCH_SWITCH,
// OK_MIN, SINK0, STORE_FLAT, HASH.
#include "../../isa.h"

#ifndef PREFETCH
#define PREFETCH 1
#endif
#ifndef WCN
#define WCN 128
#endif
#define HALF (WCN / 2)
#define NONE 0xffffffffu
Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext, _NCores; uint3 _ppad;
};
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
// a loop of one trip whose count only the pass knows: the card's compiler cannot run its body predicated beside
// the code around it (FENCE_* variants)
#define FENCE_BEGIN [loop] for (uint fence_k = 0; fence_k < 1 + (_Flags >> 31); fence_k++) {
#define FENCE_END }
#ifndef ARR
#define ARR 65
#endif
static uint r[ARR];
static uint2 wc[WCN];
static uint4 occv;   // a bit a slot: taken this pass
static uint pc, cycle, opsrun, flags, ran;
static uint4 sys1, sys2, sys3x;   // texels the step carries and does not use

uint simm12(uint w) { return asuint((int)w >> 20); }
uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }

// ---- Integers: every result in parallel, then a tree of selects on f's bits
uint alu(uint w, uint imm, uint a, uint b) {
    uint f = w & 31;
    bool I = (w & 32) != 0, sw = I && f == GVM_I_SUB;   // rsub: imm - ra
    uint B = I ? imm : b, x = sw ? B : a, y = sw ? a : B;
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    uint yi = f2 ? ~y : y, sum = x + yi + (f2 ? 1u : 0u);   // sub is x + ~y + 1
    uint g0 = f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
    uint s = y & 31, sl = x << s, sr = x >> s;   // (a shift by 32 - 0 is by 0: the rotates need no test)
    uint lo = f1 ? (f0 ? sr | (x << (32 - s)) : asuint(asint(x) >> s)) : (f0 ? sr : sl);
    uint hi = f1 ? ((x == y) != f0 ? 1u : 0u) : (f0 ? x * y : sl | (x >> (32 - s)));
    uint F = f0 ? 0u : 0x80000000u, xs = x ^ F, ys = y ^ F;
    bool lt = f1 ? ys < xs : xs < ys;
    uint g2 = f2 ? (lt ? x : y) : (lt ? 1u : 0u);
    return (f & 16) ? g2 : (f & 8) ? (f2 ? hi : lo) : g0;
}
uint alu_lite(uint w, uint imm, uint a, uint b) {   // op 1: sums, logic and the three shifts (f 0-10)
    uint f = w & 31;
    bool I = (w & 32) != 0, sw = I && f == GVM_I_SUB;
    uint B = I ? imm : b, x = sw ? B : a, y = sw ? a : B;
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    uint yi = f2 ? ~y : y, sum = x + yi + (f2 ? 1u : 0u);
    uint g0 = f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
    uint s = y & 31;
    return (f & 8) ? (f1 ? asuint(asint(x) >> s) : (f0 ? x >> s : x << s)) : g0;
}

// ---- Floats: the I form's operand is its imm12 as a float's top bits (or the long immediate)
uint fnan(float v) { uint u = asuint(v); return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u; }
bool nan_u(uint u) { return (u & 0x7fffffffu) > 0x7f800000u; }
uint fb(uint w, uint imm, bool L, uint b) { return (w & 32) ? (L ? imm : (w >> 20) << 20) : b; }
uint fsum(uint f, uint ra, uint rb, uint rc) {   // f 0-7: add mul mad msb sub nmsb min max
    precise float a = asfloat(ra);
    precise float b = asfloat(rb);
    precise float c = asfloat(rc);
    precise float p = a * b;
    precise float pr = asfloat(fnan(p));   // the product rounded before the sum
    bool prod = f == GVM_F_MAD || f == GVM_F_MSB, nm = f == GVM_F_NMSB;
    precise float X = prod ? pr : nm ? c : a;
    precise float Y = prod ? c : nm ? pr : b;
    precise float s = (f == GVM_F_MSB || f == GVM_F_SUB || nm) ? X - Y : X + Y;
    bool an = nan_u(ra), bn = nan_u(rb);
    bool less = a < b || (a == b && (ra >> 31) > (rb >> 31)), more = a > b || (a == b && (rb >> 31) > (ra >> 31));
    uint mm = an ? (bn ? 0x7fc00000u : rb) : bn ? ra : (((f & 1) != 0 ? more : less) ? ra : rb);
    return f >= GVM_F_MIN ? mm : f == GVM_F_MUL ? fnan(p) : fnan(s);
}
uint itof(uint a) { bool n = (a >> 31) != 0; return asuint((float)(n ? 0 - a : a)) | (n ? 0x80000000u : 0); }   // (fxc2: by size and sign)
uint fpu(uint w, uint ra, uint rb, uint rc) {
    uint f = w & 31, v;
#if NO_SFU   // (probe)
    [branch] if (false) {
#else
    [branch] if (f >= GVM_F_DIV) {
#endif   // the special functions, behind a branch
        precise float a = asfloat(ra);
        precise float y;
        [branch] if (f == GVM_F_DIV) {   // (the card divides by 1 / b: two ordinary numbers with their exponents moved)
            uint ea = (ra >> 23) & 0xff, eb = (rb >> 23) & 0xff;
            int de = (int)ea - (int)eb, ha = clamp(de, -126, 127);
            precise float da = asfloat((ra & 0x807fffffu) | ((uint)(127 + ha) << 23));
            precise float db = asfloat((rb & 0x807fffffu) | ((uint)(127 - (de - ha)) << 23));
            y = (ea - 1 < 254 && eb - 1 < 254) ? da / db : a / asfloat(rb);
        } else y = f == GVM_F_SQRT ? sqrt(a) : f == GVM_F_RSQRT ? rsqrt(a) : f == GVM_F_RCP ? rcp(a) : f == GVM_F_SIN ? sin(a)
                 : f == GVM_F_COS ? cos(a) : f == GVM_F_EXP2 ? exp2(a) : log2(a);
        v = fnan(y);
    } else {
        precise float a = asfloat(ra);
        precise float b = asfloat(rb);
        bool an = nan_u(ra), bn = nan_u(rb), f0 = (f & 1) != 0, f1 = (f & 2) != 0;
        // f 8-15: eq lt le unord sgnj sgnjn sgnjx frac
        bool cmp = !an && !bn && (f == GVM_F_EQ ? a == b : f == GVM_F_LT ? a < b : a <= b);
        uint sg = (ra & 0x7fffffffu) | ((f == GVM_F_SGNJ ? rb : f == GVM_F_SGNJN ? ~rb : ra ^ rb) & 0x80000000u);
        uint g1 = (f & 4) ? (f == GVM_F_FRAC ? fnan(a - floor(a)) : sg) : f == GVM_F_UNORD ? ((an || bn) ? 1u : 0u) : (cmp ? 1u : 0u);
        // f 16-23: itof utof ftoi ftou floor ceil trunc round
        precise float wh = f1 ? (f0 ? round(a) : trunc(a)) : (f0 ? ceil(a) : floor(a));
        uint toi = an ? 0 : a >= 2147483648.0 ? 0x7fffffffu : a <= -2147483648.0 ? 0x80000000u : asuint((int)a);
        uint tou = (an || a <= 0.0) ? 0 : a >= 4294967296.0 ? 0xffffffffu : a >= 2147483648.0 ? 0x80000000u + (uint)(a - 2147483648.0) : (uint)a;
#if NO_CONV   // (probe)
        uint g2 = 0;
#else
        uint g2 = (f & 4) ? fnan(wh) : f1 ? (f0 ? tou : toi) : (f0 ? asuint((float)ra) : itof(ra));
#endif
        v = (f & 16) ? g2 : (f & 8) ? g1 : fsum(f, ra, rb, rc);
    }
    return v;
}

// ---- Memory: the write cache holds word w in slot h or h + HALF, filled in that order, never emptied in a pass
#if HASH   // (variant: the word's own low bits)
uint wc_h(uint w) { return w & (HALF - 1); }
#else
uint wc_h(uint w) { return (w ^ (w >> 6)) & (HALF - 1); }
#endif
bool occ(uint s) { return ((pick(occv, s >> 5) >> (s & 31)) & 1) != 0; }
uint mem_addr(uint w, uint imm, uint a, uint b) { return (w & 16) ? a + (b << (w >> 26)) : a + imm; }
// what one memory op needs, read at once: its address, word, texel, slots' entries
struct mop { uint ad, wa, h; uint4 q; uint2 ea, eb; };
mop mread(uint w, uint imm, uint a, uint b) {
    mop m;
    m.ad = mem_addr(w, imm, a, b); m.wa = m.ad >> 2; m.h = wc_h(m.wa);
    m.q = RAM(m.ad >> 4); m.ea = wc[m.h]; m.eb = wc[m.h + HALF];
    return m;
}
// a load's value, or a store made (false: no room, nothing done): sl the slot it wrote, was what it held
bool mdo(uint w, mop m, uint val, out uint v, out uint sl, out uint2 was) {
    bool ha = occ(m.h) && m.ea.x == m.wa + 1, hb = occ(m.h + HALF) && m.eb.x == m.wa + 1;
    uint word = ha ? m.ea.y : hb ? m.eb.y : pick(m.q, m.wa & 3), sh = (m.ad & 3) * 8, wd = w & 7;
    uint x = word >> sh;
    v = wd == GVM_W_W ? x : wd == GVM_W_B ? asuint((int)(x << 24) >> 24) : wd == GVM_W_H ? asuint((int)(x << 16) >> 16)
      : wd == GVM_W_BU ? (x & 0xffu) : (x & 0xffffu);
    sl = NONE; was = 0;
    bool ok = true;
    [branch] if (w & 8) {   // a store: into its word as this core sees it
        sl = (!occ(m.h) || ha) ? m.h : (!occ(m.h + HALF) || hb) ? m.h + HALF : NONE;
        ok = sl != NONE;
#if STORE_FLAT   // (variant: the store's write without a branch)
        [flatten]
#else
        [branch]
#endif
        if (ok) {
#if FENCE_STORE
          FENCE_BEGIN
#endif
            uint msk = wd == GVM_W_W ? 0xffffffffu : ((wd & 1) ? 0xffffu : 0xffu) << sh;
            was = sl == m.h ? m.ea : m.eb;
            wc[sl] = uint2(m.wa + 1, (word & ~msk) | ((val << sh) & msk));
            occv = put(occv, sl >> 5, pick(occv, sl >> 5) | (1u << (sl & 31)));
#if FENCE_STORE
          FENCE_END
#endif
        }
    }
    return ok;
}

#if SINK0   // (variant: a write to r0 lands there and r0 is cleared after the bundle's ops)
#define WR(d, v) r[d] = v
#else
#define WR(d, v) r[(d) == 0 ? 64 : (d)] = v
#endif
#define FWD(d, v) A1 = ((d) != 0 && ra1 == (d)) ? (v) : A1; B1 = ((d) != 0 && rb1 == (d)) ? (v) : B1
// the bodies: op 0's unit, its result to op 1 by selects, op 1's unit (its write is run's)
void body_int(uint4 t, uint i0, uint i1, uint A0, uint B0, uint ra1, uint rb1, inout uint A1, inout uint B1, uint d0, out uint v0, out uint v1) {
    v0 = alu(t.x, i0, A0, B0);
    WR(d0, v0);
    FWD(d0, v0);
    v1 = alu_lite(t.y, i1, A1, B1);
}
void body_float(uint4 t, uint i0, uint i1, bool L0, bool L1, bool fl1, uint A0, uint B0, uint ra1, uint rb1, inout uint A1, inout uint B1,
                uint d0, out uint v0, out uint v1) {
    v0 = fpu(t.x, A0, fb(t.x, i0, L0, B0), r[t.x >> 26]);
    WR(d0, v0);
    FWD(d0, v0);
    v1 = fl1 ? fsum(t.y & 31, A1, fb(t.y, i1, L1, B1), r[t.y >> 26]) : alu_lite(t.y, i1, A1, B1);
}
// a load or a store, then a lite op, a float sum, or another load or store: false when a store found no room
// (nothing done; op 0 undone when it was op 1's)
uint seen(uint wa, uint4 q, uint h, uint2 e1, uint2 e2) {   // word wa as this core sees it, all read already
    bool m1 = occ(h) && e1.x == wa + 1, m2 = occ(h + HALF) && e2.x == wa + 1;
    return m1 ? e1.y : m2 ? e2.y : pick(q, wa & 3);
}
uint extend(uint w, uint word, uint addr) {   // a load's value from its word
    uint wd = w & 7, v = word >> ((addr & 3) * 8);
    return wd == GVM_W_W ? v : wd == GVM_W_B ? asuint((int)(v << 24) >> 24) : wd == GVM_W_H ? asuint((int)(v << 16) >> 16)
         : wd == GVM_W_BU ? (v & 0xffu) : (v & 0xffffu);
}
// loads alone, as x11 makes them: everything both need read at once, words chosen by selects, no store's work
void body_loads(uint4 t, uint i0, uint i1, bool L1, bool two, bool fl1, uint A0, uint B0, uint ra1, uint rb1, inout uint A1, inout uint B1,
                uint d0, out uint v0, out uint v1) {
    uint ad0 = mem_addr(t.x, i0, A0, B0), ad1 = mem_addr(t.y, i1, A1, B1);
    uint w0 = ad0 >> 2, w1 = ad1 >> 2, h0 = wc_h(w0), h1 = wc_h(w1);
    uint4 q0 = RAM(ad0 >> 4), q1 = RAM(ad1 >> 4);
    uint2 e0a = wc[h0], e0b = wc[h0 + HALF], e1a = wc[h1], e1b = wc[h1 + HALF];
    v0 = extend(t.x, seen(w0, q0, h0, e0a, e0b), ad0);
    WR(d0, v0);
    [branch] if (two && d0 != 0 && (ra1 == d0 || rb1 == d0)) {   // op 1's address from op 0's load: read again
        A1 = ra1 == d0 ? v0 : A1; B1 = rb1 == d0 ? v0 : B1;
        ad1 = mem_addr(t.y, i1, A1, B1); w1 = ad1 >> 2; h1 = wc_h(w1);
        q1 = RAM(ad1 >> 4); e1a = wc[h1]; e1b = wc[h1 + HALF];
    }
    FWD(d0, v0);
    v1 = two ? extend(t.y, seen(w1, q1, h1, e1a, e1b), ad1) : fl1 ? fsum(t.y & 31, A1, fb(t.y, i1, L1, B1), r[t.y >> 26]) : alu_lite(t.y, i1, A1, B1);
}
bool body_mem(uint4 t, uint i0, uint i1, bool L1, bool two, bool fl1, uint A0, uint B0, uint ra1, uint rb1, inout uint A1, inout uint B1,
              inout uint d0, inout uint d1, out uint v0, out uint v1) {
#if NO_STORE   // (probe: loads alone)
    body_loads(t, i0, i1, L1, two, fl1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1); return true;
#endif
#if LOADS_APART   // (variant: a bundle with no store takes x11's loads)
    [branch] if (((t.x | (two ? t.y : 0)) & 8) == 0) { body_loads(t, i0, i1, L1, two, fl1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1); return true; }
#endif
    mop q0 = mread(t.x, i0, A0, B0), q1 = q0;
#if !Q1_LATE   // (Q1_LATE: variant, op 1's texel and entries read when op 0 is done)
    [branch] if (two) q1 = mread(t.y, i1, A1, B1);   // both at once
#endif
    uint4 occ0 = occv;
    uint old0 = r[d0 == 0 ? 64 : d0], sl0, sl1;
    uint2 was0, was1;
    bool st0 = (t.x & 8) != 0, ok = true;
    v1 = 0;
    [branch] if (!mdo(t.x, q0, r[d0], v0, sl0, was0)) ok = false;   // (a store's value is r[rd])
    else {
        d0 = st0 ? 0 : d0;   // a store's rd is its value, not a result
        WR(d0, v0);
        bool dep = d0 != 0 && (ra1 == d0 || rb1 == d0);
        FWD(d0, v0);
        [branch] if (two) {
            // op 1 read again when its address needs op 0's load, or op 0's store shares its slots
#if Q1_LATE
            q1 = mread(t.y, i1, A1, B1);
#else
            [branch] if (dep || (st0 && q1.h == q0.h)) q1 = mread(t.y, i1, A1, B1);
#endif
            [branch] if (!mdo(t.y, q1, r[d1], v1, sl1, was1)) {
                [branch] if (sl0 != NONE) wc[sl0] = was0;
                occv = occ0;
                r[d0 == 0 ? 64 : d0] = old0;
                ok = false;
            }
            d1 = (t.y & 8) ? 0 : d1;
        } else if (fl1) v1 = fsum(t.y & 31, A1, fb(t.y, i1, L1, B1), r[t.y >> 26]);
        else v1 = alu_lite(t.y, i1, A1, B1);
    }
    return ok;
}

void run(uint budget) {
    uint4 cur = RAM(pc >> 4);
    [loop] while (budget > 0) {   // (no break in it: a stop zeroes the budget, as s21 does)
        uint base = pc & ~15u;
        uint4 t = cur;
        uint ctl = t.w, n = ctl & 3, path = (ctl >> 4) & 7, cc = (ctl >> 7) & 15, sub = (ctl >> 11) & 7;
        bool special = cc == GVM_CC_SPECIAL;
        uint tg = base + asuint(special ? (int)ctl >> 14 : (int)ctl >> 23) * 16;
#if PREFETCH
        uint4 nx = RAM((base >> 4) + 1), tc = RAM(tg >> 4);
#endif
#if OK_MIN   // (variant: the assembler's word that a path's bundle is one the loop makes; only path 0 stops it)
        bool ok = path != GVM_PATH_GENERAL;
#else
        bool ok = path != GVM_PATH_GENERAL && path != 7 && n < 3 && (special ? (sub == GVM_SUB_NONE || sub == GVM_SUB_J) : cc != 15);
#endif
        flags |= ok ? 0u : 4u;   // halted
        bool stop = !ok, a1 = n > 1, L = (ctl & 4) != 0, L1 = (ctl & 8) != 0;
        uint i0 = L && !L1 ? t.z : simm12(t.x), i1 = L && L1 ? t.z : simm12(t.y);
        uint d0 = n > 0 ? (t.x >> 8) & 63 : 0, d1 = a1 ? (t.y >> 8) & 63 : 0;
        uint ra0 = (t.x >> 14) & 63, rb0 = (t.x >> 20) & 63, ra1 = (t.y >> 14) & 63, rb1 = (t.y >> 20) & 63;
        uint ca = (ctl >> 11) & 63, cb = (ctl >> 17) & 63;
        // every read first: the branch's as the bundle found them; op 0's result reaches op 1 by selects
        uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
        uint v0 = 0, v1 = 0;
        bool two = path == GVM_PATH_MEM2 && a1, fl1 = path == GVM_PATH_FLOAT2 || path == GVM_PATH_MEMF;
#if DISPATCH_SWITCH   // (variant: a switch on the path)
        [forcecase] switch (ok ? path : 0) {
        case GVM_PATH_INT: body_int(t, i0, i1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1); break;
        case GVM_PATH_MEM: case GVM_PATH_MEM2: case GVM_PATH_MEMF:
            stop = !body_mem(t, i0, i1, L && L1, two, fl1, A0, B0, ra1, rb1, A1, B1, d0, d1, v0, v1); break;
        case GVM_PATH_FLOAT: case GVM_PATH_FLOAT2: body_float(t, i0, i1, L && !L1, L && L1, fl1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1); break;
        default: break;
        }
#else
        bool m0 = path == GVM_PATH_MEM || path == GVM_PATH_MEM2 || path == GVM_PATH_MEMF;
        bool f0 = path == GVM_PATH_FLOAT || path == GVM_PATH_FLOAT2;
#if INT_FIRST   // (variant: the integer body tested first)
#if FENCE_INT
#define FI_B FENCE_BEGIN
#define FI_E FENCE_END
#else
#define FI_B
#define FI_E
#endif
#if FENCE_MEM
#define FM_B FENCE_BEGIN
#define FM_E FENCE_END
#else
#define FM_B
#define FM_E
#endif
#if FENCE_FLOAT
#define FF_B FENCE_BEGIN
#define FF_E FENCE_END
#else
#define FF_B
#define FF_E
#endif
        [branch] if (ok && path == GVM_PATH_INT) { FI_B body_int(t, i0, i1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1); FI_E }
        else if (ok && m0) { FM_B stop = !body_mem(t, i0, i1, L && L1, two, fl1, A0, B0, ra1, rb1, A1, B1, d0, d1, v0, v1); FM_E }
#if !NO_FLOAT   // (NO_FLOAT: probe, no float body)
        else if (ok && f0) { FF_B body_float(t, i0, i1, L && !L1, L && L1, fl1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1); FF_E }
#endif
#else
        [branch] if (!ok) {
        } else if (m0) stop = !body_mem(t, i0, i1, L && L1, two, fl1, A0, B0, ra1, rb1, A1, B1, d0, d1, v0, v1);
        else if (f0) body_float(t, i0, i1, L && !L1, L && L1, fl1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1);
        else body_int(t, i0, i1, A0, B0, ra1, rb1, A1, B1, d0, v0, v1);
#endif
#endif
#if SINK0
        r[stop ? 64 : d1] = v1;
        r[0] = 0;
#else
        r[stop || d1 == 0 ? 64 : d1] = v1;
#endif
        // the control word: cc 1-8 against a register, 9-14 against imm6, a test and its inverse
        bool imm = cc >= GVM_CC_BEQI;
        uint kk = imm ? cc - GVM_CC_BEQI : cc - 1, kind = kk >> 1;
        uint B = imm ? (kind == 2 ? cb : asuint((int)(cb << 26) >> 26)) : CB, F = kind == 1 ? 0x80000000u : 0;
        bool cond = kind == 0 ? CA == B : kind == 3 ? ((CA >> (cb & 31)) & 1) != 0 : (CA ^ F) < (B ^ F);
        bool tk = special ? sub == GVM_SUB_J : cond != ((kk & 1) != 0);
        pc = stop ? pc : tk ? tg : base + 16;
#if PREFETCH
        cur = stop ? t : tk ? tc : nx;
#else
        cur = RAM(pc >> 4);
#endif
        budget = stop ? 0 : budget - 1;
        cycle += stop ? 0 : 1;
        opsrun += stop ? 0 : n;
    }
}

uint4 state_in(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    switch (t & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_out(uint t, uint4 s0) {
    [branch] if (t < 16) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    [branch] if (t >= 24 && t < 24 + WCN / 2) {   // the write cache: two pairs a texel, a pair with no word as 0
        uint k = 2 * (t - 24);
        return uint4(occ(k) ? wc[k] : uint2(0, 0), occ(k + 1) ? wc[k + 1] : uint2(0, 0));
    }
    return t == 16 ? uint4(pc, s0.yzw) : t == 17 ? sys1 : t == 18 ? uint4(sys2.xy, flags, sys2.w) : t == 19 ? uint4(cycle, opsrun, ran, 0) : 0;
}
struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) {
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    uint core = p.y, k;
    tick_out o;
    [branch] if (core >= _NCores) { o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = 0; return o; }
    [unroll] for (k = 0; k < 16; k++) {
        uint4 s = state_in(k, core);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    uint4 s0 = state_in(16, core);
    sys1 = state_in(17, core); sys2 = state_in(18, core); sys3x = state_in(19, core);
    pc = s0.x; flags = sys2.z; cycle = sys3x.x; opsrun = sys3x.y;
    occv = 0;
    [branch] if ((flags & 4) == 0) run(_Ticks);
    ran = cycle - sys3x.x;
    o.t0 = state_out(p.x * 8 + 0, s0); o.t1 = state_out(p.x * 8 + 1, s0); o.t2 = state_out(p.x * 8 + 2, s0); o.t3 = state_out(p.x * 8 + 3, s0);
    o.t4 = state_out(p.x * 8 + 4, s0); o.t5 = state_out(p.x * 8 + 5, s0); o.t6 = state_out(p.x * 8 + 6, s0); o.t7 = state_out(p.x * 8 + 7, s0);
    return o;
}

// ---- The commit: a point a write cache pair, drawn into its texel of RAM in the draw of its word's component
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _cpad; };
uint4 state_any(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    return (t & 4) ? ((t & 2) ? v3 : v2) : ((t & 2) ? v1 : v0);
}
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint4 e = state_any(24 + (j >> 1), c);
    uint tag = (j & 1) ? e.z : e.x, word = tag - 1, tex = word >> 2;
    float2 at = (float2(tex & 2047, tex >> 11) + 0.5) / float2(2048, _RamRows);
    c2f o;
    o.vertex = tag != 0 && (word & 3) == _Comp ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = (j & 1) ? e.w : e.y;
    return o;
}
uint4 commit_frag(c2f i) : SV_Target { return i.value; }
