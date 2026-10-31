# The volume display

A program's last whole frame as geometry, seen from a visitor's own eyes in stereo: what the holodecks
(`docs/world/holodeck.md`) show as a room round a seat. There is no wall screen for it in the
world; `GpuVolume.shader` stays as the passes' plain form, and `GpuHolodeck.shader` is what is drawn.
The picture on the machine's display is unchanged; this is a second look at the same frame.

## What a program gives

Nothing new is drawn by the guest. A program's last whole frame is a command list
(`docs/machine/gpu.md`) that stays in memory, and four words say where:

| Address | Contents |
|---|---|
| `0x81000300` | address of the frame's command list (0: none) |
| `0x81000304` | commands in it |
| `0x81000308` | frames so far |
| `0x8100030c` | the picture's size: width, and height from bit 16; from bit 28, which axis is up (1 x, 2 y, 3 z; 0: not said); bit 31, a flat screen (`seglWorldFlat`) |

- Both OpenGL libraries write them after each swap (`programs/linux/gl.c`, `gles.c`) and build
  the next frame somewhere else: frames alternate between two places, so the one named here
  stays whole until the frame after it is complete. `gles.c` has two sets of list, matrix
  blocks and vertices (`SET_SIZE`, 384 KB each by default: 3,072 commands and 10,240 compact
  vertices; a program may build it with its own split, as Quake does).
- The kernel clears the words when `/dev/gpu` is closed, so a program that ends leaves nothing.

## What the host draws

`GpuVolumePass.cginc` draws the same point mesh as the GPU device, one point a triangle, with the
visitor's camera. Of the frame's commands it takes only draws in perspective with a modelview
of their own (`VERTEX_MODELVIEW`, and a projection whose w comes from z): those have a place in
space. Clears, rectangles and what a program draws flat over its picture (Doom's status bar
and weapon) are left out. A vertex goes through the modelview only, which leaves it in the
program's camera space (`volume_vertex` in `src/gpu.h`).

- **Where the scene is.** A plane of the program's own view, `_Plane` of the way from near to far
  in equal ratios, is fitted to a rectangle of the room (`_ScreenSize`). The projection gives all
  of it: near and far from its third row, the picture's rectangle from its scales. A near plane is
  often tiny, which makes the scene huge: the holodeck's World slider moves that plane
  (`docs/world/holodeck.md`, "The world's size").
- Four passes, the GPU's own (`docs/machine/gpu.md`) in its order: solid, blended, added, multiplied,
  all with the depth test and only the first writing depth. Quake's light maps multiply its
  walls here as they do in its picture, and its glows are added. A command for one of the
  GPU's passes without depth (4 to 7) is drawn in the pass that blends alike. Textures, keyed
  texels and lit colours are as the GPU's. Blending is in Unity's linear colours, the GPU's is
  in the picture's own: a product comes out the same, a sum or a mix a little lighter.
- **Stencil.** A mask (`HolodeckMask.shader`) marks the stencil where the place the scene shows
  through is in view and empties colour and depth there, so the scene shows however far it
  reaches; the scene is drawn where the mark is; `VolumeSeal.shader` then gives those pixels the
  place's own depth and removes the mark.

A program sends only what its camera sees, so from behind or beside there is nothing to show:
a box to walk round does not work.

The harness has no room and no such display.
