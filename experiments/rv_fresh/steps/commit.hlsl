// The commit: a point for each entry of the write cache, where RAM has its texel; whole texels (what the tick did not store it read first).
#include "mem.hlsl"
Texture2D<uint4> _S0 : register(t1); Texture2D<uint4> _S1 : register(t2); Texture2D<uint4> _S2 : register(t3); Texture2D<uint4> _S3 : register(t4);
Texture2D<uint4> _S4 : register(t5); Texture2D<uint4> _S5 : register(t6); Texture2D<uint4> _S6 : register(t7); Texture2D<uint4> _S7 : register(t8);
uint4 texel(uint lin) {
    int3 at = int3((lin >> 3) & 7, lin >> 6, 0);
    switch (lin & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
struct V { float4 p : SV_Position; nointerpolation uint4 d : D; };
V cvert(uint id : SV_VertexID) {
    V o;
    uint tag = texel(16 + id / 4)[id & 3];
    // data: DATA0 is in words: texel DATA0 / 4 on
    o.d = texel(DATA0 / 4 + id);
    float x = (float(tag & 2047) + 0.5) / 2048 * 2 - 1, y = 1 - (float(tag >> 11) + 0.5) / 2048 * 2;
    o.p = tag == EMPTY ? float4(9, 9, 0, 1) : float4(x, y, 0, 1);
    return o;
}
uint4 cfrag(V i) : SV_Target0 { return i.d; }
