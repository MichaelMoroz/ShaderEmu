// Step x21: x18 with the RISC-V tick's geometry (docs/machine/multicore.md, "The geometry"): a core's 38 state pixels are
// a block of 5 of 8 columns and 8 of 16 rows, 128 blocks a band, so no warp of the card holds pixels of two cores (in
// x18 a core was a row, and a warp's rows were 4 to 8 cores, each costing the others' work). Every loop is capped.
#include "../../isa.h"

static const uint WCN = 514;         // write cache entries: two ways of HALF sets, two overflow slots, then a sink (README "x17", the boot)
static const uint WCS = WCN - 2;     // the first overflow slot
static const uint HALF = WCS / 2;
static const uint NONE = 0xffffffffu;   // an empty entry's tag, and no slot
static const uint RAM_W = 8192;     // RAM's texture's width in texels: texel t is (t % RAM_W, t / RAM_W)
static const uint T_WC = 40;        // the state's texels (STATE.md): registers 0-15, special 16-19, latch 20-21, queue 22-39
static const uint T_Q = 24;
static const uint GEO_BLOCKS = 1;   // core c's state pixel j at ((c & 127) * 8 + j / 8, (c >> 7) * 16 + j % 8): no warp holds two cores
static const uint SPIX = (T_WC + WCN / 2 + 7) / 8;
int3 sat(uint j, uint c) { return int3((c & 127) * 8 + (j >> 3), (c >> 7) * 16 + (j & 7), 0); }
static const uint QMAX = 8;         // operations in flight, a core
static const uint OP_SLOTS = 32;    // 4 KB pieces a round, a core
static const uint OP_SPAN = 272;    // the stage's width: a piece's texels
static const uint F_WAIT = 1, F_SOFT = 2, F_HALT = 4, F_SYNC = 8, F_AGAIN = 16;   // flags
static const uint E_BUDGET = 0, E_SYNC = 1, E_WAIT = 2, E_FULL = 3, E_HALT = 5;   // why a pass ended (GVM_END_*)
Texture2D<uint4> _Ram : register(t0), _Rom : register(t1);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
Texture2D<uint4> _OpState : register(t10), _Stage : register(t11);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext0, _NCores; uint3 _ppad; uint4 _Ext[2];
};
uint4 ram(uint t) { return _Ram.Load(int3(t % RAM_W, t / RAM_W, 0)); }   // texel t of RAM
uint4 data_texel(uint ad) {   // the texel a load reads: RAM's, or the ROM's from GVM_ROM (bit 30), at the same place
    uint t = ad >> 4;
    int3 at = int3(t % RAM_W, (t / RAM_W) & 0x1fff, 0);
    uint4 rom = _Rom.Load(at), ram = _Ram.Load(at);
    return (ad & GVM_ROM) != 0 ? rom : ram;
}
uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
static uint r[65];          // the registers; r[64] takes what is written to r0
static uint2 wc[WCN + 1];   // the write cache: (word, value), NONE an empty entry; wc[WCN] a sink for a store not made
static uint pc, tp, cycle, opsrun, flags, ops8;   // (tp: the fast loop's pc, in texels; ops8 its ops times 8)
static uint4 cur;           // the bundle about to run (its path 0 when the general step must make it)
static uint thr;            // the fast loop's paths: from GVM_PATH_FAST, or GVM_PATH_INT alone (an operation in flight, a pass made again)
static uint lmax, smax;     // the largest load address (bit 30 off) and store address - GVM_DEV the fast loop made
static uint st, epc, cause, tval, tvec, scratch, ien, tcmp_lo, tcmp_hi, rnd, left, core, ext, endr;
static uint4 lat0, lat1;    // the latched operation words (GVM_OPS + 0..28)
static uint4 q[16];         // the queue: operation k is q[2k], q[2k + 1]
static uint qn, qhead, qgen, refused;
static bool irq_due;        // IE, and an interrupt pending and enabled: taken before the next whole bundle

uint op_bit(uint b) { return b << 12; }   // an opc bit in an op word
uint simm12(uint w) { return asuint((int)w >> 20); }
uint fn(uint w) { return (w >> 12) & 31; }

// ---- Integers: every result in parallel, then a tree of selects on f's bits (isa.h's numbering; DXBC's shifts take
// the low 5 bits, so a rotate's 32 - s is -y). The full unit (0-23), the mid one (0-19, the fast loop's op 0), lite
// (0-7, 16-18: op 1).
uint alu_g0(uint w, uint x, uint y) {   // 0-7
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0;
    uint f2v = (w >> 14) & 1, yi = y ^ (0 - f2v), sum = x + yi + f2v;   // (sub: x + ~y + 1)
    return f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
}
uint alu_g1(uint w, uint x, uint y) {   // 8-15: slt sltu min minu sge sgeu max maxu
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0, f2 = (w & op_bit(4)) != 0;
    uint F = f0 ? 0u : 0x80000000u;
    bool lt = (x ^ F) < (y ^ F);
    bool c = f2 ? !lt : lt;
    return f1 ? (c ? x : y) : (c ? 1u : 0u);
}
uint alu_lo(uint w, uint x, uint y) {   // 16-19: sll srl sra mul
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0;
    return f1 ? (f0 ? x * y : asuint(asint(x) >> y)) : (f0 ? x >> y : x << y);
}
uint alu(uint w, uint imm, uint a, uint b) {
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0, f2 = (w & op_bit(4)) != 0;
    uint x = a, y = (w & op_bit(32)) ? imm : b, ny = 0 - y;
    uint hi = f1 ? ((x == y ? 1u : 0u) ^ (f0 ? 1u : 0u)) : (f0 ? (x << y) | (x >> ny) : (x >> y) | (x << ny));   // 20-23: ror rol seq sne
    uint g2 = f2 ? hi : alu_lo(w, x, y);
    return (w & op_bit(16)) ? g2 : (w & op_bit(8)) ? alu_g1(w, x, y) : alu_g0(w, x, y);
}
uint alu_mid(uint w, uint imm, uint a, uint b) {
    uint x = a, y = (w & op_bit(32)) ? imm : b;
    return (w & op_bit(16)) ? alu_lo(w, x, y) : (w & op_bit(8)) ? alu_g1(w, x, y) : alu_g0(w, x, y);
}
uint alu_lite(uint w, uint imm, uint a, uint b) {
    uint x = a, y = (w & op_bit(32)) ? imm : b;
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0;
    return (w & op_bit(16)) ? (f1 ? asuint(asint(x) >> y) : (f0 ? x >> y : x << y)) : alu_g0(w, x, y);
}

// ---- Floats: the I form's operand is its imm12 as a float's top bits (or the long immediate)
uint fnan(float v) { uint u = asuint(v); return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u; }
bool nan_u(uint u) { return (u & 0x7fffffffu) > 0x7f800000u; }
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

