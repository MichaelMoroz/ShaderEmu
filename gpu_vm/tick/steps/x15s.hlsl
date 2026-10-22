// Step x15 (gpu_vm/tick/steps/README.md, "x15"): x14 on x15's encoding (gpu_vm/isa.h): the control word's path at the bottom
// (the fast loop's paths 3-7: one compare), cc as fields (a near j is beq r0, r0; one offset field, at the top), ra
// at an op word's bottom and rb at its top, the I form's sub a difference, the integer unit renumbered so that the
// fast loop's op 0 is a mid unit (0-19: sums, logic, comparisons, min, max, shifts, mul) and the rotates and
// equality go to the full step (path 1), and memory widths whose low bits are the extension's shift.
//
// Variants (gvmbench --variants; each switch at its use): WCN, FULL_ALU.
// x15s, the loops' structure and stores (gpu_vm/tick/variants/x15s.txt): S_SW (a store bundle's texels read only when a
// byte, a half or a load needs them), S_STFLAT (a store with no room by selects), S_STL (a loop of store bundles),
// S_ILOOP (a loop of integer bundles), S_CLS (a loop a class in turn, by a class number), S_NEW (the step as macros,
// none of them).
#include "../../isa.h"

#ifndef WCN
#define WCN 128
#endif
#define HALF (WCN / 2)
#define NONE 0xffffffffu
#define OP(b) ((b) << 12)   // an opc bit in an op word
Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext, _NCores; uint3 _ppad;
};
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
static uint r[65];
static uint2 wc[WCN + 1];   // (word + 1, value); 0 an empty slot; WCN a sink for a store not made
static uint pc, tp, cycle, opsrun, flags, ran, ops8;   // (tp: the fast loop's pc, in texels; ops8 its ops times 8)
static uint4 cur, sys1, sys2, sys3x;   // (the texels the step carries and does not use)

uint simm12(uint w) { return asuint((int)w >> 20); }
uint fn(uint w) { return (w >> 12) & 31; }
uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }

// ---- Integers: every result in parallel, then a tree of selects on f's bits (isa.h's numbering; DXBC's shifts take
// the low 5 bits, so a rotate's 32 - s is -y). The full unit (0-23), the mid one (0-19, the fast loop's op 0), lite
// (0-7, 16-18: op 1).
uint alu_g0(uint w, uint x, uint y) {   // 0-7
    bool f0 = (w & OP(1)) != 0, f1 = (w & OP(2)) != 0;
    uint f2v = (w >> 14) & 1, yi = y ^ (0 - f2v), sum = x + yi + f2v;   // (sub: x + ~y + 1)
    return f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
}
uint alu_g1(uint w, uint x, uint y) {   // 8-15: slt sltu min minu sge sgeu max maxu
    bool f0 = (w & OP(1)) != 0, f1 = (w & OP(2)) != 0, f2 = (w & OP(4)) != 0;
    uint F = f0 ? 0u : 0x80000000u;
    bool lt = (x ^ F) < (y ^ F);
    bool c = f2 ? !lt : lt;
    return f1 ? (c ? x : y) : (c ? 1u : 0u);
}
uint alu_lo(uint w, uint x, uint y) {   // 16-19: sll srl sra mul
    bool f0 = (w & OP(1)) != 0, f1 = (w & OP(2)) != 0;
    return f1 ? (f0 ? x * y : asuint(asint(x) >> y)) : (f0 ? x >> y : x << y);
}
uint alu(uint w, uint imm, uint a, uint b) {
    bool f0 = (w & OP(1)) != 0, f1 = (w & OP(2)) != 0, f2 = (w & OP(4)) != 0;
    uint x = a, y = (w & OP(32)) ? imm : b, ny = 0 - y;
    uint hi = f1 ? ((x == y ? 1u : 0u) ^ (f0 ? 1u : 0u)) : (f0 ? (x << y) | (x >> ny) : (x >> y) | (x << ny));   // 20-23: ror rol seq sne
    uint g2 = f2 ? hi : alu_lo(w, x, y);
    return (w & OP(16)) ? g2 : (w & OP(8)) ? alu_g1(w, x, y) : alu_g0(w, x, y);
}
uint alu_mid(uint w, uint imm, uint a, uint b) {
    uint x = a, y = (w & OP(32)) ? imm : b;
    return (w & OP(16)) ? alu_lo(w, x, y) : (w & OP(8)) ? alu_g1(w, x, y) : alu_g0(w, x, y);
}
uint alu_lite(uint w, uint imm, uint a, uint b) {
    uint x = a, y = (w & OP(32)) ? imm : b;
    bool f0 = (w & OP(1)) != 0, f1 = (w & OP(2)) != 0;
    return (w & OP(16)) ? (f1 ? asuint(asint(x) >> y) : (f0 ? x >> y : x << y)) : alu_g0(w, x, y);
}

