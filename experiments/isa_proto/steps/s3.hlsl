// Step 3 (README.md): step 2 and a word store into the simplest write cache: WCN (word + 1, value)
// pairs, direct-mapped by the word's low bits; a store that finds its slot taken ends the pass.
// WC_CLEAR: the array emptied at every pass's start; else a bit a slot (WCN <= 32).

Texture2D<uint4> _Code : register(t0);   // slot s: (control, ra, rd | rb << 16, immediate)
Texture2D<uint> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
#define CODE_AT(s) int3((s) & 4095, (s) >> 12, 0)
#define CTL_T 17
#define RAM_AT(w) int3((w) & 8191, (w) >> 13, 0)

#ifndef NREG
#define NREG 64            // registers, a power of two; the last is the sink (isabench --regs)
#endif
#define RM (NREG - 1)
static uint r[NREG];
#ifndef WCN
#define WCN 32
#endif
#define WC_T 20            // the state texel of the first two pairs
static uint2 wc[WCN];
#ifndef WC_CLEAR
static uint occ = 0;
#define TAKEN(k) ((occ >> (k)) & 1)
#else
#define TAKEN(k) (wc[k].x != 0)
#endif
static uint pc;

uint run(uint budget) {
    uint n = 0;
#ifdef WC_CLEAR
    [loop] for (uint k = 0; k < WCN; k++) wc[k] = 0;
#endif
    [loop] while (n < budget) {
#if defined(PROBE_LOOP)   // (probes: the loop alone; the fetch and the pc alone. Their results are not the reference's)
        pc = pc * 3 + 1; n++; continue;
#endif
        uint4 i = _Code.Load(CODE_AT(pc));
#if defined(PROBE_FETCH)
        pc = (i.x & 512) ? i.x >> 11 : pc + 1; n++; continue;
#endif
        uint x = i.x, a = r[i.y & RM], b = r[(i.z >> 16) & RM] + i.w;
        uint v = a + b;
        bool c = (a == b) != ((x & 256) != 0);
        uint next = (x & 512) && c ? x >> 11 : pc + 1;
        uint rd = i.z & RM;
        [branch] if (x & 3) {
            uint w = v >> 2, k = w & (WCN - 1);
            uint2 e = wc[k];
            bool mine = TAKEN(k) && e.x == w + 1;
            [branch] if (x & 4) {   // a store of rd, written back as it was
                v = r[rd];
                [branch] if (TAKEN(k) && !mine) { budget = 0; break; }   // (the pass ends before it; n is not counted)
                wc[k] = uint2(w + 1, v);
#ifndef WC_CLEAR
                occ |= 1u << k;
#endif
            } else {
                v = mine ? e.y : _Ram.Load(RAM_AT(w));
            }
        }
        r[rd] = v;
        pc = next;
        n++;
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
        return uint4(TAKEN(k) ? wc[k] : 0, TAKEN(k + 1) ? wc[k + 1] : 0);
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
    ctl = uint4(pc, ran, 0, ctl.w + ran);
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