// ---- Memory: the write cache holds a word in its set's entry h or h + HALF, filled in that order, never emptied in
// a pass; an entry's tag is the word's address >> 2
uint wc_h(uint w) { return (w ^ (w >> 6)) & (HALF - 1); }
uint pick_ad(uint4 q, uint ad) { uint2 h = (ad & 8) ? q.zw : q.xy; return (ad & 4) ? h.y : h.x; }   // the word at ad
uint mem_addr(uint w, uint imm, uint a, uint b) { return a + ((w & op_bit(16)) ? b << (w >> 20) : imm); }
// a byte or a half: a shift left and one right by k = 32 - its bits, the width's low bits times 8 (isa.h); its place
// in the word is ad << 3 (the shifts take their count's low 5 bits)
uint ext_k(uint w) { return (w >> 9) & 24; }
uint extend(uint w, uint word, uint ad) {
    uint k = ext_k(w), x = (word >> (ad << 3)) << k;
    return (w & op_bit(4)) ? x >> k : asuint(asint(x) >> k);
}
uint merge(uint w, uint word, uint ad, uint val) {   // a store's value into its word
    uint sh = ad << 3, msk = (0xffffffffu >> ext_k(w)) << sh;
    return word ^ ((word ^ (val << sh)) & msk);
}
// the fast loop does not make it: a load outside RAM and the ROM (bit 30 off: RAM's place), a store outside RAM above
// the boot area
// one memory op, read at once: its address, tag, set, texel and both entries
struct mop { uint ad, tag, h; uint4 q; uint2 ea, eb; };
mop mread(uint w, uint imm, uint a, uint b) {
    mop m;
    m.ad = mem_addr(w, imm, a, b); m.tag = m.ad >> 2; m.h = wc_h(m.tag);
    m.q = data_texel(m.ad); m.ea = wc[m.h]; m.eb = wc[m.h + HALF];
    return m;
}
mop mread_entries(uint w, uint imm, uint a, uint b) {   // the same, the texel left 0 (for the caller to read)
    mop m;
    m.ad = mem_addr(w, imm, a, b); m.tag = m.ad >> 2; m.h = wc_h(m.tag);
    m.q = 0; m.ea = wc[m.h]; m.eb = wc[m.h + HALF];
    return m;
}
uint seen(mop m) { return m.ea.x == m.tag ? m.ea.y : m.eb.x == m.tag ? m.eb.y : pick_ad(m.q, m.ad); }   // its word as this core sees it
uint slot(mop m) {   // where a store of it goes: its own entry, else an empty one (NONE: no room)
    return (m.ea.x == m.tag || m.ea.x == NONE) ? m.h : (m.eb.x == m.tag || m.eb.x == NONE) ? m.h + HALF : NONE;
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

// ---- The fast loop's step: paths 3-7
void fstep(inout uint budget) {
    uint4 t = cur;
    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;   // (n * 8)
    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;
    uint4 nx = ram(tn), tc = ram(tg);
    uint i0 = (ctl & 0x20) ? t.z : simm12(t.x), i1 = (ctl & 0x40) ? t.z : simm12(t.y);
    uint d0 = (t.x >> 6) & 63, d1 = (t.y >> 6) & 63, s0 = d0 != 0 ? d0 : 64, s1 = d1 != 0 ? d1 : 64;
    uint ra0 = t.x & 63, rb0 = t.x >> 26, ra1 = t.y & 63, rb1 = t.y >> 26;
    uint ca = (ctl >> 11) & 63, cb = ctl >> 26;
    uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
    uint v0, v1;
    [branch] if (path != GVM_PATH_INT) {   // memory: one op and lite first (real code's), then the pairs
        [branch] if ((path & 1) != 0) {   // a load or a store, and a lite op (paths 3 and 5)
            bool st0 = (t.x & op_bit(8)) != 0;
            mop m0 = mread_entries(t.x, i0, A0, B0);
            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);   // (a load, a byte or a half, a word not at its start)
            smax = max(smax, st0 ? m0.ad - GVM_DEV : 0); lmax = max(lmax, st0 ? 0 : m0.ad & ~GVM_ROM);
            uint w0 = seen(m0), sl0 = slot(m0);
            v0 = extend(t.x, w0, m0.ad);
            bool nr = st0 && sl0 == NONE;
            wc[st0 ? (nr ? WCS : sl0) : WCN] = uint2(m0.tag, merge(t.x, w0, m0.ad, r[d0]));
            cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            s0 = st0 ? 64 : s0;
            r[s0] = v0;
            v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
        } else [branch] if (path == GVM_PATH_LOAD2) {   // two loads
            mop m0 = mread(t.x, i0, A0, B0), m1 = mread(t.y, i1, A1, B1);
            lmax = max(lmax, max(m0.ad & ~GVM_ROM, m1.ad & ~GVM_ROM));
            v0 = extend(t.x, seen(m0), m0.ad);
            r[s0] = v0;
            v1 = extend(t.y, seen(m1), m1.ad);
        } else {   // a store, then a load or a store that sees it by selects (path 6)
            bool st1 = (t.y & op_bit(8)) != 0;
            mop m0 = mread_entries(t.x, i0, A0, B0), m1 = mread_entries(t.y, i1, A1, B1);
            // the texels only when an op needs the old word (a byte or a half, a word not at its word's start, or a load):
            // a word's store is its value
            [branch] if (((t.x & op_bit(3)) | ((t.y ^ op_bit(8)) & op_bit(11)) | ((m0.ad | m1.ad) & 3)) != 0) { m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad); }
            smax = max(smax, max(m0.ad - GVM_DEV, st1 ? m1.ad - GVM_DEV : 0)); lmax = max(lmax, st1 ? 0 : m1.ad & ~GVM_ROM);
            uint w0 = seen(m0), sl0 = slot(m0);
            uint2 e0 = uint2(m0.tag, merge(t.x, w0, m0.ad, r[d0]));
            m1.ea = sl0 == m1.h ? e0 : m1.ea;
            m1.eb = sl0 == m1.h + HALF ? e0 : m1.eb;
            uint w1 = seen(m1), sl1 = slot(m1);
            w1 = sl0 == NONE && m0.tag == m1.tag ? e0.y : w1;   // (op 0's store in an overflow slot)
            v1 = extend(t.y, w1, m1.ad);
            bool nr = sl0 == NONE || (st1 && sl1 == NONE);   // no room: the overflow slots, and the pass ends here
            wc[sl0 == NONE ? WCS : sl0] = e0;
            wc[st1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN] = uint2(m1.tag, merge(t.y, w1, m1.ad, r[d1]));
            cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            s1 = st1 ? 64 : s1;
        }
    } else {   // integers: op 0 the mid unit, op 1 lite (op 0's result to it by selects)
        v0 = alu_mid(t.x, i0, A0, B0);
        r[s0] = v0;
        v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
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

// ---- Paths 1 and 2 (op 0 of the whole integer unit, sel, lpc or a float op; or a load or store; then lite or float lite),
// a whole bundle with any control word but eret: x16's full step, both ops' registers read first. A load or store is
// noted as the fast loop's. jr and callr take rs after the ops, the add from rs as the bundle found it (isa.h).
uint fb(uint w, uint imm, bool L, uint b) { return (w & op_bit(32)) ? (L ? imm : (w >> 20) << 20) : b; }
uint step12(uint budget) {   // the budget left: 0 when a store found no room (nothing made; the bundle again next pass)
    uint4 t = cur;
    uint base = pc & ~15u, ctl = t.w, n = GVM_N(ctl), path = ctl & 7;
    uint tg = base + asuint(GVM_OFF9(ctl)) * 16;
    bool L0 = (ctl & 0x20) != 0, L1 = (ctl & 0x40) != 0, ok = true;
    uint i0 = L0 ? t.z : simm12(t.x), i1 = L1 ? t.z : simm12(t.y);
    uint d0 = GVM_RD(t.x), d1 = GVM_RD(t.y), ra1 = GVM_RA(t.y), rb1 = GVM_RB(t.y);
    uint A0 = r[GVM_RA(t.x)], B0 = r[GVM_RB(t.x)], A1 = r[ra1], B1 = r[rb1], CA = r[GVM_CRA(ctl)], CB = r[GVM_CRB(ctl)], v0 = 0;
    bool fl1 = (t.y & op_bit(0xc0)) == op_bit(0xc0);   // (op 1 of class 3: a float; else lite)
    uint opc0 = (t.x >> 12) & 255, C0 = r[GVM_RC(t.x)], cs0 = r[GVM_CRS(ctl)];
    [branch] if (path == GVM_PATH_MEMF) {   // a load or a store into its word as this core sees it, then a float sum
        mop m = mread(t.x, i0, A0, B0);
        bool stv = (t.x & op_bit(8)) != 0;
        smax = max(smax, stv ? m.ad - GVM_DEV : 0); lmax = max(lmax, stv ? 0 : m.ad & ~GVM_ROM);
        uint word = seen(m), sl = slot(m);
        v0 = extend(t.x, word, m.ad);
        ok = !stv || sl != NONE;
        wc[stv && ok ? sl : WCN] = uint2(m.tag, merge(t.x, word, m.ad, r[d0]));
        d0 = stv ? 0 : d0;
    } else [branch] if (opc0 == GVM_OP_I(GVM_I_SEL) || opc0 == GVM_X_LPC) v0 = opc0 == GVM_X_LPC ? base + i0 : C0 != 0 ? A0 : B0;
    else v0 = (opc0 >> 6) == 1 ? alu(t.x, i0, A0, B0) : fpu(t.x, A0, fb(t.x, i0, L0, B0), C0);
    [branch] if (!ok) return 0;
    r[d0 == 0 ? 64 : d0] = v0;
    A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
    uint v1 = fl1 ? fsum(fn(t.y), A1, fb(t.y, i1, L1, B1), r[GVM_RC(t.y)]) : alu_lite(t.y, i1, A1, B1);
    r[d1 == 0 ? 64 : d1] = v1;
    uint i6 = asuint(GVM_CIMM6(ctl)), sum = CA + i6;
    r[GVM_CC(ctl) != GVM_CC_ADDBNZ ? 64 : GVM_CRA(ctl)] = sum;   // after the ops' writes: it wins
    pc = taken(ctl, CA, CB, i6, sum) != 0 ? tg : base + 16;
    [branch] if (GVM_CC(ctl) == GVM_CC_SPECIAL) {   // jr, callr (rs after the ops), j, call, the add (rs as found)
        uint sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), to = r[rs] & ~15u;
        pc = sub == GVM_SUB_JR || sub == GVM_SUB_CALLR ? to : sub == GVM_SUB_J || sub == GVM_SUB_CALL ? base + asuint(GVM_OFF18(ctl)) * 16 : base + 16;
        r[sub == GVM_SUB_CALL || sub == GVM_SUB_CALLR ? 1 : 64] = base + 16;
        r[sub == GVM_SUB_ADD ? rs : 64] = cs0 + asuint(GVM_CIMM12(ctl));
    }
    cur = ram(pc >> 4);
    cycle++;
    opsrun += n;
    return budget - 1;
}

