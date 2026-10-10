# Red Alert

Command & Conquer: Red Alert (1996) runs in the Linux image, from the same source tree as
Tiberian Dawn ([Vanilla Conquer](https://github.com/TheAssemblyArmada/Vanilla-Conquer)) and
on the same layer under it (`docs/tdawn.md`, `linux/tdawn`): a Nano-X window, the map drawn by
the GPU, the sound card. Its screen is 640 x 400, shown one to one.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/tdawn/build.sh     (first: it makes the C++ driver)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/ralert/build.sh    (20 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nano-X -p & ralert` at the console, or Red Alert in the Start menu's Games (not
tried from there). The data is the 1996 demo's: one Soviet and one Allied mission and
skirmish games, 24 MB, which the build fetches and the repository does not keep. Effects,
speech and the demo's two tunes play on the sound card.

## What the build is made of (`linux/ralert`)

- `build.sh` puts the tree at Tiberian Dawn's commit, applies Tiberian Dawn's patch (the
  shared files) and then `vanilla-conquer.patch` (Red Alert's own files, and two shared
  ones), copies `linux/tdawn`'s `host.c`, `shaderemu.cpp`, `soundio_shaderemu.cpp` and
  `cxxrt.cpp` in, and builds `vanillara` with `-DSHADEREMU_RA`.
- `gl.cpp` is Tiberian Dawn's map renderer with Red Alert's names. What differs: a cell is
  "mapped" once any of it shows and "visible" when all of it does (the other way round there);
  a map may be 128 x 128 cells; a shadow has as many levels of darkness as its pixels ask for.
- The sound pack (`ralert-sound.pak`, 12.8 MB, 294 sounds) is made by Tiberian Dawn's tool.
  The sounds are in MIX files inside the demo's two, behind an encrypted index: the build
  compiles the source's own `vanillamix` for the host and unpacks them with it.

## What was changed for this machine

Everything under "What was changed" in `docs/tdawn.md` that is in the shared files applies.
In Red Alert's own:

- **No hash of the data.** A MIX file is hashed (SHA-1) as it is read into memory, to
  compare with the sum it carries: 24 MB, some 500M instructions before the title.
- **Fading tables** (the palette's nearest colour to each faded colour, 65,000 distances a
  table, forty tables before the first mission) are made by a search that looks only at
  colours whose green is near enough to matter. The same tables.
- **The vortex's tables**, sixteen more for each theatre, are made when a vortex first
  appears. (The GPU's scene does not draw the vortex at all yet.)
- **Music that is not there.** A mission names its tune; the demo has two (Hell March and
  Crush). A tune that did not start was asked for again every frame, and each asking lists
  the folder, a million instructions: now another tune is picked, and after four failures
  none.
- **The radar's cover plate**, 160 x 141 pixels and three buttons, was drawn again whenever a
  unit moved (the unit asks for its dot on a radar that is not there): 160k a frame.
- **An object that moved does not change the cells it left and came to**, and a unit that
  looks again at a cell it has seen does not either. The game asks all of those cells to be
  drawn again, a hundred a frame; on the GPU only the scene is made again, and the renderer
  keeps a mark on every cell that has nothing of its own to draw.
- **The first run** goes past the menu straight into the Soviet mission, as after an
  install: the starter writes the setting that says the game has been run.
- The logic that repeats itself is Tiberian Dawn's, with the state sums unchanged: threat of
  a unit that steps, houses found from a table, the layer's sort, and the computer's count of
  units no team has (a unit is asked whether it could join only when one is wanted).

## The map's cells, a frame

Going through the cells in view was 35 thousand instructions of a frame early in a mission,
when most of the map is under the shroud, and is 19 now (`linux/ralert/gl.cpp`):

- a run of settled cells in a row, or of cells never seen, is passed over or made one black
  rectangle without asking the game about each cell;
- a cell with ore, a wall or a scorch mark keeps the rectangles it drew, as an object does,
  and they are put again until the game says the cell changed;
- a plain cell at the shroud's edge keeps its edge and is asked nothing unless the game has
  flagged it.

The state sums are the same, and a held frame is the same picture as with everything drawn
anew (`RALERT_REFRESH=1`) but for trees below the map's last row, which differed before. The
first Soviet mission: 16 to 18 frames a second in its first 300 frames where it was 15 to
17, 24 to 32 later; its logic is 105 to 130 thousand instructions of a frame's 180 to 200.

## A battle

`RALERT_HUNT=N` sends everything the player has after the enemy at frame N, which is the only
way a mission left alone comes to a battle. The Allied first mission with `RALERT_HUNT=50`:
a frame is 220 to 265 thousand instructions, of which the game's logic is 155 to 190 (it is
105 to 130 in the quiet mission), 14 to 17 frames a second in the harness. The profile of
it has no piece over 3%: threat scans, path finding, the houses' and each object's own turn.
A played mission in the world was measured at 551 thousand a frame (logic 359, drawing 127,
the rest 65), 5 frames a second at the world's 2.9 million instructions a second: the logic
alone would be 8 a second there. That was with the pointer in use, and a pointer that moves
was dear in every part of the frame, not only "the rest": the window system woke for each of
100 reports a second, and the game asked it for the moves wherever it looked for input. The
quiet mission with the pointer going round was 10 to 11 frames a second where it is 19 to 20
with the pointer at rest. The game reads the pointer from the machine's input words now, the
kernel reports a moving pointer 10 times a second while a program runs, and the pointer
itself is the display's cursor: 17 frames a second with the pointer going round
(`docs/input.md`, `docs/tdawn.md`).

## Movies, and the worker cores

The demo's opening movie (ENGLISH.VQA, 640 x 400, 156 frames of it before `TDAWN_AUTO`'s Return
ends it) was 41 of the 100 seconds from the command to a mission's first frame: a million
instructions a frame, and more than that in stores. A frame was decoded into a buffer and
copied to the screen, half a megabyte of stores, and the machine's core keeps 6 KB of stores
a pass (its write cache: `docs/multicore.md`).

- **The frame is a picture the GPU shows** (`host_picture`, `host_show_picture` in
  `linux/tdawn/host.c`): decoded once, into GPU memory, and drawn from there, stretched over
  the window if it is one of the small movies. No copy and no doubling by the game.
- **The worker cores decode it** when the machine has any (`--cores 4` in the harness;
  `linux/ralert/workers.c` is the game's jobs, functions a worker calls in the game's own
  memory, over the library every program has for them, `programs/mc/mcw.c`). A frame's block rows are shared between the
  cores, each writing its own rows of the picture, and the last worker unpacks the next
  frame's block pointers (LCW) while this one is drawn: the player loads a frame ahead for
  that. The movie's codebooks and pointers are allocated in whole 16 bytes with their pages
  already there (`workers_alloc`), so that no core stores beside another's and no worker
  stops at a new page; the picture is GPU memory, which a worker has as the game has it.
- A frame's palette starts on the 16 byte boundary after its pointers: a worker stores whole
  16 bytes to the pointers' very end. (The palette used to follow the pointers as they lay
  before they were moved to a boundary, and its first colours were pointers: a second of
  blue at the movie's start, on four cores only. `RALERT_MOVIE_CHECK` did not see it: it
  compares pictures, and both decodings were right.)
- The workers are the game's while a movie plays and no longer: it asks for them when a
  movie's buffers are made and gives them back when it ends, so that another program (Quake,
  whose server is on one) has them during a mission, where this game has no use for them.
- `RALERT_MOVIE_SOFT=1` is the game's own way, `RALERT_WORKERS=N` uses no more than N workers,
  and `RALERT_MOVIE_CHECK=1` decodes every shared frame on core 0 as well and compares: 0 of
  156 differ. Each movie prints a line (`ralert: movie ...`).

| The opening movie, 156 frames | Time | Core 0's instructions a frame |
|---|---|---|
| As the game does it | 41.4 s | 1,033k |
| The frame shown by the GPU | 30.5 s | 821k |
| and three workers decoding | 19.3 s | 443k |
| and one of them unpacking a frame ahead | 18.6 s | 234k |

From the command to the mission's first frame: 101.9 s, 87.8 s with the picture, 80 s with
the workers; the whole 300-frame check 123 s to 106 s, with the same state sum. What limits
a frame now is stores, not instructions: 16,000 texels of picture and 2,000 of pointers a
frame, 384 a pass a core. In the mission itself the workers do nothing yet, and their being
there costs it 4 to 8% (14.1 game frames a second where one core has 14.5).

## The mission's frame

Found with a profile of the mission alone (a snapshot at `ralert: begun`, resumed with
`--pc-log` and `--ra-log`; `tools/pc_profile.py`, `tools/pc_callers.py` for who calls a leaf
function, `tools/pc_hot_in.py` for where in a function):

- **A house whose teams can want nothing does not count its units for them.** Every house,
  twenty of them, counted its infantry, units and vessels against what its teams want, every
  frame: a thousand instructions a kind a house. What the team types that are built ahead
  ask for is found once a scenario (they do not change), and a house with none of those and
  no team in play skips the counting. 600 frames of the Soviet mission were 512M
  instructions from the command and are 493M: 30k a game frame, a tenth of it. The state
  sums are the same (`01e0dd60` and `ea72a5d1` at frame 300, `fdfd5447` for the Soviet
  mission at frame 600).

What the profile shows after that, of a frame of some 275k: the map's scene a quarter (the
cells' loop 10%, a rectangle's way into the list another 10%), shapes drawn into the atlas
the first time they are seen 7% (it falls as the atlas fills), the kernel's clock tick 7%,
looking for targets 5%, and nothing else above 2%.

## Speed

On D3D11 with fxc2 (3.5M to 4.1M instructions a second), no delay between frames, nobody
playing, in thousands of instructions a game frame:

| | Objects | Logic | Drawing | A frame | Game frames/s |
|---|---|---|---|---|---|
| Soviet mission, the game's own drawing | 64 | 235 to 300 | 390 to 520 | 660 to 810 | 5 to 6 |
| Soviet mission | 64 | 132 to 175 | 69 to 93 | 222 to 288 | 12 to 18 |
| Allied mission | 92 to 98 | 159 to 173 | 91 to 134 | 289 to 336 | 12 to 14 |

The map's scene is 66k to 106k of the drawing: the view is 20 x 16 cells, four times Tiberian
Dawn's. From the command to the menu is about a minute (190M instructions with the desktop's
own start), and as long again to a mission's first frame.

## Checking it

    TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=300 ralert      (wait for "ralert: done")

- `TDAWN_AUTO=1` presses Return through the menus; `RALERT_SIDE=allies` or `soviet` answers
  the question of sides; `RALERT_SEED`, `RALERT_FRAMES`, `RALERT_STOP_FRAME` (with
  `RALERT_NO_DELAY=1`), `RALERT_STATS_MS`, `RALERT_RENDER=soft` and `RALERT_REFRESH` are
  Tiberian Dawn's switches. `rastat:` lines say where a frame went.
- A change to the game must leave its `state` sum the same: `01e0dd60` for the Soviet mission
  (SCU01EA) and `ea72a5d1` for the Allied one (SCG02EA) at frame 300 with seed 7.
- The picture against the game's own drawing (`RALERT_RENDER=soft`, both held at one frame):
  5% of pixels differ, by more than 16 levels 0.1% to 0.9%. Those are shadows: a true blend
  where the game's table picks the palette's nearest dark colour, which on snow is 27 levels off.
- Sound: `--fixed-dt 0.004 --sound-capture` and `tools\sound_reference.py ... --rom`; a mission's
  first 300 frames, 83 seconds of sound, are the model's in every sample. `TDAWN_SOUND_LOG=1` says what
  the game asked for.
- Palette cycling goes by the clock (Tiberian Dawn's goes by frames in a run with no delay):
  a few dozen pixels of lamps differ between two runs of one build.

## Not done

- Playing it by hand: the pointer and keys are Tiberian Dawn's code, untried here.
- Skirmish, the Allied mission beyond 300 frames, saving and loading.
- The vortex in the GPU's scene; ground units sorted among walls as the game sorts them
  (the scene draws overlays first, then every object).
- Scroll steps sized by time, and frames left undrawn when late (`docs/tdawn.md` has both).
- Movies. The start: a minute to the menu.
