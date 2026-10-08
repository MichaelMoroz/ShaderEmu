# Tiberian Dawn

Command & Conquer (1995) runs in the Linux image: [Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer),
the portable version of the game's released source, as a Nano-X client. The GPU draws the map:
terrain, objects, shadows and the shroud ("The map on the GPU", below). The game still draws
its sidebar, its text and its menus itself, on its own 320x200 screen, which the GPU shows on top.
The window is the largest whole multiple of that the display has room for: 960x600 on the
desktop's 1280x720, which costs the game nothing (the GPU does the stretching).

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh     (9 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & tdawn` at the console (it is also listed for the desktop's Start
menu, which was not tried). The data is the 1995 demo's (GDI missions 1, 3, 5, 6 and 10; the
menu starts the first), which the build fetches and the repository does not keep. Effects and
speech play on the sound card (`docs/sound.md`); there is no music.

## What the build is made of (`linux/tdawn`)

- `build.sh` fetches Vanilla Conquer at a fixed commit, applies `vanilla-conquer.patch`, adds
  the files below and builds with CMake and the image's toolchain.
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
  the program from a folder of its own (the game lists its program's folder too): 70M.
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
  (`CellClass::Adjust_Threat`, 9% of mission 3). A scan for targets asks a cell whether anyone
  stands in it before the call that finds out (`Greatest_Threat`). A house is found by its
  kind from a table, not a search (`HouseClass::As_Pointer`). The layer's sort asks each
  object where it sorts once, not twice.
- **Scrolling.** The game moves the map a fixed step every pass of its loop, sized for a
  machine that makes some twenty passes a second; here a pass that scrolls is 400k to 600k
  instructions, six or seven a second. The step grows with the time since the last one
  (`ShaderEmu_Scroll_Distance`), up to 36 pixels: beyond that the game draws the whole map
  again instead of shifting it.
- A page's blit onto itself (how the map is shifted) copies rows in the order that reads each
  before it is written. Top to bottom, as it first was, a scroll downward smeared the map.

## The map on the GPU (`gl.cpp`, `host.c`)

The game drew the map into its page a cell and a sprite at a time, and that was half to two
thirds of a frame. Now the map is a scene of rectangles the GPU draws, and the game's page
goes over it with holes where the map is (index 0, which the game never draws).

- **Terrain stays.** Each cell of the map is a quad that is written once, when the cell is
  first seen, and kept (`seglKeep`): its picture is one of 42 x 42 icons in an atlas. Scrolling
  is a matrix: the same quads somewhere else, and nothing of the terrain is touched.
- **Everything else is rectangles of each frame**, in the order the game draws: overlays and
  smudges (the cell's own `Draw_It`, without its terrain), then every object of every layer
  (`Render`), then the shroud. `CC_Draw_Shape` is where they turn into rectangles: the shape's
  frame is put into the atlas the first time it is drawn (with its house colours, which are
  a table) and is a quad from then on. Only the part of a frame that has pixels is kept (a
  soldier is a small figure in a much larger frame), which is what the atlas has room for.
- **An object that did not change is not drawn again.** Its rectangles are remembered by
  their place on the map (`Kept`, six an object) and written again as they were while the
  game has not marked the object (`IsToDisplay`); one that was out of the view is not asked
  again until the view moves. The shroud's edge over a cell is worked out again only when
  the game says the cell changed. Every 32nd scene is made from nothing (`TDAWN_REFRESH=N`).
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
(`seglSprite`). In missions 3 and 10 logic is the larger part: in mission 10 looking for
targets is 8% of a frame and path finding 12%.

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
    TDAWN_SCROLL=N      scroll the map for N frames from frame 20: east, south, west, north
    TDAWN_SAY=1         in a mission, EVA says each line the data has, one after another
    TDAWN_SOUND_LOG=1   print every sound the game asks for, and whether the pack has it
    TDAWN_BUILD=N       put a refinery in view at frame N, which plays its construction
    TDAWN_CHECK_REDRAW=1  at the held frame, count the pixels a full redraw would change
    TDAWN_SKIP=N        how many frames in a row may go undrawn (0)
    TDAWN_RENDER=soft   the game draws the map itself, as before
    TDAWN_REFRESH=N     make every Nth scene from nothing (32; 0: never)

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

## Not done

- The cloak effect on the GPU (the game draws such a shape itself, over everything), and the
  sidebar, radar and text, which are still pixels.
- Text that stays: a tooltip is printed again every frame it is up, 13k.
- A soldier that walks is drawn anew every step, some 2,000 instructions through the game's
  own `Draw_It`: the larger part of a scene with many of them moving.
- Music. The full game's data (the demo's is 6 MB; the CDs' movies and music are not wanted).
- Red Alert is the same tree (`BUILD_VANILLARA`) and is not built.
- With `NANOX_SIZE=640x480` the window has no frame (640x400 leaves no room for one) and is
  left through the game's own menu (Escape).
