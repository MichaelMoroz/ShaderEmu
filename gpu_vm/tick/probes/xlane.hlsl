// xlane: cross-lane exchange inside a core's warp (xlane.cpp). One core, an 8 x 4 block; lane k = the pixel (k % 8, k / 8).
//   MODE 0: InterlockedAdd on an unbound RWStructuredBuffer<uint> (u8), lane k adds V(k); the pixel gives back orig
//   MODE 1: the same, a dependent chain of _N of them (timed)
//   MODE 2: a quad exchange by ddx_fine / ddy_fine: each pixel gets its horizontal and vertical neighbours' 32-bit words
//           (two 16-bit halves as floats, exact); the pixel gives back both
//   MODE 3: a dependent chain of horizontal exchanges (timed)
//   MODE 4: the chain's reference: the same loop with no exchange (timed)
RWStructuredBuffer<uint> _U : register(u8);   // (fxc2 has no method form: the intrinsic on element 0)
cbuffer P : register(b0) { uint _N, _Seed, _Kind, _p; };
static const uint NCAP = 200000;
uint xch(uint v, bool other, bool horiz) {   // the quad neighbour's word (horizontally or vertically)
    float lo = (float)(v & 0xffffu), hi = (float)(v >> 16);
    float dlo = horiz ? ddx_fine(lo) : ddy_fine(lo), dhi = horiz ? ddx_fine(hi) : ddy_fine(hi);
    float nlo = other ? lo - dlo : lo + dlo, nhi = other ? hi - dhi : hi + dhi;   // (the right or lower pixel: minus)
    return (uint)nlo | (uint)nhi << 16;
}
uint4 frag(float4 pos : SV_POSITION) : SV_Target {
    uint2 p = uint2(pos.xy);
    uint k = p.y * 8 + p.x, n = min(_N, NCAP), i;
    uint v = _Kind == 0 ? 1u << k : k + 1;
#if MODE == 0
    uint o;
    InterlockedAdd(_U[0], v, o);
    return uint4(o, k, 0, 0);
#elif MODE == 1
    uint x = v;
    [loop] for (i = 0; i < n; i++) { uint o; InterlockedAdd(_U[0], (x & 7) | 1, o); x = (o ^ x) + 1; }
    return uint4(x, k, 0, 0);
#elif MODE == 2
    uint w = (k * 0x9e3779b9u) ^ _Seed;
    return uint4(w, xch(w, (p.x & 1) != 0, true), xch(w, (p.y & 1) != 0, false), k);
#elif MODE == 3
    uint x = (k * 0x9e3779b9u) ^ _Seed;
    [loop] for (i = 0; i < n; i++) x = xch(x, (p.x & 1) != 0, true) + 1;
    return uint4(x, k, 0, 0);
#else
    uint x = (k * 0x9e3779b9u) ^ _Seed;
    [loop] for (i = 0; i < n; i++) x = (x ^ (x >> 3)) + 1;
    return uint4(x, k, 0, 0);
#endif
}
float4 vert(uint id : SV_VertexID) : SV_POSITION { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
