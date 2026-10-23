// v0009.hlsl for Unity, made by gpu_vm/platform/unity_tick.py: do not edit
// A tick made by gpu_vm/tick/builder/gen.py (the manifest beside it has its parameters); not edited by hand.
#include "isa.cginc"   // isa.h 1805f4006457

static const uint NB = 16;           // buckets of four entries a table
static const uint WCN = 130;         // write cache entries: 8 * NB in buckets, two overflow slots, then a sink
static const uint WCS = WCN - 2;
static const uint MV = 0;   // (mv: the move on a full pair of buckets)
static bool mvok = false;   // (mv: on for a store's lookup whose store fills the slot it frees: a free way must not hide its bucket's texel in the other)
static const uint OV = 0, OVB = 8 * NB;   // overflow entries from OVB, searched when both buckets are full
static const uint NONE = 0xffffffffu;
static const uint RAM_W = 8192;
static const uint T_WC = 40;
static const uint T_Q = 24;
static const uint T_WT = T_WC + WCN;
static const uint NTEX = 203;
static const uint SPIX = 26;
// the core's file (phase 2): XR words from texel T_XF, their tags from T_XT (XBIND), NPAIR pairs from T_PB (their count
// texel 23.x); the commit draws CPTS points a core: the write cache's, the file's slots (XBIND), the pairs
static const uint XR = 0;
static const uint XBIND = 0;
static const uint XFAST = 0;
static const uint NCLOAD = 0;   // GVM_W_NC loads (no look at this pass's stores)
static const uint THIN = 0;   // one op a bundle (the assembler's max_ops 1): the fast loop has no op 1
static const uint THIN8 = 0;   // 8-byte instructions (gvm_machine.thin)
static const uint WIDE3 = 0;   // three ops a bundle (gvm_machine.wide3)
static const uint WSH = 0;
static const uint WAD = 7;
static const uint WAITW = 0x00c01100;   // (WAD 5) the kernel's REST mask: 8 words, bit k = worker k (core 0 waits for its job), one pass after a rest   // a worker's budget: core 0's >> WSH; WAD: core 0's last pass's when it ended on sync or full
static const uint FX1 = 0;   // op 1's float functions: fpu's but the special ones (GvmAsm::fx1)
static const uint LATEBR = 0;   // branches read their registers after the bundle's ops (gvm_machine.late)
static const uint XLAZY = 0;
static const uint NPAIR = 0;
static const uint T_XF = 203;
static const uint T_XT = 203;
static const uint T_PB = 203;
static const uint T_VR = 203;
static const uint VEC = 0;   // 128-bit registers (gvm_machine.vec; needs rpk=wide)
static const uint CPTS = 130;
// the layout, as gvmbench reads it: core c's state pixel j at (slot * GEO_BW + jx, band * GEO_BAND + jy), its band c / GEO_PER,
// its slot in the band GEO_PERM's (0 the same, 1 bits reversed, 2 times GEO_MUL), jx, jy by GEO_JROW
static const uint GEO_BLOCKS = 1;
static const uint GEO_BW = 8;
static const uint GEO_BH = 4;
static const uint GEO_BAND = 16;
static const uint GEO_PER = 128;
static const uint GEO_JROW = 0;
static const uint GEO_PERM = 0;
static const uint GEO_MUL = 1;
static const uint GEO_INV = 0;
uint slot_of(uint i) { return i; }
uint core_at(uint s) { return s; }
int3 sat(uint j, uint c) { return int3(slot_of(c % 128u) * 8u + (j / 4u), (c / 128u) * 16u + (j % 4u), 0); }
static const uint QMAX = 8;
static const uint OP_SLOTS = 32;
static const uint OP_SPAN = 272;
static const uint F_WAIT = 1, F_SOFT = 2, F_HALT = 4, F_SYNC = 8, F_AGAIN = 16;
static const uint E_BUDGET = 0, E_SYNC = 1, E_WAIT = 2, E_FULL = 3, E_HALT = 5;
Texture2D<uint4> _Ram, _Rom;
Texture2D<uint4> _S0, _S1, _S2, _S3;
Texture2D<uint4> _S4, _S5, _S6, _S7;
Texture2D<uint4> _OpState, _Stage;
#include "params.cginc"
uint4 ram(uint t) { return _Ram.Load(int3(t % RAM_W, t / RAM_W, 0)); }
uint4 data_texel(uint ad) {
    uint t = ad >> 4;
    int3 at = int3(t % RAM_W, (t / RAM_W) & 0x1fff, 0);
    uint4 rom = _Rom.Load(at), ram = _Ram.Load(at);
    return (ad & GVM_ROM) != 0 ? rom : ram;
}
uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
static const uint NBK = (WCN + 4) / 4;   // the tags' elements: buckets of four (the sink's too)
static uint4 R4[65];
static uint wt[(WCN + 1)];
static uint wd[(4 * (WCN + 1))];
static uint bloom;
static uint pc, tp, cycle, opsrun, flags, ops8;
static uint4 cur;
static uint thr, bud;   // (bud: this core's budget this pass)
static uint lmax, smax;
static uint st, epc, cause, tval, tvec, scratch, ien, tcmp_lo, tcmp_hi, rnd, left, core, ext, endr;
static uint4 lat0, lat1;
static uint4 q[16];
static uint qn, qhead, qgen, refused;
static bool irq_due;
static uint pn;   // this pass's pairs

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

