// Step 13: s8's op with a sum beside it (isabench --fuse): r[re] = r[rf] +- r[rg] + imm8, done before the op
// (its result passed to the op's operands) or after it (the op's result passed to it). Two RISC-V-level ops a
// trip where neighbours fit: a slot is (control, ra | re << 8 | rf << 16 | imm8 << 24, rd | count << 8 | rb << 16, imm).

Texture2D<uint> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer CodeCb : register(b1) { uint4 _CodeCb[4096]; };
#define FETCH(s) _CodeCb[(s) & 4095]
#define CTL_T 17
#define RAM_AT(w) int3((w) & 8191, (w) >> 13, 0)
#define F_FIRST (1u << 23)   // the sum before the op; rg in bits 24 to 29, bit 30 subtracts it

#ifndef NREG
#define NREG 16
#endif
#define RM (NREG - 1)
#ifndef ARR
#define ARR 64
#endif
static uint r[ARR];
#ifndef WCN
#define WCN 64
#endif
#define WC_T 20
static uint2 wc[WCN];
static uint2 occ2 = 0;     // WCN up to 64: a bit each, two words
static uint pc;
static bool stop;

bool taken_at(uint k) { return ((k & 32 ? occ2.y : occ2.x) >> (k & 31)) & 1; }

uint alu(uint x, uint a, uint b, out bool c) {
    uint sh = b & 31, s = (x >> 3) & 7;
    bool lt = (x & 64) ? a < b : (int)a < (int)b;
    c = ((x & 128) ? a == b : lt) != ((x & 256) != 0);
    uint sum = a + ((x & 4) ? 0u - b : b);
    uint lo = (s & 2) ? ((s & 1) ? a ^ b : a | b) : ((s & 1) ? a & b : sum);
    uint hi = (s & 2) ? ((s & 1) ? (c ? 1u : 0u) : (uint)((int)a >> sh)) : ((s & 1) ? a >> sh : a << sh);
    return (s & 4) ? hi : lo;
}

uint run(uint budget) {
    uint n = 0;
    uint4 cur = FETCH(pc);
    stop = false;
    [loop] while (n < budget) {
        uint4 i = cur;
        uint x = i.x, tg = (x >> 11) & 4095, rd = i.z & RM, ra = i.y & RM, rb = (i.z >> 16) & RM;
        uint re = (i.y >> 8) & RM, rf = (i.y >> 16) & RM, rg = (x >> 24) & RM;
        uint4 nx = FETCH(pc + 1), tc = FETCH(tg);
        uint a = r[ra], B = r[rb], f = r[rf], g = r[rg];
        uint imm8 = (uint)((int)i.y >> 24);
        bool first = (x & F_FIRST) != 0, neg = (x & (1u << 30)) != 0;
        uint s1 = f + (neg ? 0u - g : g) + imm8;
        bool fw = first && re != RM;
        a = fw && ra == re ? s1 : a;
        B = fw && rb == re ? s1 : B;
        uint b = B + i.w;
        bool c = false;
        uint v = alu(x, a, b, c);
        [branch] if ((x & 3) == 1) {   // a load or a store; a store to another word's slot stops the pass, undone
            uint w = (a + b) >> 2, k = w & (WCN - 1);
            uint2 e = wc[k];
            bool tk = taken_at(k), mine = tk && e.x == w + 1;
            [branch] if (x & 4) {
                v = r[rd];
                v = fw && rd == re ? s1 : v;
                stop = tk && !mine;
                wc[k] = stop ? e : uint2(w + 1, v);
                uint bit = stop ? 0 : 1u << (k & 31);
                occ2 |= k & 32 ? uint2(0, bit) : uint2(bit, 0);
                budget = stop ? 0 : budget;
            } else {
                v = mine ? e.y : _Ram.Load(RAM_AT(w));
            }
        }
        uint F = !first && rf == rd ? v : f, G = !first && rg == rd ? v : g;
        uint s2 = F + (neg ? 0u - G : G) + imm8;
        r[rd] = v;
        r[stop ? RM : re] = first ? s1 : s2;
        bool t = (x & 512) && c;
        uint next = t ? tg : pc + 1;
        pc = stop ? pc : next;
        cur = t ? tc : nx;
        n += stop ? 0 : (i.z >> 8) & 15;
    }
    return n;
}

uint4 state_in(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    switch (t & 7) {   // (t is a constant wherever this is called)
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
struct v2f { float4 vertex : SV_POSITION; };
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
v2f vert(uint id : SV_VertexID) {
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}
uint4 state_out(uint t, uint4 ctl) {
    [branch] if (t < NREG / 4) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    [branch] if (t >= WC_T && t < WC_T + WCN / 2) {   // two pairs a texel, a pair with no word as 0
        uint k = 2 * (t - WC_T);
        return uint4(taken_at(k) ? wc[k] : 0, taken_at(k + 1) ? wc[k + 1] : 0);
    }
    return t == CTL_T ? ctl : 0;
}
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    uint k;
    [unroll] for (k = 0; k < NREG / 4; k++) {
        uint4 s = state_in(k, p.y);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    uint4 ctl = state_in(CTL_T, p.y);   // pc, the last pass's count, why it ended, the count of all passes
    pc = ctl.x;
    uint ran = run(_Ticks);
    r[0] = 0;
    ctl = uint4(pc, ran, stop ? 1 : 0, ctl.w + ran);
    uint4 s[8];
    [loop] for (k = 0; k < 8; k++) s[k] = state_out(p.x * 8 + k, ctl);
    tick_out o;
    o.t0 = s[0]; o.t1 = s[1]; o.t2 = s[2]; o.t3 = s[3]; o.t4 = s[4]; o.t5 = s[5]; o.t6 = s[6]; o.t7 = s[7];
    return o;
}

// ---- The commit: a point a pair, drawn into _Ram (R32_UINT) where its word lies; an empty one outside
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
uint4 state_in_any(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    uint4 lo = (t & 2) ? v1 : v0, hi = (t & 2) ? v3 : v2;
    return (t & 4) ? hi : lo;
}
c2f commit_vert(uint id : SV_VertexID) {
    uint4 e = state_in_any(WC_T + (id >> 1), 0);
    uint tag = (id & 1) ? e.z : e.x, word = tag - 1;
    float2 at = (float2(word & 8191, word >> 13) + 0.5) / float2(8192, _RamHeight);
    c2f o;
    o.vertex = tag != 0 ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = (id & 1) ? e.w : e.y;
    return o;
}
uint commit_frag(c2f i) : SV_Target { return i.value; }
