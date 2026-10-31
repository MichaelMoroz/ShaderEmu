# Tiberian Dawn

Command & Conquer (1995) runs in the Linux image: [Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer),
the portable version of the game's released source, as a Nano-X client. The GPU draws the map:
terrain, objects, shadows and the shroud ("The map on the GPU", below). The game still draws
its sidebar, its text and its menus itself, on its own 320x200 screen, which the GPU shows on top.
The window is the largest whole multiple of that the display has room for: 960x600 on the
desktop's 1280x720, which costs the game nothing (the GPU does the stretching).

    wsl -- bash linux/nommu/build.sh nanox/build.sh     (once)
    wsl -- bash linux/nommu/build.sh tdawn/build.sh     (a minute when a file changed; the first time it also fetches)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                  (then a new shell snapshot)

In the guest: `nano-X -p & tdawn` at the console (it is also listed for the desktop's Start
menu). The data is the 1995 demo's (GDI missions 1, 3, 5, 6 and 10; the menu starts the first),
which the build fetches and the repository does not keep. Effects and speech play on the sound
card (`docs/machine/sound.md`); there is no music in the missions (the demo has none), only the map
screen's and the score screen's tunes (`MAP1.AUD`, `WIN1.AUD`).

## What the build is made of (`linux/tdawn`)

- `build.sh` fetches Vanilla Conquer at a fixed commit, applies `vanilla-conquer.patch`, adds
  the files below and builds with CMake and the image's toolchain. The tree is put back on every
  build and Red Alert's build shares it: each game builds from a copy of its own
  (`vanilla-conquer-td`, `-ra`, by `rsync -c`), where a file keeps its time unless its bytes
  change, and the GPU library's object is replaced only when it differs.
- **No C++ library.** The toolchain's libstdc++ is for glibc and an FPU. The game uses neither
  exceptions nor run-time type information, and little of the library: g++ is given
  libstdc++'s headers only, over musl, and `cxxrt.cpp` is the rest (allocation, the string
  class's code, what a failure calls). `cxx/bits/c++locale.h` stands in for the one header
  that needs glibc's types.
- `host.c` is the machine: a Nano-X window, two 8-bit pages and a pointer picture in GPU
  memory, the palette, events. C, because the Nano-X and OpenGL headers are.
- `shaderemu.cpp` is the game's side of it: what `video_sdl2.cpp` and `wwkeyboard_sdl2.cpp`
  are for SDL.
- `-fno-lifetime-dse`: the game's `operator new` marks an object active before its constructor
  runs. A compiler may drop that store; a native `-O3` build without the flag loads a mission
  with no walls and no tiberium.
- **The whole program at once.** The C++ is compiled `-O3 -flto` with far more inlining than
  the default allows (`LTO` in `build.sh`): the game's logic is hundreds of small functions
  calling each other across source files. Two flags are not optional: `-flto-partition=1to1
  -fno-toplevel-reorder`. The game's globals are constructed in the order of its source files
  and depend on it; with the default partitions the program ends at its start with "out of
  memory" (a heap sized from a global that is not made yet). The C files (`host.c`, the workers'
  library) are left as they are: `mcw.c` is in two targets, which LTO takes for one symbol
  defined twice.

## What was changed for this machine

- **The clock.** The game reads the machine's clock word and divides in 32 bits: it asks the time
  constantly.
- **Showing a frame.** The game finishes a frame by copying its hidden page to the visible
  one, 64,000 bytes. Both pages are GPU memory: the copy is put off, the hidden page is
  shown, and the copy is made only if something reads the visible page or draws on it
  (`Catch_Up` in `shaderemu.cpp`; the figures say how often).
- **Finding files.** Only the file's own name is matched, not every spelling a folder at a time from
  the root down, and the starter runs the program from a folder of its own (the game lists its
  program's folder too). The program is `/usr/lib/tdawn/game` in the ROM, run where it lies and not
  copied to `/tmp` (`docs/system/nommu.md`, "Memory"); `/tmp/tdawn`, where it runs, has links to the data.
- **Waiting** ends the machine's frame with `pause`; the screen is shown at most 60 times a
  second unless the game asks.
- **Twenty steps a second.** The game's default speed is 15 steps of its logic a second, and
  everything on the map moves once a step: at 15 it looks slow whatever the machine draws.
  The default here is one notch faster, 20 (`Options.GameSpeed` 3; the game's own slider
  still sets it).
- **Frames left undrawn** (`TDAWN_SKIP=N`, off unless asked for). A frame that takes longer
  than the game's speed allows leaves a debt; while it is more than half a frame the next
  frames are not drawn (N in a row at most, never while the map scrolls). The game keeps its
  speed and what moves is seen to move less often, which is why it is off.
- **Logic that repeats itself**, each with the state sums unchanged: a unit that steps is lifted from
  the map and put down, and each half adds or takes its threat from nine regions of every enemy house:
  the lifting is held back and the two cancel (`CellClass::Adjust_Threat`). A house is found by its
  kind from a table, not a search (`HouseClass::As_Pointer`). The layer's sort asks each object where it
  sorts once, not twice.
- **A scan for targets reads bits.** `Greatest_Threat` goes round its unit in growing boxes
  and asks every cell of each, and nearly all are empty. The map keeps a bit a cell that has
  an occupier, once by rows and once by columns (`cell.cpp`, `cell.h`): a side of a box is a
  word or two of them, and only the cells whose bits are set are asked, in the order the
  game's loops ask them (top before bottom for each x, left before right for each y: the
  scan keeps the last target it finds, so the order is the answer). The bits follow every
  store to a cell's `OccupierPtr`: `Occupy_Down`, `Occupy_Up`, the dead occupier that
  `Cell_Occupier` drops, the cells made anew (`Init_Cells`) and a saved game's
  (`MapClass::Decode_Pointers`). `TDAWN_CHECK_OCCUPY=1` compares them with the cells every
  frame and says how many were wrong: it must say none. A target scan's cell is asked without
  asking again whether it is on the map, which the box was already kept to (`SHADEREMU_THREAT_AT` in
  `techno.cpp`), and `Evaluate_Object` measures the distance after the tests that only say no.
  Keeping a scan's answer with everything it read, as Doom's sight checks are, costs more in notes
  than the answers save.
