#ifndef TYPES_H
#define TYPES_H
#include "layout.h"

// Linux without an MMU, entered in supervisor mode: its CSRs are fields of the CPU's texels 0
// to 2, with no CSR table and no CSR cache (docs/system/nommu.md, "The state").
#if defined(NO_PAGING) && defined(SBI_HLE) && !defined(CSR_TABLE)
#define CSR_FIELDS
#endif

#define glsl_mod(x,y) (((x)-(y)*floor((x)/(y)))) 


// Where RAM texel number `lin` (16 bytes each) lives in the state texture (layout.h).
// RAM_TILE_BITS = 0: row-major order, RAM_W texels (128 KB) per row, from row RAM_ROW.
// RAM_TILE_BITS = b: square tiles of 2^b x 2^b texels laid out row by row, so nearby addresses
// are near each other in both directions (b = 4: one 4 KB page per 16x16 tile).
#ifndef RAM_TILE_BITS
#define RAM_TILE_BITS 0
#endif
#if RAM_TILE_BITS == 0
#define RAM_ADDR(lin) uint2((lin) % RAM_W, RAM_ROW + ((lin) / RAM_W))
#define RAM_LIN(x, y) ((x) + (y) * RAM_W)
#else
#define RAM_TILE (1 << RAM_TILE_BITS)
#define RAM_TILES_PER_ROW (RAM_W >> RAM_TILE_BITS)
#define RAM_IN_X(i) ((i) & (RAM_TILE - 1))
#define RAM_IN_Y(i) ((i) >> RAM_TILE_BITS)
#define RAM_IN(x, y) ((x) | ((y) << RAM_TILE_BITS))
#define RAM_TILE_OF(lin) ((lin) >> (2 * RAM_TILE_BITS))
#define RAM_WITHIN(lin) ((lin) & (RAM_TILE * RAM_TILE - 1))
#define RAM_ADDR(lin) uint2(((RAM_TILE_OF(lin) % RAM_TILES_PER_ROW) << RAM_TILE_BITS) | RAM_IN_X(RAM_WITHIN(lin)),                             RAM_ROW + ((RAM_TILE_OF(lin) / RAM_TILES_PER_ROW) << RAM_TILE_BITS) + RAM_IN_Y(RAM_WITHIN(lin)))
#define RAM_LIN(x, y) (((((y) >> RAM_TILE_BITS) * RAM_TILES_PER_ROW + ((x) >> RAM_TILE_BITS)) << (2 * RAM_TILE_BITS)) |                        RAM_IN((x) & (RAM_TILE - 1), (y) & (RAM_TILE - 1)))
#endif
// The ROM's texel t in its texture (layout.h): rows of RAM_W, texel 0 in row 0.
#define ROM_ADDR(t) uint2((t) % RAM_W, (t) / RAM_W)
// RAM's 4 MB bands (RAM_BAND_ROWS rows) from lo to hi as the commit's mask of 32 bits: bit b & 31 stands
// for bands b, b + 32 and b + 64 (the devices' 32 MB are bands 0 to 7: layout.h).
uint ram_bands(uint lo, uint hi) {
    if (hi < lo) return 0;
    if (hi - lo >= 31) return 0xffffffff;
    uint m = (2u << (hi - lo)) - 1;
    return (m << (lo & 31)) | (m >> ((32 - (lo & 31)) & 31));
}


/* shift by two to ignore byte offset */
// The write cache holds RAM texels (four words), not words: stores come in runs (a register
// save, a structure, a copy), and a run then takes one entry. A bucket is the tags of its
// L1_WAYS entries in one state texel (texel number + 1; 0 is free) and then their texels.
// There are three tables with different hashes, of 2^L1_TABLE_BITS buckets, half and a quarter
// of that; a texel goes to the next only when its bucket in the one before is full, so most
// lookups read one bucket. Entries are filled in order and never freed within a pass.
//   L1_TABLE_BITS 6, L1_WAYS 3 (the default): 336 texels, an array of 448 in the tick
//   L1_TABLE_BITS 6, L1_WAYS 4:               448 texels, arrays of 448 and 112
//   L1_TABLE_BITS 7, L1_WAYS 3:               672 texels, an array of 896
// The first is 5% more instructions a second than the second in spite of its stalls: four
// entries a bucket cost every load and store, and more than about 1,000 state texels (each is
// a pixel that runs the whole tick) cost every instruction.
#ifndef L1_BUCKETS
#ifndef L1_TABLE_BITS
#define L1_TABLE_BITS 6
#endif
#ifndef L1_WAYS
#define L1_WAYS 3
#endif
#define L1_TABLE (1 << L1_TABLE_BITS)
#define L1_BUCKETS (L1_TABLE + L1_TABLE / 2 + L1_TABLE / 4)
#define L1_BUCKETS_OF(bits) ((7u << (bits)) >> 2)   // the buckets of a cache of 2^bits
#define L1_STRIDE (L1_WAYS + 1)
#define L1_ENTRIES (L1_BUCKETS * L1_STRIDE)
// the half tables' hashes of texel t into n bits: its low bits moved round, xor higher ones
#define L1_H1(t, n) ((((t) >> 3) ^ ((t) << ((n) - 3)) ^ ((t) >> (n))) & ((1u << (n)) - 1))
#define L1_H2(t, n) ((((t) >> 2) ^ ((t) << ((n) - 2)) ^ ((t) >> ((n) + 3))) & ((1u << (n)) - 1))
#if defined(CORES) && CORES > 1
// A worker core's write cache is as large as the machine's geometry says (docs/machine/multicore.md,
// "The geometry"): tables of 2^mc_bits_now buckets, in the first buckets of the same arrays.
// mc_bits_now is the size of the core whose cache is being looked at, in the tick and in
// the commit.
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
#endif

// Where the state texels are in the 64 texels wide state block (texel lin is at x = lin % 64,
// y = lin / 64). What the tick pass keeps is one rectangle of STATE_ROWS rows from the top:
// the CPU's 44 texels, the write cache, the TLBs, the float registers. The pass draws that
// rectangle and nothing else, so the warps it runs in are full (docs/machine/multicore.md: with the
// same texels in four bands of a 64 x 64 zone the pass was 5 to 9% slower, and a second core
// cost a second pass). The CSR area, which only the commit pass writes, is under it.
#define L1_STATE_AT 44
#define TLB_STATE_AT (L1_STATE_AT + L1_ENTRIES)
#ifndef TLB2_N
#define TLB2_N 256
#endif
#define TLB_STATE_TEXELS (3 * TLB2_N / 2 + 3 * 16 / 2 + 1)
#if defined(NO_PAGING) && !defined(CORE0_TLB_ROWS)
#define FP_STATE_AT TLB_STATE_AT   // no TLBs: core 0 keeps what a worker keeps
#else
#define FP_STATE_AT (TLB_STATE_AT + TLB_STATE_TEXELS)
#endif
#define CORE0_ROWS ((FP_STATE_AT + 8 + 63) / 64)   // the rows of core 0's state the tick draws
#ifndef STATE_ROWS
#if L1_TABLE_BITS == 6 && L1_WAYS == 3
#define STATE_ROWS 16   // 44 + 448 + 409 + 8 = 909 texels
#else
#define STATE_ROWS 32   // any larger write cache
#endif
#endif
#define CSR_STATE_AT (STATE_ROWS * 64)
#define CSR_STATE_TEXELS 1024