// ---- Traps, special registers, interrupts
uint ipend() {
    bool due = (tcmp_lo | tcmp_hi) != 0 && (_TimeHi > tcmp_hi || (_TimeHi == tcmp_hi && _TimeLo >= tcmp_lo));
    return ((flags & F_SOFT) ? 1u : 0u) | (due ? 2u : 0u) | (ext << 2);
}
void trap(uint c, uint at, uint val) {
    epc = at; cause = c; tval = val;
    st = (st & GVM_ST_S ? GVM_ST_PS : 0) | (st & GVM_ST_IE ? GVM_ST_PIE : 0) | GVM_ST_S;
    pc = tvec & ~15u;
    flags &= ~F_WAIT;
}
uint sr_read(uint n) {
    [branch] if (n == GVM_SR_RANDOM) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; }
    uint4 a = uint4(st, epc, cause, tval), b = uint4(tvec, scratch, ien, ipend()), c = uint4(tcmp_lo, tcmp_hi, _TimeLo, _TimeHi);
    uint4 d = uint4(cycle, core, _Player, rnd);
    uint v = n < 4 ? pick(a, n) : n < 8 ? pick(b, n) : n < 12 ? pick(c, n) : n < 16 ? pick(d, n) : n == GVM_SR_LEFT ? left : opsrun + 1;   // (OPS counts the op reading it)
    return v;
}
void sr_write(uint n, uint v) {
    st = n == GVM_SR_STATUS ? (v & 7) | (st & GVM_ST_S) : st;
    epc = n == GVM_SR_EPC ? v : epc; cause = n == GVM_SR_CAUSE ? v : cause; tval = n == GVM_SR_TVAL ? v : tval;
    tvec = n == GVM_SR_TVEC ? v : tvec; scratch = n == GVM_SR_SCRATCH ? v : scratch; ien = n == GVM_SR_IEN ? v : ien;
    flags = n == GVM_SR_IPEND ? (flags & ~F_SOFT) | ((v & 1) ? F_SOFT : 0) : flags;
    tcmp_lo = n == GVM_SR_TCMP_LO ? v : tcmp_lo; tcmp_hi = n == GVM_SR_TCMP_HI ? v : tcmp_hi;
}

// ---- The operations: a descriptor's two texels, and the reference's ranges (what it writes, dn bytes at its dst;
// what it reads, sn at its src)
struct opd { uint kind, dst, src, n, ds, ss, row, rows, dn, sn; };
opd op_of(uint4 a, uint4 b) {
    opd o;
    o.kind = a.x; o.dst = a.y; o.src = a.z; o.n = a.w; o.ds = b.x; o.ss = b.y; o.row = b.z; o.rows = b.w;
    bool blit = o.kind == GVM_OP_BLIT && o.rows != 0 && o.row != 0;
    o.dn = o.kind == GVM_OP_COPY || o.kind == GVM_OP_FILL ? o.n : blit ? o.ds * (o.rows - 1) + o.row : 0;
    o.sn = o.kind == GVM_OP_COPY ? o.n : blit ? o.ss * (o.rows - 1) + o.row : 0;
    return o;
}
bool in_range(uint a, uint by, uint lo, uint n) { return n != 0 && (a - lo < n || lo - a < by); }
bool op_waits(uint a, uint by, bool store) {   // an operation in flight holds it: the pass ends before the op
    [branch] if (a >= GVM_OPS) return store && a == GVM_OPS && by == 16 && qn == QMAX;
    bool w = false;
    [loop] for (uint k = 0; k < min(qn, QMAX); k++) {
        opd o = op_of(q[2 * k], q[2 * k + 1]);
        w = w || in_range(a, by, o.dst, o.dn) || (store && in_range(a, by, o.src, o.sn));
    }
    return w;
}