- **Scrolling.** `ShaderEmu_Scroll_Distance` grows a step with the time since the last one, up to 36
  pixels (beyond that the game draws the whole map again instead of shifting it); only `TDAWN_SCROLL`
  uses it.
- **The map is dragged.** The game's scrolling at the picture's outermost row or column, which a mouse
  finds by being stopped there and a beam does not, is gone in both games (`ScrollClass::AI`,
  `shaderemu.cpp`). What moves the map:
  - The right button (the grip) held on the map and moved drags the map, a pixel for a pixel.
    The game gets that button's press at its release, and only when the pointer stayed
    within a 40th of the picture's width: a press that did not move is the game's right
    click. A press that begins off the map (the sidebar) is never a drag.
  - The drag follows the pointer only while the machine's own buttons word says the button
    is down (`host_buttons`): the window system's event of the release comes later.
  - While the map is dragged the pointer is the game's scroll arrow for the way the view
    goes (against the hand), and its "no" arrow where the map ends.
  `TDAWN_INPUT_LOG=1` prints each drag step and its arrow. To test without hands, under `rvc_cpu`:
  `--pointer-sweep 0 --pointer-button 2,52,54` drags (the pointer goes round the middle of
  the display, which is on the map), and `--pointer-circle 40,250,40` without a button goes
  in and out over the window's left edge, which must print nothing.
- **Events that are owed.** `host_event` asks the window system when an input word changed, and every
  10 ms while an event the words told of has not come, for a tenth of a second at most; otherwise once
  a second, unless the pointer is outside the picture.
- A page's blit onto itself (how the map is shifted) copies rows in the order that reads each
  before it is written.
- **What a frame does not do**, each with the state sums unchanged:
  - An object that moved does not ask every cell it left and came to to be drawn again:
    `MapClass::Place_Down`, `Pick_Up` and the overlap lists only tell the scene something near it
    changed (`SHADEREMU_MOVED` in `map.cpp`, as Red Alert has).
  - `FlasherClass::Process` looks at the largest of twenty houses' counters, kept beside them.
  - `MapClass::Logic` passes over a cell with nobody in it and no Tiberium before asking whether Tiberium
    grows there or a tree spawns it.
  - `LogicClass::AI` times every object's turn for `TDAWN_SLOW`'s report only when that is set.
  - The layer's sort keeps a building's and a tree's place (`ShaderEmu_SortKey` in the object, worked out
    again when its coordinate is another): they are most of the layer.
  - A message over the map stays in the page as printed until the messages are others or holes were made in
    the page (`ShaderEmu_GL_Messages` in `gl.cpp`, `MessageListClass::ShaderEmu_Signature`). A message that
    expires has its rows made holes and what is left printed again. `TDAWN_MESSAGE_GONE=redraw` is the game's way.
  - `BF_Trans`, which draws every shape the game still draws itself (the sidebar, its tabs), copies runs of pixels
    and passes nothing over a word at a time (`common/keybuff.cpp`). A shape from the uncompressed shapes' buffer,
    which stays as it is, has each row's first and last pixel kept and whether all between are pixels
    (`ShaderEmu_Spans_Of`): drawn again it is a copy a row.
  - `fixed(a, b)` divides in 32 bits when both fit (`common/fixed.cpp`).

## The map on the GPU (`gl.cpp`, `host.c`)

The map is a scene of rectangles the GPU draws, and the game's page goes over it with holes where the map
is (index 0, which the game never draws).

- **Terrain stays.** Each cell of the map is a quad that is written once, when the cell is
  first seen, and kept (`seglKeep`): its picture is one of 1,482 icons in an atlas (each in 26
  x 26 with a gutter: "A scrolled picture" below). Scrolling is a matrix: the same quads somewhere else,
  and nothing of the terrain is touched.