#if CORES > 1
// A worker core (docs/machine/multicore.md) keeps the CPU's texels, the write cache and the float
// registers, and no TLBs: one that runs with paging starts every pass with none, which costs
// nothing that can be measured. That is 500 texels with a cache of 2^6, eight whole tiles of
// 8 x 8, as core 0's 64 x 8: the GPU runs pixels in tiles. In a row of their own they were a
// line a pixel high, which costs the warps of two rows.
// The geometry (docs/machine/multicore.md): the workers are all alike. Their state is tiles of 8 x 8
// texels in the state rows beside core 0's block: MC_STRIP_TILES of them to a band of eight
// rows, one band under another, a worker's tiles side by side in one band. How many workers
// there are and how large their write caches are is a texel of the control words that core 0
// writes while every worker is parked:
//   .r  bit 0: the strip is being laid out anew (every pixel of it is zero meanwhile)
//   .g  how many workers
//   .b  their caches' tables are 2^bits buckets (3 to 6)
// A worker keeps the CPU's 44 texels, its cache and the float registers' 8, in that order.
#define MC_GEOMETRY DEV_T(0x10003du)
#define MC_STRIP_X 64
#define MC_STRIP_TILES 248
#ifndef MC_STRIP_BANDS
#define MC_STRIP_BANDS ((CORES + 47) / 49)   // 49 workers of five tiles to a band
#endif
// With the tick in eight targets (TICK_MRT, main.shader) a tile is a column of eight pixels
// there, and a worker's columns are a block of its own, TICK_BLOCK columns wide or a multiple,
// with TICK_PAD or more columns of it empty: pixels of two cores that the card runs together
// cost what both cost (measured: docs/machine/multicore.md; six columns of eight may be used). The
// blocks are side by side in bands of STATE_ROWS rows, TICK_BANDS of them, the first with
// core 0's eight columns and TICK_GAP empty ones before the workers'.
#ifndef TICK_BLOCK
#define TICK_BLOCK 4
#endif
#ifndef TICK_PAD
#define TICK_PAD 2
#endif
#ifndef TICK_GAP
#define TICK_GAP 8
#endif
#ifndef TICK_MRT_W
#define TICK_MRT_W 1040   // (core 0, the gap and 128 blocks of eight columns)
#endif
#define TICK_BANDS ((CORES + 126) / 128)
// The hybrid geometry is the machine's (geotune lays it out); MC_UNIFORM compiles the old one.
#if !defined(MC_HYBRID) && !defined(MC_UNIFORM)
#define MC_HYBRID
#endif
#ifdef MC_HYBRID
static uint mc_count = 0;   // workers the geometry has, a worker's tiles, and its block's columns
static uint mc_rows = 0;
static uint mc_cols = 0;
// MC_HYBRID (local experiment): the geometry's .a is a count of big workers and their bits
// (low byte, next byte) and how wide the workers' blocks are in the tick's targets (the small
// ones' in bits 16-23, the big ones' in bits 24-31; 0: TICK_BLOCK with TICK_PAD, and
// TICK_BLOCK_BIG with TICK_PAD_BIG, as compiled). A block of a width the geometry gives is
// the worker's columns rounded up to a multiple of it, with no column more. Workers 1..mc_nb
// have caches of mc_bb bits, their tiles in the strip's first band and their blocks in the
// targets' first band; the others are as .b says, from the next band of each, and those the
// targets' other bands do not hold go after the big ones' blocks in the first band. With .a 0
// it is the uniform geometry. linux/userland/geotune.c picks the widths for the card.
static uint mc_nb = 0, mc_bb = 0, mc_rows_b = 0, mc_cols_b = 0;
static uint mc_rows_now = 0, mc_cols_now = 0;   // the selected worker's
static uint mc_per0 = 0;   // the small workers' blocks after the big ones' in the targets' first band
#ifndef TICK_BLOCK_BIG
#define TICK_BLOCK_BIG 8
#endif
#ifndef TICK_PAD_BIG
#define TICK_PAD_BIG 0
#endif
#define TICK_COLS (TICK_MRT_W - 8 - TICK_GAP)   // a band's columns for the workers' blocks
uint mc_rows_of(uint bits) {
    return bits == 0 ? 0 : (L1_STATE_AT + 8 + (L1_BUCKETS_OF(bits) * L1_STRIDE) + 63) / 64;
}
uint mc_cols_wide(uint rows, uint block, uint pad) {
    return (rows + pad + block - 1) / block * block;
}
uint mc_cols_of(uint rows) {
    return mc_cols_wide(rows, TICK_BLOCK, TICK_PAD);
}
void mc_geo_read() {
    mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
#ifdef MC_FORCE_BITS   // (local experiment: workers' cache size, whatever the kernel wrote)
    if (mc_geo.b != 0) mc_geo.b = MC_FORCE_BITS;
#endif
    uint ws = (mc_geo.a >> 16) & 0xff, wb = mc_geo.a >> 24;
    mc_rows = mc_geo.b < 3 || mc_geo.b > 6 ? 0 : mc_rows_of(mc_geo.b);
    mc_cols = ws != 0 && ws <= 64 ? mc_cols_wide(mc_rows, ws, 0) : mc_cols_of(mc_rows);
    mc_count = 0;
    mc_nb = 0;
    mc_per0 = 0;
    mc_bb = (mc_geo.a >> 8) & 0xff;
    if ((mc_geo.a & 0xff) != 0 && mc_bb >= 3 && mc_bb <= 6 && mc_rows != 0) {
        mc_rows_b = mc_rows_of(mc_bb);
        mc_cols_b = wb != 0 && wb <= 64 ? mc_cols_wide(mc_rows_b, wb, 0) : mc_cols_wide(mc_rows_b, TICK_BLOCK_BIG, TICK_PAD_BIG);
        mc_nb = min(min(mc_geo.a & 0xff, MC_STRIP_TILES / mc_rows_b), TICK_COLS / mc_cols_b);
        mc_nb = min(min(mc_nb, mc_geo.g), (uint)CORES - 1);
        if (mc_nb != 0) mc_per0 = (TICK_COLS - mc_nb * mc_cols_b) / mc_cols;
    }
    uint sb0 = mc_nb != 0 ? 1 : 0;
    if (mc_rows != 0 && (mc_geo.r & 1) == 0)
        mc_count = mc_nb + min(min(mc_geo.g, (uint)CORES - 1) - mc_nb, min((MC_STRIP_BANDS - sb0) * (MC_STRIP_TILES / mc_rows), (TICK_BANDS - sb0) * (TICK_COLS / mc_cols) + mc_per0));
}
#define MC_GEO_READ mc_geo_read();
uint mc_bits_of(uint core) {
    return core == 0 ? (uint)L1_TABLE_BITS : core <= mc_nb ? mc_bb : core <= mc_count ? mc_geo.b : 0;
}
// Where tile `tile` of the strip is in the state texture, and the first tile of a worker.
uint2 mc_tile_at(uint tile) {
    return uint2(MC_STRIP_X + 8 * (tile % MC_STRIP_TILES), 8 * (tile / MC_STRIP_TILES));
}
uint mc_tile_of(uint core) {
    if (core <= mc_nb) return (core - 1) * mc_rows_b;
    uint j = core - 1 - mc_nb, per = MC_STRIP_TILES / max(mc_rows, 1), sb0 = mc_nb != 0 ? 1 : 0;
    return (sb0 + j / per) * MC_STRIP_TILES + (j % per) * mc_rows;
}
// Where a worker's block is in the tick's targets: its first column and its band's first row.
uint2 mc_block_of(uint core) {
    if (core <= mc_nb) return uint2(8 + TICK_GAP + (core - 1) * mc_cols_b, 0);
    uint j = core - 1 - mc_nb, per = TICK_COLS / max(mc_cols, 1), sb0 = mc_nb != 0 ? 1 : 0, below = (TICK_BANDS - sb0) * per;
    if (j >= below) return uint2(8 + TICK_GAP + mc_nb * mc_cols_b + (j - below) * mc_cols, 0);
    return uint2(8 + TICK_GAP + (j % per) * mc_cols, (sb0 + j / per) * STATE_ROWS);
}
// Makes `core` the one whose state is read.
void mc_select(uint core) {
    mc_tile0 = core == 0 ? 0 : mc_tile_of(core);
    mc_core = core;
    mc_bits_now = mc_bits_of(core);
    mc_rows_now = core != 0 && core <= mc_nb ? mc_rows_b : mc_rows;
    mc_cols_now = core != 0 && core <= mc_nb ? mc_cols_b : mc_cols;
    mc_size_now = L1_STATE_AT + L1_BUCKETS_OF(mc_bits_now) * L1_STRIDE + 8;
    state_off = uint2(0, 0);
}
// The worker whose tiles the strip's tile `tile` is one of, selected; 0 when it is nobody's.
uint mc_core_at(uint tile) {
    uint band = tile / MC_STRIP_TILES, t = tile % MC_STRIP_TILES, core;
    if (mc_nb != 0 && band == 0) {
        uint i = t / max(mc_rows_b, 1);
        if (i >= mc_nb) return 0;
        core = i + 1;
    } else {
        uint per = MC_STRIP_TILES / max(mc_rows, 1), i = t / max(mc_rows, 1);
        if (i >= per) return 0;
        core = mc_nb + (band - (mc_nb != 0 ? 1 : 0)) * per + i + 1;
    }
    if (core > mc_count) return 0;
    mc_select(core);
    return core;
}
// The worker whose block the targets' pixel is in, selected, and which of its tiles the
// pixel's column is (past its last: an empty column); 0 when it is nobody's.
uint mc_core_in(uint2 pixel, out uint tile) {
    tile = 0;
    if (pixel.x < 8 + TICK_GAP || (pixel.y % STATE_ROWS) >= 8 || mc_count == 0) return 0;
    uint band = pixel.y / STATE_ROWS, xx = pixel.x - 8 - TICK_GAP, core;
    uint per = TICK_COLS / mc_cols, sb0 = mc_nb != 0 ? 1 : 0;
    if (mc_nb != 0 && band == 0) {
        uint i = xx / mc_cols_b;
        if (i < mc_nb) {
            core = i + 1;
            tile = xx % mc_cols_b;
        } else {
            // a small worker's block after the big ones'
            xx -= mc_nb * mc_cols_b;
            i = xx / mc_cols;
            if (i >= mc_per0) return 0;
            core = mc_nb + (TICK_BANDS - sb0) * per + i + 1;
            tile = xx % mc_cols;
        }
    } else {
        uint i = xx / mc_cols;
        if (i >= per) return 0;
        core = mc_nb + (band - sb0) * per + i + 1;
        tile = xx % mc_cols;
    }
    if (core > mc_count) return 0;
    mc_select(core);
    return core;
}
#else
static uint mc_count = 0;   // workers the geometry has, a worker's tiles, and its block's columns
static uint mc_rows = 0;
static uint mc_cols = 0;
uint mc_rows_of(uint bits) {
    return bits == 0 ? 0 : (L1_STATE_AT + 8 + (L1_BUCKETS_OF(bits) * L1_STRIDE) + 63) / 64;
}
uint mc_cols_of(uint rows) {
    return (rows + TICK_PAD + TICK_BLOCK - 1) / TICK_BLOCK * TICK_BLOCK;
}
void mc_geo_read() {
    mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
#ifdef MC_FORCE_BITS   // (local experiment: workers' cache size, whatever the kernel wrote)
    if (mc_geo.b != 0) mc_geo.b = MC_FORCE_BITS;
#endif
    mc_rows = mc_geo.b < 3 || mc_geo.b > 6 ? 0 : mc_rows_of(mc_geo.b);
    mc_cols = mc_cols_of(mc_rows);
    mc_count = 0;
    if (mc_rows != 0 && (mc_geo.r & 1) == 0)
        mc_count = min(min(mc_geo.g, (uint)CORES - 1), min(MC_STRIP_BANDS * (MC_STRIP_TILES / mc_rows), TICK_BANDS * ((TICK_MRT_W - 8 - TICK_GAP) / mc_cols)));
}
#define MC_GEO_READ mc_geo_read();
uint mc_bits_of(uint core) {
    return core == 0 ? (uint)L1_TABLE_BITS : core <= mc_count ? mc_geo.b : 0;
}
// Where tile `tile` of the strip is in the state texture, and the first tile of a worker.
uint2 mc_tile_at(uint tile) {
    return uint2(MC_STRIP_X + 8 * (tile % MC_STRIP_TILES), 8 * (tile / MC_STRIP_TILES));
}
uint mc_tile_of(uint core) {
    uint j = core - 1, per = MC_STRIP_TILES / max(mc_rows, 1);
    return (j / per) * MC_STRIP_TILES + (j % per) * mc_rows;
}
// Where a worker's block is in the tick's targets: its first column and its band's first row.
uint2 mc_block_of(uint core) {
    uint j = core - 1, per = (TICK_MRT_W - 8 - TICK_GAP) / max(mc_cols, 1);
    return uint2(8 + TICK_GAP + (j % per) * mc_cols, (j / per) * STATE_ROWS);
}
// Makes `core` the one whose state is read.
void mc_select(uint core) {
    mc_tile0 = core == 0 ? 0 : mc_tile_of(core);
    mc_core = core;
    mc_bits_now = mc_bits_of(core);
    mc_size_now = L1_STATE_AT + L1_BUCKETS_OF(mc_bits_now) * L1_STRIDE + 8;
    state_off = uint2(0, 0);
}
// The worker whose tiles the strip's tile `tile` is one of, selected; 0 when it is nobody's.
uint mc_core_at(uint tile) {
    uint per = MC_STRIP_TILES / max(mc_rows, 1), i = (tile % MC_STRIP_TILES) / max(mc_rows, 1);
    uint core = (tile / MC_STRIP_TILES) * per + i + 1;
    if (i >= per || core > mc_count) return 0;
    mc_select(core);
    return core;
}
// The worker whose block the targets' pixel is in, selected, and which of its tiles the
// pixel's column is (past its last: an empty column); 0 when it is nobody's.
uint mc_core_in(uint2 pixel, out uint tile) {
    tile = 0;
    if (pixel.x < 8 + TICK_GAP || (pixel.y % STATE_ROWS) >= 8 || mc_count == 0) return 0;
    uint per = (TICK_MRT_W - 8 - TICK_GAP) / mc_cols, i = (pixel.x - 8 - TICK_GAP) / mc_cols;
    uint core = (pixel.y / STATE_ROWS) * per + i + 1;
    if (i >= per || core > mc_count) return 0;
    mc_select(core);
    tile = (pixel.x - 8 - TICK_GAP) % mc_cols;
    return core;
}
#define mc_rows_now mc_rows
#define mc_cols_now mc_cols
#endif
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
#if L1_WAYS == 4
    return tags.x == tag ? 1 : tags.y == tag ? 2 : tags.z == tag ? 3 : tags.w == tag ? 4 : 0;
#else
    return tags.x == tag ? 1 : tags.y == tag ? 2 : tags.z == tag ? 3 : 0;
#endif
}
// the last tag of a bucket: entries fill in order, so the bucket is full when it is taken
// In the tick the cache is an array of L1_DATA_N: with three entries a bucket its tags are
// element 4b and its texels the three after; with four the texels fill the array and the tags
// have one of their own. Entry e (from 1) of bucket b
// is "at" 4b + e either way, and the state texture has a bucket's tags and texels in a row.
#define L1_DATA_N (L1_BUCKETS * 4)
#if L1_WAYS == 4
#define L1_LAST(tags) ((tags).w)
#define L1_TAGS(b) l1_tag[b]
#define L1_DATA(at) l1_cache[(at) - 1]
#else
#define L1_LAST(tags) ((tags).z)
#define L1_TAGS(b) l1_cache[(b) * 4]
#define L1_DATA(at) l1_cache[at]
#endif


// (a fill or copy is only left to the commit by the full machine: it goes by the pages run)
#if defined(LAZY_MEMOP) && (defined(NO_PAGING) || defined(M_MODE_ONLY))
#undef LAZY_MEMOP
#endif

#define STALL_EXIT_CALL 1
#define STALL_CSR_CACHE 2
#define STALL_MEM_CACHE_L1 3
#define STALL_RESERVED 4
#define STALL_ILLEGAL_ENTRY_POINT 5
#define STALL_UART 6
#define STALL_FENCE 7
#define STALL_MEMOP_COPY 8
#define STALL_WFI 9   // the guest has nothing to do until the next frame
#define STALL_MEMOP_FILL 10   // as MEMOP_COPY, but every word becomes memop_src_v


// STRUCT TYPES
typedef struct {
    uint csr_cache_0_addr; uint csr_cache_0_val;
    uint csr_cache_1_addr; uint csr_cache_1_val;
    uint csr_cache_2_addr; uint csr_cache_2_val;
    uint csr_cache_3_addr; uint csr_cache_3_val;
    uint csr_cache_4_addr; uint csr_cache_4_val;
    uint csr_cache_5_addr; uint csr_cache_5_val;
    uint csr_cache_6_addr; uint csr_cache_6_val;
    uint csr_cache_7_addr; uint csr_cache_7_val;
    uint csr_cache_8_addr; uint csr_cache_8_val;
    uint csr_cache_9_addr; uint csr_cache_9_val;
    uint csr_cache_10_addr; uint csr_cache_10_val;
    uint csr_cache_11_addr; uint csr_cache_11_val;
    uint csr_cache_12_addr; uint csr_cache_12_val;
    uint csr_cache_13_addr; uint csr_cache_13_val;
    uint csr_cache_14_addr; uint csr_cache_14_val;
    uint csr_cache_15_addr; uint csr_cache_15_val;
    uint ram_l1_last_addr;uint ram_l1_last_val;
} cache_t;

typedef struct {
    uint privilege;
} csr_state;

typedef struct {
    uint rbr_thr_ier_iir;
    uint lcr_mcr_lsr_scr;
    bool thre_ip;
    bool interrupting;
    uint input_tag;
} uart_state;

typedef struct {
    bool msip;
    uint mtimecmp_lo;
    uint mtimecmp_hi;
    uint mtime_lo;
    uint mtime_hi;
} clint_state;

typedef struct {
    uint mode;
    uint ppn;
} mmu_state;

typedef struct {
    uint ptr;
    uint buf0;
    uint buf1;
    uint buf2;
    uint buf3;
    uint buf4;
    uint buf5;
    uint buf6;
    uint buf7;
    uint buf8;
    uint buf9;
    uint buf10;
    uint buf11;
    uint buf12;
    uint buf13;
    uint buf14;
    uint buf15;
    uint buf16;
    uint buf17;
    uint buf18;
    uint buf19;
    uint buf20;
    uint buf21;
    uint buf22;
    uint buf23;
    uint buf24;
    uint buf25;
    uint buf26;
    uint buf27;
    uint buf28;
    uint buf29;
    uint buf30;
    uint buf31;
    uint buf32;
    uint buf33;
    uint buf34;
    uint buf35;
    uint buf36;
    uint buf37;
    uint buf38;
    uint buf39;
    uint buf40;
    uint buf41;
    uint buf42;
    uint buf43;
    uint buf44;
    uint buf45;
    uint buf46;
    uint buf47;
    uint buf48;
    uint buf49;
    uint buf50;
    uint buf51;
    uint buf52;
    uint buf53;
    uint buf54;
    uint buf55;
    uint buf56;
    uint buf57;
    uint buf58;
    uint buf59;
    uint buf60;
    uint buf61;
    uint buf62;
    uint buf63;
    
} uart_buffer;

