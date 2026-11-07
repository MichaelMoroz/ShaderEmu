#ifndef TYPES_H
#define TYPES_H
#include "layout.h"

// The state's layout as the commit pass reads it (the tick, shader_vm/tick.hlsl, has its own
// copy of the same numbers): Linux without an MMU, its CSRs fields of the CPU's texels 0 to 2
// (docs/system/nommu.md, "The state").

// Where RAM texel number `lin` (16 bytes each) lives in the state texture: 2048 texels (32 KB) a row.
#define RAM_ADDR(lin) uint2((lin) % 2048, 64 + ((lin) / 2048))
#define RAM_LIN(x, y) ((x) + (y) * 2048)
#ifndef STATE_TEX_HEIGHT
#define STATE_TEX_HEIGHT 9280   // the state texture's rows: 64 of state, then RAM to 0x92000000
#endif
// RAM's 4 MB bands (128 rows) from lo to hi as the commit's mask of 32 bits: bit b & 31 stands
// for bands b, b + 32 and b + 64 (the devices' 32 MB are bands 0 to 7: layout.h).
uint ram_bands(uint lo, uint hi) {
    if (hi < lo) return 0;
    if (hi - lo >= 31) return 0xffffffff;
    uint m = (2u << (hi - lo)) - 1;
    return (m << (lo & 31)) | (m >> ((32 - (lo & 31)) & 31));
}


// The write cache holds RAM texels (four words), not words: stores come in runs (a register
// save, a structure, a copy), and a run then takes one entry. A bucket is the tags of its
// three entries in one state texel (texel number + 1; 0 is free) and then their texels.
// There are three tables with different hashes, of 2^L1_TABLE_BITS buckets, half and a quarter
// of that; a texel goes to the next only when its bucket in the one before is full, so most
// lookups read one bucket. Entries are filled in order and never freed within a pass.
// Core 0's (L1_TABLE_BITS 6) is 112 buckets, 448 texels.
#ifndef L1_TABLE_BITS
#define L1_TABLE_BITS 6
#endif
#define L1_TABLE (1 << L1_TABLE_BITS)
#define L1_BUCKETS (L1_TABLE + L1_TABLE / 2 + L1_TABLE / 4)
#define L1_BUCKETS_OF(bits) ((7u << (bits)) >> 2)   // the buckets of a cache of 2^bits
#define L1_STRIDE 4                                  // a bucket's tags and its three texels
#define L1_ENTRIES (L1_BUCKETS * L1_STRIDE)
// the half tables' hashes of texel t into n bits: its low bits moved round, xor higher ones
#define L1_H1(t, n) ((((t) >> 3) ^ ((t) << ((n) - 3)) ^ ((t) >> (n))) & ((1u << (n)) - 1))
#define L1_H2(t, n) ((((t) >> 2) ^ ((t) << ((n) - 2)) ^ ((t) >> ((n) + 3))) & ((1u << (n)) - 1))
#if defined(CORES) && CORES > 1
// A worker core's write cache is as large as the machine's geometry says (docs/machine/multicore.md,
// "The geometry"): tables of 2^mc_bits_now buckets. mc_bits_now is the size of the core whose
// cache is being looked at.
#define L1_NOW_BITS mc_bits_now
#define L1_NOW_TABLE (1u << L1_NOW_BITS)
#define L1_B0(t) ((t) & (L1_NOW_TABLE - 1))
#define L1_B1(t) (L1_NOW_TABLE + L1_H1(t, L1_NOW_BITS - 1))
#define L1_B2(t) (L1_NOW_TABLE + (L1_NOW_TABLE >> 1) + (L1_H2(t, L1_NOW_BITS - 1) & ((L1_NOW_TABLE >> 2) - 1)))
#else
#define L1_B0(t) ((t) & (L1_TABLE - 1))
#define L1_B1(t) (L1_TABLE + L1_H1(t, L1_TABLE_BITS - 1))
#define L1_B2(t) (L1_TABLE + (L1_TABLE >> 1) + (L1_H2(t, L1_TABLE_BITS - 1) & ((L1_TABLE >> 2) - 1)))
#endif

