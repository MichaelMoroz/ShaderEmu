# Quake

Quake (1996) runs in the Linux image: id Software's released source, GLQuake, as a Nano-X
client. Everything on the screen is drawn by the machine's GPU; there is no software renderer
in the build. The machine's float instructions (`docs/fpu.md`) were added for it.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/quake/build.sh     (3 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & quake` at the console, or Quake in the Start menu's Games (not
tried from there by hand). With no arguments it plays its three demos in turn, as Quake does,
and Escape brings its menu up, where New Game is; the Start menu starts it with the menu up
already (`quake +menu_main`). `quake +map e1m1` starts the first level. The data is the
shareware episode's `pak0.pak` (version 1.06), which the build fetches and the repository
does not keep.

Lines typed at the terminal it was started from are console commands (`map e1m2`,
`timedemo demo1`, `hold`), which is also how a test drives it through the machine's serial
console. The window's keys and buttons go to the game as Quake's own (`vid_shaderemu.c`), and
`in_mouse 1` has the pointer's travel over the window turn the view; neither has been tried
by hand yet, only the terminal's commands.

## What the build is made of (`linux/quake`)

- `build.sh` fetches the source at a fixed commit, applies `quake.patch`, adds the files
  below and compiles GLQuake's file list without its x86 assembly, with the null sound, CD
  and network drivers. Floats are the machine's instructions, constants are single precision.
- `qgl.c` is the OpenGL GLQuake calls (`include/GL/gl.h` renames each call to `qgl...`): the
  1.1 calls it uses, glBegin and floats, turned into what `programs/linux/gles.c` (its float
  build) draws. It also keeps the textures' pool, the models' poses and their light tables.
- `world_shaderemu.c` is the level and the models (below).
- `mesh_shaderemu.c` stands in for `gl_mesh.c`: a model's triangles as they are, where GLQuake
  searched each model for long strips.
- `fmath.h`, `fmath_shaderemu.c`: sine, cosine, tangent and arctangent in floats. The source
  calls C's double functions, and the C library's float ones work in doubles inside.
- `vid_shaderemu.c` is the window, the frame and the input; `sys_shaderemu.c` the system.
- `snd_shaderemu.c` is the sound (below), in place of the game's mixer.

## Sound

The machine's sound card plays it (`docs/sound.md`): a sound is a voice pointed at its WAV
where the pak file lies in the ROM, with the WAV's own loop point, and nothing is loaded or
mixed by the CPU. The game's rules stay: which of eight channels a thing's sound takes, and
how loud it is in each ear by distance and side, worked out once a frame.

- A level's own sounds (torches, hums) are loops with a place. The 18 loudest that are heard
  have a voice each; one that is out of hearing gives its voice up.
- Water and sky are two more voices, as loud as the viewer's leaf says.
- Its level is half the game's own: a fight's mix ran into the limit in 1.5% of its samples
  at the game's level, and does in 0.1% at this. `volume` is the game's setting, as before.
- No music: the game's was a CD's tracks.
- To check it: `--fixed-dt 0.004 --sound-capture F.wav --save-state F.snap` on a timedemo and
  `python tools\sound_reference.py F.wav F.snap --rom build\images\linux\rootfs.bin`. A minute
  of demo1 is 3,072,768 samples, every one the model's.

## How it is drawn

- **Textures stay palette indices.** GLQuake's 3dfx path for 8-bit textures is the one used,
  without its rescaling to powers of two and without mipmaps: the GPU looks each texel up in
  the palette, and index 255 is a hole where the texture has any.
- **The level is kept in GPU memory** (`seglKeep`, `docs/gpu.md`), as Doom's is: its polygons
  are made into quads when it loads, sorted by texture, and once more sorted by light map,
  and a frame says which runs of them to draw. The light maps are a second draw that
  multiplies (pass 3) with the light map as a texture of grey words.
- **What is in view** comes from the level's own table of which leaves see which (its PVS):
  the leaves the viewer's leaf sees are listed when the viewer enters a leaf and tested
  against the view's four planes in integers. GLQuake walked the whole BSP tree every frame.
- **The sky** is two kept commands over the same vertices, their texture laid on the picture
  (`FRAGMENT_LAID`), and water is drawn a polygon at a time with the game's clock.
- **A model's pose is the file's own bytes.** A vertex of a model is four bytes (x, y, z and
  which of 162 normals); a pose's are copied into GPU memory as they are and drawn as packed
  vertices, with the model's texture coordinates in a buffer beside them. Its shading is the
  same vertices drawn again, multiplying, each coloured from a table of the light at every
  normal (`VERTEX_TABLE`): a table a light and a turn, 128 of them kept.
