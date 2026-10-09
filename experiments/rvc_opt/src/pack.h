#ifndef PACK_H
#define PACK_H

// STATE_PACK (an experiment, docs/multicore.md): where a state texel is kept. The tick's 973
// texels are four bands of a 64 x 64 block (the first 44, the write cache from 1068, the TLBs
// from 2112, the float registers at 2528), so the warps the pass runs them in are part empty.
// With STATE_PACK they are one 64 x 16 rectangle, the CSR area is the sixteen rows under it,
// and the tick pass draws the rectangle only. Everything else still speaks of a texel by its
// old place: STATE_TEX looks it up through state_pack(), and a pass turns the pixel it is
// drawing back into that place with state_unpack().
#ifdef STATE_PACK
// (the commit pass does not have these from types.h and mmu.h)
#ifndef FP_STATE_AT
#define FP_STATE_AT 2528
#endif
#ifndef TLB_STATE_AT
#define TLB_STATE_AT 2112
#endif
#ifndef TLB_STATE_TEXELS
#define TLB_STATE_TEXELS 409
#endif
#ifndef NO_PAGING
#define PK_TLB TLB_STATE_TEXELS
#else
#define PK_TLB 0
#endif
#define PK_L1_AT 44
#define PK_TLB_AT (PK_L1_AT + L1_ENTRIES)
#define PK_FP_AT (PK_TLB_AT + PK_TLB)
#define PK_N 1024   // the rectangle; the CSR area's 1024 texels follow
// (44 + L1_ENTRIES + the TLBs' 409 + 8 = 973 texels with the default cache)

uint2 state_pack(uint2 pos) {
    if (pos.y >= 64 || pos.x >= 64) return pos;   // RAM
    uint lin = pos.x + 64 * pos.y, k = 4095;
    if (lin < 44) k = lin;
    else if (lin < 1068) k = PK_N + lin - 44;
    else if (lin < 1068 + L1_ENTRIES) k = PK_L1_AT + lin - 1068;
#ifndef NO_PAGING
    else if (lin >= TLB_STATE_AT && lin < TLB_STATE_AT + TLB_STATE_TEXELS) k = PK_TLB_AT + lin - TLB_STATE_AT;
#endif
#ifdef FPU
    else if (lin >= FP_STATE_AT && lin < FP_STATE_AT + 8) k = PK_FP_AT + lin - FP_STATE_AT;
#endif
    return uint2(k & 63, k >> 6);
}

uint2 state_unpack(uint2 pos) {
    if (pos.y >= 64 || pos.x >= 64) return pos;
    uint k = pos.x + 64 * pos.y, lin = 4095;
    if (k < 44) lin = k;
    else if (k < PK_TLB_AT) lin = 1068 + k - PK_L1_AT;
#ifndef NO_PAGING
    else if (k < PK_FP_AT) lin = TLB_STATE_AT + k - PK_TLB_AT;
#endif
#ifdef FPU
    else if (k < PK_FP_AT + 8) lin = FP_STATE_AT + k - PK_FP_AT;
#endif
    else if (k >= PK_N && k < PK_N + 1024) lin = 44 + k - PK_N;
    return uint2(lin & 63, lin >> 6);
}
#endif

#endif
