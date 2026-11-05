// The GPU device (docs/machine/gpu.md): real triangles, rasterised by the graphics card into the GPU's
// own colour and depth target. A program builds a list of commands in RAM and submits it.
// Depth test and blending are fixed; per draw the program picks how vertices are projected and
// how fragments are coloured. Select DISP_MODE_GPU to show the result.
// All numbers handed to the GPU are 16.16 fixed point.
#pragma once

#include "platform.h"
#include "gpu_uploads.h"

#define GPU_SUBMIT (*(volatile uint32_t*)0x81000010)   // what to do with the list (below); the GPU clears it
#define GPU_SUBMIT_DRAW 1        // draw it
#define GPU_SUBMIT_WRITEBACK 2   // then copy the picture into the RAM framebuffer (DISP_PIXELS)
#define GPU_SUBMIT_INTO 4        // then copy it into the rectangle at GPU_INTO, whose size the picture has
#define GPU_INTO ((volatile uint32_t*)0x81000050)   // address, width, height, row length in pixels
#define GPU_LIST   (*(volatile uint32_t*)0x81000014)   // address of the command list
#define GPU_COUNT  (*(volatile uint32_t*)0x81000018)   // number of commands in it
#define GPU_FRAMES (*(volatile uint32_t*)0x8100001c)   // lists drawn so far

#define GPU_COMMANDS 0x81500000u   // where gpu_begin() builds the list: 4096 commands of 64 bytes
#define GPU_MAX_COMMANDS 4096
#define GPU_MAX_VERTICES 196608    // per list, over all commands (65,536 triangles)

enum { GPU_END, GPU_CLEAR, GPU_RECT, GPU_DRAW, GPU_TEXT, GPU_SPRITES, GPU_UPLOAD, GPU_COPY };
#define GPU_SUBMIT_UPLOADS 8   // (gpu_submit_as adds it) the list has uploads: drawn once the host has drawn them and made the levels

// The VRAM (docs/machine/gpu.md, "VRAM"): 4096 x 4096 colours with mip levels, which gpu_upload() copies a texture into. A
// texture there is drawn by giving its slot in place of its address: GPU_VRAM_SLOT(x, y, L), (x, y) its texel (0, 0), L the
// most it is minified, 2^L (0: no levels); the block it needs is gpu_vram_block()'s, from 2^L before (x, y), and x and y are
// whole multiples of 2^L.
#define GPU_VRAM_SIDE 4096
#define GPU_VRAM_SLOT(x, y, lod) ((uint32_t)(x) | (uint32_t)(y) << 12 | (uint32_t)(lod) << 24)
#define GPU_VRAM_PREMULTIPLIED 0x08000000u   // with GPU_FRAGMENT_KEYED: the key's texels are holes (in the upload's slot and the draws')
#define GPU_VRAM_CLAMPED 0x10000000u         // padded with its edge (a light map), not as it repeats
static inline uint32_t gpu_vram_pad(uint32_t slot) { return 1u << ((slot >> 24) & 7); }
static inline void gpu_vram_block(uint32_t w, uint32_t h, uint32_t slot, uint32_t* bw, uint32_t* bh) {
    uint32_t pad = gpu_vram_pad(slot), mask = (1u << ((slot >> 24) & 7)) - 1;
    *bw = (w + 2 * pad + mask) & ~mask;
    *bh = (h + 2 * pad + mask) & ~mask;
}

// How a draw's vertices are projected.
enum {
    GPU_VERTEX_SCREEN,   // position is pixels (x, y) and depth (z, 0 near .. 1 far)
    GPU_VERTEX_CLIP,     // uniforms 0-3: clip matrix rows (OpenGL conventions)
    GPU_VERTEX_LIT,      // also 4-6: normal matrix rows, 7: light direction, 8: diffuse, 9: ambient colour
    GPU_VERTEX_MODELVIEW = 0x100,   // add: 0-3 is the projection alone and 4-6 the modelview's rows
    GPU_VERTEX_QUADS = 0x200,       // add: four corners stored per quad (in order round it) for every six vertices drawn
    GPU_VERTEX_COMPACT = 0x400,     // add: a vertex is gpu_compact_vertex, and the colour is the command's word 11
};

