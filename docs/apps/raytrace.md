# Rays on every core

Two programs made for the machine's worker cores (`multicore.md`), and for the small ones above all: a
pixel of a traced picture is a few thousand instructions of float arithmetic and a handful of bytes
stored, so a core needs next to no write cache for it.

`nxray` traces on lanes, a texel of four pixels an index: the field's or its workers'
(`docs/machine/lanes.md`; "Lanes", below). `nxpath` traces in tiles on the workers, or in steps on the
field ("On the field", below). A tile is 32 x 32 pixels, halved until every worker has two dozen tiles of
the picture and never narrower than 8. A tile's rows begin and end on 16 bytes of the picture (a tile is a
multiple of eight pixels of four bytes across, the picture's width a multiple of four), so no two cores
store to the same 16 bytes. Everything else a job writes is its own; the scene is only read.

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
its size and the picture is stretched over it. The keys are looked at after every picture.

`RAY_FRAMES=N` leaves after N pictures, `RAY_WORKERS=N` sets the start (every worker has its share of a picture in one job: `docs/machine/multicore.md`, "A share, not a tile"),
`RAY_SIZE=WxH` the picture, `RAY_STILL=1` keeps the scene where it is. Each picture's line
(`raystat:`) has its time, its rays and a sum of its pixels.

The first core traces nothing while it has workers.

### What a picture costs

`RAY_FRAMES` on `rvc_cpu` and calibrated, 31 October 2026. The calibrated time rests on a workers term
fitted to one game, so it is rough; `rvc_cpu` has no lanes and makes the same pixels by plain calls.

| | a picture on 255 workers (calibrated) | on all cores | on core 0 alone |
|---|---|---|---|
| 320 x 240, some 200,000 rays | 34 to 52 ms | about 28 million instructions | 26 million instructions |
| 640 x 480, some 815,000 rays | 148 to 220 ms | about 105 million | |

The sums of `RAY_FRAMES=4`: `53ea472d`, `226b5e92`, `37366b32`, `3ffdbe63` on `rvc_cpu`, with every worker and
with none (`tools/test_all.py nxray`, `nxray-none`); on the card they are the card's, the same on the field, on
the workers' lanes, with `RAY_LANES=0` and with no worker.

### A ray tries only what it can meet

- Core 0 works out per picture where each sphere can be on the screen (between its tangents
  from the eye, four pixels more) and where its picture in the ground can be (the sphere
  mirrored under the ground: a ray the ground throws back meets a sphere exactly when the
  eye's ray meets that image); a texel's row lists the spheres of each kind that cross it.
- A point of the ground asks a 16 x 16 grid which spheres' shadows can reach its square (a
  shadow is within radius / light.y of where the light through the centre lands).
- It is built `-O2`: `-O3` is worse here.

After a change, old and new must agree on `RAY_FRAMES=24 RAY_SIZE=160x120 nxray` (a moving
scene), not only on the still picture.

### Pictures one after another

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
- `RAY_EVEN=1` keeps the bands even, `RAY_SHARES=1` prints what the shares took.
- **A worker with no picture ends its pass** (`mc_end_pass()`, `multicore.md`); `RAY_SPIN=1` turns in place.

### Lanes

`nxray` traces on lanes (`docs/machine/lanes.md`): `trace_texel(i)` makes texel `i` of the picture, four
pixels side by side (the spheres a texel's row and four pixels can
meet, then `trace()` for each), and stores its 16 bytes, with the rays it cast in its first pixel's top
byte (which the display does not draw and the sum leaves out).

- **On the field** where the machine has one (the lean tick in the harness): a picture is one job of
  its lanes (`mcw_field_begin`), posted by the first core as soon as the picture before is
  through, while it shows that one and makes the next scene. No worker is opened. `RAY_FIELD=0`
  takes the workers instead.
- **On the workers' lanes** elsewhere: a worker's share of a picture is a band of texels, one job of
  its lanes (`mcw_lanes`); `RAY_LANES=0` makes its texels one after another (`rvc_cpu`, which has no
  lanes, does that anyway). The bands are cut again one picture in four by what each took: a texel
  costs what its band cost over its length, made smooth over the bands round it and at least a sixth
  of a pass's instructions (a lane makes six texels a pass: `RAY_FLOOR`), and a band is given as much
  of the cost as its worker's lanes make (27 or 6) while the others make theirs, half way from where
  it was.
