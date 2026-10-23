// fetchlat: the latency of each way a pixel shader can fetch a word, as a dependent chain (fetchlat.cpp): one core
// (an 8 x 4 block), every fetch's address the last fetch's word, the chain in 512 words (L1's). MODE:
//   0 Texture2D<uint4> Load, the 8192-wide RAM texture     1 Texture2D<uint> (R32_UINT)       2 Texture2D<uint2> (R32G32)
//   3 Load with a constant offset (int2(1, 0))               4 GatherRed of R32_UINT (.w)      5 SampleLevel, point, R32G32B32A32_FLOAT
//   6 a 256 x 256 texture's mip 0                            7 the RAM texture as a render target drew it last
//   8 ByteAddressBuffer                                      9 StructuredBuffer<uint4>        10 a cbuffer's uint4[512], uniform index
//   11 Sample (implicit level 0), point                      12 the RAM texture with the chain's words 64 texels apart (one a cache line)
Texture2D<uint4> _T4 : register(t0);
Texture2D<uint> _T1 : register(t1);
Texture2D<uint2> _T2 : register(t2);
Texture2D<float4> _TF : register(t3);
Texture2D<uint4> _TS : register(t4);
Texture2D<uint4> _TR : register(t5);
ByteAddressBuffer _BA : register(t6);
StructuredBuffer<uint4> _SB : register(t7);
SamplerState _Pt : register(s0);
cbuffer P : register(b0) { uint _N, _Seed, _p0, _p1; };
cbuffer C : register(b1) { uint4 _C[512]; };
static const uint NCAP = 200000;
uint4 frag(float4 pos : SV_POSITION) : SV_Target {
    uint x = _Seed & 511, k, n = min(_N, NCAP);
    [loop] for (k = 0; k < n; k++) {
#if MODE == 0
        x = _T4.Load(int3(x, 0, 0)).x;
#elif MODE == 1
        x = _T1.Load(int3(x, 0, 0));
#elif MODE == 2
        x = _T2.Load(int3(x, 0, 0)).x;
#elif MODE == 3
        x = _T4.Load(int3(x, 0, 0), int2(1, 0)).x & 511;
#elif MODE == 4
        x = _T1.GatherRed(_Pt, float2((float)x + 1.0, 0.5) / float2(8192.0, 8.0)).w;   // (the 2 x 2 at x, x + 1: .w is x's)
#elif MODE == 5
        x = asuint(_TF.SampleLevel(_Pt, float2((float)x + 0.5, 0.5) / float2(8192.0, 8.0), 0).x);
#elif MODE == 6
        x = _TS.Load(int3(x & 255, x >> 8, 0)).x;
#elif MODE == 7
        x = _TR.Load(int3(x, 0, 0)).x;
#elif MODE == 8
        x = _BA.Load(x * 4);
#elif MODE == 9
        x = _SB[x].x;
#elif MODE == 10
        x = _C[x].x;
#elif MODE == 11
        x = asuint(_TF.Sample(_Pt, float2((float)x + 0.5, 0.5) / float2(8192.0, 8.0)).x);
#elif MODE == 12
        x = _T4.Load(int3((x * 64) & 8191, (x >> 7) & 7, 0)).y;
#endif
    }
    return uint4(x, 0, 0, 0);
}
float4 vert(uint id : SV_VertexID) : SV_POSITION { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
// the render target's fill (MODE 7's texture) and the float texture's check: each pixel its texel of _TF, as bits
uint4 fill_frag(float4 pos : SV_POSITION) : SV_Target { uint i = (uint)pos.x; return uint4((i * 37u + 11u) & 511u, 0, 0, 0); }
uint4 check_frag(float4 pos : SV_POSITION) : SV_Target {
    float2 uv = pos.xy / float2(8192.0, 8.0);
    return asuint(_TF.SampleLevel(_Pt, uv, 0));
}
