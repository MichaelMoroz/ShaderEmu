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
  submitted at once. It also delivers the keyboard and pointer (`input.md`), the host's
  clock and the console's typed characters (`console.md`, "Words in memory").
- **Writeback** needs no pass of its own: when a submit asks for it, the machine's Commit pass
  (built with `GPU_DEVICE`) copies the picture into RAM, one `0x00RRGGBB` word per pixel, so
  the CPU can read what the GPU drew. The control pass notes the copy at `0x81000070`; the
  commit after the draw makes it and adds one to the copies counter. A program that needs the
  pixels waits for that counter; one that only wants them shown does not wait at all.
- The copy can go to the display's framebuffer, or to **any rectangle of RAM** from
  `0x80000000` to `0x81e00000` (RAM from `0x82000000` is Linux's, `docs/system/nommu.md`): a picture of the rectangle's size is drawn and stored there, a row length
  apart. That makes any buffer a render target, which is how a window system gives each
  window its own (`nanox.md`), and how an OpenGL program draws into a window.

Depth test (less-or-equal, write on), blending (none) and culling (none) are fixed. What a
program chooses, per draw, is one of a few ways to project vertices and one of a few ways to
colour fragments. The CPU shader has no GPU code.

## Registers

| Address | Contents |
|---|---|
| `0x81000000` | display mode: 3 shows the GPU's picture (see `display.md` for width and height) |
| `0x81000010` | submit: bit 0 draws the list; bit 1 then copies the picture to the RAM framebuffer at `0x81001000`; bit 2 copies it into the rectangle below instead; bits 9-15 say the list has commands for passes 1-7 (below). The GPU sets it back to 0 |
| `0x81000014` | address of the command list |
| `0x81000018` | number of commands in it (at most 4096) |
| `0x8100001c` | lists drawn so far; it changes when a submitted list has been drawn |
| `0x81000034` | the host's clock in milliseconds, updated once per frame |
| `0x81000038` | copies made so far |
| `0x8100003c` | host flags, written every frame. Bit 0: the guest should start its desktop when it boots. Bit 1: the host shows the console as four terminals (`docs/machine/console.md`). Bits 2-3: the workers' layout the host asks `geotune` for (0 its own pick; 1, 2, 3: small workers in 2, 4, 8 columns; `docs/machine/multicore.md`, "Packing the small workers"). Bits 8-15 and 16-23: the largest screen the host shows, width and height in 16s of pixels (0: it does not say) |
| `0x81000100` | a request for a page from the host, and its answer (`docs/system/fetch.md`) |
| `0x81000300` | a 3D program's last whole frame, for a host that shows it in space (`docs/world/volume.md`) |
| `0x81000320` | the counters' head: a core added, "ERGM" from a host that merges, merges so far, commits so far, a pass each (`docs/machine/multicore.md`, "Counters") |
| `0x81000380` | what each core ran, and how many cores there are (`docs/machine/multicore.md`) |
| `0x81000060` | a lock for programs that share the GPU: take it with an atomic swap around looking at the submit word and writing a list's registers |
| `0x81000070` | the copy still to be made for the list drawn last (0: none), then its address, width and height, row length |
| `0x81000050` | for bit 2: address of the rectangle's first pixel (a multiple of 4), its width, its height, and the length of a row in pixels (0: the width) |

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
| 4 | text | 6 a character | 1: colour, 2: font table, 4-5: x, y of the string's top left, 6: characters, 7: address of the string, 8: fragment word (its pass; mode 7 for glyphs of coverage, 8 for distance fields; `0x100`: words 9-11 are there), 9: a scale (16.16; 0: none), 10: a slant in 256ths of a pixel a row, and from bit 16 the row that stays where it is, 11: what the table's bitmap addresses are counted from, 12 (mode 8): the distance range in pixels before the scale (16.16). Drawn at the near plane |

A string for command 4 is a word a character: the glyph's number in its low half and, in the
high half, how many pixels right of x the character starts (the program adds the widths up;
the GPU cannot, a vertex at a time). A font table is a texel a glyph: the address of its
bitmap (fragment mode 3's layout), its width and its height, and in its fourth word how far
the bitmap's corner is from the character's (a signed byte right, a signed byte down). The
GPU makes the six vertices of each character's rectangle from those, so a line of text costs
the program a store a character, not a command. The table and the bitmaps may be a file's
bytes where it lies in the ROM (addresses from `0x40000000`): a font of `docs/system/fonts.md` is
drawn with nothing copied. Its glyphs are fragment mode 7: four bits a pixel of how much of
the pixel the glyph covers, rows of whole bytes, the left pixel in the high half; the
coverage is the colour's alpha, so such a command names a pass that blends.

Fragment mode 8 is a glyph of three distance fields (multi-channel, `docs/system/fonts.md`, "Distance
fields"): its table texel is where its pixels are in the face's atlas, their width and height
(16 bits each), and its quad's width and height and corner (16 bits each, signed, in 64ths of a
pixel). A pixel is three bytes, rows not padded. The fragment samples the field at four points,
the centres of its pixel's quarters: at each the four atlas pixels round it, weighed, and the
median of their channels is the distance from the edge, 0.5 on it. Word 12 times the scale is
how many of the screen's pixels the atlas's range is (times 1.2, `MSDF_SHARP`), worked out a
command in the vertex stage with how far a screen pixel is in the quad: a quarter's coverage is
twice that times (median - 0.5) + 0.5, clamped, and the colour's alpha is times the mean of the
four, so the same quad is sharp at any scale. Mode 7 is as it was.

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
| `0x800` float | the draw's numbers are IEEE single-precision floats, not 16.16: its vertices (a compact vertex's x, y and z; its fourth word stays 1/1024ths), its uniforms, and a laid texture's four words. A program built for the machine's float instructions (`docs/machine/fpu.md`) then converts nothing |
| `0x1000` tagged | a compact vertex's fourth word is u in bits 0-11 and v in bits 12-23 (1/1024ths, so within 4 repeats) and a tag in bits 24-31 |
| `0x2000` table | with tagged: word 11 is not a colour but the address of a table of them, a word each, and a vertex has the colour its tag names. A model whose vertices carry their normal's number is shaded by a table of the light at each normal, with no vertex written |
| `0x4000` packed | with tagged: a vertex is one word, x, y and z a byte each (whole numbers 0 to 255, which the matrix scales) and the tag in the top byte; u and v are a word a vertex in a second buffer, whose address is word 12. Quake's models are this in their files. Both buffers may be a file's bytes where it lies in the ROM (addresses from `0x40000000`): a model's pose is then drawn with nothing copied |
| `0x8000` points | with compact and the modelview flag: the buffer has a vertex a triangle, which faces the eye. Its corners are the point, then `s` above it and `s` to its right as the eye sees it; `s` is word 12, times 1 + word 13 x depth when the point is word 14 or more in front of the eye. All three corners have the vertex's texture coordinates |

| `0x10000` moved | word 15 is the address of two texels, each a row of three numbers (floats or 16.16 as the draw's are): u' and v' are each a row's dot with (u, v, 1). A texture that slides, turns or grows costs the program those six numbers a draw, and its vertices stay as they are |
| `0x20000` facing | with tagged: word 11 is the address of a table of the 256 ways a vertex can face, a texel each of three floats in the model's own space, and a vertex's tag is which. Word 13 is the address of three texels of floats: where the light comes from, its colour, and the colour of the light from everywhere. The vertex's colour is the last plus the second times how much it faces the first: a model is lit by those three texels and nothing is worked out for its vertices |

A textured quad is then 16 words of vertices instead of 96.

**Fragment modes**

| Mode | Colour |
|---|---|
| 0 | the interpolated vertex colour |
| 1 | a texture of `0x00RRGGBB` words, times the colour. Its address may be in the ROM, as mode 4's |
| 2 | a texture of bytes looked up in the display palette (`0x81000400`), times the colour |
| 3 | a texture of single bits, each row a whole number of bytes, leftmost pixel in the highest bit: set bits take the colour, clear bits are not drawn |
| 4 | a texture of three bytes a pixel (red, green, blue, as in a PPM file), rows not padded, starting at any byte, times the colour. Its address may be in the ROM (from `0x40000000`): the draw pass has the ROM's texture (`_Data_ROM`) as well as the state |
| 5 | a layer of tiles (below): the texture's address is four words that say where its cells, its tiles and its palette are |
| 6 | a picture as three planes of bytes one after another: width x height of luma, then the blue and the red chroma planes, each half the size each way (both sizes even). The colour is BT.601's, luma 16 to 235; times the colour. A video's frame as its decoder leaves it (`docs/apps/video.md`; `SEGL_YUV420` in the OpenGL library) |
| 7 | (text) four bits of coverage a pixel: the colour, its alpha times the coverage |
| 8 | (text) three distance fields a pixel, sampled bilinearly: the colour, its alpha times the coverage their median gives |
| +0x100 | texels equal to the key (a colour in modes 1 and 4, an index in mode 2) are not drawn |
| +0x200 | (draws) the texture is laid on the picture, not on the surface: a pixel at (x, y) of the picture takes the texel at words 12-13 plus (x, y) / 1024 times words 14-15 (16.16), and the colour is not applied. The surface still writes depth: a sky that hides what is behind it |
| +0x400 | smooth (modes 1, 2 and 4): the four texels round the point are weighed by how near their centres are (bilinear), the texture repeating. A texel of the key counts for nothing, and where such are most of the four nothing is drawn. `glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR)` asks for it of the bound texture; a texture starts as `GL_NEAREST` |
| +0x800 | lit (compact draws): the colour is times a light map. Word 12 is the address of a word a vertex, which says where in the map the vertex is (16 bits each way of the whole map); word 13 is the map's address, a texture of mode 4's kind, in RAM or the ROM; word 14 its width and, from bit 16, its height. Its four texels round the point are weighed, and beyond its edge the edge goes on. A lit wall is one draw in the opaque pass, where a second draw that multiplied (pass 3) also dimmed whatever was blended in front of the wall before it (docs/games/quake3.md) |
| +0x1000 | clamped: the texture does not repeat. Beyond its edge the edge's texels go on |

Textures are anywhere in RAM, sampled nearest and repeating, with coordinates 0..1 across.

**A layer of tiles** (fragment mode 5) is a texture whose pixels come from a grid of cells,
each naming a tile: a console's background, a map, a screen of characters. Its address is a
texel of four words: the address of the cells, of the tiles, of the palette, and tile width |
tile height << 8 | cells in a row << 16 | n << 28. A cell is 16 bits, as the Game Boy
Advance has them: a tile's number in bits 0-9, bit 10 mirrors it across and bit 11 down, and
bits 12-15 are a bank of 16 colours. A tile is width x height bytes, a tile after another;
the palette is words of `0x00RRGGBB`, and a pixel's colour is the palette's entry 16 x bank +
the tile's byte. With n not 0 the tiles are in pieces of 2^n tiles and their address is a
table of the pieces' addresses (a cartridge whose tiles are pages that turn). The texture's
width and height are the layer's in pixels; everything else is any texture's: a rectangle or
a draw of any part of it, repeating (which is a scroll that wraps), keyed on the tile's byte
(0x100), in any pass. It is not weighed (0x400). In the OpenGL library it is
`seglTexturePointer(four_words, width, height, SEGL_TILES)`, drawn by `seglQuad` or
`seglSprite`. The NES draws its picture so (`docs/games/nes.md`), which is its test: a held frame
against `tools\gpu_reference.py`, every pixel away from an edge the same.

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
  are drawn this way (`docs/games/doom.md`).
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
  light maps are kept commands that multiply (`docs/games/quake.md`).
- `seglScreenSpace()` and `seglScreenUsed()` draw triangles of whole vertices in the window's
  pixels, each with its own colour, blended over the frame in the order given: a user
  interface (`docs/apps/imgui.md`).
- `seglCompact()` draws vertices the program made compact itself; `seglCompactSpace()` gives it
  room in the frame to write them in place, and `seglCompactAt()` draws ones that already are in
  GPU memory, where they are, in any later frame too. `seglCompactTrim()` gives back room.
- `seglTagged()` and `seglPacked()` draw tagged and packed vertices, with a table of colours
  or without; `seglPoints()` gives room for points (`docs/games/quake.md`: models and particles).
- `seglMemorySpare()` is 3.9 MB more for one program: the display's own framebuffer
  (`docs/machine/display.md`), which nothing reads while the desktop is layers.
- `seglSwap()` waits until the list is drawn, with `pause` (now the rest call, below), which ends the machine's pass:
  what was left of that pass is not run (4,100 instructions a frame on average, 6% of Doom).
  `seglSwapEarly()` submits and returns at once, for a program that does other work next.
  The list, and every texture, kept vertex and palette entry it reads, must stay as they are
  until it is drawn: `seglSettle()` waits for that, and the program calls it before it next
  writes any such memory through a pointer. The library's own calls that write memory both
  sets share (the kept commands, the palette, deleting textures, the window's own texture)
  settle themselves. Doom swaps early, runs its tics, and settles when it begins to draw
  (`D_Display` in `doom_video.c`): the wait is over by then, and 93.4% of its passes'
  instructions became 99.8%. `glXSwapBuffers` (`gl.c`) never waited after its submit.
- A frame drawn on a worker core is swapped with `seglSwapHand()`: the worker leaves its set,
  command count and size in 16 bytes of its own, and the first core submits it with
  `seglSwapTake()`, which never waits. The lock word and the submit registers are the window
  system's too, and two cores have no atomic instructions between them: Quake's worker that
  swapped itself undid the window system's submits in the same pass, and its picture was
  copied into the window system's buffers. A worker's `seglSettle()` waits for its hands.
- `glXSwapBuffers` does not wait before its submit either, when the list the GPU still has
  is the program's own last frame: the GPU draws one list a pass, and a frame shorter than a
  pass waited for it every time. The newer frame takes the older one's place (the submit
  word taken back first, then the list's words, then the submit), as on a card whose
  program swaps without waiting for the display: frames drawn are one a pass, frames made
  as many as the program can. `GL_SWAP=wait` is the old way, every frame drawn; `GL_STATS=1`
  prints both counts every 1,024 frames. The caption's rate is of frames drawn.
- `seglSwapAgain()` draws the last list once more. Its textures, and the commands and vertices
  `seglLastCommand()` and `seglLastVertices()` pointed at, are the program's memory: a
  pointer that moved or a picture that changed needs no new list (`docs/games/tdawn.md`).
- A set holds 3,072 commands, 256 matrix blocks and 10,240 compact vertices of a frame's own.
- `seglFrameSpace()` is room in the frame that a program fills as it likes, another core
  of its too, and `seglScreenCompactAt()` draws tagged vertices in screen pixels that lie
  in it (`docs/apps/imgui.md`, "The copy on worker cores").
- Where the library waits for the GPU (`seglSwap()`, `seglSettle()`, a list of another
  program's in the way) the first core rests (`rest()` in `gles.c` and `gl.c`: the `REST`
  call of `/dev/gpu`, `docs/machine/multicore.md`, "A core that rests"), so that another program
  has what is left of the pass and `tools/tick_util.py` counts the pass apart. On a
  worker core's stack, on a kernel from before the call and with `GL_REST=pause` it is
  the pause hint, as it was. It is 322 instructions a wait where the pause was one.

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
  `/dev/gpu`. It maps GPU memory, `0x80000000` to `0x81b00000` (the control words are 16 MiB
  in), so buffers are written with plain stores. Its pages are mapped accessed and written:
  the machine's walk faults on a clear bit (`src/mmu.h`), and a worker's first store to a
  page stopped it until core 0 served it (`docs/machine/multicore.md`, "A game's speed"). One ioctl draws a list, copies the picture
  where asked and returns when that is done; it holds a lock, so several programs can use
  the GPU without stepping on each other's lists. An older ioctl only waits for a frame.
  Either way the kernel waits with `wfi`, which ends the emulator's frame. `/proc/gpu` has a
  line for each open of it (pid, workers, wanted, lent, tag, draws, lists drawn), which
  `nxmon` reads (`docs/system/nanox.md`).
- A program can also submit with no system call: it takes the lock word, writes the registers
  through its mapping and ends the frame with `pause`. The OpenGL library and Nano-X do this,
  and the library reads the time from the clock word instead of asking the kernel.
- Under Nano-X `glxgears` is a window: the library asks the server where the window's
  pixels are and has each frame copied there (about 500 frames a second at 300x300 on an RTX
  5090 with DXC, one emulator frame of about 8,000 instructions each). With no server running
  it takes the whole display, as before. On 19 October 2026 on an RTX 5070 laptop with D3D11 and
  fxc2 (`docs/machine/speed.md`), in the window it opens: 1,279 frames a second on the shader machine,
  1,122 on `rvc_lean`, 1,986 on the compute machine, 1,530 on the CPU harness.
- A frame of `glxgears` was 5,900 instructions and is 4,600 (13 October 2026; under
  `rvc_cpu` at 3 million a second 507 frames made a second and 650): a frame's matrix
  blocks and commands are written where the GPU reads them, not into arrays copied at the
  swap; the sine and cosine of an angle are kept by the angle (the view's three turns are
  the same every frame); the 16.16 product is the processor's two instructions (`mul` in
  `gl.c`: the compiler made four multiplies of a product of numbers it had just converted
  from floats); the window system is asked for events when a button or a key changed, while
  a button is held and four times a second, where it was every sixth frame; and the
  demo's milliseconds are 32-bit (`gears_time.py`). The numbers are the same numbers.
  Frames drawn stay one a pass: more frames made are more frames replaced before the GPU
  drew them (`GL_STATS=1`). On the shader machine its 5 s lines said 864 to 889 frames a
  second before and 1,178 to 1,209 after, 37% and 51% of them replaced. A window's new
  buffer is noticed by the machine's word for it (`REG_BUFFERS`), as `gles.c` does, not
  only by the event.
- Upstream's kernel has neither `/dev/gpu` nor `/dev/mem`. There the library falls back to
  an MTD device: the phram driver makes one for a physical range written to
  `/sys/module/phram/parameters/phram`, and the library uses `pread` and `pwrite` on it and
  spins while it waits.
- The GPU's memory must not be RAM the kernel uses, so the image's device tree starts RAM at
  `0x82000000`, above the devices, and ends it at `0x92000000` (`linux/nommu/image.py`;
  `tools/make_linux_image.py` also adds the binary to the root filesystem: `docs/system/nommu.md`, "Memory").
- The binary is static and links no C library: `programs/linux/libc.c` is a tiny runtime, and
  floating point comes from compiler-rt's soft-float routines, fetched at build time
  (`programs/linux/fetch.py`). Inside, the driver works in 16.16 fixed point.

## Checking it

`tools/gpu_reference.py SNAPSHOT TARGET.bmp` redraws the command list left in a snapshot's RAM
in software and compares it with the colour target saved by `--gpu-capture` (`--rom FILE`:
the ROM of another image than `build\images\linux`, for textures and vertices that are in it). Pause the guest
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

## RAM in one texture

The machine's state and RAM were two textures of 2048 x 9280 (290 MiB each), each commit
drawing one from the other. Now they are one, written in place (`RAM_INPLACE`,
`experiments/rvc_opt/src/ram.h`), and no pass reads the texture it draws: the D3D11 harness's
default (`--ping-pong` for the two; `--lazy` and D3D12 still have two) and the world's.
Beside it: the state rows after the tick (2048 x 64), a stage (2048 x 21) and the control
pass's rows. 580 MiB became 290 MiB and 2.9 MiB. (The texture is 8192 x 2368 since: "Rows of 8192", below.)

A pass, after the tick (which reads the state) and the unpack into the tick's texture:

- **The gather** (`ram_gather_geom`, points: one and one a core) reads from the state into
  the stage whatever the commit needs of RAM as it was: a copy's chunk, the texel of a core's
  store that found its cache full (merged with that texel's other words), the first and last
  texel of a fill or of a copy out of the ROM, the control words, the texels the picture
  copied back covers only in part, and the zap's square.
- **The state rows** (one quad) from the tick's texture and the stage.
- **RAM** as points (`ram_commit_geom`): the zap's square, each core's buckets, lone store and
  fill or copy, then the picture, in that order, the later winning where two write a texel.
  At start, point 0 is all of RAM from the image.
- **The control pass's rows** are drawn into a texture of their own and copied in: the
  harness's zones by `CopySubresourceRegion`, the world's (11 rows) by its `WindowBack` pass.

A fill and a copy out of the ROM read no RAM and are made whole in one pass. A copy from
RAM to RAM goes through the stage, 16,384 texels (256 KB) a pass, for one core a pass (of
the cores with one to make, the highest: it draws last in the gather); the core stays
stopped at `STALL_MEMOP_COPY`, its addresses and length moved on by a chunk, until the last
chunk is made. A destination inside its source ahead of it is copied from its end, so a
chunk never reads what an earlier one wrote (as `memmove`). `rvc_cpu` makes them the same
way, after the cores' pass (`copy_pass`; `RVC_COPY_STATS=1` prints how many there were and
how long cores waited).

`programs\copytest` (`tools\make_lazy_image.py DIR --program copytest --rom-kb 1024`, then
`rvc_harness --kernel mmu --image linux-net` in DIR) checks a copy of 4 KB, of 1 MB forward,
overlapping ahead and behind, from an odd address over three chunks, across and onto
`0x88000000` (where the commit's bands wrap: bit b is bands b, b + 32 and b + 64), 1 MB from the ROM, a fill of 2 MB, and a store beside a destination in its
pass: every line `ok` and `copytest: sum 0e7f76c3` on the card and on `rvc_cpu`. Before,
the card had the store beside it wrong and `rvc_cpu` the overlap ahead.

On the RTX 5090 (D3D11, fxc2), in turn with the tree before, from a cold boot: a shell loop
4.79 and 4.82 million instructions a second against 4.74 and 4.83; Doom's timedemo
(`-nodraw`, its seconds 20 to 32) 4.44, 4.72, 4.71 and 4.73 against 4.71, 4.62, 4.57 and
4.64. The commit, gather and both draws, is 0.023 to 0.039 ms a pass against 0.024 to 0.026.
A loop of 4 MB copies takes 16 passes a copy (0.020 ms of commit each) where it took one
(0.036 ms). Linux's own copies are pages: Doom's timedemo made 16 of up to 4 KB, a `cp` of
8 MB 4,096 of 4 KB, and no core waited for another's.

### A program's copy

A program asks for the same copy and fill at CSRs 0x8b0 to 0x8b3, the custom range a user-mode program
may use, which the tick and `rvc_cpu` take as 0x0b0 to 0x0b3 (`src/csr.h`, `CSR_UMEMOP_*`). Reading 0x8b0
says whether asking is worth it: 1 on the first core, 0 on a worker, whose copy from RAM would wait for
every other core's (one a pass), and 0 on a tick from before (it reads unknown CSRs as 0), so a program
built with it runs on either. (0x0b0 to 0x0b3 were never kept from user mode: their privilege bits are
a user's.)

The image's C library asks from 4 KB up for a copy and 2 KB for a fill (`linux/userland/memcpy.S`, `memset.c`, `uop.h`;
on the card a copy of 2 KB by words was faster, of 4 KB slower, 28 October 2026): a `memcpy`
whose two pointers lie at one place in a texel and a `memset`, both ranges in the GPU's memory
(0x80000000 to 0x81e00000) or in RAM (0x82000000 to 0x92000000), copy or fill the bytes up to the
first texel themselves, ask for the whole texels and do the last bytes in the next pass. The texels
never enter the core's write cache; asking ends the pass, as the kernel's does. `memmove` without an
overlap is a `memcpy`. On `rvc_cpu` (`RVC_NO_UOP=1`: 0x8b0 reads 0, as a machine without) Quake's
timedemo (`quake +timedemo demo1`, from its command to its fps line, `RVC_WCACHE=1`, 28 October 2026):

| | core 0 (M instructions) | passes | core 0's passes ended full | guest time (s) |
|---|---|---|---|---|
| without | 112.90 | 18,266 | 4,223 | 39.14 |
| with | 107.35 | 17,015 | 2,650 | 37.40 |

`programs\copytest`'s case 10 (after its sum, which stays `0e7f76c3`) is such a copy with a byte
stored beside it in its pass; `-DPROGRAM_COPY_ONLY` builds it alone for a quick look on WARP.

## Rows of 8192

Since 29 October 2026 the state texture is 8,192 texels wide and 2,368 high, where it was 2,048 by 9,280, and the
ROM's texture is as wide (`src/layout.h`: `RAM_W`, `RAM_ROW`, `STATE_TEX_HEIGHT`, `STATE_BLOCK_W`, `RAM_BAND_ROWS`;
`harness/apps/rvc_backend.h`: `kStateWidth`, `kRamRow`, `kBlockWidth`, `kBandRows`, `ramX`, `ramRow`):

| | before | now |
|---|---|---|
| the state texture (texels) | 2048 x 9280 | 8192 x 2368 |
| RAM's texel t | (t % 2048, 64 + t / 2048) | (t % 8192, 64 + t / 8192) |
| a row of RAM | 32 KB | 128 KB |
| a 4 MB band | 128 rows | 32 rows |
| the ROM's texel t | (t % 2048, rows - 1 - t / 2048) | (t % 8192, t / 8192) |
| the ROM's texture (texels, the 269 MiB of 28 October) | 2048 x 8601 | 8192 x 2151 |
| video memory of the state (MiB) | 290 | 296 |

- **What stays.** The cores' state is the block it was, 2048 x 64 at the left of rows 0-63 (core 0's 64 x 32, the
  workers' tiles in bands of eight rows, the lanes' field in rows 48-63): the tick, its targets and its geometry are
  as they were, and one core a wave holds. The rest of those rows, 6,144 x 64 (6 MiB), is no one's: the state commit
  draws the block alone (`commit_vert` in `main.shader`, `state_geom` in Unity's `Machine.shader`), and a pixel there
  of a whole-texture draw is zero. The tick's texture after the tick (2048 x 64), the stage (2048 x 21, `STAGE_W`:
  a copy's chunk is still 256 KB) and Unity's window (2048 x 11) are as wide as before.
- **The addresses.** RAM's texel is a mask and a shift of the address, as before (8,192 is a power of two: `RAM_ADDR`
  is `t & 8191` and `t >> 13`). The ROM's was a division by the texture's width, read at run time
  (`GetDimensions`), and a row counted from the bottom: it is a mask and a shift now, in the tick, the commit, the
  GPU's `texture_word` and the lean tick, with no `GetDimensions` for it.
- **The bands.** A band is still 4 MB, 32 rows now, and `ram_bands` is the mask it was: bit b for bands b, b + 32 and
  b + 64, 72 bands, the control words' 4 and 5 drawn every commit. The commit's quads (`kCommitQuads`) are as many.
- **The device rows.** The control words (0x81000000) and the counters' totals (0x80c10000) are 2,048-texel runs of a
  row: the control words the first quarter of row 192, the totals the third quarter of row 160; the network's and the console's run (0x816b8000) is the last quarter of row 245, and a fetch's 256 KB
  (0x816c0000) are rows 246 and 247 whole. The harness's zones and readbacks take their column with their row
  (`RAM_ZONE`, `kNetX`, `kControlX`), Unity's window holds the same eleven runs of 2,048 as before and puts each where
  it lies (`window_state_pos`, `window_of` in `MachineWindow.cginc`), and `Readback.shader` and `PersistRead.shader` read
  them so.
- **The views.** The memory view (`harness/core/memview.cpp`, Unity's `MemHeat.shader`) still shows RAM in rows of
  2,048: a pixel's texel index is made from its place, then read where the texture has it. The danger panel's laser
  aims at the same picture and sends the texture's column and row (`EmuDanger.Point`); a square zap is a square of
  the texture, 8,192 texels a row (`_ZapX` up to 8,191, `_ZapY` a row of 128 KB).
- **Snapshots.** `rvc_harness --save-state` writes `SX86SNP3` (8192 x 2368). One of rows of 2,048 (`SX86SNP2`, the
  address layout of 28 October) loads: its state rows go to the block, its RAM texels one after another from row 64,
  which they were in either. The tools read the width from the header (`snapshot_screen.py`, `gpu_reference.py`,
  `sound_reference.py`, `dxbc_run.py`).
- **The ROM** in the world is made by `ShaderEmuTexelsImporter` (version 2), 8,192 texels a row from row 0; the
  harness makes it so from `rootfs.bin` (`createTextureTexels`, D3D12 and `rvc_trace12` too).
- **Not ported**: the compute processors (`--compute`, `RVC11_COMPUTE`; `experiments/rv32_min`, `experiments/rvc_compute`)
  lay RAM out in rows of 2,048 and are refused by the harness.

## The ROM as one texture

The ROM (the image's `rootfs.bin`) was four RGBA8 textures, one for each word of a texel
(`_Data_MTD_R/G/B/A`, PNGs made from the file). It is one RGBAFloat texture now, `_Data_ROM`:
8192 texels a row from row 0 (2048 a row and texel 0 in the last row until 29 October 2026: "Rows of 8192"),
a 16-byte texel a pixel read back by its bits
(`asuint`). The tick, the commit, the gather, the GPU's draw (`texture_word`), the sound, the
lean tick and the world's shaders take a word as `asuint(_Data_ROM[p])[k]`: one load where a
load of the ROM was a switch over four textures and an unpack, and a copy out of the ROM one
texel load where it was four.

- The harness makes it from `rootfs.bin` itself (`createTextureTexels` in `harness/core/image.cpp`;
  D3D12 and `rvc_trace12` the same way).
- In the world it is an asset made by a ScriptedImporter (`unity/ShaderEmu/Editor/ShaderEmuTexelsImporter.cs`,
  files named `*.texels`: `ShaderEmuImages` copies the ROM in as `NAME_rom.texels`). A `UInt`
  texture cannot be a `Texture2D` in Unity 2022.3; an RGBAFloat one keeps every byte as the file
  has it as long as nothing filters or converts it (point filter, linear, no mipmaps). The
  importer's `Mismatches` read every byte back in the world's editor on 28 October 2026: 0 of
  393,240 bytes differed in a file of NaN patterns, infinities,
  denormals and -0, and 0 of 281,828,352 in the ROM (2048 x 8601).
- The Udon model still reads the ROM from lanes: "ShaderEmu/Import the ROM's lanes for the Udon
  model" makes them. The device tree is still four lanes (a row).

## Blits in place of cameras

In the world every round's pass but the tick is a blit (`VRCGraphics.Blit`): the commit's gather and RAM points
(`MachineCommitBlit.shader`) and the GPU's lists (`GpuDrawBlit.shader`) were meshes of points drawn by two cameras,
and a camera's `Render` is some 0.13 ms of the main thread where a blit is next to nothing. The tick stays a camera's: it draws eight targets at once.

- **Points from a blit** (`experiments/rvc_opt/src/amp.h`). A blit's two triangles are two patches of three points;
  the hull shader asks for a quad domain at factor 8 (never more: a driver may clamp a larger factor), 81 points a
  patch, and a geometry shader of `AMP_INST` instances makes a call of each point and instance: 5,184 calls a blit
  at 32. The calls make the meshes' points and triangles. The hull shader returns no control point by its index
  (fxc2 before patch 64 wrote `I[id]` as a temp the card did not draw, and the device was removed), and every
  shader of patches includes `src/fxc2_check.h`, which refuses an fxc2 older than 65.
- **In the meshes' order.** Where two points write one texel the later wins (the zap's square, then each core's
  buckets and lone store, then its fill or copy, then the picture; the gather's highest core with a copy), and the
  GPU's triangles blend and are tested in their order. The patches come in order and so do a point's instances,
  but not a patch's points: the tessellator emits them in an order of its own, and it is not every card's. Measured
  by stream output (28 October 2026): WARP goes from the corner (u 0, v 1) round the outer ring and inwards ring by
  ring; an RTX 5090 emits the 49 inner points row by row first and the ring after. So the order is measured on the
  card: `BlitOrder.shader`'s first pass is a blit into an 81 x 81 target in which point i draws texels (i, j) and
  (j, i), i + 1 in each, so that (i, j) ends as the later of the two; its second counts, for each point, the points
  drawn before it (an 81 x 1 target, `_AmpRank`). A call's number is `(patch * 81 + rank) * AMP_INST + instance`,
  in the order the card draws. EmuMachine blits both every frame before its rounds; the harness draws them once
  and checks that the ranks are 0 to 80 once each.
- **The gather**: a point a call, `AMP_INST` 2 (324 calls for 257 points). **The RAM commit**: nine points a call,
  and the last point (the picture, up to 20 vertices) a call of its own. A texel the commit writes alone is one
  triangle of three vertices (`ram_tri`: its corner and 1.5 texels along each side) where it was a quad of four, so
  that nine points (nine buckets of three entries, or a fill's three rows, a lone store and seven buckets) are at
  most 81 vertices, under the 85 that 1,024 values of `ram_v2f` allow. `AMP_INST` 8: a blit is 1,296 calls, and four
  blits (`_AmpFirst` 0, 1,296, 2,592 and 3,888) are 46,000 points; the mesh had 41,346. At 32 instances, one blit,
  WARP compiled the geometry shader for more than 18 minutes (1 instance 1.3 s, 8 4.6 s, 16 24.5 s). **The GPU's lists**: 13 of the device's triangles a call (39 vertices of 20 values), 67,392 triangles; pass 8
  clears the depth to far first, as the camera's clear did, then the 8 passes are a blit each into the GPU's target
  and its depth buffer.
- `EmuMachine.oldPaths` bit 5 is the points' camera again, bit 6 the GPU's (both: no order probe either), for a
  before and after in one session. The meshes and both cameras stay in the scene for that.
- In the harness `--blits` draws the gather, the RAM commit and the GPU's lists so (`gatherAmp_`, `ramAmp_`,
  `gpuAmp_` in `rvc_backend11.cpp`, the probe from `experiments/rvc_opt/amp.shader`), for WARP and the card;
  `tools/test_card.py --with=--blits` runs the cases with it.
