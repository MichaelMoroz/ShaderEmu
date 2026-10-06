# Doom

The Doom that comes with Microwindows (`src/contrib/doom`, a Nano-X client) runs in the Linux
image with the GPU drawing its picture.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/doom.sh
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X & doom` (or start it from a terminal under `nx`). `-timedemo demo1`
plays the first demo as fast as it goes; the frame rate is printed every five seconds.
The shareware `doom1.wad` is fetched by the build and is not kept in this repository.

| How the picture is made | Frames/s, `-timedemo demo1`, RTX 5090 with DXC |
|---|---|
| `DOOM_SOFTWARE=1`: the port as it comes. Doom fills its 8-bit screen, converts it to 32-bit colour, sends it to the server, which blits it | 0.9 |
| `DOOM_RENDER=soft`: Doom fills the screen; the screen is a texture and the GPU shows it through the palette | 3.7 |
| default: the GPU also draws the 3D view | 12 to 22 |

## What the build changes

The Microwindows tree is left as it is; `doom.sh` compiles a few files differently:

- The table of sprite names has no end marker and is counted up to the first zero after it.
  The count is bounded.
- The shareware data's demos are version 1.9; the source only took 1.10.
- `char` is signed (`-fsigned-char`): Doom's tables end with -1.
- The video functions and three of the renderer's entry points are compiled under other
  names, and `doom_video.c` and `doom_gl.c` take their places.

## The screen (`doom_video.c`)

Doom draws on a 320x200 screen of palette indices. That screen is allocated in GPU memory
(before Doom takes pointers into it) and each frame is one rectangle textured with it. The
palette is 256 words the GPU looks indices up in, so Doom's palette effects (damage, pickups)
are 256 stores.

## The view (`doom_gl.c`)

Doom still walks its BSP tree front to back and keeps its own list of screen columns already
covered, which is what decides whether a wall is seen at all. What it used to do next, filling
pixels a column or a row at a time, was two thirds of a frame. Instead:

- a visible seg becomes up to three textured quads (the wall, or the parts above and below an
  opening, and a texture with holes across it);
- a subsector becomes its floor and ceiling polygon. The polygons are made once per level by
  cutting a large square along the BSP tree's partition lines down to each leaf;
- a thing becomes a quad facing the viewer, with the picture for the angle it is seen from;
- the sky is one rectangle at the far end of the depth range, and the weapon is drawn flat on
  top;
- the screen (status bar, messages, menus, automap) goes over all of it, with the view's area
  holding a key index that is not drawn.

All of it goes through `programs/linux/gles.c` in Doom's own 16.16 numbers. Textures are made
the first time they are seen, as rows of palette indices in GPU memory, and dropped when the
level changes. The depth buffer sorts what Doom's column lists used to.

`DOOM_STOP_TIC=N` holds the picture of game tic N, which is how the three ways were compared.
With `DOOM_RENDER=soft` the window is the same, pixel for pixel, as the port's own path. The
GPU's view is the same scene but not the same picture:

- light is the sector's light level on everything in it; Doom's darkening with distance is
  not there;
- where two sectors are open to the sky, things further away show through instead of sky;
- a texture that really uses the key index (247) loses those pixels where it has holes;
- the screen-melt between levels shows the key colour where the view was.

The window may be any size (the frame's maximise box): the view and the screen are drawn at
the window's size, the screen stretched from its 320 by 200.

A full-screen page (title, help, intermission) is decoded once and copied after that; Doom
draws it again every frame, and a post at a time that was most of a menu frame.

## Still on the CPU

A frame of the default mode is about 230,000 instructions: the game itself (sight checks,
thinkers), the BSP walk, the status bar and other 2D drawing (about a fifth), and writing the
frame's vertices. `-timedemo` runs one game tic a frame. Played in real time Doom runs 35 tics
a second whatever the frame rate, and the tics alone are most of the CPU: the demo then shows
7 to 12 frames a second, and the title screen 24. Keyboard play and the `-2` and `-3` window
sizes have not been tried.
