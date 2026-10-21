// Step 20: s18 with code and data in one RAM of 16-byte texels (isabench --ram4): an op is a texel, fetched with
// one load (pc is its texel; a branch's target in bits 20-31 is relative to it), a word is picked from its texel,
// and the write cache keeps whole texels, filled from RAM at a texel's first store and drawn whole by the commit.
// (control | rg << 12 | target << 20, ra | rf << 8 | re << 16 | imm8 << 24, rd | count << 8 | rb << 16, imm)

Texture2D<uint4> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
#define TEX_AT(t) int3((t) & 2047, (t) >> 11, 0)
#if CODE_COPY              // (probe: code from a copy in a texture of its own, at texel 0x20000 of RAM)
Texture2D<uint4> _Code : register(t0);
#define FETCH(t) _Code.Load(int3(((t) - 0x20000) & 4095, ((t) - 0x20000) >> 12, 0))
#else
#define FETCH(t) _Ram.Load(TEX_AT(t))
#endif
#define CTL_T 17

#define NREG 16
#define RM 15
#ifndef ARR
#define ARR 64             // the array's declared size (steps/README.md: where the driver keeps it)
#endif
static uint r[ARR];
#ifndef WCN
#define WCN 32             // texels; declared 64
#endif
#define WC_T 20            // an entry is two state texels: (texel + 1) and the texel
#ifndef WCD
#define WCD 64             // the write cache's declared size
#endif
static uint wct[WCD];
static uint4 wcd[WCD];
static uint occ = 0;
static uint pc;
static bool stop;

uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
uint4 put(uint4 q, uint i, uint v) { return uint4(i == 0 ? v : q.x, i == 1 ? v : q.y, i == 2 ? v : q.z, i == 3 ? v : q.w); }