// ---- Memory: the write cache holds a texel in an entry of its bucket in the first table or the second
bool has(uint tg, uint t, uint i) { return tg != NONE && (tg & 0x0fffffffu) == t && ((tg >> (28 + i)) & 1) != 0; }
uint stag(uint tg, uint t, uint i) { return t | (((tg == NONE ? 0u : tg >> 28) | (1u << i)) << 28); }
uint pick_ad(uint4 q, uint ad) { uint2 h = (ad & 8) ? q.zw : q.xy; return (ad & 4) ? h.y : h.x; }
uint mem_addr(uint w, uint imm, uint a, uint b) { return a + ((w & op_bit(16)) ? b << (w >> 20) : imm); }
uint ext_k(uint w) { return (w >> 9) & 24; }
uint extend(uint w, uint word, uint ad) {
    uint k = ext_k(w), x = (word >> (ad << 3)) << k;
    return (w & op_bit(4)) ? x >> k : asuint(asint(x) >> k);
}
uint merge(uint w, uint word, uint ad, uint val) {
    uint sh = ad << 3, msk = (0xffffffffu >> ext_k(w)) << sh;
    return word ^ ((word ^ (val << sh)) & msk);
}
uint wc_b1(uint t) { return (t * 0x9e3779b1u) >> 28; }
uint wc_b2(uint t) { return NB + ((t * 0x85ebca6bu + 0x165667b1u) >> 28); }
uint4 bucket(uint b) { uint k = 4 * b; return uint4(wt[k], wt[k + 1], wt[k + 2], wt[k + 3]); }
struct mop { uint ad, t, i, b1, b2; uint4 q, A, B; uint sl, otag, e, w; };
void mfind(inout mop m) {
    bool4 ka = (m.A & 0x0fffffffu) == m.t || m.A == NONE, kb = (m.B & 0x0fffffffu) == m.t || m.B == NONE;
    uint msk = (ka.x ? 1u : 0u) | (ka.y ? 2u : 0u) | (ka.z ? 4u : 0u) | (ka.w ? 8u : 0u)
             | (kb.x ? 16u : 0u) | (kb.y ? 32u : 0u) | (kb.z ? 64u : 0u) | (kb.w ? 128u : 0u);
    uint k = firstbitlow(msk);
    m.otag = k < 8 ? pick(k < 4 ? m.A : m.B, k & 3) : NONE;
    m.sl = k < 4 ? 4 * m.b1 + k : k < 8 ? 4 * m.b2 + (k & 3) : NONE;
    [branch] if (OV != 0 && k >= 8) {   // both buckets full: the texel's overflow entry, or the first free one
        uint j = OV, q;
        [loop] for (q = 0; q < OV; q++) { uint g = wt[OVB + q]; j = j == OV && ((g & 0x0fffffffu) == m.t || g == NONE) ? q : j; }
        m.otag = j < OV ? wt[OVB + j] : NONE; m.sl = j < OV ? OVB + j : NONE;
    }
    [branch] if (MV != 0 && OV == 0 && k >= 8 && mvok) {   // (mv) both full: a resident with room in its other bucket moves there, its slot is the texel's
        uint j, f = NONE, from = NONE;
        [loop] for (j = 0; j < 8 && f == NONE; j++) {
            uint u = pick(j < 4 ? m.A : m.B, j & 3) & 0x0fffffffu, ob = j < 4 ? wc_b2(u) : wc_b1(u);
            uint4 o = bucket(ob);
            uint w = o.x == NONE ? 0u : o.y == NONE ? 1u : o.z == NONE ? 2u : o.w == NONE ? 3u : 4u;
            from = w < 4 ? (j < 4 ? 4 * m.b1 + j : 4 * m.b2 + (j & 3)) : from; f = w < 4 ? 4 * ob + w : f;
        }
        [branch] if (f != NONE) {
            uint l = from | 3;   // (a bucket keeps no hole, a lookup takes its first free way: its last resident fills the moved one's way)
            wt[f] = wt[from]; wd[4 * f] = wd[4 * from]; wd[4 * f + 1] = wd[4 * from + 1]; wd[4 * f + 2] = wd[4 * from + 2]; wd[4 * f + 3] = wd[4 * from + 3];
            wt[from] = wt[l]; wd[4 * from] = wd[4 * l]; wd[4 * from + 1] = wd[4 * l + 1]; wd[4 * from + 2] = wd[4 * l + 2]; wd[4 * from + 3] = wd[4 * l + 3];
            wt[l] = NONE; m.sl = l; m.otag = NONE;
            m.A = bucket(m.b1); m.B = bucket(m.b2);
        }
    }
    m.e = has(m.otag, m.t, m.i) ? m.sl : WCN;
}
mop mlook(uint ad, bool look) {
    mop m;
    m.ad = ad; m.t = ad >> 4; m.i = (ad >> 2) & 3; m.b1 = wc_b1(m.t); m.b2 = wc_b2(m.t);
    m.q = 0; m.A = NONE; m.B = NONE;
    [branch] if (look) {
        m.A = bucket(m.b1); m.B = bucket(m.b2);
    }
    mfind(m);
    m.w = wd[4 * m.e + m.i];
    return m;
}
mop mread_entries(uint w, uint imm, uint a, uint b, bool st) {
    uint ad = mem_addr(w, imm, a, b);
    return mlook(ad, st || ((ad >> 4) & bloom) == (ad >> 4));
}
mop mread_load(uint w, uint imm, uint a, uint b) {
    mop m = mread_entries(w, imm, a, b, false);
    m.q = data_texel(m.ad);
    return m;
}
mop mread(uint w, uint imm, uint a, uint b) {
    mop m = mread_entries(w, imm, a, b, true);
    m.q = data_texel(m.ad);
    return m;
}
uint seen(mop m) { return m.e != WCN ? m.w : pick(m.q, m.i); }
uint slot(mop m) { return m.sl; }
uint slot_tag(mop m, uint sl) { return m.otag; }

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
    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;
    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;
    uint4 nx = ram(tn), tc = ram(tg);
    uint i0 = (ctl & 0x20) ? t.z : simm12(t.x), i1 = (ctl & 0x40) ? t.z : simm12(t.y);
    uint d0 = (t.x >> 6) & 63, d1 = (t.y >> 6) & 63, s0 = d0 != 0 ? d0 : 64, s1 = d1 != 0 ? d1 : 64;
    uint ra0 = t.x & 63, rb0 = t.x >> 26, ra1 = t.y & 63, rb1 = t.y >> 26;
    uint ca = (ctl >> 11) & 63, cb = ctl >> 26;
    uint A0 = R4[(ra0)].x, B0 = R4[(rb0)].x, A1 = R4[(ra1)].x, B1 = R4[(rb1)].x, CA = R4[(ca)].x, CB = R4[(cb)].x;
    uint v0, v1;
    [branch] if (path != GVM_PATH_INT) {
        [branch] if ((path & 1) != 0) {   // a load or a store, and a lite op (paths 3 and 5)
            bool st0 = (t.x & op_bit(8)) != 0;
            mvok = MV != 0 && st0; mop m0 = mread_entries(t.x, i0, A0, B0, st0); mvok = false;
            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);
            smax = max(smax, st0 ? m0.ad - GVM_DEV : 0); lmax = max(lmax, st0 ? 0 : m0.ad & ~GVM_ROM);
            uint w0 = seen(m0), sl0 = slot(m0);
            v0 = extend(t.x, w0, m0.ad);
            bool nr = st0 && sl0 == NONE;
            uint at0 = st0 ? (nr ? WCS : sl0) : WCN;
            wt[at0] = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wd[4 * at0 + m0.i] = merge(t.x, w0, m0.ad, R4[(d0)].x);
            bloom |= st0 ? m0.t : 0;
            cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            s0 = st0 ? 64 : s0;
            R4[(s0)].x = v0;
            v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
        } else [branch] if (path == GVM_PATH_LOAD2) {   // two loads
            mop m0 = mread_load(t.x, i0, A0, B0), m1 = mread_load(t.y, i1, A1, B1);
            lmax = max(lmax, max(m0.ad & ~GVM_ROM, m1.ad & ~GVM_ROM));
            v0 = extend(t.x, seen(m0), m0.ad);
            R4[(s0)].x = v0;
            v1 = extend(t.y, seen(m1), m1.ad);
        } else {   // a store, then a load or a store that sees it by selects (path 6)
            bool st1 = (t.y & op_bit(8)) != 0;
            mop m0 = mread_entries(t.x, i0, A0, B0, true), m1 = mread_entries(t.y, i1, A1, B1, true);
            [branch] if (((t.x & op_bit(3)) | ((t.y ^ op_bit(8)) & op_bit(11)) | ((m0.ad | m1.ad) & 3)) != 0) { m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad); }
            smax = max(smax, max(m0.ad - GVM_DEV, st1 ? m1.ad - GVM_DEV : 0)); lmax = max(lmax, st1 ? 0 : m1.ad & ~GVM_ROM);
            uint w0 = seen(m0), sl0 = slot(m0), s0x = sl0 == NONE ? WCS : sl0;
            uint tg0 = stag(slot_tag(m0, sl0), m0.t, m0.i);
            uint e0 = merge(t.x, w0, m0.ad, R4[(d0)].x);
            bool same = m0.t == m1.t, over = same && sl0 == NONE;
            wt[s0x] = tg0; wd[4 * s0x + m0.i] = e0;
            m1.A = (s0x >> 2) == m1.b1 ? put(m1.A, s0x & 3, tg0) : m1.A;
            m1.B = (s0x >> 2) == m1.b2 ? put(m1.B, s0x & 3, tg0) : m1.B;
            mvok = MV != 0 && st1; mfind(m1); mvok = false;
            m1.sl = over ? WCS : m1.sl; m1.otag = over ? tg0 : m1.otag; m1.e = over ? (has(tg0, m1.t, m1.i) ? WCS : WCN) : m1.e;
            m1.w = wd[4 * m1.e + m1.i];
            uint w1 = seen(m1), sl1 = m1.sl;
            v1 = extend(t.y, w1, m1.ad);
            bool nr = sl0 == NONE || (st1 && sl1 == NONE);
            uint o1 = m1.otag;
            uint at1 = st1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
            wt[at1] = stag(o1, m1.t, m1.i);
            wd[4 * at1 + m1.i] = merge(t.y, w1, m1.ad, R4[(d1)].x);
            bloom |= m0.t | (st1 ? m1.t : 0);
            cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            s1 = st1 ? 64 : s1;
        }
    } else {   // integers: op 0 the mid unit, op 1 lite
        v0 = alu_mid(t.x, i0, A0, B0);
        R4[(s0)].x = v0;
        v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
    }
    R4[(s1)].x = v1;
    uint i6 = asuint((int)ctl >> 26), sum = CA + i6;
    uint tk = taken(ctl, CA, CB, i6, sum);
    R4[((ctl & 0x780) != (GVM_CC_ADDBNZ << 7) ? 64 : ca)].x = sum;
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
    uint A0 = R4[(GVM_RA(t.x))].x, B0 = R4[(GVM_RB(t.x))].x, A1 = R4[(ra1)].x, B1 = R4[(rb1)].x, CA = R4[(GVM_CRA(ctl))].x, CB = R4[(GVM_CRB(ctl))].x, v0 = 0;
    bool fl1 = (t.y & op_bit(0xc0)) == op_bit(0xc0);   // (op 1 of class 3: a float; else lite)
    uint opc0 = (t.x >> 12) & 255, C0 = R4[(GVM_RC(t.x))].x, cs0 = R4[(GVM_CRS(ctl))].x;
    [branch] if (path == GVM_PATH_MEMF) {   // a load or a store into its word as this core sees it, then a float sum
        mop m = mread(t.x, i0, A0, B0);
        bool stv = (t.x & op_bit(8)) != 0;
        smax = max(smax, stv ? m.ad - GVM_DEV : 0); lmax = max(lmax, stv ? 0 : m.ad & ~GVM_ROM);
        uint word = seen(m), sl = slot(m);
        v0 = extend(t.x, word, m.ad);
        ok = !stv || sl != NONE;
        uint at = stv && ok ? sl : WCN;
        wt[at] = stag(slot_tag(m, sl), m.t, m.i);
        wd[4 * at + m.i] = merge(t.x, word, m.ad, R4[(d0)].x);
        bloom |= stv ? m.t : 0;
        d0 = stv ? 0 : d0;
    } else [branch] if (opc0 == GVM_OP_I(GVM_I_SEL) || opc0 == GVM_X_LPC) v0 = opc0 == GVM_X_LPC ? base + i0 : C0 != 0 ? A0 : B0;
    else [branch] if ((opc0 >> 6) == 1) v0 = alu(t.x, i0, A0, B0); else [branch] if ((opc0 & 31) <= GVM_F_MAX) v0 = fsum(opc0 & 31, A0, fb(t.x, i0, L0, B0), C0); else v0 = fpu(t.x, A0, fb(t.x, i0, L0, B0), C0);   // (s12b 3)
    [branch] if (!ok) return 0;
    R4[(d0 == 0 ? 64 : d0)].x = v0;
    A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
    uint v1;
    [branch] if (fl1) v1 = fsum(fn(t.y), A1, fb(t.y, i1, L1, B1), R4[(GVM_RC(t.y))].x); else v1 = alu_lite(t.y, i1, A1, B1);   // (s12b 2)
    R4[(d1 == 0 ? 64 : d1)].x = v1;
    [flatten] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL) {   // (late branches: the registers as the ops left them)
        uint ca = GVM_CRA(ctl), cb = GVM_CRB(ctl);
        CA = d1 != 0 && ca == d1 ? v1 : d0 != 0 && ca == d0 ? v0 : CA; CB = d1 != 0 && cb == d1 ? v1 : d0 != 0 && cb == d0 ? v0 : CB;
    }
    uint i6 = asuint(GVM_CIMM6(ctl)), sum = CA + i6;
    R4[(GVM_CC(ctl) != GVM_CC_ADDBNZ ? 64 : GVM_CRA(ctl))].x = sum;   // after the ops' writes: it wins
    pc = taken(ctl, CA, CB, i6, sum) != 0 ? tg : base + 16;
    [branch] if (GVM_CC(ctl) == GVM_CC_SPECIAL) {   // jr, callr (rs after the ops), j, call, the add (rs as found)
        uint sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), to = R4[(rs)].x & ~15u;
        pc = sub == GVM_SUB_JR || sub == GVM_SUB_CALLR ? to : sub == GVM_SUB_J || sub == GVM_SUB_CALL ? base + asuint(GVM_OFF18(ctl)) * 16 : base + 16;
        R4[(sub == GVM_SUB_CALL || sub == GVM_SUB_CALLR ? 1 : 64)].x = base + 16;
        R4[(sub == GVM_SUB_ADD ? rs : 64)].x = cs0 + asuint(GVM_CIMM12(ctl));
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
    uint v = n < 4 ? pick(a, n) : n < 8 ? pick(b, n) : n < 12 ? pick(c, n) : n < 16 ? pick(d, n) : n == GVM_SR_LEFT ? left : n == GVM_SR_XR ? XR : n == GVM_SR_NPAIR ? NPAIR : opsrun + 1;   // (OPS counts the op reading it)
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
    mop m = mlook(a, true);
    uint v = pick_ad(data_texel(a), a);
    return a >= GVM_ROM ? v : m.e != WCN ? m.w : v;
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
    uint o = a & 3, nw = (o + by + 3) >> 2, k, sl[5], wast[5], was[5], wix[5];
    bool ok = true;
    [loop] for (k = 0; k < min(nw, 5u) && ok; k++) {
        uint wi = (a >> 2) + k, t = wi >> 2, i = wi & 3;
        mop lk = mlook(wi << 2, true);
        uint s = lk.sl;
        ok = s != NONE;
        [branch] if (ok) {
            uint tg = lk.otag;
            uint e = wd[4 * s + i];
            uint old = lk.e != WCN ? lk.w : pick(ram(t), i);
            uint lo = k == 0 ? o : 0, hi = min(4u, o + by - 4 * k);
            uint m = (hi >= 4 ? 0xffffffffu : (1u << (8 * hi)) - 1) & ~((1u << (8 * lo)) - 1);
            uint val = o != 0 ? (word_of(d, k) << (8 * o)) | (k != 0 ? word_of(d, k - 1) >> (32 - 8 * o) : 0) : word_of(d, k);
            sl[k] = s; wast[k] = tg; was[k] = e; wix[k] = 4 * s + i;
            wt[s] = stag(tg, t, i); wd[4 * s + i] = old ^ ((old ^ val) & m);
            bloom |= t;
        }
    }
    [branch] if (!ok) [loop] for (k = k - 1; k > 0; k--) { wt[sl[k - 1]] = wast[k - 1]; wd[wix[k - 1]] = was[k - 1]; }   // (undone, last first)
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
uint file_any(uint w, uint imm, uint a, uint epcw, inout uint e, inout bool ill) {
    uint opc = (w >> 12) & 255, mode = (opc >> 3) & 7, k = opc & 7, rd = (w >> 6) & 63;
    bool pair = mode == GVM_M_FWRITE && k == GVM_FK_PAIR;
    ill = pair ? NPAIR == 0 : XR == 0 || (mode == GVM_M_FREAD ? k > GVM_FK_BIND : k > GVM_FK_UNBIND) || (mode == GVM_M_FREAD ? k == GVM_FK_BIND : k == GVM_FK_UNBIND);
    [branch] if (ill) return 0;
    return 0;
}

