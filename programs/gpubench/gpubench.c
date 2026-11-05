// The GPU's fragment cost: LAYERS rectangles over the whole 1280 x 720 picture, in one fragment mode, drawn
// every frame. Built once a mode (-DMODE=N, programs\build.bat makes gpubench0.bin ...): run each with
// RVC_PASS_PROF=1 and read "gpu draw" (docs/machine/gpu.md, "Cost").
//   0 colour   1 words, nearest   2 words, smooth   3 indexed, nearest   4 indexed, smooth
//   5 three bytes, smooth   6 compact quads, words smooth, times a light map (Quake's walls)
//   7 as 2 and 8 as 6, the texture and the light map uploaded into the VRAM first (trilinear, at most 16 times smaller)
//   9 the vertices' cost: 3,600 rectangles of 16 x 16 tiling the picture, a command each, in passes 0, 1 and 2 by turns
//   10 a copy (gpu_copy): 64 rectangles of 32 x 32 in the top left 256 x 256, moving a pixel a frame, copied into a slot at
//      the list's end and drawn from there over the rest of the picture, four times as large (the frame before's copy)
//   11 the kinds an upload converts: a mask, coverage, a layer of tiles (keyed), a film's frame and a glyph's distance fields,
//      each 64 x 64 drawn twice as large from RAM on the left and from its slot on the right: the halves must match
//   12 sprite records of every kind (CMD_SPRITES) without a palette, with one, and the shades: what the device's streams
//      make of them, to compare with a capture under --define GPU_NO_STREAMS

#include "../common/gpu.h"

#ifndef MODE
#define MODE 1
#endif
#define W 1280
#define H 720
#define LAYERS 8
#define TEX 256
#define ONE 65536

static uint32_t words[TEX * TEX] GPU_ALIGNED;
static uint8_t bytes[TEX * TEX * 3] GPU_ALIGNED;
static uint8_t light[64 * 64 * 3] GPU_ALIGNED;
#define VRAM (MODE == 7 || MODE == 8)
#define BASE (MODE == 7 ? 2 : MODE == 8 ? 6 : MODE)
#define WORDS_SLOT GPU_VRAM_SLOT(16, 16, 4)
#define LIGHT_SLOT (GPU_VRAM_SLOT(400, 1, 0) | GPU_VRAM_CLAMPED)
#if BASE == 6
static gpu_compact_vertex corners[LAYERS * 4];
static uint32_t light_at[LAYERS * 4] GPU_ALIGNED;
#endif