// ---- Floats: the I form's operand is its imm12 as a float's top bits (or the long immediate)
uint fnan(float v) { uint u = asuint(v); return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u; }
bool nan_u(uint u) { return (u & 0x7fffffffu) > 0x7f800000u; }
uint fb(uint w, uint imm, bool L, uint b) { return (w & OP(32)) ? (L ? imm : (w >> 20) << 20) : b; }
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
    uint f = fn(w), v;
    [branch] if (f >= GVM_F_DIV) {   // the special functions, behind a branch
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
        uint g2 = (f & 4) ? fnan(wh) : f1 ? (f0 ? tou : toi) : (f0 ? asuint((float)ra) : itof(ra));
        v = (f & 16) ? g2 : (f & 8) ? g1 : fsum(f, ra, rb, rc);
    }
    return v;
}

// ---- Memory: the write cache holds word w in slot h or h + HALF, filled in that order, never emptied in a pass
uint wc_h(uint w) { return (w ^ (w >> 6)) & (HALF - 1); }
uint mem_addr(uint w, uint imm, uint a, uint b) { return a + ((w & OP(16)) ? b << (w >> 20) : imm); }
// a byte or half by a shift left and one right by k = 32 - its bits: the width's low bits times 8 (isa.h)
uint ext_k(uint w) { return (w >> 9) & 24; }
uint extend(uint w, uint word, uint ad) {
    uint k = ext_k(w), x = (word >> ((ad & 3) * 8)) << k;
    return (w & OP(4)) ? x >> k : asuint(asint(x) >> k);
}
uint merge(uint w, uint word, uint ad, uint val) {   // a store's value into its word
    uint sh = (ad & 3) * 8, msk = (0xffffffffu >> ext_k(w)) << sh;
    return (word & ~msk) | ((val << sh) & msk);
}
// one memory op read at once: its address, tag (word + 1), slot, texel and both entries
struct mop { uint ad, tag, h; uint4 q; uint2 ea, eb; };
mop mread(uint w, uint imm, uint a, uint b) {
    mop m;
    m.ad = mem_addr(w, imm, a, b); m.tag = (m.ad >> 2) + 1; m.h = wc_h(m.tag - 1);
    m.q = RAM(m.ad >> 4); m.ea = wc[m.h]; m.eb = wc[m.h + HALF];
    return m;
}
uint seen(mop m) { return m.ea.x == m.tag ? m.ea.y : m.eb.x == m.tag ? m.eb.y : pick(m.q, (m.tag - 1) & 3); }
uint slot(mop m) {   // where a store of it goes: its own entry, else an empty one (NONE: no room)
    return (m.ea.x == m.tag || m.ea.x == 0) ? m.h : (m.eb.x == m.tag || m.eb.x == 0) ? m.h + HALF : NONE;
}

// ---- The control word's test (cc's bits, isa.h): a comparison (less, signed by bit 2), equality, a bit, or addbnz's
// sum (0, the inverse its sense); against imm6 when bit 3
uint taken(uint ctl, uint CA, uint CB, uint i6, uint sum) {   // (i6 the rb field signed, sum CA + i6)
    bool im = (ctl & 0x400) != 0;
    uint Bc = im ? i6 : CB, F = (ctl << 22) & 0x80000000u;
    bool lt = (CA ^ F) < (Bc ^ F);
    uint o = im ? (CA >> Bc) & 1 : (sum == 0 ? 1u : 0u);
    uint c = (ctl & 0x100) ? (lt ? 1u : 0u) : (ctl & 0x200) ? (CA == Bc ? 1u : 0u) : o;
    return (ctl & 0x80) ? (c == 0 ? 1u : 0u) : c;
}