uint run(uint budget) {
    uint n = 0, cnt = 0;
    uint4 cur = FETCH(pc);
    stop = false;
    [loop] while (n < budget) {
        uint4 i = cur;
        uint x = i.x, tg = pc + (uint)((int)x >> 20);
        uint4 nx = FETCH(pc + 1), tc = FETCH(tg);
        uint rd = i.z & RM, rb = i.z >> 16, ra = i.y & RM;
        uint re = (i.y >> 16) & RM, rf = (i.y >> 8) & RM, rg = (x >> 12) & RM;
        bool big = (i.y & 32) != 0;
        uint imm8 = big ? i.w : (uint)((int)i.y >> 24);
        uint a = r[ra], b = r[rb] + (big ? 0u : i.w), f0 = r[rf], g0 = r[rg];
        // the integer unit: sel (bits 3-5) sum and or xor sll srl sra cmp
        uint s = x & 56;
        uint sum = a + ((x & 4) ? 0u - b : b);
        uint F = (x & 64) ? 0u : 0x80000000u;
        uint cm = ((x & 128) ? (a == b ? ~0u : 0u) : ((a ^ F) < (b ^ F) ? ~0u : 0u)) ^ ((x & 256) ? ~0u : 0u);
        bool c = cm != 0;
        uint lg = (s & 16) ? ((s & 8) ? a ^ b : a | b) : a & b;
        uint sr = (s & 16) ? (uint)((int)a >> b) : a >> b;
        uint sh = (s == 32) ? a << b : sr;
        uint v = (s & 32) ? ((s == 56) ? cm & 1 : sh) : (s ? lg : sum);
        v = (x & 1024) ? a + (uint)((int)(i.z << 24) >> 28) : v;
        bool mem = (x & 3) == 1, ld2 = (x & 2048) != 0, st2 = (x & 65536) != 0;
        uint l2 = 0;
        [branch] if (mem || ld2) {   // both texels read together; a store that finds a slot taken stops the pass, undone
            uint w = (a + b) >> 2, T = w >> 2, k = T & (WCN - 1);
            uint w2 = st2 ? w + 1 : (f0 + g0 + imm8) >> 2, T2 = w2 >> 2, k2 = T2 & (WCN - 1);
            uint4 q1 = _Ram.Load(TEX_AT(T)), q2 = _Ram.Load(TEX_AT(T2));
            bool tk = (occ >> k) & 1, mine = tk && wct[k] == T + 1;
            bool tk2 = (occ >> k2) & 1, mine2 = tk2 && wct[k2] == T2 + 1;
            uint4 d1 = mine ? wcd[k] : q1, d2 = mine2 ? wcd[k2] : q2;   // the texels as this core sees them
            l2 = pick(d2, w2 & 3);
            [branch] if (mem && (x & 4)) {
                uint d = r[rd];
                v = d;
                stop = (tk && !mine) || (st2 && T2 != T && tk2 && !mine2);
                budget = stop ? 0 : budget;
                [branch] if (!stop) {
                    d1 = put(d1, w & 3, d);
                    d1 = st2 && T2 == T ? put(d1, w2 & 3, g0) : d1;
                    wct[k] = T + 1; wcd[k] = d1; occ |= 1u << k;
                    [branch] if (st2 && T2 != T) { wct[k2] = T2 + 1; wcd[k2] = put(d2, w2 & 3, g0); occ |= 1u << k2; }
                }
                l2 = w2 == w ? d : l2;
            } else if (mem) {
                v = pick(d1, w & 3);
            }
        }
        // the second slot's unit (bits 17-19): sum and or xor sll srl sra sub, on r[rf] and r[rg] + imm8
        uint F2 = rf == rd ? v : f0, G2 = rg == rd ? v : g0, y2 = G2 + imm8, s2s = x & (7u << 17);
        uint lg2 = (s2s & (2u << 17)) ? ((s2s & (1u << 17)) ? F2 ^ y2 : F2 | y2) : F2 & y2;
        uint sr2 = (s2s & (2u << 17)) ? (uint)((int)F2 >> y2) : F2 >> y2;
        uint sh2 = (s2s == (4u << 17)) ? F2 << y2 : sr2;
        uint sum2 = F2 + ((s2s == (7u << 17)) ? 0u - y2 : y2);
        uint s2 = (s2s & (4u << 17)) ? ((s2s == (7u << 17)) ? sum2 : sh2) : (s2s ? lg2 : sum2);
        s2 = (i.y & 4096) ? (F2 + imm8) & G2 : s2;
        s2 = ld2 ? l2 : s2;
        r[rd] = v;
        r[stop ? RM : re] = s2;
        bool t = (x & 512) && c;
        pc = stop ? pc : (t ? tg : pc + 1);
        cur = t ? tc : nx;
        cnt = (i.z >> 8) & 15;
        n += cnt;
    }
    return stop ? n - cnt : n;
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
    [branch] if (t >= WC_T && t < WC_T + 2 * WCN) {   // an entry's tag (0: none), then its texel
        uint k = (t - WC_T) >> 1;
        bool on = ((occ >> k) & 1) != 0;
        return (t & 1) ? wcd[k] : uint4(on ? wct[k] : 0, 0, 0, 0);
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
    [unroll] for (k = 0; k < 8; k++) s[k] = state_out(p.x * 8 + k, ctl);
    tick_out o;
    o.t0 = s[0]; o.t1 = s[1]; o.t2 = s[2]; o.t3 = s[3]; o.t4 = s[4]; o.t5 = s[5]; o.t6 = s[6]; o.t7 = s[7];
    return o;
}

// ---- The commit: a point an entry, its texel drawn whole into _Ram (R32G32B32A32_UINT); an empty one outside
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint4 value : V; };
uint4 state_in_any(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    uint4 lo = (t & 2) ? v1 : v0, hi = (t & 2) ? v3 : v2;
    return (t & 4) ? hi : lo;
}
c2f commit_vert(uint id : SV_VertexID) {
    uint k = id < WCN ? id : 0, tag = id < WCN ? state_in_any(WC_T + 2 * k, 0).x : 0, tex = tag - 1;
    float2 at = (float2(tex & 2047, tex >> 11) + 0.5) / float2(2048, _RamHeight);
    c2f o;
    o.vertex = tag != 0 ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = state_in_any(WC_T + 2 * k + 1, 0);
    return o;
}
uint4 commit_frag(c2f i) : SV_Target { return i.value; }
