// codemap: one core (an 8 x 4 block, every lane the same address, as the tick) walks a chain of N texels whose places in
// the texture are a map of their index (codemap.cpp). Texel i's .x holds the next index: i + 1 (straight code, PAT 0) or a
// random cycle (data reads, PAT 1); .y is 0. MAP:
//   0 row-major, W wide (W 8192 is today's RAM texture)      1 tiles of T x T texels, row-major inside, tiles row-major
//   2 Morton: the index's low 20 bits interleaved, x even    3 column-major, H tall
//   4 StructuredBuffer<uint4> (linear, no texture)                5 strips SW wide, SH tall, side by side in the 8192-wide texture
//   6 pairs: texels 2k and 2k + 1 one above the other (the card's 32-byte sector: codemap --line), the pairs in rows W wide
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
#elif MAP == 6
    return _T.Load(int3((i >> 1) & (W - 1), ((i >> 1) / W) * 2 + (i & 1), 0));   // (pairs: i and i + 1 one above the other, a sector)
#else
    return _B[i];
#endif
}
// PF (--pf): the core's 32 lanes warm the cache, each lane a different texel, all loads in flight together; their .y (0)
// is folded into the answer at the end, so nothing waits for them. 1: PFN texels from the chain's start, before the loop;
// 2: every PFE steps, the PFE texels ahead of the chain (PFE / 32 loads a lane).
#ifndef PF
#define PF 0
#endif
#ifndef PFN
#define PFN 1024
#endif
#ifndef PFE
#define PFE 256
#endif
#ifndef PFS
#define PFS 1   // (--pfs) texels between one lane's load and the next lane's: PFN texels covered with PFN / PFS loads
#endif
uint4 frag(float4 pos : SV_POSITION) : SV_Target {
#if SER
    uint x = _Prev.Load(int3(0, 0, 0)).x % _Seed, k, n = min(_N, NCAP);   // (--short: the last draw's word: the draws in order)
#else
    uint x = _Seed, k, n = min(_N, NCAP);
#endif
    uint lane = ((uint)pos.x & 7) + 8 * ((uint)pos.y & 3), acc = 0;
#if PF == 1
    [loop] for (k = 0; k < PFN / (32 * PFS); k++) acc ^= at((x + PFS * (lane + 32 * k)) % _Seed).y;
#endif
    [loop] for (k = 0; k < n; k++) {
#if PF == 2
        [branch] if ((k & (PFE - 1)) == 0) {
            uint j;
            [unroll] for (j = 0; j < max(PFE / (32 * PFS), 1); j++) acc ^= at((x + PFE + PFS * (lane + 32 * j)) % _Seed).y;
        }
#endif
#if F2
        x = at(x).x | at(x + 1).y;
#else
        x = at(x).x;
#endif
    }
    return uint4(x | acc, 0, 0, 0);
}
float4 vert(uint id : SV_VertexID) : SV_POSITION { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
uint4 fill(float4 pos : SV_POSITION) : SV_Target { return uint4(0, 0, 0, 0); }   // (--short: a texel drawn between the chains' draws)
// --line N: the texture cache's line by its shape. Step k loads a base texel (64 apart in x, 16 rows apart in y: no two in one
// line) and then the texel (_p0, _p1) from it, whose address waits for the base's word (0); the draw waits for the last draw's
// target, so every draw starts cold. A neighbour in the base's line costs what a hit costs; outside it, what a miss costs.
uint4 linefrag(float4 pos : SV_POSITION) : SV_Target {
    uint acc = _Prev.Load(int3(0, 0, 0)).x & 0x80000000u, k, n = min(_N, NCAP);
    [loop] for (k = 0; k < n; k++) {
        int2 b = int2((k * 64) & 8191, ((k * 64) >> 13) * 16);
        uint v = _T.Load(int3(b.x + acc, b.y, 0)).y;
        acc = _T.Load(int3(b.x + (int)_p0 + v, b.y + (int)_p1, 0)).y;
    }
    return uint4(acc, 0, 0, 0);
}