uint mem_any(uint w, uint imm, uint a, uint b, uint epcw, inout uint e, inout bool ill) {
    uint opc = (w >> 12) & 255, mode = (opc >> 3) & 7, wd = opc & 7, rd = (w >> 6) & 63;
   // the core's file (gen.py's xcode)
    [branch] if (mode <= 3) {
        uint by = wd == GVM_W_W ? 4u : wd == GVM_W_D ? 8u : wd == GVM_W_Q ? 16u : wd == GVM_W_NC ? (NCLOAD != 0 && (mode & 1) == 0 ? 4u : 0u) : (wd & 1) ? 1u : 2u;
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
            [loop] for (uint j = 0; j < clamp(by >> 2, 1u, 4u); j++) { uint x = (rd + j) & 63; R4[(x == 0 ? 64 : x)].x = pick(v, j); }
            return 0;
        }
        [branch] if (!store_ok(ad, by)) { trap(GVM_CAUSE_STORE, epcw, ad); return 1; }
        uint4 d = uint4(R4[(rd)].x, R4[((rd + 1) & 63)].x, R4[((rd + 2) & 63)].x, R4[((rd + 3) & 63)].x);
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
    uint old = word_seen(a), x = R4[(rd)].x;
    bool lt = asint(old) < asint(b), ltu = old < b;
    uint nw = mode == GVM_M_AMO2 ? (wd == GVM_AMO2_MINU ? (ltu ? old : b) : (ltu ? b : old))
            : wd == GVM_AMO_SWAP ? b : wd == GVM_AMO_ADD ? old + b : wd == GVM_AMO_AND ? (old & b) : wd == GVM_AMO_OR ? (old | b)
            : wd == GVM_AMO_XOR ? (old ^ b) : wd == GVM_AMO_MIN ? (lt ? old : b) : wd == GVM_AMO_MAX ? (lt ? b : old) : (old == x ? b : old);
    [branch] if (!store_any(a, 4, uint4(nw, 0, 0, 0))) { pc = epcw; e = E_FULL; return 3; }
    R4[(rd == 0 ? 64 : rd)].x = old;
    return 0;
}
uint op_any(uint w, uint imm, bool L, uint base, uint s, inout uint e) {
    uint opc = (w >> 12) & 255, rd = (w >> 6) & 63, rc = (w >> 20) & 63, cls = opc >> 6, f = opc & 31, epcw = base | (s * 4);
    uint a = R4[(w & 63)].x, b = R4[(w >> 26)].x, c = R4[(rc)].x, v = 0, res = 0;
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
        v = opc <= GVM_X_BREV ? ext_op(opc, a, b) : opc == GVM_X_BFI ? (R4[(rd)].x & ~m) | ((a << (imm & 31)) & m) : base + imm;
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
    R4[(res != 0 || !wb || rd == 0 ? 64 : rd)].x = v;
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
    uint ca0 = R4[(GVM_CRA(ctl))].x, cb0 = R4[(GVM_CRB(ctl))].x, cs0 = R4[(GVM_CRS(ctl))].x;   // as this run of the bundle begins
    [loop] for (; s < n && res == 0; s++) {
        uint w = s != 0 ? t.y : t.x;
        bool L = ((ctl >> (5 + s)) & 1) != 0;
        res = op_any(w, L ? t.z : simm12(w), L, base, s, e);
    }
    [branch] if (res == 1) return NONE;
    [branch] if (res != 0) { pc = ((pc >> 2) & 3) >= n ? base | 12 : pc; return e; }
    [branch] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL) { ca0 = R4[(GVM_CRA(ctl))].x; cb0 = R4[(GVM_CRB(ctl))].x; }   // (late branches)
    uint cc = GVM_CC(ctl), sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), next = base + 16, i6 = asuint(GVM_CIMM6(ctl)), sum = ca0 + i6;
    bool bad = cc == GVM_CC_SPECIAL ? (sub == GVM_SUB_ILLEGAL || (sub == GVM_SUB_ADD && rs == 0)) : cc == GVM_CC_ADDBNZ && GVM_CRA(ctl) == 0;
    [branch] if (bad) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }
    [branch] if (cc == GVM_CC_SPECIAL) {
        uint to_far = base + asuint(GVM_OFF18(ctl)) * 16, to = R4[(rs)].x & ~15u;
        [branch] if (sub == GVM_SUB_ERET) {
            [branch] if ((st & GVM_ST_S) == 0) { trap(GVM_CAUSE_PRIV, base | 12, ctl); return NONE; }
            next = epc;
            st = (st & GVM_ST_PIE ? GVM_ST_IE : 0) | (st & GVM_ST_PS ? GVM_ST_S : 0) | GVM_ST_PIE;
        }
        next = sub == GVM_SUB_JR || sub == GVM_SUB_CALLR ? to : sub == GVM_SUB_J || sub == GVM_SUB_CALL ? to_far : next;
        R4[(sub == GVM_SUB_CALLR || sub == GVM_SUB_CALL ? 1 : 64)].x = base + 16;
        R4[(sub == GVM_SUB_ADD ? rs : 64)].x = cs0 + asuint(GVM_CIMM12(ctl));
    } else {
        R4[(cc == GVM_CC_ADDBNZ ? GVM_CRA(ctl) : 64)].x = sum;
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
    e = e == NONE && NPAIR != 0 && pn >= NPAIR ? E_FULL : e;   // the bundle that filled the pair buffer ends the pass
    irq_due = (st & GVM_ST_IE) != 0 && (ipend() & ien) != 0;
    cur = ram(pc >> 4);
    cur.w &= (pc & 15) != 0 || irq_due ? ~7u : 0xffffffffu;   // (the fast loop makes whole bundles, and takes no interrupt)
    endr = e != NONE ? e : endr;
    return e != NONE ? 0 : budget - 1;
}

