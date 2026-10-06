// GPU test card: 3,600 rectangles in one command list, written back to the RAM framebuffer.
// It exercises the long command list (the vertex shader finds each vertex's command by binary
// search) and the writeback path, and is checked against tools/gpu_reference.py.

#include "../common/gpu.h"

#define W 1280
#define H 720
#define CELL 16

int main(void) {
    DISP_WIDTH = W;
    DISP_HEIGHT = H;
    DISP_MODE = DISP_MODE_GPU;
    gpu_begin();
    gpu_clear(0x00000000);
    uint32_t n = 0;
    for (int y = 0; y < H / CELL; y++)
        for (int x = 0; x < W / CELL; x++, n++) {
            // every cell its own colour, inset by a pixel so neighbours stay distinct
            uint32_t colour = ((uint32_t)(x * 3) << 16) | ((uint32_t)(y * 5) << 8) | (n * 37 & 0xff);
            gpu_rect(x * CELL + 1, y * CELL + 1, (x + 1) * CELL, (y + 1) * CELL, colour);
        }
    gpu_submit_as(GPU_SUBMIT_DRAW | GPU_SUBMIT_WRITEBACK);
    uart_puts("rects: done\r\n");
    for (;;) cpu_wait();
}
