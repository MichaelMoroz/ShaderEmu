// Step 12: groups of four step-1 slots, each place with its role (isabench --roles): 0 the whole integer
// unit, 1 a sum, 2 a sum or a word load or store, 3 a sum or a branch. Reads first, results passed on by
// selects, writes last; the group's store is checked there, and a store to another word's slot drops the
// group and ends the pass. The next group and the branch's are fetched ahead.

Texture2D<uint> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer CodeCb : register(b1) { uint4 _CodeCb[4096]; };   // slot s: (control, ra, rd | rb << 16, imm)
#define FETCH(s) _CodeCb[(s) & 4095]
#define CTL_T 17
#define RAM_AT(w) int3((w) & 8191, (w) >> 13, 0)

#ifndef NREG
#define NREG 16            // registers, a power of two; the last is the sink (isabench --regs)
#endif
#ifndef ARR
#define ARR NREG           // the array's declared size (where the driver keeps it: steps/README.md)
#endif
#define RM (NREG - 1)
static uint r[ARR];
#ifndef WCN
#define WCN 32             // the write cache: (word + 1, value) pairs by the word's low bits, a bit each taken
#endif
#define WC_T 20
static uint2 wc[WCN];
static uint occ = 0;
static uint pc;
static bool stop;

bool compare(uint x, uint a, uint b) {
    bool lt = (x & 64) ? a < b : (int)a < (int)b;
    return ((x & 128) ? a == b : lt) != ((x & 256) != 0);
}
uint alu(uint x, uint a, uint b) {
    uint sh = b & 31, s = (x >> 3) & 7;
    uint sum = a + ((x & 4) ? 0u - b : b);
    uint lo = (s & 2) ? ((s & 1) ? a ^ b : a | b) : ((s & 1) ? a & b : sum);
    uint hi = (s & 2) ? ((s & 1) ? (compare(x, a, b) ? 1u : 0u) : (uint)((int)a >> sh)) : ((s & 1) ? a >> sh : a << sh);
    return (s & 4) ? hi : lo;
}
uint sum(uint x, uint a, uint b) { return a + ((x & 7) == 4 ? 0u - b : b); }   // bit 2 subtracts in an integer op only
uint pass_on(uint v, uint rv, uint4 q, uint d) { return rv == (q.z & RM) ? d : v; }   // v, or op q's result d

uint run(uint budget) {
    uint n = 0;
    uint4 c0 = FETCH(4 * pc), c1 = FETCH(4 * pc + 1), c2 = FETCH(4 * pc + 2), c3 = FETCH(4 * pc + 3);
    stop = false;
    [loop] while (n < budget) {
        uint4 q0 = c0, q1 = c1, q2 = c2, q3 = c3;
        uint tg = q3.x >> 11;
        uint4 n0 = FETCH(4 * pc + 4), n1 = FETCH(4 * pc + 5), n2 = FETCH(4 * pc + 6), n3 = FETCH(4 * pc + 7);
        uint4 t0 = FETCH(4 * tg), t1 = FETCH(4 * tg + 1), t2 = FETCH(4 * tg + 2), t3 = FETCH(4 * tg + 3);
        uint a0 = r[q0.y & RM], b0 = r[(q0.z >> 16) & RM];
        uint a1 = r[q1.y & RM], b1 = r[(q1.z >> 16) & RM];
        uint a2 = r[q2.y & RM], b2 = r[(q2.z >> 16) & RM], d2 = r[q2.z & RM];
        uint a3 = r[q3.y & RM], b3 = r[(q3.z >> 16) & RM];
        uint v0 = alu(q0.x, a0, b0 + q0.w);
        a1 = pass_on(a1, q1.y & RM, q0, v0); b1 = pass_on(b1, (q1.z >> 16) & RM, q0, v0);
        uint v1 = sum(q1.x, a1, b1 + q1.w);
        a2 = pass_on(pass_on(a2, q2.y & RM, q0, v0), q2.y & RM, q1, v1);
        b2 = pass_on(pass_on(b2, (q2.z >> 16) & RM, q0, v0), (q2.z >> 16) & RM, q1, v1);
        d2 = pass_on(pass_on(d2, q2.z & RM, q0, v0), q2.z & RM, q1, v1);
        uint v2 = sum(q2.x, a2, b2 + q2.w);
        uint sw = 0, sk = 0;
        bool st = false;
        [branch] if ((q2.x & 3) == 1) {   // a load or a store
            uint w = v2 >> 2, k = w & (WCN - 1);
            uint2 e = wc[k];
            bool mine = ((occ >> k) & 1) != 0 && e.x == w + 1;
            st = (q2.x & 4) != 0;
            v2 = st ? d2 : (mine ? e.y : _Ram.Load(RAM_AT(w)));
            sw = w; sk = k;
            stop = st && ((occ >> k) & 1) != 0 && !mine;
        }
        a3 = pass_on(pass_on(pass_on(a3, q3.y & RM, q0, v0), q3.y & RM, q1, v1), q3.y & RM, q2, v2);
        b3 = pass_on(pass_on(pass_on(b3, (q3.z >> 16) & RM, q0, v0), (q3.z >> 16) & RM, q1, v1), (q3.z >> 16) & RM, q2, v2);
        b3 += q3.w;
        uint v3 = sum(q3.x, a3, b3);
        bool t = (q3.x & 512) && compare(q3.x, a3, b3);
        r[stop ? RM : q0.z & RM] = v0;
        r[stop ? RM : q1.z & RM] = v1;
        r[stop ? RM : q2.z & RM] = v2;
        r[stop ? RM : q3.z & RM] = v3;
        [branch] if (st && !stop) { wc[sk] = uint2(sw + 1, d2); occ |= 1u << sk; }
        n += stop ? 0 : (q0.z >> 8) & 15;
        pc = stop ? pc : (t ? tg : pc + 1);
        budget = stop ? 0 : budget;
        c0 = t ? t0 : n0; c1 = t ? t1 : n1; c2 = t ? t2 : n2; c3 = t ? t3 : n3;
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