void run(uint budget) {
    [branch] if (flags & F_HALT) { endr = E_HALT; return; }
    [branch] if ((flags & F_SYNC) != 0 && qn != 0) { endr = E_SYNC; return; }
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
        [loop] while (budget > 0 && guard > 0 && (cur.w & 7) >= thr) { fstep(budget); guard--; }
        cycle += b0 - budget;
        opsrun += ops8 >> 3;
        pc = b0 != budget ? tp << 4 : pc;
        uint path = cur.w & 7;
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
    [branch] if (t < 16) return uint4(R4[(4 * t)].x, R4[(4 * t + 1)].x, R4[(4 * t + 2)].x, R4[(4 * t + 3)].x);
    [branch] if (t >= T_WC && t < T_WT) { uint e = 4 * (t - T_WC); return uint4(wd[e], wd[e + 1], wd[e + 2], wd[e + 3]); }
    [branch] if (t >= T_WT && t < T_WT + (WCN + 3) / 4) {
        uint k = 4 * (t - T_WT);
        return uint4(wt[k], wt[k + 1], k + 2 < WCN ? wt[k + 2] : NONE, k + 3 < WCN ? wt[k + 3] : NONE);
    }
    [branch] if (t >= T_Q && t < T_WC) return q[t - T_Q];
    uint4 s16 = uint4(pc, st, epc, cause), s17 = uint4(tval, tvec, scratch, ien), s18 = uint4(tcmp_lo, tcmp_hi, flags, rnd);
    uint4 s19 = uint4(cycle, opsrun, cycle - s3.x, endr == E_BUDGET && cycle - s3.x < bud && (flags & F_HALT) == 0 ? E_FULL : endr);
    uint4 s22 = uint4(qn, qhead, qgen, refused);
    return t == 16 ? s16 : t == 17 ? s17 : t == 18 ? s18 : t == 19 ? s19 : t == 20 ? lat0 : t == 21 ? lat1 : t == 22 ? s22 : 0;
}
uint4 again_out(uint t, uint c) {   // (the file as it was, its slots clean: their values were committed; no pairs)
    uint4 v = state_in(t, c);
    return t >= T_WT ? uint4(NONE, NONE, NONE, NONE) : t >= T_WC ? uint4(0, 0, 0, 0) : t == 18 ? uint4(v.xy, v.z | F_AGAIN, v.w) : t == 19 ? uint4(v.xyz, E_BUDGET) : v;
}
struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) {
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
void load_state(uint c) {
    uint k;
    [unroll] for (k = 0; k < 16; k++) {
        uint4 s = state_in(k, c);
        R4[(4 * k)].x = s.x; R4[(4 * k + 1)].x = s.y; R4[(4 * k + 2)].x = s.z; R4[(4 * k + 3)].x = s.w;
    }
    R4[(0)].x = 0;
    [loop] for (k = 0; k <= WCN; k++) wt[k] = NONE;
    bloom = 0;
    pn = 0;
    uint4 s16 = state_in(16, c), s17 = state_in(17, c), s18 = state_in(18, c), s19 = state_in(19, c);
    uint4 s22 = state_in(22, c), os = _OpState.Load(int3(c, 0, 0));
    pc = s16.x; st = s16.y; epc = s16.z; cause = s16.w;
    tval = s17.x; tvec = s17.y; scratch = s17.z; ien = s17.w;
    tcmp_lo = s18.x; tcmp_hi = s18.y; flags = s18.z; rnd = s18.w;
    cycle = s19.x; opsrun = s19.y; endr = E_BUDGET;
    lat0 = state_in(20, c); lat1 = state_in(21, c);
    qn = min(s22.x, QMAX); qhead = s22.y; qgen = s22.z; refused = s22.w;
    [unroll] for (k = 0; k < 16; k++) q[k] = state_in(T_Q + k, c);
    [branch] if (os.x == qgen) {
        uint d = min(os.y, qn);
        [loop] for (k = 0; k + d < qn; k++) { q[2 * k] = q[2 * (k + d)]; q[2 * k + 1] = q[2 * (k + d) + 1]; }
        qn -= d; qhead = os.z; refused += os.w;
    }
    qgen++;
}

tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    tick_out o;
    uint rx = p.x % 8u, ry = p.y % 16u;
    core = (p.y / 16u) * 128u + core_at(p.x / 8u);
    uint j = rx * 4u + ry;
    [branch] if (core >= _NCores || j >= SPIX || ry >= 4u) { o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = 0; return o; }
    ext = (pick(_Ext[core >> 7], (core >> 5) & 3) >> (core & 31)) & 1;
    bud = core == 0 ? _Ticks : _Ticks >> WSH;   // (wsh)
    [branch] if (WAD != 0 && core != 0) {   // (wad: the pass ends about when core 0's does)
        uint4 z = state_in(19, 0);
        bool cap = WAD == 1 || WAD == 4 ? z.w == E_SYNC || z.w == E_FULL : WAD != 5 && z.w == E_FULL;   // (2, 3, 6: core 0 had work: its cache was full; 5 never then)
        [branch] if (WAD == 7 && cap) cap = ram((WAITW >> 4) + 2).x != 0;   // (7: on full only while a program has declared its mask)
        [branch] if (WAD >= 5 && z.w == E_SYNC) {   // (5-7: core 0 rested: the workers it waits for, from the kernel's word at WAITW; no mask, no cap)
            uint4 m0 = ram(WAITW >> 4), m1 = ram((WAITW >> 4) + 1);
            uint4 mm = (core >> 7) != 0 ? m1 : m0;
            bool given = any((m0 | m1) != 0), mine = ((pick(mm, (core >> 5) & 3) >> (core & 31)) & 1) != 0;
            cap = given && !mine;
        }
        [branch] if (WAD == 4 && cap) {   // (4: not when the workers are the work: half of cores 1-8 ran their whole budget)
            uint busy = 0, k;
            [loop] for (k = 1; k <= 8; k++) busy += k < _NCores && state_in(19, k).w == E_BUDGET ? 1u : 0u;
            cap = busy < 4;
        }
        bud = cap ? clamp(z.z + (z.z >> 2), WAD == 3 ? bud >> 1 : 512u, bud) : bud;
    }
    load_state(core);
    thr = (flags & F_AGAIN) != 0 || qn != 0 ? GVM_PATH_INT : GVM_PATH_FAST;
    flags &= ~F_AGAIN;
    lmax = 0; smax = 0;
    run(core == 0 ? _Ticks : bud);
    bool again = lmax >= _RamEnd || smax >= _RamEnd - GVM_DEV;
    uint4 s19 = state_in(19, core);
    o.t0 = state_out(j * 8 + 0, s19); o.t1 = state_out(j * 8 + 1, s19); o.t2 = state_out(j * 8 + 2, s19); o.t3 = state_out(j * 8 + 3, s19);
    o.t4 = state_out(j * 8 + 4, s19); o.t5 = state_out(j * 8 + 5, s19); o.t6 = state_out(j * 8 + 6, s19); o.t7 = state_out(j * 8 + 7, s19);
    [branch] if (again) {
        uint t = j * 8;
        o.t0 = again_out(t, core); o.t1 = again_out(t + 1, core); o.t2 = again_out(t + 2, core); o.t3 = again_out(t + 3, core);
        o.t4 = again_out(t + 4, core); o.t5 = again_out(t + 5, core); o.t6 = again_out(t + 6, core); o.t7 = again_out(t + 7, core);
    }
    return o;
}

// ---- The commit: a point a write cache entry, drawn into its texel of RAM in the draw of its word's component

uint4 state_any(uint t, uint c) {
    int3 at = sat(t >> 3, c);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    return (t & 4) ? ((t & 2) ? v3 : v2) : ((t & 2) ? v1 : v0);
}
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint tg = pick(state_any(T_WT + (j >> 2), c), j & 3), tex = tg & 0x0fffffffu;
    float2 at = (float2(tex % RAM_W, tex / RAM_W) + 0.5) / float2(RAM_W, _RamRows);
    c2f o;
    o.vertex = tg != NONE && ((tg >> (28 + _Comp)) & 1) != 0 ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = pick(state_any(T_WC + j, c), _Comp);
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