// ---- The full step (x13's: every path, ops in order, each seeing the one before)
// a load's value, or a store made (false: no room, nothing done): sl the slot it wrote, was what it held
bool mdo(uint w, mop m, uint val, out uint v, out uint sl, out uint2 was) {
    uint word = seen(m);
    v = extend(w, word, m.ad);
    sl = NONE; was = 0;
    bool ok = true;
    [branch] if (w & OP(8)) {   // a store: into its word as this core sees it
        sl = slot(m);
        ok = sl != NONE;
        [branch] if (ok) { was = sl == m.h ? m.ea : m.eb; wc[sl] = uint2(m.tag, merge(w, word, m.ad, val)); }
    }
    return ok;
}
#define WR(d, v) r[(d) == 0 ? 64 : (d)] = v
#define FWD(d, v) A1 = ((d) != 0 && ra1 == (d)) ? (v) : A1; B1 = ((d) != 0 && rb1 == (d)) ? (v) : B1
// a load or a store, then a lite op, a float sum, or another load or store: false when a store found no room
// (nothing done; op 0 undone when it was op 1's)
bool body_mem(uint4 t, uint i0, uint i1, bool L1, bool two, bool fl1, uint A0, uint B0, uint ra1, uint rb1, inout uint A1, inout uint B1,
              inout uint d0, inout uint d1, out uint v0, out uint v1) {
    mop q0 = mread(t.x, i0, A0, B0), q1 = q0;
    [branch] if (two) q1 = mread(t.y, i1, A1, B1);
    uint old0 = r[d0 == 0 ? 64 : d0], sl0, sl1;
    uint2 was0, was1;
    bool st0 = (t.x & OP(8)) != 0, ok = true;
    v1 = 0;
    [branch] if (!mdo(t.x, q0, r[d0], v0, sl0, was0)) ok = false;   // (a store's value is r[rd])
    else {
        d0 = st0 ? 0 : d0;   // a store's rd is its value, not a result
        WR(d0, v0);
        FWD(d0, v0);
        [branch] if (two) {
            [branch] if (st0 && q1.h == q0.h) q1 = mread(t.y, i1, A1, B1);   // op 0's store shares its slots
            [branch] if (!mdo(t.y, q1, r[d1], v1, sl1, was1)) {
                [branch] if (sl0 != NONE) wc[sl0] = was0;
                r[d0 == 0 ? 64 : d0] = old0;
                ok = false;
            }
            d1 = (t.y & OP(8)) ? 0 : d1;
        } else if (fl1) v1 = fsum(fn(t.y), A1, fb(t.y, i1, L1, B1), r[GVM_RC(t.y)]);
        else v1 = alu_lite(t.y, i1, A1, B1);
    }
    return ok;
}
bool step() {   // false: the pass ends (a halt, or a store with no room)
    uint base = pc & ~15u;
    uint4 t = cur;
    uint ctl = t.w, n = GVM_N(ctl), path = ctl & 7, cc = GVM_CC(ctl);
    uint tg = base + asuint(GVM_OFF9(ctl)) * 16;
    uint4 nx = RAM((base >> 4) + 1), tc = RAM(tg >> 4);
    bool ok = path != GVM_PATH_GENERAL;   // (the assembler's word that a path's bundle is one the loop makes)
    flags |= ok ? 0u : 4u;   // halted
    bool stop = !ok, L0 = (ctl & 0x20) != 0, L1 = (ctl & 0x40) != 0;
    uint i0 = L0 ? t.z : simm12(t.x), i1 = L1 ? t.z : simm12(t.y);
    uint d0 = GVM_RD(t.x), d1 = GVM_RD(t.y);
    uint ra0 = GVM_RA(t.x), rb0 = GVM_RB(t.x), ra1 = GVM_RA(t.y), rb1 = GVM_RB(t.y);
    uint ca = GVM_CRA(ctl), cb = GVM_CRB(ctl);
    // every read first: the branch's as the bundle found them; op 0's result reaches op 1 by selects
    uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
    uint v0 = 0, v1 = 0;
    bool fl1 = path == GVM_PATH_MEMF || (path == GVM_PATH_ALU && (t.y & OP(0xc0)) == OP(0xc0));   // (op 1 of class 3: a float)
    bool in0 = path == GVM_PATH_INT || (path == GVM_PATH_ALU && (t.x & OP(0xc0)) == OP(0x40));      // (op 0 an integer op)
    [branch] if (ok && in0) {
        v0 = alu(t.x, i0, A0, B0);
        WR(d0, v0);
        FWD(d0, v0);
        v1 = fl1 ? fsum(fn(t.y), A1, fb(t.y, i1, L1, B1), r[GVM_RC(t.y)]) : alu_lite(t.y, i1, A1, B1);
    } else if (ok && path != GVM_PATH_ALU) {
        stop = !body_mem(t, i0, i1, L1, path >= GVM_PATH_LOAD && (path & 1) != 0, fl1, A0, B0, ra1, rb1, A1, B1, d0, d1, v0, v1);
    } else if (ok) {
        v0 = fpu(t.x, A0, fb(t.x, i0, L0, B0), r[GVM_RC(t.x)]);
        WR(d0, v0);
        FWD(d0, v0);
        v1 = fl1 ? fsum(fn(t.y), A1, fb(t.y, i1, L1, B1), r[GVM_RC(t.y)]) : alu_lite(t.y, i1, A1, B1);
    }
    r[stop || d1 == 0 ? 64 : d1] = v1;
    uint i6 = asuint(GVM_CIMM6(ctl)), sum = CA + i6;
    bool tk = cc != GVM_CC_SPECIAL && taken(ctl, CA, CB, i6, sum) != 0;   // (cc 0 is path 0's: halted above)
    r[stop || cc != GVM_CC_ADDBNZ ? 64 : ca] = sum;   // after the ops' writes: it wins
    pc = stop ? pc : tk ? tg : base + 16;
    cur = stop ? t : tk ? tc : nx;
    cycle += stop ? 0 : 1;
    opsrun += stop ? 0 : n;
    return !stop;
}

