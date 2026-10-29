# Tiberian Dawn

Command & Conquer (1995) runs in the Linux image: [Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer),
the portable version of the game's released source, as a Nano-X client. The GPU draws the map:
terrain, objects, shadows and the shroud ("The map on the GPU", below). The game still draws
its sidebar, its text and its menus itself, on its own 320x200 screen, which the GPU shows on top.
The window is the largest whole multiple of that the display has room for: 960x600 on the
desktop's 1280x720, which costs the game nothing (the GPU does the stretching).

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh     (a minute when a file changed; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

That is the image with the MMU. The world and `rvc_harness` with no arguments run Linux
without one, where the program is flat: `bash linux/nommu/build.sh tdawn/build.sh`, then
`root.sh` and `image.py` (`docs/system/nommu.md`, "Building it").

In the guest: `nano-X -p & tdawn` at the console (it is also listed for the desktop's Start
menu, which was not tried). The data is the 1995 demo's (GDI missions 1, 3, 5, 6 and 10; the
menu starts the first), which the build fetches and the repository does not keep. Effects and
speech play on the sound card (`docs/machine/sound.md`); there is no music in the missions (the demo
has none), only the map screen's and the score screen's tunes (`MAP1.AUD`, `WIN1.AUD`).

## What the build is made of (`linux/tdawn`)

- `build.sh` fetches Vanilla Conquer at a fixed commit, applies `vanilla-conquer.patch`, adds
  the files below and builds with CMake and the image's toolchain. The tree is put back on every
  build and Red Alert's build shares it: each game builds from a copy of its own
  (`vanilla-conquer-td`, `-ra`, by `rsync -c`), where a file keeps its time unless its bytes
  change, and the GPU library's object is replaced only when it differs. A build with nothing
  changed is 2 to 3 s; before, every build compiled and linked the whole game again (62 and 72 s).
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
  calling each other across source files, and a frame is a fifth fewer instructions for it
  (both games; every state sum the same). The programs are 5 and 8 MB where they were 1.4 and
  1.9, and a build is one minute and two where it was 9 and 20 seconds: with the default
  inlining limits it is 33 and 50 seconds and 12 to 15% of a frame. With the counts of a
  played game (below) twice these limits again are 2% more of Red Alert's frame, and twice
  those nothing. Two flags are not optional: `-flto-partition=1to1 -fno-toplevel-reorder`.
  The game's globals are constructed in the order of its source files and depend on it; with
  the default partitions the program ends at its start with "out of memory" (a heap sized
  from a global that is not made yet). The C files (`host.c`, the workers' library) are
  left as they are: `mcw.c` is in two targets, which LTO takes for one symbol defined twice.

## What was changed for this machine

- **The clock.** The game asks the time constantly, and each answer was a system call and a
  64-bit division (a library call of some 400 instructions): 45k instructions a game frame.
  It reads the machine's clock word and divides in 32 bits.
- **Showing a frame.** The game finishes a frame by copying its hidden page to the visible
  one, 64,000 bytes. Both pages are GPU memory: the copy is put off, the hidden page is
  shown, and the copy is made only if something reads the visible page or draws on it
  (`Catch_Up` in `shaderemu.cpp`; the figures say how often).
- **Finding files.** Every file was looked for in any spelling, a folder at a time from the
  root down, and reading the root folder reads the ROM: 6,282 folder listings before the first
  mission, 212M instructions. Only the file's own name is matched now, and the starter runs
  the program from a folder of its own (the game lists its program's folder too): 70M. The
  program is `/usr/lib/tdawn/game` in the ROM, run where it lies and not copied to `/tmp`
  (`docs/system/nommu.md`, "Memory"); `/tmp/tdawn`, where it runs, has links to the data.
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
- **Logic that repeats itself**, each with the state sums unchanged:
  a unit that steps is lifted from the map and put down, and each half added or took its
  threat from nine regions of every enemy house: the lifting is held back and the two cancel
  (`CellClass::Adjust_Threat`, 9% of mission 3). A house is found by its
  kind from a table, not a search (`HouseClass::As_Pointer`). The layer's sort asks each
  object where it sorts once, not twice.
- **A scan for targets reads bits.** `Greatest_Threat` goes round its unit in growing boxes
  and asks every cell of each, and nearly all are empty. The map keeps a bit a cell that has
  an occupier, once by rows and once by columns (`cell.cpp`, `cell.h`): a side of a box is a
  word or two of them, and only the cells whose bits are set are asked, in the order the
  game's loops ask them (top before bottom for each x, left before right for each y: the
  scan keeps the last target it finds, so the order is the answer). The bits follow every
  store to a cell's `OccupierPtr`: `Occupy_Down`, `Occupy_Up`, the dead occupier that
  `Cell_Occupier` drops, the cells made anew (`Init_Cells`) and a saved game's
  (`MapClass::Decode_Pointers`). `TDAWN_CHECK_OCCUPY=1` compares them with the cells every
  frame and says how many were wrong: none in 1,200 frames of mission 10 and 300 of missions
  1 and 3. The scan itself went from 11.5k to 5.7k instructions a frame in mission 10, and
  its logic from 99k to 93k a frame over 1,200 frames (113k to 107k over 300; mission 3 124k
  to 120k; mission 1 has few scans and stays 49k). What is left is some 70 instructions a
  box, and the cells that do have someone (`Evaluate_Cell`, 2.8k a frame, as before).
- **Scrolling.** The game moved the map a fixed step every pass of its loop, sized for a
  machine that makes some twenty passes a second; here a pass that scrolls is 400k to 600k
  instructions, six or seven a second. `ShaderEmu_Scroll_Distance` grows a step with the
  time since the last one, up to 36 pixels (beyond that the game draws the whole map again
  instead of shifting it); only `TDAWN_SCROLL` uses it now.
- **The map is dragged.** The game scrolled while the pointer was on the picture's outermost
  row or column, which a mouse finds by being stopped there and a beam does not. That is
  gone in both games (`ScrollClass::AI`, `shaderemu.cpp`); a margin past the edges to scroll
  from was tried in its place and taken out too. What moves the map:
  - The right button (the grip) held on the map and moved drags the map, a pixel for a pixel.
    The game gets that button's press at its release, and only when the pointer stayed
    within a 40th of the picture's width: a press that did not move is the game's right
    click as before. A press that begins off the map (the sidebar) is never a drag.
  - The drag follows the pointer only while the machine's own buttons word says the button
    is down (`host_buttons`): the window system's event of the release comes later.
  - While the map is dragged the pointer is the game's scroll arrow for the way the view
    goes (against the hand), and its "no" arrow where the map ends.
  `TDAWN_INPUT_LOG=1` prints each drag step and its arrow. To test without hands, under `rvc_cpu`:
  `--pointer-sweep 0 --pointer-button 2,52,54` drags (the pointer goes round the middle of
  the display, which is on the map), and `--pointer-circle 40,250,40` without a button goes
  in and out over the window's left edge, which must print nothing.
- **Events that are owed.** `host_event` asked the window system eight times when an input
  word changed and then every 250 ms. On a fast machine (`rvc_harness --cpu`) all eight came
  before the kernel's next tick had reported the button, so a press arrived at its release
  and a release 250 ms later. It now also asks every 10 ms while an event the words told of
  has not come, for a tenth of a second at most: 0 to 22 ms there.
- A page's blit onto itself (how the map is shifted) copies rows in the order that reads each
  before it is written. Top to bottom, as it first was, a scroll downward smeared the map.
- **What a frame did for nothing** (October 2026, "At two million instructions a second"
  below), each with the state sums unchanged:
  - An object that moved asked every cell it left and came to to be drawn again, 165 calls a
    frame in mission 3. On the GPU that paints over nothing: `MapClass::Place_Down`,
    `Pick_Up` and the overlap lists only tell the scene something near it changed
    (`SHADEREMU_MOVED` in `map.cpp`, as Red Alert had).
  - `FlasherClass::Process` looked at a counter for each of twenty houses in every object every
    frame; the largest of them is kept beside them and says by itself that none counts.
  - `MapClass::Logic` asks 120 cells a frame whether Tiberium grows there or a tree spawns it:
    a cell with nobody in it and no Tiberium is passed over before the questions.
  - `LogicClass::AI` timed every object's turn for `TDAWN_SLOW`'s report: only when that is set.
  - The layer's sort keeps a building's and a tree's place (`ShaderEmu_SortKey` in the
    object, worked out again when its coordinate is another): they are most of the layer.
  - A message over the map was printed again every frame it was up, 14 thousand instructions:
    it stays in the page as printed until the messages are others or holes were made in the
    page (`ShaderEmu_GL_Messages` in `gl.cpp`, `MessageListClass::ShaderEmu_Signature`).
  - `BF_Trans`, which draws every shape the game still draws itself (the sidebar, its tabs),
    copies runs of pixels and passes nothing over a word at a time (`common/keybuff.cpp`).
    A shape from the uncompressed shapes' buffer, which stays as it is, has each row's first
    and last pixel kept and whether all between are pixels (`ShaderEmu_Spans_Of`): drawn
    again it is a copy a row. Red Alert's power bar and credits' tab, drawn every frame
    while they count, were 38 thousand a frame of a mission's first hundred.
  - `fixed(a, b)` divides in 32 bits when both fit (`common/fixed.cpp`).
  - The window system is asked for events once a second, not four times, unless the
    machine's input words moved or the pointer is outside the picture (`host_event`).
  - A message over the map that expired had the page cleared and everything drawn again,
    the sidebar too: 600 to 900 thousand instructions in Red Alert, three times in its
    Soviet mission's 1,200 frames. Where the GPU draws the map the message's rows are made
    holes and what is left printed again (`ShaderEmu_GL_Messages`, which did that already
    for a message that changed). `TDAWN_MESSAGE_GONE=redraw` is the game's way.

