// A prototype tick for a shader-native ISA (README.md): five instructions told apart by a 2-bit class
// and their flags, no switch. Inside a class every result is made and the control word's bits pick
// one by selects. Code is four words a texel, RAM one word a texel.

Texture2D<uint4> _Code : register(t0);   // R32G32B32A32_UINT: slot s is (control, ra, rd | rb << 16, immediate)
Texture2D<uint>  _Ram : register(t1);    // R32_UINT: word w of RAM
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };

#define CODE_SHIFT 12
#define RAM_SHIFT 13
#define CODE_AT(s) int3((s) & ((1u << CODE_SHIFT) - 1), (s) >> CODE_SHIFT, 0)
#define RAM_AT(w) int3((w) & ((1u << RAM_SHIFT) - 1), (w) >> RAM_SHIFT, 0)
#define CODE_BASE 0x80400000u   // the byte address of slot 0, as a link register holds it

// The control word: bits 0-1 the class, the rest the class's own (README.md, "The control word").
#define K_INT 0
#define K_MEM 1
#define K_FPU 2
#define K_OTHER 3          // bit 2 set: SFU
#define S0 (1u << 3)       // INT, FPU, SFU: the result picked, bits 3-5
#define B_NEG (1u << 2)    // INT, FPU: b negated
#define B_UNS (1u << 6)    // INT: unsigned compare
#define B_EQ (1u << 7)     // INT: compare equal
#define B_INV (1u << 8)    // INT: the comparison inverted
#define B_BR (1u << 9)     // INT: a branch, taken when the comparison holds, to slot x >> 11
#define B_SELV (1u << 10)  // INT: the comparison picks a or b, not 1 or 0 (min, max)
#define B_STORE (1u << 2)  // MEM
#define B_NC (1u << 3)     // MEM: a load no store of this pass can have touched
#define UP_SHIFT 4         // MEM: 32 less the access's bits (0, 16, 24), five bits
#define B_MUNS (1u << 9)   // MEM: a load not sign-extended
#define F_NEGA (1u << 6)   // FPU: a negated
#define F_MULB (1u << 7)   // FPU mad: times b, else times 1
#define F_ADDB (1u << 8)   // FPU mad: plus b
#define F_ADDRD (1u << 9)  // FPU mad: plus rd (neither: plus -0)
#define F_NEGC (1u << 10)  // FPU mad: rd negated
#define F_M0 (1u << 11)    // FPU: compare <= / sign from ~b
#define F_M1 (1u << 12)    // FPU: compare == / sign from a ^ b
#define F_NEGR (1u << 13)  // FPU: the result negated (max is -min(-a, -b))
#define B_SFU (1u << 2)    // OTHER
#define B_DIV (1u << 3)    // OTHER: a division, else a product
#define B_JALR (1u << 4)   // OTHER: a jump to a + b, rd the link
#define B_HIGH (1u << 5)   // OTHER: the high word of a product; the remainder of a division
#define B_SIGNA (1u << 6)  // OTHER: a signed
#define B_SIGNB (1u << 7)  // OTHER: b signed (a division is signed by B_SIGNA alone)
#define B_SYS (1u << 8)    // OTHER: CSRs, ecall, atomics, fence, traps: the general step's

// r0 is zero (never a destination), r1-r31 the integers, r32-r63 the floats as bits, r64 the
// sink a result nobody wants goes to; 65-67 pad the last state texel.
static uint r[68];
static uint pc;            // a slot of _Code

// ---- The write cache: a bucket is two (word + 1, value) pairs, picked by the word's low bits;
// a bit a bucket says it was written in this pass, so the array is never cleared.
#define WC_N 256
static uint4 wc[WC_N];
static uint4 occ0 = 0, occ1 = 0;
static uint bloom = 0;     // the stored words' numbers or-ed: a load outside it is in no bucket

