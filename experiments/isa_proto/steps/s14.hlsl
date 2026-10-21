// Step 14: s13 trimmed by its bytecode. The sum beside the op comes after it only (r[re] = r[rf] + r[rg] + imm8,
// reading the op's result); a sum that would come first is a branch's own (bit 10: compare the op's sum with
// r[rb]). Fields lie where one instruction takes them: (control | rg << 12 | target << 20,
// ra | rf << 8 | re << 16 | imm8 << 24, rd | count << 8 | rb << 16, imm); 16 registers.

Texture2D<uint> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer CodeCb : register(b1) { uint4 _CodeCb[4096]; };
#define FETCH(s) _CodeCb[s]   // (the packer keeps pc and targets under 4,096)
#define CTL_T 17
#define RAM_AT(w) int3((w) & 8191, (w) >> 13, 0)

#undef NREG
#define NREG 16
#define RM 15
static uint r[64];         // declared 64 (steps/README.md: where the driver keeps it)
#ifndef WCN
#define WCN 32             // used; declared 64
#endif
#define WC_T 20
static uint2 wc[64];
static uint occ = 0;
static uint pc;
static bool stop;

uint run(uint budget) {
    uint n = 0, cnt = 0;
    uint4 cur = FETCH(pc);
    stop = false;
    [loop] while (n < budget) {
        uint4 i = cur;
        uint x = i.x, tg = x >> 20;
        uint4 nx = FETCH(pc + 1), tc = FETCH(tg);
        uint rd = i.z & RM, rb = i.z >> 16, ra = i.y & RM;
        uint re = (i.y >> 16) & RM, rf = (i.y >> 8) & RM, rg = (x >> 12) & RM;
        uint a = r[ra], b = r[rb] + i.w, f0 = r[rf], g0 = r[rg];
        // the integer unit: sel (bits 3-5) sum and or xor sll srl sra cmp
        uint s = x & 56;
        uint sum = a + ((x & 4) ? 0u - b : b);
        uint F = (x & 64) ? 0u : 0x80000000u;
        uint ca = (x & 1024) ? sum : a, cb = (x & 1024) ? g0 : b;
        uint cm = ((x & 128) ? (ca == cb ? ~0u : 0u) : ((ca ^ F) < (cb ^ F) ? ~0u : 0u)) ^ ((x & 256) ? ~0u : 0u);
        bool c = cm != 0;
        uint lg = (s & 16) ? ((s & 8) ? a ^ b : a | b) : a & b;
        uint sr = (s & 16) ? (uint)((int)a >> b) : a >> b;
        uint sh = (s == 32) ? a << b : sr;
        uint v = (s & 32) ? ((s == 56) ? cm & 1 : sh) : (s ? lg : sum);
        [branch] if ((x & 3) == 1) {   // a load or a store; a store to another word's slot stops the pass, undone
            uint w = (a + b) >> 2, k = w & (WCN - 1);
            uint2 e = wc[k];
            bool tk = (occ >> k) & 1, mine = tk && e.x == w + 1;
            [branch] if (x & 4) {
                v = r[rd];
                stop = tk && !mine;
                wc[k] = stop ? e : uint2(w + 1, v);
                occ |= stop ? 0 : 1u << k;
                budget = stop ? 0 : budget;
            } else {
                v = mine ? e.y : _Ram.Load(RAM_AT(w));
            }
        }
#if NO_SEC                 // (dissection: no sum beside the op)
        r[rd] = v;
#elif SEC_EARLY            // the sum's operands read with the op's, the op's result passed by selects
        r[rd] = v;
        uint F2 = rf == rd ? v : f0, G2 = rg == rd ? v : g0;
        uint s2 = F2 + G2 + (uint)((int)i.y >> 24);
#if SEC_BRANCH
        [branch] if (re != RM && !stop) r[re] = s2;
#else
        r[stop ? RM : re] = s2;
#endif
#else
        r[rd] = v;
        r[stop ? RM : re] = r[rf] + r[rg] + (uint)((int)i.y >> 24);
#endif
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
