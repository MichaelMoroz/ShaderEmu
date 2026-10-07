# Doom

The Doom that comes with Microwindows (`src/contrib/doom`, a Nano-X client) runs in the Linux
image with the GPU drawing its picture.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/doom.sh
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: start it from the desktop's Start menu or a terminal, or `nano-X -p & doom` at
the console. `-playdemo demo1` plays the first demo in real time, `-timedemo demo1` one tic a
frame, `-warp 1 1` starts in the first level.
The window is the largest of one, two or three times 320x200 that the display has room for
(960x600 on 1280x720: the GPU does the stretching, and draws the 3D view at that size, at the
same frame rate); `-1`, `-2` or `-3` says which.
The shareware `doom1.wad` is fetched by the build and is not kept in this repository.

## Speed

The machine runs 2.7 to 2.9 million instructions a second where it matters (FXC on D3D11,
which is what VRChat runs), and Doom wants 35 game tics a second whatever the frame rate. So
a frame rate is `(instructions a second - 35 x a tic) / a frame`, and both had to shrink.
Every five seconds Doom prints its frame rate and those two costs (`doomstat:` lines;
`DOOM_STATS_MS=1000` prints every second).

| demo1 in real time | A tic | A frame | Frames/s, D3D11 + FXC | D3D12 + DXC |
|---|---|---|---|---|
| before | 50k to 70k | 60k to 160k | 4 to 9 (measured at 2.0M/s) | |
| now | 25k to 43k | 45k to 60k | 19 to 35, mostly over 26 | 27 to 35 |

35 is the most there is: Doom draws once a tic. The other two demos run at 21 to 35 on
D3D11. The one window of the first demo that fell to 19 is its largest fight, where a tic
is 43k. Where the time went, and what was done:

