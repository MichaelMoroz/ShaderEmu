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

## Smooth textures

The game asks for `GL_LINEAR` of the level's textures, its light maps, the models' skins, the
sky and its pictures, and for `GL_NEAREST` of the console's letters, and is given both: the GPU
weighs the four texels round a point for a texture so marked (`docs/gpu.md`, fragment flag
`0x400`; `qglTexParameterf` in `qgl.c`). The light is what shows most: a light map's texel is
16 units of wall, and was a square of one brightness. There are no smaller copies of a
texture for far walls (mipmaps), so those shimmer as they did. It costs the machine nothing
that can be measured (the same frames a second on e1m5), the picture agrees with
`tools/gpu_reference.py` within 2 levels at every pixel, and `QUAKE_FILTER=nearest` is the
picture as it was.

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
    QUAKE_FILTER=nearest  every texture's nearest texel, as before the GPU could weigh four
    QUAKE_STATS_MS=N    how often the quakestat: lines come (5000; 0: never)
    hold                the console command: the frame after next stays
    -nokeep             draw the level a polygon at a time, as before it was kept
    QUAKE_DEFINES=-DSE_COUNT    (to build.sh) counts of the server's work in the quakestat: lines

## The server on a worker core

A frame of a single-player game is the server's (physics, the monsters' QuakeC) and then the
client's (the picture). On e1m5 the server's part was 355 of a frame's 515 thousand
instructions. On a machine with worker cores (`docs/multicore.md`; `--cores 2` in the harness)
the server's physics runs on one, as a function of the game in the game's own memory
(`linux/quake/server_shaderemu.c`), and the two go at their own pace:

- **The client does not wait.** When the server's frame is still out, the client's frame goes
  on without one: it draws from the server's last two answers, moving things between them,
  which is what Quake's client does in a network game (the game only switched that off for a
  server in the same program). When the server's frame has ended its messages are sent, and
  the next is begun with the moves the client made meanwhile: at once, from a few places in
  the client's frame (`SE_ServerPoll`), not at the client's next frame.
- **The frame's messages are made on the worker too** (what the client is told: the entities
  it can see, 40 thousand instructions). They are kept there, and the first core puts them
  into the connection when it takes the frame, because the client reads its end meanwhile.
- **The server's frame has its own length** (`sv_frametime`: the time since its last one began,
  0.1 s of the game at most, as a frame of the game always was). Where that limit holds, the
  game's time goes slower than the clock, as it did before, and the client's time goes at
  that rate between the server's frames: at the slower of the last two frames' rates and a
  tenth slower still. At the rate of the last frame alone the client was at the newer
  frame's time early whenever the next took longer, and in one frame of eight nothing it
  drew moved, the player's own view included, which in play looked like being stuck for a
  moment. Now no frame stands still (the quakestat: line counts them); the client is a
  little behind when the next frame comes instead, and is taken up to it.
- **The server keeps to its own data.** Console lines, commands and cvar changes that QuakeC
  makes on the worker are kept and done by the first core afterwards; an error there ends
  the job and is raised on the first core; the server's random numbers are its own (the C
  library's have one state, which the client uses too). A console command that may be the
  server's (anything but the keys' and the like) waits for the server's frame to end.
  Loading, leaving and demos are on the first core, as they were.
- **Every variable has its own 16 bytes** (`-fno-common -fdata-sections`, and the sections
  aligned by `objcopy` in `build.sh`): two cores must not store to the same 16 bytes in one
  pass of the machine, and a variable of the server's beside one of the client's would be
  that.

On e1m5, standing where the level starts, in real time (RTX 5090, harness, D3D11):

| | Frames a second | Server frames a second | The first core's frame |
|---|---|---|---|
| server on the first core (`QUAKE_SERVER=inline`, or no worker cores) | 4.85 | 4.85 | 515 thousand instructions |
| server on a worker, the client waiting for it (`QUAKE_SERVER=wait`) | 6.3 | 6.3 | 211 thousand, and 9 passes of waiting |
| server on a worker, each at its own pace (the default) | 13.3 | 5.9 | 153 thousand |
| the same after the client's frame was gone through (water, sky, leaves, models' light: 117 thousand) and the machine's float loads and stores were put on its fast step | 22.2 | 7.5 | 115 thousand |

Of 54 frames drawn, 47 are between two of the server's. The server's frame is the 330
thousand instructions it was and decides how fast the game's time goes (0.1 s a frame at
most: 0.59 of the clock's rate here, 0.49 with the server on the first core). A pass of the
machine takes 6.5 ms while Quake runs, against 5 for integer code: floats, loads and stores. A pass of the machine with a busy
worker costs about 5% more than without, which those figures include.

    QUAKE_SERVER=inline   the server on the first core
    QUAKE_SERVER=late     on the first core, its messages sent at the end of the client's frame
    QUAKE_SERVER=wait     on a worker, the client's frame waiting for it at its end
    QUAKE_SUM=N           a sum over every entity's fields each N frames of the server
                          (but the world's model and the players' names, whose places depend on the heap)
    QUAKE_NET=1           what a packet costs, every 200 sent (the game has its UDP driver: docs/lan.md)
    QUAKE_TRACK=N         where the player is and how fast, each N frames of the server
    QUAKE_TEST_EXIT=F     (with QUAKE_TRACK) at frame F the player is put into the level's exit

`late` and `wait` do the same things in the same order, one on a core and one on two. With
`+host_framerate 0.05` (a frame's length is then not the machine's speed) and `--fixed-dt
0.004`, `QUAKE_SUM=10` must print the same sums with `inline`, `late` and `wait`: that is the
check that the server on a worker computes what it computed on the first core. The default
cannot be checked so (how many frames the client draws to one of the server's is the
machine's speed); it runs the same job.

Play is checked with keys typed as console commands: `QUAKE_TRACK=5` and `+forward` after
"track 30:" must take the player the same way in every mode, and `QUAKE_TRACK=5
QUAKE_TEST_EXIT=40 quake +map e1m1` with `+attack` after "track 80:" must end the level, load
the next and go on printing where the player is. It did not at first: with the server's
messages sent late one was still unread when the level changed, the connection takes one
reliable message at a time, the "reconnect" could not be sent, and the client read the new
level in the old one's state (`i >= cl.maxclients`). The client now reads what it was sent
before the server is replaced (`SE_ServerDrop`). And a button counts as down for a frame of
the server's if any of the client's moves since the last had it down: the server took the
last move only, and a tap between two of its frames (a jump, a shot) was lost.

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