- **Everything else is rectangles of each frame**, in the order the game draws: overlays and
  smudges (the cell's own `Draw_It`, without its terrain), then every object of every layer
  (`Render`), then the shroud. `CC_Draw_Shape` is where they turn into rectangles: the shape's
  frame is put into the atlas the first time it is drawn (with its house colours, which are
  a table) and is a quad from then on. Only the part of a frame that has pixels is kept (a
  soldier is a small figure in a much larger frame), which is what the atlas has room for.
- **An object that did not change is not drawn again.** Its rectangles are remembered by
  their place on the map (`Kept`, six an object) and while the game has not marked the object
  (`IsToDisplay`) the scene is only told where they are kept (`host_quads`: the worker core
  reads them there, so nothing in `gl.cpp` may change `kept` before `host_scene_settle()`);
  one that was out of the view is passed over until the view moves, by the view the object
  itself remembers it drew nothing at (`ShaderEmu_Away`). The object says where its rectangles
  are kept (`ShaderEmu_Kept`). The shroud's edge over a cell is worked out again only when the
  game says the cell changed. In case a change went unmarked, every 256th scene has its cells from
  nothing, and every scene one object of each layer in turn; the page is not filled for it.
  (`TDAWN_REFRESH=N` other than 256: every Nth scene whole.)
- **What is under the objects and over them stays.** A scene is three parts
  (`host_scene_part`): the cells' own rectangles, the objects, the shroud. The first and the
  last are noted once and kept while no cell of the view has changed (`ShaderEmu_GL_Cell`,
  which `Redraw_Objects` and `Flag_Cell` call) and the view has not moved.
- **A scene is made again only for a change in it.** The game marks an object that changed and
  asks its cells to be drawn again, which here paints over nothing: `Refresh_Cells` and
  `Redraw_Objects` only note whether the place is in the view or within five cells of it, and
  a frame in which nothing there changed shows the scene it has (`ShaderEmu_GL_Changed`).
- **Shadows** are the pixels the game's translucency table would darken. They are a second
  rectangle from a mask of one bit a pixel, in black with the transparency the table has, under
  the shape's own. The shroud's edges are pictures of words with a transparency each; cells
  never seen are black rectangles, one a run.
- **The page on top.** What the game still draws over the map itself (text, the band being dragged,
  the placing cursor's cells when the atlas is full) lands in the page and shows over the scene. The
  page's part over the map is made all holes again only after something was drawn there.
- **The pointer is the display's cursor** (`pointer_publish()` in `host.c`,
  `docs/machine/display.md`): its picture, 32 x 32 colours made from the game's 8-bit one with the
  palette, is given to the window system as the window's cursor (`docs/system/nanox.md`), one
  cursor for each hot spot the game has used, and whatever shows the display draws it at the
  pointer at its own rate. It is a rectangle of the scene where the picture is stretched over a larger
  window or the game's pointer is over 32 pixels, under the window system's arrow; `TDAWN_POINTER=drawn`
  is that everywhere. The picture is made again when a colour it used has changed in the palette (a fade).
- **Where the pointer is** comes from the machine's input words, not from the window system
  (`pointer_read()`, `docs/machine/input.md`).
- **A frame with nothing new** (the pointer moved, a dialog is open) draws the last list
  again (`seglSwapAgain`): its textures are the page's memory, and the pointer's corners and
  the page it shows are changed in place.
- All sprites are in the blended pass with no depth, so that a shape and its shadow keep the
  game's order. What does not fit (a shape with the cloak effect, a full atlas: "A long battle", a map of
  more than 64 x 64 cells) the game draws itself, into the page; `TDAWN_RENDER=soft` makes it all.

Against the game's own drawing the picture differs in the shadows' and the shroud's shade
(a true blend, where the table picked the palette's nearest dark colour): some of the pixels by a few levels.

## Speed

The game wants 20 frames a second here, and a frame of the game is one step of its logic and one drawing of
what changed. `game_par.py tdawn --calibrated` (31 October 2026), mission 3, seed 7, frames 301 to 1200, nobody
holding them back; beside it the card:

| | Game frames a second | Passes a frame | Core 0 a frame | All cores a frame |
|---|---|---|---|---|
| calibrated, 31 October 2026 | 60.8 | 10.68 | 80.1k | 126.6k |
| card, RTX 5090, as the model was fitted (`game_par.py tdawn --card`) | 60.4 | | | |

A mission's first few hundred frames decode its shape files ahead on a worker: a frame summed over all cores is
measured from frame 301 on.

Each run prints where a frame went (`tdstat:` lines, every five seconds):

    TDAWN_STATS_MS=N    how often (0: never)
    TDAWN_FRAMES=N      N game frames with no delay between them, then leave
    TDAWN_STOP_FRAME=N  hold the picture of game frame N; TDAWN_NO_DELAY=1 gets there with no delay
    TDAWN_SCEN=N        start at mission N
    TDAWN_SEED=N        the same game every run
    TDAWN_AUTO=1        Return is pressed through the menus (with FROMINSTALL: straight to the mission)
    TDAWN_INPUT_LOG=1   print the keys and buttons that arrive, and where in the window
    TDAWN_RESIZE=WxH    give the window that size after its 60th frame, as a drag would
    TDAWN_SCROLL=N      scroll the map for N frames from frame 20, twelve each way: east, south, west, north, then the four diagonals
    TDAWN_SELECT=N      every N frames the player's units and soldiers are selected (their bars and corners are drawn into the page)
    TDAWN_MOVE=N,DX,DY  at frame N the player's units and soldiers are selected and sent, as by a click, to the cell DX,DY from the view's middle (TDAWN_MOVE_UNSELECT=1: unselected after; Red Alert too)
    TDAWN_DRAG=N[,S]    from the Nth frame the map is dragged a pixel a frame, every place of a cell and the map's edges (from step S)
    TDAWN_SUMS=1        after every frame, a sum of the map's part of the picture as the program has it ("Checking it")
    TDAWN_CHECK_GPU=1   after every frame, the GPU's picture in the window's buffer against the program's ("Checking it")
    TDAWN_SAY=1         in a mission, EVA says each line the data has, one after another
    TDAWN_SOUND_LOG=1   print every sound the game asks for, and whether the pack has it
    TDAWN_BUILD=N       put a refinery in view at frame N, which plays its construction
    TDAWN_CHECK_REDRAW=1  at the held frame, count the pixels a scene from nothing would change ("Checking it")
    TDAWN_PICTURE=1     at the held frame, the picture as the program has it, to the console ("Checking it")
    TDAWN_CHECK_OCCUPY=1  every frame, count the cells whose occupancy bit is not the cell's own
    TDAWN_SKIP=N        how many frames in a row may go undrawn (0)
    TDAWN_RENDER=soft   the game draws the map itself
    TDAWN_REFRESH=N     make every Nth scene from nothing (256; 0: never)
    TDAWN_SLOW=N        with TDAWN_STATS_MS=0: say each frame of more than N thousand instructions, and where they went
    TDAWN_PATH_CORES=N  worker cores for a unit's other path searches (3; 0: this core makes them all)
    TDAWN_PRE_CORES=N   worker cores that ask about the map's cells ahead of a long search (32)
    TDAWN_WALK=old      the game's own walk round an obstacle (Follow_Edge)
    TDAWN_FRAME_LOG=1   at the end, every frame's instructions: all, logic, drawing, the map and its parts, shapes put
    TDAWN_MARK=A,B      a line as frame A begins and as frame B does (rvc_cpu --pc-from ": mark begins" --until ": mark ends")
    TDAWN_MESSAGE_GONE=redraw  a message that expires clears the page and has everything drawn again
    TDAWN_FRAMES_CORE=N  no more than N cores unpack shape files (6; 0: the game builds every frame)
    TDAWN_READY_DRAW=0  the game's own drawing unpacks its shapes itself
    TDAWN_SPRITES=one   the scene's rectangles go to the GPU's library one call each (seglSprite)
    TDAWN_CHECK_LIST=1  at the end, a sum of every scene's vertices and draw commands (the same both ways)
    TDAWN_CHECK_SORT=1  the ground layer's pass made by a worker is made here too and compared (RALERT_CHECK_SORT)
    TDAWN_THREAT=here   the houses' threat is added on this core; TDAWN_CHECK_THREAT=1 sums it where read
    TDAWN_DRAW=here     the objects are drawn by this core, not a frame behind by a worker (RALERT_DRAW)

### Why a frame is not half

A frame's largest part is logic, and it has no large piece: every object's turn reads what the turn before it stored and
takes its random numbers in turn, and the state sums are the test. What a worker can make exactly is something whose inputs
do not change while it works and whose result is waited for or thrown away: the path searches are that, and what reads a
state that holds still for two passes, the drawing's time (the sort) or the next frame (the threat). A target scan or a unit's
search made ahead would need every store that could change its answer told of. Copying an object for the objects' worker costs
about what drawing it did, and the walk through the layers that says what to copy is the first core's. The rest is the kernel:
its tick, mostly (`AGENTS.md` has why the tick is 100 Hz).

What the long frames are (`TDAWN_FRAME_LOG=1`, three stores a frame; `TDAWN_SLOW` times every object's turn and prints, which makes a frame
longer):

- **A unit's path search**, the first core's own first search (below): one long walk, which the unit's turn waits for.
- **Red Alert's landing** and **an aircraft turned** (`docs/games/ralert.md`).

Not done: a unit's search a frame ahead on a worker (its inputs are what `Can_Enter_Cell` says of the cells it asks about, as if the unit were
lifted already; the unit, its cell and its destination have to be guessed a frame before); the turning of a shape on the scene's core; the cells'
part of a scene made again only for the cells that changed.