typedef struct {
    uint stall;
    uint clock;
    uint commits;
    uint xreg[32];
    uint pc;
    csr_state csr;
    clint_state clint;
    uart_state uart;
    uart_buffer uart_buffer;
    mmu_state mmu;

    bool reservation_en;
    uint reservation_addr;

    uint memop_src_v;
    uint memop_dst_v;
    uint memop_src_p;
    uint memop_dst_p;
    uint memop_n;

    uint stall_count;
    uint trap_count;

    uint rtc0;
    uint rtc1;

    float start_time_ref;

    uint debug_do_tick;
    uint debug_last_ins;
    uint debug_last_stall;
    
    

    uint debug_arb_0;
#ifdef CSR_FIELDS
    uint mstatus, mie, mip, stvec, sscratch, sepc, scause, stval, scounteren, fcsr;
#endif
    /*  */
    /*  */
    /*  */
    /*  */
    /*  */
    /*  */
    /*  */

    cache_t cache;
} cpu_t;

typedef struct {
    bool en;
    bool irq;
    uint type;
    uint value;
} trap;

typedef struct {
    uint write_reg;
    uint write_val;
    uint pc_val;
    uint csr_write;
    uint csr_val;
    uint mem_wr_addr;
    uint mem_wr_value;
    uint mem_wr_size;
    trap trap;
} ins_ret;

#include "l1_local.h"

// GLOBAL STATICS
static cpu_t cpu;

#ifdef PASS_TICK
// With L1_LOCAL the array is a local of the tick's main and starts undefined (zeroing a static
// one costs every tick): an entry may only be read when its occupancy bit is set.
#ifndef L1_LOCAL
static uint4 l1_cache[L1_DATA_N];
#if L1_WAYS == 4
static uint4 l1_tag[L1_BUCKETS];
#endif
#endif
// buckets whose tags have been written this pass. Up to 128 buckets (L1_TABLE_BITS 6 and below) a uint4,
// its word picked by selects: an array of four words is in local memory and read by a dynamic index on
// every probe; the uint4 made the tick 3.9% faster on the shell loop, 5.0% on Doom, 3.9% on Quake's e1m1
// (RVC_TICK_ALT on the card, 28 October 2026)
#if L1_TABLE_BITS < 7
static uint4 l1_occ = 0;
#define L1_OCC(b) (((idx_uint4(l1_occ, (b) >> 5) >> ((b) & 31)) & 1) != 0)
#define L1_OCC_SET(b) { uint occ_b_ = (b); l1_occ |= uint4((occ_b_ >> 5) == 0, (occ_b_ >> 5) == 1, (occ_b_ >> 5) == 2, (occ_b_ >> 5) == 3) * (1u << (occ_b_ & 31)); }
#else
static uint l1_occ[(L1_BUCKETS + 31) / 32];
#define L1_OCC(b) (((l1_occ[(b) >> 5] >> ((b) & 31)) & 1) != 0)
#define L1_OCC_SET(b) l1_occ[(b) >> 5] |= 1u << ((b) & 31);
#endif
#endif

// TYPE HELPERS
ins_ret ins_ret_noop() {
    ins_ret ret = (ins_ret)0;
    ret.pc_val = cpu.pc + 4;
    return ret;
}

uint sign_extend(uint x, uint b) {
    uint m = ((uint)1) << (b - 1);
    return (x ^ m) - m;
}

// The tick pass hands its write bloom filter (the OR of every written word address) to the
// commit pass in a spare state word, texel (41,0).r, so RAM texels that cannot have been
// written skip the eight write-cache lookups. The commit pass clears the word again, so the
// state after a frame is unchanged.
#ifdef PASS_TICK
static uint mem_cache_bloom = 0;
// one bit per 4 MB of RAM this tick wrote to, for a commit that skips the rest (COMMIT_BANDS)
static uint mem_dirty = 0;
// How many instructions the fast run in progress may be (cpu.h). A store the write cache has
// no room for makes it none, which is how the run ends there: its loop looks at this and at
// nothing else.
static uint fast_budget = 0;
// Searching the write cache is most of what a load or a store costs beyond its arithmetic (a
// bucket's tags are three array reads, and a full bucket sends the search on to the next
// table's), and most searches are for a texel just used: the next word of a register save, of
// a structure, of a copy. So the two texels found or made last are remembered with where they
// are, and so is the texel looked for last and not found. Entries do not move or go within a
// pass, so what is remembered stays true; a texel that is made is no longer one not found.
#if defined(X_P_ALU) || defined(X_P_READ) || defined(X_P_TEX)
#define X_PROBE
#endif
static uint probe = 0;   // X_P_*: work added to every fast instruction, to time it (docs/machine/tick-costs.md)
static uint l1_last_t = 0xffffffff, l1_last_at = 0, l1_last_t2 = 0xffffffff, l1_last_at2 = 0;
static uint l1_absent_t = 0xffffffff;
#define L1_KNOWN(t) ((t) == l1_last_t || (t) == l1_last_t2)
#define L1_KNOWN_AT(t) ((t) == l1_last_t ? l1_last_at : l1_last_at2)
#define L1_REMEMBER(t, at) { l1_last_t2 = l1_last_t; l1_last_at2 = l1_last_at; l1_last_t = t; l1_last_at = at; }
// And a bit for each texel the cache was given in this pass, by a hash of its number: a load
// of a texel whose bit is clear does not search. (The filter of addresses or-ed together,
// mem_cache_bloom, lets nine loads in ten through; half of those find nothing.)
#define L1_HAS_BITS 10
static uint l1_has[1 << (L1_HAS_BITS - 5)];
#define L1_HAS_H(t) (((t) ^ ((t) >> L1_HAS_BITS)) & ((1u << L1_HAS_BITS) - 1))
#define L1_HAS(t) (((l1_has[L1_HAS_H(t) >> 5] >> (L1_HAS_H(t) & 31)) & 1) != 0)
#define L1_HAS_SET(t) l1_has[L1_HAS_H(t) >> 5] |= 1u << (L1_HAS_H(t) & 31);
#if CORES > 1
// a worker core's word (41,0).a: 1 while it runs, else what parked it (mc.h)
static uint mc_word = 0;
#endif
#endif

#ifdef PASS_TICK
// Guest registers in an indexable array for the tick loop: one indexed store per instruction
// instead of a conditional move over every register.
#ifdef FPU
// With the F extension the float registers follow, as their bits: f0 is xr[XR_F]. A float
// instruction then reads and writes the one array, and adds nothing the fast loop carries.
#define XR_F 33
static uint xr[XR_F + 32];
// They are kept from pass to pass in eight texels of the state zone, after the TLBs' (FP_STATE_AT).
void fp_state_load() {
    for (uint k = 0; k < 8; k++) {
#if CORES > 1
        uint4 t = STATE_TEX(mc_state_at(FP_STATE_AT + k));
#else
        uint4 t = STATE_TEX(uint2((FP_STATE_AT + k) & 63, (FP_STATE_AT + k) >> 6));
#endif
        xr[XR_F + 4 * k] = t.r; xr[XR_F + 4 * k + 1] = t.g; xr[XR_F + 4 * k + 2] = t.b; xr[XR_F + 4 * k + 3] = t.a;
    }
}
// What a pixel of those texels holds after this pass; false for any other pixel.
bool fp_state_texel(uint2 pos, out uint4 t) {
    uint k = pos.x + 64 * pos.y - FP_STATE_AT;
    t = 0;
    if (pos.x + 64 * pos.y < FP_STATE_AT || k >= 8) return false;
    t = uint4(xr[XR_F + 4 * k], xr[XR_F + 4 * k + 1], xr[XR_F + 4 * k + 2], xr[XR_F + 4 * k + 3]);
    return true;
}
#else
static uint xr[33];  // [32] is a scratch slot for writes that must not land (x0, no result)
#endif
uint xreg(uint i) {
    return xr[i];
}
void xreg_load() {
    for (uint k = 0; k < 32; k++) xr[k] = cpu.xreg[k];
}
void xreg_store() {
    for (uint k = 0; k < 32; k++) cpu.xreg[k] = xr[k];
}
#elif defined(XREG_TREE)
// FXC turns cpu.xreg[i] into ~70 instructions ending in a 32-step dependent OR chain.
// A balanced select tree gives the same value with a dependency depth of 5.
uint xreg(uint i) {
    bool b0 = (i & 1) != 0, b1 = (i & 2) != 0, b2 = (i & 4) != 0, b3 = (i & 8) != 0, b4 = (i & 16) != 0;
    uint p0 = b0 ? cpu.xreg[1] : cpu.xreg[0];
    uint p1 = b0 ? cpu.xreg[3] : cpu.xreg[2];
    uint p2 = b0 ? cpu.xreg[5] : cpu.xreg[4];
    uint p3 = b0 ? cpu.xreg[7] : cpu.xreg[6];
    uint p4 = b0 ? cpu.xreg[9] : cpu.xreg[8];
    uint p5 = b0 ? cpu.xreg[11] : cpu.xreg[10];
    uint p6 = b0 ? cpu.xreg[13] : cpu.xreg[12];
    uint p7 = b0 ? cpu.xreg[15] : cpu.xreg[14];
    uint p8 = b0 ? cpu.xreg[17] : cpu.xreg[16];
    uint p9 = b0 ? cpu.xreg[19] : cpu.xreg[18];
    uint p10 = b0 ? cpu.xreg[21] : cpu.xreg[20];
    uint p11 = b0 ? cpu.xreg[23] : cpu.xreg[22];
    uint p12 = b0 ? cpu.xreg[25] : cpu.xreg[24];
    uint p13 = b0 ? cpu.xreg[27] : cpu.xreg[26];
    uint p14 = b0 ? cpu.xreg[29] : cpu.xreg[28];
    uint p15 = b0 ? cpu.xreg[31] : cpu.xreg[30];
    uint q0 = b1 ? p1 : p0;
    uint q1 = b1 ? p3 : p2;
    uint q2 = b1 ? p5 : p4;
    uint q3 = b1 ? p7 : p6;
    uint q4 = b1 ? p9 : p8;
    uint q5 = b1 ? p11 : p10;
    uint q6 = b1 ? p13 : p12;
    uint q7 = b1 ? p15 : p14;
    uint r0 = b2 ? q1 : q0;
    uint r1 = b2 ? q3 : q2;
    uint r2 = b2 ? q5 : q4;
    uint r3 = b2 ? q7 : q6;
    uint s0 = b3 ? r1 : r0;
    uint s1 = b3 ? r3 : r2;
    return b4 ? s1 : s0;
}
#else
uint xreg(uint i) {
    return cpu.xreg[i];
}
#endif

// ENCODE/DECODE LOGIC (state serialization into self texture)

// (the write cache is at L1_STATE_AT and the CSR area at CSR_STATE_AT: see the layout above)


#define STATE_WIDTH 64
#define STATE_HEIGHT 64