// ---- The general step's memory: any address, any alignment
uint word_seen(uint a) {   // a readable word's value as this core sees it
    uint v = pick_ad(data_texel(a), a), tag = a >> 2, h = wc_h(tag);
    uint2 ea = wc[h], eb = wc[h + HALF];
    return a >= GVM_ROM ? v : ea.x == tag ? ea.y : eb.x == tag ? eb.y : v;
}
bool readable(uint a) { return a < _RamEnd || (a >= GVM_ROM && a < GVM_ROM_END); }
bool load_any(uint a, uint by, out uint v) {   // by 1, 2 or 4; false: a fault
    v = 0;
    [branch] if (a >= GVM_OPS) {   // in flight, refused, the latched words
        uint o = a - GVM_OPS;
        v = o == 0 ? qn : o == 4 ? refused : o < 16 ? pick(lat0, o >> 2) : pick(lat1, (o >> 2) & 3);
        return o <= 32 - by && ((a | by) & 3) == 0;
    }
    bool ok = readable(a) && readable(a + by - 1);
    [branch] if (ok) {
        uint sh = (a & 3) * 8, lo = word_seen(a & ~3u), hi = 0;
        [branch] if ((a & 3) + by > 4) hi = word_seen((a & ~3u) + 4);
        uint w = sh != 0 ? (lo >> sh) | (hi << (32 - sh)) : lo;
        v = by == 4 ? w : w & ((1u << (by * 8)) - 1);
    }
    return ok;
}
bool store_ok(uint a, uint by) {
    return a >= GVM_OPS ? a - GVM_OPS <= 32 - by && ((a | by) & 3) == 0 : a >= GVM_DEV && a < _RamEnd && a + by - 1 < _RamEnd;
}
uint word_of(uint4 d, uint k) { return k < 4 ? pick(d, k) : 0; }
bool store_any(uint a, uint by, uint4 d) {   // by bytes of d at a, into the write cache; false: no room, nothing made
    uint o = a & 3, nw = (o + by + 3) >> 2, k, sl[5];
    uint2 was[5];
    bool ok = true;
    [loop] for (k = 0; k < min(nw, 5u) && ok; k++) {
        uint tag = (a >> 2) + k, h = wc_h(tag);
        uint2 ea = wc[h], eb = wc[h + HALF];
        uint s = (ea.x == tag || ea.x == NONE) ? h : (eb.x == tag || eb.x == NONE) ? h + HALF : NONE;
        ok = s != NONE;
        [branch] if (ok) {
            uint2 e = wc[s];
            uint old = e.x == tag ? e.y : pick_ad(ram(tag >> 2), tag << 2);
            uint lo = k == 0 ? o : 0, hi = min(4u, o + by - 4 * k);
            uint m = (hi >= 4 ? 0xffffffffu : (1u << (8 * hi)) - 1) & ~((1u << (8 * lo)) - 1);
            uint val = o != 0 ? (word_of(d, k) << (8 * o)) | (k != 0 ? word_of(d, k - 1) >> (32 - 8 * o) : 0) : word_of(d, k);
            sl[k] = s; was[k] = e;
            wc[s] = uint2(tag, old ^ ((old ^ val) & m));
        }
    }
    [branch] if (!ok) [loop] for (k = k - 1; k > 0; k--) wc[sl[k - 1]] = was[k - 1];   // (undone, last first)
    return ok;
}