// Where the state texels are in a core's block (texel lin is at x = lin % 64, y = lin / 64): the
// CPU's 44 texels, the write cache, then the float registers' 8.
#define L1_STATE_AT 44
#define FP_STATE_AT (L1_STATE_AT + L1_ENTRIES)

#if CORES > 1
// The geometry (docs/machine/multicore.md): every core has a slot of 64 x 8 texels in the state's rows, core k's
// at ((k & 31) * 64, (k >> 5) * 8). Core 0 fills its slot with a cache of 2^6, its texels in rows; a worker has 2^5 or
// less, in whole tiles of 8 x 8 (texel w at ((w >> 6) * 8 + (w & 7), (w >> 3) & 7)), and tiles 5 and 6 of the slots from
// the ninth on are the lanes' field's.
// How many workers there are and how large their write caches are is a texel of the control words,
// which core 0 writes while every worker is parked:
//   .r  bit 0: the slots are being laid out anew (every pixel of them is zero meanwhile)
//   .g  how many workers
//   .b  their caches' tables are 2^bits buckets (3 to 5)
// A worker keeps the CPU's 44 texels, its cache and the float registers' 8, in that order.
#define MC_GEOMETRY DEV_T(0x10003du)
static uint mc_count = 0;   // workers the geometry has
void mc_geo_read() {
    mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
    mc_count = (mc_geo.r & 1) == 0 && mc_geo.b >= 3 && mc_geo.b <= 5 ? min(mc_geo.g, (uint)CORES - 1) : 0;
}
#define MC_GEO_READ mc_geo_read();
uint mc_bits_of(uint core) {
    return core == 0 ? (uint)L1_TABLE_BITS : core <= mc_count ? mc_geo.b : 0;
}
uint2 mc_slot_of(uint core) {
    return uint2((core & 31) << 6, (core >> 5) << 3);
}
// Makes `core` the one whose state is read.
void mc_select(uint core) {
    mc_slot = mc_slot_of(core);
    mc_core = core;
    mc_bits_now = mc_bits_of(core);
    mc_size_now = L1_STATE_AT + L1_BUCKETS_OF(mc_bits_now) * L1_STRIDE + 8;
    state_off = uint2(0, 0);
}
#define MC_ENTRIES (mc_core == 0 ? (uint)L1_ENTRIES : L1_BUCKETS_OF(mc_bits_now) * L1_STRIDE)
// Where texel lin (as core 0 numbers them) of this core's state is, from where its rows begin;
// far outside the texture, which reads as zeros, for what a worker does not keep.
uint2 mc_state_at(uint lin) {
    if (mc_core != 0) {
        uint cache_end = L1_STATE_AT + MC_ENTRIES;
        if (lin >= FP_STATE_AT && lin < FP_STATE_AT + 8) lin = cache_end + (lin - FP_STATE_AT);
        else if (lin >= cache_end) return uint2(0, 0x4000);
    }
    return uint2(lin % 64, lin / 64);
}
// Which texel the pixel at `pos` of this core's rows keeps, as the place it has in core 0's
// layout; (63, 63), which is nobody's, for a pixel past a worker's last texel.
uint2 mc_texel_of(uint2 pos) {
    if (mc_core == 0) return pos;
    uint w = pos.y * 64 + pos.x, cache_end = L1_STATE_AT + MC_ENTRIES;
    if (w >= cache_end + 8) return uint2(63, 63);
    uint lin = w < cache_end ? w : FP_STATE_AT + (w - cache_end);
    return uint2(lin % 64, lin / 64);
}
#endif
uint l1_slot(uint4 tags, uint tag) {
    return tags.x == tag ? 1 : tags.y == tag ? 2 : tags.z == tag ? 3 : 0;
}
// the last tag of a bucket: entries fill in order, so the bucket is full when it is taken
#define L1_LAST(tags) ((tags).z)