## The map on the GPU (`gl.cpp`, `host.c`)

The game drew the map into its page a cell and a sprite at a time, and that was half to two
thirds of a frame. Now the map is a scene of rectangles the GPU draws, and the game's page
goes over it with holes where the map is (index 0, which the game never draws).

- **Terrain stays.** Each cell of the map is a quad that is written once, when the cell is
  first seen, and kept (`seglKeep`): its picture is one of 1,482 icons in an atlas (each in 26
  x 26 with a gutter: "It was the window's size" below). Scrolling
  is a matrix: the same quads somewhere else, and nothing of the terrain is touched.
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
  itself remembers it drew nothing at (`ShaderEmu_Away`: most objects are such, and asking
  the table about each was 2 thousand instructions a frame). The object says where
  its rectangles are kept (`ShaderEmu_Kept`). The shroud's edge over a cell is worked out
  again only when the game says the cell changed. In case a change went unmarked, every
  256th scene has its cells from nothing, and every scene one object of each layer in
  turn; the page is not filled for it. (All of it in one scene was a frame of 90
  thousand instructions here and 200 thousand in Red Alert, every 13 seconds.
  `TDAWN_REFRESH=N` other than 256 is that: every Nth scene whole.)
- **What is under the objects and over them stays.** A scene is three parts
  (`host_scene_part`): the cells' own rectangles, the objects, the shroud. The first and the
  last are noted once and kept while no cell of the view has changed (`ShaderEmu_GL_Cell`,
  which `Redraw_Objects` and `Flag_Cell` call) and the view has not moved: going through the
  cells in view every frame was a third of the scene.
- **A scene is made again only for a change in it.** The game marks an object that changed and
  asks its cells to be drawn again, which here paints over nothing: `Refresh_Cells` and
  `Redraw_Objects` only note whether the place is in the view or within five cells of it, and
  a frame in which nothing there changed shows the scene it has (`ShaderEmu_GL_Changed`).
- **Shadows** are the pixels the game's translucency table would darken. They are a second
  rectangle from a mask of one bit a pixel, in black with the transparency the table has, under
  the shape's own. The shroud's edges are pictures of words with a transparency each; cells
  never seen are black rectangles, one a run.
- **The page on top.** What the game still draws over the map itself (health bars, text, the
  band being dragged, the placing cursor's cells when the atlas is full) lands in the page
  and shows over the scene. The page's part over the map is made all holes again only after
  something was drawn there.
- **The pointer is the display's cursor** (`pointer_publish()` in `host.c`,
  `docs/machine/display.md`): its picture, 32 x 32 colours made from the game's 8-bit one with the
  palette, is given to the window system as the window's cursor (`docs/system/nanox.md`), one
  cursor for each hot spot the game has used, and whatever shows the display draws it at the
  pointer at its own rate. It used to be a rectangle of the scene, which moved when the game
  showed a frame: five times a second in a battle. It still is one where the picture is
  stretched over a larger window or the game's pointer is over 32 pixels, under the window
  system's arrow; `TDAWN_POINTER=drawn` is that everywhere. The picture is made again when a
  colour it used has changed in the palette (a fade).
- **Where the pointer is** comes from the machine's input words, not from the window system
  (`pointer_read()`, `docs/machine/input.md`).
- **A frame with nothing new** (the pointer moved, a dialog is open) draws the last list
  again (`seglSwapAgain`): its textures are the page's memory, and the pointer's corners and
  the page it shows are changed in place.
- All sprites are in the blended pass with no depth, so that a shape and its shadow keep the
  game's order. What does not fit (a shape with the cloak effect, a full atlas, a map of more
  than 64 x 64 cells) the game draws itself, into the page; `TDAWN_RENDER=soft` makes it all.