Profiles: a battle's flat profile hides what a whole-program build inlined; instructions by the function a line was written in (`addr2line -f -i`
over a count for every pc, or `tools/prof.py`, `docs/machine/profiling.md`) see through it.

## Checking it

The game is integer-only and, with a seed, plays the same on any CPU. Both ends print a sum
of the game's state (every unit's, soldier's and building's place and strength, and the map's
overlays): build the patched tree natively with a `host.c` that only writes the shown page
to a file, hold both at one frame, and compare the sums and the pictures.

    TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=10 TDAWN_STOP_FRAME=300 tdawn FROMINSTALL
    tdawn: stopped at frame 300: SCG10EA, 15 units, 23 infantry, 38 buildings, 552 overlays, state f7650705

`tools/test_all.py`'s `tdawn-*`: `tdawn-300` (mission 10, `f7650705`), `tdawn-1300` (mission 3, `1a9a814b`), `tdawn-paths`,
`tdawn-move` and `tdawn-move-refresh` (600 picture sums, the same), `tdawn-won` ("A mission won"). The pictures (`--gpu-capture`:
the game's picture is the top left of the GPU's target) differ only where real time shows: the pointer, a tooltip under it and the
credits counter counting. The palette's cycling (water, the selection colour) goes by the clock in play and by the game's frames in
a run with `TDAWN_NO_DELAY`, or two runs' pictures would differ in it.

### A scrolled picture

What the scene keeps (a cell's quad, an object's rectangles by their place on the map, the shroud's part, the page's holes) is kept across a
scroll. The check is a sweep by single pixels: `TDAWN_DRAG=20` drags the map from the 20th frame as the right button does (through
`ShaderEmu_Drag`, so by the same thirds of a lepton), a pixel a frame for 1,320 frames: thirty each of the eight ways and back, a cell's 24 x 24
places row by row, then 120 each way and back, which runs into the map's edges. `TDAWN_SELECT=10` keeps the bars and corners of the player's own
in the page. Three things are held against it:

    TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=1 TDAWN_DRAG=20 TDAWN_SELECT=10 TDAWN_SUMS=1 TDAWN_STATS_MS=0 TDAWN_FRAMES=1360 tdawn FROMINSTALL
    the same with TDAWN_REFRESH=1, then: python tools/tdawn_sums.py A.log B.log
    1360 frames in both (1360 and 1360), 1127 distinct views; view differs in 0; picture differs in 0, the page over the map in 0

- **The kept scene against one made whole every time** (`rvc_cpu`): `TDAWN_SUMS=1` says a sum of the map's part of the picture after every frame,
  of the scene drawn here in the program (`host_scene_picture()` in `host.c`: the kept cells, the three parts' rectangles from the atlas, a sum of
  what darkens a pixel, the page over it). Missions 3 and 10 differ in one frame each (a 6 x 7 thing in a battle, a frame late: not looked into).
- **The GPU's picture against the program's** (`rvc_harness --cpu`): `TDAWN_CHECK_GPU=1` reads the window's own buffer after every frame, where the
  GPU leaves its picture, and counts the pixels of the map's part that are not the program's (those something darkens and the pointer's surroundings
  left out): `1320 frames held against the GPU's picture, 0 of them differ`, in parts of 310 steps (`TDAWN_DRAG=20,300` begins at step 300) at the
  world's size: a display and a GPU target of 1,280 x 720, the window 960 x 600. It reads the hidden page as the program's picture.
