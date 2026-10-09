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
  has been drawn it takes the submit word back and counts the frame, so the next list can be
  submitted at once. It also delivers the keyboard and pointer (`input.md`) and the host's
  clock.
- **Writeback** needs no pass of its own: when a submit asks for it, the machine's Commit pass
  (built with `GPU_DEVICE`) copies the picture into RAM, one `0x00RRGGBB` word per pixel, so
  the CPU can read what the GPU drew. The control pass notes the copy at `0x87000070`; the
  commit after the draw makes it and adds one to the copies counter. A program that needs the
  pixels waits for that counter; one that only wants them shown does not wait at all.
- The copy can go to the display's framebuffer, or to **any rectangle of RAM** from
  `0x86000000` up: a picture of the rectangle's size is drawn and stored there, a row length
  apart. That makes any buffer a render target, which is how a window system gives each
  window its own (`nanox.md`), and how an OpenGL program draws into a window.

Depth test (less-or-equal, write on), blending (none) and culling (none) are fixed. What a
program chooses, per draw, is one of a few ways to project vertices and one of a few ways to
colour fragments. The CPU shader has no GPU code.

## Registers

| Address | Contents |
|---|---|
| `0x87000000` | display mode: 3 shows the GPU's picture (see `display.md` for width and height) |
| `0x87000010` | submit: bit 0 draws the list; bit 1 then copies the picture to the RAM framebuffer at `0x87001000`; bit 2 copies it into the rectangle below instead; bits 9-15 say the list has commands for passes 1-7 (below). The GPU sets it back to 0 |
| `0x87000014` | address of the command list |
| `0x87000018` | number of commands in it (at most 4096) |
| `0x8700001c` | lists drawn so far; it changes when a submitted list has been drawn |
| `0x87000034` | the host's clock in milliseconds, updated once per frame |
| `0x87000038` | copies made so far |
| `0x8700003c` | host flags, written every frame. Bit 0: the guest should start its desktop when it boots. Bit 1: the host shows the console as four terminals (`docs/console.md`). Bits 8-15 and 16-23: the largest screen the host shows, width and height in 16s of pixels (0: it does not say) |
| `0x87000100` | a request for a page from the host, and its answer (`docs/fetch.md`) |
| `0x87000300` | a 3D program's last whole frame, for a host that shows it in space (`docs/volume.md`) |
| `0x87000060` | a lock for programs that share the GPU: take it with an atomic swap around looking at the submit word and writing a list's registers |
| `0x87000070` | the copy still to be made for the list drawn last (0: none), then its address, width and height, row length |
| `0x87000050` | for bit 2: address of the rectangle's first pixel (a multiple of 4), its width, its height, and the length of a row in pixels (0: the width) |

The list is drawn between two CPU frames, once. The picture then stays until the next submit.
Nothing the CPU can observe changes inside one of its frames, so a program should end its frame
with `wfi` after submitting (`cpu_wait()`; `gpu_submit()` does this and waits for the counter).
`wfi` is not allowed in user mode, so a Linux program uses `pause` instead (the Zihintpause
hint, `0x0100000f`), which ends the frame the same way from any mode.

