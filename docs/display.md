# Display

The machine's display is plain RAM at fixed physical addresses. The CPU shader knows nothing
about it: a program draws with ordinary stores, and whatever shows the picture (the harness
window today, a material in a VRChat world later) reads the RAM texture and decodes it.

| Address | Contents |
|---|---|
| `0x87000000` | mode: 0 = off, 1 = 32-bit colour, 2 = 8-bit indexed, 3 = the GPU's picture |
| `0x87000004` | width in pixels |
| `0x87000008` | height in pixels |
| `0x87000040` | cursor x: where the left edge of the cursor image is, in display pixels (may be negative) |
| `0x87000044` | cursor y |
| `0x87000048` | cursor on: 1 shows it |
| `0x8700004c` | address of the cursor image: 32x32 words, `0xAARRGGBB`, drawn where the top byte is not 0 |
| `0x87000400` | palette for mode 2: 256 words, `0x00RRGGBB` |
| `0x87001000` | pixels, rows top to bottom, no padding between rows |

- **Mode 1**: one word per pixel, `0x00RRGGBB`.
- **Mode 2**: one byte per pixel, an index into the palette. This is the layout Doom renders
  into, and it packs 16 pixels into each 16-byte RAM texel.
- **Mode 3**: the pixels are not in RAM: the display shows what the GPU drew (`gpu.md`), at
  this width and height.
- The cursor is drawn by whatever shows the display, over the picture in any mode. It is in
  neither the framebuffer nor the GPU's picture, so moving it is two stores and nothing
  under it needs saving. The image may be anywhere in RAM.
- Width and height may each be 1 to 2048. Write them first and the mode last, so a
  half-initialised display is never shown.
- The control words share one RAM texel (16 bytes), so a reader gets all three with one fetch.

`programs/common/platform.h` has the definitions for C programs. The harness shows the display
to the right of the memory image, scaled by the largest whole factor that fits.

Under Linux with this project's kernel the display is also `/dev/fb0`
(`linux/kernel/shaderemu_fb.c`): 32 bits per pixel, 640x480 until a program sets another mode,
and opening it switches the display to mode 1. The keyboard and pointer are in `input.md`,
and the window system that draws here in `nanox.md`.

Pixel writes go through the CPU's write cache like any other store, so the number of distinct
words a program can draw per frame is bounded by the cache. The GPU (`gpu.md`) is the way
around that: it draws triangles, rectangles and images with the graphics card's rasteriser.
