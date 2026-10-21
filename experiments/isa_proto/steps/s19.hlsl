// Step 19: s18's ops in 32-bit words, fetched from RAM (isabench --ram): an op is a word, and a second word
// after it is a second op or the op's 32-bit literal. pc is a word's index; each trip reads ahead the two words
// after its own and its target's two. The fields are laid out in bench.cpp ("Words in RAM").

Texture2D<uint> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
#define CTL_T 17
#define RAM_AT(w) int3((w) & 8191, (w) >> 13, 0)

#undef NREG
#define NREG 16
#define RM 15
#ifndef ARR
#define ARR 64             // the array's declared size (steps/README.md: where the driver keeps it)
#endif
static uint r[ARR];
#ifndef WCN
#define WCN 32             // used; declared 64
#endif
#define WC_T 20
#ifndef WCD
#define WCD 64             // the write cache's declared size
#endif
static uint2 wc[WCD];
static uint occ = 0;
static uint pc;
static bool stop;

#define RW(w) _Ram.Load(int3((w) & 8191, (w) >> 13, 0))
#define W_BR 0x8000u
#define W_STEP 0x40000u

uint run(uint budget) {
    uint n = 0, cnt = 0;
    uint c0 = RW(pc), c1 = RW(pc + 1);
    stop = false;
    [loop] while (n < budget) {
        uint x = c0, y = c1;
        uint nxt = x & (3u << 19);
        bool pair = nxt == (1u << 19), lit = nxt == (2u << 19), br = (x & W_BR) != 0;
        uint len = nxt ? 2 : 1;
        uint imm = lit && !br ? y : (uint)((int)(x << 4) >> 25);
        uint tg = pc + (lit ? y : (uint)((int)(x << 20) >> 24));
        uint n0 = RW(pc + len), n1 = RW(pc + len + 1), t0 = RW(tg), t1 = RW(tg + 1);
        uint ra = x & RM, rb = x >> 28;
        uint rd = br ? ((x & W_STEP) ? ra : RM) : (x >> 4) & RM;
        uint rf = y & RM, rg = y >> 28, re = pair ? (y >> 4) & RM : RM;
        uint imm2 = (uint)((int)(y << 4) >> 18);
        uint a = r[ra], b = r[rb] + imm, f0 = r[rf], g0 = r[rg];
        // the integer unit: sel (bits 8-10) sum and or xor sll srl sra cmp
        uint s = x & 0x700;
        uint sum = a + ((x & 0x800) ? 0u - b : b);
        uint F = (x & 0x1000) ? 0u : 0x80000000u;
        uint cm = ((x & 0x2000) ? (a == b ? ~0u : 0u) : ((a ^ F) < (b ^ F) ? ~0u : 0u)) ^ ((x & 0x4000) ? ~0u : 0u);
        bool c = cm != 0;
        uint lg = (s & 0x200) ? ((s & 0x100) ? a ^ b : a | b) : a & b;
        uint sr = (s & 0x200) ? (uint)((int)a >> b) : a >> b;
        uint sh = (s == 0x400) ? a << b : sr;
        uint v = (s & 0x400) ? ((s == 0x700) ? cm & 1 : sh) : (s ? lg : sum);
        v = (x & W_STEP) ? a - imm : v;
        bool mem = (x & 0x10000) != 0, ld2 = pair && (y & 0x800), st2 = pair && (y & 0x1000);
        uint l2 = 0;
        [branch] if (mem || ld2) {   // both words' slots and RAM read together; a store that finds a slot taken stops the pass, undone
            uint w = (a + b) >> 2, k = w & (WCN - 1);
            uint w2 = st2 ? w + 1 : (f0 + g0 + imm2) >> 2, k2 = w2 & (WCN - 1);
            uint2 e = wc[k], e2 = wc[k2];
            uint m1 = RW(w), m2 = RW(w2);
            bool tk = (occ >> k) & 1, mine = tk && e.x == w + 1;
            bool tk2 = (occ >> k2) & 1, mine2 = tk2 && e2.x == w2 + 1;
            l2 = mine2 ? e2.y : m2;
            [branch] if (mem && (x & 0x800)) {
                uint d = r[rd];
                v = d;
                stop = (tk && !mine) || (st2 && tk2 && !mine2);
                budget = stop ? 0 : budget;
                [branch] if (!stop) {
                    wc[k] = uint2(w + 1, d);
                    occ |= 1u << k;
                    [branch] if (st2) { wc[k2] = uint2(w2 + 1, g0); occ |= 1u << k2; }
                }
                l2 = w2 == w ? d : l2;
            } else if (mem) {
                v = mine ? e.y : m1;
            }
        }
        // the second word's unit (bits 8-10): sum and or xor sll srl sra sub, on r[rf] and r[rg] + imm14
        uint F2 = rf == rd ? v : f0, G2 = rg == rd ? v : g0, y2 = G2 + imm2, s2s = y & 0x700;
        uint lg2 = (s2s & 0x200) ? ((s2s & 0x100) ? F2 ^ y2 : F2 | y2) : F2 & y2;
        uint sr2 = (s2s & 0x200) ? (uint)((int)F2 >> y2) : F2 >> y2;
        uint sh2 = (s2s == 0x400) ? F2 << y2 : sr2;
        uint sum2 = F2 + ((s2s == 0x700) ? 0u - y2 : y2);
        uint s2 = (s2s & 0x400) ? ((s2s == 0x700) ? sum2 : sh2) : (s2s ? lg2 : sum2);
        s2 = (y & 0x2000) ? (F2 + imm2) & G2 : s2;
        s2 = ld2 ? l2 : s2;
        r[rd] = v;
        r[stop ? RM : re] = s2;
        bool t = br && c;
        pc = stop ? pc : (t ? tg : pc + len);
        c0 = t ? t0 : n0;
        c1 = t ? t1 : n1;
        cnt = 1 - ((x >> 17) & 1) + ((x >> 18) & 1) + (pair ? 1 + ((y >> 13) & 1) : 0);
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
    [branch] if (t >= WC_T && t < WC_T + WCN / 2) {   // two pairs a texel, a pair with no word as 0
        uint k = 2 * (t - WC_T);
        return uint4(((occ >> k) & 1) ? wc[k] : 0, ((occ >> (k + 1)) & 1) ? wc[k + 1] : 0);
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