// How its fragments are coloured.
enum {
    GPU_FRAGMENT_COLOUR,          // the vertex colour
    GPU_FRAGMENT_TEXTURE,         // a texture of 0x00RRGGBB words, times the colour
    GPU_FRAGMENT_INDEXED,         // a texture of bytes through the display palette, times the colour
    GPU_FRAGMENT_MASK,            // a 1-bit texture (rows of whole bytes, leftmost bit highest): set bits take the colour
    GPU_FRAGMENT_RGB24,           // a texture of three bytes a pixel (red, green, blue; rows not padded), times the colour
    GPU_FRAGMENT_KEYED = 0x100,   // add: texels equal to the key are not drawn
};

// The pass a command is drawn in, added to its fragment mode with GPU_PASS(). Passes are drawn
// in this order, each command list order within; colours are 0xTTRRGGBB, T being transparency.
enum {
    GPU_PASS_OPAQUE,         // depth tested and written (what a command is without GPU_PASS)
    GPU_PASS_BLEND,          // over what is there by its alpha; depth tested, not written
    GPU_PASS_ADD,            // added, scaled by its alpha
    GPU_PASS_MULTIPLY,       // what is there times its colour
    GPU_PASS_FLAT,           // the same four with no depth test: for drawing on top
    GPU_PASS_FLAT_BLEND,
    GPU_PASS_FLAT_ADD,
    GPU_PASS_FLAT_MULTIPLY,
    // what is there's alpha, as GL keeps it (each pass's factors on alpha too): drawn after GPU_PASS_MULTIPLY
    GPU_PASS_SHINE = 8,            // added where what is there is transparent (GL_ONE_MINUS_DST_ALPHA, GL_ONE)
    GPU_PASS_MASKED_MULTIPLY,      // what is there times the colour, plus what is there times its transparency
                                   // (GL_DST_COLOR, GL_ONE_MINUS_DST_ALPHA): Quake III's light map over a glow
    GPU_PASS_FLAT_SHINE = 12,      // the two with no depth test: drawn last
    GPU_PASS_FLAT_MASKED_MULTIPLY,
};
#define GPU_PASS(n) ((uint32_t)(n) << 16)

// The GPU reads vertices and uniform vectors a whole 16-byte RAM texel at a time, so both must
// sit on 16-byte boundaries. Textures may be anywhere.
#define GPU_ALIGNED __attribute__((aligned(16)))

typedef struct {
    int32_t position[4];   // w = 1 for points
    int32_t normal[4];
    int32_t uv[4];         // 0..1 across the texture, repeating
    int32_t colour[4];     // 0..1 red, green, blue (not used by GPU_VERTEX_LIT)
} GPU_ALIGNED gpu_vertex;

// One texel: u and v are in 1/1024ths, 16 bits each (so within 32 repeats of the texture).
typedef struct {
    int32_t x, y, z;
    uint32_t uv;   // u | v << 16
} GPU_ALIGNED gpu_compact_vertex;

static volatile uint32_t* gpu_next;
static uint32_t gpu_commands, gpu_vertices;
static uint32_t gpu_passes;   // passes 1-15 this list uses, one bit each, for the submit word
static uint32_t gpu_uploads;  // GPU_SUBMIT_UPLOADS if the list has uploads

static inline void gpu_begin(void) {
    gpu_next = (volatile uint32_t*)GPU_COMMANDS;
    gpu_commands = 0;
    gpu_vertices = 0;
    gpu_passes = 0;
    gpu_uploads = 0;
}

// Notes the pass of a fragment mode, so the submit word can tell the host which to draw.
static inline uint32_t gpu_mode(uint32_t fragment_mode) {
    gpu_passes |= (1u << ((fragment_mode >> 16) & 15)) & 0xfffe;
    return fragment_mode;
}

