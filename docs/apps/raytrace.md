# Rays on every core

Two programs made for the machine's worker cores (`multicore.md`), and for the small ones
above all: a pixel of a traced picture is a few thousand instructions of float arithmetic
and a handful of bytes stored, so a core needs next to no write cache for it, and the
machine's geometry lets a program have fifteen such cores in place of three large ones.

(`nxray` traces on lanes since 20 October 2026, a texel of four pixels an index: the field's or its
workers' (`docs/machine/lanes.md`; "Lanes", below). What follows of tiles is `nxpath`'s, and `nxray`'s
of before.)

Both worked the same way. Core 0 traces nothing: it opens the workers there are (`mcw_open`;
it once laid the cores out by size, `mcw_shape` and `mcw_fit`, which are gone: 15 of the
smaller sizes and 7 of the largest on a machine of 16 cores, 63 and 27 on one of 64), gives
every worker that has none its share (`mcw_post`, `mcw_done`), and shows the picture as it
fills. A tile is 32 x 32 pixels, halved until every worker has two dozen tiles of the picture
and never narrower than 8 (it was 16 x 16 when 32 would leave a worker fewer than four): with
63 workers a picture of 320 x 240 in tiles of 32 is 80 tiles, hardly more than one each
(0.57 s; 0.48 s in tiles of 16), and in tiles of 16 where 32 would do, the handing out
costs (640 x 480: 1.76 s against 1.40 s). The sum is the same either way. The picture is words in GPU memory that the display draws where they
lie (`seglTexturePointer`), and a tile's rows begin and end on 16 bytes of it (a tile is a
multiple of eight pixels of four bytes across, the picture's width a multiple of four), so no two cores store
to the same 16 bytes. Everything else a job writes is its own stack; the scene is only read.

## `nxray`: a ray tracer

`linux/raytrace` (`build.sh` after Nano-X's; Start menu: Other, "Rays (every core)"). Six
balls over a chequered ground, one light, shadows and two mirrorings, turning; 320 x 240.

A bar under the picture has a button for each of these keys but the last, saying how each
stands, and a line of figures (pictures and rays a second), as `nxphys` has:

    r   the next size of picture: 160 x 120, 240 x 180, 320 x 240, 480 x 360, 640 x 480
    m   how often a ray is mirrored: 2, 0, 1          h   shadows, on and off
    space  stop and go                                w   each tile framed in its core's colour
    W   all the workers, one, none (a, 1, 0: each by itself)        q, Escape  leave

The two pictures' memory is for 640 x 480 from the start (or `RAY_SIZE`'s, if larger): a new
size is the same memory given to the textures again, with no picture out. The window keeps
its size and the picture is stretched over it. The keys are looked at after every picture
now: before, only while one was waited for, and with pictures that were whole at once (160 x
120 with every worker) a key was not taken at all. With the defaults a test's sums and rays were
what they had been (`9383472e`, `226b5e92`, `c69f022b`, `337a52f6` for `RAY_FRAMES=4` on
`rvc_cpu`; since the lanes, "Lanes" below, other ones), and a picture took as long (36 to 43 ms
there, 40 to 42 before).

`RAY_FRAMES=N` leaves after N pictures, `RAY_WORKERS=N` sets the start (every worker has its share of a picture in one job: `docs/machine/multicore.md`, "A share, not a tile"),
`RAY_SIZE=WxH` the picture, `RAY_STILL=1` keeps the scene where it is. Each picture's line
(`raystat:`) has its time, its rays and a sum of its pixels.

The first core traces nothing while it has workers (it had some tiles of its own until 13
October 2026: "Pictures one after another", below).

One picture (201,460 rays) on the shader machine with 16 cores (it had `--cores 16` then), the same sum every time:

| The cores | A picture | Against core 0 alone |
|---|---|---|
| core 0 alone | 16.9 s | |
| 3 large | 7.8 s | 2.2 times |
| 7 large | 4.2 s | 4.0 |
| 12 of size 5 | 2.6 s | 6.5 |
| 15 small (size 4) | 2.2 s | 7.7 |
| 3 large and 12 small (the kernel's default) | 2.2 s | 7.7 |
| 15 of the smallest (size 3) | 1.3 to 1.5 s | 12 |

(Those are the tick in one target. In eight, each worker's pixels a block of its own, a
large worker costs no more than a small one: `multicore.md`, "Eight texels a pixel".)

On the machine's 64 cores, the program's own choices:

| | 16 cores | 64 cores |
|---|---|---|
| `nxray`, 320 x 240 | 1.21 s with 15 workers | 0.48 s with 63 |
| `nxray`, 640 x 480 (`RAY_SIZE`) | 4.72 s | 1.62 s (1.35 s with 63 of the smallest, `RAY_SHAPE`) |
| `nxpath`, 160 x 120, 8 samples | 11.4 s, 89 thousand rays a second | 6.9 s, 148 thousand |
| `nxpath`, 320 x 240, 4 samples | 18.7 s, 109 thousand | 9.4 s, 217 thousand |

The path tracer gains less: core 0 draws its panel seven times a second and hands the tiles
out in between (the `nxpath` of before, a Cornell box with a Dear ImGui panel: below).

### A ray tries only what it can meet (12 October 2026)

A picture of 320 x 240 went from 43.6 to 19.6 million instructions on one core and from
42.9 to 20.6 on every core together; the arithmetic for what a ray does try is unchanged,
so the sums are.

- Core 0 works out per picture where each sphere can be on the screen (between its tangents
  from the eye, four pixels more) and where its picture in the ground can be (the sphere
  mirrored under the ground: a ray the ground throws back meets a sphere exactly when the
  eye's ray meets that image); a tile's row lists the spheres of each kind that cross it.
- A point of the ground asks a 16 x 16 grid which spheres' shadows can reach its square (a
  shadow is within radius / light.y of where the light through the centre lands).
- `-O3` was 3 to 16% worse here; it is built `-O2`.

After a change, old and new must agree on `RAY_FRAMES=24 RAY_SIZE=160x120 nxray` (a moving
scene), not only on the still picture. The start did not move: of 2.7 million instructions
with every worker, about half is `mcw_open` (a worker's stack pages, its start, its first
steps) and the rest the program's exec, the GPU's memory mapped and the window made.

### Pictures one after another (13 October 2026)

On the shader machine with 255 workers, the scene turning: 320 x 240 went from 17.4 to 17.6
pictures a second to 28 to 31, 640 x 480 from 6.8 to 8.3 to 8.7; a test's pictures
(`RAY_FRAMES`, each timed by itself, the same sums) from 52 to 61 ms to 37 to 44, the first
from 75 to 94 ms to 64 to 70. Of the workers' instructions (255 times 8,192 a pass) 38% were
run at 320 x 240 and 65% at 640 x 480; now 75% and 94%.

`RAY_FRAMES=60 nxray` on 19 October 2026 on an RTX 5070 laptop (`docs/machine/speed.md`), a picture:
44.2 ms on the shader machine (183 million instructions a second, 178 of them the workers'), 46.1 on
`rvc_lean`, 12.2 on the compute machine (337 million), 60.2 on the CPU harness, 6.6 s on core 0 alone
(the plain compute shader).

A picture of 320 x 240 is 80 thousand instructions a share, ten passes, and took seventeen:
the 255 jobs were four passes of the first core to give out (the last worker began that much
later) and a pass to take back, the scene was made while every worker waited, and the picture
shown was waited for.

- **A worker's job is every picture** (`trace_pictures`): it is posted once and goes from a
  picture to the next when that one is published (`go.frame`), saying in 16 bytes of its own
  how far it is. A worker whose core the kernel wants ends its job between two pictures, and
  the first core traces what it had of the pictures that are out (`look_at_jobs`).
- **Two pictures are out at once** (`pictures[2]`, each with its scene, its plan and its
  pixels, two textures): while the workers trace one, the first core shows the one before
  (`seglSwapEarly`), makes the scene of the next but one and publishes it, so a worker that
  is through begins the next in the same pass. A picture's pixels are written again only
  when it has been shown (`go.shown`). A test has one out, to time each by itself
  (`RAY_ONE=1` for that with the scene turning: 21 pictures a second).
- **A share that took long gives tiles away** (`give_shares`): by what each share took in
  the picture before (its instructions, `rdcycle`), the dearest has some of its last tiles
  traced by the cheapest after its own, and so on inwards. At 640 x 480 the dearest share
  was 371 thousand instructions against a mean of 302 and is 327 (166 tiles of 9,600 change
  hands); at 320 x 240 the dearest is 7 thousand over the mean, under a pass, and nothing is
  given. `RAY_EVEN=1` gives none, `RAY_SHARES=1` prints what the shares took.
- **A worker with no picture ends its pass** (`mc_end_pass()`, `multicore.md`), where it
  would turn in place; `RAY_SPIN=1` for that.

### Lanes (20 October 2026)

`nxray` traces on lanes (`docs/machine/lanes.md`): `trace_texel(i)` makes texel `i` of the picture, four
pixels side by side, by the same arithmetic as before (the spheres a texel's row and four pixels can
meet, then `trace()` for each), and stores its 16 bytes, with the rays it cast in its first pixel's top
byte (which the display does not draw and the sum leaves out).

- **On the field** where the machine has one (the lean tick in the harness): a picture is one job of
  its 3,906 lanes (`mcw_field_begin`), posted by the first core as soon as the picture before is
  through, while it shows that one and makes the next scene. No worker is opened. `RAY_FIELD=0`
  takes the workers instead.
- **On the workers' lanes** elsewhere: a worker's share of a picture is a band of texels, one job of
  its lanes (`mcw_lanes`); `RAY_LANES=0` makes its texels one after another (`rvc_cpu`, which has no
  lanes, does that anyway). The bands are cut again one picture in four by what each took: a texel
  costs what its band cost over its length, made smooth over the bands round it and at least a sixth
  of a pass's instructions (a lane makes six texels a pass: `RAY_FLOOR`), and a band is given as much
  of the cost as its worker's lanes make (27 or 6) while the others make theirs, half way from where
  it was. The tiles, their order and the tiles given from a dear share to a cheap one are gone.
- **The first core's part** went from some 95 thousand instructions a picture to 12 (on the field):
  the scene's sines in floats (`sin_cos`: the C library's are doubles, 2,800 instructions a call, a
  quarter of it), the bands not cut again three pictures in four (their cut in floats: two 64-bit
  divisions a share were half of it), the jobs asked about every 250 ms, not 50, and up to four
  pictures out at once where GPU memory has room (`OUT`; a third and fourth picture are of the
  picture's own size, so 640 x 480 has two).
- The sums of `RAY_FRAMES=4` are now `53ea472d`, `226b5e92`, `37366b32`, `3ffdbe63` on `rvc_cpu`, with
  every worker and with none (the sines in floats move the scene by their last bits); on the card they
  are the card's, the same on the field, on the workers' lanes, with `RAY_LANES=0` and with no worker.
  On `rvc_cpu` a picture takes 51 to 60 ms (36 to 43 before: no lanes there, and bands in place of tiles).

On the RTX 5070 laptop, geotune's layout (16 workers of 2^5, 239 of 2^3), free-running:

| | 320 x 240 | 640 x 480 |
|---|---|---|
| before (the tiles, 255 workers) | 28 to 31 pictures a second (175 million instructions a second in the test's 60) | 6.8 to 8.7 |
| the workers' lanes | 52 to 56 a second, 1,370 million | 16 to 18, 1,500 million |
| the workers' lanes, every worker 2^5 (`--workers 8`) | 95 to 105, 2,280 million | 35 to 37, 3,300 million |
| the field | 105 to 125, 2,300 to 2,750 million | 32 to 38, 3,200 to 3,500 million |

`RAY_SHARES=1` prints each picture's shares (the workers'), or on the field what the first core's part
of the picture took and how many texels the lanes left it.

## `nxpath`: a mesh viewer with a path tracer

`linux/pathtrace` (`build.sh` after Dear ImGui's; Start menu: Other, "Path tracer (every
core)"; `nxpath [MESH]`, and Files opens an `.obj` or `.ply` with it). Since 13 October 2026
it is a small scene editor: meshes are read from files, put into a scene with lights, shown
by OpenGL, and path traced by the worker cores under a tree of boxes that they built.

- **The scene** (`scene.h`): meshes as read (`mesh.c`: Wavefront `.obj`, Stanford `.ply` as
  text or little-endian bytes, faces of any size, normals made where the file has none; a
  plane, a box and a ball made here), objects that place a mesh (place, turn, size; a
  material, six numbers since 14 October: "What a thing is made of", below; a colour), up to
  64 objects and eight lights (balls that shine), one camera that goes
  round a point, and the sky's light. It starts with a ground, the teapot, a mirror ball and
  a light. The example meshes are `/root/Models/teapot.obj` (6,320 triangles), `bunny.ply`
  (3,851) and `knot.obj` (5,040, `make_knot.py`).
- **The window** (`nxpath.c`): menus along the top (File, Add, View, Render), the view on the
  left, a panel of the window system's own on the right (`ui.h`), and along the bottom a line
  of figures with the picture's bar. The panel is the scene's list and three tabs (Material
  since 14 October, below), laid out as one table of rectangles that is both what is drawn
  and what a click is on. "Object" (or "Light"): what the handles do (move, turn, size), the
  three numbers of each with a button down and up (a light: its place, radius, power,
  colour), Duplicate, Delete. "Render": Detail, samples a pixel (1 to 512, or no end), turns
  of a path, the background's light, "Traced when the view is still", Render
  or Stop, the scene or the picture, Export (a PPM file). In the view the left button picks
  what is clicked and turns the view while held, the right one moves it, the wheel goes
  nearer. Keys: g, r, s the handles, Delete, Ctrl+D a copy, Home the whole scene in view, L
  tracing when still, F5 renders or goes on, Escape stops.
- **One camera** (see "The view is the picture" below): the picture traced is the view's own
  shape, the view's size over what Detail says (all of it, a half, a third, a quarter).
- **OpenGL has no lights here**, no colour a vertex and no lines (`gles.c`): a mesh is drawn
  where it lies in GPU memory (`seglTagged`), each vertex tagged with the one of 256
  directions its normal is nearest, and an object has a table of 256 colours, its own colour
  lit from each direction as its matrix turns it. A turn, a colour or a selection is 256
  words written, not the mesh.
- **The handles are ImGuizmo's** (`gizmo.cpp`), Dear ImGui's only part here: its frame is
  made of the pointer and the picture's matrices, drawn over the picture, and is 55 thousand
  instructions for the move handles, 120 for the turn ones. ImGuizmo is fetched at a commit
  and has three circles' side counts changed at build time (`build.sh`): a stated count was a
  sine and a cosine a side in doubles. A frame is drawn only when something changed or the
  pointer moved in the picture.
- **The world** (`bvh.c`): every object's triangles where they stand, 80 bytes each, under
  one tree of boxes of at most four triangles a leaf. The first core cuts the triangles into
  parts by a grid, the workers build a binned tree a part in memory that is each one's own,
  and the first core builds the tree over the parts. It is the same tree whichever cores
  built it. The test scene's 12,105 triangles: 5.7 to 6.0 s on the first core alone, 0.36 s
  with 63 or 255 workers (1.6 s with three); 75,842 triangles 42 s and 1.7 s (`rvc_cpu`). The
  first core's own part is what is left, so more than 63 workers gain nothing.
- **The tracer** (`tracer.c`) is the one of before with another scene: the tree and the
  lights' balls, a light picked by chance at every matt surface and a ray to a point of it,
  the sky for a ray that meets nothing, glass that casts no shadow, and no lamp seen after a
  matt surface by way of a mirror or glass (materials, glass's shadow, a light's cone and the
  background since 14 October: "What a thing is made of, and the light", below). Tiles,
  shares, the numbers a path draws and the picture made as the samples come in are as below.
- **Settings are the registry's** (`pathtrace.detail`, `.samples`, `.turns`, `.background`, `.live`:
  `docs/system/persist.md`).

For tests: `NXPATH_AUTO=1` renders at the start and `NXPATH_EXIT=1` leaves when the picture
is done, `NXPATH_SIZE=WxH`, `NXPATH_SAMPLES=N`, `NXPATH_CORES=N` (0: this core alone),
`NXPATH_SAVE=FILE`, `NXPATH_LIVE=0` (not traced when still), `NXPATH_PRINT=1` (a line for
every change of the scene, with the panel's controls counted: none may lie over another or
have words wider than its place; a line when the small picture is shown, when the picture has
its first sample, and `pathstat:` whenever a picture ends), `NXPATH_CHECK=N` (N rays through
the tree and by every triangle: 0 may differ; and 50 N of the tracer's rays through OpenGL's
matrices: 0 thousandths of a pixel apart). A picture of `NXPATH_SIZE` is a test's: it has its
own shape, is shown as large as fits and has no small picture before it. The picture's line
has the tree's time, the picture's, its rays and its sum. On `rvc_cpu`, 160 x 120 with 2
samples of the scene it starts with (7,282 triangles): sum `f38bbd71` with 255 workers and
with none (`02f213e9` since 14 October, below); the tree 483 ms against 3,632, the picture
134 ms against 12,359.

### The view is the picture (13 October 2026, later)

The picture had a size of its own (160 x 120 to 640 x 480, four by three) and was shown as
large as fitted in a view of another shape, 520 by 462: OpenGL's camera took the view's
shape, the tracer's the picture's, and a thing at the picture's edge was 48 pixels from where
OpenGL had drawn it (by the two openings' arithmetic: 0.8 across against 0.675). A picture was made on Render only, and the window showed nothing of how
far it was but a line of text when it ended.

- **One shape.** The picture is the view's width and height over Detail's number, the width
  to four, and the camera is told how much wider than high it sees (`aspect` in `sc_camera`,
  0 for a test's picture): `eye_way` opens across by it, so a third of 520 being 172 moves
  nothing. The handles and a click are the same over the traced picture as over OpenGL's.
- **Traced when still.** A change of the scene or the view (`invalidate`) gives up what is
  being traced (`pt.stop` 2: a share ends with the tile it is at, where 1 ends with its
  sample) and OpenGL draws; when nothing is held and nothing has changed for 300 ms the
  picture is begun again. The tree is built again only if an object's mesh or matrix is not
  what it was built of (`world_sum`): a turn of the view, a colour or a light costs no tree.
- **Small first.** A quarter of the picture each way with one sample, shown as the squares
  it is (so is the picture: the device smooths no texture of `seglTexturePointer`'s kind, and
  asked to, drew it half its width aside), then the picture itself over it once every pixel has a sample, and sample after
  sample from there: the bar says which of how many. "No end" goes on until something
  changes or Stop. More samples asked of a picture that is there, or Render after Stop, go
  on from the samples it has.

On `rvc_cpu`, the scene it starts with, a view of 520 x 490 at a half (260 x 245): the small
picture (68 x 62) 118 to 134 ms after the picture is begun, the first sample of the picture
at 281 to 361 ms, 4 samples 636 to 690 ms; at a third (172 x 163) 68 to 76 ms and 201 ms.
After the wheel and after Home the same three lines again with the tree's old time; the
samples' slider from 4 to 256 goes on with no small picture, Escape stops at 35, F5 goes on
to 56; with L off the wheel begins nothing and F5 does. The two cameras: 0 thousandths of a
pixel apart over 1,000 rays at a half and at a third, in two views. The panel in its four
states: 57, 20, 36 and 7 controls, none over another, none with words too wide, the lowest
ending at 486 of 508. Not done: the handles dragged over a traced picture (as below), any of
it on the shader machine, and a picture larger than the view (a window made larger is the
way to one).

### What a thing is made of, and the light (14 October 2026)

An object was matt, a mirror or glass, glass had no colour and cast no shadow, the sky always
shone, and a light's brightness went with the square of its radius.

- **A material is six numbers and a colour** (`sc_object`, `sc_material()` for the six
  starting points Matt, Plastic, Metal, Mirror, Glass, Glow): `rough` (0 a mirror's shine to
  1 none), `metal` (how much of it mirrors in its colour), `glass` (how much lets light
  through), `ior` (1 to 2.5: how glass bends, and how much a coat mirrors), `density` (how
  soon glass takes the light that is not of its colour) and `glow` (its own light). A path
  picks by chance among them at each surface: through the glass; the metal's shine; or, under
  1 of roughness, a coat that mirrors more the flatter the ray (Schlick) over the colour's
  matt. A shine under 0.15 of roughness is a mirror's with its way put a little off; a
  rougher one asks a light for its light, weighed by how near the mirrored way it comes.
- **Glass takes light by its colour** (Beer and Lambert): a ray that meets a surface of it
  from inside has gone its length through it, and each colour is less by `density` times that
  length times what the colour lacks (`fade`, (1 - x/8) to the eighth for e to the -x). Which
  side a ray is on is the flat triangle's to say, turned the way its corners' normals are:
  the mean normal said it wrong where a ray skims a round thing. Light to a surface through
  glass is less the same way (the lengths inside from where the ray leaves less where it came
  in) and by a twelfth a surface: glass has a shadow of its colour. No light gathered by it.
- **A light's light at a surface** (next event estimation, `light_at`): a ray into the cone
  the light's ball fills from there, evenly by solid angle, where it was a ray to a point of
  the whole ball, half of them to its far side. `power` is of the whole ball (`shine`): a
  smaller light is as bright on the ground and brighter to look at. Lights start at 0.12 and
  0.1 of radius.
- **The background gives no light** unless asked: its colour and power are the Render tab's,
  and the registry's `pathtrace.background` and `.background.colour` (`pathtrace.sky` is gone).
- **Turns** are 1 to 16, 8 at first (glass uses two a thing), and from the third on any path
  that carries little ends by chance, not only one off a matt surface.
- **The panel** has a third tab, Material: the six starting points, the colour, and a slider
  with its number for each of the six.

On `rvc_cpu`, the scene it starts with, 160 x 120: the sum is now `02f213e9` (2 samples, 255
workers and none, 81,647 rays, 230 ms with workers as before), 0 of 20 rays wrong by the
tree. The picture's mean light (`light` in `pathstat:`, thousandths, 16 samples) with the
light at 0.05, 0.12 and 0.30 of radius: 146, 147 and 161, which is the same but for the
light's own disc in the picture (0.4, 2 and 15 by its size). The panel in its seven states: no
control over another, none with words too wide, the lowest ending at 500 of 508. Looked at:
a matt, a metal and a glass teapot at 32 and 64 samples. Not done: a roughened glass, a
glowing mesh, a glass mesh that is not closed, more than one light, the shader machine; a
rough shine's light is weighed by a rule (Phong's) that is not the one its next way is drawn
by, and nothing compared it with a tracer known to be right.

### Example scenes (15 October 2026)

A scene is a text file (`.scene`), opened by File, "Open a scene...", by File, Examples, by
`nxpath FILE.scene` or `nxpath NAME` (an example's name), and by Files (`docs/system/open.md`). The
examples are `linux/pathtrace/scenes`, in the image at `/usr/share/pathtrace`:

| Name | What it is |
|---|---|
| `cornell` | The Cornell box: the walls, the two blocks, the light and the camera as the Cornell Program of Computer Graphics measured them (`cornell-*.obj`, millimetres, at a hundredth; the light half a millimetre under the ceiling it lies in in the data), white 0.725 0.71 0.68, red 0.63 0.065 0.05, green 0.14 0.45 0.091, the light 17 12 4, a lens of 35 mm over 25 mm of film |
| `cornell-balls` | The same room with a mirror ball and a glass ball |
| `balls` | Clear and green glass, gold, copper, a mirror and red plastic balls on a plane, a light and the sky |
| `materials` | The example meshes: a matt teapot, a plastic rabbit, a metal teapot, a glass knot |
| `beer` | Panes of one blue glass 5, 15 and 45 hundredths thick, and balls of an amber one of densities 0.5, 2 and 8 (Beer and Lambert), lit from behind: coloured shadows |
| `mirrors` | Two mirrors face to face, a teapot and a ball between them seen again and again until a path's turns end |
| `glow` | No light and no sky: a ball that glows, and things round it lit by it |

A line is a word and what follows it; `#` to the end of a line is not read; a name with
spaces is in quotes:

    camera target X Y Z turn DEG tilt DEG far F lens L
    background colour R G B power P
    object NAME MESH [kind matt|plastic|metal|mirror|glass|glow] [colour R G B] [rough|metal|glass|index|density|glow N]
                     [size X Y Z | scale S | fit A] [place X Y Z | move X Y Z] [turn X Y Z]
    light NAME place X Y Z radius R power P colour R G B

MESH is `plane`, `box`, `ball` or a mesh file, from the scene's folder unless it begins with
`/`. What follows an object is done in its order: `kind` sets the six numbers as the panel's
buttons do (a number after it changes one), `fit A` makes the longest side A and stands the
thing on the ground at the middle, `move` goes from there. A line that is wrong stops the
reading; the status line and the console say which and why, and what was read is kept.

File, "Save the scene..." writes the scene there is in this form, every number of every thing
written out (`size`, `place`, `turn` and the six of the material), a mesh by the file it was
read from: opened again it is the same scene, to the sum. It offers `/root/Models` and the
name of the scene last opened; Open then starts in the folder last saved to.
`NXPATH_WRITE=FILE` writes the scene the program starts with and prints `nxpath: FILE
written`: `test_all.py`'s `nxpath-saved-*` save the start and two examples and trace the file
to their sums.

- **A glowing thing is a light now** (`pt_glowing`, `glow_at` in `tracer.c`): its triangles
  are listed by area at every picture's start, and a matt or rough surface asks one of them
  for its light as it asks a light's ball (a point of a triangle picked by area, and a ray to
  it); light_at picks the glowing things as one more light. A glowing thing met after a matt
  surface then adds nothing, as a lamp does, where it was the only way its light came, a
  speck at a time. With no glowing thing nothing a path draws changes: the scene the program
  starts with is still `02f213e9`. The Cornell box's light is a glowing quad. There are no
  caustics: light by way of glass or a mirror onto a matt surface is not found (above).
- The sums (`rvc_cpu`, 2 samples, with 255 workers and with none; `tools/test_all.py`'s
  `pathtrace` tag): `cornell` `78c07525` and `cornell-balls` `b962c063` at 120 x 120, at
  160 x 120 `balls` `b85eb16e`, `materials` `5ecc7a24`, `beer` `3c29787e`, `mirrors`
  `2f211b98`, `glow` `2858a999`; `NXPATH_CHECK=500 nxpath cornell` 0 rays wrong and 0
  thousandths apart. Looked at: each at 32 samples (the Cornell box at 64, 240 x 240), and
  the menu and an example chosen from it under `rvc_harness --cpu --warp`. Not done: the
  shader machine; a glowing ball's far side is picked as often as its near one, so half of
  its rays are lost (`glow` is grainy at 32 samples); scenes are not written, only read.

Not done: textures, a mesh's own materials (`.mtl`), undo, more than
one camera. A click picks by each thing's ball, not its triangles. The handles follow the
machine's own pointer words, which `nxkey` does not move: a drag is tested with `rvc_cpu
--pointer-circle` and `--pointer-button`, and was, in a test of `gizmo.cpp` alone, not in
this program. Nothing of it has run on the shader machine.

### On the field (20 October 2026; in steps since 21 October)

Where the machine has the lanes' field (`docs/machine/lanes.md`) `nxpath` traces on it, and its
workers build the tree and fold what the field makes; `NXPATH_FIELD=0` is the workers' shares of
before. A picture's samples go in **batches** (`pt_batch_step`, `pt_batch_fold` in `tracer.c`): a
batch is one job of the field in steps, an index a pixel's sample; up to 4 samples a batch, up to the
next sample whose picture is shown. The workers fold a batch into the sums in the samples' order, every
n-th texel a worker, making the picture when it is shown; two batches, so the field makes one while the
workers fold the other.

**A path in steps.** A path is longer than a pass in a fifth to a half of a scene's instructions
(22% of the teapot's, 57% of the Cornell box's), so a lane makes it in steps (`docs/machine/lanes.md`,
"Steps"): a step goes on while the pass has 2,500 instructions left (`PT_LEAF_LEFT`, `PT_SHADE_LEFT`),
and stops where the path can be taken up again:

- **In a turn's search** (`world_nearest` with `limited`): between two boxes, the turns the search took
  kept as a bit a depth (Laine's restart trail, 64 bits: the tree is at most 60 deep), with what it found.
  Taken up again, the search goes down those turns from the top, the boxes it put by put by again, to
  the box it would have gone into next; the tree's order is the same for the same ray, so nothing is
  searched twice. (A resume from the nearest place no box left has, the first way tried, could not stop
  while a box put by held the ray's start, which near a surface is most of the time.)
- **In a light's ray** (`world_between_steps`): the same, the boxes' two in their order, with the glass
  it had crossed. The turn's shading is done again from its start with the same random numbers up to
  that ray, which is cheap and comes out the same.
- **Between two turns**, and after a search before its shading.

A path not through is six texels: where it is, its way, its light and weight, its random numbers, the
triangle and place found and how far, the trail, the glass crossed; through, its light and rays in the
first. The batches are 59 MB (two of 640 x 480 values), made when the field is first used.

**Rounds.** A round waits for its slowest lane, and the last of a batch's paths cost a round each. The
field goes on round after round while the workers fold, and when they have nothing to do it ends at the
next round's end and leaves them the paths still going (`mcw_field_steps_rounds`), which the fold makes
from where they are (`NXPATH_ROUNDS=N`: N rounds a batch, fixed).

**The self-test** (`NXPATH_PROBE=N`, with workers, `rvc_cpu` too): N paths from the picture's middle row
made in steps on a worker, each step given a pass, then traced whole: steps, the most rounds, a step's
instructions, how many went over a pass, and how many paths came out other than whole. At 160 x 120 with
3,000 paths, every scene: 0 over a pass, 0 other, the longest step 7,144 to 8,162 instructions. A step that
went over a pass on the card is made by the first core and counted: `NXPATH_PRINT=1` says how many, and
the picture's line says "steps too long for the lanes".

On the RTX 5070 laptop, 260 x 243, 64 samples, every scene the same sum on the field as on the workers:

| scene | the field, in steps | 255 workers | the field before steps (long paths skipped) |
|---|---|---|---|
| teapot | 6.5 s | 8.0 s | 4.8 s |
| cornell | 24.6 s | 27.3 s | 25.6 s |
| cornell-balls | 24.1 s | 28.0 s | 24.6 s |
| balls | 15.0 s | 14.6 s | 13.4 s |
| materials | 12.2 s | 12.8 s | 9.0 s |
| beer | 7.2 s | 9.5 s | 6.4 s |
| mirrors | 16.8 s | 20.9 s | 14.1 s |
| glow | 17.8 s | 16.4 s | 11.3 s |

The field is ahead of the workers in six scenes of eight, and behind the field of before in all but
the Cornell boxes: there the long paths were the workers' at once, beside the field, and a lane's step
leaves up to 2,500 of its pass's 8,192 instructions unused where it stops. The workers' column is a few percent over the earlier
table's; on `rvc_cpu` the workers' instructions are as before (teapot 1% fewer, cornell and glow level). On
`rvc_cpu`, which has no field, the sums are as before (`02f213e9`).

## The path tracer of before (to 13 October 2026)

What follows is the Cornell box that `nxpath` was: one baked mesh, analytic balls, a Dear
ImGui panel. Its sums and switches (`NXPATH_OLD`, `NXPATH_SCENE`, `NXPATH_DEPTH`,
`NXPATH_BOUNCES`, `NXPATH_FIRST`, `NXPATH_COUNT`, and `NXPATH_CORES` as the panel's four
layouts) went with it; the tracer's arithmetic, tiles and shares are still the program's.

`linux/pathtrace` (`build.sh` after Dear ImGui's, `linux/imgui`; Start menu: Other, "Path
tracer (every core)"). A Cornell box: a red and a green wall, a lamp in the ceiling, a block
and two balls that are matt, a mirror or glass. `tracer.c` is the tracer, plain C, and its
job is one more sample of every pixel of a tile: a path from the eye that turns up to so
many times, the lamp's light on every matt surface it meets by a ray to a point of the lamp,
a path that carries little ended by chance. The samples' sums are three floats a pixel in
memory from `mcw_alloc`; the picture is made from them as each sample comes in, so it clears
while one watches. A sample is held to 2.5 times white (the lamp itself, seen in a mirror).

**No light by way of a mirror or glass onto a matt surface.** A path that has met a matt
surface and then finds the lamp by a mirror or through glass is one in thousands and worth a
hundred others, and held to 2.5 it was still a speck: 443 pixels of 320 x 240 at 16 samples
stood more than 40 levels out of the middle of the eight round them, where a room with matt
balls has 51 (362 with the mirror alone, 138 with the glass alone; the arithmetic of before
13 October had 434). Such a lamp is not seen now: 41 specks, which is the matt room's, and
the picture 0.7% darker (level 84.5 to 83.9; at 1,024 samples 85.0 to 84.2). The lamp in the
mirror or through the glass, as the eye sees it, is as it was. Glass lets the lamp's light
through as if it were not there (it casts no shadow), so that light was counted twice
before. Measured with `tracer.c` built for the host, the sums written to a file.

`nxpath.cpp` is the panel, Dear ImGui (`imgui.md`): the picture's size (160 x 120 to 640 x 480, which is 4.2 s a sample with 63 workers), samples a pixel,
turns of a path, the lamp, the two balls' kinds, which core traced what, and how the cores
are laid out (the smallest, small or large workers, as many as the machine has room for, or
this core alone); Render, Stop, how
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

### A sample gone through (12 October 2026)

A sample of 160 x 120 on the workers: 42.5 to 28.8 million instructions with the balls, 77.3
to 61.3 with the teapot (the rabbit and the knot by a fifth too); the start with every
worker 7.8 to 5.1 million.

- `nearest` says what a ray met and how far, and the surface is made once, for what was
  nearest. A ray that starts beyond the balls and the block on some side and goes away
  skips them (`clear_of_things`; the block's top is at 0.5 itself, so a ray along it is
  still the block's).
- A box test takes lesser and greater as `fmin.s`/`fmax.s`, and a box put by keeps where
  the ray enters it, so taking it up again is one comparison; `mesh_between` tries a
  box's two together as `mesh_nearest` does. Trees with leaves of 1, 2 or 8 triangles or
  32 bins were within 1% (a leaf of 1: 3 to 5% worse) and gave the same sums.
- The panel's `sincosf` is nxpath's own: it keeps the library's answers by the angle's bits
  (the radio buttons' circles were a third of a panel frame, in doubles).
- Nothing is cleared or touched at start: a picture's own part is cleared when it is asked
  for, and its sums touched then, with workers only.
- `tracer.c` is built `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops` (6% on the
  balls, 1 to 2% on the models), `nxpath.cpp` with `-DNDEBUG`, and the link asks for LTO
  (`-flto=auto`): Dear ImGui's objects are made for it (`docs/apps/imgui.md`).

### A picture noticed when it is whole (13 October 2026)

160 x 120 with 2 samples took 268 to 277 ms on the shader machine and takes 104 to 108; 320 x
240 with 8 samples 1,486 to 1,495 ms and 1,356 to 1,366; the teapot's two samples 407 ms and
251. The sums are the same (`2c64fd53`, `27f592b2`, `83fe9a32` there). The tracing itself was
not what took the time:

- **Every worker stopped twice at the tracer's code.** With workers the first core runs none
  of it, so its two pages were pages no core had run: 510 stops, each waiting for the first
  core's next look. `pt_read_code()` reads them before the shares go out.
- **A panel frame is a tenth of a second in which nothing is noticed**, neither a worker that
  stopped nor a picture that is whole, and one began 50 ms after Render. None is drawn in a
  picture's first 150 ms nor when the samples to come are under 120 ms away (by what the
  samples so far took), for 600 ms at most.
- A worker is asked about (`mcw_done`) when its slot says it is through and every fourth
  time round, not every time: asking 255 of them is the dear part of a look.

What is left in a long picture is the tracing: 320 x 240 with 8 samples is 3.78 million
instructions for the dearest share, 461 passes, and takes 1,360 ms at 2.95 ms a pass.

Tried and not kept: every float constant taken from an integer register (`li`, `fmv.w.x`)
where the compiler loads it from memory, by rewriting its assembly. A sample then had 11%
more instructions and a quarter of the loads, which by what an instruction costs the first
core (`docs/machine/tick-costs.md`) is 6% less time; on 255 workers it was 4% more (1,422 ms for
1,365), and `nxray` 5% fewer pictures a second. A worker's time is its instructions. And
tiles of four pixels for 160 x 120 (twice as many a worker): the dearest share was 1.08 times
the mean where it is 1.14, and the picture took 117 to 120 ms where it takes 102.

320 x 240 with 8 samples on 19 October 2026 on an RTX 5070 laptop (`docs/machine/speed.md`), the tree
and the picture: 375 and 1,655 ms on the shader machine, 407 and 1,721 on `rvc_lean`, 191 and 945 on
the compute machine, 57 and 2,152 on the CPU harness.

### Fewer instructions a sample (13 October 2026, later)

A sample of 160 x 120 on the workers went from 28.9 to 19.9 million instructions with the
balls and from 61 to 50 with the teapot. On the shader machine 320 x 240 with 8 samples: the
program of the morning before 1,208 to 1,240 ms, the build above 1,097 to 1,150, this one
763 to 838; the short picture 254, 88 and 65 to 78. A worker's time is its instructions, so that is all
of it. The numbers a path draws are others now, so the sums are: on `rvc_cpu` at 160 x 120
with 2 samples `98058d33` the balls, `c190195e` the teapot, `01bda610` the rabbit, `c227a663`
the knot, the same with `NXPATH_CORES=3`; on the shader machine `2cb22f33` and, 320 x 240
with 8, `43db5431`. (Those were before the specks went, above: since then `74dbb1bc`,
`baa6fdd8`, `3f4a65e7` and `1673bf2f` on `rvc_cpu`, the balls' the same with `NXPATH_CORES=3`;
the shader machine's have not been taken again. `NXPATH_OLD=1` is still `84d4eafd`.)

- **`NXPATH_OLD=1` is the arithmetic of before**, with its sums (`84d4eafd` and the models'
  above, `2c64fd53` and `27f592b2` on the shader machine): `tracer.c` is built twice, the
  second time with `PT_OLD` as `pt_tile_old` and `pt_share_old`.
- **A tile's code is made twice**, for the balls and the block and for a model
  (`tile_of_balls`, `tile_of_model`): each has only its own things to try and to keep in
  registers. 17% of a sample with the balls, 13% with the teapot, the same sums.
- A number is a product and a sum (`next`), where three shifts were six instructions, and
  a pixel's first is mixed twice (a step apart, every pixel's n-th would be its neighbour's
  and a constant); a turn's new way is a point of the sphere by Marsaglia's two numbers and
  a root, where the cube took six and a quotient; one over the ray's way is worked out once
  and a wall or a side of the block is a product (`block_span`: lesser and greater as an
  instruction each); the room is the first of the three walls ahead, with no test of each.
- Only a ray from above the things going up is spared them (the four tests of the sides
  cost more than they spared: 2.4%).
- The picture is made of the sums after the first two samples, when their number has
  doubled or is a multiple of sixteen, and at the last (`shows`); a share that ends between
  shows what it has. The first core alone shows every sample.
- Every worker began on a page of code the first core had not run (`mcw_entry`, which the
  link had put beside the old tracer's): `nxpath` reads it before it opens them.
  `NXPATH_COUNT=1` prints the pages brought for workers: 0.

That it is the same picture: old and new at 16 and at 64 samples differ by 12.1 and 6.4
levels, root mean square (the noise of each, halved by four times the samples), their
8 x 8 blocks' means by 1.6 and 0.8, and each channel's mean by under 0.1 of a level
(`95.8, 88.2, 70.1`). The tree of boxes is as it was checked (1,638, 1,686, 1,698).

Tried and not kept: the tests against constants as whole numbers' (7% more instructions);
the lamp's light left out before the root where the surface faces away (1% more: the
compiler's registers); `-fira-loop-pressure` is 2% fewer and is in `build.sh`. A quotient or
a root costs a worker what a product does, and a load less: one over the way is not faster
for being products.

### Meshes

What stands in the box is the panel's choice ("in the box", `NXPATH_SCENE=N`): the two balls
and the block (0), or a model of triangles with one ball in the near corner: the Utah teapot
(1; 6,320 triangles), the Stanford rabbit at its third size (2; 3,851) or a torus knot made
by arithmetic (3; 5,040). The model's material is the first of the panel's two (matt, mirror
or glass; `NXPATH_FIRST=N`), the ball's the second.

- `linux/pathtrace/make_meshes.py` makes them at the build, on the host: it reads the teapot
  (an OBJ file of Stanford's CS148 course) and the rabbit (the scanning repository's
  `bun_zipper_res3.ply`), which `build.sh` fetches once, turns and sizes each to where it
  stands in the unit box, winds every triangle outward, gives every corner the mean normal
  of the faces round it, and writes `meshes.c`: a triangle as a corner, its two edges and
  three normals (18 floats, written as their bits), and a tree of boxes over them.
- The tree is split where the two halves' surfaces times their triangles are least (twelve
  places an axis tried), down to four triangles a leaf: 3,835 boxes for the teapot, 15 deep.
  A box is 32 bytes: its corners, and either where its triangles are and how many, or
  where its two boxes are.
- `tracer.c` goes down it (`mesh_nearest`): both of a box's two are tested, the nearer is
  gone into first and the farther kept on a stack of 64, and a kept box is tested again
  when it is taken, against what has been found since. A triangle is Moeller and
  Trumbore's test on the kept edges. A shadow's ray stops at the first triangle
  (`mesh_between`). A ray is 1.5 to 2 times what it is among the balls: 240 x 180 with 12
  samples is 1.7 s with the balls and 3.4 to 4.5 s with a model, 255 workers.
- The models are `const`: pages of the program's file, never copied. A worker stops at a
  page nobody has read, so `nxpath` reads the chosen model's boxes and triangles when a
  picture is asked for (`read_model`), and touches that picture's part of the sums then.
- To check the tree: `NXPATH_DEPTH=1 NXPATH_PRINT=1 NXPATH_SAMPLES=1 NXPATH_SCENE=N nxpath`
  prints how far every pixel's ray goes, and `python tools\pathtrace_reference.py meshes.c
  teapot LOG` tests every ray against every triangle with no tree: every ray that meets the
  model must have the guest's distance (1,638 for the teapot, 1,686 the rabbit, 1,698 the
  knot, at 160 x 120). `NXPATH_PRINT=1` alone prints the picture.
- The sums (`rvc_cpu`, 160 x 120, 2 samples): `84d4eafd` the balls, `40792470` the teapot,
  `46a882a3` the rabbit, `d29e8be5` the knot; with `NXPATH_CORES=3` the same. The shader
  machine's differ (its divisions and square roots are the card's).

## Checking them

A picture's sum does not depend on which cores traced it, or how many: run one with
`RAY_WORKERS=0` (or `NXPATH_CORES=0`) and one with all of them. `rvc_harness --cpu` runs both
at once the speed for looking at them (`cpu-harness.md`); the times above are the shader
machine's (`rvc_harness`).