Against the game's own drawing the picture differs in the shadows' and the shroud's shade
(a true blend, where the table picked the palette's nearest dark colour): 4% to 14% of the
pixels by a few levels, 0.05% by more than 32, which are the pointer.

## Speed

The game wants 20 frames a second here, and a frame of the game is one step of its logic and
one drawing of what changed. On D3D11 with fxc2 (4.0M instructions a second with the PC's GPU
otherwise idle), 300 frames with no delay between them, nobody playing:

| Mission | Logic | Drawing | A frame | Frames/s | The game's own drawing: a frame | frames/s |
|---|---|---|---|---|---|---|
| 1 | 41k to 53k | 65k to 71k | 125k to 137k | 28 to 32 | 248k to 517k | 6.8 to 14.0 |
| 3 | 101k to 109k | 53k to 54k | 174k to 185k | 22 to 24 | 280k to 567k | 5.4 to 10.3 |
| 10 | 83k to 164k | 39k to 94k | 176k to 236k | 17 to 23 | 313k to 589k | 6.3 to 11.4 |

Mission 10 has stretches of path finding where one frame's logic is 350k. Its figures
include a tooltip: the pointer rests on a unit, and the game prints the name every frame,
13k. Scrolling costs nothing more. In the VRChat world the machine runs 2.7M to 2.9M a
second, where these frames would be 20 to 23, 15 to 17 and 11 to 16 a second (not measured).

Where a frame of mission 1 goes now (`tdstat:` says): the scene 60k to 68k, of which the
cells 13k, the shroud 8k and the objects 40k (53 written again as they were, 12 drawn anew).
A rectangle that joins the command before it is written by a function that calls nothing
(`seglSprite`; in batches by `seglSprites` since 14 October 2026, "The workers on the second
layer" below). In missions 3 and 10 logic is the larger part: in mission 10 looking for
targets is 6% of a frame (it was 8% to 10% before the scan read bits) and path finding 12%.

The figures below are the game's own drawing (`TDAWN_RENDER=soft`), from before:

| Mission | Objects | Logic | Drawing | A frame | Frames/s, D3D11 + FXC (3.0M/s) | D3D12 + DXC (3.8M/s) |
|---|---|---|---|---|---|---|
| 1 | 22 to 36 | 76k to 128k | 141k to 386k | 239k to 540k | 5.8 to 12.2 | 8.6 to 15.6 |
| 3 | 82 | 177k to 207k (466k as it starts) | 67k to 162k | 280k to 567k | 5.4 to 10.3 | 7.8 to 13.1 |
| 10 | 75 | 133k to 213k (356k at times) | 115k to 468k | 281k to 701k | 4.4 to 10.3 | 6.4 to 12.5 |

So it plays, at a half to two thirds of its speed where it matters (FXC is what VRChat runs).
Starting takes about 25 seconds to the first mission's first frame (DXC). Where a frame goes
in mission 10:

- 21% in one sprite routine (`BF_Ghost_Fading_Trans`: shadows and the shroud, two table
  lookups a pixel), 10% in `memcpy` (the terrain's 24-pixel rows, a call a row), 2% drawing
  text. Drawing is software throughout, and this is the part the GPU can take.
- 7% looking for targets (`Evaluate_Cell`, `Greatest_Threat`): logic, as with Doom's sight checks.
- Mission 10 takes about 40 seconds to start, 15 more than mission 1: 11% of that run's
  samples are musl's `qsort`, called from the scenario file's index (`IndexClass::Is_Present`).

Each run prints these figures (`tdstat:` lines, every five seconds):

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
    TDAWN_RENDER=soft   the game draws the map itself, as before
    TDAWN_REFRESH=N     make every Nth scene from nothing (256; 0: never)
    TDAWN_SLOW=N        with TDAWN_STATS_MS=0: say each frame of more than N thousand instructions, and where they went
    TDAWN_PATH_CORES=N  worker cores for a unit's other path searches (3; 0: this core makes them all)
    TDAWN_PRE_CORES=N   worker cores that ask about the map's cells ahead of a long search (32)
    TDAWN_WALK=old      the game's own walk round an obstacle (Follow_Edge)
    TDAWN_FRAME_LOG=1   at the end, every frame's instructions: all, logic, drawing, the map and its parts, shapes put
    TDAWN_MARK=A,B      a line as frame A begins and as frame B does (rvc_cpu --pc-from ": mark begins" --until ": mark ends")
    TDAWN_MESSAGE_GONE=redraw  a message that expires clears the page and has everything drawn again, as it was
    TDAWN_FRAMES_CORE=N  no more than N cores unpack shape files (6; 0: the game builds every frame)
    TDAWN_READY_DRAW=0  the game's own drawing unpacks its shapes itself
    TDAWN_SPRITES=one   the scene's rectangles go to the GPU's library one call each (seglSprite), as before
    TDAWN_CHECK_LIST=1  at the end, a sum of every scene's vertices and draw commands (the same both ways)
    TDAWN_CHECK_SORT=1  the ground layer's pass made by a worker is made here too and compared (RALERT_CHECK_SORT)
    TDAWN_THREAT=here   the houses' threat is added on this core, as before; TDAWN_CHECK_THREAT=1 sums it where read
    TDAWN_DRAW=here     the objects are drawn by this core, not a frame behind by a worker (RALERT_DRAW)

## At two million instructions a second

The world's machine is slower than the harness's and a visitor's card slower still: the
measure here is a core of 2 MIPS (`rvc_cpu --ips 2000000`, which counts a pass's fixed cost
too: 1.88M instructions a second are the game's). Game frames a second, nobody playing, no
delay between frames, before and after the work of October 2026 (both builds on one kernel):

| | 300 frames, before | after | 1,200 frames, before | after |
|---|---|---|---|---|
| Mission 1 | 15.7 | 29.3 | 17.5 | 36.7 |
| Mission 3 | 12.0 | 22.1 | 10.1 | 20.9 |
| Mission 10 | 11.8 | 23.2 | 13.2 | 26.1 |
| Red Alert, Soviet | 9.7 | 23.6 | 12.6 | 30.9 |
| Red Alert, Allied | 9.0 | 20.6 | 8.7 | 21.7 |

In thousands of instructions a frame, 1,200 frames: mission 1 107 to 49; mission 3 185
(logic 124, drawing 42) to 88 (64, 14); mission 10 141 to 70; Red Alert's Soviet mission 149
to 59 and its Allied one 214 (122, 68) to 84 (55, 18). What it was made of: the
whole-program build (a fifth of everything), the compiler told what a game runs (another
twentieth to tenth: "What the compiler is told"), the scene's kept parts and its objects by
reference, new shapes unpacked and put into the atlas by other cores ("Frames made ready"),
the items under "What a frame did for nothing" above and Red Alert's own (`docs/games/ralert.md`),
and the C library's new `memcpy`.

These are averages (the 300-frame ones leave out the first hundred frames). A mission's
first four frames are its start: 0.3 s in mission 1, 0.6 s in mission 10, 0.9 and 1.2 s in
Red Alert's two, 1.5 s in mission 3 (the first scene, every shape's first use, every unit's
first scan and path at once: it was 1.9 s before a unit's searches were shared out, "A
unit's path searches" below). From frame 4 to frame 71 the missions run at 27.3, 21.6 and
28.0 frames a second and Red Alert's at 19.0 and 20.8 (they were 10: the power bar and the
credits' tab, drawn again every frame while they count up, became row copies); mission 3's
first 71 frames with its start counted are 15.5 a second (they were 13.5). All
five hold 15 from frame 4 on: the lowest stretch of each in 1,200 frames is 28.9, 16.8
(mission 3's battle, which was 10), 21.1, 20.8 and 15.3 (the Allied mission's frames 494
to 560: 126 objects of which 44 are fires, each an animation that changes every frame).

The measure is the missions as they play. Beyond it, as a stress figure: Red Alert with
everything sent to hunt (`RALERT_HUNT=50`, 800 frames), a battle no mission
left alone has: the Soviet mission 8.7 to 24.3 (its lowest stretch 15.7), the Allied one
5.6 to 15.2 (122 thousand a frame: logic 79, drawing 31): 15.4 in frames 71 to 140, 16 to
17 at its end, and 9.1 to 13 for the hundred and fifty frames in which its reinforcements
land and the radar opens (160 objects). That landing is what is left under 15. In windows
of a second and a half (`RALERT_STATS_MS=1500`) the rest of the battle is 13.2 to 19.4, and
the landing is: seven frames at 4.5 while the radar opens (drawing 275 thousand a frame, of
which the map is 28: the 18 frames of the opening, 160 pixels square, unpacked by the game
as they play and copied to the page), then seventy frames at 8 to 13 with logic at 86 to
145 thousand for 137 to 158 objects and the scene at 44 to 76 for up to 73 of them drawn
anew. The logic there has no part of its own (soldiers 15% of the frame, vehicles 9,
buildings 8, fires 8, the houses 4, path searches 4): with no drawing at all its worst
window would be 11 frames a second.

A battle's frame has no large part left: the true flat profile (instructions by the
function a line was written in: `addr2line -f -i` over a count for every pc, which sees
through the whole-program build's inlining) has nothing over 5%, and its top is index arithmetic
(`FixedHeapClass::operator[]` under every `CCPtr`, 7% with its callers), timers and the
scene's own loop. The kernel's tick is 10% of such a frame (below).

Found with a call graph, not a flat profile (a private copy of `rvc_cpu` that keeps a
shadow stack for the game's address space from `jal`/`jalr` and `ret`, and counts the
kernel's instructions by what entered it): inclusive costs said what a flat list of
hundreds of functions under 1.5% each did not. The kernel's timer tick is 3,700
instructions, 18 thousand a battle's frame, of which the input driver's own timer was 600
(`linux/kernel/tick_input_hook.py`: the tick polls input itself). The rest is the kernel's
timekeeping (840) and a long tail; fewer ticks a second would be worth 5% of everything at
this speed (measured: a kernel of 20 Hz with `HIGH_RES_TIMERS` took the Allied battle from
147 to 139 thousand a frame), and is not done (`AGENTS.md` has why the tick is 100 Hz).

### 13 October 2026: the long frames, and why a frame is not half

Measured again in windows of a second and a half (`TDAWN_STATS_MS=1500`, 1,200 frames, the
first window left out as the mission's start), before and after the path searches' help
("A unit's path searches" below), the word-at-a-time `LCW_Uncompress` and the counts made
again (`tdawn.profile`):

| `rvc_cpu --ips 2000000` | Average, before | after | Lowest window, before | after | A frame, thousands |
|---|---|---|---|---|---|
| Mission 1 | 37.1 | 37.0 | 24.1 | 24.1 | 50 to 50 |
| Mission 3 | 20.7 | 21.4 | 12.7 | 13.8 | 90 to 87 |
| Mission 10 | 25.9 | 26.8 | 14.7 | 17.4 | 72 to 70 |
| Red Alert, Soviet | 29.6 | 29.9 | 17.7 | 18.4 | 63 to 62 |
| Red Alert, Allied | 21.4 | 21.5 | 10.0 | 9.9 | 87 to 86 |

At `--ips 3000000 --ticks 8192` the averages are 56.5 to 56.3, 31.3 to 32.4, 39.4 to 40.7,
45.2 to 45.6 and 32.6 to 32.7, and the lowest windows 40.9 to 41.7, 22.3 to 22.9, 21.0 to
28.6, 30.3 to 30.9 and 17.0 to 16.8. Mission 3's first window (its start) is 13 frames
where it was 6. (Mission 3's 1,200 frames on 19 October 2026 on an RTX 5070 laptop, from frame 300,
`docs/machine/speed.md`: 52.1 frames a second on the shader machine, 46.9 on `rvc_lean`, 94.2 on the
compute machine, 178.7 on the CPU harness.) On the shader machine (D3D11 with fxc2, 3.9 to 4.0 million instructions a
second, through the card's queue, before and after in turn twice): mission 3's 800 frames
48.6 and 49.1 frames a second before, 50.6 and 52.6 after, its lowest window 36.8 and 37.6
to 38.3 and 41.5; mission 10's 600 frames 60.0 and 59.9 to 61.9 and 62.1, its lowest
window 45.3 and 45.7 to 50.3 and 50.8; the state sums the same (`3025508e`, `c57c822c`),
and three held frames (mission 3 and Red Alert's Soviet mission at frame 300, its Allied
one at 262, with the sidebar up) the same in every pixel.

The game's own speed is 20 steps a second, and from frame 4 on every mission is over it on
average at two million instructions a second; what is under it is the windows above. They
are single long frames: a unit's path searches in missions 3 and 10 (below), and in Red
Alert's Allied mission one frame of 1.76 million instructions when the sidebar first shows
(`docs/games/ralert.md`) and the landing.

Why the frames between are not half of what they were, by what a frame of mission 3 is
(87 thousand instructions of the first core):

- **Logic, 61 thousand.** The profile by source line has no line over 1.8% and no function
  over 7% of its own (`InfantryClass::AI`); every object's turn reads what the turn before
  it stored and takes its random numbers in turn, and the state sums are the test. What a
  worker can make exactly is something whose inputs do not change while it works and whose
  result is waited for or thrown away: the path searches are that, and nothing else in the
  list is over two passes long.
- **Drawing, 15 thousand** (Red Alert's Allied mission 19 of 86). With all of it on other
  cores a frame would be 1.2 times as fast, 1.3 in Red Alert, and it cannot be while the
  next step's logic runs: `Draw_It` reads the objects that step moves. A copy of what it
  reads was not built: an object is 300 bytes and more of stores to copy, as dear on the
  shader machine as the 600 to 800 instructions of drawing it.
- **The rest, 8 thousand,** and the kernel throughout: 12% of the first core's
  instructions are the kernel's (its tick, mostly), which is not the game's to change.

### 13 October 2026, second round: what the long frames are

Every frame's instructions are kept and said at the end (`TDAWN_FRAME_LOG=1`, three stores a
frame: `TDAWN_SLOW` times every object's turn and prints, which makes a frame of mission 3
123 thousand where it is 87). Before and after this round (the frames made ready by six
cores and asked for ahead, the game's own drawing from them, a message's expiry, the
refresh in turns, and Red Alert's own in `docs/games/ralert.md`; the counts made again), at
`rvc_cpu --ips 2000000`, windows of a second and a half, the first left out:

| | Average, before | after | Lowest window, before | after | Windows under 20, before | after |
|---|---|---|---|---|---|---|
| Mission 1 | 37.1 | 37.5 | 24.0 | 29.0 | 0 of 20 | 0 |
| Mission 3 | 21.5 | 21.5 | 14.4 | 15.0 | 9 of 36 | 11 |
| Mission 10 | 26.9 | 26.8 | 17.0 | 17.9 | 2 of 28 | 2 |
| Red Alert, Soviet | 30.1 | 30.2 | 18.5 | 17.7 | 2 of 25 | 2 |
| Red Alert, Allied | 21.7 | 22.0 | 10.4 | 11.9 | 5 of 35 | 4 |

(A window's figure moves by half a frame a second with where its stat line falls: the line
itself is 50 to 80 thousand instructions of console, in every window of both columns. Red
Alert's Soviet opening is the same frames before and after: 102 thousand instructions a
frame in frames 14 to 45, 26 of them the power bar and the credits' tab counting up.)
At `--ips 3000000 --ticks 8192` the lowest windows are 40.5 to 46.8, 23.1 to 22.9, 29.1 to
27.3, 30.9 to 29.2 and 16.9 to 17.8.

On the shader machine (D3D11 with fxc2, 3.9 to 4.1 million instructions a second, through
the card's queue, before and after in turn): Red Alert's Allied mission, 600 frames, 50.6
and 49.0 frames a second to 52.1 and 52.4, but its lowest window 34.2 and 37.6 to 30.9 and
30.3 (the landing, which is slower there than before: not looked into); mission 1's 800
frames 87.5 and 87.6 to 89.4 and 87.9, lowest 70.6 and 69.7 to 72.6 and 72.0; once each, Red
Alert's Soviet mission 69.3 to 70.3 (lowest 53.4 to 55.2) and mission 3 54.6 to 55.4 (41.5
to 42.3). The state sums are the same.

What went: the frame in which Red Alert's sidebar comes (1,727 thousand instructions to
521: its window 10.4 to 18.5), the frames in which a message expired (600 to 900 thousand),
the scene's refresh (90 and 200 thousand every 256th scene), a mission's first scene (505
thousand to 327 in Red Alert's Allied mission), mission 1's lowest window. The goal of
that round, every window at 20, is not reached: what every window under 20 is now, by its
frames of over 150 thousand instructions (34 in mission 3, 14 in mission 10, 9 and 39 in
Red Alert's two):

- **A unit's path search**, nearly all of mission 3's and mission 10's (150 to 430
  thousand; mission 10's frame 188 is several units, 1,353 thousand), all 7 of mission 1's,
  and Red Alert's of 150 to 270 thousand. It is the first core's own first search (below):
  one long walk, which the unit's turn waits for. Mission 3's battle (frames 540 to 760) is
  at 15 to 19 for these and for its frames between, which are 95 to 100 thousand.
- **Red Alert's landing** (the Allied mission's frames 490 to 560, 11.9 and 12.5 frames a
  second): logic 70 to 220 thousand a frame for 126 to 158 objects, the cells' walk 16 to
  29 thousand every frame (the shroud opens as they land), 50 to 80 objects drawn anew.
- **An aircraft turned** (Red Alert: 249 and 241 thousand for a large one, 40 thousand for
  each new turn of a small one): `Scale_Rotate` on the first core, 83 thousand of the 249.
- Red Alert's Soviet frame 130, 532 thousand (it was 680): 437 thousand of it one unit's
  first sight of new land, a cell at a time (`MapClass::Sight_From`).

Not done, and what it would take: a unit's search a frame ahead on a worker (its inputs
are what `Can_Enter_Cell` says of the cells it asks about, as if the unit were lifted
already: a search made on a table of those taken at a frame's end is the search the unit
would make if the answers it used are the same when its turn comes, which 32 workers can
say in two passes; the unit, its cell and its destination have to be guessed a frame
before); the turning of a shape on the scene's core; the cells' part of a scene made again
only for the cells that changed. Not kept: the objects' turns of the refresh by a sum of
the object's address (six instructions an object a scene, a thousand a frame: by its place
in the layer it is two).

## Checking it

The game is integer-only and, with a seed, plays the same on any CPU. Both ends print a sum
of the game's state (every unit's, soldier's and building's place and strength, and the map's
overlays): build the patched tree natively with a `host.c` that only writes the shown page
to a file, hold both at one frame, and compare the sums and the pictures.

    TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=10 TDAWN_STOP_FRAME=300 tdawn FROMINSTALL
    tdawn: stopped at frame 300: SCG10EA, 15 units, 23 infantry, 38 buildings, 552 overlays, state f7650705

The sums of missions 1, 3 and 10 agree with an x86-64 build at frame 300, on both backends.
The pictures (`--gpu-capture`: the game's picture is the top left of the GPU's target) differ
only where real time shows: the pointer, a tooltip under it and the credits counter counting.
The palette's cycling (water, the selection colour) goes by the clock in play and by the
game's frames in a run with `TDAWN_NO_DELAY`, or two runs' pictures would differ in it.

### A scrolled picture

Nothing above moves the map, and what the scene keeps (a cell's quad, an object's rectangles
by their place on the map, the shroud's part, the page's holes) is kept across a scroll. The
check is a sweep by single pixels: `TDAWN_DRAG=20` drags the map from the 20th frame as the
right button does (through `ShaderEmu_Drag`, so by the same thirds of a lepton), a pixel a
frame for 1,320 frames: thirty each of the eight ways and back, a cell's 24 x 24 places row
by row, then 120 each way and back, which runs into the map's edges. `TDAWN_SELECT=10` keeps
the bars and corners of the player's own in the page. Three things are held against it:

    TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=1 TDAWN_DRAG=20 TDAWN_SELECT=10 TDAWN_SUMS=1 TDAWN_STATS_MS=0 TDAWN_FRAMES=1360 tdawn FROMINSTALL
    the same with TDAWN_REFRESH=1, then: python tools/tdawn_sums.py A.log B.log
    1360 frames in both (1360 and 1360), 1127 distinct views; view differs in 0; picture differs in 0, the page over the map in 0

- **The kept scene against one made whole every time** (`rvc_cpu`, a minute): `TDAWN_SUMS=1`
  says a sum of the map's part of the picture after every frame, of the scene drawn here in
  the program (`host_scene_picture()` in `host.c`: the kept cells, the three parts'
  rectangles from the atlas, a sum of what darkens a pixel, the page over it). Missions 3 and
  10 differ in one frame each (a 6 x 7 thing in a battle, a frame late: not looked into).
- **The GPU's picture against the program's** (`rvc_harness --cpu`): `TDAWN_CHECK_GPU=1`
  reads the window's own buffer after every frame, where the GPU leaves its picture, and
  counts the pixels of the map's part that are not the program's (those something darkens
  and the pointer's surroundings left out): `1320 frames held against the GPU's picture, 0
  of them differ`, in parts of 310 steps (`TDAWN_DRAG=20,300` begins at step 300) at the
  world's size: a display and a GPU target of 1,280 x 720, the window 960 x 600.
- **At one held frame**, `TDAWN_CHECK_REDRAW=1` forgets everything (atlas, kept quads, the
  frames made ready: the game builds its own) and counts what a scene from nothing changes
  (a tooltip that still waits for its delay counts: a full redraw prints it at once), and
  `TDAWN_PICTURE=1` prints the picture for `python tools/tdawn_picture.py UART.log
  CAPTURE.bmp` to hold against the harness's `--gpu-capture`.

None of the three sees a fault in what the game itself draws into the page (bars, corners,
lines): all ways of drawing share that code. Red Alert has the sums, the GPU's check and the
selection under the same names (`docs/games/ralert.md`).

On 13 October 2026, after a report from the world of stray pieces beside a unit at single
scroll offsets: all 1,320 steps of missions 1 and 3 by the first two checks, mission 10 by
the first: nothing but the two frames named, at a window of a whole multiple of the game's
screen.

**It was the window's size.** In a window that is no whole multiple (the developer's was 794
x 545: a game pixel 2.48 by 2.73 of the window's) a quad's left or top edge can lie exactly on
a window pixel's middle, and the card then takes the texel one before the quad's own
rectangle: the next icon or shape of the atlas, as a column or a row a pixel thick, at single
scroll offsets. From a snapshot made with it on the screen (`foreign.py` of that session: for
every quad, whether a covered pixel's texel is outside its rectangle): a column of 442 pixels
(five terrain cells and two of the shroud's shapes) and a row of 120. The software rasteriser
(`--warp`) does not do it: this is checked on the card, `rvc_harness --cpu`. A compact
vertex's texture place is a whole texel and the GPU has no clamp for a rectangle of an atlas,
so the atlas has gutters: an icon and a shroud shape each in 26 x 26 with its edge said again
round it (1,482 icons where there were 1,764; the shades' texture 256 x 256 words), a shape
with a ring of holes. A held frame at 794 x 545: 119 and 114 of 150 quads with another's
texel beside or above, then 1 and 0; the sweep's first 330 frames (`TDAWN_RESIZE=794x545`)
16,315 pixels not the program's, then 670 (the largest frame 370, then 30); at 640 x 400 the
same picture as before. Left: a shape whose edge is on a middle loses that column to the
ring's holes, and the game's own pointer still takes its far column there. Not run on the
card: Red Alert, which has the same atlas.

## The scene on a worker core

Making a frame's rectangles (some two hundred: shapes, shadows, the shroud's edges) into GPU
commands was an eighth of a mission's frame, and it is the one part of the frame nothing else
waits for until the frame is shown. On a machine with worker cores (`docs/machine/multicore.md`) it
is a worker's (`linux/tdawn/host.c`, which Red Alert shares):

- `host_sprite()` and `host_block()` note what was asked, 32 bytes each;
  `host_scene_end()` gives the notes to the worker, which makes the commands as this core
  did (`scene_job`).
- `host_present()` of that scene is put off while the worker has it. The game goes on to
  its next frame's logic, and the frame is shown when the worker has done: from
  `host_event()`, which the game asks every frame, or before the next scene begins at the
  latest. A pass or two of the machine later than before; nothing is drawn into the page in
  between.
- The GPU's library has one state, so nothing of `host.c` calls it while the worker has the
  scene (`scene_finish()` first: the pointer's picture, a movie's, an event of the window's).
  It is compiled with a section a variable, each starting on 16 bytes (`build.sh`), because
  two cores must not store to the same 16.
- A waited-for job would cost more than it saves (a pass or two at each end): the putting
  off is what makes it pay. A scene of fewer than 20 notes is made here as before.

Mission 10, drawing: 51 to 43 thousand instructions a frame (Red Alert's first Soviet
mission: 91 to 67 thousand, 16.4 to 17.8 frames a second). The state sums are the same and
so is a held frame's picture but for the pointer, which is drawn by the clock.
`TDAWN_SCENE=inline` is the way it was.

## The workers on the second layer (14 October 2026)

All of the game's worker cores are now taken through `programs/mc/mcwx.h` ("The second
layer", `docs/machine/multicore.md`), and any worker does any job: the scene's (`mcw_call`, asked with
`mcw_ready` and joined), the files of frames, a unit's other path searches and the 32 that ask
about cells (`mcw_try_call`, added to `mcwx` for this: on a worker with nothing to do, or not
at all, so that a search or a file is never made on the first core by mistake). There are no
fixed numbers of workers any more: 42 are opened (10 in Red Alert, which has no workers that
ask about cells) and whichever is free takes the next job.
Red Alert's movie shares a frame's rows with `mcw_for` and unpacks the next frame's pointers
with `mcw_try_call` (`linux/ralert/workers.c`). With `TDAWN_MARK` (`RALERT_MARK`) the game
counts what each kind of job ran in `mcwx`'s counters and says it before the second mark
(`tdworkers:`): the jobs' own instructions are all but some 5% of what the workers ran, so the
workers are not waiting in any number to speak of.

New work on the workers, each exact (the state sums are the same):

- **The ground layer's sort** (`LayerClass::Sort` in `layer.cpp`, both games): a worker makes
  the pass from where the objects are while the first core draws the frame (`Map.Render`,
  which moves nothing), into a list of its own, and `Sort()` takes that list. 3.6% of
  mission 3's first core. `TDAWN_CHECK_SORT=1`: `1300 passes made by a worker core, 0 of them
  wrong` in both games.
- **The houses' threat** (`cell.cpp`, Tiberian Dawn): a unit that steps changes nine regions of
  every enemy house; the first core only notes the region, the houses and the value, and at the
  frame's end a worker adds them (sums: their order is no matter). Whoever reads threat
  (`Cell_Threat`) settles first, and so does a long path search before its helpers begin; a
  search on a worker reads threat as it is. 1.4% of mission 3's first core (`TDAWN_THREAT=here`
  against it in one build). `TDAWN_CHECK_THREAT=1` sums every house's regions wherever they
  are read: the same 3,974 reads and sum both ways.
- **The scene's rectangles** go to the GPU's library in batches of 128 (`seglSprites`, added to
  `programs/linux/gles.c`): the same commands and vertices as `seglSprite` one by one
  (`TDAWN_CHECK_LIST=1`: the same sum of 1,333,424 words both ways), with the state kept in
  registers. The scene's job is 20.0 to 13.6 thousand instructions a frame in mission 3 and 42
  to 28 in Red Alert's Allied mission: the workers', not the first core's.

And on the first core: a target scan's cell is asked without asking again whether it is on
the map, which the box was already kept to (`SHADEREMU_THREAT_AT` in `techno.cpp`): 1.2%.

`python tools/game_par.py`, three runs each, before and after (the same shared root):

| | Core 0 a frame | All cores | Factor | Guest seconds |
|---|---|---|---|---|
| Mission 3 | 84.1k to 82.2k | 122.4k to 121.1k | 1.45 to 1.47 | 22.89 to 22.35 |
| Red Alert, Soviet | 52.8k to 52.5k | 85.1k to 80.1k | 1.61 to 1.53 | 14.38 to 14.29 |
| Red Alert, Allied | 80.7k to 79.2k | 133.9k to 122.8k | 1.66 to 1.55 | 21.92 to 21.50 |

The workers' jobs a frame after (`tdworkers:`): mission 3 the scene 13.6k, frames 9.0k, path
searches 7.4k, cells 3.5k; Red Alert the scene 13.3k and 28.1k, frames 13.2k and 13.6k. The
factor falls in Red Alert because the scene costs the workers less, not because they do less.

What was tried and not kept: a scan's box read whole first, to pass over rings with nobody in
them (2.5% slower: most scans end at a quarter of their range); the scene's vertices written by
the game itself into the list (no better than the batches); the compiler's counts made again
from the missions (the old counts were better by a half to one percent, though they miss the
functions changed since). Why the first core is not half: its frame is the logic (`docs/games/tdawn.md`,
"why a frame is not half"), every object's turn reading what the turn before it stored, and
what a worker can make exactly is what reads a state that holds still for two passes: the
drawing's time (the sort) or the next frame (the threat). A target scan or a unit's search
made ahead would need every store that could change its answer told of, which is the next step.

## The objects a frame behind

The objects' part of a frame's scene is drawn by a worker while the first core makes the next
frame's logic (`Behind_Note` and `Behind_Draw` in both games' `gl.cpp`, `host_scene_behind` in
`host.c`). The first core, where `Render` was, does what a frame's drawing does to the game:
for each object of the layers it decides as before whether it is passed over, shown from its
kept rectangles or drawn anew; one drawn anew in the view is copied as it is (its whole object,
by its kind's size), and the first core does to the live object what `Render(true)` would have
(its mark, the animation's, a door's redraw flag, a line's frame count in Tiberian Dawn, where
the object drew nothing). The cells and the shroud stay the first core's. After `Map.Render`
(the sidebar drawn), the worker draws the copies, notes the objects' rectangles and makes the
scene's commands; the frame is shown when it has. The next frame joins it before its input.
A frame is shown a frame later than it was; the held pictures are the same.

What the worker stores outside its own memory is in texels of its own (`drawing` in `gl.cpp`,
`own` and `scene_noted` in `host.c`), a frame's shape it needs that no core has made is asked
for by the first core at the join (`frame_seen`), it waits for the GPU by ending passes, and it
never allocates: Red Alert's turned shapes have their buffer kept (`turned()`), and a type's
dimensions are not made on a worker (`techno.cpp`). The game's shape flags (`IsTheaterShape`,
`UseBigShapeBuffer`, set around a building's, an animation's and a turned object's drawing) are
the first core's and left alone on the worker (`ShaderEmu_Shape_Flags()` in `externs.h`): its
stores undid the first core's beside them and showed it a wrong flag (`RVC_RACES` in
`RALERT_ATTACK`, 15 October 2026). `TDAWN_DRAW=here` (`RALERT_DRAW=here`) is
the way it was; the `tdworkers:` line says the worker's part ("objects behind").

`python tools/game_par.py`, three runs each, the same build with `TDAWN_DRAW=here`/`RALERT_DRAW=here` and without:

| | Core 0 a frame | All cores | Factor | Guest seconds |
|---|---|---|---|---|
| Mission 3 | 83.1k to 79.6k | 121.5k to 123.9k | 1.46 to 1.56 | 22.60 to 21.70 |
| Red Alert, Soviet | 53.1k to 51.9k | 80.4k to 77.7k | 1.52 to 1.50 | 14.46 to 14.14 |
| Red Alert, Allied | 80.9k to 76.5k | 124.4k to 127.9k | 1.54 to 1.67 | 21.96 to 20.98 |

Against the binaries before 14 October 2026 (84.1k, 52.8k and 80.7k): 5.4%, 1.7% and 5.2% of
the first core. It is not the half: copying an object costs about what drawing it did (a
word copy loop was slower than `memcpy`), and the walk through the layers that says what to
copy is the first core's (the decisions need its marks); what went is the drawing of the
objects in the view. Checked: the state sums at frames 300 and 1,300, `TDAWN_CHECK_PATHS=1`,
`RVC_RACES` 0 undone in the three workloads, the movie check, and held frames of mission 3
(300, 600, 750) and the Allied mission (262, 500, 800) by `rvc_harness --cpu --warp` before
and after: the same pixels but for Red Alert's palette cycling by the clock, which two runs
of one build show too.

## Selected units: their bars (15 October 2026)

Reported from the world: the health bars of selected units blinked, and while selected units
moved on the screen a game ran at about half its speed, back to full when they left it. Two
causes, both games:

- **The bars went into the wrong page.** Between frames the game's page is the seen one
  (`Render` sets the hidden one and gives the old one back), and the worker that draws the
  objects a frame behind drew a selected unit's bars and box there: each one copied the whole
  hidden page into the seen one first (`Catch_Up`, the whole page, on the worker while the first
  core waited at the join), and the frame was shown from the hidden page without them,
  the next showing of the same scene from the seen page with them. `page copies` in the stat
  line was 1 a frame with units selected (0 before). The worker now draws into the hidden page
  (`ShaderEmu_GL_Post`), and the first core has its own page back at the join (`Behind_Joined`,
  called by `host.c` on the first core).
- **A bar made the whole page holes.** Anything the game draws over the map itself marks the
  page touched, and the next scene filled all of the view's part of the page and drew the
  object anew: this was so before 14 October too (mission 1, six units selected and moving:
  124 thousand instructions a frame against 65). The bars and box are now `gl.cpp`'s
  (`ShaderEmu_GL_Bars`, called from `TechnoClass::Draw_It`): the same pixels as the game's
  calls (`Remap`, `Draw_Rect`, `Fill_Rect`, `Draw_Line`, clipped to the window), written into the
  hidden page without its lock (whose count is the first core's), kept with the object by the
  map's pixels and put again every frame it is shown as it was, and made holes again by their
  own pixels before the next scene. An object with bars stays as it was drawn.

`TDAWN_MOVE=100,8,0`, `rvc_cpu --ips 2000000`, the stat windows ending in frames 121 to 330
(core 0's thousands of instructions a frame; HEAD is the build of 5680284):

| | Before 14 Oct | HEAD | Now | Nothing selected, now |
|---|---|---|---|---|
| Mission 1, 6 sent | 15.1 fps, 124k | 18.2 fps, 74k | 27.0 fps, 66k | 30.6 fps, 59k |
| Mission 3, 12 sent | 9.9 fps, 188k | 14.4 fps, 110k | 17.1 fps, 108k | 18.6 fps, 99k |
| Mission 10, 15 sent | 9.2 fps, 204k | 13.7 fps, 112k | 16.5 fps, 112k | 18.7 fps, 99k |
| Red Alert, Soviet, 12 sent | 8.1 fps, 230k | 8.1 fps, 115k | 18.5 fps, 99k | 21.4 fps, 86k |
| Red Alert, Allied, 15 sent | 6.8 fps, 274k | 9.0 fps, 109k | 16.0 fps, 102k | 18.4 fps, 89k |

(HEAD's first core ran fewer instructions a second: it waited for the worker.) Checked: held
frames 150 to 153 of mission 1 and of the Soviet mission (`rvc_harness --cpu --warp`) the same
pixels as before 14 October but for Red Alert's water by the clock, where HEAD's had no bars
or boxes at all (mission 1: 1,164 white pixels of 1,912); `TDAWN_SUMS` with
`TDAWN_REFRESH=1` against without: 0 frames differ with `TDAWN_MOVE` in mission 3 and the
Soviet mission and in the drag of mission 1 (`TDAWN_DRAG=20 TDAWN_SELECT=10`), the page the
same as a build that drew the bars anew every frame; the state sums at 300 and 1,300 frames
of missions 1, 3 and 10 and both Red Alert missions as before; `TDAWN_CHECK_PATHS=1` 0 wrong;
`RVC_RACES` 0 undone with units selected and moving (a worker's `Lock` of the page was undone
by the first core's: hence no lock). `TDAWN_CHECK_GPU` cannot see the fault: it reads the
hidden page as the program's picture.

Found on the way and not changed: in the Allied mission's drag (`RALERT_REFRESH=1` against
without, nobody selected) a soldier is drawn a pixel high for 71 frames (418 to 488): a shape
that changes without the object marked, as kept; HEAD has it too.

## A long battle (15 October 2026)

Red Alert's base destroyed (`docs/games/ralert.md`, "A base destroyed") found three things in the
code both games share, mended in both: the atlas, once full, left every new shape to the game
for good (now its shapes are forgotten before a scene that may not fit and put again as they
are drawn: `shapes_forget`, `TDAWN_ATLAS=N` every N scenes for a test); a shaken screen copied
the page 2 x shakes + 2 times (now the same random picks and frames without the copies;
`TDAWN_SHAKE=copy` is the old way); and the worker that draws the objects a frame behind read
the object an animation is attached to while the logic could destroy it, and built a frame no
core had ready with the game's `Build_Frame`, whose buffers and heap are the first core's (now
the copy is detached as it is copied and the frame made into the worker's own buffer:
`ShaderEmu_Frame_Into`; the game's own drawing is not made on the worker). Mission 3's 600
frames of `TDAWN_MOVE=100,8,0` have the same picture sums as before, with and without
`TDAWN_REFRESH=1`, and the state sums are the same.

## Target scans kept, tried

A target scan's answer was kept with everything it read, as Doom's sight checks are
(`docs/games/doom.md`): the scanner's place, house, allies, range and cell, every object of the cells
it asked about as it was (place, house, allies, limbo, down, discovered, owned, cloak, kind),
and a generation for each block of 8 x 8 cells its box covered, bumped at every store to a
cell's occupiers. A later scan of the same object and method used the answer when all of that
was as it had been; `TDAWN_CHECK_SCANS=1` made each such scan again: 742 answers used in mission
3's 1,300 frames, 0 of them wrong, and the state sums the same. It was not kept: a quarter of
the area scans could use their answer (mission 3's units are rarely all still), and noting what
a scan read cost more than the answers saved, 83.8 thousand instructions of the first core a
frame against 81.1 without it in one build. A worker making the scans ahead would have raised
the share and not the cost of the notes and the checks. What a profile of scans said (1,300
frames): area scans 77% of their instructions (soldiers 2,247 at 2.1 thousand each, buildings
744 at 4.5 thousand), scans of what is in range the rest; none of the whole map in mission 3.

Kept instead, on the first core: `Evaluate_Object` measures the distance after the tests that
only say no (`techno.cpp`), which most objects of a scan fail first: 79.6 to 79.2 thousand
instructions a frame, the state sums the same.

## The start

From the command to a mission's first frame Tiberian Dawn took 76 million instructions and
Red Alert 197 (38 and 98 seconds at two million a second); they take 44 and 140 now. What a call graph of the start
showed, and what was done about it, in the two games' shared code (`common/`):

- **A folder was listed for every file looked for**, to find it in any spelling: 644
  listings before Red Alert's first frame, each an open, the reads and a pattern match for
  every name. A folder's names are read once now (`ShaderEmu_Folder_Of` in
  `file_posix.cpp`), a name without wildcards is compared, not matched, and the game's own
  writes and deletions make the names be read again (`rawfile.cpp`).
- **A file was opened to see whether it is there**, 1,370 times in Tiberian Dawn: a file
  that is not among its folder's names is not there, and one that opened once is not opened
  for that again (`ShaderEmu_File_Listed`).
- **A mix file kept in memory was read into the heap**: the file's pages and a copy of
  them, 25 million instructions for Red Alert's 24 MB and as much memory twice. It is
  mapped from its file now, all pages there at once (`ShaderEmu_Mix_Map`: a worker core
  that unpacks shapes must not meet a page that is not there), and a page the game writes
  to (a shape's header) becomes a copy of its own. Not for a mix inside another, nor one
  whose sum is checked.
- **Red Alert's title picture was unpacked five times** on the way to a mission (2.4
  million each): the one last unpacked is kept (`Load_Title_Screen`).
- **Tiberian Dawn's cells were made one at a time** sixteen times, a call to clear each:
  cleared in one piece (`MapClass::Init_Cells`).

Not done: the two `.ini` files read a line at a time (15 million in Red Alert), the key
arithmetic for Red Alert's encrypted mixes (7), its map's zones filled cell by cell at a
scenario's start and whenever a wall falls (7 each time), the intro's first frames.

## What the compiler is told

Both games are compiled with counts from played games (profile-guided optimisation): how
often every branch was taken, and which function each virtual call reached. 9% of a Red
Alert battle's frame, 2 to 4% of Tiberian Dawn's, on top of the whole-program build. On
this machine that is all instruction count: which calls are inlined (a virtual call that
nearly always reaches one function becomes a test and that function's body), which path
falls through, what stays in registers.

- The toolchain's own counting library is of another float ABI, so `host.c` has the
  functions the compiler's counters call (`SHADEREMU_PGO`; an empty `libgcov.a` is put in the
  toolchain's way at the link): when the program ends they print every counter that counted
  to the console, as lines beginning `GC`. Checked against the same compiler's own library
  on this computer: the same counts in the same file, but for the order of a call's targets.
- `TDAWN_PROFILE=count linux/tdawn/build.sh` (and `ralert`'s) builds the program that
  counts, three times as large and a third as fast. Play what should be fast, with
  `TDAWN_SCENE=inline TDAWN_FRAMES_CORE=0` (the counters are not two cores' to write): the
  missions of "Speed" for 1,200 frames, and Red Alert's with `RALERT_HUNT=50` for 800. Build
  both games again at once after: the counting programs are in the image's folder meanwhile.
- `python3 linux/tdawn/profile.py make linux/tdawn/tdawn.profile LOG ...` sums the runs'
  logs into the file the build keeps (500 KB of text; `linux/ralert/ralert.profile`), and an
  ordinary build writes the compiler's `.gcda` files from it (`profile.py put`) and compiles
  with `-fprofile-use -fprofile-partial-training`: what no run touched is compiled as
  before, not for size. A call's targets are known by a sum that has the source file's path
  in it: count with the repository's build script, not in another folder.
- Red Alert counts only its own folder (`-fprofile-exclude-files=/common/`): with counters
  in the shared code its start reads no file of its encrypted archives (something in the
  key arithmetic does not survive the change of code; not looked into further).

The counts are of a tree's functions as they were: a function changed since is compiled
without them (no warning: `-Wno-coverage-mismatch`), so a profile ages gracefully and is
worth making again after a large change. The state sums are the same with and without.

Quake and Quake III are compiled the same way (29 October 2026), with their own counts: `linux/quake/quake.profile`,
`linux/quake3/quake3.profile`. Their counters are `linux/userland/pgo/gcov.c`'s
(host.c's functions, the values kept in a pool of its own, not malloc'd, since worker cores run counted code too), and
they are written at the program's end or at `kill -USR2`, so a game that never ends by itself is counted part way:

- `QUAKE3_PROFILE=count linux/quake3/build.sh` (`QUAKE_PROFILE`, and `DOOM_PROFILE`, `HALFLIFE_PROFILE`
  for the two below that keep none) builds the counting program in a folder of its own
  (`$WORK/src/*-count-build`); `=off` builds without the counts kept. The counting build has the same drivers and
  hidden globals as the one that uses the counts.
- Counted on `rvc_cpu`, each under a minute: Quake III `QUAKE3_STATS_FRAMES=100 quake3 +set com_maxfps 0 +set fixedtime
  33 +devmap q3dm17 +addbot sarge 3 & sleep 115; kill -USR2 $!` (the level's first 1,700 frames); Quake `quake
  +timedemo demo1` (100 s) and `QUAKE_TRACK=5 quake +map e1m1` (90 s), each sent USR2 at the end. `profile.py make`
  makes the file from the runs' consoles.
- Quake on the card, two rounds each against the same tree without counts: its timedemo 73.0 to 75.4 fps, the server's
  ticks in e1m1 58.0 to 68.0 a second, the start to the level 16.4 to 15.6 s, but its first frame 0.44 to 0.50 s (the
  start's code is run once in a count, and is compiled as rarely run: 1.25% more instructions to "Quake Initialized").
  Quake III's start to the level 27.8 to 25.8 s.
- Not kept: Doom (`doom -1 -timedemo demo1` and `demo2`, 110 s each): on `rvc_cpu` its timedemo 148.1 to 139.3 million
  instructions on the first core, but on the card 91.9 to 91.1 tics a second, at 11.2 rounds a frame for 10.5 (two
  rounds each). Half-Life (hldemo1 to second 43 of the level, in sections): 2% fewer instructions from second 31 to 43
  and the same frames, 162 to 163, for 1.7 MB of counts. Tiberian Dawn's and Red Alert's counts made again on this tree
  gave 0.1% and 0.4% fewer instructions on the first core: the old ones are kept.

## Frames made ready

A shape the scene has not shown before has to be unpacked (the game's `Build_Frame`: 5 to 18
thousand instructions, 200 thousand for a large one late in its file) and put into the atlas
a pixel at a time (3 thousand more): one or two a frame in a battle, three when
reinforcements land, and most of a mission's first frames. Both are other cores' work
(`host.c`, shared with Red Alert):

- Six workers unpack shape files (`TDAWN_FRAMES_CORE=N`: no more than N; 0: none). A file is
  unpacked in runs: 64 frames, or 128 KB of them, from the frame that was asked for, with
  the game's own unpacking (`ShaderEmu_Frame_Step` in `keyframe.cpp`, which touches nothing
  of the program's but its buffer, and makes a frame from the one before it where the file
  has it as a change: a file's last frames were its whole chain each). Of each frame it
  keeps the rectangle that has pixels, a bit for every index in it, and each row's first and
  last pixel. Then the file rests. A frame asked for that is not there, or one whose
  twelfth frame after it is not, has the file go on from there: at once if a core has it (a
  count the job looks at, `asks`), else on a core with none, in the same call.
- What will be asked for is asked for ahead, as a mission begins and in every frame in
  which something was made (`ShaderEmu_GL_Ahead` in `gl.cpp`): the file of every unit,
  soldier, aircraft and building there is, on the map or not yet (being built, on its way
  in a transport), a building's construction too, every animation's first run, and the
  files the game tells of (`ShaderEmu_GL_Ahead_File`: Red Alert's sidebar, `docs/games/ralert.md`).
- From those bits the first core knows a ready frame's shadow levels without looking at
  its pixels, gives it its place in the atlas, and leaves the pixels to the core that makes
  the scene (`host_fill()`): they are written before that scene's commands, and nothing
  else reads the atlas. A cell's icon is copied there the same way (`fill_icon`: a
  mission's first scene was 320 of them, 250 thousand instructions).
- **The game's own drawing takes a ready frame too** (`ShaderEmu_Ready_Shape` in
  `conquer.cpp`, `ShaderEmu_Ready_To_Page` in `common/keybuff.cpp`): a shape drawn plainly
  into the page (the sidebar's pieces, tabs, buttons) is copied from the worker's
  rectangle, a row's run by its span, and is neither unpacked nor measured by this core.
  `TDAWN_READY_DRAW=0` is the game's own way.
- The memory is the workers' alone to write: 8 MB of address space, of which 2 MB (3 in
  Red Alert) have their pages from the program's start, with the cores' buffers and the
  frames' table (1.9 to 2.4 million instructions of the start, about a second at two
  million a second: 128 KB brought in a mission's frame was 32 page faults, 70 thousand
  instructions). More comes two pages a frame when less than 256 KB is left.
- A frame asked for that is not ready hands the cores that are done their next files
  (`frames_pump` from `host_frame`): a score screen has no scene's frame to do it, and a file
  begun there was never finished, its frames built by the game at every drawing.
- `Reset_Theater_Shapes` and a new scenario stop the workers and forget everything
  (`host_frames_forget()`): a file may go then.

`TDAWN_CHECK_FRAMES=1` (`RALERT_CHECK_FRAMES=1`) has the game build every such frame too and
compares rectangle and pixels: `0 of them differ from the game's own` at the end.
`TDAWN_CHECK_READY=1` draws every shape of the game's own drawing both ways and compares the
window's pixels: `0 of them differ from its own`. In 600 frames of Red Alert's Allied mission
the game still built 55 frames itself (462 thousand instructions) where it had built some
150 (1.4 million, 836 thousand of them in the frame the sidebar comes): 17 of the 55 are an
aircraft's, which the game turns before they are put.

## A unit's path searches

A unit that needs a way searches up to four times in one `FootClass::Basic_Path`, by other
rules over the same cells with nothing changed between (from its `Mark` up to its `Mark`
down): first with everything it could shoot or wait for taken as passable, then with
nothing, then the rules between. A blocked search follows the obstacle's edge both ways, up
to 400 cells each: 40 to 400 thousand instructions, and mission 3 begins with eight units
doing it in one frame. Three things, all in `findpath.cpp` (`ShaderEmu_Find_Path`):

- What `Can_Enter_Cell` said of a cell is kept for the unit's searches
  (`ShaderEmu_Enter_Keep`): 64 thousand answers in mission 3's first 1,200 frames, 5% of its
  battle. `TDAWN_CHECK_ENTER=1` asks again each time: `0 of them wrong`. And a free cell is
  not measured for its distance (`Passable_Cell`: only a cloaked blocker's matters).
- The rules differ only in a threshold that is compared with a cell's answer and nowhere
  else. A search notes which answers it met; one that met none between its threshold and
  another's went the other's way step for step, and that one is not run: its path, its
  moves and its list of cells are put back as the search would have left them.
- A search whose edge walks have come by 48 cells is a long one, and the unit's other
  searches begin then on worker cores, each with its own threshold, while the first core
  goes on with its own (`ShaderEmu_Path_Help`; up to 3, on whichever workers are free since
  "The workers on the second layer", `host_path_cores()` in `host.c`). A search stores only into its core's own memory (its path, its lists, its kept
  answers: `ShaderEmu_Core`, 16 bytes apart); which core is in the threshold's upper bits
  all the way down. Searches nobody asks the result of are stopped and waited for before
  the unit is put back. (Until 13 October 2026 the help was for the unit after one whose
  first search had been long, and one search at a time.)
- At the same moment up to 32 more workers ask `Can_Enter_Cell` about every cell of the
  map for that unit, two rows of it each (`ShaderEmu_Pre_Job`, `host_pre_cores()`,
  `TDAWN_PRE_CORES=N`): a long walk asks about some 900 cells nobody has asked about, 80 to
  250 instructions each, and two passes later every core's search finds the answer there
  (`Passable_Cell` looks in `ShaderEmu_Pre` before it asks). They store whole rows of two
  tables and nothing else, and have ended before the unit is put back.
- The walk round an obstacle is `ShaderEmu_Walk`: `Follow_Edge` step for step, with a cell's
  kept answer read in place and a cell that is new to the path put on it in place.
  `TDAWN_WALK=old` is the game's own.

`TDAWN_CHECK_PATHS=1` makes every such search on the first core too and compares path,
moves and cells, and makes every edge walk both ways: `50 searches not run again and 32
another core's, 0 of them wrong` and `278 edge walks made both ways, 0 of them wrong` in
mission 3's 1,200 frames (110 walks in mission 10, 64 in mission 1). `TDAWN_PATH_CORES=0`
is one core. Red Alert has the searches not run again, the helpers (started only when both
walks of a pair failed) and the walk, not the cells ahead (`docs/games/ralert.md`): its
searches are 2% of a battle.

What a long search is, measured (`rvc_cpu`, mission 3): one of its two walks reaches the
other side in 3 to 50 cells, and the other goes the long way round, 230 to 280 cells kept
of up to 400 walked, to lose the comparison; the game makes both to the end. That walk
was 150 to 195 thousand instructions for the first search (every cell new) and 105 to 115
for the next, and a unit made two or three such searches, or four walks when its way had
two obstacles. A frame with such a unit, of the 1,200:

| | Mission 3 | Mission 10 |
|---|---|---|
| Frames with a unit of over 100 thousand instructions | 23 | 10 (9 after) |
| Their mean, before | 434 thousand | 619 thousand |
| after | 266 thousand | 400 thousand |
| The longest, before | 859 thousand | 2,086 thousand (several units) |
| after | 426 thousand | 1,361 thousand |

What is left is the first core's own first search: one long walk, 100 thousand
instructions of 250 a step, and with two obstacles two. Its outcome is needed before the
unit's turn can go on, and a walk is one step after another.

Tried on the way and not kept: the answer by threshold kept for each cell of one search
(a tenth slower: a walk asks about most cells once or twice); the first core waiting for
the 32 workers and making its walk again from the start by reading their answers (the
walk is as long by reading as by asking the kept answers, 120 thousand against 125, and
the wait is three passes). Not done: the rest of a search with two obstacles made by a
worker from the shorter walk's end while the first core finishes the longer (three frames
of mission 3's 23, 480 thousand each); the two walks of one obstacle on two cores gain
nothing, since it is the long one that is waited for.

## Movies

Tiberian Dawn's movies are 320 x 200 in 4x2 blocks (`UnVQ_4x2`; Red Alert's are 4x4, `docs/games/ralert.md`,
"Movies, and the worker cores"). Since 15 October 2026 a frame's block rows are libmedia's
`media_vq2_rows` (`docs/system/media.md`, "VQA"), shared between worker cores: the drawer is given
`ShaderEmu_UnVQ_4x2` in place of the game's (`common/vqadrawer.cpp`, Tiberian Dawn's patch,
compiled out of Red Alert's, `SHADEREMU_RA`), and `build.sh` links `libmedia.a`
(`TDAWN_MEDIA=FILE` for another build of it).

- The movie has the workers as Red Alert's has them: the first frame closes the scene's
  (`host_workers_close()`, which the next mission opens again) and opens fifteen, and
  `Play_Movie` gives them back when the movie is closed (`ShaderEmu_Movie_End`).
- `TDAWN_MOVIE_WORKERS=N` uses no more than N (0: this core alone); `TDAWN_MOVIE_CHECK=1`
  decodes every frame with the game's own `UnVQ_4x2` as well, compares the two, and says at
  the movie's end how many differ (`tdawn: movie NAME.VQA: ...`; also with `TDAWN_FRAMES`).
- The demo has no movies. The decoder was checked on the build computer against the game's
  own C (`UnVQ_4x2` and `UnVQ_4x4` of the fetched tree, `media_vq2_rows` and `media_vq4_rows`
  with threads for workers): random codebooks and pointers, solid blocks among them, frames
  of 320 x 200, 640 x 400, 320 x 156 and 640 x 200, alone, inside a wider page and at an
  offset of 4 bytes, with 0, 1, 3 and 8 workers: 1,152 frames, 172.8 million bytes, 0 differ.
- And in the game, under `rvc_cpu`, with a synthetic VQA (version 2, 320 x 200, 4x2 blocks, a
  whole codebook every frame, a quarter of the blocks solid; no sound) linked under every movie
  name into `/tmp/tdawn`: `TDAWN_AUTO=1 TDAWN_MOVIE_CHECK=1` played the demo's briefing and
  action movies (`GDI1.VQA`, `LANDING.VQA`), 26 frames each before the Return, with 15 workers,
  0 of 26 differ; `RVC_RACES`: 0 undone. (The demo looks for those two; the intro's `LOGO` and
  `INTRO2` it does not play with its default settings.)
- With the full game's own movies (`DINO.VQA` and `NOD1PRE.VQA` of the multimedia samples at
  samples.mplayerhq.hu, their checksums the archive's; not kept in the repository): `DINO.VQA`
  under those names, 112 frames of 320 x 156 each time, 0 of 112 differ, and `RVC_RACES` 0 undone.
  Every picture of both files is ffmpeg's to the byte and the sound the game's own decoder's
  (`linux/libmedia/test/check_vqa.py`, `docs/system/media.md`, "VQA").
- That run first showed 2 stores undone: the profile line (every `TDAWN_STATS_MS`, 5 s at
  first) zeroed `gl.cpp`'s counters on the first core while the worker drawing the objects a
  frame behind (`Behind_Draw`) added to them and wrote beside them. The profile now settles the
  scene first (`host_scene_settle()`), in both games' `conquer.cpp`.

## A mission won (15 October 2026)

`TDAWN_WIN=N` wins the mission at frame N (once) and prints a line at each step, as Red Alert's
`RALERT_WIN` (`docs/games/ralert.md`, "A mission won", where a won mission waited 140 s for the score
screen's table of colours and the demo then asked for "out of memory"). Tiberian Dawn's demo
goes from mission 1 to 10 by its own code, and its score screen makes no such table: from the
win at frame 100 of mission 1 (`TDAWN_SCEN=1`) its score screen is 36 s at two million a
second (11.5 s at forty million), and mission 10 is then played with its state at frame 300
`325b1830` (`tdawn-won` in `tools/test_all.py`). Half of that screen's instructions are
`Linear_Blit_To_Linear`, the page copied whole at each of its steps; not changed. The table
and the doubled rows (`common/interpal.cpp`, `host_for`) are shared, for the screens that use
them.

## Not done

- The movies on the GPU, as Red Alert's (`host_picture`): the frame is still the game's page.
- The cloak effect on the GPU (the game draws such a shape itself, over everything), and the
  sidebar, radar and text, which are still pixels.
- Path finding in one piece: a search's two edge walks could be two cores' (a worker cannot
  post a job, so a worker's search would need a third core told by hand; "A unit's path
  searches" found the two walks of one obstacle on two cores gain nothing).
- Text that stays: a tooltip is printed again every frame it is up, 13k (a message is not).
- A soldier that walks is drawn anew every step, some 2,000 instructions through the game's
  own `Draw_It`: the larger part of a scene with many of them moving (on a worker now, "The
  objects a frame behind").
- Music in the missions. The full game's data (the demo's is 6 MB; the CDs' movies and music are not wanted).
- Red Alert is the same tree (`BUILD_VANILLARA`), built by `linux/ralert/build.sh` (`docs/games/ralert.md`).
- With `NANOX_SIZE=640x480` the window has no frame (640x400 leaves no room for one) and is
  left through the game's own menu (Escape).
