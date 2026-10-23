// floor: the pieces of an interpreter step, timed one warp a core as the tick runs (gpu_vm/tick/probes/floor.cpp).
// A core is a block of 8 x 4 pixels that all run the same loop; each turn of the loop is one "bundle" of a synthetic
// program in _Code (a probe's own byte-aligned encoding, floor.cpp's gen()), its parts chosen by defines:
//   FETCH  1: the next bundle only (straight code); 2: both the fall-through and the target, the taken one selected
//   FILE   0: no registers; 1: uint R[65] (64 = the sink); 2: uint4 R[17] (a word by index: .x-.w chosen); 3: uint4 R[65], .x
//   OPS    ops a bundle (0-2); each reads NRD registers (2 or 3) and writes one
//   UNIT   0 a move; 1 an add; 2, 4, 8, 16 that many functions computed and one selected (a flattened unit);
//          9 x23's mid unit (I/R forms by a bit, inverted operands, compares, min/max, shifts, mul);
//          20 eight float functions (add mul mad msb sub nmsb min max) selected; 21 an add, and the float unit behind a
//          branch taken by a bit of the bundle (FPCT percent of bundles)
//   BR     0: taken by a bit of the bundle (no register); 1: x23's 16 kinds (lt/ltu/eq/bit, imm6, sense) on two registers
//          read; 2: one register != 0, a sense bit; 3: op 1's result != 0, a sense bit (forwarded, no read); 4: lt/ltu/eq
//          with a sense bit on two registers (6 kinds)
//   IMM    1: op 0's second operand is the bundle's word 2 when a bit says (the I/R form's select)
//   MEM    0: no memory; op 0 a load (address R[ra] + word 2): 1 the data texel alone (no write cache); 2 a direct-mapped
//          write cache of 64 texels (one tag); 3 two buckets of four (x23's: 2 x 16 buckets, 8 tags read); 4 as 3 behind
//          a bloom word (x23's filter); 5 a store into the direct-mapped cache (tag and word written)
//   ILP    2: two independent chains a turn (their own pc and registers)
// Every loop is capped (NCAP) whatever _N says.
Texture2D<uint4> _Code : register(t0);
Texture2D<uint4> _Data : register(t1);
cbuffer P : register(b0) { uint _N, _Cores, _Seed, _Mask; };
#ifndef FETCH
#define FETCH 2
#endif
#ifndef FILE
#define FILE 3
#endif
#ifndef OPS
#define OPS 2
#endif
#ifndef NRD
#define NRD 2
#endif
#ifndef UNIT
#define UNIT 1
#endif
#ifndef BR
#define BR 0
#endif
#ifndef IMM
#define IMM 0
#endif
#ifndef MEM
#define MEM 0
#endif
#ifndef BRFIRST
#define BRFIRST 0
#endif
#ifndef WRT
#define WRT 1
#endif
#ifndef ILP
#define ILP 1
#endif
static const uint NCAP = 100000;   // (a draw stays well under a second)
static const uint DMASK = 0x7ffcu;   // loads within 32 KB of data (2,048 texels: the texture cache keeps them)
static const uint CW = 8192;   // _Code's width: bundle b at (b % CW, b / CW)