bool occupied(uint b) {
    uint4 o = (b & 128) ? occ1 : occ0;
    uint2 h = (b & 64) ? o.zw : o.xy;
    return ((((b & 32) ? h.y : h.x) >> b) & 1) != 0;   // (a shift takes the low five bits)
}
void occupy(uint b) {
    uint i = (b >> 5) & 3;
    uint4 m = (uint4)(uint4(0, 1, 2, 3) == i) << (b & 31);
    if (b & 128) occ1 |= m; else occ0 |= m;
}
uint load_word(uint w, bool search) {
    uint v = _Ram.Load(RAM_AT(w));
#ifdef PATH_NO_SEARCH
    search = false;
#endif
    [branch] if (search && (w & bloom) == w) {
        uint b = w & (WC_N - 1);
        [branch] if (occupied(b)) {
            uint4 e = wc[b];
            v = e.x == w + 1 ? e.y : e.z == w + 1 ? e.w : v;
        }
    }
    return v;
}
// false: the bucket is full and nothing was stored (the pass ends before the store)
bool store_word(uint w, uint v) {
    uint b = w & (WC_N - 1), t = w + 1;
    uint4 e = 0;
    [branch] if (occupied(b)) e = wc[b];
    [branch] if (e.x == t || e.x == 0) e.xy = uint2(t, v);
    else [branch] if (e.z == t || e.z == 0) e.zw = uint2(t, v);
    else return false;
    wc[b] = e;
    occupy(b);
    bloom |= w;
    return true;
}

uint mulhu(uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
    uint lh = al * bh, hl = ah * bl;
    uint mid = ((al * bl) >> 16) + (lh & 0xffff) + (hl & 0xffff);
    return ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}
#define MASK(x, bit) asuint(asint((x) << (31 - (bit))) >> 31)   // bit of x as all ones or none
// one of eight by three bits of x, the lowest at s0
uint pick8(uint x, uint s0, uint v0, uint v1, uint v2, uint v3, uint v4, uint v5, uint v6, uint v7) {
    bool b0 = (x & s0) != 0, b1 = (x & (s0 << 1)) != 0;
    uint lo = b1 ? (b0 ? v3 : v2) : (b0 ? v1 : v0), hi = b1 ? (b0 ? v7 : v6) : (b0 ? v5 : v4);
    return (x & (s0 << 2)) ? hi : lo;
}