// ---- The general step: one op of any class at slot s. 0: done; 1: trapped; 2: the pass ends after it (counted);
// 3: the pass ends before it (not counted, pc at it)
uint ext_op(uint opc, uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;   // the high word of a * b, by halves
    uint ll = al * bl, lh = al * bh, hl = ah * bl, mid = (ll >> 16) + (lh & 0xffff) + (hl & 0xffff);
    uint hu = ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    bool sg = opc == GVM_X_DIV || opc == GVM_X_REM, na = sg && (a >> 31) != 0, nb = sg && (b >> 31) != 0;
    uint ua = na ? 0 - a : a, ub = nb ? 0 - b : b, qt = ub == 0 ? 0xffffffffu : ua / ub, m = ub == 0 ? ua : ua - qt * ub;
    bool rem = opc == GVM_X_REM || opc == GVM_X_REMU;
    uint dv = b == 0 ? (rem ? a : 0xffffffffu) : rem ? (na ? 0 - m : m) : (na != nb ? 0 - qt : qt);
    uint mh = opc == GVM_X_MULHU ? hu : hu - ((a >> 31) ? b : 0) - (opc == GVM_X_MULH && (b >> 31) ? a : 0);
    uint bits = opc == GVM_X_CLZ ? (a == 0 ? 32u : 31u - firstbithigh(a)) : opc == GVM_X_CTZ ? (a == 0 ? 32u : firstbitlow(a))
              : opc == GVM_X_POPC ? countbits(a) : opc == GVM_X_BSWAP ? (a >> 24 | ((a >> 8) & 0xff00u) | ((a << 8) & 0xff0000u) | a << 24)
              : reversebits(a);
    return opc <= GVM_X_MULHSU ? mh : opc <= GVM_X_REMU ? dv : bits;
}
uint bfmask(uint imm) { uint len = ((imm >> 5) & 31) + 1; return len >= 32 ? 0xffffffffu : (1u << len) - 1; }
uint mem_any(uint w, uint imm, uint a, uint b, uint epcw, inout uint e, inout bool ill) {
    uint opc = (w >> 12) & 255, mode = (opc >> 3) & 7, wd = opc & 7, rd = (w >> 6) & 63;
    [branch] if (mode <= 3) {
        uint by = wd == GVM_W_W ? 4u : wd == GVM_W_D ? 8u : wd == GVM_W_Q ? 16u : wd == 5 ? 0u : (wd & 1) ? 1u : 2u;
        ill = by == 0;
        uint ad = a + (mode >= 2 ? b << ((w >> 20) & 31) : imm);
        bool store = (mode & 1) != 0;
        by = by <= 4 ? min(by, 4 - (ad & 3)) : by;   // (a byte, a half or a word stays in its word)
        [branch] if (ill) return 0;
        [branch] if (op_waits(ad, by, store)) { pc = epcw; e = E_SYNC; return 3; }
        [branch] if (!store) {
            uint4 v = 0;
            bool ok = true;
            [loop] for (uint k = 0; k < clamp(by >> 2, 1u, 4u) && ok; k++) { uint x; ok = load_any(ad + 4 * k, min(by, 4u), x); v = put(v, k, x); }
            [branch] if (!ok) { trap(GVM_CAUSE_LOAD, epcw, ad); return 1; }
            v.x = wd == GVM_W_B ? asuint(asint(v.x << 24) >> 24) : wd == GVM_W_H ? asuint(asint(v.x << 16) >> 16) : v.x;
            [loop] for (uint j = 0; j < clamp(by >> 2, 1u, 4u); j++) { uint x = (rd + j) & 63; r[x == 0 ? 64 : x] = pick(v, j); }
            return 0;
        }
        [branch] if (!store_ok(ad, by)) { trap(GVM_CAUSE_STORE, epcw, ad); return 1; }
        uint4 d = uint4(r[rd], r[(rd + 1) & 63], r[(rd + 2) & 63], r[(rd + 3) & 63]);
        [branch] if (ad >= GVM_OPS) {   // the latched words; a 16-byte store at GVM_OPS asks for the operation
            [loop] for (uint k = 0; k < min(by >> 2, 4u); k++) {
                uint i = ((ad - GVM_OPS) >> 2) + k;
                lat0 = i < 4 ? put(lat0, i, pick(d, k)) : lat0; lat1 = i >= 4 ? put(lat1, i & 3, pick(d, k)) : lat1;
            }
            [branch] if (ad == GVM_OPS && by == 16) { q[2 * qn] = lat0; q[2 * qn + 1] = lat1; qn++; thr = GVM_PATH_INT; }
            return 0;
        }
        [branch] if (!store_any(ad, by, d)) { pc = epcw; e = E_FULL; return 3; }
        return 0;
    }
    ill = !(mode == GVM_M_AMO || (mode == GVM_M_AMO2 && wd <= GVM_AMO2_MAXU));
    [branch] if (ill) return 0;
    [branch] if ((a & 3) != 0 || !store_ok(a, 4) || a >= GVM_OPS) { trap(GVM_CAUSE_STORE, epcw, a); return 1; }
    [branch] if (op_waits(a, 4, true)) { pc = epcw; e = E_SYNC; return 3; }
    uint old = word_seen(a), x = r[rd];
    bool lt = asint(old) < asint(b), ltu = old < b;
    uint nw = mode == GVM_M_AMO2 ? (wd == GVM_AMO2_MINU ? (ltu ? old : b) : (ltu ? b : old))
            : wd == GVM_AMO_SWAP ? b : wd == GVM_AMO_ADD ? old + b : wd == GVM_AMO_AND ? (old & b) : wd == GVM_AMO_OR ? (old | b)
            : wd == GVM_AMO_XOR ? (old ^ b) : wd == GVM_AMO_MIN ? (lt ? old : b) : wd == GVM_AMO_MAX ? (lt ? b : old) : (old == x ? b : old);
    [branch] if (!store_any(a, 4, uint4(nw, 0, 0, 0))) { pc = epcw; e = E_FULL; return 3; }
    r[rd == 0 ? 64 : rd] = old;
    return 0;
}
uint op_any(uint w, uint imm, bool L, uint base, uint s, inout uint e) {
    uint opc = (w >> 12) & 255, rd = (w >> 6) & 63, rc = (w >> 20) & 63, cls = opc >> 6, f = opc & 31, epcw = base | (s * 4);
    uint a = r[w & 63], b = r[w >> 26], c = r[rc], v = 0, res = 0;
    bool I = (opc & 32) != 0, ill = false, wb = true, super = (st & GVM_ST_S) != 0;
    [branch] if (cls == 1) {   // integer: the unit, then the R form's carries, shadd, sel, and the I form's bfe
        uint cin = c & 1, s1 = a + b, s2 = s1 + cin, d1 = a - b;
        uint x = f == GVM_I_SHADD ? a + (b << rc) : f == GVM_I_SEL ? (c != 0 ? a : b) : f == GVM_I_ADDCY ? s2
               : f == GVM_I_CARRY ? ((s1 < a || s2 < s1) ? 1u : 0u) : f == GVM_I_SUBBY ? d1 - cin : ((a < b || d1 < cin) ? 1u : 0u);
        uint bx = a >> (imm & 31), m = bfmask(imm), bv = bx & m, top = ~m & (0 - ((bv & ~(m >> 1)) != 0 ? 1u : 0u));
        ill = I ? f > GVM_I_COMMON_LAST && f != GVM_I_BFE : f > GVM_I_BORROW;
        v = f <= GVM_I_COMMON_LAST ? alu(w, imm, a, b) : I ? bv | ((imm & 0x400) ? top : 0) : x;
    } else [branch] if (cls == 3) {   // float
        ill = I && (f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB);
        v = fpu(w, a, I ? (L ? imm : (w >> 20) << 20) : b, c);
    } else [branch] if (cls == 2) {
        res = mem_any(w, imm, a, b, epcw, e, ill);
        wb = false;
    } else {   // extended
        wb = opc <= GVM_X_LPC || (opc >= GVM_X_MFSR && opc != GVM_X_MTSR);
        uint m = bfmask(imm) << (imm & 31), n = imm & 0xfff;
        v = opc <= GVM_X_BREV ? ext_op(opc, a, b) : opc == GVM_X_BFI ? (r[rd] & ~m) | ((a << (imm & 31)) & m) : base + imm;
        ill = opc == GVM_X_ILL || opc == 0x0f || (opc > GVM_X_FENCE && opc < GVM_X_MFSR) || opc > GVM_X_SRCLR || (opc >= GVM_X_MFSR && n >= GVM_SR_COUNT);
        [branch] if (ill) {
        } else if (opc == GVM_X_SYSCALL) { trap(GVM_CAUSE_SYSCALL, epcw, 0); res = 1; }
        else if (opc == GVM_X_BREAK) {
            [branch] if (_Flags & 1) { pc = epcw; flags |= F_HALT; e = E_HALT; res = 3; }
            else { trap(GVM_CAUSE_BREAK, epcw, 0); res = 1; }
        } else if (opc == GVM_X_WAIT) {   // a user's: the pass's end; a supervisor's: until an interrupt is pending
            pc = epcw + 4;
            bool go = super && (ipend() & ien) != 0;
            flags |= super && !go ? F_WAIT : 0;
            e = E_WAIT; res = go ? 0 : 2;
        } else if (opc == GVM_X_SYNC) { pc = epcw + 4; flags |= F_SYNC; e = E_SYNC; res = 2; }
        else if (opc >= GVM_X_MFSR) {
            [branch] if (!super && (opc != GVM_X_MFSR || n < GVM_SR_USER_FIRST)) { trap(GVM_CAUSE_PRIV, epcw, w); res = 1; }
            else {
                v = sr_read(n);
                [branch] if (opc != GVM_X_MFSR) sr_write(n, opc == GVM_X_SRSET ? v | a : opc == GVM_X_SRCLR ? v & ~a : a);
            }
        }
    }
    [branch] if (ill) { trap(GVM_CAUSE_ILLEGAL, epcw, w); res = 1; }
    opsrun += res == 0 || res == 2 ? 1 : 0;
    r[res != 0 || !wb || rd == 0 ? 64 : rd] = v;
    return res;
}
// The bundle at pc from its word on; NONE when the core goes on, else why the pass ends
uint bundle() {
    uint base = pc & ~15u, s = (pc >> 2) & 3, e = NONE, res = 0;
    [branch] if (base >= _RamEnd) { trap(GVM_CAUSE_FETCH, pc, pc); return NONE; }
    uint4 t = ram(base >> 4);
    uint ctl = t.w, n = GVM_N(ctl);
    cycle++;
    [branch] if (n == 3) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }
    uint ca0 = r[GVM_CRA(ctl)], cb0 = r[GVM_CRB(ctl)], cs0 = r[GVM_CRS(ctl)];   // as this run of the bundle begins
    [loop] for (; s < n && res == 0; s++) {
        uint w = s != 0 ? t.y : t.x;
        bool L = ((ctl >> (5 + s)) & 1) != 0;
        res = op_any(w, L ? t.z : simm12(w), L, base, s, e);
    }
    [branch] if (res == 1) return NONE;
    [branch] if (res != 0) { pc = ((pc >> 2) & 3) >= n ? base | 12 : pc; return e; }
    uint cc = GVM_CC(ctl), sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), next = base + 16, i6 = asuint(GVM_CIMM6(ctl)), sum = ca0 + i6;
    bool bad = cc == GVM_CC_SPECIAL ? (sub == GVM_SUB_ILLEGAL || (sub == GVM_SUB_ADD && rs == 0)) : cc == GVM_CC_ADDBNZ && GVM_CRA(ctl) == 0;
    [branch] if (bad) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }
    [branch] if (cc == GVM_CC_SPECIAL) {
        uint to_far = base + asuint(GVM_OFF18(ctl)) * 16, to = r[rs] & ~15u;
        [branch] if (sub == GVM_SUB_ERET) {
            [branch] if ((st & GVM_ST_S) == 0) { trap(GVM_CAUSE_PRIV, base | 12, ctl); return NONE; }
            next = epc;
            st = (st & GVM_ST_PIE ? GVM_ST_IE : 0) | (st & GVM_ST_PS ? GVM_ST_S : 0) | GVM_ST_PIE;
        }
        next = sub == GVM_SUB_JR || sub == GVM_SUB_CALLR ? to : sub == GVM_SUB_J || sub == GVM_SUB_CALL ? to_far : next;
        r[sub == GVM_SUB_CALLR || sub == GVM_SUB_CALL ? 1 : 64] = base + 16;
        r[sub == GVM_SUB_ADD ? rs : 64] = cs0 + asuint(GVM_CIMM12(ctl));
    } else {
        r[cc == GVM_CC_ADDBNZ ? GVM_CRA(ctl) : 64] = sum;
        next = taken(ctl, ca0, cb0, i6, sum) != 0 ? base + asuint(GVM_OFF9(ctl)) * 16 : next;
    }
    pc = next;
    return NONE;
}
// The general step: an interrupt if one is due, then a bundle; the budget left, 0 when the pass ends
uint general(uint budget) {
    [branch] if (irq_due && (pc & 15) == 0) {
        uint p = ipend() & ien;
        trap(GVM_CAUSE_IRQ | ((p & 4) ? 2u : (p & 2) ? 1u : 0u), pc, 0);
    }
    left = budget - 1;
    uint e = bundle();
    irq_due = (st & GVM_ST_IE) != 0 && (ipend() & ien) != 0;
    cur = ram(pc >> 4);
    cur.w &= (pc & 15) != 0 || irq_due ? ~7u : 0xffffffffu;   // (the fast loop makes whole bundles, and takes no interrupt)
    endr = e != NONE ? e : endr;
    return e != NONE ? 0 : budget - 1;
}
void run(uint budget) {
    [branch] if (flags & F_HALT) { endr = E_HALT; return; }
    [branch] if ((flags & F_SYNC) != 0 && qn != 0) { endr = E_SYNC; return; }   // a sync waits for the operations
    flags &= ~F_SYNC;
    [branch] if ((flags & F_WAIT) != 0 && (ipend() & ien) == 0) { endr = E_WAIT; return; }
    flags &= ~F_WAIT;
    irq_due = (st & GVM_ST_IE) != 0 && (ipend() & ien) != 0;
    cur = ram(pc >> 4);
    cur.w &= (pc & 15) != 0 || irq_due ? ~7u : 0xffffffffu;
    uint guard = budget + 8;   // a hard cap on the trips of both loops, whatever the budget does
    [loop] while (budget > 0 && guard > 0) {
        uint b0 = budget;
        guard--;
        tp = pc >> 4;
        ops8 = 0;
        [loop] while (budget > 0 && guard > 0 && (cur.w & 7) >= thr) { fstep(budget); guard--; }   // paths 3-7 (or 7)
        cycle += b0 - budget;
        opsrun += ops8 >> 3;
        pc = b0 != budget ? tp << 4 : pc;
        uint path = cur.w & 7;   // (HLSL makes both sides of ?: : an if)
        [branch] if (budget > 0 && (path == GVM_PATH_ALU || path == GVM_PATH_MEMF) && thr == GVM_PATH_FAST) budget = step12(budget);
        else [branch] if (budget > 0) budget = general(budget);
    }
}