#if !(S_NEW || S_SW || S_STL || S_ILOOP || S_STFLAT || S_CLS)
// ---- The fast loop's step: paths 3-7
void fstep(inout uint budget) {
    uint4 t = cur;
    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;   // (n * 8)
    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;
    uint4 nx = RAM(tn), tc = RAM(tg);
    uint i0 = (ctl & 0x20) ? t.z : simm12(t.x), i1 = (ctl & 0x40) ? t.z : simm12(t.y);
    uint d0 = (t.x >> 6) & 63, d1 = (t.y >> 6) & 63, s0 = d0 != 0 ? d0 : 64, s1 = d1 != 0 ? d1 : 64;
    uint ra0 = t.x & 63, rb0 = t.x >> 26, ra1 = t.y & 63, rb1 = t.y >> 26;
    uint ca = (ctl >> 11) & 63, cb = ctl >> 26;
    uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
    uint v0, v1;
    [branch] if (path == GVM_PATH_INT) {
#if FULL_ALU   // (a probe: the whole unit, 0-23, as x14's fast loop had it)
        v0 = alu(t.x, i0, A0, B0);
#else
        v0 = alu_mid(t.x, i0, A0, B0);
#endif
        r[s0] = v0;
        v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
    } else [branch] if (path < GVM_PATH_STORE) {   // loads: op 1 a load (its address as the bundle found it) or lite
        mop m0 = mread(t.x, i0, A0, B0), m1 = mread(t.y, i1, A1, B1);
        v0 = extend(t.x, seen(m0), m0.ad);
        r[s0] = v0;
        v1 = (ctl & 1) ? extend(t.y, seen(m1), m1.ad) : alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
    } else {   // op 0 a store, op 1 lite, a load or a store: op 1 sees op 0's store by selects; both slots known before
        // either is written
        bool two = (ctl & 1) != 0, st1 = two && (t.y & OP(8)) != 0;
        mop m0 = mread(t.x, i0, A0, B0), m1 = mread(t.y, i1, A1, B1);
        uint w0 = seen(m0), sl0 = slot(m0);
        uint2 e0 = uint2(m0.tag, merge(t.x, w0, m0.ad, r[d0]));
        m1.ea = sl0 == m1.h ? e0 : m1.ea;
        m1.eb = sl0 == m1.h + HALF ? e0 : m1.eb;
        uint w1 = seen(m1), sl1 = slot(m1);
        v1 = two ? extend(t.y, w1, m1.ad) : alu_lite(t.y, i1, A1, B1);
        [branch] if (sl0 == NONE || (st1 && sl1 == NONE)) {   // no room: the bundle again, in the next pass
            tg = tp; tc = t; tn = tp; nx = t; ca = 64;
            s1 = 64; budget = 1; cycle--; n8 = 0;
        } else {
            wc[sl0] = e0;
            wc[st1 ? sl1 : WCN] = uint2(m1.tag, merge(t.y, w1, m1.ad, r[d1]));
        }
        s1 = st1 ? 64 : s1;
    }
    r[s1] = v1;
    uint i6 = asuint((int)ctl >> 26), sum = CA + i6;
    uint tk = taken(ctl, CA, CB, i6, sum);
    r[(ctl & 0x780) != (GVM_CC_ADDBNZ << 7) ? 64 : ca] = sum;   // after the ops' writes: it wins
    tp = tk ? tg : tn;
    cur = tk ? tc : nx;
    budget--;
    ops8 += n8;
}
bool fast_path(uint4 t) { return (t.w & 7) >= GVM_PATH_FAST; }   // paths 3-7
void run(uint budget) {
    cur = RAM(pc >> 4);
    [loop] while (budget > 0) {
        uint b0 = budget;
        tp = pc >> 4;
        ops8 = 0;
        [loop] while (budget > 0 && fast_path(cur)) fstep(budget);
        cycle += b0 - budget;
        opsrun += ops8 >> 3;
        pc = b0 != budget ? tp << 4 : pc;
        [branch] if (budget > 0) budget = step() ? budget - 1 : 0;
    }
}
#else
// ---- x15s: the fast loop's structure. The fast step's parts are x15's, as macros: fstep makes any of paths 3-7,
// fstep_i, _l and _s one class each (a loop of its own decided it).
#define FDEC \
    uint4 t = cur; \
    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18; \
    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1; \
    uint4 nx = RAM(tn), tc = RAM(tg); \
    uint i0 = (ctl & 0x20) ? t.z : simm12(t.x), i1 = (ctl & 0x40) ? t.z : simm12(t.y); \
    uint d0 = (t.x >> 6) & 63, d1 = (t.y >> 6) & 63, s0 = d0 != 0 ? d0 : 64, s1 = d1 != 0 ? d1 : 64; \
    uint ra0 = t.x & 63, rb0 = t.x >> 26, ra1 = t.y & 63, rb1 = t.y >> 26; \
    uint ca = (ctl >> 11) & 63, cb = ctl >> 26; \
    uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb]; \
    uint v0, v1;
