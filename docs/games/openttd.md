# OpenTTD

`openttd` is [OpenTTD](https://www.openttd.org/) 15.3, the open remake of Transport Tycoon
Deluxe, on the desktop, with the open base sets in place of the original game's files:
OpenGFX 8.0 (pictures, GPL-2.0), OpenSFX 1.0.3 (sounds, CC BY-SA 3.0) and OpenMSX 0.4.2 (music,
GPL-2.0). Nothing of the 1995 game is in the image. The engine is upstream's, compiled with
the patch below; the window, the sound, the music and the mixer are new (`linux/openttd/shaderemu_v.cpp`,
`shaderemu_s.cpp`, `shaderemu_m.cpp`, `mixer_shaderemu.cpp`). The picture is drawn by the GPU from lists the
processor makes ("The picture on the GPU"; `OPENTTD_DRAW=cpu` is the 8-bit blitter into a paletted texture that the
GPU shows as one quad); sounds are voices of the sound card, and the music is its FM voices.

    / # openttd                                         # the title screen
    / # openttd -g                                      # straight into a new game
    / # openttd -g /persistence/openttd/save/NAME.sav   # a saved game
    / # OPENTTD_RATE=1 openttd                          # every 10 s: pictures drawn, game ticks run
    / # OPENTTD_SCROLL=1 OPENTTD_RATE=1 openttd -g      # the map scrolled to and fro, 2 s each way
    / # OPENTTD_PLAY=2 openttd -g /usr/share/openttd/busy.sav   # the play benchmark ("Speed")

    wsl -- bash linux/nommu/build.sh openttd/build.sh   # after Nano-X's build; the first time builds GCC as well

then `root.sh` and `image.py` (`docs/system/nommu.md`, "Building it").

The Start menu has it under Games, the desktop has its icon (upstream's `media/openttd.128.png`,
`linux/apps/icons/openttd.png`), and a `.sav` file opens in it (`docs/system/open.md`). Its
settings and saved games are in `/persistence/openttd` (`docs/system/persist.md`), or in `/tmp`
when there is no such folder. `OPENTTD_HOST=1` makes the game `-g` starts or loads a server,
`OPENTTD_JOIN=1` opens the server list at once, and `OPENTTD_HOSTING=FILE` has `openttd` written
there once the server's game runs (`nxgame`, `docs/system/lan.md`).

## The compiler

OpenTTD is C++20 and wants the whole of libstdc++: exceptions, run-time types,
`std::filesystem`, `std::regex`, threads' types. `linux/openttd/toolchain.sh` builds GCC 13.3
for `riscv32-linux-musl` once (about eight minutes) over the toolchain's binutils and musl,
with libstdc++ and libatomic (RV32 has no 64-bit atomics). It is position-independent by default
and has emulated thread-local storage, since a flat program's loader sets up none. Its drivers
`ottd-cc` and `ottd-c++` link a flat program where the work folder is no-MMU Linux's, and put
`ehframe.c`'s two marks round `.eh_frame` with a constructor that registers it: without auxv
there is no `PT_GNU_EH_FRAME` for the unwinder to find. The binary is 31 MB, its data 21 MB (13 MB
of it OpenSFX).

It is compiled as one program with a played game's counts, as Tiberian Dawn's
(`docs/games/tdawn.md`, "The whole program at once", "What the compiler is told"):

- Every global hidden (`nommu_hidden.h` in the drivers, a load from the GOT less at every use).
- `-flto -flto-partition=1to1 -fno-toplevel-reorder` and larger inlining limits (`OPT` in `build.sh`; `OPENTTD_LTO=off` is
  file by file). What the link compiles reaches no launcher, so the 16 bytes a variable are given by an assembler of the
  game's own (`-B`, `ottd-as/as`, the objcopy after it), and `-fdata-sections` must be on the link's line too: without it
  the pieces have no variable a section, statics of the tick's and of the first core's lie in one texel, and `RVC_RACES`
  says undone (`_tw_tick_time`, `_tw_next`).
- `-fprofile-use` with `openttd.profile.gz`; `OPENTTD_PROFILE=off` is without, `=count` builds the counting program in a
  folder of its own. `linux/openttd/counts.py` says how the counts are made and carried out of the machine: played, with the
  drawing on two workers, since stores of many workers to one counter in a pass lose all but one, and a run of the
  first core's ticks, a new world and the windows. A function changed since has no counts: make them again after a large
  change to the source. A compiler without thread-local storage makes `__gcov_indirect_call` a plain global, and every
  file's destructor calls `__gcov_exit`: `gcov.c` is compiled `-D__thread= -DGCOV_ONCE`, or a run ends with 100 MB of counts
  on the console.

The tree in `~/nm/work/src/openttd` with the patch applied gives the patch back: `git diff 15.3`.
`build.sh` puts the tree back from the patch on every build (`OPENTTD_TREE=keep` leaves it as it stands).

## What the patch changes

`openttd.patch` is applied to the release by `build.sh`, which writes only the files whose text
changes, so a patched header does not rebuild everything. With `-DSHADEREMU=ON` the build defines
`WITH_SHADEREMU`, and the changes are under it but for the industry list's sort and seeks inside
`RandomAccessFile`'s buffer, which `host_sum.sh`'s build has too:

- The build takes the video and sound drivers and `mixer_shaderemu.cpp` in place of `mixer.cpp`,
  needs neither SDL nor Allegro, leaves out `extmidi` (it forks a player), and links Nano-X and
  `gles.o`. `StartNewThread` says no: what would be a thread's is done where it is asked for, or
  handed to a worker core by the port's own code.
- `null_v.cpp`: with `OPENTTD_SUM` set, a sum over every tile, the random state, the date and
  the companies' money, at the end of a `-v null:ticks=N` run.
- Defaults: 8-bit hardware (`support8bpp`), no zoom closer than normal (its sprites are 16 times
  the pixels), and a map of 64 by 64 for a new game (`MAP_SIZE_DEFAULT`).
- Starting, measured with a one-tick run (`-v null:ticks=1 -g`):
  - The base sets' MD5 sums are not checked (a whole pass over 20 MB).
  - A sprite drawn only at normal zoom, with no closer zoom shown, makes its zoomed-out levels
    from normal and the closest level is only its size; a font's glyph is encoded from its font
    zoom alone.
  - `RandomAccessFile` reads 8 KB at a time (each read from the ROM costs some 8,000
    instructions however small), seeks inside what it holds without reading, reads from an
    8-byte boundary (the kernel copies a misaligned read a byte at a time) and has stdio
    unbuffered under it.
  - Each base set's sprite offset table is read once, not at every load of the sprites.
  - The industry list is sorted on names made once, not made at every comparison.
  - A river's search marks tiles in a bit a tile, not a hash set (most of making a river).
  - The title screen's game is `make_title.sh`'s, 64 by 64 with two towns (13.7 KB).
- Without a game thread `GameLoopPause` draws a world being made, once a second: a picture is
  dear against the making.
- A viewport is drawn by worker cores (below), and with `OPENTTD_DRAW=cpu` a scroll as wide as the
  screen is one move by the machine's copy (CSRs `0xb0` to `0xb3`, `docs/machine/gpu.md`) in place of a
  `memmove` a row: what wraps round a row's end lands in the strips drawn anew ("Scrolls").
  `OPENTTD_MEMOP=0` moves it row by row, `=check` compares the two at every scroll.

## Music

`shaderemu_m.cpp` is a music driver ("shaderemu") that plays OpenMSX's MIDI files on the sound
card's FM voices, as Doom's music (`linux/nanox/doom_sound.c`): a song is turned into notes,
twenty voices of tracks, and the card plays it with no more work from the processor. Its
instruments are DMXOPL's General MIDI bank for OPL (`genmidi.op2`, MIT, fetched by `build.sh`),
double-voice instruments as two notes, the drums from channel 10. Program, volume,
expression, pan and all-notes-off are read; pitch bend is not. A song's start and end as the
music set gives them are kept, and the volume slider sets the voices' volume. OpenMSX is
unpacked into the base set folder: music sets are not looked for in tars.

A song is made on a worker core (`SongService` in `shaderemu_m.cpp`): the first core reads the
file and goes on, a free worker reads it from those bytes (`MidiFile::SeLoadData`, upstream's
reader of a file given the bytes where they lie), makes the notes and writes the voices' tracks,
and the first core starts the voices when it next looks (`IsSongPlaying`, which says the song
plays meanwhile). What the worker allocates comes from a heap of 4 MB beside the link graph's
(`SeWorkerHeap`, `SeWorkerArena` in `linkgraphjob.cpp`; OpenMSX's largest song takes 3.1 MB);
a song whose heap runs out, or with no workers, is made by the first core, and so is every song
with `OPENTTD_MUSIC=core`. `OPENTTD_MUSIC_SUM=1` prints each song's sum of its tracks, `=all` every
song of the set's at the first: the sums on the worker and with `OPENTTD_MUSIC=core` must be the same.

## The viewport on worker cores

While a `ViewportKeep` lives (round a frame's dirty blocks and viewport scrolls in
`UpdateWindows`, round `DrawDirtyBlocks` and a scroll's strips), each part `ViewportDoDraw` is
asked for is kept with the clip it had (`ViewportKept`). At the flush (`ViewportFlush`: the last
keep's end, a scroll whose rectangle holds a kept part (`ViewportFlushOver`), the mouse pointer
drawn, round a viewport drawn without transparency or shaded, and a part asked for over one
kept, which must be drawn after it, its texts too) the parts are cut in pieces and every core
finds, sorts and draws pieces (`mcw_for`, the dearest first):

- A piece costs (width + 4) x (height + 4) + 240, in pixels at normal zoom: the tiles round it
  that draw nothing in it are passed over by their reach ("A tile's reach"), and the 240 is its
  tiles passed and its sprites sorted (fitted to `OPENTTD_PIECE_LOG`'s pieces; `OPENTTD_CUT_M=4,4,240`
  changes the three). Each part gets its share of the cores by its cost and is cut in the columns
  and rows that make its pieces cheapest, a column at least 16 pixels and a row at least 8, so a
  strip a scroll exposed, 24 pixels by 458, is cut in rows. `OPENTTD_CUT_GREEDY=1` cuts the dearest
  piece again while that shortens the flush, and remembers the cuts of the last 16 flushes' sizes.
- A piece is drawn into a buffer of its own (copied from the screen first); then all cores copy
  the buffers to the screen by 16 rows each, so no two write the same 16 bytes in a pass. Under
  the GPU's blitter a piece is records instead ("The picture on the GPU").
- Each core has its own drawer (`_vd`), `_cur_dpi`, `_cur_ti`, colour remaps and NewGRF
  registers; `VpCore()` tells the cores by their stack (`gfx_func.h`).
- A worker may not allocate: its lists get room before a round (`_vp_room`), and one that fills
  gives its piece up and doubles. It may not load a sprite: the sprites it lacked are loaded by
  the first core with the rest of their eight (`VpLoadWanted`) and the piece is drawn again by
  the workers; given up twice, by the first core. It may resolve canals and rivers (OpenTTD's own
  water sprites are a NewGRF's: `WorkerSafe`), no other NewGRF chain. A vehicle's sprites made
  anew when drawn and its colour map, which it keeps, are made by the first core over the whole
  part before the round (`ViewportRevalidateVehicles`): two workers drawing one vehicle would both
  write its colour map.
- Signs, text effects, the cargo overlay and strings are the first core's, after the copy.
- `GetRawSprite` of a loaded sprite saves nothing and tests the stacks once, a parent sprite is not
  cleared before all its fields are set, and `ViewportAddLandscape` asks once a part whether any
  selection or highlight exists, not `DrawTileSelection` at each tile.

`OPENTTD_WORKERS=N` asks for N workers (47 by default, at most `VP_CORES` - 1; 0 draws as upstream):
more than the default cost the first core more a round than their pieces give back.
`RVC_RACES` over two rounds of the play script: 0 undone; the reads as they were are a core waiting on
another's word.

### Lines left behind

The blitter rounds a sprite's ends up to whole pixels, so a sprite whose box ends up to two
pixels before a part's top or left still draws its last row or column in it. Upstream's dirty
rectangles meet where they meet, but bands of rows cut every scrolled strip, and a row missing
where parts meet would line up across the screen as it scrolls. So `AddSortableSpriteToDraw`,
`AddCombinedSprite` and `ViewportAddVehicles` keep a sprite ending up to 2 x (`ScaleByZoom(1)` - 1)
before the part. A parent sprite outside the part can have children that draw in it: a parent is
kept within 128 pixels (`VP_REACH`) and dropped at the sort when it and its children miss the part.

### A tile's reach

A part is drawn by visiting every tile whose sprites could reach into it: upstream looks 250
pixels below the part for buildings and the highest bridge the settings allow, and as far to the
sides, so a strip of 24 pixels a scroll exposed would pass many more tiles than draw in it.
Each tile has its reach at each zoom, in three bytes (how far its sprites, what was culled of
them too, end above its north corner, to its left and to its right, in steps of 8), learned when
it is drawn (`VpReachLearn`, a worker's sent to the first core with its piece) and forgotten when
the tile is marked dirty (`MarkTileDirtyByTile`, all of them in `MarkWholeScreenDirty`). A tile
whose known reach ends below or beside the part, by 2 pixels at its zoom and a step more, is
passed over; while a tile highlight is shown, none below 64 pixels (its sprites are not the
tile's). The search below a part stops at the highest bridge on the map (`VpBridgeMost`), not
the highest one could build.

### One order

Upstream's sorter orders two sprites by the others of the rectangle drawn: it compares every two
whose boxes do not lie apart, takes them in the order the tiles were visited, and two whose boxes
lie apart on two axes the opposite way round (one behind on x, the other on y) are in no order of
their own. Under it a scrolled strip and the whole screen sort the same two sprites differently.
The sort (`ViewportSortParentSpritesKept`) is upstream's search with three changes: the sprites
are first put in an order of their own (by x + y of the box, then the rest of it and the sprite),
only two that meet on the screen (with their children, two pixels wider) are compared, and the
shortcut for a sprite with one before it is gone. Do not break upstream's "no order" by a key of each
sprite: that makes cycles, broken differently in each part. In the checks below every picture is the
same as one drawn whole; it is not proved for every scene: three sprites whose boxes order them round
in a circle can still come out by the part.

### Scrolls

Under the GPU's blitter a scroll is a record ("The picture on the GPU"), and no pixel is moved by the
processor. This section is `OPENTTD_DRAW=cpu`'s.

The machine's copy writes whole texels of the destination from words of the source
(`harness/apps/rvc_cpu.cpp`, `memop`), so a move by a number of bytes not a multiple of 4 would come
out shifted, a pixel in four. A move is row by row unless it is whole words, and the copy is of the
destination's whole texels, the ends by the processor. `OPENTTD_PLAY_ODD=1` makes the play script's
way back a pixel longer a step, an odd move.

A scroll costs what it exposes:

- The move of the screen is the workers', and not waited for: the machine copies the rows aside,
  the workers copy them back moved (`MoveOdd`), and the first core goes on to find what the
  scroll exposed. `ScrollSettle()` waits for it before anything reads or writes the screen: the
  pieces' round of a flush, `GfxScroll`, the mouse pointer undrawn, a move as a block, another
  move, the end of `UpdateWindows`' keep.
- A view narrower than the screen (a vehicle's window following it) has its rows copied aside and
  moved back by the workers, 4 rows a job (`MoveRows`).
- A kept part wholly inside the scrolled rectangle, before and after the move, moves with it
  (`ViewportScrollKept`); any other kept part flushes first.

### Checking a picture

`OPENTTD_CHECK=1`: at every picture of the play script the screen is drawn again whole and the
two compared (a picture with nothing drawn too); `openttd: check:` says how many pixels differ,
where, and the longest line of them in a row or a column; `=keep` shows the picture as drawn, its
errors kept, `=list` names the first differing pixels with both colours. `OPENTTD_HOLD=N|bad|big`
stops at picture N of the play script, at the first that differs or the first by over 1,000
pixels, and says where both pictures are in memory (`rvc_cpu --save-ram`). The round's line says
how many differ; the program's first picture, before the play script, is no fault of it.
`OPENTTD_PAUSE=1` pauses the game, which draws the screen again for the status bar's "paused".

`tools/test_all.py openttd-scroll` (with workers and the odd way back, 198 pictures checked on
31 October 2026) and `openttd-scroll-serial` (the first core alone, 199) run it and want 0 pixels.

A part asked for over a kept one of another view, or of the same view moved since, flushes first
(`ViewportKept`), as upstream draws one after the other: the pieces of both would be copied out, then
the texts of both, and the old rectangles' station names would come last. Over one of the same view,
only what the kept one does not cover is kept (`OPENTTD_KEEP_FLUSH=1`: always flushed). A vehicle's
window over the main view must not take that way, or the window's view is lost where the two meet.

## Windows

An open window cost the game its tick: the tick waits while the first core works on a picture (`TwHold`), and every
instruction of a window's paint is such work. The same view standing still (`OPENTTD_PLAY_PART=window`,
`OPENTTD_PLAY_SCENE=none|one|many|vehicle`, `OPENTTD_PAUSE=1` for a paused one; `OPENTTD_PLAY_WHO=1` prints the paints by
window class), calibrated, 31 October 2026 (`rvc_cpu --pass-model`), means of rounds 2 to 6 at the 66 ms pace, so pictures a second say little
here and game ticks a second say what is left:

| Scene | Before: ticks/s, held, core 0 a picture, lists a picture | After |
|---|---|---|
| no window | 6.15, 31%, 144k, 1.61 | 6.77, 26%, 126k, 1.55 |
| the finances | 3.16, 60%, 223k, 0.97 | 6.29, 31%, 141k, 1.47 |
| finances, station list, a station, towns | 2.29, 71%, 268k, 0.84 | 5.48, 33%, 149k, 1.16 |
| a train's window following it | 5.12, 41%, 177k, 2.03 | 6.18, 33%, 157k, 1.95 |
| the same, paused | no ticks, 7%, 49k, 0.36 | no ticks, 7%, 46k, 0.37 |

"Held" is the share of the time the tick waits for the first core: a tick's own length goes by the game (106 to 500 ms a tick
in one run's rounds), so that share says more than ticks a second. Two turns of tree and change gave the same numbers: a run
of the same image and switches is the same run. Lists rise after because more pictures draw: the view under the windows is drawn
more often now that the tick moves its vehicles more often.

What it was: none of the suspects of area. A window adds few lists (a list is 0.57 ms, and the fewer main-view pieces under a
window pay for them) and a view cut round it costs little. It was the strings: formatting and laying out a text is thousands of
instructions, 120 thousand a paint of the finances, whose money changes at every tick of any vehicle.

- A block of the dirty grid says who marked it: `DIRTY_VIEW` a viewport (`AddDirtyView`, from `MarkViewportDirty`), else
  anything. A rectangle of `DIRTY_VIEW` blocks draws only the windows with a viewport, and of those the viewport's widget
  alone, no `OnPaint` and no other widget (`view_alone` in `DrawOverlappedWindow`): a vehicle passing beside a window, or by the
  64 pixels of a block under its edge, painted all of the window's texts in that rectangle, and a window's picture stays as it
  is under the view's change, on the GPU and in the 8-bit picture. Rectangles are made of blocks of one kind.
- What the tick says of a window's widgets (its deferred tasks, `TwDrain`, and the company's finances, `_se_tick_marks`) goes
  to a second grid (`AddDirtyLater`), joined to the dirty blocks at most every `OPENTTD_WINDOW_MS` (1,000; 0 at once), and at 16
  times what the last such round took (`SeDirtyLaterDrawn`: windows dear to draw take some 6% of the time at most). With it at
  4,000 the finances are as no window. A window that comes, goes or changes its size is drawn at once (`Window::Close`,
  `ReInit`, `FindWindowPlacementAndResize`), and all that a click or a key marks; `CheckPicture` joins them first.
- A vehicle's view (`NWidgetDisplayFlag::NoTransparency`) flushed the kept parts twice at each paint for a transparency that is the same
  unless something transparent is set: not any more then.
- A number's digits are made in 32 bits once they fit (`FormatNumber`: twenty 64-bit divisions and remainders were most of it).
- `DrawDirtyBlocks` looks only at the columns and rows of `_invalid_rect`, not the whole grid at every picture.

A following window's cost that is left is its view: a train's boxes in blocks of 64 by 8, old and new, are most of a small
view at each tick (the workers ran 2.5 times the instructions of no window; the flush 44 ms against 37, and the tick goes on
in it). Tried: blocks 16 pixels wide, a quarter fewer pixels with the vehicle's window (flush 41 ms to 35), but more rectangles and
parts, and zoomed out the first core 601k to 694k a picture: not kept. `OPENTTD_WINDOW_MS` 500 instead of 1,000 without the
16 times: the finances 5.0 game ticks a second against no window's 6.3.

`tools/test_all.py openttd-windows` (`OPENTTD_PLAY_MOVE=1`: moved at picture 20 of the part, closed at 40) checks every picture against
one drawn whole. Paused captures against the tree on the software device are the same to the pixel with four windows.

## The tick in slices

`OPENTTD_TICK=slices` runs the tick on the first core, on a stack of its own (`ottd_tick_swap`,
`shaderemu_v.cpp`): it gives way at `TickYieldPoint()` once its slice of a picture is spent, in
the vehicles' loop, between a train's two handler calls and its steps, at every step of YAPF's
search, in the loading of a station's vehicles and in the landscape's tile loop. The slice is
what the last pictures left of 100 ms (`OPENTTD_FRAME_MS`), at least 10 ms (`OPENTTD_TICK_LEAST`),
less how far slices ran past theirs, all in the machine's 5 kHz time (`VpTime`; the clock's is
too coarse). The drawing between two slices changes no game state; the company is the drawing's
while the tick is out, and a command or a save first finishes the tick (a world being made does
not give way). `OPENTTD_TICK=whole` is upstream's, every tick at once. Slices are the way when no
worker is free for the tick; by default the tick is on a worker.

## The tick on a worker

The tick runs on a worker core of its own (`shaderemu_t.cpp`), on a stack and a heap of its own
(4 MB, `OPENTTD_TICK_HEAP`; more comes from the first core) while the first core draws. It waits where
the first core holds it (`TwHold`): while the first core reads the input, runs commands and the
windows' timers, and finds the viewports' parts; it runs while the pieces are drawn and between two
pictures. Whatever of the window system it would touch is left for the first core (`TickDeferTask`),
and what the viewports draw of a vehicle is a copy (`Vehicle::drawn`) made at the first core's sync for
every vehicle the tick said it changed (`VehicleChanged`, from `UpdateViewport` and
`MarkAllViewportsDirty`). A delete the drawing could be reading waits until no worker draws
(`TickExclusive`). `OPENTTD_TRAPS=1` lists at the end where the tick reached what only the first core
may use.

- Its pictures overlap with the tick: a picture's last flush is left out (`VpFlushOut`) and the
  next picture's input, commands and timers run while the workers draw its pieces; the pieces
  draw the vehicles from a copy of their own made at the flush (`VehiclesKeptBegin`), the reach
  of a tile forgotten meanwhile is forgotten after it, the tick's frees wait until no piece is
  out (`TwAfterFlush`), and anything that reads or writes the screen finishes the flush first
  (`VpFlushFinish`: a command, the pointer, a sprite cache's deletes, a check). `OPENTTD_PIPE=0`
  waits for the pieces at the flush.
- While it scrolls, the ticks are spaced out to keep the picture's work at 90 ms
  (`OPENTTD_PACE_MS`): every tick costs the first core its sync, and the windows its texts and the
  status bar change. While pictures are late the space starts at half a second and grows by a quarter a
  picture, to a tick a second at most (`OPENTTD_PACE`, 0 never), and is gone eight pictures after the
  view stops (`OPENTTD_PACE_LOG=1` prints it; `OPENTTD_TICK_MS=N` is no more than a tick every N ms).
  The ramp starts high and steep since a slower one fills up with ticks while zoomed out, every tick's
  moves being parts drawn again.
- A loaded game has none of its water regions: they are all made at the start, one a job on the workers
  (`WaterRegionsMakeAll`; the labelling into `WaterRegionMade`, kept by the first core).

The game is the same: `OPENTTD_SUM_AT=36` on the scroll gives `d4e04e63`, 846 vehicles,
`26aae0d5` on the worker, whole and sliced (`OPENTTD_TICK_LEAST=300` for slices that end ticks).
`OPENTTD_CHECK=1` is 0 pixels paused; with the game running a sprite of a ship (z 56, an aqueduct's
height) can differ from the picture drawn whole, with the worker's flush and without it
(`OPENTTD_PIPE=0`), and every vehicle's copy is as it is: three boxes in a circle ("One order").

## The picture on the GPU

The blitter is `8bpp-gpu` (`linux/openttd/shaderemu_gpu.cpp`): it draws no pixel. A sprite's zoom levels
stay in the sprite cache as plain rows of palette indices, where the GPU reads them; a sprite, a fill, a
line's pixels (a run of `SetPixel`s on one row joined), a remapped rectangle and a scroll become records of
the GPU's sprite command (`CMD_SPRITES`, `docs/machine/gpu.md`, "Sprites of a paletted picture"), a texel
each, in the order the game draws them. The GPU draws a list's records the later nearer into its target and
copies it into a picture of words in GPU memory (red the index); the next list starts with that picture as
its first record (kind 7), and a second list shows it through the palette in the window. The palette and its
animation are the show list's 256 words: a palette change draws nothing again.

- Company colours and recolours are a record's remap table (kind 1); a table a worker may change again is
  copied beside its records first.
- The transparent buildings and a shaded window remap what is already there: they are drawn in the list's
  second, blended pass and add to green, 1 for the first table and 16 for the second (words 7 and 8 of the
  command). Whoever reads the picture next makes the remaps, the next list's first record or the CPU's copy.
  A third table ends the list.
- A scroll is the picture's own record shifted, after the list so far is drawn: no pixel is moved by the
  processor.
- The viewport's pieces are recorded by the worker cores into records of their own, which the first core puts
  in the list after the round in the pieces' order ("One order"): a piece that runs out of room, or meets a
  remap the first core has not seen, is given up and drawn again by the first core.
- The pointer's sprites go to an overlay that only the show list draws.

A list is submitted at the frame's end, and the game's tick runs while the GPU draws it: the processor waits
only at the next frame's first drawing, if the list is not drawn by then (its records' chunks are used again).

`OPENTTD_CHECK=1` reads the GPU's picture back for each comparison. `OPENTTD_HOLD_WHO=x,y` with a hold prints
every list's records that cover that pixel. `OPENTTD_PLAY_JUMP=1` turns smooth scrolling off: a smooth
scroll's step goes by the time between pictures, so two runs' pictures are compared only with it. The
CPU's blitter moves a scroll's whole rows (`ScrollAsBlock`), including the few pixels right of the
rectangle that upstream leaves as they were: compare with `OPENTTD_DRAW=cpu OPENTTD_MEMOP=0`. Under
`rvc_harness --cpu` the GPU reads RAM that the processor wrote outside the GPU's memory only every 0.25
s: run with `--cpu-fresh`.

The check: `OPENTTD_PLAY_SHORT=1 OPENTTD_PLAY_ODD=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1` under
`rvc_harness --cpu --warp --cpu-fresh`, 0 differ; a window drawn again by the wall's time can come a
picture early under WARP against the 8-bit blitter's sums. `tools/test_all.py openttd-gpu --races`: 0 undone.
Calibrated (31 October 2026, the play benchmark, alternating), the two blitters are within the model's 7%.

## Is it the same game

`host_sum.sh` builds the same tree for the build computer, without a window and without
`WITH_SHADEREMU`, and runs `test.cfg` (64 by 64, 1950, no competitors). The machine must give
its sums (`tools/test_all.py`, case `openttd`):

| Run | Date | Sum |
|---|---|---|
| seed 1, 500 ticks | 712229 | `9d6fc0a0` |
| seed 2, 2,000 ticks | 712250 | `66880c87` |

## Speed

Play is measured on upstream's title game (`busy.sav`: towns, trains, ships and canals) with
`OPENTTD_PLAY=N`: a script of 200 pictures run N times, the same views at any speed, the game
running: 60 scrolling, 50 zoomed out twice and scrolling, 60 with three trains' and a station's
windows open and the view following a train, 29 after they close. Each part prints its pictures
a second, and what those were spent on; the first round warms the sprite cache, the second is the
one read. The autosave is off under it (one is minutes of LZMA at 2 MIPS, in whichever part it falls).
`OPENTTD_PLAY_PART=scroll` is the script's 60 scrolling pictures alone, `follow` its 60 with the
windows and the view following a train; `OPENTTD_PLAY_SHORT=1` is 30 of the scroll's pictures, 20
zoomed out and 20 with the windows, for one round on the card, with the sprite cache cold;
`OPENTTD_PAUSE=1` pauses the game.

Calibrated, 31 October 2026 (`rvc_cpu --pass-model` with `tools/card_model.json`, `RVC_SOUND`, `RVC_WCACHE`, `RVC_ADFAULT`,
two runs alike, `OPENTTD_PLAY=2`, the second round; another run of the same command gave 11.6 a round). The model is good to
about 7% and rests on one program for its workers' term, so it says most of the shares, and the card's figures below are
the speed:

| Part | Pictures | Seconds | Pictures a second | Game ticks |
|---|---|---|---|---|
| scroll | 60 | 3.99 | 15.0 | 25 |
| zoomed out | 50 | 8.02 | 6.2 | 22 |
| windows | 60 | 3.96 | 15.2 | 15 |
| windows closed | 29 | 1.92 | 15.1 | 10 |
| the round | 200 | 17.90 | 11.2 | 72 |

The scroll and the windows stand at the picture's interval of 66 ms (a part of 60 in 3.96 s): a part's own cost shows as
the zoomed out's, whose first core is 466k instructions a picture, a scroll's 185k. The paused scroll is the same 15.3 a second.

Calibrated, the same day: the title screen 29 pictures and all its 37 game ticks a second; a new game of 64 by 64 (`openttd -g`)
about 40 s to make, then 15 pictures and all 37 ticks a second.

### Speed on 31 October

The picture's interval is 66 ms (`OPENTTD_FRAME_MS`): a picture that does not wait for it lets the pace space the ticks less.

On the card, RTX 5090 (`rvc_harness --d3d11 --unity 11.1,1.4,0.75`, `OPENTTD_PLAY=2`, two runs):

| | |
|---|---|
| round 2, pictures a second | 10.29, 11.21 |
| round 2, game ticks a second | 2.57, 2.86 |
| round 1, pictures a second | 8.30, 8.52 |
| the run less its two rounds (boot and start), s | 25.2, 25.2 |
| the whole run, s | 68.7, 66.6 |

Under a VR frame the card's game ticks are about two thirds of the calibrated run's 4.0 a second above.

## The start

From the program's start to a game loaded with `-g` (`busy.sav`) is 35 s, calibrated, 31 October 2026 (`/proc/uptime` round a
run, less its round).

- The title's own game (`opntitle.dat`) is not loaded before a game asked for with `-g`
  (`LoadIntroGame`).
- A load's LZMA is decoded ahead on a worker into a ring of 16 pieces of 64 KB that the first
  core copies from while it reads what it took (`SeLzmaAhead` in `saveload.cpp`). The decoder's
  memory is made by the first core when it decodes the first piece. The worker allocates only
  from a reserve of 64 KB, and the blocks it lets go are freed by the first core.
- The link graph's jobs a load starts are not run at once: each goes to a worker of its own while
  one is free ("The link graph on the workers"), and with no workers one runs each tick; each is
  finished before it is first asked about (`SeRunPending`).
- The missing-glyph check reads every string of the language. It is skipped for the image's English,
  whose base font has every glyph.
- The music's fine tune is a `pow` once a tone, not once a note. A song's notes are put in
  buckets by voice, not looked through once a voice (`shaderemu_m.cpp`). The song is made on a
  worker ("Music").
- The empty world before a load asked for with `-g`, its sprites and windows, is not made: the load makes them anew
  (`OPENTTD_EMPTY_FIRST=1` makes it).
- A savegame's arrays of bytes come by `std::copy_n` from the read buffer, of 16-bit words by a loop of their own.
- A field whose type in the file is its type in memory is read by one switch (`SlSaveLoadConv`). A game that names its size has
  a first empty map of 64 x 64 (its first chunk makes the map anew), and the map's megabytes come from `calloc` with what the
  kernel cleared not cleared again.
- The unwinder's tables are filed at the first throw (`__wrap__Unwind_Find_FDE` in `shaderemu_t.cpp`): libgcc reads every FDE to
  file its range when a program registers them, and a played game throws nothing.

## The first core's small blocks

Up to 1 KB the first core's `malloc` and `free` are lists by size, in steps of 16, in 8 MB of its own (`shaderemu_t.cpp`, `FhNew`;
a played `busy.sav` takes 2.6 MB; when they are out, musl's; `OPENTTD_MALLOC=musl` is musl's always). `free` of the first core's
block on the tick's worker goes by the ring.

A machine without paging runs out of memory in one piece, not of memory: the sprites' arenas are 256 KB for the cores there are,
not cleared; the tick's heap is 4 MB (a played run takes 211 KB; more comes from the first core).

## Sprites loaded

A sprite's levels closer than the normal zoom are shown nowhere in the image's settings. Their
pixels are not decoded; only their sizes are recorded, so that `ResizeSprites` pads the normal
level exactly as whole (`SeCloseLevelsUnseen` in `grf.cpp`). `OPENTTD_SPRITES=whole` decodes them.

## The game's own speed

The simulation is measured by `OPENTTD_WARM=100 OPENTTD_SUM=1 openttd -v null:ticks=300 -g
/usr/share/openttd/busy.sav` on `rvc_cpu`. It reports the game loop's ticks per second over
ticks 100 to 300 (the first warm the ships' water regions) and the state's sum. The sum must
stay `6abca409`, and `503e40ee` with 1,300 ticks, as upstream's. Calibrated, 31 October 2026:
8.04 ticks a second.

- The frame rate window's timer reads the machine's clock word, not `clock_gettime`.
- `UpdateBoundingBoxCoordinates` keeps the bounds of a sequence of one sprite while the sprite
  and the sprite cache's generation are the same.
- While the vehicles tick, `Vehicle::UpdateViewport` only notes the vehicle. Its box, its place
  in the viewport hash and the dirty blocks are made once, after the last vehicle has ticked
  (`VpDeferBegin`, `VpDeferEnd` in `vehicle.cpp`). The blocks marked are those of the old box and the
  last one, the only ones a picture can have shown. A vehicle deleted, or a sound placed at a
  vehicle, makes its update first. `OPENTTD_VP_NOW=1` updates at once.
- `GetAcceleration` divides by the processor's 32-bit division when both sides fit (`SeDiv64`).
  The station and industry ticks take the 64-bit tick counter's remainder once, not once a station.
- A vehicle's tick has no `PerformanceAccumulator`: the machine's clock is in milliseconds. The frame
  rate window shows no vehicle times.
- While the vehicles' places are marked (`VpDeferEnd`), the viewports are listed once
  (`ViewportsFixed`), not by walking every window for each vehicle.
- A text effect changed where no viewport can show it (a loading vehicle's percent) keeps its new
  text and makes its string and width only when a viewport can show it (`RefreshStaleTextEffects`,
  before the dirty blocks are drawn). An income added where no viewport can show it is made the same
  way (`AddTextEffect`).
- `AddDirtyBlock` stores the bytes of up to four blocks a column itself. A vehicle's new sprite
  sequence is not zeroed before `GetImage` sets it (`SpecializedVehicle::UpdateViewport`): only the
  sprites it counts are read.
- In `VpDeferEnd` the boxes of the vehicles whose sprite's bounds are kept are made on the workers
  (`VpBoxes`, `Vehicle::UpdateViewportBox`), with whether any viewport shows the area that would be
  marked (`ViewportsFixedSee`, the tests of `MarkViewportDirty`). Each stores to its own vehicle.
  The first core then puts each vehicle in the viewport hash and marks its blocks in order, and skips
  the marking of a vehicle no viewport shows.

The vehicles tick in one order on one core. Their random numbers come from one state in that order, so
they cannot tick on workers and keep the same game.

## The link graph on the workers

Upstream runs each link graph job (cargo distribution: demands, the multi-commodity flow, the flows
mapped) in a thread of its own, on a copy of the graph made when it is spawned, and joins it at a
date set then. Here a job is handed to one of four slots (`SeHand` in `linkgraphjob.cpp`,
`OPENTTD_LG=N` slots, 0 runs each where upstream does without threads), the first core gives the slot a
free worker or says it has none (`SeLinkGraphService`, from `TwService`; never while a flush's pieces are
out), and the job is finished where it is first asked about, as upstream joins its thread. A job on a worker
allocates from its slot's heap (1 MB, `OPENTTD_LG_HEAP`) through the program's `operator new`, its blocks of
up to 1 KB used again by size; what it leaves, its nodes' annotations with their flows, is copied out of
that heap by the core that finishes it (`SeFinish`), each tick for a job whose worker has ended (`SeReap`),
so the slot takes the next job. A job with no worker is handed again the next tick, and one whose heap runs
out is run where it is asked about. The jobs a load leaves are handed from the game's first tick on
(`SeRunOnePending`), not at the load: a paused game's pictures pay nothing for them. The game is the same:
`6abca409` after 300 ticks of busy.sav, `503e40ee` after 1,300.

## Ships' paths made ahead

A ship asks the pathfinder (YAPF) when it enters a tile with its cached path empty, and the expensive
calls are those that make water regions anew. After the vehicles have ticked, each ship that will ask at
its next tile has that path made ahead on a worker (`YapfShipAhead` in `yapf_ship.cpp`). The worker starts
from a copy of the ship and of the pathfinder's settings. The ships counted on each docking tile come from
a snapshot. The station's docking tiles and the region patches the region search starts from are found by
the first core. The worker reads no pool.

- **The map is still while the worker reads.** The worker reads the map only while it does not change
  (below), and whoever ends the stillness waits until every worker has stopped at its next node.
- **Every tile read is logged.** Each read of a tile again must find it as logged, or the
  worker gives up. A tile the search stands on is logged by its words and its neighbours'
  heights. A tile only entered is logged by its water tracks. A docking tile's neighbours and
  a bridge's walk are logged by their types.
- **An invalid water region is made in the worker's own memory**, logged by its tiles' water
  tracks. A valid region is noted with its generation.
- **The worker allocates from a heap of its own.** A worker may not allocate. The program's
  `operator new` gives a worker making a path a piece of a heap the first core made, which
  is let go all at once.

When the ship asks, the path is taken only if all of these are as they were: the ship's
inputs, the settings, each region's generation, every tile logged, and the ships on every
docking tile counted. Otherwise, and for a lost ship's random path, the first core makes the
path itself. A load or a new map first stops every worker (`YapfShipAheadCancel`).
A picture drawn with no tick since the last one, or while the game is paused, gives the paths up
first (`YapfShipAheadQuiet`), or the jobs of the last tick would hold workers through every paused picture.

With the tick on a worker of its own the map is still only while the tick waits, between two ticks
(`YapfShipAheadGo` in `TwMain`), or mid-tick for a path made ahead that it is about to take. The tick
asks for the paths at the end of its vehicles (`in.req`), the first core posts each to a free worker
or says it has none (`YapfShipAheadService`, from `TwService`), and before the tick goes on, and
when the first core holds it (it may run a command), the stillness ends (`YapfShipAheadStop`). A job
reads only once its word that it reads was stored a pass before and the map is still still
(`SeShipAheadPoll`); whoever ends the stillness waits for every job whose word says so. The checks
of the tiles are the tick's alone there (mcw's calls are the first core's), and so is the making of
water regions made invalid (`WaterRegionsMakeAll`). The game is the same: the played script's sum at
tick 40 (`OPENTTD_SUM_AT=40`, `cfbecedf`, vehicles `fb8c297e`) with and without the paths ahead and
the link graph's workers, and `OPENTTD_SHIP_AHEAD=check` says 0 differ.
With no job out the stillness ends without a pass (a job posted later starts in a later pass and
sees the word), and in a paused game the tick gives every path out up (`YapfShipAheadDrop`): a
paused game takes none, and a job out would make every hold of the tick wait a pass or two for it.

The water regions made invalid are made at the end of each tick's vehicles, one region a job on the
workers (`WaterRegionsMakeAll` in `water_regions.cpp`, from main's 5a3b851: the labeling `Make` into a
`WaterRegionMade` of its own, `Keep` on the first core). A ship's job that still makes a region of its own
waits at every sixteenth tile of it (`SeShipAheadPoll`).

`OPENTTD_SHIP_AHEAD=check` makes every path taken again on the first core and compares the two:
0 differ; the `ship paths ahead` line at the end says how many were made, taken, invalid and why.
`OPENTTD_SHIP_AHEAD=0` is without. The check of the tiles logged is cut in parts of at
least 128 tiles, each part on a free worker and the first on the first core (`AheadValidTiles`): the
map is still while the first core waits there, and the reason given is the first part's that has one,
as in one pass. A slot's heap gives the same places every job, so a job clears its indexes only where
the last job logged (`clean_size`). Ships are listed again every 64 ticks, not walked in the whole
vehicle pool each tick.

## At exit

The program gives its workers back itself at exit (`VpWorkersAtExit` in `viewport.cpp`,
registered with `atexit` at the first `mcw_open`): the ships' paths made ahead are cancelled
and joined, a lingering team is ended, and `mcw_close` runs while the memory is still the
program's: a worker left running writes into pages the next program gets. A ship's path waiting
for the drawing stores its answer only when it changes. The GPU's memory is handed out from the same
place to every program (`seglMemory`), so the screen's picture is cleared at start. `test_all.py
openttd-twice` plays busy.sav, then plays it again paused with every picture checked (0 pixels, the
workers drawing), then starts it a third time for the state's sum after a tick (`e49e18e3`).

## Not done

- Pitch bend and other controllers in the music.
- The tick is one worker's ("The tick on a worker"): its vehicles, paths, tile loop, stations and
  towns are not split over more workers (but for ships' paths made ahead and the link graph's jobs), and the first core's sync of the vehicles it moved bounds how many ticks a scrolling
  picture can take. Signs, texts and windows' widgets are the first core's.
- A NewGRF chain other than canals' is resolved by the first core: only canals' resolver was
  read through for stores outside a core's own registers (`WorkerSafe` in `newgrf_spritegroup.h`).
- One order is not proved for every scene: sprites whose boxes order them in a circle ("One order").
- Online content and screenshots in PNG (no libcurl, no libpng) are not built.