int main(void) {
    DISP_WIDTH = W;
    DISP_HEIGHT = H;
    DISP_MODE = DISP_MODE_GPU;
    for (uint32_t i = 0; i < TEX * TEX; i++) {
        uint32_t x = i % TEX, y = i / TEX;
        words[i] = ((x * 7) & 0xff) << 16 | ((y * 5) & 0xff) << 8 | ((x ^ y) & 0xff);
        bytes[3 * i] = (uint8_t)(x * 3);
        bytes[3 * i + 1] = (uint8_t)(y * 3);
        bytes[3 * i + 2] = (uint8_t)(x ^ y);
    }
    for (uint32_t i = 0; i < 64 * 64 * 3; i++) light[i] = (uint8_t)(128 + (i & 127));

#if VRAM
    gpu_begin();
    gpu_upload(words, GPU_FRAGMENT_TEXTURE, TEX, TEX, WORDS_SLOT, 0, 0, 0);
    gpu_upload(light, GPU_FRAGMENT_RGB24, 64, 64, LIGHT_SLOT, 0, 0, 0);
    gpu_submit();
#endif
    gpu_begin();
    gpu_clear(0x00203040);
#if MODE == 10
    {
        const uint32_t slot = GPU_VRAM_SLOT(16, 16, 0);
        for (uint32_t frame = 0;; frame++) {
            gpu_begin();
            gpu_clear(0x00102030);
            for (int n = 0; n < 64; n++) {
                int x = (n % 8) * 32 + (int)(frame % 32), y = (n / 8) * 32;
                gpu_rect(x, y, x + 24, y + 24, (uint32_t)(n * 2654435761u >> 8) & 0x00ffffff);
            }
            volatile uint32_t* c = gpu_command(GPU_RECT, 6);   // the copy of the frame before, four times as large
            c[1] = 0x00ffffff;
            c[4] = 272, c[5] = 0, c[6] = 272 + 1024, c[7] = 1024 > H ? H : 1024;
            c[8] = GPU_FRAGMENT_TEXTURE;
            c[9] = slot, c[10] = 256, c[11] = 256;
            c[12] = 0, c[13] = 0, c[14] = ONE, c[15] = (1024 > H ? H : 1024) * ONE / 1024;
            gpu_copy(0, 0, 256, 256, slot, 0, 0, 0);
            gpu_submit();
        }
    }
#endif
#if MODE == 11
    {
        // (the kinds' numbers in the device: docs/machine/gpu.md)
        enum { MASK = 3, TILES = 5, YUV = 6, COVER = 7, MSDF = 8 };
        static uint8_t mask[8 * 64] GPU_ALIGNED, cover[32 * 64] GPU_ALIGNED, film[64 * 64 + 2 * 32 * 32] GPU_ALIGNED;
        static uint8_t fields[3 * 64 * 64] GPU_ALIGNED, tiles[4 * 8 * 8] GPU_ALIGNED;
        static uint16_t cells[8 * 8] GPU_ALIGNED;
        static uint32_t palette[32] GPU_ALIGNED, layer[4] GPU_ALIGNED;
        for (int i = 0; i < 8 * 64; i++) mask[i] = (uint8_t)(i * 37 ^ (i >> 3) * 11);
        for (int i = 0; i < 32 * 64; i++) cover[i] = (uint8_t)(i * 29 + (i >> 5) * 7);
        for (int i = 0; i < 64 * 64; i++) film[i] = (uint8_t)(16 + ((i % 64) * 3 + (i / 64) * 2) % 220);
        for (int i = 0; i < 32 * 32; i++) film[64 * 64 + i] = (uint8_t)(64 + (i % 32) * 4), film[64 * 64 + 1024 + i] = (uint8_t)(200 - (i / 32) * 4);
        for (int i = 0; i < 64 * 64; i++) {
            int x = i % 64, y = i / 64, d = 128 + (24 - (x - 32 < 0 ? 32 - x : x - 32) - (y - 32 < 0 ? 32 - y : y - 32)) * 6;
            uint8_t v = (uint8_t)(d < 0 ? 0 : d > 255 ? 255 : d);
            fields[3 * i] = v, fields[3 * i + 1] = (uint8_t)(v + (x & 7)), fields[3 * i + 2] = (uint8_t)(v - (y & 7));
        }
        for (int i = 0; i < 4 * 64; i++) tiles[i] = (uint8_t)((i * 5 + (i >> 6)) % 16);
        for (int i = 0; i < 64; i++) cells[i] = (uint16_t)((i % 4) | (i & 4 ? 0x400 : 0) | (i & 8 ? 0x800 : 0) | (i % 2) << 12);
        for (int i = 0; i < 32; i++) palette[i] = (uint32_t)(i * 0x00070b0d) & 0x00ffffff;
        layer[0] = (uint32_t)cells, layer[1] = (uint32_t)tiles, layer[2] = (uint32_t)palette, layer[3] = 8 | 8 << 8 | 8 << 16;
        static const uint32_t kinds[5] = {MASK, COVER, TILES | GPU_FRAGMENT_KEYED, YUV, MSDF};
        const void* sources[5] = {mask, cover, layer, film, fields};
        gpu_begin();
        for (int k = 0; k < 5; k++)
            gpu_upload(sources[k], kinds[k], 64, 64, GPU_VRAM_SLOT(16 + 96 * k, 16, 0), 0, 0, 0);
        gpu_submit();
        gpu_begin();
        gpu_clear(0x00203040);
        for (int k = 0; k < 5; k++)
            for (int side = 0; side < 2; side++) {
                volatile uint32_t* c = gpu_command(GPU_RECT, 6);
                c[1] = kinds[k] == MASK || kinds[k] == COVER || kinds[k] == MSDF ? 0x00f0c040 : 0x00ffffff;
                c[2] = kinds[k] == MSDF ? 0x3f000000 : 0;   // the key (MSDF: its range, 0.5 a screen pixel)
                c[4] = 64 + 512 * side, c[5] = 8 + 140 * k, c[6] = c[4] + 128, c[7] = c[5] + 128;
                c[8] = kinds[k] | (kinds[k] == COVER || kinds[k] == MSDF ? GPU_PASS(GPU_PASS_BLEND) : 0);
                c[9] = side ? GPU_VRAM_SLOT(16 + 96 * k, 16, 0) : (uint32_t)sources[k];
                c[10] = 64, c[11] = 64;
                c[12] = 0, c[13] = 0, c[14] = ONE, c[15] = ONE;
            }
        for (;;) gpu_submit();
    }
#endif
#if MODE == 12
    {
        enum { COPY, REMAP, BLACK, SHADE, FILL, FILL_SHADE, CHECKER, WORDS, RAW };
        static uint8_t sprite[64 * 64] GPU_ALIGNED, table[256] GPU_ALIGNED, ta[256] GPU_ALIGNED, tb[256] GPU_ALIGNED;
        static uint32_t picture[64 * 64] GPU_ALIGNED, palette[256] GPU_ALIGNED, recs[4][9 * 4] GPU_ALIGNED;
        static uint32_t big[640 * 400] GPU_ALIGNED;   // (as OpenTTD's picture: a words record of rows of 2,560 bytes)
        for (int i = 0; i < 640 * 400; i++) big[i] = (uint32_t)(((i % 640) + (i / 640) * 3) & 0xff) << 16;
        for (int i = 0; i < 64 * 64; i++) {
            int x = i % 64, y = i / 64;
            sprite[i] = (uint8_t)(((x - 32) * (x - 32) + (y - 32) * (y - 32) < 900) ? 1 + (x * 3 + y) % 250 : 0);
            picture[i] = (uint32_t)((x * 4) & 0xff) << 16 | (uint32_t)(i % 3 == 0 ? 0x11 : i % 3 == 1 ? 0x02 : 0) << 8;
        }
        for (int i = 0; i < 256; i++) {
            table[i] = (uint8_t)(i % 7 == 0 ? 0 : 255 - i), ta[i] = (uint8_t)(i + 40), tb[i] = (uint8_t)(i ^ 0x5a);
            palette[i] = (uint32_t)(i * 0x00010307u) & 0x00ffffff;
        }
        static const int kinds[3][9] = {{COPY, REMAP, BLACK, RAW, WORDS, FILL, CHECKER, -1, -1}, {COPY, REMAP, BLACK, RAW, WORDS, FILL, CHECKER, -1, -1},
                                        {SHADE, FILL_SHADE, -1, -1, -1, -1, -1, -1, -1}};
        gpu_begin();
        gpu_clear(0x00203040);
        uint32_t order = 1;
        for (int row = 0; row < 3; row++) {
            uint32_t n = 0;
            for (int k = 0; k < 9 && kinds[row][k] >= 0; k++, n++) {
                int kind = kinds[row][k], x = 8 + 72 * k, y = 8 + 80 * row;
                uint32_t stride = kind == WORDS ? 64 * 4 : 64, *r = &recs[row][4 * n];
                r[0] = kind == WORDS ? (uint32_t)picture : kind == FILL || kind == CHECKER ? (uint32_t)(40 + 20 * k) | (kind == CHECKER ? 0x100u : 0) : (uint32_t)sprite;
                r[1] = kind == REMAP ? (uint32_t)table : kind == SHADE || kind == FILL_SHADE ? 0x30u : 0;
                r[2] = (uint32_t)x | (uint32_t)y << 11 | (stride & 1023) << 22;
                r[3] = 64u | 64u << 11 | (uint32_t)kind << 22 | (stride >> 10) << 26;
            }
            volatile uint32_t* c = gpu_command(GPU_SPRITES, 6 * n);
            c[1] = (uint32_t)recs[row];
            c[2] = n;
            c[4] = row == 2 ? 4u : 1u;
            c[5] = order;
            c[6] = row == 1 ? (uint32_t)palette : 0;
            c[7] = (uint32_t)ta;
            c[8] = (uint32_t)tb;
            order += n;
            if (row == 2) gpu_passes |= 4;
        }
        {
            uint32_t* r = recs[3];
            r[0] = (uint32_t)big, r[1] = 0, r[2] = 0u | 260u << 11 | (2560u & 1023) << 22, r[3] = 640u | 400u << 11 | (uint32_t)WORDS << 22 | (2560u >> 10) << 26;
            volatile uint32_t* c = gpu_command(GPU_SPRITES, 6);
            c[1] = (uint32_t)r, c[2] = 1, c[4] = 1, c[5] = order, c[6] = (uint32_t)palette, c[7] = (uint32_t)ta, c[8] = (uint32_t)tb;
        }
        for (;;) gpu_submit();
    }
#endif
#if MODE == 9
    for (int n = 0; n < (W / 16) * (H / 16); n++) {
        int x = (n % (W / 16)) * 16, y = (n / (W / 16)) * 16;
        gpu_rect_in(x, y, x + 16, y + 16, (uint32_t)(0x80000000u | (n * 2654435761u >> 8)), (uint32_t)(n % 3));
    }
    for (;;) gpu_submit();
#endif
    for (int l = 0; l < LAYERS; l++) {
#if BASE == 6
        static const int at[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; k++) {
            gpu_compact_vertex* v = &corners[l * 4 + k];
            v->x = at[k][0] * W * ONE;
            v->y = at[k][1] * H * ONE;
            v->z = 0;
            v->uv = (uint32_t)(at[k][0] * 5 * 1024 + l * 40) | (uint32_t)(at[k][1] * 3 * 1024) << 16;
            light_at[l * 4 + k] = (uint32_t)(at[k][0] * 65535) | (uint32_t)(at[k][1] * 65535) << 16;
        }
        gpu_draw((const gpu_vertex*)&corners[l * 4], 6, GPU_VERTEX_SCREEN | GPU_VERTEX_QUADS | GPU_VERTEX_COMPACT,
                 GPU_FRAGMENT_TEXTURE | 0x400 /* SMOOTH */ | 0x800 /* LIT */, 0, VRAM ? (const void*)WORDS_SLOT : words, TEX, TEX, 0);
        gpu_next[-16 + 11] = 0x00ffffff;
        gpu_next[-16 + 12] = (uint32_t)light_at + 16 * l;
        gpu_next[-16 + 13] = VRAM ? LIGHT_SLOT : (uint32_t)light;
        gpu_next[-16 + 14] = 64 | 64 << 16;
#else
        static const uint32_t modes[6] = {GPU_FRAGMENT_COLOUR, GPU_FRAGMENT_TEXTURE, GPU_FRAGMENT_TEXTURE | 0x400,
                                          GPU_FRAGMENT_INDEXED, GPU_FRAGMENT_INDEXED | 0x400, GPU_FRAGMENT_RGB24 | 0x400};
        volatile uint32_t* c = gpu_command(GPU_RECT, 6);
        c[1] = 0x00ffffff - l * 0x00080808;
        c[4] = 0;
        c[5] = 0;
        c[6] = W;
        c[7] = H;
        c[8] = modes[BASE];
        c[9] = VRAM ? WORDS_SLOT : BASE == 5 ? (uint32_t)bytes : (uint32_t)words;
        c[10] = TEX;
        c[11] = TEX;
        c[12] = l * 4096;
        c[13] = 0;
        c[14] = 5 * ONE;   // about 1.6 texels a pixel across: a minified texture, as in a game's distance
        c[15] = 3 * ONE;
#endif
    }
    for (;;) gpu_submit();
}