A vertex flag (`0x100` in a draw's vertex word) has the GPU multiply projection (c0-c3) by a
modelview matrix (rows in c4-c6) per vertex, so the program does not multiply matrices itself.

## Commands

A command is 16 words (64 bytes). Word 0 is the opcode and word 3 is the command's first
vertex in the mesh; commands must claim separate ranges in increasing order (the vertex shader
finds its command by binary search on word 3), 196,608 vertices in all. Triangles are drawn in
mesh order.

| Op | Command | Vertices | Words |
|---|---|---|---|
| 0 | end | | |
| 1 | clear | 3 | 1: colour. Fills the picture, at the far plane |
| 2 | rectangle | 6 | 1: colour, 2: key, 4-7: x0, y0, x1, y1 (exclusive), 8: fragment mode, 9-11: texture address, width, height, 12-15: u0, v0, u1, v1. Drawn at the near plane |
| 3 | draw | count | 1: vertex buffer, 2: count, 4: vertex mode, 5: fragment mode, 6: uniforms address, 7-9: texture address, width, height, 10: key, 11: colour (compact vertices), 12-15: where a laid texture lies |
| 4 | text | 6 a character | 1: colour, 2: font table, 4-5: x, y of the string's top left, 6: characters, 7: address of the string, 8: fragment word (its pass). Drawn at the near plane |

A string for command 4 is a word a character: the glyph's number in its low half and, in the
high half, how many pixels right of x the character starts (the program adds the widths up;
the GPU cannot, a vertex at a time). A font table is a texel a glyph: the address of its
bitmap (fragment mode 3's layout), its width and its height. The GPU makes the six vertices
of each character's rectangle from those, so a line of text costs the program a store a
character, not a command.

A vertex is 64 bytes: position, normal, texture coordinates, colour, four numbers each.
Numbers are 16.16 fixed point, or floats when the draw says so (the float flag below). A colour given as one word is `0xTTRRGGBB`, T being
transparency, so that `0x00RRGGBB` is opaque; a vertex colour's fourth number is its alpha.
Vertex buffers and uniform vectors must be 16-byte aligned, because the GPU reads them a RAM
texel at a time.

**Vertex modes**

| Mode | Projection |
|---|---|
| 0 screen | position is pixels (x, y) and depth (z, 0 near to 1 far) |
| 1 clip | uniforms 0-3 are the rows of a clip matrix (OpenGL conventions: y up, z from -w to w) |
| 2 lit | as clip; uniforms 4-6 are a normal matrix, 7 a light direction, 8 a diffuse and 9 an ambient colour. The vertex colour becomes max(n.l, 0) * diffuse + ambient |

Flags added to the vertex mode:

| Flag | Meaning |
|---|---|
| `0x100` modelview | uniforms 0-3 are the projection alone and 4-6 the modelview's rows; the GPU multiplies |
| `0x200` quads | the buffer holds four corners per quad, in order round it, for every six vertices drawn |
| `0x400` compact | a vertex is one texel: x, y, z, then u in the low and v in the high 16 bits, in 1/1024ths (so within 32 repeats). All of the command's vertices take the colour in its word 11 |
| `0x800` float | the draw's numbers are IEEE single-precision floats, not 16.16: its vertices (a compact vertex's x, y and z; its fourth word stays 1/1024ths), its uniforms, and a laid texture's four words. A program built for the machine's float instructions (`docs/fpu.md`) then converts nothing |
| `0x1000` tagged | a compact vertex's fourth word is u in bits 0-11 and v in bits 12-23 (1/1024ths, so within 4 repeats) and a tag in bits 24-31 |
| `0x2000` table | with tagged: word 11 is not a colour but the address of a table of them, a word each, and a vertex has the colour its tag names. A model whose vertices carry their normal's number is shaded by a table of the light at each normal, with no vertex written |
| `0x4000` packed | with tagged: a vertex is one word, x, y and z a byte each (whole numbers 0 to 255, which the matrix scales) and the tag in the top byte; u and v are a word a vertex in a second buffer, whose address is word 12. Quake's models are this in their files |
| `0x8000` points | with compact and the modelview flag: the buffer has a vertex a triangle, which faces the eye. Its corners are the point, then `s` above it and `s` to its right as the eye sees it; `s` is word 12, times 1 + word 13 x depth when the point is word 14 or more in front of the eye. All three corners have the vertex's texture coordinates |

A textured quad is then 16 words of vertices instead of 96.

**Fragment modes**

| Mode | Colour |
|---|---|
| 0 | the interpolated vertex colour |
| 1 | a texture of `0x00RRGGBB` words, times the colour |
| 2 | a texture of bytes looked up in the display palette (`0x87000400`), times the colour |
| 3 | a texture of single bits, each row a whole number of bytes, leftmost pixel in the highest bit: set bits take the colour, clear bits are not drawn |
| 4 | a texture of three bytes a pixel (red, green, blue, as in a PPM file), rows not padded, starting at any byte, times the colour. Its address may be in the ROM (from `0x40000000`): the draw pass has the ROM's four textures (`_Data_MTD_R/G/B/A`) as well as the state |
| +0x100 | texels equal to the key (a colour, or an index in mode 2) are not drawn |
| +0x200 | (draws) the texture is laid on the picture, not on the surface: a pixel at (x, y) of the picture takes the texel at words 12-13 plus (x, y) / 1024 times words 14-15 (16.16), and the colour is not applied. The surface still writes depth: a sky that hides what is behind it |

Textures are anywhere in RAM, sampled nearest and repeating, with coordinates 0..1 across.

**Passes.** Bits 16-18 of the fragment mode word say which pass a command is drawn in. The
passes are drawn in order, each as one draw of the whole mesh with fixed blending and depth
use, and within a pass commands keep the list's order:

| Pass | Blending | Depth |
|---|---|---|
| 0 | none | tested and written |
| 1 | over what is there, by alpha | tested |
| 2 | added, scaled by alpha | tested |
| 3 | what is there times the colour | tested |
| 4-7 | the same four | not used: for drawing on top |

So a frame is at most eight draws however many commands and state changes it has, and a list
that only uses pass 0 is one, as before. The host draws passes 1-7 only while lists ask for
them: it reads the submit word back, sees the bits, and from then on draws those passes (and
for some hundred frames after their last use). A list that needs a pass not being drawn yet
stays submitted, undrawn, until it is; that delay happens once, not per frame. In the shader
the pass is `_GpuPass` and the passes drawn `_GpuPasses`; a host that leaves both at zero draws
pass 0 only, and lists that ask for more wait for ever.

`programs/blend` is the test card for all of this.

A window system uses the GPU as a 2D accelerator this way (`nanox.md`): each list starts with
a rectangle textured with the buffer it draws on and is copied back into that buffer, so the
CPU and the GPU take turns drawing on the same pixels. The screen is then one more list: a
textured rectangle per window, copied into the display's framebuffer.

## For C programs

`programs/common/gpu.h`: `gpu_begin()`, `gpu_clear()`, `gpu_rect()`, `gpu_image()`,
`gpu_draw()`, `gpu_submit()`, and `gpu_submit_as()` to ask for the writeback.
`programs/gears` is the example: three gears, 640 triangles, lit and textured, with a bar and
a keyed image on top (`w` toggles the writeback). `programs/rects` is a test card of 3,600
rectangles in one list, written back.

## OpenGL for Nano-X programs

`programs/linux/gles.c` (`include/GLES/gl.h`, `segl.h`) is an OpenGL in the manner of OpenGL ES
1.x: vertex arrays and `glDrawArrays`, matrices, paletted and RGBA textures, blending, depth
and alpha test. It is built one of two ways, and a number inside it (a matrix's, a vertex's)
is then that kind:

- **fixed point** (the default): `GLfixed` is what the device reads and nothing is converted.
  Doom and Tiberian Dawn, integers throughout, are built with this.
- **floats** (`-DSEGL_FLOAT`): matrices and vertices are floats and every draw has the float
  flag. Quake and the guest compiler's `libgles.a` are built with this.

Either build takes both kinds of call (`glTranslatex` and `glTranslatef`) and of array
(`GL_FIXED` and `GL_FLOAT`) and converts the other kind. `glRotatef` is about any axis in the
float build, about an axis of the coordinate system in the other.

- `GL_QUADS` and triangles go into GPU memory as compact vertices; calls with the same state
  join one command. Blending and the depth test choose the pass, so a program may set state
  in any order and still costs at most eight draws.
- A paletted texture (`GL_COLOR_INDEX8_EXT`) is looked up in one shared palette
  (`glColorTableEXT`, or `seglPalette()`); with `GL_ALPHA_TEST` on, texels of the index given
  to `glColorKeySE` are holes.
- `seglMemory()` hands out GPU memory and `seglTexturePointer()` makes it a texture with no
  copy; `seglInit(window)` attaches to a Nano-X window and `seglSwap()` shows the frame.
- A matrix holds 16.16 numbers, so `glOrthox` over a pixel-sized range is not exact (2/320
  is not representable). Draw 2D in units of the whole view (0 to 1) instead.
- **Commands that stay** (`seglKeep`): a program whose scene mostly stands still writes its
  quads into `seglMemory()` once and a command for each run of them (`seglKeptQuads`). Those
  commands are the head of every frame's list from then on, in both of the library's sets,
  and a frame costs what it changes: a run's grey, its texture, how many of its quads show,
  or the vertices themselves, which are the program's memory. `seglKeptMatrices()` gives them
  the frame's matrices; `seglKeptLaid()` lays a run's texture on the picture. Doom's levels
  are drawn this way (`docs/doom.md`).
- `seglQuad()` is one textured quad in one call, for the many small draws that differ in
  texture: about 150 instructions against `glDrawArrays` and the state calls before it.
  `seglWindowTexture()` makes the window's own pixels a texture: the last frame, for free.
- A window may be resized at any time, and a program that stretches a picture over it must
  not map the pointer by the size it asked for. `seglPicturePoint()` turns an event's x, y
  into a pixel of such a picture at the window's size now; `seglWindowChanged()` (on a
  `GR_UPDATE_SIZE` event) has the library learn the new size at once, not at the next swap.
