// Step 2 (README.md): step 1 and a word load. Bits 0-1 of the control word: 0 the add (and branch),
// 1 a load of the word at a + b from RAM (R32_UINT, a word a texel).

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
static uint pc;

uint run(uint budget) {
    uint n = 0;
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
        [branch] if (x & 1) v = _Ram.Load(RAM_AT(v >> 2));
        r[i.z & RM] = v;
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