- **The compiler's defaults.** The toolchain made position-independent code with a stack
  guard: a load from a table for every use of a global, and a stored and checked word in most
  functions. Doom is built `-fno-pie -flto` now, and nothing has the guard
  (`linux/userland/toolchain.sh`; libraries stay position-independent, because the guest's
  own linker, TinyCC's, takes no other kind).
- **Sight checks** were half of a tic. A monster that has not noticed the player asks whether
  it can see him several times a second, and the port asked that before asking whether he is
  in front of it. `doom_fast.c` asks in the other order (the same answer, a third fewer walks
  of the BSP tree) and walks the tree in one loop with a stack of its own.
- **Lines 64 bytes apart.** Doom marks each line it has visited in the line itself. Lines are
  64 bytes, and stores 64 bytes apart share 32 sets of the machine's write cache: a walk that
  touched more than 128 lines ended the machine's frame. The marks are in arrays of their own
  now (`doom_line_mark`, and one for sight).
- **Things at rest.** The thinker loop passes by a thing that has no state to count down and
  is not moving, without calling its thinker.
- **The view** was two thirds of a frame: walking the BSP tree for what is visible and sending
  every wall, floor and thing again. The level is kept in GPU memory now (below).
- **The message** at the top of the view was drawn again every frame, a letter at a time (6k
  to 9k instructions while one showed). It stays on the screen while it says the same.
- **Time.** Doom asks the time several times a frame, 416 instructions a system call. It
  reads the machine's clock word (`0x87000034`) instead.
- **Matrices.** `glOrthox` and the library's 64-bit divisions cost a frame about 5k; the flat
  views are constant matrices, and a window's size divides in 32 bits.
- **Fixed-point division** takes as many bits a step as the divisor leaves room for, instead
  of one.

A demo must play the same after any of this. `DOOM_HASH_TICS=N` prints a sum of the game's
state (every thing's place, state and health, and the random numbers used) every N tics;
`-timedemo demo1 -nodraw` runs tics only. `DOOM_PORT=sight,look,think` brings the port's own
code back for those three, and the sums of all three demos agree with it.

## What the build changes

The Microwindows tree is left as it is; `doom.sh` compiles a few files differently:

- The table of sprite names has no end marker and is counted up to the first zero after it.
  The count is bounded.
- The shareware data's demos are version 1.9; the source only took 1.10.
- `char` is signed (`-fsigned-char`): Doom's tables end with -1.
- The video functions, the renderer's entry points, the sight check, the search for players,
  the thinker loop, the clock, the message drawer and the screen melt are compiled under
  other names, and `doom_video.c`, `doom_gl.c` and `doom_fast.c` take their places.

## The screen (`doom_video.c`)

Doom draws on a 320x200 screen of palette indices. That screen is allocated in GPU memory
(before Doom takes pointers into it) and each frame is one rectangle textured with it. The
palette is 256 words the GPU looks indices up in, so Doom's palette effects (damage, pickups)
are 256 stores.

A full-screen page (title, help, intermission) is decoded once; after that only the rows
something else drew on (a menu) are copied back, and none when nothing did.

The melt between two screens is done by the GPU: 160 strips of the picture the window showed
last (the window's own pixels, used as a texture) slide down over the new frame, each moved on
by that frame's fall. Doom's own melt copies the whole screen every frame, and has no 3D view
in it here. The fall and its random numbers are Doom's.

## The view (`doom_gl.c`)

Walls and floors do not move, but for doors and lifts. When a level starts, all of it is
written into GPU memory once: a quad for every wall a seg can show (upper, lower, middle, and
a texture with holes across an opening) and for every two triangles of a subsector's floor and
ceiling. The polygons come from cutting a large square along the BSP tree's partition lines
down to each leaf. Quads are sorted into runs with one sector, one texture and one shade, and
each run is one command that stays at the head of every frame's list
(`seglKeep` in `programs/linux/gles.c`). The first level of the shareware game is 2,032 quads
in 764 runs. The depth buffer decides what is seen.

A frame then sends what changed:

- the view's matrices (one block of 28 words);
- a sector whose floor or ceiling moved: its planes' heights and the walls that stand on
  them (each sector has a list);
- a sector whose light changed: the grey of its runs, in Doom's sixteen light levels, and
  only in the set of commands the frame is built in (a run's grey is one word of its command,
  and commands are 64 bytes apart: the write cache holds few such words);
- the gun's flash, which lights everything for a tic or two, is one white rectangle added
  over the view, not a new grey for every run;
- a floor or ceiling seen from the wrong side is not drawn, as in Doom;
- a wall on a line that does something has a run of its own, so a switch changing its
  picture or a wall sliding is one command and one quad;
- animated textures: the runs that show one take the next picture;
- things. The sectors the viewer's sector can see into (Doom's REJECT table) are searched for
  things ahead of the viewer and within the view's quarter turn. A thing in view keeps a
  place among the kept commands while it stays in view: a frame turns its quad to face the
  viewer and sends a picture, a height or a light only when they change.

The sky is a problem of its own once everything is drawn: Doom paints sky over whatever is
behind a sky ceiling, and a depth buffer shows it. So sky ceilings, and the wall between two
of them, are drawn too, with the sky's picture laid on the screen instead of on the surface
(`FRAGMENT_LAID`, `docs/gpu.md`): they hide what is behind them and look like the sky. One
rectangle of sky at the far end of the depth range is still drawn behind everything.

A wall's texture, and a sprite's, is kept on its side (a row of texels is a column of the
picture), so each of Doom's columns is one run of bytes to copy; whoever draws with it swaps
the coordinates. Copied upright, a column's bytes are a row apart, and stores a row apart
filled the write cache every few dozen: a texture took hundreds of machine frames to make.
Making a level this way costs 5.4 million instructions (it was 11 million with a generic sort
and textures copied a pixel at a time).

`DOOM_PORT=view` draws the way it was done before: Doom walks its BSP tree every frame and
what it finds visible is sent. `DOOM_STOP_TIC=N` holds the picture of game tic N, which is how
pictures are compared. Against that older way the kept level differs in a few hundred pixels
of a frame, and where they differ by more it is the older way that was wrong: it showed sky
through a floor Doom's own renderer draws.

With `DOOM_RENDER=soft` Doom's own renderer fills the view and the GPU only shows the screen;
`DOOM_SOFTWARE=1` is the port as it comes. The GPU's view is the same scene as Doom's own but
not the same picture:

- light is the sector's light level on everything in it; Doom's darkening with distance is
  not there, and the gun's flash adds light where Doom's multiplies it;
- a texture that really uses the key index (247) loses those pixels where it has holes.

The window may be any size (the frame's maximise box): the view and the screen are drawn at
the window's size, the screen stretched from its 320 by 200.

## Still on the CPU

A tic is the game itself: sight checks (about a third still), thinkers, movement. A frame is
the things in view (about 250 instructions each, and 20 for each one looked at and left out),
the status bar and the frame's bookkeeping. Sound costs it next to nothing: effects and music
are the sound card's (`docs/sound.md`, `linux/nanox/doom_sound.c`).