// ---- The fast loop. Returns how many ran; it stops at the budget, at a SYS (left for the general
// step) and at a store with no room (left for the next pass): those two set the budget to 0.
uint run(uint budget, out uint why) {
    uint n = 0;
    why = 0;
#ifdef PREFETCH
    uint4 i = _Code.Load(CODE_AT(pc));
#endif
    [loop] while (n < budget) {
#ifdef PREFETCH
        // the slot after and the branch target asked for now, used at the end (PREFETCH: both)
        uint4 after = _Code.Load(CODE_AT(pc + 1)), there = _Code.Load(CODE_AT(i.x >> 11));
#elif defined(PROBE_NOFETCH)
        uint4 i = _Code.Load(CODE_AT(PROBE_NOFETCH));   // (a probe: the same slot every time)
#else
        uint4 i = _Code.Load(CODE_AT(pc));
#endif
#ifdef PROBE_NOREG
        uint x = i.x, rd = 3, a = r[1] ^ i.y, b = r[2] + i.w + i.z, v, next = pc + 1;   // (a probe: no indexed registers)
#else
        uint x = i.x, rd = i.z & 0xffff, a = r[i.y], b = r[i.z >> 16] + i.w, v, next = pc + 1;
#endif
        int sa = asint(a), sb = asint(b);
#ifdef PATH_CLASS
        uint k = PATH_CLASS;   // (the counts: one class's path compiled alone)
#else
        uint k = x & 3;
#endif
        [branch] if (k == K_INT) {
            // a +- b, and, or, xor, sll, srl, sra, a comparison (0 or 1, or a or b); a branch compares a with b
            // (the comparison as a mask of all ones or none; signed is unsigned with the signs turned over)
            uint t = (x << 25 & 0x80000000) ^ 0x80000000;
            uint c = (x & B_EQ) ? (a == b ? ~0u : 0u) : ((a ^ t) < (b ^ t) ? ~0u : 0u);
            c ^= MASK(x, 8);
#ifdef INT_SPLIT
            v = a + ((x & B_NEG) ? 0 - b : b);
            [branch] if (x & (7 * S0)) {   // (the sum and the branches pass no further)
#endif
            uint cv = (x & B_SELV) ? (c ? a : b) : c & 1;
            v = pick8(x, S0, a + ((x & B_NEG) ? 0 - b : b), a & b, a | b, a ^ b, a << b, a >> b, asuint(sa >> b), cv);
#ifdef INT_SPLIT
            }
#endif
            next = (c & MASK(x, 9)) ? x >> 11 : next;
        } else [branch] if (k == K_MEM) {
            // a word, half or byte at a + b: the part shifted to the top, then down
            uint ad = a + b, w = ad >> 2, up = (x >> UP_SHIFT) & 31, sh = (ad & 3) << 3;
#ifdef PATH_STORE
            [branch] if (PATH_STORE) {
#else
            [branch] if (x & B_STORE) {
#endif
                v = r[rd];   // written back as it was
                uint m = 0xffffffffu >> up << sh, word = v;
                [branch] if (up != 0) word = (load_word(w, true) & ~m) | ((v << sh) & m);
                [branch] if (!store_word(w, word)) { why = 2; next = pc; rd = 64; budget = 0; }
            } else {
                uint y = load_word(w, (x & B_NC) == 0) << (up - sh);
                v = (x & B_MUNS) ? y >> up : asuint(asint(y) >> up);
            }
        } else [branch] if (k == K_FPU) {
            // mad, min, a comparison, sign injection, int to float, float to int, frac, floor
            float fa = asfloat(a ^ ((x & F_NEGA) ? 0x80000000 : 0)), fb = asfloat(b ^ ((x & B_NEG) ? 0x80000000 : 0));
            float fc = asfloat(r[rd] ^ ((x & F_NEGC) ? 0x80000000 : 0));
            float m = (x & F_MULB) ? fb : 1.0, add = (x & F_ADDB) ? fb : (x & F_ADDRD) ? fc : -0.0;
            uint sgn = (x & F_M1) ? a ^ b : (x & F_M0) ? ~b : b;
            uint c = (x & F_M1) ? (fa == fb ? 1u : 0u) : (x & F_M0) ? (fa <= fb ? 1u : 0u) : (fa < fb ? 1u : 0u);
            v = pick8(x, S0, asuint(mad(fa, m, add)), asuint(min(fa, fb)), c, (a & 0x7fffffff) | (sgn & 0x80000000),
                      asuint((float)sa), asuint((int)fa), asuint(frac(fa)), asuint(floor(fa)));
            v ^= (x & F_NEGR) ? 0x80000000 : 0;
        } else {
            float fa = asfloat(a), fb = asfloat(b);
#ifdef PATH_SFU
            [branch] if (PATH_SFU) {
#else
            [branch] if (x & B_SFU) {
#endif
                // sin or cos from one sincos, exp2, log2, sqrt, rsqrt, b / a (rcp: b = 1.0)
                float sn, cs;
                sincos(fa, sn, cs);
                v = pick8(x, S0, asuint(sn), asuint(cs), asuint(exp2(fa)), asuint(log2(fa)),
                          asuint(sqrt(fa)), asuint(rsqrt(fa)), asuint(fb / fa), asuint(fb / fa));
            } else [branch] if (x & B_DIV) {
                bool signed_ = (x & B_SIGNA) != 0, z = b == 0, ovf = signed_ && a == 0x80000000 && b == 0xffffffff;
                uint q = signed_ ? asuint(sa / sb) : a / b, rm = signed_ ? asuint(sa % sb) : a % b;
                v = (x & B_HIGH) ? (z ? a : ovf ? 0 : rm) : (z ? 0xffffffff : ovf ? a : q);
            } else [branch] if (x & B_JALR) {
                v = (pc << 2) + (CODE_BASE + 4);
                next = (a + b - CODE_BASE) >> 2;
            } else [branch] if ((x & B_SYS) == 0) {
                // a product, or its high word with a's and b's signs as asked
                v = (x & B_HIGH) ? mulhu(a, b) - ((x & B_SIGNA) && sa < 0 ? b : 0) - ((x & B_SIGNB) && sb < 0 ? a : 0) : a * b;
            } else {
                why = 1; v = 0; next = pc; rd = 64; budget = 0;
            }
        }
        r[rd] = v;   // (an instruction that stops is counted too, and taken off after the loop)
#ifdef PREFETCH
        uint4 got = next == pc + 1 ? after : there;
        [branch] if (next != pc + 1 && next != (x >> 11)) got = _Code.Load(CODE_AT(next));
        i = got;
#endif
        pc = next;
        n++;
    }
    return why != 0 ? n - 1 : n;
}

// ---- The pass. State texels: 0-16 the registers, 17 the pass's end, 18-19 the occupied
// buckets, from 20 the buckets. Texel t of core c is target t & 7's pixel (t >> 3, c).
#define CTL_T 17
#define OCC_T 18
#define WC_T 20
#define STATE_PIX ((WC_T + WC_N + 7) / 8)
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);   // the state as the last
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);   // pass wrote it
uint4 state_in(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    switch (t & 7) {   // (t is a constant wherever this is called: the switch folds away)
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_in_any(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    uint4 lo = (t & 2) ? v1 : v0, hi = (t & 2) ? v3 : v2;
    return (t & 4) ? hi : lo;
}
struct v2f { float4 vertex : SV_POSITION; };
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
v2f vert(uint id : SV_VertexID) {   // one triangle over the whole target
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}

uint4 state_out(uint t, uint4 ctl) {
    [branch] if (t < CTL_T) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    [branch] if (t < WC_T) return t == CTL_T ? ctl : t == OCC_T ? occ0 : occ1;
    return wc[min(t - WC_T, WC_N - 1)];
}

tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);   // p.y the core, p.x which eight of its texels this pixel writes
    uint k, why;
    [unroll] for (k = 0; k < CTL_T; k++) {
        uint4 s = state_in(k, p.y);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    uint4 ctl = state_in(CTL_T, p.y);   // pc, the last pass's count, why it ended, the count of all passes
    pc = ctl.x;
    uint ran = run(_Ticks, why);
    ctl = uint4(pc, ran, why, ctl.w + ran);
    uint4 s[8];
    [loop] for (k = 0; k < 8; k++) s[k] = state_out(p.x * 8 + k, ctl);
    tick_out o;
    o.t0 = s[0]; o.t1 = s[1]; o.t2 = s[2]; o.t3 = s[3]; o.t4 = s[4]; o.t5 = s[5]; o.t6 = s[6]; o.t7 = s[7];
    return o;
}

// ---- The commit: a point for each pair of each bucket, drawn into _Ram (R32_UINT) where its word
// lies when the bucket was written this pass; the rest are put outside the target.
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint core = id / (2 * WC_N), b = (id >> 1) % WC_N;
    uint4 e = state_in_any(WC_T + b, core), o = state_in_any(OCC_T + (b >> 7), core);
    uint tag = (id & 1) ? e.z : e.x, word = tag - 1;
    bool on = ((o[(b >> 5) & 3] >> (b & 31)) & 1) != 0 && tag != 0;
    c2f r;
    float2 at = (float2(word & ((1u << RAM_SHIFT) - 1), word >> RAM_SHIFT) + 0.5) / float2(1u << RAM_SHIFT, _RamHeight);
    r.vertex = on ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    r.value = (id & 1) ? e.w : e.y;
    return r;
}
uint commit_frag(c2f i) : SV_Target { return i.value; }