#if FILE == 1
static uint R[65 * ILP];
#define RD(i) R[(i)]
#define WR(i, v) R[(i)] = (v)
#elif FILE == 2
static uint4 R[17 * ILP];
uint rget(uint i) { uint4 q = R[i >> 2]; return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
#define RD(i) rget(i)
#define WR(i, v) { uint i_ = (i), v_ = (v); uint4 q_ = R[i_ >> 2]; R[i_ >> 2] = uint4((i_ & 3) == 0 ? v_ : q_.x, (i_ & 3) == 1 ? v_ : q_.y, (i_ & 3) == 2 ? v_ : q_.z, (i_ & 3) == 3 ? v_ : q_.w); }
#elif FILE == 3
static uint4 R[65 * ILP];
#define RD(i) R[(i)].x
#define WR(i, v) R[(i)].x = (v)
#elif FILE == 4 || FILE == 5   // NR registers in the GPU's own registers: reads a tree of selects, writes a select each
static const uint NR = FILE == 4 ? 16 : 8;
static uint G[NR];
uint gread(uint i) {
    uint l0[8], l1[4], l2[2], k;
    [unroll] for (k = 0; k < NR / 2; k++) l0[k] = (i & 1) ? G[2 * k + 1] : G[2 * k];
    [unroll] for (k = 0; k < NR / 4; k++) l1[k] = (i & 2) ? l0[2 * k + 1] : l0[2 * k];
    [unroll] for (k = 0; k < NR / 8; k++) l2[k] = (i & 4) ? l1[2 * k + 1] : l1[2 * k];
    return NR == 16 ? ((i & 8) ? l2[1] : l2[0]) : l2[0];
}
void gwrite(uint i, uint v) { uint k; [unroll] for (k = 1; k < NR; k++) G[k] = (i & (NR - 1)) == k ? v : G[k]; }
#define RD(i) gread(i)
#define WR(i, v) gwrite(i, v)
#elif FILE == 6 || FILE == 7   // NF fast registers (0..NF-1) in GPU registers, the rest in local memory; 7: a branch an operand
#ifndef NF
#define NF 8
#endif
static uint G[NF];
static uint4 R[65 * ILP];
uint gtree(uint i) {
    uint l0[4], l1[2], k;
    [unroll] for (k = 0; k < NF / 2; k++) l0[k] = (i & 1) ? G[2 * k + 1] : G[2 * k];
    [unroll] for (k = 0; k < NF / 4; k++) l1[k] = (i & 2) ? l0[2 * k + 1] : l0[2 * k];
    return NF == 8 ? ((i & 4) ? l1[1] : l1[0]) : l1[0];
}
uint hread(uint i) {
#if FILE == 7
    uint r;
    [branch] if (i < NF) r = gtree(i); else r = R[i].x;
    return r;
#else
    return i < NF ? gtree(i) : R[i].x;
#endif
}
void hwrite(uint i, uint v) { uint k; [unroll] for (k = 1; k < NF; k++) G[k] = i == k ? v : G[k]; R[i < NF ? 64 : i].x = v; }
#define RD(i) hread(i)
#define WR(i, v) hwrite(i, v)
#else
static uint Rz[ILP * 2];
#define RD(i) ((i) * 0x9e3779b1u + Rz[0])
#define WR(i, v) Rz[0] ^= (v)
#endif

#if MEM >= 2
#if MEM == 2 || MEM == 5
static const uint NT = 64;
#else
static const uint NT = 128;
#endif
static uint wt[NT * ILP];
static uint wd[4 * NT * ILP];
static uint bloom;
#endif

uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }

uint unit(uint f, uint a, uint b, uint c) {
#if UNIT == 0
    return a;
#elif UNIT == 1 || UNIT == 21
    return NRD >= 3 ? a + b + c : a + b;
#elif UNIT == 2
    return (f & 1) ? a ^ b : a + b;
#elif UNIT == 4
    uint s = a + ((f & 1) ? 0 - b : b);
    return (f & 2) ? ((f & 1) ? a | b : a & b) : s;
#elif UNIT == 8
    uint s = a + ((f & 1) ? 0 - b : b), lg = (f & 1) ? a | b : a & b, sh = (f & 1) ? a >> b : a << b, m = (f & 1) ? a ^ b : a * b;
    return (f & 4) ? ((f & 2) ? m : sh) : ((f & 2) ? lg : s);
#elif UNIT == 16
    uint s = a + ((f & 1) ? 0 - b : b), lg = (f & 1) ? a | b : a & b, sh = (f & 1) ? a >> b : a << b, m = (f & 1) ? a ^ b : a * b;
    uint F = (f & 1) ? 0u : 0x80000000u;
    bool lt = (a ^ F) < (b ^ F);
    uint cmp = (f & 2) ? (lt ? a : b) : (lt ? 1u : 0u), sra = asuint(asint(a) >> b), eq = a == b ? 1u : 0u;
    uint hi = (f & 2) ? ((f & 1) ? eq : sra) : cmp;
    uint lo = (f & 4) ? ((f & 2) ? m : sh) : ((f & 2) ? lg : s);
    return (f & 8) ? hi : lo;
#elif UNIT == 9   // x23's alu_mid (f's bits as its opc bits 0-4)
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    uint x = a, y = b;
    uint f2v = f2 ? 1u : 0u, yi = y ^ (0 - f2v), sum = x + yi + f2v;
    uint g0 = f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
    uint F = f0 ? 0u : 0x80000000u;
    bool lt = (x ^ F) < (y ^ F), cc = f2 ? !lt : lt;
    uint g1 = f1 ? (cc ? x : y) : (cc ? 1u : 0u);
    uint lo = f1 ? (f0 ? x * y : asuint(asint(x) >> y)) : (f0 ? x >> y : x << y);
    return (f & 16) ? lo : (f & 8) ? g1 : g0;
#elif UNIT == 20
    float x = asfloat(a), y = asfloat(b), z = asfloat(c);
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    float p = x * ((f2 && !f1 && !f0) ? -1.0 : 1.0);
    float prod = x * y;
    float r = f2 ? (f1 ? (f0 ? max(x, y) : min(x, y)) : (f0 ? z - prod : x - y)) : (f1 ? (f0 ? prod - z : prod + z) : (f0 ? prod : x + y));
    uint u = asuint(r);
    return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u;
#endif
}
#if UNIT == 21
uint funit(uint f, uint a, uint b, uint c) {
    float x = asfloat(a), y = asfloat(b), z = asfloat(c);
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    float prod = x * y;
    float r = f2 ? (f1 ? (f0 ? max(x, y) : min(x, y)) : (f0 ? z - prod : x - y)) : (f1 ? (f0 ? prod - z : prod + z) : (f0 ? prod : x + y));
    uint u = asuint(r);
    return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u;
}
#endif

