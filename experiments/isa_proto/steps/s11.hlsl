// Step 11: the whole integer unit and memory in groups (isabench --decoded K). An op is two words of four,
// decoded ahead: (ra, rb, rd | target << 8, imm) and (negation mask, AND mask, XOR mask, control). A group's
// reads come first, results pass to later ops by selects, its writes come last; its one store is checked
// and written there, and a store to another word's slot drops the whole group and ends the pass.
#ifndef GROUP
#define GROUP 4
#endif

Texture2D<uint> _Ram : register(t1);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer CodeCb : register(b1) { uint4 _CodeCb[4096]; };
#define FETCH(s) _CodeCb[(s) & 4095]
#define CTL_T 17
#define RAM_AT(w) int3((w) & 8191, (w) >> 13, 0)

// the control word (bench.cpp, "Decoded groups")
#define D_SH 1u
#define D_SHR 2u
#define D_SRA 4u
#define D_CMP 8u
#define D_LG 16u
#define D_EQ 32u
#define D_INV 64u
#define D_BR 128u
#define D_MEM 256u
#define D_ST 512u

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

uint run(uint budget) {
    uint n = 0;
    stop = false;
    [loop] while (n < budget) {
        uint4 q0[GROUP], q1[GROUP];
        uint A[GROUP], B[GROUP], D[GROUP], V[GROUP];
        [unroll] for (uint f = 0; f < GROUP; f++) {
            q0[f] = FETCH(2 * (GROUP * pc + f)); q1[f] = FETCH(2 * (GROUP * pc + f) + 1);
            A[f] = r[q0[f].x]; B[f] = r[q0[f].y]; D[f] = r[q0[f].z & RM];
        }
        bool c = false;
        uint sw = 0, sv = 0, sk = 0;   // the group's store: its word, value and slot
        bool st = false;
        [unroll] for (uint g = 0; g < GROUP; g++) {
            uint ra = q0[g].x, rb = q0[g].y, x = q1[g].w;
            uint a = A[g], b = B[g], d = D[g];
            [unroll] for (uint h = 0; h < GROUP; h++) if (h < g) {
                uint rd = q0[h].z & RM;
                a = ra == rd ? V[h] : a; b = rb == rd ? V[h] : b; d = (q0[g].z & RM) == rd ? V[h] : d;
            }
            uint bb = (b ^ q1[g].x) + q0[g].w;
            uint sum = a + bb;
            uint lg = ((a & bb) & q1[g].y) ^ ((a ^ bb) & q1[g].z);
            uint sr = (x & D_SRA) ? (uint)((int)a >> bb) : a >> bb;
            uint sh = (x & D_SHR) ? sr : a << bb;
            uint F = x & 0x80000000u;
            bool lt = (a ^ F) < (bb ^ F);
            c = ((x & D_EQ) ? a == bb : lt) != ((x & D_INV) != 0);
            uint v = (x & D_SH) ? sh : ((x & D_LG) ? lg : sum);
            v = (x & D_CMP) ? (c ? 1u : 0u) : v;
            [branch] if (x & D_MEM) {
                uint w = sum >> 2, k = w & (WCN - 1);
                uint2 e = wc[k];
                bool mine = ((occ >> k) & 1) != 0 && e.x == w + 1;
                v = (x & D_ST) ? d : (mine ? e.y : _Ram.Load(RAM_AT(w)));
                st = st || (x & D_ST) != 0;
                sw = (x & D_ST) ? w : sw; sv = (x & D_ST) ? d : sv; sk = (x & D_ST) ? k : sk;
            }
            V[g] = v;
        }
        // the store, then every write; a store to another word's slot undoes the group
        uint2 se = wc[sk];
        bool taken = ((occ >> sk) & 1) != 0;
        stop = st && taken && se.x != sw + 1;
        [unroll] for (uint e = 0; e < GROUP; e++) r[stop ? RM : q0[e].z & RM] = V[e];
        [branch] if (st && !stop) { wc[sk] = uint2(sw + 1, sv); occ |= 1u << sk; }
        bool t = (q1[GROUP - 1].w & D_BR) && c;
        uint next = t ? q0[GROUP - 1].z >> 8 : pc + 1;
        n += stop ? 0 : (q1[0].w >> 16) & 15;
        pc = stop ? pc : next;
        budget = stop ? 0 : budget;
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
