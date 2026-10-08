// What ShareEncode.shader and ShareDecode.shader agree on (docs/share.md): the rows of bytes,
// the kinds a quarter comes in, and the transform's tables. tools/share_reference.py is the
// same in numpy.
#ifndef SHARE_CODEC_INCLUDED
#define SHARE_CODEC_INCLUDED

// The unit is a cell of 8x8 pixels; a 32x32 quarter has 16 of them, and there are 40 x 24
// quarters. A row of 1024 bytes each: the quarters' rows, then each quarter's cells' row.
static const int QuarterCols = 40, Quarters = 960;
// A quarter's row: 8 bytes of flags, its coarse picture in 4 bytes (two colours of the
// palette, a bit a cell saying which), then its exact bytes.
static const uint AtCoarse = 8, AtExact = 16;
// The palette: 256 colours of 5:6:5, four bytes an entry (two used), in a row of the
// encoder's target and in one of the receiver's store.
static const uint PaletteRow = 1927, StorePaletteRow = 1920;
// Levels (share_reference.py: MOVE, SEED_AT): an entry moves to its cells' mean when that
// is further; one without cells takes a cell that is further than this from its entry.
static const float PaletteMove = 4.0, PaletteSeed = 16.0;
// A cells' row: 64 bytes a cell: its length, a bit a channel that has more than a mean, the
// means of Y, Cb, Cr, then a block for each such channel.
static const uint Slot = 64;
// A quarter sent as a bitmap; 0: as cells. With 128 in the encoder's kind byte its colours
// were told apart at 4 bits a channel only: its shape is exact, its colours are not, and
// its cells may follow sharp.
static const uint CodeF = 3, CodeT = 4, CodeB = 5;
// Coefficients (zigzag order) a cell's blocks keep: of colour only the mean. Sharp detail
// matters here, colour and gradients less.
static const uint KeepY = 50, KeepC = 1;

// coefficient k of a block in zigzag order is at row * 8 + column, and back
static const uint Zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
static const uint Unzig[64] = {
    0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42, 3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63
};
// The quantiser's steps, by row * 8 + column, in JPEG's units. For brightness one step for
// every coefficient: JPEG's table gives fine detail the coarsest steps, and fine detail is
// what is wanted here. (Colour keeps JPEG's table, of which only the mean's entry is used.)
static const float QuantLuma[64] = {
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24
};
static const float QuantChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

// The step of the coefficient at row * 8 + column, in the transform's units (a mean: 1 a level).
float quant_step(uint at, bool chroma) {
    if (at == 0) return 1.0;
    float scale = ((at & 7) == 0 ? 0.70710678 : 1.0) * ((at >> 3) == 0 ? 0.70710678 : 1.0);
    float q = chroma ? QuantChroma[at] : QuantLuma[at];
    return q / (16.0 * scale);
}

float dct_cos(uint k, uint x) {
    return cos((2.0 * (float)x + 1.0) * (float)k * 0.19634954);
}

// What a count of steps of the coefficient at row * 8 + column adds at sample p of its block.
float dct_term(uint at, uint2 p, bool chroma) {
    uint u = at & 7, v = at >> 3;
    return quant_step(at, chroma) * (u == 0 ? 1.0 : 2.0) * (v == 0 ? 1.0 : 2.0) * dct_cos(u, p.x) * dct_cos(v, p.y);
}

// colours here are 0..255
float3 to_ycc(float3 c) {
    float y = dot(c, float3(0.299, 0.587, 0.114));
    return float3(y, 128.0 + 0.564 * (c.b - y), 128.0 + 0.713 * (c.r - y));
}

float3 from_ycc(float3 v) {
    float r = v.x + 1.402 * (v.z - 128.0);
    float b = v.x + 1.772 * (v.y - 128.0);
    float g = (v.x - 0.299 * r - 0.114 * b) / 0.587;
    return clamp(float3(r, g, b), 0.0, 255.0);
}

// and 0..1 here
uint pack565(float3 c) {
    uint3 v = (uint3)(saturate(c) * float3(31, 63, 31) + 0.5);
    return v.r << 11 | v.g << 5 | v.b;
}

// The same, of the colour's nearest at 4 bits a channel: colours that close count as one.
uint pack444(float3 c) {
    uint3 v = (uint3)(saturate(c) * 15.0 + 0.5);
    return (v.r << 1 | v.r >> 3) << 11 | (v.g << 2 | v.g >> 2) << 5 | (v.b << 1 | v.b >> 3);
}

uint pack_colour(float3 c, bool coarsely) {
    return coarsely ? pack444(c) : pack565(c);
}

float3 unpack565(uint v) {
    return float3((float)(v >> 11), (float)((v >> 5) & 63), (float)(v & 31)) / float3(31, 63, 31);
}

uint bits_set(uint v) {
    uint n = 0;
    for (uint i = 0; i < 8; i++) n += (v >> i) & 1;
    return n;
}

// 0..255 as 4 bits
uint nibble_of(float v) {
    return (uint)clamp(floor(v / 17.0 + 0.5), 0.0, 15.0);
}

#endif