- **At one held frame**, `TDAWN_CHECK_REDRAW=1` forgets everything (atlas, kept quads, the frames made ready: the game builds its own) and counts what
  a scene from nothing changes (a tooltip that still waits for its delay counts: a full redraw prints it at once), and `TDAWN_PICTURE=1` prints the
  picture for `python tools/tdawn_picture.py UART.log CAPTURE.bmp` to hold against the harness's `--gpu-capture`.

None of the three sees a fault in what the game itself draws into the page: all ways of drawing share that code. Red Alert has the sums, the GPU's check
and the selection under the same names (`docs/games/ralert.md`).

**A window that is no whole multiple.** In such a window a quad's left or top edge can lie exactly on a window pixel's middle, and the card then takes the
texel one before the quad's own rectangle: the next icon or shape of the atlas, as a column or a row a pixel thick, at single scroll offsets. The software
rasteriser (`--warp`) does not do it: this is checked on the card, `rvc_harness --cpu` (`TDAWN_RESIZE=794x545`). A compact vertex's texture place is a whole
texel and the GPU has no clamp for a rectangle of an atlas, so the atlas has gutters: an icon and a shroud shape each in 26 x 26 with its edge said again round
it (the shades' texture 256 x 256 words), a shape with a ring of holes. Left: a shape whose edge is on a middle loses that column to the ring's holes, and the
game's own pointer still takes its far column there.

## The scene on a worker core

Making a frame's rectangles (some two hundred: shapes, shadows, the shroud's edges) into GPU commands is the one part of the frame nothing else waits for until
the frame is shown. On a machine with worker cores (`docs/machine/multicore.md`) it is a worker's (`linux/tdawn/host.c`, which Red Alert shares):

- `host_sprite()` and `host_block()` note what was asked, 32 bytes each; `host_scene_end()` gives the notes to the worker, which makes the commands (`scene_job`).
- `host_present()` of that scene is put off while the worker has it. The game goes on to its next frame's logic, and the frame is shown when the worker has done:
  from `host_event()`, which the game asks every frame, or before the next scene begins at the latest. Nothing is drawn into the page in between.
- The GPU's library has one state, so nothing of `host.c` calls it while the worker has the scene (`scene_finish()` first: the pointer's picture, a movie's, an event
  of the window's). It is compiled with a section a variable, each starting on 16 bytes (`build.sh`), because two cores must not store to the same 16.
- A waited-for job would cost more than it saves (a pass or two at each end): the putting off is what makes it pay. A scene of fewer than 20 notes is made here.

`TDAWN_SCENE=inline` is no worker. A counting build runs with it.

## The workers on the second layer

All of the game's worker cores are taken through `programs/mc/mcwx.h` ("The second layer", `docs/machine/multicore.md`), and any worker does any job: the scene's (`mcw_call`, asked
with `mcw_ready` and joined), the files of frames, a unit's other path searches and the 32 that ask about cells (`mcw_try_call`: on a worker with nothing to do, or not at all, so that
a search or a file is never made on the first core by mistake). 42 are opened (10 in Red Alert, which has no workers that ask about cells) and whichever is free takes the next job.
Red Alert's movie shares a frame's rows with `mcw_for` and unpacks the next frame's pointers with `mcw_try_call` (`linux/ralert/workers.c`). With `TDAWN_MARK` (`RALERT_MARK`) the game
counts what each kind of job ran in `mcwx`'s counters and says it before the second mark (`tdworkers:`).

Also on the workers, each exact (the state sums are the same):

- **The ground layer's sort** (`LayerClass::Sort` in `layer.cpp`, both games): a worker makes the pass from where the objects are while the first core draws the frame (`Map.Render`, which
  moves nothing), into a list of its own, and `Sort()` takes that list. `TDAWN_CHECK_SORT=1` must say `1300 passes made by a worker core, 0 of them wrong` in both games.
- **The houses' threat** (`cell.cpp`, Tiberian Dawn): a unit that steps changes nine regions of every enemy house; the first core only notes the region, the houses and the value, and at the
  frame's end a worker adds them (sums: their order is no matter). Whoever reads threat (`Cell_Threat`) settles first, and so does a long path search before its helpers begin; a search on a
  worker reads threat as it is. `TDAWN_CHECK_THREAT=1` sums every house's regions wherever they are read: the same reads and sum both ways.
- **The scene's rectangles** go to the GPU's library in batches of 128 (`seglSprites` in `programs/linux/gles.c`): the same commands and vertices as `seglSprite` one by one (`TDAWN_CHECK_LIST=1`:
  the same sum both ways), with the state kept in registers.

A scan's box read whole first, to pass over rings with nobody in them, is slower: most scans end at a quarter of their range. The scene's vertices written by the game itself into the list are no better
than the batches.

## The objects a frame behind

The objects' part of a frame's scene is drawn by a worker while the first core makes the next frame's logic (`Behind_Note` and `Behind_Draw` in both games' `gl.cpp`, `host_scene_behind` in `host.c`).
The first core, where `Render` was, does what a frame's drawing does to the game: for each object of the layers it decides whether it is passed over, shown from its kept rectangles or drawn anew; one
drawn anew in the view is copied as it is (its whole object, by its kind's size), and the first core does to the live object what `Render(true)` would have (its mark, the animation's, a door's redraw
flag, a line's frame count in Tiberian Dawn, where the object drew nothing). The cells and the shroud stay the first core's. After `Map.Render` (the sidebar drawn), the worker draws the copies, notes the
objects' rectangles and makes the scene's commands; the frame is shown when it has. The next frame joins it before its input. So a frame is shown a frame later.

What the worker stores outside its own memory is in texels of its own (`drawing` in `gl.cpp`, `own` and `scene_noted` in `host.c`), a frame's shape it needs that no core has made is asked for by the first
core at the join (`frame_seen`), it waits for the GPU by ending passes, and it never allocates: Red Alert's turned shapes have their buffer kept (`turned()`), and a type's dimensions are not made on a worker
(`techno.cpp`). The game's shape flags (`IsTheaterShape`, `UseBigShapeBuffer`, set around a building's, an animation's and a turned object's drawing) are the first core's and left alone on the worker
(`ShaderEmu_Shape_Flags()` in `externs.h`). `TDAWN_DRAW=here` (`RALERT_DRAW=here`) is the first core's drawing; the `tdworkers:` line says the worker's part ("objects behind").