#define FINT \
    v0 = alu_mid(t.x, i0, A0, B0); \
    r[s0] = v0; \
    v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
#define FLOAD { \
    mop m0 = mread(t.x, i0, A0, B0), m1 = mread(t.y, i1, A1, B1); \
    v0 = extend(t.x, seen(m0), m0.ad); \
    r[s0] = v0; \
    v1 = (ctl & 1) ? extend(t.y, seen(m1), m1.ad) : alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1); }
#if S_SW   // (variant: a store bundle reads its texels of RAM only when an op needs the word: a byte or half store, or a
           // load; a word's store is its value whatever the word held, so no wait for the texture)
#define SREAD \
    mop m0 = mread_e(t.x, i0, A0, B0), m1 = mread_e(t.y, i1, A1, B1); \
    [branch] if (((t.x & OP(3)) | (two ? (t.y ^ OP(8)) & OP(11) : 0)) != 0) { m0.q = RAM(m0.ad >> 4); m1.q = RAM(m1.ad >> 4); }
#else
#define SREAD mop m0 = mread(t.x, i0, A0, B0), m1 = mread(t.y, i1, A1, B1);
#endif
#if S_STFLAT   // (variant: no room by selects, no branch)
#define SFULL \
    bool full = sl0 == NONE || (st1 && sl1 == NONE); \
    wc[full ? WCN : sl0] = e0; \
    wc[st1 && !full ? sl1 : WCN] = uint2(m1.tag, merge(t.y, w1, m1.ad, r[d1])); \
    tg = full ? tp : tg; tc = full ? t : tc; tn = full ? tp : tn; nx = full ? t : nx; ca = full ? 64 : ca; \
    s1 = full ? 64 : s1; budget = full ? 1 : budget; cycle -= full ? 1 : 0; n8 = full ? 0 : n8;
#else
#define SFULL \
    [branch] if (sl0 == NONE || (st1 && sl1 == NONE)) { \
        tg = tp; tc = t; tn = tp; nx = t; ca = 64; \
        s1 = 64; budget = 1; cycle--; n8 = 0; \
    } else { \
        wc[sl0] = e0; \
        wc[st1 ? sl1 : WCN] = uint2(m1.tag, merge(t.y, w1, m1.ad, r[d1])); \
    }
#endif
#define FSTORE { \
    bool two = (ctl & 1) != 0, st1 = two && (t.y & OP(8)) != 0; \
    SREAD \
    uint w0 = seen(m0), sl0 = slot(m0); \
    uint2 e0 = uint2(m0.tag, merge(t.x, w0, m0.ad, r[d0])); \
    m1.ea = sl0 == m1.h ? e0 : m1.ea; \
    m1.eb = sl0 == m1.h + HALF ? e0 : m1.eb; \
    uint w1 = seen(m1), sl1 = slot(m1); \
    v1 = two ? extend(t.y, w1, m1.ad) : alu_lite(t.y, i1, A1, B1); \
    SFULL \
    s1 = st1 ? 64 : s1; }