// x23's branch test (cc in t.w bits 1-4 as isa.h's cc bits 7-10)
uint taken16(uint cc, uint CA, uint CB, uint i6) {
    bool im = (cc & 8) != 0;
    uint Bc = im ? i6 : CB, F = (cc << 29) & 0x80000000u;
    bool lt = (CA ^ F) < (Bc ^ F);
    uint sum = CA + i6;
    uint o = im ? (CA >> Bc) & 1 : (sum == 0 ? 1u : 0u);
    uint c = (cc & 2) ? (lt ? 1u : 0u) : (cc & 4) ? (CA == Bc ? 1u : 0u) : o;
    return (cc & 1) ? (c == 0 ? 1u : 0u) : c;
}

uint4 code(uint b) { return _Code.Load(int3(b % CW, b / CW, 0)); }
static uint4 g_nn[ILP];
uint4 dtex(uint t) { return _Data.Load(int3(t % 8192, (t / 8192) & 7, 0)); }

// One bundle (t, at code index tp) of chain K: its registers from K * 65 (or none), its next pc returned in tp, its
// next bundle in t. Encoding (floor.cpp): t.x op 0 (ra bits 0-7, rb 8-15, rd 16-23, f 24-31), t.y op 1 (the same),
// t.z the long immediate (or a load's offset), t.w control: bit 0 taken (BR 0) or the sense, bits 1-4 cc, bits 8-15 ca,
// 16-23 cb (or imm6 in its low bits), 24-31 the offset in bundles (signed).
#ifndef MEMBR
#define MEMBR 0
#endif
#ifndef MEMRT
#define MEMRT 0
#endif
#ifndef DEFER
#define DEFER 0
#endif
static uint pv[8], pd[8];
#ifndef HOIST
#define HOIST 0
#endif
static uint hv[8];   // (HOIST: the block's loads, issued at its start)
#if MEM
uint memval(uint4 t, uint base) {   // op 0's load as the block found its registers
    uint A0 = RD((t.x & 255) + base);
#if MEM == 1
    uint ad = (A0 + t.z) & DMASK;
    return pick(dtex(ad >> 4), (ad >> 2) & 3);
#else
    uint ad = (A0 + t.z) & DMASK, tx = ad >> 4, i = (ad >> 2) & 3;
    uint b1 = (tx * 0x9e3779b1u) >> 28, b2 = 16 + ((tx * 0x85ebca6bu + 0x165667b1u) >> 28);
    uint4 q = dtex(tx);
    uint4 A = uint4(wt[4 * b1], wt[4 * b1 + 1], wt[4 * b1 + 2], wt[4 * b1 + 3]);
    uint4 B = uint4(wt[4 * b2], wt[4 * b2 + 1], wt[4 * b2 + 2], wt[4 * b2 + 3]);
    bool4 ka = (A & 0x0fffffffu) == tx, kb = (B & 0x0fffffffu) == tx;
    uint msk = (ka.x ? 1u : 0u) | (ka.y ? 2u : 0u) | (ka.z ? 4u : 0u) | (ka.w ? 8u : 0u)
             | (kb.x ? 16u : 0u) | (kb.y ? 32u : 0u) | (kb.z ? 64u : 0u) | (kb.w ? 128u : 0u);
    uint k = firstbitlow(msk);
    uint e = k < 4 ? 4 * b1 + k : k < 8 ? 4 * b2 + (k & 3) : NT;
    return e < NT ? wd[4 * e + i] : pick(q, i);
#endif
}
#endif   // (DEFER: a load's value and register, written at the block's end)
uint ops(uint4 t, uint base, uint K, bool memk, inout uint sum, uint slot = 0) {   // a bundle's two ops (no fetch, no branch); op 1's value
    uint v0 = 0, v1 = 0;
#if OPS >= 1
    uint ra0 = (t.x & 255) + base, rb0 = ((t.x >> 8) & 255) + base, d0 = ((t.x >> 16) & 255) + base, f0 = t.x >> 24;
    uint A0 = RD(ra0), B0 = RD(rb0);
#if NRD >= 3
    uint C0 = RD(((t.x >> 26) & 63) + base);
#else
    uint C0 = A0;
#endif
#if IMM
    B0 = (f0 & 0x80) ? t.z : B0;
#endif
#if MEM == 0
#if UNIT == 21
    [branch] if (f0 & 0x40) v0 = funit(f0, A0, B0, C0); else v0 = unit(f0, A0, B0, C0);
#else
    v0 = unit(f0, A0, B0, C0);
#endif
#else
#if MEMBR
    [branch]
#endif
    if (!memk) v0 = unit(f0, A0, B0, C0); else {
#if HOIST
    v0 = hv[slot];
#else
#if MEM == 1
    uint ad = (A0 + t.z) & DMASK;
    v0 = pick(dtex(ad >> 4), (ad >> 2) & 3);
#elif MEM == 2 || MEM == 5
    uint ad = (A0 + t.z) & DMASK, tx = ad >> 4, i = (ad >> 2) & 3, e = (tx & (NT - 1)) + K * NT;
    uint4 q = dtex(tx);
    bool hit = wt[e] == tx;
#if MEM == 5
    uint w = hit ? wd[4 * e + i] : pick(q, i);
    wt[e] = tx; wd[4 * e + i] = B0;   // (a store: the texel's tag and its word; a miss takes the entry)
    v0 = w;
#else
    v0 = hit ? wd[4 * e + i] : pick(q, i);
#endif
#else   // two buckets of four (x23's mfind without overflow)
    uint ad = (A0 + t.z) & DMASK, tx = ad >> 4, i = (ad >> 2) & 3;
    uint b1 = (tx * 0x9e3779b1u) >> 28, b2 = 16 + ((tx * 0x85ebca6bu + 0x165667b1u) >> 28);
    uint4 q = dtex(tx);
    uint4 A = 0xffffffffu, B = 0xffffffffu;
    uint KB = K * NT;
#if MEM == 4
    [branch] if ((tx & bloom) == tx)
#endif
    {
        A = uint4(wt[KB + 4 * b1], wt[KB + 4 * b1 + 1], wt[KB + 4 * b1 + 2], wt[KB + 4 * b1 + 3]);
        B = uint4(wt[KB + 4 * b2], wt[KB + 4 * b2 + 1], wt[KB + 4 * b2 + 2], wt[KB + 4 * b2 + 3]);
    }
    bool4 ka = (A & 0x0fffffffu) == tx, kb = (B & 0x0fffffffu) == tx;
    uint msk = (ka.x ? 1u : 0u) | (ka.y ? 2u : 0u) | (ka.z ? 4u : 0u) | (ka.w ? 8u : 0u)
             | (kb.x ? 16u : 0u) | (kb.y ? 32u : 0u) | (kb.z ? 64u : 0u) | (kb.w ? 128u : 0u);
    uint k = firstbitlow(msk);
    uint e = k < 4 ? 4 * b1 + k : k < 8 ? 4 * b2 + (k & 3) : NT;
    v0 = e < NT ? wd[4 * (e + KB) + i] : pick(q, i);
#endif
#endif
    }
#endif
#if WRT
    if (DEFER && memk) { pv[slot] = v0; pd[slot] = d0; } else WR(d0, v0);
#else
    sum ^= v0 + d0;
#endif
#endif
#if OPS >= 2
    uint ra1 = (t.y & 255) + base, rb1 = ((t.y >> 8) & 255) + base, d1 = ((t.y >> 16) & 255) + base, f1 = t.y >> 24;
    uint A1 = RD(ra1), B1 = RD(rb1);
#if NRD >= 3
    uint C1 = RD(((t.y >> 26) & 63) + base);
#else
    uint C1 = A1;
#endif
#if MEM == 0 && UNIT == 21
    v1 = unit(f1, A1, B1, C1);
#else
    v1 = unit(f1, A1, B1, C1);
#endif
#if WRT
    WR(d1, v1);
#else
    sum ^= v1 + d1;
#endif
#endif
    return v1;
}
void bundle(inout uint4 t, inout uint tp, uint K, inout uint sum) {
    uint ctl = t.w;
    uint tn = (tp + 1) & _Mask;
#if FETCH == 2
    uint tg = (tp + asuint((int)ctl >> 24)) & _Mask;
    uint4 nx = code(tn), tc = code(tg);
#elif FETCH == 4   // the fall-through fetched a bundle ahead, the target by a branch only when taken
    uint tg = (tp + asuint((int)ctl >> 24)) & _Mask;
    uint4 nx = g_nn[K];
#elif FETCH == 3   // the fall-through fetched a bundle ahead: the target, its fall-through and the one after the fall-through now
    uint tg = (tp + asuint((int)ctl >> 24)) & _Mask;
    uint4 nx = g_nn[K], tc = code(tg), tc1 = code((tg + 1) & _Mask), nn2 = code((tp + 2) & _Mask);
#else
    uint4 nx = code(tn);
#endif
#if FILE == 2
    uint base = K * 68;
#else
    uint base = K * 65;
#endif
    uint v0 = 0, v1 = 0;
#if BRFIRST   // the branch's registers read with the ops' (as the bundle found them)
#if BR == 0
    bool tk = (ctl & 1) != 0;
#elif BR == 1
    uint CA = RD(((ctl >> 8) & 255) + base), CB = RD(((ctl >> 16) & 255) + base);
    bool tk = taken16((ctl >> 1) & 15, CA, CB, asuint((int)(ctl << 10) >> 26)) != 0;
#elif BR == 2
    bool tk = (RD(((ctl >> 8) & 255) + base) != 0) == ((ctl & 1) != 0);   // (random registers are not 0: taken as the bit, 15%)
#elif BR == 3
    bool tk = (v1 != 0) == ((ctl & 1) != 0);
#elif BR == 4
    uint CA = RD(((ctl >> 8) & 255) + base), CB = RD(((ctl >> 16) & 255) + base);
    uint F = (ctl << 29) & 0x80000000u;
    bool c = (ctl & 4) ? (CA ^ F) < (CB ^ F) : CA == CB;
    bool tk = c != ((ctl & 1) != 0);
#endif
#endif
#if MEMRT   // whether op 0 is a load: a bit of the bundle (--fpct of them)
    v1 = ops(t, base, K, ((t.x >> 24) & 0x40) != 0, sum);
#else
    v1 = ops(t, base, K, true, sum);
#endif
#if !BRFIRST
#if BR == 0
    bool tk = (ctl & 1) != 0;
#elif BR == 1
    uint CA = RD(((ctl >> 8) & 255) + base), CB = RD(((ctl >> 16) & 255) + base);
    bool tk = taken16((ctl >> 1) & 15, CA, CB, asuint((int)(ctl << 10) >> 26)) != 0;
#elif BR == 2
    bool tk = (RD(((ctl >> 8) & 255) + base) != 0) == ((ctl & 1) != 0);   // (random registers are not 0: taken as the bit, 15%)
#elif BR == 3
    bool tk = (v1 != 0) == ((ctl & 1) != 0);
#elif BR == 4
    uint CA = RD(((ctl >> 8) & 255) + base), CB = RD(((ctl >> 16) & 255) + base);
    uint F = (ctl << 29) & 0x80000000u;
    bool c = (ctl & 4) ? (CA ^ F) < (CB ^ F) : CA == CB;
    bool tk = c != ((ctl & 1) != 0);
#endif
#endif
#if FETCH == 2
    tp = tk ? tg : tn;
    t = tk ? tc : nx;
#elif FETCH == 3
    tp = tk ? tg : tn;
    t = tk ? tc : nx;
    g_nn[K] = tk ? tc1 : nn2;
#elif FETCH == 4
    [branch] if (tk) { tp = tg; t = code(tg); g_nn[K] = code((tg + 1) & _Mask); }
    else { tp = tn; t = nx; g_nn[K] = code((tn + 1) & _Mask); }
#else
    tp = tn;
    t = nx;
    sum += tk ? 1u : 0u;
#endif
}



