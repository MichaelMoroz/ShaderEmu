// Step 5: step 1's slots two to a bundle as they are (isabench --pairs, --par): two reads of the code, the
// second may branch (to bundle x >> 11), bit 15 of its third word: the first is none. PAR: both read first.

Texture2D<uint4> _Code : register(t0);   // slot s: (control, ra, rd | rb << 16, immediate)
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
StructuredBuffer<uint4> _CodeBuf : register(t10);
cbuffer CodeCb : register(b1) { uint4 _CodeCb[4096]; };
#define CODE_AT(s) int3((s) & 4095, (s) >> 12, 0)
#define CTL_T 17

#if CODE_SRC == 1          // where the code is read: a texture (0), a structured buffer (1), a constant buffer (2)
#define FETCH(s) _CodeBuf[s]
#elif CODE_SRC == 2
#define FETCH(s) _CodeCb[(s) & 4095]
#else
#define FETCH(s) _Code.Load(CODE_AT(s))
#endif

#ifndef NREG
#define NREG 64            // registers, a power of two; the last is the sink (isabench --regs)
#endif
#define RM (NREG - 1)
#if REG_SEL                // eight registers as values, read and written by selects (no indexed array)
static uint r0, r1, r2, r3, r4, r5, r6, r7;
#define RD4(k, a, b, c, d) ((k) & 2 ? ((k) & 1 ? d : c) : ((k) & 1 ? b : a))
#define RD(k) ((k) & 4 ? RD4(k, r4, r5, r6, r7) : RD4(k, r0, r1, r2, r3))
#define WR(k, v) { uint k_ = (k), v_ = (v); r1 = k_ == 1 ? v_ : r1; r2 = k_ == 2 ? v_ : r2; r3 = k_ == 3 ? v_ : r3;     r4 = k_ == 4 ? v_ : r4; r5 = k_ == 5 ? v_ : r5; r6 = k_ == 6 ? v_ : r6; r7 = k_ == 7 ? v_ : r7; }
#else
static uint r[NREG];
#define RD(k) r[k]
#define WR(k, v) r[k] = (v)
#endif
static uint pc;

uint run(uint budget) {
    uint n = 0;
#if FETCH_AHEAD
    uint4 cur0 = FETCH(2 * pc), cur1 = FETCH(2 * pc + 1);
#endif
    [loop] while (n < budget) {
#if defined(PROBE_LOOP)   // (probes: the loop alone; the fetch and the pc alone. Their results are not the reference's)
        pc = pc * 3 + 1; n++; continue;
#endif
#if FETCH_AHEAD           // the next bundle and the branch's target asked for before this one runs
        uint4 i = cur0, j = cur1;
        uint4 nx0 = FETCH(2 * pc + 2), nx1 = FETCH(2 * pc + 3), tg0 = FETCH(2 * (j.x >> 11)), tg1 = FETCH(2 * (j.x >> 11) + 1);
#else
        uint4 i = FETCH(2 * pc), j = FETCH(2 * pc + 1);
#endif
        uint a0 = RD(i.y & RM), b0 = RD((i.z >> 16) & RM) + i.w;
#if PAR
        uint a = RD(j.y & RM), b = RD((j.z >> 16) & RM) + j.w;
        WR(i.z & RM, a0 + b0);
#else
        WR(i.z & RM, a0 + b0);
        uint a = RD(j.y & RM), b = RD((j.z >> 16) & RM) + j.w;
#endif
        WR(j.z & RM, a + b);
        uint x = j.x;
        bool c = (a == b) != ((x & 256) != 0);
        bool t = (x & 512) && c;
        pc = t ? x >> 11 : pc + 1;
#if FETCH_AHEAD
        cur0 = t ? tg0 : nx0; cur1 = t ? tg1 : nx1;
#endif
        n += (j.z & 0x8000) ? 1 : 2;
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
#if REG_SEL
    if (t == 0) return uint4(0, r1, r2, r3);
    if (t == 1) return uint4(r4, r5, r6, r7);
#else
    [branch] if (t < NREG / 4) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
#endif
    return t == CTL_T ? ctl : 0;
}
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    uint k;
#if REG_SEL
    uint4 s0 = state_in(0, p.y);
    uint4 s1 = state_in(1, p.y);
    r0 = 0; r1 = s0.y; r2 = s0.z; r3 = s0.w; r4 = s1.x; r5 = s1.y; r6 = s1.z; r7 = s1.w;
#else
    [unroll] for (k = 0; k < NREG / 4; k++) {
        uint4 s = state_in(k, p.y);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
#endif
    uint4 ctl = state_in(CTL_T, p.y);   // pc, the last pass's count, why it ended, the count of all passes
    pc = ctl.x;
    uint ran = run(_Ticks);
#if !REG_SEL
    r[0] = 0;
#endif
    ctl = uint4(pc, ran, 0, ctl.w + ran);
    uint4 s[8];
    [loop] for (k = 0; k < 8; k++) s[k] = state_out(p.x * 8 + k, ctl);
    tick_out o;
    o.t0 = s[0]; o.t1 = s[1]; o.t2 = s[2]; o.t3 = s[3]; o.t4 = s[4]; o.t5 = s[5]; o.t6 = s[6]; o.t7 = s[7];
    return o;
}