- **The first core's part** is small: the scene's sines in floats (`sin_cos`: the C library's are
  doubles), the bands not cut again three pictures in four (their cut in floats), the jobs asked about every 250 ms, not 50, and up to four
  pictures out at once where GPU memory has room (`OUT`; a third and fourth picture are of the
  picture's own size, so 640 x 480 has two).
`RAY_SHARES=1` prints each picture's shares (the workers'), or on the field what the first core's part
of the picture took and how many texels the lanes left it.

## `nxpath`: a mesh viewer with a path tracer

`linux/pathtrace` (`build.sh` after Dear ImGui's; Start menu: Other, "Path tracer (every
core)"; `nxpath [MESH]`, and Files opens an `.obj` or `.ply` with it). It is a small scene editor: meshes are read from files, put into a scene with lights, shown
by OpenGL, and path traced by the worker cores under a tree of boxes that they built.

- **The scene** (`scene.h`): meshes as read (`mesh.c`: Wavefront `.obj`, Stanford `.ply` as
  text or little-endian bytes, faces of any size, normals made where the file has none; a
  plane, a box and a ball made here), objects that place a mesh (place, turn, size; a
  material, six numbers: "What a thing is made of", below; a colour), up to
  64 objects and eight lights (balls that shine), one camera that goes
  round a point, and the sky's light. It starts with a ground, the teapot, a mirror ball and
  a light. The example meshes are `/root/Models/teapot.obj` (6,320 triangles), `bunny.ply`
  (3,851) and `knot.obj` (5,040, `make_knot.py`).
- **The window** (`nxpath.c`): menus along the top (File, Add, View, Render), the view on the
  left, a panel of the window system's own on the right (`ui.h`), and along the bottom a line
  of figures with the picture's bar. The panel is the scene's list and three tabs, laid out as one table of rectangles that is both what is drawn
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
  made of the pointer and the picture's matrices, drawn over the picture. ImGuizmo is fetched at a commit
  and has three circles' side counts changed at build time (`build.sh`). A frame is drawn only when something changed or the
  pointer moved in the picture.
- **The world** (`bvh.c`): every object's triangles where they stand, 80 bytes each, under
  one tree of boxes of at most four triangles a leaf. The first core cuts the triangles into
  parts by a grid, the workers build a binned tree a part in memory that is each one's own,
  and the first core builds the tree over the parts. It is the same tree whichever cores
  built it. The first core's own part is what is left, so many more workers gain nothing.
- **The tracer** (`tracer.c`): the tree and the lights' balls, a light picked by chance at every matt surface and a ray to a point of it,
  the sky for a ray that meets nothing, glass that casts no shadow, and no lamp seen after a
  matt surface by way of a mirror or glass ("What a thing is made of, and the light", below). A sample is
  held to 2.5 times white. No light by way of a mirror or glass onto a matt surface is counted: such a
  path is one in thousands and is a speck.
- `pt_read_code()` reads the tracer's pages before the shares go out: with workers the first core runs none
  of that code, and a worker stops at a page no core has run. `tracer.c` is built `-O3 -fno-tree-vectorize
  -fsched-pressure -funroll-loops -fira-loop-pressure`: a worker's time is its instructions, so float constants
  from integer registers and such tricks for the first core's loads do not pay here.
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
has the tree's time, the picture's, its rays and its sum. On `rvc_cpu`, 160 x 120 with 2 samples of the
scene it starts with (7,282 triangles): sum `d63313e9` with 255 workers and with none. 320 x 240 with 8
samples: the tree 179 ms by 227 workers, the picture 1,439 ms on 255, 1,306,934 rays, 850 million
instructions on all cores in the whole run (calibrated, 31 October 2026; the workers term rests on one game).

### The view is the picture

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

Over 1,000 rays the two cameras must be 0 thousandths of a pixel apart. The panel in each state: no
control over another, none with words too wide. Not done: the handles dragged over a traced picture, and a
picture larger than the view (a window made larger is the way to one).

### What a thing is made of, and the light

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
  side a ray is on is the flat triangle's to say, turned the way its corners' normals are. Light to a surface through
  glass is less the same way (the lengths inside from where the ray leaves less where it came
  in) and by a twelfth a surface: glass has a shadow of its colour. No light gathered by it.