- `seglSprite()` is a rectangle of a texture over what the list drew before it: always the
  blended pass with no depth, so that many small pictures and their shadows keep their
  order. A texture of `SEGL_BITS` is one bit a pixel, drawn in the call's colour.
- `seglKeptBlend()` puts a kept command into a blended pass, with a texture of words: Quake's
  light maps are kept commands that multiply (`docs/quake.md`).
- `seglScreenSpace()` and `seglScreenUsed()` draw triangles of whole vertices in the window's
  pixels, each with its own colour, blended over the frame in the order given: a user
  interface (`docs/imgui.md`).
- `seglCompact()` draws vertices the program made compact itself; `seglCompactSpace()` gives it
  room in the frame to write them in place, and `seglCompactAt()` draws ones that already are in
  GPU memory, where they are, in any later frame too. `seglCompactTrim()` gives back room.
- `seglTagged()` and `seglPacked()` draw tagged and packed vertices, with a table of colours
  or without; `seglPoints()` gives room for points (`docs/quake.md`: models and particles).
- `seglMemorySpare()` is 3.9 MB more for one program: the display's own framebuffer
  (`docs/display.md`), which nothing reads while the desktop is layers.
- `seglSwapAgain()` draws the last list once more. Its textures, and the commands and vertices
  `seglLastCommand()` and `seglLastVertices()` pointed at, are the program's memory: a
  pointer that moved or a picture that changed needs no new list (`docs/tdawn.md`).
