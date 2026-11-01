// What every step of the fresh tick shares: the bindings, the vertex shader, reading and writing the state.
Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t1); Texture2D<uint4> _S1 : register(t2); Texture2D<uint4> _S2 : register(t3); Texture2D<uint4> _S3 : register(t4);
Texture2D<uint4> _S4 : register(t5); Texture2D<uint4> _S5 : register(t6); Texture2D<uint4> _S6 : register(t7); Texture2D<uint4> _S7 : register(t8);
cbuffer C : register(b0) { uint _Budget; };
float4 vert(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
struct Out { uint4 o0 : SV_Target0; uint4 o1 : SV_Target1; uint4 o2 : SV_Target2; uint4 o3 : SV_Target3; uint4 o4 : SV_Target4; uint4 o5 : SV_Target5; uint4 o6 : SV_Target6; uint4 o7 : SV_Target7; };
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
#pragma static_noinit
#ifndef XN
#define XN 64
#endif
static uint x[XN];   // registers, [32] the sink
void regs_in() {
    int3 a = int3(0, 0, 0); uint4 t;
    t = _S0.Load(a); x[0] = t.x; x[1] = t.y; x[2] = t.z; x[3] = t.w;
    t = _S1.Load(a); x[4] = t.x; x[5] = t.y; x[6] = t.z; x[7] = t.w;
    t = _S2.Load(a); x[8] = t.x; x[9] = t.y; x[10] = t.z; x[11] = t.w;
    t = _S3.Load(a); x[12] = t.x; x[13] = t.y; x[14] = t.z; x[15] = t.w;
    t = _S4.Load(a); x[16] = t.x; x[17] = t.y; x[18] = t.z; x[19] = t.w;
    t = _S5.Load(a); x[20] = t.x; x[21] = t.y; x[22] = t.z; x[23] = t.w;
    t = _S6.Load(a); x[24] = t.x; x[25] = t.y; x[26] = t.z; x[27] = t.w;
    t = _S7.Load(a); x[28] = t.x; x[29] = t.y; x[30] = t.z; x[31] = t.w;
}
Out state_out(int3 me, uint pc, uint count) {
    Out r;
    [branch] if (me.x == 0 && me.y == 0) {
        r.o0 = uint4(x[0], x[1], x[2], x[3]); r.o1 = uint4(x[4], x[5], x[6], x[7]); r.o2 = uint4(x[8], x[9], x[10], x[11]); r.o3 = uint4(x[12], x[13], x[14], x[15]);
        r.o4 = uint4(x[16], x[17], x[18], x[19]); r.o5 = uint4(x[20], x[21], x[22], x[23]); r.o6 = uint4(x[24], x[25], x[26], x[27]); r.o7 = uint4(x[28], x[29], x[30], x[31]);
    } else {
        r.o0 = uint4(pc, count, 0, 0); r.o1 = 0; r.o2 = 0; r.o3 = 0; r.o4 = 0; r.o5 = 0; r.o6 = 0; r.o7 = 0;
    }
    return r;
}