- **A light's light at a surface** (next event estimation, `light_at`): a ray into the cone
  the light's ball fills from there, evenly by solid angle. `power` is of the whole ball (`shine`): a
  smaller light is as bright on the ground and brighter to look at. Lights start at 0.12 and
  0.1 of radius.
- **The background gives no light** unless asked: its colour and power are the Render tab's,
  and the registry's `pathtrace.background` and `.background.colour`.
- **Turns** are 1 to 16, 8 at first (glass uses two a thing), and from the third on any path
  that carries little ends by chance, not only one off a matt surface.
- **The panel** has a third tab, Material: the six starting points, the colour, and a slider
  with its number for each of the six.

Not done: a roughened glass, a glass mesh that is not closed, more than one light; a rough shine's light is
weighed by a rule (Phong's) that is not the one its next way is drawn by, and nothing has compared it with a
tracer known to be right.

### Example scenes

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

- **A glowing thing is a light** (`pt_glowing`, `glow_at` in `tracer.c`): its triangles
  are listed by area at every picture's start, and a matt or rough surface asks one of them
  for its light as it asks a light's ball (a point of a triangle picked by area, and a ray to
  it); light_at picks the glowing things as one more light. A glowing thing met after a matt
  surface then adds nothing, as a lamp does. The Cornell box's light is a glowing quad. There are no
  caustics: light by way of glass or a mirror onto a matt surface is not found (above).
- The sums (`rvc_cpu`, 2 samples, with 255 workers and with none; `tools/test_all.py`'s
  `pathtrace` tag): `cornell` `78c07525` and `cornell-balls` `b962c063` at 120 x 120, at
  160 x 120 `balls` `820f06bc`, `materials` `5ecc7a24`, `beer` `3c29787e`, `mirrors`
  `2f211b98`, `glow` `2858a999`; `NXPATH_CHECK=500 nxpath cornell` 0 rays wrong and 0
  thousandths apart. Not done: a glowing ball's far side is picked as often as its near one, so half of
  its rays are lost (`glow` is grainy at 32 samples).

Not done: textures, a mesh's own materials (`.mtl`), undo, more than
one camera. A click picks by each thing's ball, not its triangles. The handles follow the
machine's own pointer words, which `nxkey` does not move: a drag is tested with `rvc_cpu
--pointer-circle` and `--pointer-button`, which has been done on `gizmo.cpp` alone, not in this program.

### On the field

Where the machine has the lanes' field (`docs/machine/lanes.md`) `nxpath` traces on it, and its
workers build the tree and fold what the field makes; `NXPATH_FIELD=0` takes the workers' shares
instead. A picture's samples go in **batches** (`pt_batch_step`, `pt_batch_fold` in `tracer.c`): a
batch is one job of the field in steps, an index a pixel's sample; up to 4 samples a batch, up to the
next sample whose picture is shown. The workers fold a batch into the sums in the samples' order, every
n-th texel a worker, making the picture when it is shown; two batches, so the field makes one while the
workers fold the other.

**A path in steps.** A path is often longer than a pass, so a lane makes it in steps (`docs/machine/lanes.md`,
"Steps"): a step goes on while the pass has 2,500 instructions left (`PT_LEAF_LEFT`, `PT_SHADE_LEFT`),
and stops where the path can be taken up again:

- **In a turn's search** (`world_nearest` with `limited`): between two boxes, the turns the search took
  kept as a bit a depth (Laine's restart trail, 64 bits: the tree is at most 60 deep), with what it found.
  Taken up again, the search goes down those turns from the top, the boxes it put by put by again, to
  the box it would have gone into next; the tree's order is the same for the same ray, so nothing is
  searched twice.
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
3,000 paths the scene it starts with says 0 over a pass, 0 other, the longest step 7,926 instructions
(`rvc_cpu`, 31 October 2026); every scene must say 0. A step that
went over a pass on the card is made by the first core and counted: `NXPATH_PRINT=1` says how many, and
the picture's line says "steps too long for the lanes".

`rvc_cpu` has no field: how the field compares with the workers wants the card, by scene, as a lane's step
leaves part of its pass's 8,192 instructions unused where it stops.

## Checking them

A picture's sum does not depend on which cores traced it, or how many: run one with
`RAY_WORKERS=0` (or `NXPATH_CORES=0`) and one with all of them. `rvc_harness --cpu` runs them at the
speed for looking at them (`cpu-harness.md`). `python tools/test_all.py workers` has both, and the `pathtrace`
tag every example scene.