#ifdef VBLOCK   // blocks of 1-8 bundles (the first bundle says how many), each bundle's op 0 a load or not by a bit: one turn a block
#ifndef MEMBR
#endif
static uint nbund;
uint klen(uint c) { uint t = (c >> 1) & 15; return t < 2 ? 1u : t < 5 ? 2u : t < 7 ? 3u : t < 10 ? 4u : t < 11 ? 5u : t < 13 ? 6u : 8u; }
bool decide(uint ctl) { return (RD((ctl >> 8) & 255) != 0) == ((ctl & 1) != 0); }
void vblock_run(uint core, inout uint sum) {
    uint4 blk[8], nxt[8];
    uint bi = (core * 13u) & _Mask, k, j;
    [unroll] for (k = 0; k < 8; k++) blk[k] = code(bi * 8 + k);
    uint n = min(_N, NCAP / 8);
    nbund = 0;
    [loop] for (j = 0; j < n; j++) {
        uint c0 = blk[0].w, K = klen(c0);
        uint nb = (bi + (decide(c0) ? asuint((int)c0 >> 24) : 1u)) & _Mask;
        [unroll] for (k = 0; k < 8; k++) nxt[k] = code(nb * 8 + k);
        [unroll] for (k = 0; k < 8; k++) pd[k] = 64;
        [unroll] for (k = 0; k < 8; k++) {
            [branch] if (k < K) sum += ops(blk[k], 0, 0, ((blk[k].x >> 24) & 0x40) != 0, sum, k);
        }
#if DEFER
        [unroll] for (k = 0; k < 8; k++) WR(pd[k], pv[k]);
#endif
        nbund += K;
        [unroll] for (k = 0; k < 8; k++) blk[k] = nxt[k];
        bi = nb;
    }
    sum ^= bi;
}
#endif
#ifdef BLOCK   // blocks of BLOCK bundles: fetched together at addresses that do not wait for them, run straight, one decision
#ifndef EARLY
#define EARLY 0
#endif
#ifndef MEMEVERY
#define MEMEVERY 1
#endif
bool decide(uint ctl) { return (RD((ctl >> 8) & 255) != 0) == ((ctl & 1) != 0); }   // a register != 0 (15% taken)
void block_run(uint core, inout uint sum) {
    uint4 blk[BLOCK], nxt[BLOCK];
    uint bi = (core * 13u) & _Mask, k, j;
    [unroll] for (k = 0; k < BLOCK; k++) blk[k] = code(bi * BLOCK + k);
    uint n = min(_N, NCAP / BLOCK);
    [loop] for (j = 0; j < n; j++) {
#if EARLY   // the decision from the block's first bundle, made before its ops: the next block in flight while they run
        uint c0 = blk[0].w;
        uint nb = (bi + (decide(c0) ? asuint((int)c0 >> 24) : 1u)) & _Mask;
        [unroll] for (k = 0; k < BLOCK; k++) nxt[k] = code(nb * BLOCK + k);
#endif
#if HOIST && MEM   // the block's loads issued at its start, their latencies overlapping
        [unroll] for (k = 0; k < BLOCK; k += MEMEVERY) hv[k] = memval(blk[k], 0);
#endif
        [unroll] for (k = 0; k < BLOCK; k++) sum += ops(blk[k], 0, 0, (k % MEMEVERY) == 0, sum, k);
#if DEFER   // the block's loads land in their registers at its end (the compiler keeps their uses in later blocks)
        [unroll] for (k = 0; k < BLOCK; k += MEMEVERY) WR(pd[k], pv[k]);
#endif
#if !EARLY   // the decision at the block's end, on its last bundle's register as the ops left it
        uint cl = blk[BLOCK - 1].w;
        uint nb = (bi + (decide(cl) ? asuint((int)cl >> 24) : 1u)) & _Mask;
        [unroll] for (k = 0; k < BLOCK; k++) nxt[k] = code(nb * BLOCK + k);
#endif
        [unroll] for (k = 0; k < BLOCK; k++) blk[k] = nxt[k];
        bi = nb;
    }
    sum ^= bi;
}
#endif
#ifdef CHASE
static uint CA_[CHASE];
#endif
uint4 frag(float4 pos : SV_POSITION) : SV_Target {
    uint2 p = uint2(pos.xy);
    uint core = (p.y / 4u) * 128u + p.x / 8u;
    [branch] if (core >= _Cores) return 0;
#ifdef LANES
    [branch] if ((p.x & 7) != 0 || (p.y & 3) != 0) return 0;
#endif
#ifdef CHASE
    {   // a dependent chase through CHASE words of local memory: x = A[x] + 1 (A a permutation by a stride)
        uint k, x = core & (CHASE - 1);
        [loop] for (k = 0; k < CHASE; k++) CA_[k] = (k * 5u + 1u + (_Seed & 4u)) & (CHASE - 1);   // (a full cycle)
        uint n = min(_N, NCAP);
        [loop] for (k = 0; k < n; k++) x = CA_[x] ^ (k & 1);
        return uint4(x, core, 0, 0);
    }
#endif
    uint k, s = _Seed ^ (core * 0x85ebca6bu);
#if FILE == 1 || FILE == 3
    [loop] for (k = 0; k < 65 * ILP; k++) { WR(k, (k % 65) == 0 || (k % 65) == 64 ? 0u : (k * 0x9e3779b9u) ^ s); }
#elif FILE == 2
    [loop] for (k = 0; k < 17 * ILP; k++) R[k] = uint4(k * 4, k * 4 + 1, k * 4 + 2, k * 4 + 3) * 0x9e3779b9u ^ s;
#elif FILE == 4 || FILE == 5
    [unroll] for (k = 0; k < NR; k++) G[k] = k == 0 ? 0u : (k * 0x9e3779b9u) ^ s;
#elif FILE == 6 || FILE == 7
    [unroll] for (k = 0; k < NF; k++) G[k] = k == 0 ? 0u : (k * 0x9e3779b9u) ^ s;
    [loop] for (k = 0; k < 65; k++) R[k].x = k == 0 || k == 64 ? 0u : (k * 0x9e3779b9u) ^ s;
#else
    Rz[0] = s; Rz[1] = s;
#endif
#if MEM >= 2
    // tags: entry e holds texel (e * 7 + core) & 0x3ffff... about half the loads find theirs (floor.cpp's data range)
    [loop] for (k = 0; k < NT * ILP; k++) wt[k] = (MEM == 2 || MEM == 5) ? ((((k * 2654435761u ^ s) >> 8) & 31u) << 6 | (k & 63u)) : ((k * 2654435761u ^ s) >> 8) & 0x7ffu;
    [loop] for (k = 0; k < 4 * NT * ILP; k++) wd[k] = k ^ s;
    bloom = 0x7ffu;
#endif
    uint tp = (core * 13u) & _Mask, sum = 0;
    uint4 t = code(tp);
    g_nn[0] = code((tp + 1) & _Mask);
#if ILP == 2
    uint tp2 = (core * 29u + 7u) & _Mask;
    uint4 t2 = code(tp2);
    g_nn[1] = code((tp2 + 1) & _Mask);
#endif
#ifdef VBLOCK
    vblock_run(core, sum);
    uint n = 0;
#elif defined(BLOCK)
    block_run(core, sum);
    uint n = 0;
#else
    uint n = min(_N, NCAP);
#endif
    [loop] for (k = 0; k < n; k++) {
        bundle(t, tp, 0, sum);
#if ILP == 2
        bundle(t2, tp2, 1, sum);
#endif
    }
    uint h = tp ^ sum ^ t.x;
#if ILP == 2
    h ^= tp2 * 3u ^ t2.y;
#endif
#if FILE == 4 || FILE == 5
    [unroll] for (k = 0; k < NR; k++) h = h * 31u + G[k];
#elif FILE == 6 || FILE == 7
    [unroll] for (k = 0; k < NF; k++) h = h * 31u + G[k];
    [loop] for (k = NF; k < 64; k++) h = h * 31u + R[k].x;
#elif FILE != 0
    [loop] for (k = 0; k < 65 * ILP; k++) h = h * 31u + RD(k);
#else
    h ^= Rz[0];
#endif
#if MEM >= 2
    [loop] for (k = 0; k < 4 * NT * ILP; k += 5) h = h * 17u + wd[k] + wt[k / 4];
#endif
#ifdef VBLOCK
    return uint4(h, core, nbund, 0);
#else
    return uint4(h, core, 0, 0);
#endif
}
float4 vert(uint id : SV_VertexID) : SV_POSITION { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
