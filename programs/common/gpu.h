// The GPU device (docs/gpu.md): real triangles, rasterised by the graphics card into the GPU's
// own colour and depth target. A program builds a list of commands in RAM and submits it.
// Depth test and blending are fixed; per draw the program picks how vertices are projected and
// how fragments are coloured. Select DISP_MODE_GPU to show the result.
// All numbers handed to the GPU are 16.16 fixed point.
#pragma once

#include "platform.h"

#define GPU_SUBMIT (*(volatile uint32_t*)0x87000010)   // what to do with the list (below); the GPU clears it
#define GPU_SUBMIT_DRAW 1        // draw it
#define GPU_SUBMIT_WRITEBACK 2   // then copy the picture into the RAM framebuffer (DISP_PIXELS)
#define GPU_SUBMIT_INTO 4        // then copy it into the rectangle at GPU_INTO, whose size the picture has
#define GPU_INTO ((volatile uint32_t*)0x87000050)   // address, width, height, row length in pixels
#define GPU_LIST   (*(volatile uint32_t*)0x87000014)   // address of the command list
#define GPU_COUNT  (*(volatile uint32_t*)0x87000018)   // number of commands in it
#define GPU_FRAMES (*(volatile uint32_t*)0x8700001c)   // lists drawn so far

#define GPU_COMMANDS 0x87500000u   // where gpu_begin() builds the list: 4096 commands of 64 bytes
#define GPU_MAX_COMMANDS 4096
#define GPU_MAX_VERTICES 196608    // per list, over all commands (65,536 triangles)

enum { GPU_END, GPU_CLEAR, GPU_RECT, GPU_DRAW };

// How a draw's vertices are projected.
enum {
    GPU_VERTEX_SCREEN,   // position is pixels (x, y) and depth (z, 0 near .. 1 far)
    GPU_VERTEX_CLIP,     // uniforms 0-3: clip matrix rows (OpenGL conventions)
    GPU_VERTEX_LIT,      // also 4-6: normal matrix rows, 7: light direction, 8: diffuse, 9: ambient colour
};

// How its fragments are coloured.
enum {
    GPU_FRAGMENT_COLOUR,          // the vertex colour
    GPU_FRAGMENT_TEXTURE,         // a texture of 0x00RRGGBB words, times the colour
    GPU_FRAGMENT_INDEXED,         // a texture of bytes through the display palette, times the colour
    GPU_FRAGMENT_MASK,            // a 1-bit texture (rows of whole bytes, leftmost bit highest): set bits take the colour
    GPU_FRAGMENT_KEYED = 0x100,   // add: texels equal to the key are not drawn
};

// The GPU reads vertices and uniform vectors a whole 16-byte RAM texel at a time, so both must
// sit on 16-byte boundaries. Textures may be anywhere.
#define GPU_ALIGNED __attribute__((aligned(16)))

typedef struct {
    int32_t position[4];   // w = 1 for points
    int32_t normal[4];
    int32_t uv[4];         // 0..1 across the texture, repeating
    int32_t colour[4];     // 0..1 red, green, blue (not used by GPU_VERTEX_LIT)
} GPU_ALIGNED gpu_vertex;

static volatile uint32_t* gpu_next;
static uint32_t gpu_commands, gpu_vertices;

static inline void gpu_begin(void) {
    gpu_next = (volatile uint32_t*)GPU_COMMANDS;
    gpu_commands = 0;
    gpu_vertices = 0;
}

// Commands are 16 words; word 3 is where the command's vertices sit in the GPU's mesh.
static inline volatile uint32_t* gpu_command(uint32_t op, uint32_t vertices) {
    volatile uint32_t* c = gpu_next;
    c[0] = op;
    c[3] = gpu_vertices;
    gpu_next += 16;
    gpu_commands++;
    gpu_vertices += vertices;
    return c;
}

// Fills the picture with a colour (0x00RRGGBB), behind everything.
static inline void gpu_clear(uint32_t colour) {
    gpu_command(GPU_CLEAR, 3)[1] = colour;
}

// Fills x0 <= x < x1, y0 <= y < y1, in front of everything drawn in 3D.
static inline void gpu_rect(int x0, int y0, int x1, int y1, uint32_t colour) {
    volatile uint32_t* c = gpu_command(GPU_RECT, 6);
    c[1] = colour;
    c[4] = (uint32_t)x0;
    c[5] = (uint32_t)y0;
    c[6] = (uint32_t)x1;
    c[7] = (uint32_t)y1;
    c[8] = GPU_FRAGMENT_COLOUR;
}

// Draws an image of 0x00RRGGBB words (iw x ih) stretched over the rectangle. With use_key,
// pixels of colour `key` are left out.
static inline void gpu_image(int x0, int y0, int x1, int y1, const uint32_t* image, uint32_t iw, uint32_t ih, int use_key, uint32_t key) {
    volatile uint32_t* c = gpu_command(GPU_RECT, 6);
    c[1] = 0x00ffffff;
    c[2] = key;
    c[4] = (uint32_t)x0;
    c[5] = (uint32_t)y0;
    c[6] = (uint32_t)x1;
    c[7] = (uint32_t)y1;
    c[8] = GPU_FRAGMENT_TEXTURE | (use_key ? GPU_FRAGMENT_KEYED : 0);
    c[9] = (uint32_t)image;
    c[10] = iw;
    c[11] = ih;
    c[12] = 0;
    c[13] = 0;
    c[14] = 65536;
    c[15] = 65536;
}

// Draws `count` vertices as triangles. `uniforms` (GPU_ALIGNED vectors) is what the vertex mode
// reads; `texture` (w x h) is what the fragment mode reads. Either may be 0 if unused.
static inline void gpu_draw(const gpu_vertex* vertices, uint32_t count, uint32_t vertex_mode, uint32_t fragment_mode,
                            const int32_t (*uniforms)[4], const void* texture, uint32_t w, uint32_t h, uint32_t key) {
    volatile uint32_t* c = gpu_command(GPU_DRAW, count);
    c[1] = (uint32_t)vertices;
    c[2] = count;
    c[4] = vertex_mode;
    c[5] = fragment_mode;
    c[6] = (uint32_t)uniforms;
    c[7] = (uint32_t)texture;
    c[8] = w;
    c[9] = h;
    c[10] = key;
}

// Hands the list to the GPU and waits until it has been drawn. `how` is GPU_SUBMIT_DRAW,
// optionally with GPU_SUBMIT_WRITEBACK.
static inline void gpu_submit_as(uint32_t how) {
    uint32_t before = GPU_FRAMES;
    GPU_LIST = GPU_COMMANDS;
    GPU_COUNT = gpu_commands;
    GPU_SUBMIT = how;
    do {
        cpu_wait();   // the list is drawn between frames
    } while (GPU_FRAMES == before);
}

static inline void gpu_submit(void) {
    gpu_submit_as(GPU_SUBMIT_DRAW);
}