uint4 state_in(uint t, uint c) {
    int3 at = sat(t >> 3, c);
    switch (t & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_out(uint t, uint4 s3) {
    [branch] if (t < 16) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    [branch] if (t >= T_WC && t < T_WC + WCN / 2) {   // the write cache: two entries a texel
        uint k = 2 * (t - T_WC);
        return uint4(wc[k], wc[k + 1]);
    }
    [branch] if (t >= T_Q && t < T_WC) return q[t - T_Q];
    uint4 s16 = uint4(pc, st, epc, cause), s17 = uint4(tval, tvec, scratch, ien), s18 = uint4(tcmp_lo, tcmp_hi, flags, rnd);
    uint4 s19 = uint4(cycle, opsrun, cycle - s3.x, endr == E_BUDGET && cycle - s3.x < _Ticks && (flags & F_HALT) == 0 ? E_FULL : endr);
    uint4 s22 = uint4(qn, qhead, qgen, refused);
    return t == 16 ? s16 : t == 17 ? s17 : t == 18 ? s18 : t == 19 ? s19 : t == 20 ? lat0 : t == 21 ? lat1 : t == 22 ? s22 : 0;
}
uint4 again_out(uint t, uint c) {   // the state as the pass found it, to be made again: no store, F_AGAIN
    uint4 v = state_in(t, c);
    return t >= T_WC ? uint4(NONE, 0, NONE, 0) : t == 18 ? uint4(v.xy, v.z | F_AGAIN, v.w) : t == 19 ? uint4(v.xyz, E_BUDGET) : v;
}
struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) {
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
void load_state(uint c) {   // a core's state as the pass found it
    uint k;
    [unroll] for (k = 0; k < 16; k++) {
        uint4 s = state_in(k, c);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    [loop] for (k = 0; k <= WCN; k++) wc[k] = uint2(NONE, 0);
    uint4 s16 = state_in(16, c), s17 = state_in(17, c), s18 = state_in(18, c), s19 = state_in(19, c);
    uint4 s22 = state_in(22, c), os = _OpState.Load(int3(c, 0, 0));
    pc = s16.x; st = s16.y; epc = s16.z; cause = s16.w;
    tval = s17.x; tvec = s17.y; scratch = s17.z; ien = s17.w;
    tcmp_lo = s18.x; tcmp_hi = s18.y; flags = s18.z; rnd = s18.w;
    cycle = s19.x; opsrun = s19.y; endr = E_BUDGET;
    lat0 = state_in(20, c); lat1 = state_in(21, c);
    qn = min(s22.x, QMAX); qhead = s22.y; qgen = s22.z; refused = s22.w;
    [unroll] for (k = 0; k < 16; k++) q[k] = state_in(T_Q + k, c);
    [branch] if (os.x == qgen) {   // the last pass's rounds: operations through, pieces made of the next, refused
        uint d = min(os.y, qn);
        [loop] for (k = 0; k + d < qn; k++) { q[2 * k] = q[2 * (k + d)]; q[2 * k + 1] = q[2 * (k + d) + 1]; }
        qn -= d; qhead = os.z; refused += os.w;
    }
    qgen++;
}
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    tick_out o;
    core = (p.y >> 4) * 128 + (p.x >> 3);
    uint j = (p.x & 7) * 8 + (p.y & 15);
    [branch] if (core >= _NCores || j >= SPIX || (p.y & 15) >= 8) { o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = 0; return o; }
    ext = (pick(_Ext[core >> 7], (core >> 5) & 3) >> (core & 31)) & 1;
    load_state(core);
    thr = (flags & F_AGAIN) != 0 || qn != 0 ? GVM_PATH_INT : GVM_PATH_FAST;   // (a pass made again: memory in the general step)
    flags &= ~F_AGAIN;
    lmax = 0; smax = 0;
    run(_Ticks);
    bool again = lmax >= _RamEnd || smax >= _RamEnd - GVM_DEV;   // one it should not have made
    uint4 s19 = state_in(19, core);
    o.t0 = state_out(j * 8 + 0, s19); o.t1 = state_out(j * 8 + 1, s19); o.t2 = state_out(j * 8 + 2, s19); o.t3 = state_out(j * 8 + 3, s19);
    o.t4 = state_out(j * 8 + 4, s19); o.t5 = state_out(j * 8 + 5, s19); o.t6 = state_out(j * 8 + 6, s19); o.t7 = state_out(j * 8 + 7, s19);
    [branch] if (again) {   // a load or store the fast loop does not make: this pass is made again in the next, as it began
        uint t = j * 8;
        o.t0 = again_out(t, core); o.t1 = again_out(t + 1, core); o.t2 = again_out(t + 2, core); o.t3 = again_out(t + 3, core);
        o.t4 = again_out(t + 4, core); o.t5 = again_out(t + 5, core); o.t6 = again_out(t + 6, core); o.t7 = again_out(t + 7, core);
    }
    return o;
}

// ---- The commit: a point a write cache entry, drawn into its texel of RAM in the draw of its word's component
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _cpad; };
uint4 state_any(uint t, uint c) {
    int3 at = sat(t >> 3, c);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    return (t & 4) ? ((t & 2) ? v3 : v2) : ((t & 2) ? v1 : v0);
}
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint4 e = state_any(T_WC + (j >> 1), c);
    uint word = (j & 1) ? e.z : e.x, tex = word >> 2;
    float2 at = (float2(tex % RAM_W, tex / RAM_W) + 0.5) / float2(RAM_W, _RamRows);
    c2f o;
    o.vertex = word != NONE && (word & 3) == _Comp ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = (j & 1) ? e.w : e.y;
    return o;
}
uint4 commit_frag(c2f i) : SV_Target { return i.value; }

