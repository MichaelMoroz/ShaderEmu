// The GPU's fragment cost: LAYERS rectangles over the whole 1280 x 720 picture, in one fragment mode, drawn
// every frame. Built once a mode (-DMODE=N, programs\build.bat makes gpubench0.bin ...): run each with
// RVC_PASS_PROF=1 and read "gpu draw" (docs/machine/gpu.md, "Cost").
//   0 colour   1 words, nearest   2 words, smooth   3 indexed, nearest   4 indexed, smooth
//   5 three bytes, smooth   6 compact quads, words smooth, times a light map (Quake's walls)
//   7 as 2 and 8 as 6, the texture and the light map uploaded into the VRAM first (trilinear, at most 16 times smaller)
//   9 the vertices' cost: 3,600 rectangles of 16 x 16 tiling the picture, a command each, in passes 0, 1 and 2 by turns
//   10 a copy (gpu_copy): 64 rectangles of 32 x 32 in the top left 256 x 256, moving a pixel a frame, copied into a slot at
//      the list's end and drawn from there over the rest of the picture, four times as large (the frame before's copy)

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