#ifdef PASS_TICK
void decode() {
    cpu = (cpu_t)0;

#ifdef CSR_FIELDS
    uint4 csr0 = STATE_TEX(uint2(0, 0)), csr1 = STATE_TEX(uint2(1, 0)), csr2 = STATE_TEX(uint2(2, 0));
    cpu.mstatus = csr0.r; cpu.mie = csr0.g; cpu.mip = csr0.b; cpu.stvec = csr0.a;
    cpu.sscratch = csr1.r; cpu.sepc = csr1.g; cpu.scause = csr1.b; cpu.stval = csr1.a;
    cpu.scounteren = csr2.r; cpu.fcsr = csr2.g;
#else
    cpu.cache.csr_cache_0_addr = uint(4294967295);
    cpu.cache.csr_cache_0_val = uint(3735930846);
    cpu.cache.csr_cache_1_addr = uint(4294967295);
    cpu.cache.csr_cache_1_val = uint(3735930846);
    cpu.cache.csr_cache_2_addr = uint(4294967295);
    cpu.cache.csr_cache_2_val = uint(3735930846);
    cpu.cache.csr_cache_3_addr = uint(4294967295);
    cpu.cache.csr_cache_3_val = uint(3735930846);
    cpu.cache.csr_cache_4_addr = uint(4294967295);
    cpu.cache.csr_cache_4_val = uint(3735930846);
    cpu.cache.csr_cache_5_addr = uint(4294967295);
    cpu.cache.csr_cache_5_val = uint(3735930846);
    cpu.cache.csr_cache_6_addr = uint(4294967295);
    cpu.cache.csr_cache_6_val = uint(3735930846);
    cpu.cache.csr_cache_7_addr = uint(4294967295);
    cpu.cache.csr_cache_7_val = uint(3735930846);
    cpu.cache.csr_cache_8_addr = uint(4294967295);
    cpu.cache.csr_cache_8_val = uint(3735930846);
    cpu.cache.csr_cache_9_addr = uint(4294967295);
    cpu.cache.csr_cache_9_val = uint(3735930846);
    cpu.cache.csr_cache_10_addr = uint(4294967295);
    cpu.cache.csr_cache_10_val = uint(3735930846);
    cpu.cache.csr_cache_11_addr = uint(4294967295);
    cpu.cache.csr_cache_11_val = uint(3735930846);
    cpu.cache.csr_cache_12_addr = uint(4294967295);
    cpu.cache.csr_cache_12_val = uint(3735930846);
    cpu.cache.csr_cache_13_addr = uint(4294967295);
    cpu.cache.csr_cache_13_val = uint(3735930846);
    cpu.cache.csr_cache_14_addr = uint(4294967295);
    cpu.cache.csr_cache_14_val = uint(3735930846);
    cpu.cache.csr_cache_15_addr = uint(4294967295);
    cpu.cache.csr_cache_15_val = uint(3735930846);
#endif
    cpu.cache.ram_l1_last_addr = uint(4294967295);
    cpu.cache.ram_l1_last_val = uint(3735931358);
    cpu.csr.privilege = uint(idx_uint4(STATE_TEX(uint2(8, 0)), 2));
#ifndef CSR_FIELDS
    cpu.uart.rbr_thr_ier_iir = uint(idx_uint4(STATE_TEX(uint2(8, 0)), 3));
    cpu.uart.lcr_mcr_lsr_scr = uint(idx_uint4(STATE_TEX(uint2(9, 0)), 0));
    cpu.uart.thre_ip = bool(idx_uint4(STATE_TEX(uint2(9, 0)), 1));
    cpu.uart.interrupting = bool(idx_uint4(STATE_TEX(uint2(9, 0)), 2));
    cpu.uart.input_tag = uint(idx_uint4(STATE_TEX(uint2(9, 0)), 3));
    cpu.clint.msip = bool(idx_uint4(STATE_TEX(uint2(10, 0)), 0));
#endif
    cpu.clint.mtimecmp_lo = uint(idx_uint4(STATE_TEX(uint2(10, 0)), 1));
    cpu.clint.mtimecmp_hi = uint(idx_uint4(STATE_TEX(uint2(10, 0)), 2));
#ifndef CSR_FIELDS
    cpu.clint.mtime_lo = uint(idx_uint4(STATE_TEX(uint2(10, 0)), 3));
    cpu.clint.mtime_hi = uint(idx_uint4(STATE_TEX(uint2(11, 0)), 0));
    cpu.mmu.mode = uint(idx_uint4(STATE_TEX(uint2(11, 0)), 1));
    cpu.mmu.ppn = uint(idx_uint4(STATE_TEX(uint2(11, 0)), 2));
    cpu.uart_buffer.ptr = uint(4294967295);
    cpu.uart_buffer.buf0 = uint(0);
    cpu.uart_buffer.buf1 = uint(0);
    cpu.uart_buffer.buf2 = uint(0);
    cpu.uart_buffer.buf3 = uint(0);
    cpu.uart_buffer.buf4 = uint(0);
    cpu.uart_buffer.buf5 = uint(0);
    cpu.uart_buffer.buf6 = uint(0);
    cpu.uart_buffer.buf7 = uint(0);
    cpu.uart_buffer.buf8 = uint(0);
    cpu.uart_buffer.buf9 = uint(0);
    cpu.uart_buffer.buf10 = uint(0);
    cpu.uart_buffer.buf11 = uint(0);
    cpu.uart_buffer.buf12 = uint(0);
    cpu.uart_buffer.buf13 = uint(0);
    cpu.uart_buffer.buf14 = uint(0);
    cpu.uart_buffer.buf15 = uint(0);
    cpu.uart_buffer.buf16 = uint(0);
    cpu.uart_buffer.buf17 = uint(0);
    cpu.uart_buffer.buf18 = uint(0);
    cpu.uart_buffer.buf19 = uint(0);
    cpu.uart_buffer.buf20 = uint(0);
    cpu.uart_buffer.buf21 = uint(0);
    cpu.uart_buffer.buf22 = uint(0);
    cpu.uart_buffer.buf23 = uint(0);
    cpu.uart_buffer.buf24 = uint(0);
    cpu.uart_buffer.buf25 = uint(0);
    cpu.uart_buffer.buf26 = uint(0);
    cpu.uart_buffer.buf27 = uint(0);
    cpu.uart_buffer.buf28 = uint(0);
    cpu.uart_buffer.buf29 = uint(0);
    cpu.uart_buffer.buf30 = uint(0);
    cpu.uart_buffer.buf31 = uint(0);
    cpu.uart_buffer.buf32 = uint(0);
    cpu.uart_buffer.buf33 = uint(0);
    cpu.uart_buffer.buf34 = uint(0);
    cpu.uart_buffer.buf35 = uint(0);
    cpu.uart_buffer.buf36 = uint(0);
    cpu.uart_buffer.buf37 = uint(0);
    cpu.uart_buffer.buf38 = uint(0);
    cpu.uart_buffer.buf39 = uint(0);
    cpu.uart_buffer.buf40 = uint(0);
    cpu.uart_buffer.buf41 = uint(0);
    cpu.uart_buffer.buf42 = uint(0);
    cpu.uart_buffer.buf43 = uint(0);
    cpu.uart_buffer.buf44 = uint(0);
    cpu.uart_buffer.buf45 = uint(0);
    cpu.uart_buffer.buf46 = uint(0);
    cpu.uart_buffer.buf47 = uint(0);
    cpu.uart_buffer.buf48 = uint(0);
    cpu.uart_buffer.buf49 = uint(0);
    cpu.uart_buffer.buf50 = uint(0);
    cpu.uart_buffer.buf51 = uint(0);
    cpu.uart_buffer.buf52 = uint(0);
    cpu.uart_buffer.buf53 = uint(0);
    cpu.uart_buffer.buf54 = uint(0);
    cpu.uart_buffer.buf55 = uint(0);
    cpu.uart_buffer.buf56 = uint(0);
    cpu.uart_buffer.buf57 = uint(0);
    cpu.uart_buffer.buf58 = uint(0);
    cpu.uart_buffer.buf59 = uint(0);
    cpu.uart_buffer.buf60 = uint(0);
    cpu.uart_buffer.buf61 = uint(0);
    cpu.uart_buffer.buf62 = uint(0);
    cpu.uart_buffer.buf63 = uint(0);
#endif
    cpu.stall = uint(idx_uint4(STATE_TEX(uint2(28, 0)), 0));
    cpu.clock = uint(idx_uint4(STATE_TEX(uint2(28, 0)), 1));
    cpu.commits = uint(idx_uint4(STATE_TEX(uint2(28, 0)), 2));
    cpu.xreg[0] = uint(idx_uint4(STATE_TEX(uint2(28, 0)), 3));
    cpu.xreg[1] = uint(idx_uint4(STATE_TEX(uint2(29, 0)), 0));
    cpu.xreg[2] = uint(idx_uint4(STATE_TEX(uint2(29, 0)), 1));
    cpu.xreg[3] = uint(idx_uint4(STATE_TEX(uint2(29, 0)), 2));
    cpu.xreg[4] = uint(idx_uint4(STATE_TEX(uint2(29, 0)), 3));
    cpu.xreg[5] = uint(idx_uint4(STATE_TEX(uint2(30, 0)), 0));
    cpu.xreg[6] = uint(idx_uint4(STATE_TEX(uint2(30, 0)), 1));
    cpu.xreg[7] = uint(idx_uint4(STATE_TEX(uint2(30, 0)), 2));
    cpu.xreg[8] = uint(idx_uint4(STATE_TEX(uint2(30, 0)), 3));
    cpu.xreg[9] = uint(idx_uint4(STATE_TEX(uint2(31, 0)), 0));
    cpu.xreg[10] = uint(idx_uint4(STATE_TEX(uint2(31, 0)), 1));
    cpu.xreg[11] = uint(idx_uint4(STATE_TEX(uint2(31, 0)), 2));
    cpu.xreg[12] = uint(idx_uint4(STATE_TEX(uint2(31, 0)), 3));
    cpu.xreg[13] = uint(idx_uint4(STATE_TEX(uint2(32, 0)), 0));
    cpu.xreg[14] = uint(idx_uint4(STATE_TEX(uint2(32, 0)), 1));
    cpu.xreg[15] = uint(idx_uint4(STATE_TEX(uint2(32, 0)), 2));
    cpu.xreg[16] = uint(idx_uint4(STATE_TEX(uint2(32, 0)), 3));
    cpu.xreg[17] = uint(idx_uint4(STATE_TEX(uint2(33, 0)), 0));
    cpu.xreg[18] = uint(idx_uint4(STATE_TEX(uint2(33, 0)), 1));
    cpu.xreg[19] = uint(idx_uint4(STATE_TEX(uint2(33, 0)), 2));
    cpu.xreg[20] = uint(idx_uint4(STATE_TEX(uint2(33, 0)), 3));
    cpu.xreg[21] = uint(idx_uint4(STATE_TEX(uint2(34, 0)), 0));
    cpu.xreg[22] = uint(idx_uint4(STATE_TEX(uint2(34, 0)), 1));
    cpu.xreg[23] = uint(idx_uint4(STATE_TEX(uint2(34, 0)), 2));
    cpu.xreg[24] = uint(idx_uint4(STATE_TEX(uint2(34, 0)), 3));
    cpu.xreg[25] = uint(idx_uint4(STATE_TEX(uint2(35, 0)), 0));
    cpu.xreg[26] = uint(idx_uint4(STATE_TEX(uint2(35, 0)), 1));
    cpu.xreg[27] = uint(idx_uint4(STATE_TEX(uint2(35, 0)), 2));
    cpu.xreg[28] = uint(idx_uint4(STATE_TEX(uint2(35, 0)), 3));
    cpu.xreg[29] = uint(idx_uint4(STATE_TEX(uint2(36, 0)), 0));
    cpu.xreg[30] = uint(idx_uint4(STATE_TEX(uint2(36, 0)), 1));
    cpu.xreg[31] = uint(idx_uint4(STATE_TEX(uint2(36, 0)), 2));
    cpu.pc = uint(idx_uint4(STATE_TEX(uint2(36, 0)), 3));
    cpu.reservation_en = bool(idx_uint4(STATE_TEX(uint2(37, 0)), 0));
    cpu.reservation_addr = uint(idx_uint4(STATE_TEX(uint2(37, 0)), 1));
    cpu.memop_src_v = uint(idx_uint4(STATE_TEX(uint2(37, 0)), 2));
    cpu.memop_dst_v = uint(idx_uint4(STATE_TEX(uint2(37, 0)), 3));
#ifndef CSR_FIELDS
    cpu.memop_src_p = uint(idx_uint4(STATE_TEX(uint2(38, 0)), 0));
    cpu.memop_dst_p = uint(idx_uint4(STATE_TEX(uint2(38, 0)), 1));
#endif
    cpu.memop_n = uint(idx_uint4(STATE_TEX(uint2(38, 0)), 2));
    cpu.stall_count = uint(idx_uint4(STATE_TEX(uint2(38, 0)), 3));
    cpu.trap_count = uint(idx_uint4(STATE_TEX(uint2(39, 0)), 0));
#ifndef CSR_FIELDS
    cpu.rtc0 = uint(idx_uint4(STATE_TEX(uint2(39, 0)), 1));
    cpu.rtc1 = uint(idx_uint4(STATE_TEX(uint2(39, 0)), 2));
    cpu.start_time_ref = asfloat(idx_uint4(STATE_TEX(uint2(39, 0)), 3));
    cpu.debug_do_tick = uint(idx_uint4(STATE_TEX(uint2(40, 0)), 0));
#endif
    cpu.debug_last_ins = uint(idx_uint4(STATE_TEX(uint2(40, 0)), 1));
    cpu.debug_last_stall = uint(idx_uint4(STATE_TEX(uint2(40, 0)), 2));
    // The fills and copies this pass leaves to the commit (LAZY_MEMOP): none yet. The first is
    // debug_arb_0 (1 a copy, 2 a fill, and its texels from bit 2), trap_count (its destination
    // in RAM) and debug_last_ins (its source, or the fill word); the second is stall_count,
    // memop_dst_p and memop_src_p, which a fill or copy that ends the pass has otherwise.
    cpu.debug_arb_0 = 0;
    cpu.stall_count = 0;

}
#endif

#ifdef PASS_COMMIT
#define NEEDS_DFC
#endif
#ifdef PASS_FB
#define NEEDS_DFC
#endif