- **A particle is one vertex** (`VERTEX_POINTS`): the GPU makes GLQuake's triangle of it, and
  its size. Its colour is a palette index: a 16 x 16 texture holds every index once.
- **A flash** is the glow GLQuake draws by default (`gl_flashblend 1`), a square of a round
  picture, added. Lighting the walls with it instead made their light maps again each frame.
- **The 2D screen** (status bar, console, menus) is rectangles of textures in the one blended
  pass with no depth (`seglSprite`), so that it keeps the order it is drawn in.
- **GPU memory is a cache.** Every texture is kept in ordinary memory as the GPU will read
  it; GPU memory holds the ones being drawn, the level, and the poses and tables, and what
  was drawn longest ago makes room (a pose first). It is 7.2 MB: the 3.3 MB a GL program has
  and the display's own framebuffer, which nothing uses while the desktop is layers
  (`seglMemorySpare`). A level and a fight use 2 to 4.5 MB of it.

Not as GLQuake draws: no model shadows, no wobble of the world seen from under water, no
depth trick for the weapon (it can poke into a wall), nearest texels everywhere, and a
model's light in steps of 1/32.

## Speed

A frame standing still at the start of the first level, in instructions:

| | A frame |
|---|---|
| GLQuake as it is, floats in software | 3,430k |
| the level and models in integers | 1,810k |
| the machine's float instructions | 752k |
| the GL library in floats, our own sine | 576k |
| the level kept in GPU memory | 390k |
| the game's clocks as floats | 263k |
| poses and particles as the GPU takes them, 2D by `seglSprite` | 195k |

The machine runs 3.7 to 3.9 million instructions a second (D3D11, fxc2), so 250k a frame is
15 frames a second. `quakestat:` lines, every five seconds, say where a frame went:

| | A frame | Server | In view | Level | Models | Particles, weapon | 2D, swap | Frames/s |
|---|---|---|---|---|---|---|---|---|
| e1m1, at the start | 195k | 111k | 40k | 1k | 4k | 3k | 19k | 19 |
| the start map | 221k | 64k | 43k | 40k | 30k | 8k | 19k | 17 |
| demo1 (e1m3), walking | 205k | 0 | 26k | 1k | 59k | 21k | 45k | 18 |
| demo1, a fight | 296k | 0 | 30k | 1k | 93k | 73k | 26k | 13 |
| e1m5, at the start | 509k | 356k | 35k | 34k | 35k | 5k | 18k | 7.5 |

- A fight has about 11 models and 400 to 700 particles in view. A particle is some 70
  instructions, most of them the game moving it.
- e1m5 is the game's server: its monsters' moves and looks are traces through the level
  (point tests 11%, the QuakeC interpreter 11%, line traces 8%). Nothing there is drawing.
- "Level" is light maps made again: surfaces whose light flickers, ten times a second.
- The game slows down rather than skipping when a frame is longer than a tenth of a second,
  as Quake always has.

From the command to a level's first frame is about 30 seconds, and of a level's own loading
half is its light maps being placed (`AllocBlock`, a third) and the kept level being built.

    QUAKE_HOLD=N        keep frame N on the screen and stop there ("quake: holding frame N")
    QUAKE_STATS_MS=N    how often the quakestat: lines come (5000; 0: never)
    hold                the console command: the frame after next stays
    -nokeep             draw the level a polygon at a time, as before it was kept
    QUAKE_DEFINES=-DSE_COUNT    (to build.sh) counts of the server's work in the quakestat: lines

## Checking it

- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run
  `python tools\gpu_reference.py SNAP BMP --size 640x480` (the picture is the window's size,
  not the display's). On the start map 99.996% of pixels away from triangle edges agree. In a
  demo's fight it is 96.6%: the model drops triangles that cross the near plane and the card
  clips them. Of the pixels the models and particles draw there, none and 9 of 2,022 differ.
- A change to how it draws: hold the same frame before and after and compare the window's
  pixels, which are the display's second layer in the snapshot. `+host_framerate 0.05` makes
  a level's frame N the same game time each run; `timedemo` does for a demo.
- To see where instructions go, snapshot a run at a `quakestat:` line and resume it with
  `--pc-log` (`tools\pc_profile.py`, `quake=quake.nm`).
- `linux/quake/quake.patch` is `git diff -- WinQuake` of the tree at `build.sh`'s commit.

## Not done

- Music, and hearing the sound by ear: it was checked against the card's model only.
- Playing it: keys and the pointer by hand, in the harness's window and in the VRChat world.
- The server: a busy level is 350k instructions a frame before anything is drawn.
- Loading: the light maps' placement is a search of every block for every surface.
