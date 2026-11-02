#ifndef FB_H
#define FB_H


// RAM_ADDR / RAM_LIN: see types.h
#ifndef STATE_TEX_HEIGHT
#define STATE_TEX_HEIGHT 9280   // the state texture's rows: 64 of state, then RAM to 0x92000000
#endif
#define RAM_MAX (2048 * (STATE_TEX_HEIGHT - 64) * 4 * 4)


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


#define BUFFER_MAX 63


#define NO_CHAR 0x80000000
struct fbupd {
    uint2 pos;
    uint upshift;
    uint char;
};
static fbupd fblog[BUFFER_MAX];



#endif