#ifdef NEEDS_DFC
void decode_for_commit() {
    cpu = (cpu_t)0;

    cpu.cache.csr_cache_0_addr = uint(idx_uint4(STATE_TEX_HART(uint2(0, 0), 0), 0));
    cpu.cache.csr_cache_0_val = uint(idx_uint4(STATE_TEX_HART(uint2(0, 0), 0), 1));
    cpu.cache.csr_cache_1_addr = uint(idx_uint4(STATE_TEX_HART(uint2(0, 0), 0), 2));
    cpu.cache.csr_cache_1_val = uint(idx_uint4(STATE_TEX_HART(uint2(0, 0), 0), 3));
    cpu.cache.csr_cache_2_addr = uint(idx_uint4(STATE_TEX_HART(uint2(1, 0), 0), 0));
    cpu.cache.csr_cache_2_val = uint(idx_uint4(STATE_TEX_HART(uint2(1, 0), 0), 1));
    cpu.cache.csr_cache_3_addr = uint(idx_uint4(STATE_TEX_HART(uint2(1, 0), 0), 2));
    cpu.cache.csr_cache_3_val = uint(idx_uint4(STATE_TEX_HART(uint2(1, 0), 0), 3));
    cpu.cache.csr_cache_4_addr = uint(idx_uint4(STATE_TEX_HART(uint2(2, 0), 0), 0));
    cpu.cache.csr_cache_4_val = uint(idx_uint4(STATE_TEX_HART(uint2(2, 0), 0), 1));
    cpu.cache.csr_cache_5_addr = uint(idx_uint4(STATE_TEX_HART(uint2(2, 0), 0), 2));
    cpu.cache.csr_cache_5_val = uint(idx_uint4(STATE_TEX_HART(uint2(2, 0), 0), 3));
    cpu.cache.csr_cache_6_addr = uint(idx_uint4(STATE_TEX_HART(uint2(3, 0), 0), 0));
    cpu.cache.csr_cache_6_val = uint(idx_uint4(STATE_TEX_HART(uint2(3, 0), 0), 1));
    cpu.cache.csr_cache_7_addr = uint(idx_uint4(STATE_TEX_HART(uint2(3, 0), 0), 2));
    cpu.cache.csr_cache_7_val = uint(idx_uint4(STATE_TEX_HART(uint2(3, 0), 0), 3));
    cpu.cache.csr_cache_8_addr = uint(idx_uint4(STATE_TEX_HART(uint2(4, 0), 0), 0));
    cpu.cache.csr_cache_8_val = uint(idx_uint4(STATE_TEX_HART(uint2(4, 0), 0), 1));
    cpu.cache.csr_cache_9_addr = uint(idx_uint4(STATE_TEX_HART(uint2(4, 0), 0), 2));
    cpu.cache.csr_cache_9_val = uint(idx_uint4(STATE_TEX_HART(uint2(4, 0), 0), 3));
    cpu.cache.csr_cache_10_addr = uint(idx_uint4(STATE_TEX_HART(uint2(5, 0), 0), 0));
    cpu.cache.csr_cache_10_val = uint(idx_uint4(STATE_TEX_HART(uint2(5, 0), 0), 1));
    cpu.cache.csr_cache_11_addr = uint(idx_uint4(STATE_TEX_HART(uint2(5, 0), 0), 2));
    cpu.cache.csr_cache_11_val = uint(idx_uint4(STATE_TEX_HART(uint2(5, 0), 0), 3));
    cpu.cache.csr_cache_12_addr = uint(idx_uint4(STATE_TEX_HART(uint2(6, 0), 0), 0));
    cpu.cache.csr_cache_12_val = uint(idx_uint4(STATE_TEX_HART(uint2(6, 0), 0), 1));
    cpu.cache.csr_cache_13_addr = uint(idx_uint4(STATE_TEX_HART(uint2(6, 0), 0), 2));
    cpu.cache.csr_cache_13_val = uint(idx_uint4(STATE_TEX_HART(uint2(6, 0), 0), 3));
    cpu.cache.csr_cache_14_addr = uint(idx_uint4(STATE_TEX_HART(uint2(7, 0), 0), 0));
    cpu.cache.csr_cache_14_val = uint(idx_uint4(STATE_TEX_HART(uint2(7, 0), 0), 1));
    cpu.cache.csr_cache_15_addr = uint(idx_uint4(STATE_TEX_HART(uint2(7, 0), 0), 2));
    cpu.cache.csr_cache_15_val = uint(idx_uint4(STATE_TEX_HART(uint2(7, 0), 0), 3));
    cpu.cache.ram_l1_last_addr = uint(idx_uint4(STATE_TEX_HART(uint2(8, 0), 0), 0));
    cpu.cache.ram_l1_last_val = uint(idx_uint4(STATE_TEX_HART(uint2(8, 0), 0), 1));
    cpu.csr.privilege = uint(idx_uint4(STATE_TEX_HART(uint2(8, 0), 0), 2));
    cpu.uart.rbr_thr_ier_iir = uint(idx_uint4(STATE_TEX_HART(uint2(8, 0), 0), 3));
    cpu.uart.lcr_mcr_lsr_scr = uint(idx_uint4(STATE_TEX_HART(uint2(9, 0), 0), 0));
    cpu.uart.thre_ip = bool(idx_uint4(STATE_TEX_HART(uint2(9, 0), 0), 1));
    cpu.uart.interrupting = bool(idx_uint4(STATE_TEX_HART(uint2(9, 0), 0), 2));
    cpu.uart.input_tag = uint(idx_uint4(STATE_TEX_HART(uint2(9, 0), 0), 3));
    cpu.clint.msip = bool(idx_uint4(STATE_TEX_HART(uint2(10, 0), 0), 0));
    cpu.clint.mtimecmp_lo = uint(idx_uint4(STATE_TEX_HART(uint2(10, 0), 0), 1));
    cpu.clint.mtimecmp_hi = uint(idx_uint4(STATE_TEX_HART(uint2(10, 0), 0), 2));
    cpu.clint.mtime_lo = uint(idx_uint4(STATE_TEX_HART(uint2(10, 0), 0), 3));
    cpu.clint.mtime_hi = uint(idx_uint4(STATE_TEX_HART(uint2(11, 0), 0), 0));
    cpu.mmu.mode = uint(idx_uint4(STATE_TEX_HART(uint2(11, 0), 0), 1));
    cpu.mmu.ppn = uint(idx_uint4(STATE_TEX_HART(uint2(11, 0), 0), 2));
    cpu.uart_buffer.ptr = uint(idx_uint4(STATE_TEX_HART(uint2(11, 0), 0), 3));
    cpu.uart_buffer.buf0 = uint(idx_uint4(STATE_TEX_HART(uint2(12, 0), 0), 0));
    cpu.uart_buffer.buf1 = uint(idx_uint4(STATE_TEX_HART(uint2(12, 0), 0), 1));
    cpu.uart_buffer.buf2 = uint(idx_uint4(STATE_TEX_HART(uint2(12, 0), 0), 2));
    cpu.uart_buffer.buf3 = uint(idx_uint4(STATE_TEX_HART(uint2(12, 0), 0), 3));
    cpu.uart_buffer.buf4 = uint(idx_uint4(STATE_TEX_HART(uint2(13, 0), 0), 0));
    cpu.uart_buffer.buf5 = uint(idx_uint4(STATE_TEX_HART(uint2(13, 0), 0), 1));
    cpu.uart_buffer.buf6 = uint(idx_uint4(STATE_TEX_HART(uint2(13, 0), 0), 2));
    cpu.uart_buffer.buf7 = uint(idx_uint4(STATE_TEX_HART(uint2(13, 0), 0), 3));
    cpu.uart_buffer.buf8 = uint(idx_uint4(STATE_TEX_HART(uint2(14, 0), 0), 0));
    cpu.uart_buffer.buf9 = uint(idx_uint4(STATE_TEX_HART(uint2(14, 0), 0), 1));
    cpu.uart_buffer.buf10 = uint(idx_uint4(STATE_TEX_HART(uint2(14, 0), 0), 2));
    cpu.uart_buffer.buf11 = uint(idx_uint4(STATE_TEX_HART(uint2(14, 0), 0), 3));
    cpu.uart_buffer.buf12 = uint(idx_uint4(STATE_TEX_HART(uint2(15, 0), 0), 0));
    cpu.uart_buffer.buf13 = uint(idx_uint4(STATE_TEX_HART(uint2(15, 0), 0), 1));
    cpu.uart_buffer.buf14 = uint(idx_uint4(STATE_TEX_HART(uint2(15, 0), 0), 2));
    cpu.uart_buffer.buf15 = uint(idx_uint4(STATE_TEX_HART(uint2(15, 0), 0), 3));
    cpu.uart_buffer.buf16 = uint(idx_uint4(STATE_TEX_HART(uint2(16, 0), 0), 0));
    cpu.uart_buffer.buf17 = uint(idx_uint4(STATE_TEX_HART(uint2(16, 0), 0), 1));
    cpu.uart_buffer.buf18 = uint(idx_uint4(STATE_TEX_HART(uint2(16, 0), 0), 2));
    cpu.uart_buffer.buf19 = uint(idx_uint4(STATE_TEX_HART(uint2(16, 0), 0), 3));
    cpu.uart_buffer.buf20 = uint(idx_uint4(STATE_TEX_HART(uint2(17, 0), 0), 0));
    cpu.uart_buffer.buf21 = uint(idx_uint4(STATE_TEX_HART(uint2(17, 0), 0), 1));
    cpu.uart_buffer.buf22 = uint(idx_uint4(STATE_TEX_HART(uint2(17, 0), 0), 2));
    cpu.uart_buffer.buf23 = uint(idx_uint4(STATE_TEX_HART(uint2(17, 0), 0), 3));
    cpu.uart_buffer.buf24 = uint(idx_uint4(STATE_TEX_HART(uint2(18, 0), 0), 0));
    cpu.uart_buffer.buf25 = uint(idx_uint4(STATE_TEX_HART(uint2(18, 0), 0), 1));
    cpu.uart_buffer.buf26 = uint(idx_uint4(STATE_TEX_HART(uint2(18, 0), 0), 2));
    cpu.uart_buffer.buf27 = uint(idx_uint4(STATE_TEX_HART(uint2(18, 0), 0), 3));
    cpu.uart_buffer.buf28 = uint(idx_uint4(STATE_TEX_HART(uint2(19, 0), 0), 0));
    cpu.uart_buffer.buf29 = uint(idx_uint4(STATE_TEX_HART(uint2(19, 0), 0), 1));
    cpu.uart_buffer.buf30 = uint(idx_uint4(STATE_TEX_HART(uint2(19, 0), 0), 2));
    cpu.uart_buffer.buf31 = uint(idx_uint4(STATE_TEX_HART(uint2(19, 0), 0), 3));
    cpu.uart_buffer.buf32 = uint(idx_uint4(STATE_TEX_HART(uint2(20, 0), 0), 0));
    cpu.uart_buffer.buf33 = uint(idx_uint4(STATE_TEX_HART(uint2(20, 0), 0), 1));
    cpu.uart_buffer.buf34 = uint(idx_uint4(STATE_TEX_HART(uint2(20, 0), 0), 2));
    cpu.uart_buffer.buf35 = uint(idx_uint4(STATE_TEX_HART(uint2(20, 0), 0), 3));
    cpu.uart_buffer.buf36 = uint(idx_uint4(STATE_TEX_HART(uint2(21, 0), 0), 0));
    cpu.uart_buffer.buf37 = uint(idx_uint4(STATE_TEX_HART(uint2(21, 0), 0), 1));
    cpu.uart_buffer.buf38 = uint(idx_uint4(STATE_TEX_HART(uint2(21, 0), 0), 2));
    cpu.uart_buffer.buf39 = uint(idx_uint4(STATE_TEX_HART(uint2(21, 0), 0), 3));
    cpu.uart_buffer.buf40 = uint(idx_uint4(STATE_TEX_HART(uint2(22, 0), 0), 0));
    cpu.uart_buffer.buf41 = uint(idx_uint4(STATE_TEX_HART(uint2(22, 0), 0), 1));
    cpu.uart_buffer.buf42 = uint(idx_uint4(STATE_TEX_HART(uint2(22, 0), 0), 2));
    cpu.uart_buffer.buf43 = uint(idx_uint4(STATE_TEX_HART(uint2(22, 0), 0), 3));
    cpu.uart_buffer.buf44 = uint(idx_uint4(STATE_TEX_HART(uint2(23, 0), 0), 0));
    cpu.uart_buffer.buf45 = uint(idx_uint4(STATE_TEX_HART(uint2(23, 0), 0), 1));
    cpu.uart_buffer.buf46 = uint(idx_uint4(STATE_TEX_HART(uint2(23, 0), 0), 2));
    cpu.uart_buffer.buf47 = uint(idx_uint4(STATE_TEX_HART(uint2(23, 0), 0), 3));
    cpu.uart_buffer.buf48 = uint(idx_uint4(STATE_TEX_HART(uint2(24, 0), 0), 0));
    cpu.uart_buffer.buf49 = uint(idx_uint4(STATE_TEX_HART(uint2(24, 0), 0), 1));
    cpu.uart_buffer.buf50 = uint(idx_uint4(STATE_TEX_HART(uint2(24, 0), 0), 2));
    cpu.uart_buffer.buf51 = uint(idx_uint4(STATE_TEX_HART(uint2(24, 0), 0), 3));
    cpu.uart_buffer.buf52 = uint(idx_uint4(STATE_TEX_HART(uint2(25, 0), 0), 0));
    cpu.uart_buffer.buf53 = uint(idx_uint4(STATE_TEX_HART(uint2(25, 0), 0), 1));
    cpu.uart_buffer.buf54 = uint(idx_uint4(STATE_TEX_HART(uint2(25, 0), 0), 2));
    cpu.uart_buffer.buf55 = uint(idx_uint4(STATE_TEX_HART(uint2(25, 0), 0), 3));
    cpu.uart_buffer.buf56 = uint(idx_uint4(STATE_TEX_HART(uint2(26, 0), 0), 0));
    cpu.uart_buffer.buf57 = uint(idx_uint4(STATE_TEX_HART(uint2(26, 0), 0), 1));
    cpu.uart_buffer.buf58 = uint(idx_uint4(STATE_TEX_HART(uint2(26, 0), 0), 2));
    cpu.uart_buffer.buf59 = uint(idx_uint4(STATE_TEX_HART(uint2(26, 0), 0), 3));
    cpu.uart_buffer.buf60 = uint(idx_uint4(STATE_TEX_HART(uint2(27, 0), 0), 0));
    cpu.uart_buffer.buf61 = uint(idx_uint4(STATE_TEX_HART(uint2(27, 0), 0), 1));
    cpu.uart_buffer.buf62 = uint(idx_uint4(STATE_TEX_HART(uint2(27, 0), 0), 2));
    cpu.uart_buffer.buf63 = uint(idx_uint4(STATE_TEX_HART(uint2(27, 0), 0), 3));
    cpu.stall = uint(idx_uint4(STATE_TEX_HART(uint2(28, 0), 0), 0));
    cpu.clock = uint(idx_uint4(STATE_TEX_HART(uint2(28, 0), 0), 1));
    cpu.commits = uint(idx_uint4(STATE_TEX_HART(uint2(28, 0), 0), 2));
    cpu.xreg[0] = uint(idx_uint4(STATE_TEX_HART(uint2(28, 0), 0), 3));
    cpu.xreg[1] = uint(idx_uint4(STATE_TEX_HART(uint2(29, 0), 0), 0));
    cpu.xreg[2] = uint(idx_uint4(STATE_TEX_HART(uint2(29, 0), 0), 1));
    cpu.xreg[3] = uint(idx_uint4(STATE_TEX_HART(uint2(29, 0), 0), 2));
    cpu.xreg[4] = uint(idx_uint4(STATE_TEX_HART(uint2(29, 0), 0), 3));
    cpu.xreg[5] = uint(idx_uint4(STATE_TEX_HART(uint2(30, 0), 0), 0));
    cpu.xreg[6] = uint(idx_uint4(STATE_TEX_HART(uint2(30, 0), 0), 1));
    cpu.xreg[7] = uint(idx_uint4(STATE_TEX_HART(uint2(30, 0), 0), 2));
    cpu.xreg[8] = uint(idx_uint4(STATE_TEX_HART(uint2(30, 0), 0), 3));
    cpu.xreg[9] = uint(idx_uint4(STATE_TEX_HART(uint2(31, 0), 0), 0));
    cpu.xreg[10] = uint(idx_uint4(STATE_TEX_HART(uint2(31, 0), 0), 1));
    cpu.xreg[11] = uint(idx_uint4(STATE_TEX_HART(uint2(31, 0), 0), 2));
    cpu.xreg[12] = uint(idx_uint4(STATE_TEX_HART(uint2(31, 0), 0), 3));
    cpu.xreg[13] = uint(idx_uint4(STATE_TEX_HART(uint2(32, 0), 0), 0));
    cpu.xreg[14] = uint(idx_uint4(STATE_TEX_HART(uint2(32, 0), 0), 1));
    cpu.xreg[15] = uint(idx_uint4(STATE_TEX_HART(uint2(32, 0), 0), 2));
    cpu.xreg[16] = uint(idx_uint4(STATE_TEX_HART(uint2(32, 0), 0), 3));
    cpu.xreg[17] = uint(idx_uint4(STATE_TEX_HART(uint2(33, 0), 0), 0));
    cpu.xreg[18] = uint(idx_uint4(STATE_TEX_HART(uint2(33, 0), 0), 1));
    cpu.xreg[19] = uint(idx_uint4(STATE_TEX_HART(uint2(33, 0), 0), 2));
    cpu.xreg[20] = uint(idx_uint4(STATE_TEX_HART(uint2(33, 0), 0), 3));
    cpu.xreg[21] = uint(idx_uint4(STATE_TEX_HART(uint2(34, 0), 0), 0));
    cpu.xreg[22] = uint(idx_uint4(STATE_TEX_HART(uint2(34, 0), 0), 1));
    cpu.xreg[23] = uint(idx_uint4(STATE_TEX_HART(uint2(34, 0), 0), 2));
    cpu.xreg[24] = uint(idx_uint4(STATE_TEX_HART(uint2(34, 0), 0), 3));
    cpu.xreg[25] = uint(idx_uint4(STATE_TEX_HART(uint2(35, 0), 0), 0));
    cpu.xreg[26] = uint(idx_uint4(STATE_TEX_HART(uint2(35, 0), 0), 1));
    cpu.xreg[27] = uint(idx_uint4(STATE_TEX_HART(uint2(35, 0), 0), 2));
    cpu.xreg[28] = uint(idx_uint4(STATE_TEX_HART(uint2(35, 0), 0), 3));
    cpu.xreg[29] = uint(idx_uint4(STATE_TEX_HART(uint2(36, 0), 0), 0));
    cpu.xreg[30] = uint(idx_uint4(STATE_TEX_HART(uint2(36, 0), 0), 1));
    cpu.xreg[31] = uint(idx_uint4(STATE_TEX_HART(uint2(36, 0), 0), 2));
    cpu.pc = uint(idx_uint4(STATE_TEX_HART(uint2(36, 0), 0), 3));
    cpu.reservation_en = bool(idx_uint4(STATE_TEX_HART(uint2(37, 0), 0), 0));
    cpu.reservation_addr = uint(idx_uint4(STATE_TEX_HART(uint2(37, 0), 0), 1));
    cpu.memop_src_v = uint(idx_uint4(STATE_TEX_HART(uint2(37, 0), 0), 2));
    cpu.memop_dst_v = uint(idx_uint4(STATE_TEX_HART(uint2(37, 0), 0), 3));
    cpu.memop_src_p = uint(idx_uint4(STATE_TEX_HART(uint2(38, 0), 0), 0));
    cpu.memop_dst_p = uint(idx_uint4(STATE_TEX_HART(uint2(38, 0), 0), 1));
    cpu.memop_n = uint(idx_uint4(STATE_TEX_HART(uint2(38, 0), 0), 2));
    cpu.stall_count = uint(idx_uint4(STATE_TEX_HART(uint2(38, 0), 0), 3));
    cpu.trap_count = uint(idx_uint4(STATE_TEX_HART(uint2(39, 0), 0), 0));
    cpu.rtc0 = uint(idx_uint4(STATE_TEX_HART(uint2(39, 0), 0), 1));
    cpu.rtc1 = uint(idx_uint4(STATE_TEX_HART(uint2(39, 0), 0), 2));
    cpu.start_time_ref = asfloat(idx_uint4(STATE_TEX_HART(uint2(39, 0), 0), 3));
    cpu.debug_do_tick = uint(idx_uint4(STATE_TEX_HART(uint2(40, 0), 0), 0));
    cpu.debug_last_ins = uint(idx_uint4(STATE_TEX_HART(uint2(40, 0), 0), 1));
    cpu.debug_last_stall = uint(idx_uint4(STATE_TEX_HART(uint2(40, 0), 0), 2));
    cpu.debug_arb_0 = uint(idx_uint4(STATE_TEX_HART(uint2(40, 0), 0), 3));
#ifdef CSR_FIELDS
    // a copy's addresses are its virtual ones (no translation); the commit's geometry reads 37 too
    cpu.memop_src_p = cpu.memop_src_v;
    cpu.memop_dst_p = cpu.memop_dst_v & 0x7fffffff;
#endif

}
#endif

