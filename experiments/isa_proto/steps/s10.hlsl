// Step 10: step 1's ops GROUP to a trip (isabench --group K): a group's slots are read together, one fetch
// apart from the next group's, done one after the other; only the last may branch, to a group.
#ifndef GROUP
#define GROUP 4
#endif

Texture2D<uint4> _Code : register(t0);
cbuffer Pass : register(b0) { uint _Ticks; uint _RamHeight; uint2 _pad; };
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer CodeCb : register(b1) { uint4 _CodeCb[4096]; };   // slot s: (control, ra, rd | rb << 16, imm)
#define FETCH(s) _CodeCb[(s) & 4095]
#define CTL_T 17

#ifndef NREG
#define NREG 16            // registers, a power of two; the last is the sink (isabench --regs)
#endif
#ifndef ARR
#define ARR 64             // the array's declared size (where the driver keeps it: steps/README.md)
#endif
#define RM (NREG - 1)
static uint r[ARR];
static uint pc, keep;

uint run(uint budget) {
    uint n = 0, k;
    uint4 cur[GROUP];
    [unroll] for (k = 0; k < GROUP; k++) cur[k] = FETCH(GROUP * pc + k);
    [loop] while (n < budget) {
        uint4 q[GROUP], nx[GROUP], tc[GROUP];
        [unroll] for (k = 0; k < GROUP; k++) q[k] = cur[k];
        uint tg = q[GROUP - 1].x >> 11;
        [unroll] for (k = 0; k < GROUP; k++) { nx[k] = FETCH(GROUP * (pc + 1) + k); tc[k] = FETCH(GROUP * tg + k); }
        uint a = 0, b = 0;
#if FWD                    // every read first, an earlier op's result passed on by a select, every write last
        uint A[GROUP], B[GROUP], V[GROUP];
        #if PROBE_NOREAD           // (probes: no reads from the array; no writes to it. Not the reference's results)
        [unroll] for (uint f = 0; f < GROUP; f++) { A[f] = q[f].y; B[f] = q[f].z; }
#else
        [unroll] for (uint f = 0; f < GROUP; f++) { A[f] = r[q[f].y & RM]; B[f] = r[(q[f].z >> 16) & RM]; }
#endif
        [unroll] for (uint g = 0; g < GROUP; g++) {
            uint ra = q[g].y & RM, rb = (q[g].z >> 16) & RM;
            a = A[g]; b = B[g];
            [unroll] for (uint h = 0; h < GROUP; h++) if (h < g) { uint d = q[h].z & RM; a = ra == d ? V[h] : a; b = rb == d ? V[h] : b; }
            b += q[g].w;
            V[g] = a + b;
        }
        #if PROBE_NOWRITE
        [unroll] for (uint e = 0; e < GROUP; e++) keep ^= V[e];
#else
        [unroll] for (uint e = 0; e < GROUP; e++) r[q[e].z & RM] = V[e];
#endif
#else
        [unroll] for (k = 0; k < GROUP; k++) {
            a = r[q[k].y & RM]; b = r[(q[k].z >> 16) & RM] + q[k].w;
            r[q[k].z & RM] = a + b;
        }
#endif
        uint x = q[GROUP - 1].x;
        bool t = (x & 512) && ((a == b) != ((x & 256) != 0));
        n += (q[0].z >> 8) & 15;
        pc = t ? tg : pc + 1;
        [unroll] for (k = 0; k < GROUP; k++) cur[k] = t ? tc[k] : nx[k];
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
    ctl = uint4(pc, ran, keep, ctl.w + ran);
    uint4 s[8];
    [loop] for (k = 0; k < 8; k++) s[k] = state_out(p.x * 8 + k, ctl);
    tick_out o;
    o.t0 = s[0]; o.t1 = s[1]; o.t2 = s[2]; o.t3 = s[3]; o.t4 = s[4]; o.t5 = s[5]; o.t6 = s[6]; o.t7 = s[7];
    return o;
}