// A core's stall (texel 28's first word): what the commit clears. The tick's own list is in tick.hlsl.
#define STALL_CSR_CACHE 2
#define STALL_MEM_CACHE_L1 3
#define STALL_UART 6
#define STALL_FENCE 7
#define STALL_MEMOP_COPY 8
#define STALL_WFI 9   // the guest has nothing to do until the next frame
#define STALL_MEMOP_FILL 10   // as MEMOP_COPY, but every word becomes memop_src_v

uint4 l1_state_texel(uint lin) {
    lin += L1_STATE_AT;
#if CORES > 1
    return STATE_TEX_HART(mc_state_at(lin), 0);
#else
    return STATE_TEX_HART(uint2(lin % 64, lin / 64), 0);
#endif
}

// The cache's copy of RAM texel t from the state texture, if it has one.
bool l1_state_find(uint t, out uint4 texel) {
    texel = 0;
    uint b = L1_B0(t);
    uint4 tags = l1_state_texel(b * L1_STRIDE);
    uint e = l1_slot(tags, t + 1);
    [branch]
    if (e == 0 && L1_LAST(tags) != 0) {
        b = L1_B1(t);
        tags = l1_state_texel(b * L1_STRIDE);
        e = l1_slot(tags, t + 1);
        [branch]
        if (e == 0 && L1_LAST(tags) != 0) {
            b = L1_B2(t);
            tags = l1_state_texel(b * L1_STRIDE);
            e = l1_slot(tags, t + 1);
        }
    }
    [branch]
    if (e != 0) {
        texel = l1_state_texel(b * L1_STRIDE + e);
    }
    return e != 0;
}

uint mem_get_cached_or_tex_from_state_cache(uint addr) {
    uint4 cached;
    if (l1_state_find(addr >> 4, cached)) return idx_uint4(cached, (addr >> 2) & 0x3);

    // query RAM texture
    uint idx = (addr >> 2) & 0x3;
    addr >>= 4;
    uint4 raw = RAM_TEX(RAM_ADDR(addr));
    return idx_uint4(raw, idx);
}

#ifdef PASS_COMMIT
uint mem_get_cached_or_tex_from_state_cache_or_mtd(uint addr) {
    if (addr & 0x80000000) {
        return mem_get_cached_or_tex_from_state_cache(addr & 0x7fffffff);
    } else if (addr & 0x40000000) {
        // query MTD/ROM
        uint mtd_addr = addr - 0x40000000;
        uint mtd_idx = (mtd_addr >> 2) & 0x3;
        mtd_addr = mtd_addr >> 4;
        uint2 mtd_lin = uint2(mtd_addr & 2047, mtd_addr >> 11);
        return idx_uint4(asuint(_Data_ROM[mtd_lin]), mtd_idx);   // (the ROM: one texture, a texel a pixel)
    }
    return 0;
}


// A texel of a core's state as the commit leaves it: the commit counted, a stall the commit
// ends cleared, and the tick's hand-over word (41,0).r cleared.
uint4 commit(uint2 pos, uint4 ret) {
    if (pos.y < 64 && pos.x >= 64) {
        return (uint4)0;
    }

    uint pos_id = pos.x | ((pos.y) << 16);

    // increment commits
    if (!_Init && pos_id == 28) {
        ++ret.b;
    }

    // clear stalls
    if (pos_id == 28) {
        switch (ret.r) {
            case STALL_CSR_CACHE:
                ret.r = 0;
                break;
            case STALL_MEM_CACHE_L1:
                ret.r = 0;
                break;
            case STALL_FENCE:
                ret.r = 0;
                break;
            case STALL_MEMOP_COPY: // will be handled now, so we can unset
                ret.r = 0;
                break;
            case STALL_UART:
                ret.r = 0;
                break;
            case STALL_WFI:
                ret.r = 0;
                break;
            case STALL_MEMOP_FILL:
                ret.r = 0;
                break;
        }
    }

    // hand-over word is cleared here (FXC hits an internal error if this sits next to the
    // stall-clearing switch above)
    if (pos.x == 41 && pos.y == 0) {
        ret = uint4(0, ret.g, ret.b, ret.a);
    }
    return ret;
}
#endif

#endif