// The second-level and megapage TLBs (mmu.h) are kept from pass to pass in the state zone, after
// the write cache (TLB_STATE_AT): two entries (tag, page) a texel, then one texel with the generation.

#ifdef PASS_TICK
bool pixel_has_state(uint2 pos) {
    // CSR data area and FB are not "state", they're only touched by commit
    uint lin = pos.x + 64 * pos.y;
#if CORES > 1
    if (hart != 0) {   // a worker: no TLBs
#ifdef FPU
        if (lin >= FP_STATE_AT && lin < FP_STATE_AT + 8) return true;
#endif
        return lin < L1_STATE_AT + L1_ENTRIES;
    }
#endif
#ifndef NO_PAGING
    if (lin >= TLB_STATE_AT && lin < TLB_STATE_AT + TLB_STATE_TEXELS) return true;
#endif
#ifdef FPU
    if (lin >= FP_STATE_AT && lin < FP_STATE_AT + 8) return true;
#endif
    return lin < L1_STATE_AT + L1_ENTRIES;
}

uint4 encode_l1(L1P uint2 pos) {
    // L1 cache
    uint s_lin = pos.x + pos.y * 64;
    if (s_lin >= L1_STATE_AT && s_lin < L1_STATE_AT + L1_ENTRIES) {
        uint offset = s_lin - L1_STATE_AT;
        // a bucket that was not used, and an entry of it that was not, are zeros (the array
        // itself starts undefined with L1_LOCAL)
        uint bucket = offset / L1_STRIDE, entry = offset % L1_STRIDE;
        if (!L1_OCC(bucket)) return 0;
        uint4 tags = L1_TAGS(bucket);
        if (entry == 0) return tags;
        if (idx_uint4(tags, entry - 1) == 0) return 0;
        return L1_DATA(bucket * 4 + entry);
    }

    // fallback is passthrough
    uint4 ret = STATE_TEX(pos);

    if (pos.x == 41 && pos.y == 0) {
        ret.r = mem_cache_bloom;
#if CORES > 1
        ret.a = mc_word;
#endif
#ifdef COMMIT_BANDS
        ret.g = mem_dirty;
#endif
        return ret;
    }

    // cpu_t serialization
    uint pos_id = pos.x | (pos.y << 16);
#ifdef CSR_FIELDS
    if (pos_id == 0) return uint4(cpu.mstatus, cpu.mie, cpu.mip, cpu.stvec);
    if (pos_id == 1) return uint4(cpu.sscratch, cpu.sepc, cpu.scause, cpu.stval);
    if (pos_id == 2) return uint4(cpu.scounteren, cpu.fcsr, 0, 0);
    if (pos_id < 8) return (uint4)0;
    // texel 11's last word is the old console's "nothing printed"; 12 to 27 were its buffer
    if (pos_id == 8) return uint4(cpu.cache.ram_l1_last_addr, cpu.cache.ram_l1_last_val, cpu.csr.privilege, 0);
    if (pos_id == 10) return uint4(0, cpu.clint.mtimecmp_lo, cpu.clint.mtimecmp_hi, 0);
    if (pos_id == 11) return uint4(0, 0, 0, 0xffffffff);
    if (pos_id < 28) return (uint4)0;
    if (pos_id == 38) return uint4(0, 0, cpu.memop_n, cpu.stall_count);
    if (pos_id == 39) return uint4(cpu.trap_count, 0, 0, 0);
    if (pos_id == 40) return uint4(0, cpu.debug_last_ins, cpu.stall == 0 ? cpu.debug_last_stall : cpu.stall, cpu.debug_arb_0);
#endif
    [forcecase]
    switch (pos_id) {
    case 0:
        ret.r = uint(cpu.cache.csr_cache_0_addr);
        ret.g = uint(cpu.cache.csr_cache_0_val);
        ret.b = uint(cpu.cache.csr_cache_1_addr);
        ret.a = uint(cpu.cache.csr_cache_1_val);
        return ret;
    case 1:
        ret.r = uint(cpu.cache.csr_cache_2_addr);
        ret.g = uint(cpu.cache.csr_cache_2_val);
        ret.b = uint(cpu.cache.csr_cache_3_addr);
        ret.a = uint(cpu.cache.csr_cache_3_val);
        return ret;
    case 10:
        ret.r = uint(cpu.clint.msip);
        ret.a = uint(cpu.clint.mtime_lo);
        ret.b = uint(cpu.clint.mtimecmp_hi);
        ret.g = uint(cpu.clint.mtimecmp_lo);
        return ret;
    case 11:
        ret.r = uint(cpu.clint.mtime_hi);
        ret.g = uint(cpu.mmu.mode);
        ret.b = uint(cpu.mmu.ppn);
        ret.a = uint(cpu.uart_buffer.ptr);
        return ret;
    case 12:
        ret.r = uint(cpu.uart_buffer.buf0);
        ret.g = uint(cpu.uart_buffer.buf1);
        ret.b = uint(cpu.uart_buffer.buf2);
        ret.a = uint(cpu.uart_buffer.buf3);
        return ret;
    case 13:
        ret.r = uint(cpu.uart_buffer.buf4);
        ret.g = uint(cpu.uart_buffer.buf5);
        ret.b = uint(cpu.uart_buffer.buf6);
        ret.a = uint(cpu.uart_buffer.buf7);
        return ret;
    case 14:
        ret.b = uint(cpu.uart_buffer.buf10);
        ret.a = uint(cpu.uart_buffer.buf11);
        ret.r = uint(cpu.uart_buffer.buf8);
        ret.g = uint(cpu.uart_buffer.buf9);
        return ret;
    case 15:
        ret.r = uint(cpu.uart_buffer.buf12);
        ret.g = uint(cpu.uart_buffer.buf13);
        ret.b = uint(cpu.uart_buffer.buf14);
        ret.a = uint(cpu.uart_buffer.buf15);
        return ret;
    case 16:
        ret.r = uint(cpu.uart_buffer.buf16);
        ret.g = uint(cpu.uart_buffer.buf17);
        ret.b = uint(cpu.uart_buffer.buf18);
        ret.a = uint(cpu.uart_buffer.buf19);
        return ret;
    case 17:
        ret.r = uint(cpu.uart_buffer.buf20);
        ret.g = uint(cpu.uart_buffer.buf21);
        ret.b = uint(cpu.uart_buffer.buf22);
        ret.a = uint(cpu.uart_buffer.buf23);
        return ret;
    case 18:
        ret.r = uint(cpu.uart_buffer.buf24);
        ret.g = uint(cpu.uart_buffer.buf25);
        ret.b = uint(cpu.uart_buffer.buf26);
        ret.a = uint(cpu.uart_buffer.buf27);
        return ret;
    case 19:
        ret.r = uint(cpu.uart_buffer.buf28);
        ret.g = uint(cpu.uart_buffer.buf29);
        ret.b = uint(cpu.uart_buffer.buf30);
        ret.a = uint(cpu.uart_buffer.buf31);
        return ret;
    case 2:
        ret.r = uint(cpu.cache.csr_cache_4_addr);
        ret.g = uint(cpu.cache.csr_cache_4_val);
        ret.b = uint(cpu.cache.csr_cache_5_addr);
        ret.a = uint(cpu.cache.csr_cache_5_val);
        return ret;
    case 20:
        ret.r = uint(cpu.uart_buffer.buf32);
        ret.g = uint(cpu.uart_buffer.buf33);
        ret.b = uint(cpu.uart_buffer.buf34);
        ret.a = uint(cpu.uart_buffer.buf35);
        return ret;
    case 21:
        ret.r = uint(cpu.uart_buffer.buf36);
        ret.g = uint(cpu.uart_buffer.buf37);
        ret.b = uint(cpu.uart_buffer.buf38);
        ret.a = uint(cpu.uart_buffer.buf39);
        return ret;
    case 22:
        ret.r = uint(cpu.uart_buffer.buf40);
        ret.g = uint(cpu.uart_buffer.buf41);
        ret.b = uint(cpu.uart_buffer.buf42);
        ret.a = uint(cpu.uart_buffer.buf43);
        return ret;
    case 23:
        ret.r = uint(cpu.uart_buffer.buf44);
        ret.g = uint(cpu.uart_buffer.buf45);
        ret.b = uint(cpu.uart_buffer.buf46);
        ret.a = uint(cpu.uart_buffer.buf47);
        return ret;
    case 24:
        ret.r = uint(cpu.uart_buffer.buf48);
        ret.g = uint(cpu.uart_buffer.buf49);
        ret.b = uint(cpu.uart_buffer.buf50);
        ret.a = uint(cpu.uart_buffer.buf51);
        return ret;
    case 25:
        ret.r = uint(cpu.uart_buffer.buf52);
        ret.g = uint(cpu.uart_buffer.buf53);
        ret.b = uint(cpu.uart_buffer.buf54);
        ret.a = uint(cpu.uart_buffer.buf55);
        return ret;
    case 26:
        ret.r = uint(cpu.uart_buffer.buf56);
        ret.g = uint(cpu.uart_buffer.buf57);
        ret.b = uint(cpu.uart_buffer.buf58);
        ret.a = uint(cpu.uart_buffer.buf59);
        return ret;
    case 27:
        ret.r = uint(cpu.uart_buffer.buf60);
        ret.g = uint(cpu.uart_buffer.buf61);
        ret.b = uint(cpu.uart_buffer.buf62);
        ret.a = uint(cpu.uart_buffer.buf63);
        return ret;
    case 28:
        ret.g = uint(cpu.clock);
        ret.b = uint(cpu.commits);
        ret.r = uint(cpu.stall);
        ret.a = uint(cpu.xreg[0]);
        return ret;
    case 29:
        ret.r = uint(cpu.xreg[1]);
        ret.g = uint(cpu.xreg[2]);
        ret.b = uint(cpu.xreg[3]);
        ret.a = uint(cpu.xreg[4]);
        return ret;
    case 3:
        ret.r = uint(cpu.cache.csr_cache_6_addr);
        ret.g = uint(cpu.cache.csr_cache_6_val);
        ret.b = uint(cpu.cache.csr_cache_7_addr);
        ret.a = uint(cpu.cache.csr_cache_7_val);
        return ret;
    case 30:
        ret.r = uint(cpu.xreg[5]);
        ret.g = uint(cpu.xreg[6]);
        ret.b = uint(cpu.xreg[7]);
        ret.a = uint(cpu.xreg[8]);
        return ret;
    case 31:
        ret.g = uint(cpu.xreg[10]);
        ret.b = uint(cpu.xreg[11]);
        ret.a = uint(cpu.xreg[12]);
        ret.r = uint(cpu.xreg[9]);
        return ret;
    case 32:
        ret.r = uint(cpu.xreg[13]);
        ret.g = uint(cpu.xreg[14]);
        ret.b = uint(cpu.xreg[15]);
        ret.a = uint(cpu.xreg[16]);
        return ret;
    case 33:
        ret.r = uint(cpu.xreg[17]);
        ret.g = uint(cpu.xreg[18]);
        ret.b = uint(cpu.xreg[19]);
        ret.a = uint(cpu.xreg[20]);
        return ret;
    case 34:
        ret.r = uint(cpu.xreg[21]);
        ret.g = uint(cpu.xreg[22]);
        ret.b = uint(cpu.xreg[23]);
        ret.a = uint(cpu.xreg[24]);
        return ret;
    case 35:
        ret.r = uint(cpu.xreg[25]);
        ret.g = uint(cpu.xreg[26]);
        ret.b = uint(cpu.xreg[27]);
        ret.a = uint(cpu.xreg[28]);
        return ret;
    case 36:
        ret.a = uint(cpu.pc);
        ret.r = uint(cpu.xreg[29]);
        ret.g = uint(cpu.xreg[30]);
        ret.b = uint(cpu.xreg[31]);
        return ret;
    case 37:
        ret.a = uint(cpu.memop_dst_v);
        ret.b = uint(cpu.memop_src_v);
        ret.g = uint(cpu.reservation_addr);
        ret.r = uint(cpu.reservation_en);
        return ret;
    case 38:
        ret.g = uint(cpu.memop_dst_p);
        ret.b = uint(cpu.memop_n);
        ret.r = uint(cpu.memop_src_p);
        ret.a = uint(cpu.stall_count);
        return ret;
    case 39:
        ret.g = uint(cpu.rtc0);
        ret.b = uint(cpu.rtc1);
        ret.a = asuint(cpu.start_time_ref);
        ret.r = uint(cpu.trap_count);
        return ret;
    case 4:
        ret.r = uint(cpu.cache.csr_cache_8_addr);
        ret.g = uint(cpu.cache.csr_cache_8_val);
        ret.b = uint(cpu.cache.csr_cache_9_addr);
        ret.a = uint(cpu.cache.csr_cache_9_val);
        return ret;
    case 40:
        ret.a = uint(cpu.debug_arb_0);
        ret.r = uint(cpu.debug_do_tick);
        ret.g = uint(cpu.debug_last_ins);
        ret.b = uint(cpu.stall == 0 ? cpu.debug_last_stall : cpu.stall);
        return ret;
    case 5:
        ret.r = uint(cpu.cache.csr_cache_10_addr);
        ret.g = uint(cpu.cache.csr_cache_10_val);
        ret.b = uint(cpu.cache.csr_cache_11_addr);
        ret.a = uint(cpu.cache.csr_cache_11_val);
        return ret;
    case 6:
        ret.r = uint(cpu.cache.csr_cache_12_addr);
        ret.g = uint(cpu.cache.csr_cache_12_val);
        ret.b = uint(cpu.cache.csr_cache_13_addr);
        ret.a = uint(cpu.cache.csr_cache_13_val);
        return ret;
    case 7:
        ret.r = uint(cpu.cache.csr_cache_14_addr);
        ret.g = uint(cpu.cache.csr_cache_14_val);
        ret.b = uint(cpu.cache.csr_cache_15_addr);
        ret.a = uint(cpu.cache.csr_cache_15_val);
        return ret;
    case 8:
        ret.r = uint(cpu.cache.ram_l1_last_addr);
        ret.g = uint(cpu.cache.ram_l1_last_val);
        ret.b = uint(cpu.csr.privilege);
        ret.a = uint(cpu.uart.rbr_thr_ier_iir);
        return ret;
    case 9:
        ret.a = uint(cpu.uart.input_tag);
        ret.b = uint(cpu.uart.interrupting);
        ret.r = uint(cpu.uart.lcr_mcr_lsr_scr);
        ret.g = uint(cpu.uart.thre_ip);
        return ret;

    }

    return ret;
}

