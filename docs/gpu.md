# GPU

The machine has a GPU: a second device next to the CPU, in its own shader
(`experiments/rvc_opt/gpu.shader`). It draws real triangles with the graphics card's
rasteriser. A program writes vertex buffers and a short list of commands into RAM and sets one
word; the list is then drawn into the GPU's own colour and depth target.

How it is drawn:

- **GPUDraw** is a mesh of 65,536 triangles that carries nothing but vertex numbers. Its vertex
  shader reads the machine's state texture: it looks up which command claims the vertex, reads
  that vertex from the guest's buffer and places it. Vertices no command claims collapse to
  nothing. The fragment shader reads the same texture for textures and the palette.
- The target is a 2048x2048 colour and depth target; the picture is its top-left
  width x height pixels. In the harness it is a render target; in a VRChat world it is a camera
  that sees only this mesh, on a layer of its own, clearing depth only.
- **GPUControl** is one small extra update zone on the state texture. After a submitted list
  has been drawn it takes the submit word back and counts the frame.

Depth test (less-or-equal, write on), blending (none) and culling (none) are fixed. What a
program chooses, per draw, is one of a few ways to project vertices and one of a few ways to
colour fragments. The CPU shader has no GPU code.

## Registers

| Address | Contents |
|---|---|
| `0x87000000` | display mode: 3 shows the GPU's picture (see `display.md` for width and height) |
| `0x87000010` | submit: write 1 to draw the list; the GPU sets it back to 0 |
| `0x87000014` | address of the command list |
| `0x87000018` | number of commands in it (at most 256) |
| `0x8700001c` | lists drawn so far; it changes when a submitted list has been drawn |

The list is drawn between two CPU frames, once. The picture then stays until the next submit.
Nothing the CPU can observe changes inside one of its frames, so a program should end its frame
with `wfi` after submitting (`cpu_wait()`; `gpu_submit()` does this and waits for the counter).

## Commands

A command is 16 words (64 bytes). Word 0 is the opcode and word 3 is the command's first
vertex in the mesh; commands must claim separate ranges, 196,608 vertices in all. Triangles are
drawn in mesh order.

| Op | Command | Vertices | Words |
|---|---|---|---|
| 0 | end | | |
| 1 | clear | 3 | 1: colour. Fills the picture, at the far plane |
| 2 | rectangle | 6 | 1: colour, 2: key, 4-7: x0, y0, x1, y1 (exclusive), 8: fragment mode, 9-11: texture address, width, height, 12-15: u0, v0, u1, v1. Drawn at the near plane |
| 3 | draw | count | 1: vertex buffer, 2: count, 4: vertex mode, 5: fragment mode, 6: uniforms address, 7-9: texture address, width, height, 10: key |

A vertex is 64 bytes: position, normal, texture coordinates, colour, four numbers each.
Numbers are 16.16 fixed point; colours given as one word are `0x00RRGGBB`. Vertex buffers and
uniform vectors must be 16-byte aligned, because the GPU reads them a RAM texel at a time.

**Vertex modes**

| Mode | Projection |
|---|---|
| 0 screen | position is pixels (x, y) and depth (z, 0 near to 1 far) |
| 1 clip | uniforms 0-3 are the rows of a clip matrix (OpenGL conventions: y up, z from -w to w) |
| 2 lit | as clip; uniforms 4-6 are a normal matrix, 7 a light direction, 8 a diffuse and 9 an ambient colour. The vertex colour becomes max(n.l, 0) * diffuse + ambient |

**Fragment modes**

| Mode | Colour |
|---|---|
| 0 | the interpolated vertex colour |
| 1 | a texture of `0x00RRGGBB` words, times the colour |
| 2 | a texture of bytes looked up in the display palette (`0x87000400`), times the colour |
| +0x100 | texels equal to the key (a colour, or an index in mode 2) are not drawn |

Textures are anywhere in RAM, sampled nearest and repeating, with coordinates 0..1 across.

## For C programs

`programs/common/gpu.h`: `gpu_begin()`, `gpu_clear()`, `gpu_rect()`, `gpu_image()`,
`gpu_draw()`, `gpu_submit()`. `programs/gears` is the example: three gears, 640 triangles, lit
and textured, with a bar and a keyed image on top.

## From Linux

The stock `glxgears.c` from Mesa's demos runs under the Linux image, unmodified, at about 70
frames a second:

    / # glxgears
    / # glxgears -geometry 1280x720

- `programs/linux/gl.c` is the driver: a small OpenGL 1.x library (immediate mode, display
  lists, the matrix stacks, one light, flat and smooth shading) with the GLX and Xlib calls a
  program uses to open its window. A display list becomes a vertex buffer in GPU memory;
  calling it becomes a lit draw; `glXSwapBuffers` submits. A call outside the subset does not
  exist, so such a program fails to link rather than misbehave.
- The kernel has no `/dev/mem`. GPU memory is reached through an MTD device instead: the
  phram driver makes one for a physical range written to
  `/sys/module/phram/parameters/phram`, and the library reads and writes it with `pread` and
  `pwrite`. No kernel change is needed.
- The GPU's memory must not be RAM the kernel uses, so the image's device tree ends RAM at
  `0x87000000` (`tools/make_linux_image.py`, which also adds the binary to the root
  filesystem).
- The binary is static and links no C library: `programs/linux/libc.c` is a tiny runtime, and
  floating point comes from compiler-rt's soft-float routines, fetched at build time
  (`programs/linux/fetch.py`). Inside, the driver works in 16.16 fixed point.

## Checking it

`tools/gpu_reference.py SNAPSHOT TARGET.bmp` redraws the command list left in a snapshot's RAM
in software and compares it with the colour target saved by `--gpu-capture`. Pause the guest
before snapshotting. The card's rasteriser is not bit-exact with the model: expect agreement
away from triangle edges except for single-pixel shifts where a nearest-sampled texture changes
texel.

## Cost

The graphics card rasterises, so cost follows covered pixels, not pixels times triangles.
Gears at 1280x720 takes about 0.02 ms on an RTX 5090. The vertex shader runs for all 196,608
mesh vertices every frame, most of which exit after reading one word.

In D3D12 the state texture must be in the non-pixel-shader-resource state as well as the
pixel-shader one while GPUDraw runs: without it the vertex shader reads stale data, with no
error.