#define FTAIL { \
    r[s1] = v1; \
    uint i6 = asuint((int)ctl >> 26), sum = CA + i6; \
    uint tk = taken(ctl, CA, CB, i6, sum); \
    r[(ctl & 0x780) != (GVM_CC_ADDBNZ << 7) ? 64 : ca] = sum; \
    tp = tk ? tg : tn; \
    cur = tk ? tc : nx; \
    budget--; \
    ops8 += n8; }
mop mread_e(uint w, uint imm, uint a, uint b) {   // (S_SW: the entries, no texel)
    mop m;
    m.ad = mem_addr(w, imm, a, b); m.tag = (m.ad >> 2) + 1; m.h = wc_h(m.tag - 1);
    m.q = 0; m.ea = wc[m.h]; m.eb = wc[m.h + HALF];
    return m;
}
void fstep(inout uint budget) {
    FDEC
    [branch] if (path == GVM_PATH_INT) { FINT }
    else [branch] if (path < GVM_PATH_STORE) FLOAD
    else FSTORE
    FTAIL
}
void fstep_i(inout uint budget) { FDEC FINT FTAIL }
void fstep_l(inout uint budget) { FDEC FLOAD FTAIL }
void fstep_s(inout uint budget) { FDEC FSTORE FTAIL }
bool fast_path(uint4 t) { return (t.w & 7) >= GVM_PATH_FAST; }   // paths 3-7
// a loop's test: paths lo to lo + cnt - 1 (of 8), budget left; a bundle of the class with less budget than its place
// in it leaves for the full step, which makes it as well
#define GO(lo, cnt) (((cur.w & 7) - (lo)) < min(budget, (cnt)))
// the class of cur's path (0 the full step's, 1 integers, 2 loads, 3 stores), 0 with no budget left
#define CLS (budget != 0 ? (0xFAC0u >> ((cur.w & 7) * 2)) & 3 : 0u)
void run(uint budget) {
    cur = RAM(pc >> 4);
    [loop] while (budget > 0) {
        uint b0 = budget;
        tp = pc >> 4;
        ops8 = 0;
#if S_CLS   // (variant: a loop a class in turn, the class a number made after each step, 0 to leave)
        uint c = CLS;
        [loop] while (c != 0) {
            [loop] while (c == 1) { fstep_i(budget); c = CLS; }
            [loop] while (c == 2) { fstep_l(budget); c = CLS; }
            [loop] while (c == 3) { fstep_s(budget); c = CLS; }
        }
#elif S_STL || S_ILOOP   // (variants: x15's branches, the integer side a loop of integer bundles (S_ILOOP), the store
                         // side one of store bundles (S_STL))
        [loop] while (budget > 0 && fast_path(cur)) {
            uint p = cur.w & 7;
#if S_ILOOP
            [branch] if (p == GVM_PATH_INT) { [loop] do fstep_i(budget); while (GO(3u, 1u)); }
#else
            [branch] if (p == GVM_PATH_INT) fstep_i(budget);
#endif
            else [branch] if (p < GVM_PATH_STORE) fstep_l(budget);
#if S_STL
            else { [loop] do fstep_s(budget); while (GO(6u, 2u)); }
#else
            else fstep_s(budget);
#endif
        }
#else
        [loop] while (budget > 0 && fast_path(cur)) fstep(budget);
#endif
        cycle += b0 - budget;
        opsrun += ops8 >> 3;
        pc = b0 != budget ? tp << 4 : pc;
        [branch] if (budget > 0) budget = step() ? budget - 1 : 0;
    }
}
#endif

uint4 state_in(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    switch (t & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_out(uint t, uint4 s0) {
    [branch] if (t < 16) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    [branch] if (t >= 24 && t < 24 + WCN / 2) {   // the write cache: two pairs a texel, an empty slot 0
        uint k = 2 * (t - 24);
        return uint4(wc[k], wc[k + 1]);
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
    [loop] for (k = 0; k <= WCN; k++) wc[k] = 0;
    uint4 s0 = state_in(16, core);
    sys1 = state_in(17, core); sys2 = state_in(18, core); sys3x = state_in(19, core);
    pc = s0.x; flags = sys2.z; cycle = sys3x.x; opsrun = sys3x.y;
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