- A set holds 3,072 commands, 256 matrix blocks and 10,240 compact vertices of a frame's own.

## From Linux

The stock `glxgears.c` from Mesa's demos runs under the Linux image, unmodified, at about 145
frames a second with this project's kernel (about 80 on upstream's):

    / # glxgears
    / # glxgears -geometry 1280x720

- `programs/linux/gl.c` is the driver: a small OpenGL 1.x library (immediate mode, display
  lists, the matrix stacks, one light, flat and smooth shading) with the GLX and Xlib calls a
  program uses to open its window. A display list becomes a vertex buffer in GPU memory;
  calling it becomes a lit draw; `glXSwapBuffers` submits. A call outside the subset does not
  exist, so such a program fails to link rather than misbehave.
- This project's kernel (`linux/kernel`: pimaker's 5.17.11 fork plus our drivers) has
  `/dev/gpu`. It maps GPU memory, `0x86000000` to `0x87b00000` (the control words are 16 MiB
  in), so buffers are written with plain stores. One ioctl draws a list, copies the picture
  where asked and returns when that is done; it holds a lock, so several programs can use
  the GPU without stepping on each other's lists. An older ioctl only waits for a frame.
  Either way the kernel waits with `wfi`, which ends the emulator's frame.
- A program can also submit with no system call: it takes the lock word, writes the registers
  through its mapping and ends the frame with `pause`. The OpenGL library and Nano-X do this,
  and the library reads the time from the clock word instead of asking the kernel.
- Under Nano-X `glxgears` is a window: the library asks the server where the window's
  pixels are and has each frame copied there (about 500 frames a second at 300x300 on an RTX
  5090 with DXC, one emulator frame of about 8,000 instructions each). With no server running
  it takes the whole display, as before.
- Upstream's kernel has neither `/dev/gpu` nor `/dev/mem`. There the library falls back to
  an MTD device: the phram driver makes one for a physical range written to
  `/sys/module/phram/parameters/phram`, and the library uses `pread` and `pwrite` on it and
  spins while it waits.
- The GPU's memory must not be RAM the kernel uses, so the image's device tree ends RAM at
  `0x86000000` (`tools/make_linux_image.py`, which also adds the binary to the root
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
