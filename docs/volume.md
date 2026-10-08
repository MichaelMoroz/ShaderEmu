# The volume display

A 3D program's picture is one view of its scene. The volume display shows the scene itself: in
the VRChat world a third screen hangs on the left wall, by the console, and it is a window. Behind it a
visitor sees the program's geometry from wherever their own eyes are, in stereo, in depth. The
picture on the machine's display is unchanged; this is a second look at the same frame.

## What a program gives

Nothing new is drawn by the guest. A program's last whole frame is a command list
(`docs/gpu.md`) that stays in memory, and three words say where:

| Address | Contents |
|---|---|
| `0x87000300` | address of the frame's command list (0: none) |
| `0x87000304` | commands in it |
| `0x87000308` | frames so far |

- Both OpenGL libraries write them after each swap (`programs/linux/gl.c`, `gles.c`) and build
  the next frame somewhere else: frames alternate between two places, so the one named here
  stays whole until the frame after it is complete. `gles.c` has two sets of list, matrix
  blocks and vertices of half the old size each (1,536 commands, 16,384 compact vertices).
- The kernel clears the words when `/dev/gpu` is closed, so a program that ends leaves nothing.

## What the host draws

`GpuVolume.shader` draws the same point mesh as the GPU device, one point a triangle, with the
visitor's camera. Of the frame's commands it takes only draws in perspective with a modelview
of their own (`VERTEX_MODELVIEW`, and a projection whose w comes from z): those have a place in
space. Clears, rectangles and what a program draws flat over its picture (Doom's status bar
and weapon) are left out. A vertex goes through the modelview only, which leaves it in the
program's camera space (`volume_vertex` in `src/gpu.h`), and from there behind the screen.

- **Where the scene is.** The screen is a plane of the program's own view: its near plane,
  with the picture's rectangle on that plane fitted to the screen. The program's camera then
  stands its near distance in front of the screen, and a visitor whose eye is there sees the
  program's picture exactly; anywhere else they see the same scene from the side. The
  projection gives all of it: near and far from its third row, the rectangle from its scales.
- **The slider** under the screen moves that plane from near (the default) towards far, in
  equal ratios. A near plane is often tiny, which makes the scene huge and far behind the
  wall: glxgears' is 2 units wide, 35 units in front of gears 8 units across. With the plane
  among the gears they are at the wall, and what is nearer than the plane is cut away at it
  (a clip distance in `GpuVolume.shader`): the screen is a window, and nothing is before it.
- One pass. Every command is drawn solid, with depth, whatever pass it asked for: blended and
  added surfaces are opaque here. Textures, keyed texels and lit colours are as the GPU's.
- **Stencil.** `VolumeMask.shader` on the screen's quad marks the stencil where it is in view
  and empties colour and depth there, so the scene shows however far it reaches behind the
  wall; the scene is drawn where the mark is; `VolumeSeal.shader` then gives those pixels the
  screen's own depth and removes the mark. All three come after everything solid and the sky
  (queues 2501 to 2503).
- The button under the screen turns the three off.

A box that one could walk round was tried first. It does not work: a program sends only what
its camera sees, so from behind or beside there is nothing to show.

The harness has no room and no such display.