Checks: the state sums at frames 300 and 1,300, `TDAWN_CHECK_PATHS=1`, `RVC_RACES` 0 undone, the movie check, and held frames by `rvc_harness --cpu --warp` against `TDAWN_DRAW=here`.

## Selected units: their bars

- **The bars go into the hidden page.** Between frames the game's page is the seen one (`Render` sets the hidden one and gives the old one back). The worker that draws the objects a frame behind draws into the
  hidden page (`ShaderEmu_GL_Post`), and the first core has its own page back at the join (`Behind_Joined`, called by `host.c` on the first core). `page copies` in the stat line must not count one a frame with units selected.
- **A bar does not make the whole page holes.** The bars and box are `gl.cpp`'s (`ShaderEmu_GL_Bars`, called from `TechnoClass::Draw_It`): the same pixels as the game's calls (`Remap`, `Draw_Rect`, `Fill_Rect`, `Draw_Line`,
  clipped to the window), written into the hidden page without its lock (whose count is the first core's: a worker's `Lock` would be undone by the first core's), kept with the object by the map's pixels and put again every frame it
  is shown as it was, and made holes again by their own pixels before the next scene. An object with bars stays as it was drawn.

Checks: held frames by `rvc_harness --cpu --warp`; `TDAWN_SUMS` with `TDAWN_REFRESH=1` against without, 0 frames differ with `TDAWN_MOVE`, and in the drag with `TDAWN_SELECT`; `RVC_RACES` 0 undone with units selected and moving.
`TDAWN_CHECK_GPU` cannot see a fault here.

Known: in Red Alert's Allied drag (`RALERT_REFRESH=1` against without, nobody selected) a soldier is drawn a pixel high for some frames: a shape that changes without the object marked, as kept.

## A long battle

The shared code (`docs/games/ralert.md`, "A base destroyed"): the atlas's shapes are forgotten before a scene that may not fit and put again as they are drawn (`shapes_forget`, `TDAWN_ATLAS=N` every N scenes for a test); a shaken screen makes
the same random picks and frames without copying the page (`TDAWN_SHAKE=copy` copies); and the worker that draws the objects a frame behind has its copy of an animation detached as it is copied, and makes a frame no core had ready into its own
buffer (`ShaderEmu_Frame_Into`), since the game's `Build_Frame` buffers and heap are the first core's and the object an animation is attached to may be destroyed meanwhile. The game's own drawing is not made on the worker.

## The start

In the two games' shared code (`common/`):

- **A folder's names are read once** (`ShaderEmu_Folder_Of` in `file_posix.cpp`), a name without wildcards is compared, not matched, and the game's own writes and deletions make the names be read again (`rawfile.cpp`).
- **A file that is not among its folder's names is not there**, and one that opened once is not opened for that again (`ShaderEmu_File_Listed`).
- **A mix file kept in memory is mapped from its file**, all pages there at once (`ShaderEmu_Mix_Map`: a worker core that unpacks shapes must not meet a page that is not there), and a page the game writes to (a shape's header) becomes a copy of its own.
  Not for a mix inside another, nor one whose sum is checked.
- **Red Alert's title picture** last unpacked is kept (`Load_Title_Screen`).
- **Tiberian Dawn's cells** are cleared in one piece (`MapClass::Init_Cells`).

Not done: the two `.ini` files read a line at a time in Red Alert, the key arithmetic for its encrypted mixes, its map's zones filled at a scenario's start, the intro's first frames.

## What the compiler is told

Both games are compiled with counts from played games (profile-guided optimisation): how often every branch was taken, and which function each virtual call reached. On this machine that is all instruction count: which calls are inlined (a virtual call that
nearly always reaches one function becomes a test and that function's body), which path falls through, what stays in registers.

- The toolchain's own counting library is of another float ABI, so `host.c` has the functions the compiler's counters call (`SHADEREMU_PGO`; an empty `libgcov.a` is put in the toolchain's way at the link): when the program ends they print every counter that counted
  to the console, as lines beginning `GC`.
- `TDAWN_PROFILE=count linux/tdawn/build.sh` (and `ralert`'s) builds the program that counts, three times as large and a third as fast. Play what should be fast, with `TDAWN_SCENE=inline TDAWN_FRAMES_CORE=0` (the counters are not two cores' to write): the missions of "Speed"
  for 1,200 frames, and Red Alert's with `RALERT_HUNT=50` for 800. Build both games again at once after: the counting programs are in the image's folder meanwhile.
- `python3 linux/tdawn/profile.py make linux/tdawn/tdawn.profile LOG ...` sums the runs' logs into the file the build keeps (`linux/ralert/ralert.profile`), and an ordinary build writes the compiler's `.gcda` files from it (`profile.py put`) and compiles with `-fprofile-use
  -fprofile-partial-training`: what no run touched is compiled as before, not for size. A call's targets are known by a sum that has the source file's path in it: count with the repository's build script, not in another folder.
- Red Alert counts only its own folder (`-fprofile-exclude-files=/common/`): with counters in the shared code its start reads no file of its encrypted archives (something in the key arithmetic does not survive the change of code; not looked into further).

The counts are of a tree's functions as they were: a function changed since is compiled without them (no warning: `-Wno-coverage-mismatch`), so a profile ages gracefully and is worth making again after a large change. The state sums are the same with and without.

Quake, Quake III, the NES and ClassiCube are compiled the same way, with their own counts: `linux/quake/quake.profile`, `linux/quake3/quake3.profile`, `linux/nes/nes.profile`, `linux/classicube/classicube.profile`. Their counters are `linux/userland/pgo/gcov.c`'s (the values
kept in a pool of its own, not malloc'd, since worker cores run counted code too), and they are written at the program's end or at `kill -USR2`, so a game that never ends by itself is counted part way:

- `QUAKE3_PROFILE=count linux/quake3/build.sh` (`QUAKE_PROFILE`, `NES_PROFILE`, `CLASSICUBE_PROFILE`, and `DOOM_PROFILE`, `HALFLIFE_PROFILE` for the two below that keep none) builds the counting program in a folder of its own (`$WORK/src/*-count-build`); `=off` builds without the
  counts kept. The counting build has the same drivers and hidden globals as the one that uses the counts.
- Counted on `rvc_cpu`: Quake III `QUAKE3_STATS_FRAMES=100 quake3 +set com_maxfps 0 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3 & sleep 115; kill -USR2 $!` (the level's first frames); Quake `quake +timedemo demo1` and `QUAKE_TRACK=5 quake +map e1m1`, each sent USR2 at the end.
  The NES `NES_FRAMES=1500 nes thwaite` and `nova`, which end by themselves. ClassiCube new worlds of seeds 7 and 3, 60 s each, sent USR2. `profile.py make` makes the file from the runs' consoles.
- Quake's start's code is run once in a count, and is compiled as rarely run.
- Not kept: Doom's counts (fewer instructions on `rvc_cpu`, but no faster on the card) and Half-Life's (too little for its size). Counts of Tiberian Dawn and Red Alert made again on this tree give next to nothing: the old ones are kept.

## Frames made ready

A shape the scene has not shown before has to be unpacked (the game's `Build_Frame`) and put into the atlas a pixel at a time: one or two a frame in a battle, three when reinforcements land, and most of a mission's first frames. Both are other cores' work (`host.c`, shared with Red Alert):

- Six workers unpack shape files (`TDAWN_FRAMES_CORE=N`: no more than N; 0: none). A file is unpacked in runs: 64 frames, or 128 KB of them, from the frame that was asked for, with the game's own unpacking (`ShaderEmu_Frame_Step` in `keyframe.cpp`, which touches nothing of the program's but its buffer, and makes a frame from the one before it where the file
  has it as a change: a file's last frames were its whole chain each). Of each frame it keeps the rectangle that has pixels, a bit for every index in it, and each row's first and last pixel. Then the file rests. A frame asked for that is not there, or one whose twelfth frame after it is not, has the file go on from there: at once if a core has it (a count the job looks at, `asks`), else on a core with none, in the same call.
- What will be asked for is asked for ahead, as a mission begins and in every frame in which something was made (`ShaderEmu_GL_Ahead` in `gl.cpp`): the file of every unit, soldier, aircraft and building there is, on the map or not yet (being built, on its way in a transport), a building's construction too, every animation's first run, and the files the game tells of (`ShaderEmu_GL_Ahead_File`: Red Alert's sidebar, `docs/games/ralert.md`).
- From those bits the first core knows a ready frame's shadow levels without looking at its pixels, gives it its place in the atlas, and leaves the pixels to the core that makes the scene (`host_fill()`): they are written before that scene's commands, and nothing else reads the atlas. A cell's icon is copied there the same way (`fill_icon`).
- **The game's own drawing takes a ready frame too** (`ShaderEmu_Ready_Shape` in `conquer.cpp`, `ShaderEmu_Ready_To_Page` in `common/keybuff.cpp`): a shape drawn plainly into the page (the sidebar's pieces, tabs, buttons) is copied from the worker's rectangle, a row's run by its span, and is neither unpacked nor measured by this core. `TDAWN_READY_DRAW=0` is the game's own way.
- The memory is the workers' alone to write: 8 MB of address space, of which 2 MB (3 in Red Alert) have their pages from the program's start, with the cores' buffers and the frames' table. More comes two pages a frame when less than 256 KB is left.
- A frame asked for that is not ready hands the cores that are done their next files (`frames_pump` from `host_frame`): a score screen has no scene's frame to do it.
- `Reset_Theater_Shapes` and a new scenario stop the workers and forget everything (`host_frames_forget()`): a file may go then.

`LCW_Uncompress` copies a run of 24 bytes or more a word at a time (`LCW_Back` in `common/lcw.cpp`: bytes from behind that are closer than their count are a pattern that repeats, copied in pieces no longer than the distance).

`TDAWN_CHECK_FRAMES=1` (`RALERT_CHECK_FRAMES=1`) has the game build every such frame too and compares rectangle and pixels: `0 of them differ from the game's own` at the end. `TDAWN_CHECK_READY=1` draws every shape of the game's own drawing both ways and compares the window's pixels: `0 of them differ from its own`.

## A unit's path searches

A unit that needs a way searches up to four times in one `FootClass::Basic_Path`, by other rules over the same cells with nothing changed between (from its `Mark` up to its `Mark` down): first with everything it could shoot or wait for taken as passable, then with nothing, then the rules between. A blocked search follows the obstacle's edge both ways, up to 400 cells each, and mission 3 begins with eight units doing it in one frame. All in `findpath.cpp` (`ShaderEmu_Find_Path`):

- What `Can_Enter_Cell` said of a cell is kept for the unit's searches (`ShaderEmu_Enter_Keep`). `TDAWN_CHECK_ENTER=1` asks again each time: `0 of them wrong`. And a free cell is not measured for its distance (`Passable_Cell`: only a cloaked blocker's matters).
- The rules differ only in a threshold that is compared with a cell's answer and nowhere else. A search notes which answers it met; one that met none between its threshold and another's went the other's way step for step, and that one is not run: its path, its moves and its list of cells are put back as the search would have left them.
- A search whose edge walks have come by 48 cells is a long one, and the unit's other searches begin then on worker cores, each with its own threshold, while the first core goes on with its own (`ShaderEmu_Path_Help`; up to 3, on whichever workers are free, `host_path_cores()` in `host.c`). A search stores only into its core's own memory (its path, its lists, its kept answers: `ShaderEmu_Core`, 16 bytes apart); which core is in the threshold's upper bits all the way down. Searches nobody asks the result of are stopped and waited for before the unit is put back.
- At the same moment up to 32 more workers ask `Can_Enter_Cell` about every cell of the map for that unit, two rows of it each (`ShaderEmu_Pre_Job`, `host_pre_cores()`, `TDAWN_PRE_CORES=N`): two passes later every core's search finds the answer there (`Passable_Cell` looks in `ShaderEmu_Pre` before it asks). They store whole rows of two tables and nothing else, and have ended before the unit is put back.
- The walk round an obstacle is `ShaderEmu_Walk`: `Follow_Edge` step for step, with a cell's kept answer read in place and a cell that is new to the path put on it in place. `TDAWN_WALK=old` is the game's own.

`TDAWN_CHECK_PATHS=1` makes every such search on the first core too and compares path, moves and cells, and makes every edge walk both ways: `tools/test_all.py`'s `tdawn-paths` wants `N searches not run again and M another core's, 0 of them wrong` and `N edge walks made both ways, 0 of them wrong`, with state `1a9a814b`. `TDAWN_PATH_CORES=0` is one core. Red Alert has the searches not run again, the helpers (started only when both walks of a pair failed) and the walk, not the cells ahead (`docs/games/ralert.md`).

What a long search is: one of its two walks reaches the other side quickly, and the other goes the long way round, to lose the comparison; the game makes both to the end. What is left is the first core's own first search: its outcome is needed before the unit's turn can go on, and a walk is one step after another.

The answer by threshold kept for each cell of one search is slower (a walk asks about most cells once or twice), and the first core waiting for the 32 workers and reading their answers gains nothing. Not done: the rest of a search with two obstacles made by a worker from the shorter walk's end while the first core finishes the longer; the two walks of one obstacle on two cores gain nothing, since it is the long one that is waited for.

## Movies

Tiberian Dawn's movies are 320 x 200 in 4x2 blocks (`UnVQ_4x2`; Red Alert's are 4x4, `docs/games/ralert.md`, "Movies, and the worker cores"). A frame's block rows are libmedia's `media_vq2_rows` (`docs/system/media.md`, "VQA"), shared between worker cores: the drawer is given `ShaderEmu_UnVQ_4x2` in place of the game's (`common/vqadrawer.cpp`, Tiberian Dawn's patch, compiled out of Red Alert's, `SHADEREMU_RA`), and `build.sh` links `libmedia.a` (`TDAWN_MEDIA=FILE` for another build of it).

- The movie has the workers as Red Alert's has them: the first frame closes the scene's (`host_workers_close()`, which the next mission opens again) and opens fifteen, and `Play_Movie` gives them back when the movie is closed (`ShaderEmu_Movie_End`).
- `TDAWN_MOVIE_WORKERS=N` uses no more than N (0: this core alone); `TDAWN_MOVIE_CHECK=1` decodes every frame with the game's own `UnVQ_4x2` as well, compares the two, and says at the movie's end how many differ (`tdawn: movie NAME.VQA: ...`; also with `TDAWN_FRAMES`).
- The demo has no movies. To run one in the game under `rvc_cpu`, link a VQA under every movie name into `/tmp/tdawn`: `TDAWN_AUTO=1 TDAWN_MOVIE_CHECK=1` plays the demo's briefing and action movies (`GDI1.VQA`, `LANDING.VQA`), and must say 0 differ, `RVC_RACES` 0 undone. (The demo looks for those two; the intro's `LOGO` and `INTRO2` it does not play with its default settings.) `linux/libmedia/test/check_vqa.py` checks the decoder against ffmpeg (`docs/system/media.md`, "VQA").
- The profile line settles the scene first (`host_scene_settle()`, both games' `conquer.cpp`): it zeroes `gl.cpp`'s counters, which the worker drawing the objects adds to.

## A mission won

`TDAWN_WIN=N` wins the mission at frame N (once) and prints a line at each step, as Red Alert's `RALERT_WIN` (`docs/games/ralert.md`, "A mission won"). Tiberian Dawn's demo goes from mission 1 to 10 by its own code, and its score screen makes no table of colours: mission 10 is then played with its state at frame 300 `325b1830` (`tdawn-won` in `tools/test_all.py`). Half of that screen's instructions are `Linear_Blit_To_Linear`, the page copied whole at each of its steps; not changed. The table and the doubled rows (`common/interpal.cpp`, `host_for`) are shared, for the screens that use them.

## Not done

- The movies on the GPU, as Red Alert's (`host_picture`): the frame is still the game's page.
- The cloak effect on the GPU (the game draws such a shape itself, over everything), and the sidebar, radar and text, which are still pixels.
- Text that stays: a tooltip is printed again every frame it is up (a message is not).
- A soldier that walks is drawn anew every step, through the game's own `Draw_It`: the larger part of a scene with many of them moving (on a worker, "The objects a frame behind").
- Music in the missions. The full game's data (the demo's is 6 MB; the CDs' movies and music are not wanted).
- Red Alert is the same tree (`BUILD_VANILLARA`), built by `linux/ralert/build.sh` (`docs/games/ralert.md`).
- With `NANOX_SIZE=640x480` the window has no frame (640x400 leaves no room for one) and is left through the game's own menu (Escape).
