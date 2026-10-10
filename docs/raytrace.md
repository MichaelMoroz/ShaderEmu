# Rays on every core

Two programs made for the machine's worker cores (`multicore.md`), and for the small ones
above all: a pixel of a traced picture is a few thousand instructions of float arithmetic
and a handful of bytes stored, so a core needs next to no write cache for it, and the
machine's geometry lets a program have fifteen such cores in place of three large ones.

Both work the same way. Core 0 traces nothing: it lays the cores out (`mcw_shape`), hands a
tile of 32 x 32 pixels to every worker that has none (`mcw_post`, `mcw_done`), and shows the
picture as it fills. The picture is words in GPU memory that the display draws where they
lie (`seglTexturePointer`), and a tile's rows begin and end on 16 bytes of it (a tile is 32
pixels of four bytes across, the picture's width a multiple of four), so no two cores store
to the same 16 bytes. Everything else a job writes is its own stack; the scene is only read.

## `nxray`: a ray tracer

`linux/raytrace` (`build.sh` after Nano-X's; Start menu: Other, "Rays (every core)"). Six
balls over a chequered ground, one light, shadows and three mirrorings, turning; 320 x 240.

    a   all the workers the geometry has      1   one worker      0   core 0 alone
    s   the next geometry (15 small, 3 large and 12 small, 7 large, 15 of the smallest)
    w   each tile framed in its core's colour  space  stop and go   q, Escape  leave

`RAY_FRAMES=N` leaves after N pictures, `RAY_WORKERS=N` and `RAY_SHAPE=4,4,...` set the start,
`RAY_SIZE=WxH` the picture, `RAY_STILL=1` keeps the scene where it is. Each picture's line
(`raystat:`) has its time, its rays and a sum of its pixels.

One picture (201,460 rays) on the shader machine, `--cores 16`, the same sum every time:

| The cores | A picture | Against core 0 alone |
|---|---|---|
| core 0 alone | 16.9 s | |
| 3 large | 7.8 s | 2.2 times |
| 7 large | 4.2 s | 4.0 |
| 12 of size 5 | 2.6 s | 6.5 |
| 15 small (size 4) | 2.2 s | 7.7 |
| 3 large and 12 small (the kernel's default) | 2.2 s | 7.7 |
| 15 of the smallest (size 3) | 1.3 to 1.5 s | 12 |

The smallest cores are the fastest: what a busy core costs the pass is its pixels, and this
work never fills even their cache.

## `nxpath`: a path tracer, with a panel

`linux/pathtrace` (`build.sh` after Dear ImGui's, `linux/imgui`; Start menu: Other, "Path
tracer (every core)"). A Cornell box: a red and a green wall, a lamp in the ceiling, a block
and two balls that are matt, a mirror or glass. `tracer.c` is the tracer, plain C, and its
job is one more sample of every pixel of a tile: a path from the eye that turns up to so
many times, the lamp's light on every matt surface it meets by a ray to a point of the lamp,
a path that carries little ended by chance. The samples' sums are three floats a pixel in
memory from `mcw_alloc`; the picture is made from them as each sample comes in, so it clears
while one watches. A sample is held to 2.5 times white: one path in thousands reaches the
lamp by a mirror or through glass and would be a white speck for good.

`nxpath.cpp` is the panel, Dear ImGui (`imgui.md`): the picture's size, samples a pixel,
turns of a path, the lamp, the two balls' kinds, which core traced what, and how the cores
are laid out (15 of the smallest, 15 small, 7 large, or this core alone); Render, Stop, how
far it is and how many rays a second. The panel is drawn seven times a second while a
picture is traced (it is some 200 thousand instructions of core 0) and the tiles are handed
out in between.

`NXPATH_AUTO=1` presses Render at the start and `NXPATH_EXIT=1` leaves when the picture is
done; `NXPATH_SAMPLES`, `NXPATH_BOUNCES`, `NXPATH_SIZE=WxH` and `NXPATH_CORES=N` (0 to 3, the
panel's four layouts) set the panel. A picture's line (`pathstat:`) has its time, rays and
sum.

160 x 120, 8 samples a pixel (1,023,465 rays), the same sum whichever cores:

| The cores | The picture | Rays a second |
|---|---|---|
| core 0 alone | 2 minutes (15.2 s a sample) | 8 thousand |
| 7 large | 32 s | 31 thousand |
| 15 small | 21 s | 48 thousand |
| 15 of the smallest | 13.6 s | 75 thousand, 9.4 times core 0 |

## Checking them

A picture's sum does not depend on which cores traced it, or how many: run one with
`RAY_WORKERS=0` (or `NXPATH_CORES=3`) and one with all of them. `rvc_harness --cpu` runs both
at once the speed for looking at them (`cpu-harness.md`); the times above are the shader
machine's (`rvc_harness --cores 16`).