// ---- The operations, in rounds of three draws: stage (each piece's texels as they will be), write (into RAM), progress
// (_OpState: the queue's generation, operations through, pieces made of the next, refused). A piece is up to 4 KB within
// one 4 KB block of its destination; a round makes up to OP_SLOTS of them a core, in the queue's order, each operation
// whole before the next starts, and the next only if it is independent of those before it in the round.
bool op_ok(opd o) {   // the reference's checks (gvm__range)
    uint de = o.dst + o.dn - 1, se = o.src + o.sn - 1;
    bool dok = o.dn == 0 || (de >= o.dst && o.dst >= GVM_DEV && de < _RamEnd);
    bool sok = o.sn == 0 || (se >= o.src && (se < _RamEnd || (o.src >= GVM_ROM && se < GVM_ROM_END)));
    return (o.kind == GVM_OP_COPY || o.kind == GVM_OP_FILL || o.kind == GVM_OP_BLIT) && dok && sok;
}
uint op_per_row(opd o) { return o.kind == GVM_OP_BLIT ? ((o.row + 4095) >> 12) + 1 : 0; }
uint op_segs(opd o) {
    return o.kind == GVM_OP_BLIT ? (o.dn != 0 ? o.rows * op_per_row(o) : 0) : o.n == 0 ? 0 : ((o.dst + o.n - 1) >> 12) - (o.dst >> 12) + 1;
}
bool overlap(uint a, uint an, uint b, uint bn) { return an != 0 && bn != 0 && a < b + bn && b < a + an; }
bool op_seq(opd o) {   // a blit whose rows share texels or read what it writes: a piece a round, in order
    return o.kind == GVM_OP_BLIT && o.dn != 0 && ((o.rows > 1 && o.ds < o.row + 15) || overlap(o.dst, o.dn, o.src, o.sn));
}
bool conflict(opd o, opd e) {   // o reads what e writes, or o and e write one texel
    uint ot = o.dst >> 4, et = e.dst >> 4;
    return overlap(o.src, o.sn, e.dst, e.dn) || overlap(ot, o.dn != 0 ? ((o.dst + o.dn - 1) >> 4) - ot + 1 : 0, et, e.dn != 0 ? ((e.dst + e.dn - 1) >> 4) - et + 1 : 0);
}
void op_piece(opd o, uint g, out uint d0, out uint d1, out uint s0) {   // piece g: bytes [d0, d1) from s0
    bool blit = o.kind == GVM_OP_BLIT, back = o.kind == GVM_OP_COPY && o.dst > o.src && o.dst - o.src < o.n;
    uint P = blit ? op_per_row(o) : op_segs(o), rw = blit ? g / P : 0, j = blit ? g % P : back ? P - 1 - g : g;
    uint dr = o.dst + rw * o.ds, sr = o.src + rw * o.ss, L = blit ? o.row : o.n, A = dr & ~4095u;
    d0 = max(dr, A + j * 4096); d1 = max(d0, min(dr + L, A + (j + 1) * 4096)); s0 = sr + (d0 - dr);
}
opd op_at(uint c, uint k) { return op_of(state_any(T_Q + 2 * k, c), state_any(T_Q + 2 * k + 1, c)); }
struct plan_t { uint d, p, rf, k, g; bool hit; };
plan_t plan(uint c, uint want) {   // core c's round: slot want's operation and piece, and the progress after the round
    uint4 qs = state_any(22, c), os = _OpState.Load(int3(c, 0, 0));
    bool fresh = os.x != qs.z;
    plan_t P;
    P.d = fresh ? 0 : os.y; P.p = fresh ? qs.y : os.z; P.rf = fresh ? 0 : os.w; P.k = 0; P.g = 0; P.hit = false;
    uint used = 0, first = P.d;
    bool stop = c >= _NCores;
    [loop] for (uint k = P.d; k < min(qs.x, QMAX) && !stop; k++) {
        opd o = op_at(c, k);
        bool ok = op_ok(o), indep = true;
        [loop] for (uint i = first; i < min(k, QMAX) && ok; i++) { opd e = op_at(c, i); indep = indep && !(op_ok(e) && conflict(o, e)); }
        [branch] if (!ok) { P.rf++; P.d = k + 1; P.p = 0; }
        else [branch] if (!indep) stop = true;
        else {
            uint S = op_segs(o), rem = S - P.p, take = min(op_seq(o) ? min(rem, 1u) : rem, OP_SLOTS - used);
            [flatten] if (want - used < take) { P.hit = true; P.k = k; P.g = P.p + want - used; }
            used += take; P.p += take;
            stop = P.p < S || used == OP_SLOTS;
            [flatten] if (P.p >= S) { P.d = k + 1; P.p = 0; }
        }
    }
    return P;
}
struct o2f { float4 vertex : SV_POSITION; nointerpolation uint4 j0 : J0; nointerpolation uint4 j1 : J1; };
float4 corner(uint vid, float x0, float x1, float y0, float y1, float w, float h, bool on) {   // a quad, six vertices
    uint v = vid % 6;
    float x = (v == 1 || v == 3 || v == 4) ? x1 : x0, y = (v == 2 || v == 4 || v == 5) ? y1 : y0;
    return on ? float4(x / w * 2 - 1, 1 - y / h * 2, 0, 1) : float4(-2, -2, 0, 1);
}
void slot_piece(uint inst, out opd o, out uint d0, out uint d1, out uint s0, out bool on) {
    plan_t P = plan(inst / OP_SLOTS, inst % OP_SLOTS);
    o = op_at(inst / OP_SLOTS, P.k);
    op_piece(o, P.g, d0, d1, s0);
    on = P.hit && d1 > d0;
}
o2f op_stage_vert(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    opd o; uint d0, d1, s0; bool on;
    slot_piece(inst, o, d0, d1, s0, on);
    uint cnt = ((d1 - 1) >> 4) - (d0 >> 4) + 1, sh = (o.dst & 3) * 8, dl = d0 - s0;
    o2f r;
    r.vertex = corner(vid, 0, (float)cnt, (float)inst, (float)(inst + 1), (float)OP_SPAN, (float)(_NCores * OP_SLOTS), on);
    r.j0 = uint4(d0, d1, s0, o.kind);
    r.j1 = uint4(sh != 0 ? (o.src << sh) | (o.src >> (32 - sh)) : o.src, o.kind == GVM_OP_BLIT && dl != 0 && dl < d1 - d0 ? dl : 0, 0, 0);
    return r;
}
uint src_byte(uint a) { return (pick_ad(data_texel(a), a) >> ((a & 3) * 8)) & 0xffu; }
uint4 op_stage_frag(o2f i) : SV_Target {
    uint d0 = i.j0.x, d1 = i.j0.y, s0 = i.j0.z, t = (d0 >> 4) + (uint)i.vertex.x, k;
    uint lo = max(d0, t * 16) - t * 16, hi = min(d1, t * 16 + 16) - t * 16, sa = s0 + (t * 16 - d0);
    uint4 old = ram(t), nv = i.j1.x;   // a fill: its word turned to the destination's bytes
    [branch] if (i.j0.w != GVM_OP_FILL) {
        [branch] if (i.j1.y != 0) {   // a blit's row that reads what it writes: a byte at a time, as made in order
            [loop] for (k = 0; k < 16; k++) {
                uint b = t * 16 + k - d0, v = src_byte(s0 + b % i.j1.y);
                nv = put(nv, k >> 2, (pick(nv, k >> 2) & ~(0xffu << (8 * (k & 3)))) | (v << (8 * (k & 3))));
            }
        } else {   // two source texels, their bytes moved into place
            uint4 a = data_texel(sa), b = data_texel(sa + 16);
            uint w[8] = { a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w };
            uint o = sa & 15, sh = (o & 3) * 8;
            [unroll] for (k = 0; k < 4; k++) {
                uint x = w[(o >> 2) + k], y = w[(o >> 2) + k + 1];
                nv = put(nv, k, sh != 0 ? (x >> sh) | (y << (32 - sh)) : x);
            }
        }
    }
    [unroll] for (k = 0; k < 4; k++) {   // the bytes [lo, hi) of the texel are the piece's
        uint l = clamp((int)lo - 4 * (int)k, 0, 4), h = clamp((int)hi - 4 * (int)k, 0, 4);
        uint m = (h >= 4 ? 0xffffffffu : (1u << (8 * h)) - 1) & ~(l >= 4 ? 0xffffffffu : (1u << (8 * l)) - 1);
        old = put(old, k, pick(old, k) ^ ((pick(old, k) ^ pick(nv, k)) & m));
    }
    return old;
}
o2f op_write_vert(uint vid : SV_VertexID, uint inst : SV_InstanceID) {   // two quads: the piece's texels on two rows
    opd o; uint d0, d1, s0; bool on;
    slot_piece(inst, o, d0, d1, s0, on);
    uint t0 = d0 >> 4, t1 = (d1 - 1) >> 4, x0 = t0 % RAM_W, y0 = t0 / RAM_W, xe = x0 + t1 - t0 + 1;
    bool second = vid >= 6;
    o2f r;
    r.vertex = corner(vid, second ? 0 : (float)x0, (float)(second ? xe - RAM_W : min(xe, RAM_W)), (float)(y0 + (second ? 1 : 0)),
                      (float)(y0 + (second ? 2 : 1)), (float)RAM_W, (float)_RamRows, on && (!second || xe > RAM_W));
    r.j0 = uint4(t0, t1, inst, 0);
    r.j1 = 0;
    return r;
}
uint4 op_write_frag(o2f i) : SV_Target {
    uint2 px = uint2(i.vertex.xy);
    uint t = px.y * RAM_W + px.x;
    if (t < i.j0.x || t > i.j0.y) discard;
    return _Stage.Load(int3(t - i.j0.x, i.j0.z, 0));
}
struct p2f { float4 vertex : SV_POSITION; nointerpolation uint4 v : V; };
p2f op_prog_vert(uint c : SV_VertexID) {
    plan_t P = plan(c, NONE);
    p2f o;
    o.vertex = float4((float)(2 * c + 1) / (float)_NCores - 1, 0, 0, 1);   // (fxc2 took (float)c's bits for c in the loads)
    o.v = uint4(state_any(22, c).z, P.d, P.p, P.rf);
    return o;
}
uint4 op_prog_frag(p2f i) : SV_Target { return i.v; }
