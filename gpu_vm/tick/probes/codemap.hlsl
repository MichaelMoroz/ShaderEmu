// codemap: one core (an 8 x 4 block, every lane the same address, as the tick) walks a chain of N texels whose places in
// the texture are a map of their index (codemap.cpp). Texel i's .x holds the next index: i + 1 (straight code, PAT 0) or a
// random cycle (data reads, PAT 1); .y is 0. MAP:
//   0 row-major, W wide (W 8192 is today's RAM texture)      1 tiles of T x T texels, row-major inside, tiles row-major
//   2 Morton: the index's low 20 bits interleaved, x even    3 column-major, H tall
//   4 StructuredBuffer<uint4> (linear, no texture)                5 strips SW wide, SH tall, side by side in the 8192-wide texture
// F2 1: the next texel fetched as well (the tick's "both"), its .y (0) or'ed in.
Texture2D<uint4> _T : register(t0);
StructuredBuffer<uint4> _B : register(t1);
Texture2D<uint4> _Prev : register(t2);
cbuffer P : register(b0) { uint _N, _Seed, _p0, _p1; };
static const uint NCAP = 400000;
#ifndef W
#define W 8192
#endif
#ifndef T
#define T 4
#endif
#ifndef SW
#define SW 512
#endif
#ifndef SH
#define SH 1024
#endif
#ifndef H
#define H 1024
#endif
uint even(uint v) {   // bits 0, 2, 4, ... of v packed
    v &= 0x55555555u; v = (v | (v >> 1)) & 0x33333333u; v = (v | (v >> 2)) & 0x0f0f0f0fu;
    v = (v | (v >> 4)) & 0x00ff00ffu; return (v | (v >> 8)) & 0x0000ffffu;
}
uint4 at(uint i) {
#if MAP == 0
    return _T.Load(int3(i & (W - 1), i / W, 0));
#elif MAP == 1
    uint tile = i / (T * T), in_ = i & (T * T - 1);
    return _T.Load(int3((tile & (8192 / T - 1)) * T + (in_ & (T - 1)), (tile / (8192 / T)) * T + in_ / T, 0));
#elif MAP == 2
    uint lo = i & 0xfffffu;
    return _T.Load(int3(even(lo) | ((i >> 20) << 10), even(lo >> 1), 0));
#elif MAP == 3
    return _T.Load(int3(i / H, i & (H - 1), 0));
#elif MAP == 5
    return _T.Load(int3((i & (SW - 1)) + (i / (SW * SH)) * SW, (i / SW) & (SH - 1), 0));
#else
    return _B[i];
#endif
}
uint4 frag(float4 pos : SV_POSITION) : SV_Target {
#if SER
    uint x = _Prev.Load(int3(0, 0, 0)).x % _Seed, k, n = min(_N, NCAP);   // (--short: the last draw's word: the draws in order)
#else
    uint x = _Seed, k, n = min(_N, NCAP);
#endif
    [loop] for (k = 0; k < n; k++) {
#if F2
        x = at(x).x | at(x + 1).y;
#else
        x = at(x).x;
#endif
    }
    return uint4(x, 0, 0, 0);
}
float4 vert(uint id : SV_VertexID) : SV_POSITION { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
uint4 fill(float4 pos : SV_POSITION) : SV_Target { return uint4(0, 0, 0, 0); }   // (--short: a texel drawn between the chains' draws)