// A rectangle of one colour in a pass: gpu_rect() with blending.
static inline void gpu_rect_in(int x0, int y0, int x1, int y1, uint32_t colour, uint32_t pass);

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

static inline void gpu_rect_in(int x0, int y0, int x1, int y1, uint32_t colour, uint32_t pass) {
    gpu_rect(x0, y0, x1, y1, colour);
    gpu_next[-16 + 8] = gpu_mode(GPU_FRAGMENT_COLOUR | GPU_PASS(pass));
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
    c[5] = gpu_mode(fragment_mode);
    c[6] = (uint32_t)uniforms;
    c[7] = (uint32_t)texture;
    c[8] = w;
    c[9] = h;
    c[10] = key;
}

// Copies a texture into the VRAM at `slot` (GPU_VRAM_SLOT): `kind` GPU_FRAGMENT_TEXTURE, _INDEXED or _RGB24, with
// GPU_FRAGMENT_KEYED for texels equal to `key` to be holes; `palette` 0 for the display's, `row` 0 for rows of `w`.
// A rectangle of the picture (x, y from its top left, w x h) copied into the VRAM's slot at (dx, dy) when the list's passes
// are drawn: after all of them, whatever comes after it in the list (docs/machine/gpu.md, "Copies"). `turned`: the rows
// turned over (OpenGL's order). The slot is drawn from like any other in the next list; it has no levels.
static inline void gpu_copy(int x, int y, int w, int h, uint32_t slot, int dx, int dy, int turned) {
    volatile uint32_t* c = gpu_command(GPU_COPY, 6);
    c[1] = (uint32_t)x | (uint32_t)y << 16;
    c[2] = (uint32_t)w | (uint32_t)h << 16;
    c[4] = slot;
    c[5] = (uint32_t)dx | (uint32_t)dy << 16;
    c[6] = turned ? 1u : 0u;
}

static inline void gpu_upload(const void* pixels, uint32_t kind, uint32_t w, uint32_t h, uint32_t slot, uint32_t key,
                              const uint32_t* palette, uint32_t row) {
    volatile uint32_t* c = gpu_command(GPU_UPLOAD, 6);
    c[1] = (uint32_t)pixels;
    c[2] = kind;
    c[4] = w;
    c[5] = h;
    c[6] = slot;
    c[7] = key;
    c[8] = (uint32_t)palette;
    c[9] = row;
    gpu_uploads = GPU_SUBMIT_UPLOADS;
}

// Hands the list to the GPU and waits until it has been drawn. `how` is GPU_SUBMIT_DRAW,
// optionally with GPU_SUBMIT_WRITEBACK.
static inline void gpu_submit_as(uint32_t how) {
    uint32_t before = GPU_FRAMES;
    // (every texture the list names in RAM or the ROM copied into the VRAM first: the GPU draws from there alone)
    gpu_uploads_list l = {0, 0, 0, (uint32_t*)GPU_COMMANDS, GPU_COMMANDS, gpu_commands, GPU_MAX_COMMANDS, gpu_vertices,
                          GPU_MAX_VERTICES, 0, 0, 0};
    static gpu_up_state ups;
    gpu_list_uploads(&l, 0, &ups);
    gpu_commands = l.count, gpu_vertices = l.mesh, gpu_next = (volatile uint32_t*)GPU_COMMANDS + 16 * gpu_commands;
    GPU_LIST = GPU_COMMANDS;
    GPU_COUNT = gpu_commands;
    GPU_SUBMIT = how | (gpu_passes << 8) | gpu_uploads;   // drawn once the host draws every pass used
    do {
        __asm__ volatile(".word 0x0100000f");   // the pause hint: the list is drawn between frames (wfi would wait on rvc_cpu for an interrupt nothing raises)
    } while (GPU_FRAMES == before);
}

static inline void gpu_submit(void) {
    gpu_submit_as(GPU_SUBMIT_DRAW);
}