uint tex_get_csr(uint addr) {
    uint lin = (addr >> 2) + CSR_STATE_AT;
    uint idx = addr & 0x3;
    return idx_uint4(STATE_TEX(uint2(lin % 64, lin / 64)), idx);
}
#endif

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
        return idx_uint4(asuint(_Data_ROM[ROM_ADDR(mtd_addr)]), mtd_idx);   // (the ROM: one texture, a texel a pixel)
    }
    return 0;
}

// `ret` is the texel as it is (with CORES > 1, a RAM texel as the cores before this one leave it)
// The texel at physical address `a` for a copy the pass left to the commit: RAM as the texture
// has it, or the ROM.
uint4 lazy_source(uint a) {
    [branch]
    if (a & 0x80000000) {
        return RAM_TEX(RAM_ADDR((a & 0x7fffffff) >> 4));
    }
    return uint4(mem_get_cached_or_tex_from_state_cache_or_mtd(a), mem_get_cached_or_tex_from_state_cache_or_mtd(a + 4),
                 mem_get_cached_or_tex_from_state_cache_or_mtd(a + 8), mem_get_cached_or_tex_from_state_cache_or_mtd(a + 12));
}

uint4 commit(uint2 pos, uint4 ret) {
    if (pos.y < 64 && pos.x >= 64) {
        return (uint4)0;
    }

    uint pos_id = pos.x | ((pos.y) << 16);

    // increment commits
    if (!_Init && pos_id == 28) {
        ++ret.b;
    }

    uint c_bloom = STATE_TEX_HART(uint2(41, 0), 0).r;
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

    uint s_lin = pos.x + pos.y * 64;

#ifndef CSR_FIELDS
    // CSR area
    if (s_lin >= CSR_STATE_AT && s_lin < CSR_STATE_AT + CSR_STATE_TEXELS) {
        uint addr = s_lin - CSR_STATE_AT;
        addr <<= 2;
        if      (cpu.cache.csr_cache_0_addr == addr + 0) { ret.x = cpu.cache.csr_cache_0_val; }
        else if (cpu.cache.csr_cache_0_addr == addr + 1) { ret.y = cpu.cache.csr_cache_0_val; }
        else if (cpu.cache.csr_cache_0_addr == addr + 2) { ret.z = cpu.cache.csr_cache_0_val; }
        else if (cpu.cache.csr_cache_0_addr == addr + 3) { ret.w = cpu.cache.csr_cache_0_val; }
        if      (cpu.cache.csr_cache_1_addr == addr + 0) { ret.x = cpu.cache.csr_cache_1_val; }
        else if (cpu.cache.csr_cache_1_addr == addr + 1) { ret.y = cpu.cache.csr_cache_1_val; }
        else if (cpu.cache.csr_cache_1_addr == addr + 2) { ret.z = cpu.cache.csr_cache_1_val; }
        else if (cpu.cache.csr_cache_1_addr == addr + 3) { ret.w = cpu.cache.csr_cache_1_val; }
        if      (cpu.cache.csr_cache_2_addr == addr + 0) { ret.x = cpu.cache.csr_cache_2_val; }
        else if (cpu.cache.csr_cache_2_addr == addr + 1) { ret.y = cpu.cache.csr_cache_2_val; }
        else if (cpu.cache.csr_cache_2_addr == addr + 2) { ret.z = cpu.cache.csr_cache_2_val; }
        else if (cpu.cache.csr_cache_2_addr == addr + 3) { ret.w = cpu.cache.csr_cache_2_val; }
        if      (cpu.cache.csr_cache_3_addr == addr + 0) { ret.x = cpu.cache.csr_cache_3_val; }
        else if (cpu.cache.csr_cache_3_addr == addr + 1) { ret.y = cpu.cache.csr_cache_3_val; }
        else if (cpu.cache.csr_cache_3_addr == addr + 2) { ret.z = cpu.cache.csr_cache_3_val; }
        else if (cpu.cache.csr_cache_3_addr == addr + 3) { ret.w = cpu.cache.csr_cache_3_val; }
        if      (cpu.cache.csr_cache_4_addr == addr + 0) { ret.x = cpu.cache.csr_cache_4_val; }
        else if (cpu.cache.csr_cache_4_addr == addr + 1) { ret.y = cpu.cache.csr_cache_4_val; }
        else if (cpu.cache.csr_cache_4_addr == addr + 2) { ret.z = cpu.cache.csr_cache_4_val; }
        else if (cpu.cache.csr_cache_4_addr == addr + 3) { ret.w = cpu.cache.csr_cache_4_val; }
        if      (cpu.cache.csr_cache_5_addr == addr + 0) { ret.x = cpu.cache.csr_cache_5_val; }
        else if (cpu.cache.csr_cache_5_addr == addr + 1) { ret.y = cpu.cache.csr_cache_5_val; }
        else if (cpu.cache.csr_cache_5_addr == addr + 2) { ret.z = cpu.cache.csr_cache_5_val; }
        else if (cpu.cache.csr_cache_5_addr == addr + 3) { ret.w = cpu.cache.csr_cache_5_val; }
        if      (cpu.cache.csr_cache_6_addr == addr + 0) { ret.x = cpu.cache.csr_cache_6_val; }
        else if (cpu.cache.csr_cache_6_addr == addr + 1) { ret.y = cpu.cache.csr_cache_6_val; }
        else if (cpu.cache.csr_cache_6_addr == addr + 2) { ret.z = cpu.cache.csr_cache_6_val; }
        else if (cpu.cache.csr_cache_6_addr == addr + 3) { ret.w = cpu.cache.csr_cache_6_val; }
        if      (cpu.cache.csr_cache_7_addr == addr + 0) { ret.x = cpu.cache.csr_cache_7_val; }
        else if (cpu.cache.csr_cache_7_addr == addr + 1) { ret.y = cpu.cache.csr_cache_7_val; }
        else if (cpu.cache.csr_cache_7_addr == addr + 2) { ret.z = cpu.cache.csr_cache_7_val; }
        else if (cpu.cache.csr_cache_7_addr == addr + 3) { ret.w = cpu.cache.csr_cache_7_val; }
        if      (cpu.cache.csr_cache_8_addr == addr + 0) { ret.x = cpu.cache.csr_cache_8_val; }
        else if (cpu.cache.csr_cache_8_addr == addr + 1) { ret.y = cpu.cache.csr_cache_8_val; }
        else if (cpu.cache.csr_cache_8_addr == addr + 2) { ret.z = cpu.cache.csr_cache_8_val; }
        else if (cpu.cache.csr_cache_8_addr == addr + 3) { ret.w = cpu.cache.csr_cache_8_val; }
        if      (cpu.cache.csr_cache_9_addr == addr + 0) { ret.x = cpu.cache.csr_cache_9_val; }
        else if (cpu.cache.csr_cache_9_addr == addr + 1) { ret.y = cpu.cache.csr_cache_9_val; }
        else if (cpu.cache.csr_cache_9_addr == addr + 2) { ret.z = cpu.cache.csr_cache_9_val; }
        else if (cpu.cache.csr_cache_9_addr == addr + 3) { ret.w = cpu.cache.csr_cache_9_val; }
        if      (cpu.cache.csr_cache_10_addr == addr + 0) { ret.x = cpu.cache.csr_cache_10_val; }
        else if (cpu.cache.csr_cache_10_addr == addr + 1) { ret.y = cpu.cache.csr_cache_10_val; }
        else if (cpu.cache.csr_cache_10_addr == addr + 2) { ret.z = cpu.cache.csr_cache_10_val; }
        else if (cpu.cache.csr_cache_10_addr == addr + 3) { ret.w = cpu.cache.csr_cache_10_val; }
        if      (cpu.cache.csr_cache_11_addr == addr + 0) { ret.x = cpu.cache.csr_cache_11_val; }
        else if (cpu.cache.csr_cache_11_addr == addr + 1) { ret.y = cpu.cache.csr_cache_11_val; }
        else if (cpu.cache.csr_cache_11_addr == addr + 2) { ret.z = cpu.cache.csr_cache_11_val; }
        else if (cpu.cache.csr_cache_11_addr == addr + 3) { ret.w = cpu.cache.csr_cache_11_val; }
        if      (cpu.cache.csr_cache_12_addr == addr + 0) { ret.x = cpu.cache.csr_cache_12_val; }
        else if (cpu.cache.csr_cache_12_addr == addr + 1) { ret.y = cpu.cache.csr_cache_12_val; }
        else if (cpu.cache.csr_cache_12_addr == addr + 2) { ret.z = cpu.cache.csr_cache_12_val; }
        else if (cpu.cache.csr_cache_12_addr == addr + 3) { ret.w = cpu.cache.csr_cache_12_val; }
        if      (cpu.cache.csr_cache_13_addr == addr + 0) { ret.x = cpu.cache.csr_cache_13_val; }
        else if (cpu.cache.csr_cache_13_addr == addr + 1) { ret.y = cpu.cache.csr_cache_13_val; }
        else if (cpu.cache.csr_cache_13_addr == addr + 2) { ret.z = cpu.cache.csr_cache_13_val; }
        else if (cpu.cache.csr_cache_13_addr == addr + 3) { ret.w = cpu.cache.csr_cache_13_val; }
        if      (cpu.cache.csr_cache_14_addr == addr + 0) { ret.x = cpu.cache.csr_cache_14_val; }
        else if (cpu.cache.csr_cache_14_addr == addr + 1) { ret.y = cpu.cache.csr_cache_14_val; }
        else if (cpu.cache.csr_cache_14_addr == addr + 2) { ret.z = cpu.cache.csr_cache_14_val; }
        else if (cpu.cache.csr_cache_14_addr == addr + 3) { ret.w = cpu.cache.csr_cache_14_val; }
        if      (cpu.cache.csr_cache_15_addr == addr + 0) { ret.x = cpu.cache.csr_cache_15_val; }
        else if (cpu.cache.csr_cache_15_addr == addr + 1) { ret.y = cpu.cache.csr_cache_15_val; }
        else if (cpu.cache.csr_cache_15_addr == addr + 2) { ret.z = cpu.cache.csr_cache_15_val; }
        else if (cpu.cache.csr_cache_15_addr == addr + 3) { ret.w = cpu.cache.csr_cache_15_val; }

    }
#endif

    // RAM
    if (pos.y >= RAM_ROW) {
        uint2 pos_ram = uint2(pos.x, pos.y - RAM_ROW);
        uint lin = RAM_LIN(pos_ram.x, pos_ram.y);

        if (_Init) {
            // init main memory from texture
            uint2 ram_dim;
            _Data_RAM_A.GetDimensions(ram_dim.x, ram_dim.y);
            uint2 pos2 = uint2(lin % 2048, lin / 2048);  // the payload image is row-major
            // something feels off here... off by one, you might say...
            // ...but this way it actually works
            pos2.y = ram_dim.y - pos2.y - 1;

            if (pos2.y >= ram_dim.y) {
                // zero unset memory
                ret = uint4(0, 0, 0, 0);
            } else {
                uint r = unpack_raw_float4(_Data_RAM_R[pos2]);
                uint g = unpack_raw_float4(_Data_RAM_G[pos2]);
                uint b = unpack_raw_float4(_Data_RAM_B[pos2]);
                uint a = unpack_raw_float4(_Data_RAM_A[pos2]);
                ret = uint4(r, g, b, a);
            }
        } else {
            lin <<= 4;

            // A fill or copy the pass left to us (LAZY_MEMOP: kind in the low bits, texels above;
            // whole texels, its source as the texture has it): made first, so that what the
            // pass stored to its destination afterwards, which the cache has, lies on top.
            uint lazy = cpu.debug_arb_0, lazy2 = cpu.stall_count;
            [branch]
            if (lazy != 0 && lin >= cpu.trap_count && ((lin - cpu.trap_count) >> 4) < (lazy >> 2)) {
                if ((lazy & 3) == 2) {
                    ret = uint4(cpu.debug_last_ins, cpu.debug_last_ins, cpu.debug_last_ins, cpu.debug_last_ins);
                } else {
                    ret = lazy_source(cpu.debug_last_ins + (lin - cpu.trap_count));
                }
            }
            [branch]
            if (lazy2 != 0 && lin >= cpu.memop_dst_p && ((lin - cpu.memop_dst_p) >> 4) < (lazy2 >> 2)) {
                if ((lazy2 & 3) == 2) {
                    ret = uint4(cpu.memop_src_p, cpu.memop_src_p, cpu.memop_src_p, cpu.memop_src_p);
                } else {
                    ret = lazy_source(cpu.memop_src_p + (lin - cpu.memop_dst_p));
                }
            }

            if ((cpu.stall == STALL_MEMOP_COPY || cpu.stall == STALL_MEMOP_FILL) &&
                (lin + (3 << 2)) >= cpu.memop_dst_p &&
                lin < (cpu.memop_dst_p + cpu.memop_n))
            {
                // we've found ourselves in a position for a memcpy (or a fill), ignore cache and perform it
                // (a memop is always the last instruction in a commit cycle, so we know we have
                // higher priority than the L1 cache)
                [loop]
                for (uint icp = 0; icp < 4; icp++) {
                    uint lin_word = lin + (icp << 2);
                    if (lin_word >= cpu.memop_dst_p &&
                        lin_word < (cpu.memop_dst_p + cpu.memop_n))
                    {
                        uint offset = lin_word - cpu.memop_dst_p;
                        uint src = cpu.memop_src_p + offset;
                        uint transfer = cpu.memop_src_v;
                        [branch]
                        if (cpu.stall == STALL_MEMOP_COPY) transfer = mem_get_cached_or_tex_from_state_cache_or_mtd(src);
                        set_idx_uint4(ret, transfer, icp);
                    }
                }
            } else {
                // write back L1 cache
                [branch]
                if ((lin & c_bloom) == lin) {
                    uint4 cached;
                    if (l1_state_find(lin >> 4, cached)) ret = cached;
                }

                     if (cpu.cache.ram_l1_last_addr == lin           ) { ret.r = cpu.cache.ram_l1_last_val; }
                else if (cpu.cache.ram_l1_last_addr == lin + (1 << 2)) { ret.g = cpu.cache.ram_l1_last_val; }
                else if (cpu.cache.ram_l1_last_addr == lin + (2 << 2)) { ret.b = cpu.cache.ram_l1_last_val; }
                else if (cpu.cache.ram_l1_last_addr == lin + (3 << 2)) { ret.a = cpu.cache.ram_l1_last_val; }
            }
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

/*

C# layout:

    #region LOAD
    private const int STATE_WIDTH = 64;
    private const int STATE_HEIGHT = 64;
    private const int TOTAL_WIDTH = 2048;
    private const int TOTAL_HEIGHT = 4096;
    private int[] addr_lin(int lin) {
        int[] ret = new int[3];
        ret[2] = lin & 0x3; // c
        lin = lin >> 2;
        ret[0] = lin % 64; // x
        ret[1] = lin / 64; // y
        return ret;
    }
    private uint load_csr__privilege() { return decodePackedData(8, 0, 2); }
    private uint load_uart__rbr_thr_ier_iir() { return decodePackedData(8, 0, 3); }
    private uint load_uart__lcr_mcr_lsr_scr() { return decodePackedData(9, 0, 0); }
    private uint load_uart__thre_ip() { return decodePackedData(9, 0, 1); }
    private uint load_uart__interrupting() { return decodePackedData(9, 0, 2); }
    private uint load_uart__input_tag() { return decodePackedData(9, 0, 3); }
    private uint load_clint__msip() { return decodePackedData(10, 0, 0); }
    private uint load_clint__mtimecmp_lo() { return decodePackedData(10, 0, 1); }
    private uint load_clint__mtimecmp_hi() { return decodePackedData(10, 0, 2); }
    private uint load_clint__mtime_lo() { return decodePackedData(10, 0, 3); }
    private uint load_clint__mtime_hi() { return decodePackedData(11, 0, 0); }
    private uint load_mmu__mode() { return decodePackedData(11, 0, 1); }
    private uint load_mmu__ppn() { return decodePackedData(11, 0, 2); }
    private uint load_stall() { return decodePackedData(28, 0, 0); }
    private uint load_clock() { return decodePackedData(28, 0, 1); }
    private uint load_commits() { return decodePackedData(28, 0, 2); }
    private uint load_xreg0() { return decodePackedData(28, 0, 3); }
    private uint load_xreg1() { return decodePackedData(29, 0, 0); }
    private uint load_xreg2() { return decodePackedData(29, 0, 1); }
    private uint load_xreg3() { return decodePackedData(29, 0, 2); }
    private uint load_xreg4() { return decodePackedData(29, 0, 3); }
    private uint load_xreg5() { return decodePackedData(30, 0, 0); }
    private uint load_xreg6() { return decodePackedData(30, 0, 1); }
    private uint load_xreg7() { return decodePackedData(30, 0, 2); }
    private uint load_xreg8() { return decodePackedData(30, 0, 3); }
    private uint load_xreg9() { return decodePackedData(31, 0, 0); }
    private uint load_xreg10() { return decodePackedData(31, 0, 1); }
    private uint load_xreg11() { return decodePackedData(31, 0, 2); }
    private uint load_xreg12() { return decodePackedData(31, 0, 3); }
    private uint load_xreg13() { return decodePackedData(32, 0, 0); }
    private uint load_xreg14() { return decodePackedData(32, 0, 1); }
    private uint load_xreg15() { return decodePackedData(32, 0, 2); }
    private uint load_xreg16() { return decodePackedData(32, 0, 3); }
    private uint load_xreg17() { return decodePackedData(33, 0, 0); }
    private uint load_xreg18() { return decodePackedData(33, 0, 1); }
    private uint load_xreg19() { return decodePackedData(33, 0, 2); }
    private uint load_xreg20() { return decodePackedData(33, 0, 3); }
    private uint load_xreg21() { return decodePackedData(34, 0, 0); }
    private uint load_xreg22() { return decodePackedData(34, 0, 1); }
    private uint load_xreg23() { return decodePackedData(34, 0, 2); }
    private uint load_xreg24() { return decodePackedData(34, 0, 3); }
    private uint load_xreg25() { return decodePackedData(35, 0, 0); }
    private uint load_xreg26() { return decodePackedData(35, 0, 1); }
    private uint load_xreg27() { return decodePackedData(35, 0, 2); }
    private uint load_xreg28() { return decodePackedData(35, 0, 3); }
    private uint load_xreg29() { return decodePackedData(36, 0, 0); }
    private uint load_xreg30() { return decodePackedData(36, 0, 1); }
    private uint load_xreg31() { return decodePackedData(36, 0, 2); }
    private uint load_pc() { return decodePackedData(36, 0, 3); }
    private uint load_reservation_en() { return decodePackedData(37, 0, 0); }
    private uint load_reservation_addr() { return decodePackedData(37, 0, 1); }
    private uint load_memop_src_v() { return decodePackedData(37, 0, 2); }
    private uint load_memop_dst_v() { return decodePackedData(37, 0, 3); }
    private uint load_memop_src_p() { return decodePackedData(38, 0, 0); }
    private uint load_memop_dst_p() { return decodePackedData(38, 0, 1); }
    private uint load_memop_n() { return decodePackedData(38, 0, 2); }
    private uint load_stall_count() { return decodePackedData(38, 0, 3); }
    private uint load_trap_count() { return decodePackedData(39, 0, 0); }
    private uint load_rtc0() { return decodePackedData(39, 0, 1); }
    private uint load_rtc1() { return decodePackedData(39, 0, 2); }
    private uint load_start_time_ref() { return decodePackedData(39, 0, 3); }
    private uint load_debug_do_tick() { return decodePackedData(40, 0, 0); }
    private uint load_debug_last_ins() { return decodePackedData(40, 0, 1); }
    private uint load_debug_last_stall() { return decodePackedData(40, 0, 2); }
    private uint load_debug_arb_0() { return decodePackedData(40, 0, 3); }
    private uint load_csr(int addr) {
        int lin = (addr >> 2) + CSR_STATE_AT;   // 1024 with the default write cache
        return decodePackedData(lin % 64, lin / 64, addr & 0x3);
    }
    private uint load_l1_cache(int addr) {
        int lin = (addr >> 2) + 44;
        return decodePackedData(lin % 64, lin / 64, addr & 0x3);
    }
    #endregion

*/
