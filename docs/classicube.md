# ClassiCube

ClassiCube, the open client for Minecraft Classic, runs in the Linux image as a Nano-X client:
a 64 x 64 x 64 world it generates at start, to walk, dig and build in. Everything on the screen
is drawn by the machine's GPU. It was chosen because a frame of it is almost all drawing: the
world is chunks of quads that stay in GPU memory, and the CPU works when a block changes.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh        (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/classicube/build.sh   (6 s; the first time it also fetches)
    python tools\make_linux_image.py                                     (then a new shell snapshot)

In the guest: `nano-X -p & classicube` at the console, or ClassiCube in the Start menu's Games
(not tried from there). It starts in single player. W, A, S and D walk, Space jumps, the left
button digs and the right one builds; the pointer's travel over the window turns the view, and
so do the arrow keys, since the pointer is not held in the window. Escape is the game's menu.
Its textures are the game's own (`misc/cc_textures.zip` of its repository), so the build
fetches nothing but the source.

Lines typed at the terminal it was started from drive it, which is how a test reaches it
through the machine's serial console:

    down KEY / up KEY     a key or button, by the game's names (W, Space, Right, LeftMouse, RightMouse)
    turn DX DY            the pointer's travel, in pixels
    point X Y             the pointer's place in the window
    hold                  the frame after next stays ("classicube: holding frame N")
    anything else         said in chat (the game's own commands start with /)

## What the build is made of (`linux/classicube`)

- `build.sh` fetches the source at a fixed commit, applies `classicube.patch` and compiles every
  file of the game with `PLAT_SHADEREMU`: each leaves itself out by the platform's defines.
- `Graphics_ShaderEmu.c` is the game's graphics backend, written on the GPU device itself
  (`docs/gpu.md`), not on the OpenGL library: command lists, matrix blocks and GPU memory are
  its own.
- `Window_ShaderEmu.c` is the Nano-X window, the keys and the pointer, and the terminal's lines.
- `Math_ShaderEmu.c` is sine and cosine in floats: the game's own work in doubles.
- `store_zip.py` rewrites the texture pack with nothing compressed, at build time: inflating
  the zip and its PNGs was 7 seconds of every start.
- `classicube.patch` (`git diff` of the tree at `build.sh`'s commit): the platform in `Core.h`
  (low-memory build, no launcher, no FreeType, no sound), a vertex colour as the device has
  them, the map generator's noise in floats, one chunk built a frame, the chunk builder's
  short cuts (below), water's and lava's animation at 5 and 2.5 pictures a second, random
  block ticks at a quarter of the rate, the map's borders in quads of 16 blocks, a view
  distance of 128 and a seed from `CLASSICUBE_SEED`.


## Meshes from a worker core

A chunk's mesh is some 100 to 200 thousand instructions, and the game builds one a frame
while it has any to build: 15 seconds at 4 frames a second after the world is made. On a
machine with worker cores (`docs/multicore.md`) a second builder runs on one:

- `Builder.c` is compiled twice (`build.sh`: `-DSE_SECOND`, its three public names renamed).
  The second copy has every buffer of the first as its own, takes eight chunks at a time
  that have never had a mesh, and builds them into memory of its own; it makes no vertex
  buffer and writes nothing of the chunks'.
- The first core goes on building the nearest chunk each frame, and when the worker's eight
  are done makes them the chunks' own (`SE_Mesh_Collect` in `MapRenderer.c`): the parts'
  counts, and the vertices packed into GPU memory as any chunk's are (`Gfx_SE_UnlockFrom`).
- A chunk that is made again (a block changed) stays the first core's, which draws it by its
  old mesh until the new one is there. Anything that makes chunks stale while the worker has
  some (`se_mesh_serial`) throws its eight away, and whatever frees the chunks or the blocks
  waits for it first (`MapRenderer_SE_Settle`).
- The build gives every variable its own 16 bytes, as Quake's does, and the game touches all
  of its data once before the first job (`mcw_touch`), or the worker would stop at each page
  of its own buffers until the next frame.

The world's making has two steps that are noise by the column, a third of it: the height
map and the strata. Both are shared out by rows of z between the first core and up to three
workers (`SE_GenShare` in `Generator.c`; the game asks for the workers on its own thread
before the generator's begins). The rest of the making draws from one run of random numbers
and stays in order.

From the command to the whole world drawn: 38.0 s on four cores (40.3 s on two, where only
the meshes have a worker), 52 s with `CLASSICUBE_MESH=inline` (the first core alone); the
finished world is the same 83 commands and 6,980 quads.

## How it is drawn

- **A chunk is packed vertices** (`VERTEX_PACKED`): a word a vertex, bytes of x, y and z in
  eighths of a block from the chunk's corner, with the texture coordinates a word beside it:
  32 bytes a quad. Each chunk has a matrix block that puts it in its place. The colour of a
  vertex (a face's shade, in sun or in shadow) is a tag into one table that all chunks share.
- **The terrain's textures are four across.** A packed vertex has 12 bits of u in 1,024ths,
  four repeats, and a run of 16 equal blocks is one quad that repeats its tile 16 times. So
  the atlas the chunks read is the game's (a strip 16 texels wide) written four times side
  by side, and u is a quarter of the game's.
- **Other buffers are compact vertices**, a texel each: with the command's colour when all of a
  buffer's vertices have one, with tags and a table of the buffer's own colours otherwise. A
  quad's texture coordinates are moved to start in the texture's first repeat.
- **A buffer filled and drawn again and again in one frame lives in the frame's set** (the 2D
  screen's quads, the models); one drawn frame after frame as it is gets GPU memory of its
  own on its second frame, and costs nothing after.
- **Passes**: what tests depth is pass 0, or pass 1 when it blends (water, which the game
  draws twice, depth first: here that first draw is left out, since pass 1 writes no depth).
  Everything drawn without depth (the 2D screen, the block in the hand) is pass 5, blended,
  so that it keeps its order. The alpha test is a key: a texel with no alpha becomes wholly
  transparent black when its texture is copied, and pass 0 leaves those out.
- **GPU memory** is the 3.3 MB a GL program has and, while the desktop is layers, the
  display's own framebuffer (3.9 MB), handed out in spans; a span given back is free two
  frames later, so that the list being shown never reads freed memory.

Not as the game draws elsewhere: no fog (the far chunks end at a line), clouds that stand
still, no lines (the block outline is the game's quads, so it is there), nearest texels, and
no smooth lighting.

## Speed

Instructions a frame in a 854 x 480 window, standing where the game starts with seed 7. The
machine runs 3.9 million a second on D3D11 with fxc2:

| | A frame | Frames/s |
|---|---|---|
| the first build that drew | 235k | 10 |
| the generator's noise and sine in floats, a view of 128 blocks | 110k | 20 |
| texels copied in runs, no frame rate line, animations and random ticks slower | 48k | 70 |
| walking | 58k to 62k | 55 to 60 |
| digging without pause | 148k | 25 |
| building without pause at the top of a shaft | 1,200k | 3 |

- A frame standing still is about 85 draws and 7,000 quads. A draw is some 130 instructions.
- **A changed block builds its chunk again**, one chunk a frame. That was 1,000k instructions
  and is 550k: a row of blocks that is all rock between rows of rock, or all air, is passed
  over whole, a block of rock among six is passed over, and the bookkeeping walks the four
  atlases a map uses instead of all 512. So a block dug is one frame of 0.15 s. A block put
  at the top of a deep shaft changes the light of every chunk under it, and each of those is
  such a frame: that is the last row of the table.
- Water flowing after a dig goes on changing blocks, a chunk each time.
- From the command to the world standing is about 30 seconds: half of it before the first
  frame (the game's own start, the texture pack's pictures), 3 the generator and 12 the 64
  chunks, at five to ten frames a second. It was 100.

`ccstat:` lines, every five seconds, give frames, instructions a frame, draws, quads, and
the texels and vertices made for the device.

    CLASSICUBE_SEED=N        the same world every time
    CLASSICUBE_HOLD=N        keep frame N on the screen and stop there
    CLASSICUBE_STATS_MS=N    how often the ccstat: lines come (5000; 0: never)
    CLASSICUBE_SIZE=WxH      the window's size (854x480)
    CLASSICUBE_DEFINES=-DSE_COUNT   (to build.sh) a chunk's instructions by phase, in the ccstat: lines

## Checking it

- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run
  `python tools\gpu_reference.py SNAP BMP --size 854x480`. At the start 99.87% of pixels away
  from triangle edges are identical. The model cuts triangles at the near plane now, as the
  card does: before, it left out every triangle that crossed it, here the sky and the borders.
- A change to how it draws or builds chunks: hold the same frame of the same seed before and
  after and compare the GPU's targets. Two runs of one build give the same picture.
- To see where instructions go, snapshot a run at `classicube: steady` and resume it with
  `--pc-log` (`tools\pc_profile.py`, `classicube=classicube.nm`).

## Not done

- Playing it by hand: the window's keys and pointer were written as Quake's are and never
  pressed; only the terminal's lines were tried. The world's beams are a pointer, not a
  mouse that is held: turning by them wants a look.
- Sound (the game's WAVs are in its repository; the sound card could play them from the ROM).
- Fog, and clouds that move.
- A world larger than 64 x 64 x 64 (the low-memory build's), and one kept from run to run:
  a saved map goes to `/tmp`.
- The VRChat world: "Import boot images" after this image, and the board's list of programs.
- `linux\prebuilt` does not have it.
