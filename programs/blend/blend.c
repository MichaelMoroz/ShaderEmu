// GPU pass test card: every pass (opaque, blended, added, multiplied; with and without the depth
// test) over the same background, with flat rectangles and with triangles whose alpha varies by
// vertex. Checked against tools/gpu_reference.py like the other test card.

#include "../common/gpu.h"

#define W 640
#define H 480
#define ONE 65536

static gpu_vertex triangles[8 * 6];
static uint32_t stripes[8 * 8];   // an 8x8 texture, half of it half transparent

// Two triangles in screen space at one depth, alpha falling from left to right.
static void quad(gpu_vertex* v, int x0, int y0, int x1, int y1, int depth, int red, int green, int blue) {
    static const int corner[6][2] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
    for (int k = 0; k < 6; k++) {
        int right = corner[k][0], low = corner[k][1];
        v[k].position[0] = (right ? x1 : x0) * ONE;
        v[k].position[1] = (low ? y1 : y0) * ONE;
        v[k].position[2] = depth;
        v[k].position[3] = ONE;
        v[k].uv[0] = right * 4 * ONE;
        v[k].uv[1] = low * 4 * ONE;
        v[k].colour[0] = red;
        v[k].colour[1] = green;
        v[k].colour[2] = blue;
        v[k].colour[3] = right ? ONE / 5 : ONE;
    }
}

int main(void) {
    DISP_WIDTH = W;
    DISP_HEIGHT = H;
    DISP_MODE = DISP_MODE_GPU;
    for (int i = 0; i < 64; i++)
        stripes[i] = ((i ^ (i >> 3)) & 1) ? 0x00ffffff : 0x80ffffff;   // opaque white, or half transparent

    gpu_begin();
    gpu_clear(0x00203040);
    // background: bars of colour, and an opaque wall in the middle of the depth range
    for (int k = 0; k < 8; k++)
        gpu_rect(k * 80, 0, k * 80 + 60, H, (k & 1 ? 0x00c04020 : 0x002060c0) + k * 0x00081008);
    quad(&triangles[0], 0, 200, W, 280, ONE / 2, ONE, ONE, ONE / 4);
    gpu_draw(&triangles[0], 6, GPU_VERTEX_SCREEN, GPU_FRAGMENT_COLOUR, 0, 0, 0, 0, 0);

    // (Sizes are whole texels, so no pixel centre sits on a texel boundary.)
    // one column per pass 1-7: a flat rectangle on top, then triangles in front of the wall
    // (depth 1/4) and behind it (depth 3/4), which only the passes without depth still show
    for (int p = 1; p < 8; p++) {
        int x = (p - 1) * 90 + 10;
        gpu_rect_in(x, 20, x + 64, 84, p & 1 ? 0x60f0e040 : 0x90e060f0, p);
        quad(&triangles[p * 6], x, 110, x + 64, 366, (p & 1 ? 1 : 3) * (ONE / 4), ONE, ONE / 2, ONE / 8);
        gpu_draw(&triangles[p * 6], 6, GPU_VERTEX_SCREEN, GPU_FRAGMENT_TEXTURE | GPU_PASS(p), 0, stripes, 8, 8, 0);
        // and a textured rectangle whose texels carry the transparency
        volatile uint32_t* c = gpu_command(GPU_RECT, 6);
        c[1] = 0x0040ff80;
        c[4] = x;
        c[5] = 390;
        c[6] = x + 64;
        c[7] = 454;
        c[8] = gpu_mode(GPU_FRAGMENT_TEXTURE | GPU_PASS(p));
        c[9] = (uint32_t)stripes;
        c[10] = 8;
        c[11] = 8;
        c[12] = 0;
        c[13] = 0;
        c[14] = 2 * ONE;
        c[15] = 2 * ONE;
    }
    // compact quads: two of them from eight one-texel corners, opaque and blended, their colour
    // (and its transparency) given by the command
    static gpu_compact_vertex corners[8];
    for (int q = 0; q < 2; q++) {
        static const int at[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (int k = 0; k < 4; k++) {
            corners[q * 4 + k].x = (560 + q * 16 + at[k][0] * 48) * ONE;
            corners[q * 4 + k].y = (120 + q * 128 + at[k][1] * 96) * ONE;
            corners[q * 4 + k].z = ONE / 8;
            corners[q * 4 + k].uv = (uint32_t)(at[k][0] * 3 * 1024) | (uint32_t)(at[k][1] * 6 * 1024) << 16;
        }
    }
    for (int q = 0; q < 2; q++) {
        gpu_draw((const gpu_vertex*)&corners[q * 4], 6, GPU_VERTEX_SCREEN | GPU_VERTEX_QUADS | GPU_VERTEX_COMPACT,
                 GPU_FRAGMENT_TEXTURE | GPU_PASS(q ? GPU_PASS_BLEND : GPU_PASS_OPAQUE), 0, stripes, 8, 8, 0);
        gpu_next[-16 + 11] = q ? 0x60ff4040 : 0x0040c0ff;
    }
    gpu_submit_as(GPU_SUBMIT_DRAW | GPU_SUBMIT_WRITEBACK);
    uart_puts("blend: done\r\n");
    for (;;) cpu_wait();
}
