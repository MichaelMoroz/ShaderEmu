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

    wsl -- bash /mnt/c/.../linux/openttd/build.sh   # after Nano-X's build; the first time builds GCC as well

For Linux without an MMU, which the world runs:
`bash linux/nommu/build.sh openttd/build.sh`, then `root.sh` and `image.py`
(`docs/system/nommu.md`, "Building it").

The Start menu has it under Games, the desktop has its icon (upstream's `media/openttd.128.png`,
`linux/apps/icons/openttd.png`), and a `.sav` file opens in it (`docs/system/open.md`). Its
settings and saved games are in `/persistence/openttd` (`docs/system/persist.md`), or in `/tmp`
when there is no such folder.

## The compiler

OpenTTD is C++20 and wants the whole of libstdc++: exceptions, run-time types,
`std::filesystem`, `std::regex`, threads' types. `linux/openttd/toolchain.sh` builds GCC 13.3
for `riscv32-linux-musl` once (about eight minutes) over the toolchain's binutils and musl,
with libstdc++ and libatomic (RV32 has no 64-bit atomics). It is position-independent by default
and has emulated thread-local storage, since a flat program's loader sets up none. Its drivers
`ottd-cc` and `ottd-c++` link a flat program where the work folder is no-MMU Linux's, a static
one otherwise, and put `ehframe.c`'s two marks round `.eh_frame` with a constructor that
registers it: without auxv there is no `PT_GNU_EH_FRAME` for the unwinder to find. The
binary is 14.8 MB, its data 21 MB (13 MB of it OpenSFX).

## What the patch changes

`openttd.patch` is applied to the release by `build.sh`, which writes only the files whose text
changes (a patched header otherwise rebuilt everything: 210 s against 10). With
`-DSHADEREMU=ON` the build defines `WITH_SHADEREMU`, and the changes are under it but for the
industry list's sort and seeks inside `RandomAccessFile`'s buffer, which `host_sum.sh`'s build has
too:

- The build takes the video and sound drivers and `mixer_shaderemu.cpp` in place of `mixer.cpp`,
  needs neither SDL nor Allegro, leaves out `extmidi` (it forks a player), and links Nano-X and
  `gles.o`. `StartNewThread` says no: the game runs on the first core alone.
- `null_v.cpp`: with `OPENTTD_SUM` set, a sum over every tile, the random state, the date and
  the companies' money, at the end of a `-v null:ticks=N` run.
- Defaults: the 8-bit blitter (`support8bpp` hardware), no zoom closer than normal (its sprites
  are 16 times the pixels), and a map of 64 by 64 for a new game (256 takes some sixteen minutes
  to make at 2 MIPS).
- Starting, measured with a one-tick run (`-v null:ticks=1 -g`) from 1.39 billion instructions
  to 350 million:
  - The base sets' MD5 sums are not checked (a whole pass over 20 MB).
  - A sprite drawn only at normal zoom, with no closer zoom shown, makes its zoomed-out levels
    from normal and the closest level is only its size; a font's glyph is encoded from its font
    zoom alone. Making the 4x level of every sprite was a third of a start.
  - `RandomAccessFile` reads 8 KB at a time (each read from the ROM costs some 8,000
    instructions however small), seeks inside what it holds without reading, reads from an
    8-byte boundary (the kernel copies a misaligned read a byte at a time: 100 million of a
    start) and has stdio unbuffered under it.
  - Each base set's sprite offset table is read once, not at every load of the sprites.
  - The industry list is sorted on names made once, not made at every comparison.
  - A river's search marks tiles in a bit a tile, not a hash set (most of making a river).
  - The title screen's game is `make_title.sh`'s, 64 by 64 with two towns (13.7 KB):
    upstream's took 226 million instructions to load.
- Without a game thread `GameLoopPause` draws a world being made, once a second: a picture is
  some 400,000 instructions, and four a second were a third of the making.

- A viewport is drawn by worker cores (below), and a scroll as wide as the screen is
  one move by the machine's copy (CSRs `0xb0` to `0xb3`, `docs/machine/gpu.md`) in place of a
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
a song whose heap runs out, or with no workers, is made by the first core as before, and so is
every song with `OPENTTD_MUSIC=core`. A song was 26 million instructions of the first core's start
to the game. `OPENTTD_MUSIC_SUM=1` prints each song's sum of its tracks, `=all` every song of the
set's at the first: the 31 songs of OpenMSX give the same sums on the worker and with
`OPENTTD_MUSIC=core`.

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
  tiles passed and its sprites sorted (fitted to `OPENTTD_PIECE_LOG`'s pieces, spent about 12k
  instructions + 50 a pixel; `OPENTTD_CUT_M=4,4,240` changes the three). The dearest piece of the
  flush is cut again, in columns or in rows, while the flush gets no longer (a tie is cut), the
  flush being its dearest piece or all the work over the cores, and never into more pieces than
  workers (a second wave cost as much as the first). A strip a scroll exposed, 24 pixels by 458,
  is so cut in rows. Parts of the sizes of one of the last 16 flushes' are cut as they were.
- A piece is drawn into a buffer of its own (copied from the screen first); then all cores copy
  the buffers to the screen by 16 rows each, so no two write the same 16 bytes in a pass. Before,
  pieces of two parts that shared 16 bytes of the screen were one core's, and a chain of them
  (parts cut round a window) was a third of a flush.
- Each core has its own drawer (`_vd`), `_cur_dpi`, `_cur_ti`, colour remaps and NewGRF
  registers; `VpCore()` tells the cores by their stack (`gfx_func.h`).
- A worker may not allocate: its lists get room before a round (`_vp_room`), and one that fills
  gives its piece up and doubles. It may not load a sprite: the sprites it lacked are loaded by
  the first core with the rest of their eight (`VpLoadWanted`) and the piece is drawn again by
  the workers; given up twice, by the first core. It may resolve canals and rivers (OpenTTD's own
  water sprites are a NewGRF's: `WorkerSafe`), no other NewGRF chain. A vehicle's sprites made
  anew when drawn and its colour map, which it keeps, are made by the first core over the whole
  part before the round (`ViewportRevalidateVehicles`).
- Signs, text effects, the cargo overlay and strings are the first core's, after the copy.

`OPENTTD_WORKERS=N` asks for N workers (31 by default, `VP_CORES` - 1; 0 draws as upstream).
`RVC_RACES` over two rounds of the play script (below): 0 undone; the reads as they were are the
team's round word. Before the colour maps were made first, two workers drawing one vehicle
both wrote its colour map (29 undone).

### Lines left behind

The blitter rounds a sprite's ends up to whole pixels, so a sprite whose box ends up to two
pixels before a part's top or left still draws its last row or column in it, but
`AddSortableSpriteToDraw`, `AddCombinedSprite` and `ViewportAddVehicles` dropped it: the part
showed a row of pixels missing where it met the next. Upstream's dirty rectangles meet where they
meet; bands of rows cut every scrolled strip, so the rows lined up across the screen as it
scrolled. They now keep a sprite ending up to 2 x (`ScaleByZoom(1)` - 1) before the part. A
parent sprite outside the part was dropped with its children, which can draw in it: a parent is
kept within 128 pixels (`VP_REACH`; children reach up to 68 in the play script) and dropped at the sort
when it and its children miss the part.

### A tile's reach

A part is drawn by visiting every tile whose sprites could reach into it: upstream looks 250
pixels below the part for buildings and the highest bridge the settings allow, and as far to the
sides, so a strip of 24 pixels a scroll exposed passed many more tiles than drew in it.
Each tile now has its reach at each zoom, in three bytes (how far its sprites, what was culled of
them too, end above its north corner, to its left and to its right, in steps of 8), learned when
it is drawn (`VpReachLearn`, a worker's sent to the first core with its piece) and forgotten when
the tile is marked dirty (`MarkTileDirtyByTile`, all of them in `MarkWholeScreenDirty`). A tile
whose known reach ends below or beside the part, by 2 pixels at its zoom and a step more, is
passed over; while a tile highlight is shown, none below 64 pixels (its sprites are not the
tile's). The search below a part stops at the highest bridge on the map (`VpBridgeMost`), not
the highest one could build.
Each zoom's table is made by the first core before a round (`ViewportFlush`): a worker learns into a
table only once it is there, and the first core draws no piece, so zoomed out none was ever made and
every tile in reach was drawn (186 tiles a piece of the played script's zoomed out part, 101 since; on
`rvc_cpu` at the card's speed 3.68 pictures a second to 4.38).

### One order

Upstream's sorter orders two sprites by the others of the rectangle drawn: it compares every two
whose boxes do not lie apart, takes them in the order the tiles were visited, and two whose boxes
lie apart on two axes the opposite way round (one behind on x, the other on y) are in no order of
their own. A scrolled strip and the whole screen sorted the same two sprites differently: single
pixels apart, 96 in the scroll part on the first core alone, 904 on workers. The sort
(`ViewportSortParentSpritesKept`) is upstream's search with three changes: the sprites are first
put in an order of their own (by x + y of the box, then the rest of it and the sprite), only two
that meet on the screen (with their children, two pixels wider) are compared, and the shortcut for
a sprite with one before it is gone. Breaking upstream's "no order" by a key of each sprite made
it worse (cycles: a pole above and behind a wagon by the key, the wagon under a roof under the
pole by the boxes, and the cycle broken differently in each part). In the checks below every
picture is now the same as one drawn whole; it is not proved for every scene: three sprites whose
boxes order them round in a circle can still come out by the part.

### Scrolls

The machine's copy writes whole texels of the destination from words of the source
(`harness/apps/rvc_cpu.cpp`, `memop`): a move by a number of bytes not a multiple of 4 came out
shifted, a pixel in four. The play script scrolls by 24 and 16 pixels; when the view stopped
following a train it jumped by whatever it took (609 by -209 in one run), and the part the jump
kept was a pixel off (6,000 to 16,000 pixels, once in a few runs). A move now is row by row unless
it is whole words, and the copy is of the destination's whole texels, the ends by the processor.
`OPENTTD_PLAY_ODD=1` makes the play script's way back a pixel longer a step, an odd move.

A scroll costs what it exposes:

- The move of the screen is the workers', and not waited for: the machine copies the rows aside,
  the workers copy them back moved (`MoveOdd`), and the first core goes on to find what the
  scroll exposed. `ScrollSettle()` waits for it before anything reads or writes the screen: the
  pieces' round of a flush, `GfxScroll`, the mouse pointer undrawn, a move as a block, another
  move, the end of `UpdateWindows`' keep. In the scroll a move is 7.6 ms a picture, 2.4 of it the
  first core's own.
- A view narrower than the screen (a vehicle's window following it) moved row by row on the
  first core, 286k instructions a scroll; its rows are now copied aside and moved back by the
  workers, 4 rows a job (`MoveRows`).
- A kept part wholly inside the scrolled rectangle, before and after the move, moves with it
  (`ViewportScrollKept`); any other kept part flushes first, as before.

### Checking a picture

`OPENTTD_CHECK=1`: at every picture of the play script the screen is drawn again whole and the
two compared (a picture with nothing drawn too); `openttd: check:` says how many pixels differ,
where, and the longest line of them in a row or a column; `=keep` shows the picture as drawn, its
errors kept, `=list` names the first differing pixels with both colours. `OPENTTD_HOLD=N|bad|big`
stops at picture N of the play script, at the first that differs or the first by over 1,000
pixels, and says where both pictures are in memory (`rvc_cpu --save-ram`). The round's line says
how many differ. Over the paused play script (`OPENTTD_PAUSE=1`, which draws the screen again
for the status bar's "paused"):

| | Pictures differing, pixels |
|---|---|
| upstream's sort, 31 workers (20 October) | 127 of 132, 27,323 |
| upstream's sort, the first core alone | 74 of 132, 1,855 |
| one order, 31 workers or the first core alone, `OPENTTD_PLAY_ODD` too | 0 of 199, 0 |
| one order, the card (`rvc_harness --d3d11`, `rvc_lean`, the scroll part, odd way back) | 0 of 60 |

`tools/test_all.py openttd-scroll` (with workers and the odd way back) and `openttd-scroll-serial`
(the first core alone) run it and want 0 pixels.

When the windows closed, station names could stay on the screen with workers (3,814 pixels, in
`=keep` to the end of the script): the closed windows' rectangles were kept, the view then jumped
too far to scroll, and the whole view drawn again was kept over them in the same flush; the
pieces of both were copied out, then the texts of both, the old rectangles' station names last.
A part asked for over a kept one of another view, or of the same view moved since, now flushes
first (`ViewportKept`), as upstream draws one after the other; over one of the same view, only
what the kept one does not cover is kept (`OPENTTD_KEEP_FLUSH=1`: always flushed). Taking that
from a vehicle's window over the main view lost the window's view where the two met, 67,557
pixels at the windows' opening.

## Under the windows

A window's paint makes and lays out all its strings however little of it is shown: the station's was 130
thousand instructions, a vehicle's 25 thousand, the toolbar's 1.1 million (its checker a pixel at a time).
Upstream paints each dirty rectangle by itself, and each window in it once for every piece the windows in
front cut it in. With the windows of the play script open the first core spent half its picture there.

- A viewport's change under a window marks nothing (`AddDirtyBlockSeen` in `window.cpp`: a vehicle that
  moves, a sign, from `MarkViewportDirty`), nor does a window's own or a widget's change where a window in
  front covers it (`Window::SetDirty`, `NWidgetBase::SetDirty`); a window that moves or closes marks what it
  uncovers, as before. `OPENTTD_DIRTY_ALL=1` marks everything.
- A picture's dirty rectangles are painted together (`RedrawScreenRects`, from `DrawDirtyBlocks`), from the
  back window to the front: a window with a viewport where each rectangle shows it, as upstream, and any
  other window once, over the box of every rectangle on it and not cut for the windows in front, which paint
  that box again after it. `OPENTTD_WINDOW_CUT=1` is upstream's way.
- A checker (`FILLRECT_CHECKER`) is one record of the GPU's (`SeGpuChecker`), not a pixel a call.

The play script's windows part on `rvc_cpu` at the card's speed: the windows' paints 252 thousand
instructions a picture to 132, the first core 559 thousand to some 330, 6.55 pictures a second to 8.59;
`OPENTTD_REDRAW_LOG=1` prints each window paint, its rectangle and its cost.

## The tick in slices

A game tick of `busy.sav` is 1.2 to 1.7 million instructions of the first core, most of a second
at 2 MIPS, and upstream's loop draws only between ticks: a scroll showed a picture a tick. The
tick (`::GameLoop`) now runs on a stack of its own on the first core (`ottd_tick_swap`,
`shaderemu_v.cpp`) and gives way at `TickYieldPoint()` once its slice of a picture is spent: in
the vehicles' loop, between a train's two handler calls and its steps, at every step of YAPF's
search, in the loading of a station's vehicles and in the landscape's tile loop. The slice is
what the last pictures left of 100 ms (`OPENTTD_FRAME_MS`), at least 10 ms (`OPENTTD_TICK_LEAST`),
less how far slices ran past theirs, all in the machine's 5 kHz time (`VpTime`; the clock's is
too coarse). The drawing between two slices changes no game state; the company is the drawing's
while the tick is out, and a command or a save first finishes the tick (a world being made does
not give way).
`OPENTTD_TICK=whole` is upstream's, every tick at once.

The game is the same: `OPENTTD_SUM_AT=N` sums the map and every vehicle at the end of tick N, and
the play script's follow part gives the same (`240cf69c`, 837 vehicles, `892eac93`) whole and
sliced. The price is the game's own speed: while the picture is dear, as in a scroll, a tick is
spread over some 60 pictures, so the game runs at about a sixtieth of its speed until the view
stops moving. `OPENTTD_TICK=slices` is this way now; by default the tick is on a worker.

## The tick on a worker

The tick runs on a worker core of its own (`shaderemu_t.cpp`), on a stack and a 16 MB heap of
its own, while the first core draws. It waits where the first core holds it (`TwHold`): while
the first core reads the input, runs commands and the windows' timers, and finds the viewports'
parts; it runs while the pieces are drawn and between two pictures. Whatever of the window
system it would touch is left for the first core (`TickDeferTask`), and what the viewports draw
of a vehicle is a copy (`Vehicle::drawn`) made at the first core's sync for every vehicle the
tick said it changed (`VehicleChanged`, from `UpdateViewport` and `MarkAllViewportsDirty`). A
delete the drawing could be reading waits until no worker draws (`TickExclusive`).

- Its pictures overlap with the tick: a picture's last flush is left out (`VpFlushOut`) and the
  next picture's input, commands and timers run while the workers draw its pieces; the pieces
  draw the vehicles from a copy of their own made at the flush (`VehiclesKeptBegin`), the reach
  of a tile forgotten meanwhile is forgotten after it, the tick's frees wait until no piece is
  out (`TwAfterFlush`), and anything that reads or writes the screen finishes the flush first
  (`VpFlushFinish`: a command, the pointer, a sprite cache's deletes, a check). `OPENTTD_PIPE=0`
  waits for the pieces at the flush.
- While it scrolls, the ticks are spaced out to keep the picture's work at 60 ms
  (`OPENTTD_PACE_MS`): every tick costs the first core its sync, about 370 instructions a vehicle
  moved, and the windows its texts and the status bar change; while pictures are late the space
  starts at half a second and grows by a quarter a picture, to a tick a second at most
  (`OPENTTD_PACE`, 0 never), and is gone eight pictures after the view stops (`OPENTTD_PACE_LOG=1`
  prints it). It grew from 200 ms by an eighth before the link graph and the ships' paths left the
  tick: the tick a third cheaper then filled that ramp with twice the ticks while zoomed out, every
  tick's moves parts drawn again, and the card's played zoomed out fell from 1.84 pictures a second
  to 1.47 (at once to the most: 1.86, but 1.20 ticks a second against 1.26 before).
- A loaded game has none of its water regions, and each ship's first path made the regions it
  crossed one by one in the tick, half a million instructions each: ticks of 10 s and more in the
  first minute of `busy.sav`. They are all made at the start, one a job on the workers
  (`WaterRegionsMakeAll`; the labelling into `WaterRegionMade`, kept by the first core).
- 47 workers by default (`OPENTTD_WORKERS`): 127 cost the first core more a round than their
  pieces gave back (12.95 pictures a second paused against 13.9).

The game is the same: `OPENTTD_SUM_AT=36` on the scroll gives `d4e04e63`, 846 vehicles,
`26aae0d5` on the worker, whole and sliced (`OPENTTD_TICK_LEAST=300` for slices that end ticks).
`OPENTTD_CHECK=1` is 0 pixels paused; with the game running a sprite of a ship (z 56, an aqueduct's
height) differs now and then from the picture drawn whole, with the worker's flush and without it
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

Checked (29 October 2026): `OPENTTD_PLAY_SHORT=1 OPENTTD_PLAY_ODD=1 OPENTTD_PAUSE=1 OPENTTD_CHECK=1` under
`rvc_harness --cpu --warp --cpu-fresh` checked 70 pictures, 0 differ; the 200 pictures' sums of the paused
script with `OPENTTD_PLAY_JUMP=1` are the 8-bit blitter's but for picture 113, which shows picture 114's
window a picture early in both GPU runs (a window that is drawn again by the wall's time: WARP's pictures are
slower). `tools/test_all.py openttd-gpu --races`: 0 undone.

On the card (`tools/rv_compare`, `rvc_harness --d3d11 --unity 11.1,1.4,0.75`, two rounds alternating; GPU
and CPU are the same binary and image, `OPENTTD_DRAW=cpu` the second; "after" the frozen image of
`build/after`):

| Part | GPU | CPU | after |
|---|---|---|---|
| `openttd` (paused): scroll, pictures/s | 3.46, 3.21 | 2.50, 2.57 | 3.19, 3.21, 3.22 |
| zoomed out, pictures/s | 2.36, 2.49 | 2.23, 2.35 | 2.33, 2.25, 2.21 |
| windows, pictures/s | 20.0, 26.2 | 20.3, 17.7 | 17.9, 18.1, 18.0 |
| start to the game, s | 59.9, 59.7 | 61.7, 61.5 | 95.1 |
| `openttd-play` (played): game ticks/s | 1.26, 1.26 | 1.16, 1.15 | 1.05, 1.05 |
| scroll, pictures/s | 1.06, 1.05 | 1.02, 1.02 | 1.08, 1.09 |
| zoomed out, pictures/s | 1.23, 1.23 | 1.15, 1.15 | 1.02, 1.02 |
| windows, pictures/s | 1.59, 1.58 | 1.34, 1.33 | - |
| `openttd-title`: title frames/s | 33.1, 33.3 | 28.3, 28.7 | 27.1, 27.4 |
| start to the title drawn, s | 44.4, 44.1 | 46.2, 46.3 | 58.1 |

The CPU column is slower than the image of 28 October 23:58 (`rvc_cpu`, the paused short script: 1.62
pictures a second against 3.77), a loss of the CPU's path in the same commits. A played picture is about
half the game's tick on the first core (three ships' path searches that rebuilt water regions were 30% of
it, `RVC_PROF`) and collecting the viewport's sprites; the GPU's records are some 5% of the first core.

## Is it the same game

`host_sum.sh` builds the same tree for the build computer, without a window and without
`WITH_SHADEREMU`, and runs `test.cfg` (64 by 64, 1950, no competitors). The machine must give
its sums (`tools/test_all.py`, case `openttd`):

| Run | Date | Sum |
|---|---|---|
| seed 1, 500 ticks | 712229 | `9d6fc0a0` |
| seed 2, 2,000 ticks | 712250 | `66880c87` |

## Speed

At 2 MIPS on the desktop (`rvc_harness --cpu --cpu-ips 2000000`): the window says "Loading
OpenTTD..." until the title screen is up, some 85 seconds after the program starts, and Generate
makes a world of 64 by 64 in about 50 seconds, its progress drawn once a second (its Abort
button is not read meanwhile). A game on that map then runs at full speed, every game tick it
asks for, with 28 pictures a second; zoomed all the
way out about 20. Making a world of 128 by 128 takes about four times as long, 256 by 256 sixteen.

Play is measured on upstream's title game (`busy.sav`: towns, trains, ships and canals) with
`OPENTTD_PLAY=N`: a script of 200 pictures run N times, the same views at any speed, the game
running: 60 scrolling, 50 zoomed out twice and scrolling, 60 with three trains' and a station's
windows open and the view following a train, 29 after they close. Each part prints its pictures
a second; the first round warms the sprite cache, the second is the one read. The autosave is
off under it (one is 358 million instructions of LZMA, a third of a round where it fell).
`rvc_cpu --ips 2000000`, the second of two rounds, seconds of the machine's time:

| Part | Upstream's drawing | Viewport on 31 workers |
|---|---|---|
| scroll, 60 pictures | 116.2 (0.52 a second) | 73.0 (0.82) |
| zoomed out, 50 | 352.8 (0.14) | 109.1 (0.46) |
| windows, 60 | 251.3 (0.24) | 149.5 (0.40) |
| windows closed, 29 | 37.4 (0.78) | 37.3 (0.78) |
| the round, 200 | 757.8 (0.26) | 368.9 (0.54) |

The first core's instructions in the round went from 1,361 million to 550 million, all cores'
from 1,613 to 2,232 million. On the way: whole pieces on 7 workers 619 s, canals on workers 543,
15 workers 484, columns and rows cut together 418, 31 workers 384; the parts of a picture flushed
together with pieces sharing 16 bytes one core's 411, in buffers of their own 347; one order,
parents kept within a reach and a part over a kept one flushing first 368.9 (21 October).
What is left of the first core's (`RVC_PROF`, the same two rounds): the game's tick, 0.98 million
instructions a tick (vehicles' ticks 0.92), is 36%; the windows' widgets and texts 14%; keeping,
cutting and flushing the viewport's parts and waiting for the workers, with the scrolls'
copies, most of the rest of `UpdateWindows`' 45%.

On the card the whole round does not fit a run of five minutes. `OPENTTD_PLAY_SHORT=1` is 30 of the
scroll's pictures, 20 zoomed out and 20 with the windows; one round, the sprite cache cold, so the
first pictures load sprites. Upstream's drawing (the patch before the viewport on workers, its
blits on them) against 31 workers, run before, after, after, before (`rvc_harness --d3d11`, the
lean tick of b935841), seconds, against `rvc_cpu` at 2 MIPS (the same, and its second round, warm):

| Part | Card, upstream's | Card, workers | `rvc_cpu`, cold | `rvc_cpu`, warm |
|---|---|---|---|---|
| scroll, 30 | 29.8, 30.0 | 25.1, 25.1 (1.19 times) | 93.8 against 76.8 (1.22) | 53.3 against 33.0 (1.62) |
| zoomed out, 20 | 77.2, 78.6 | 36.2, 36.3 (2.15) | 234.8 against 113.5 (2.07) | 174.7 against 48.8 (3.58) |
| windows, 20 | 30.7, 30.4 | 18.0, 18.0 (1.70) | 91.3 against 55.5 (1.64) | 80.3 against 53.1 (1.51) |
| the round, 71 | 137.7, 139.0 | 79.3, 79.4 (1.74) | 419.9 against 245.9 (1.71) | 308.4 against 134.9 (2.29) |

The card gives the cold round's gain `rvc_cpu` gives, about 3 times as fast.

## The start

From the program's start to a game loaded with `-g` (`busy.sav`), the card's `start to the game`:
95.1 s before, 58.7 s after (two rounds, 58.69 and 58.67; `tools/rv_compare`, 29 October 2026).
On `rvc_cpu` the guest's seconds to the game went 126.1, 101.9, 97.7, 86.9, 83.1 as these came in:

- The title's own game (`opntitle.dat`) is not loaded before a game asked for with `-g`
  (`LoadIntroGame`): it was loaded only to be thrown away.
- A load's LZMA is decoded ahead on a worker into a ring of 16 pieces of 64 KB that the first
  core copies from while it reads what it took (`SeLzmaAhead` in `saveload.cpp`). The decoder's
  memory is made by the first core when it decodes the first piece. The worker allocates only
  from a reserve of 64 KB, and the blocks it lets go are freed by the first core.
- The link graph's jobs a load starts are not run at once: each goes to a worker of its own while
  one is free ("The link graph on the workers"), and with no workers one runs each tick; each is
  finished before it is first asked about (`SeRunPending`). The game state is the same:
  `503e40ee` after 1,300 ticks.
- The missing-glyph check reads every string of the language (11.5 million instructions). It is
  skipped for the image's English, whose base font has every glyph.
- The music's fine tune is a `pow` once a tone, not once a note. A song's notes are put in
  buckets by voice, not looked through once a voice (`shaderemu_m.cpp`). The song is made on a
  worker ("Music"): the card's start to the game 58.4 s to 53.5 (four rounds each, alternating;
  the parts' pictures and ticks a second as they were).

What is left of the first core's 275 million instructions: the savegame's objects read
(`SlObject`) 104 million, `AfterLoadGame` 35, the empty world made before the load 34, the
sprites loaded twice 16 each, and the music's first song 23.

## Sprites loaded

A sprite's levels closer than the normal zoom are shown nowhere in the image's settings. Their
pixels are not decoded; only their sizes are recorded, so that `ResizeSprites` pads the normal
level exactly as before (`SeCloseLevelsUnseen` in `grf.cpp`). The pictures are the same: every
picture's sum matched, skipped against whole. `OPENTTD_SPRITES=whole` is the old way.

## Sprites loaded ahead

A game's first pictures loaded every sprite they lacked: a worker that met one gave its piece up, the first
core loaded it with the rest of its eight on the workers (`VpLoadWanted`), and the piece was drawn again;
the first round of the play script scrolled, zoomed out and drew windows at 20 to 45% less than the second.
Now, when a game's sprites are placed (`GfxLoadSprites` in `AfterLoadGame`), every normal sprite of a file
is decoded and encoded on idle workers while the first core loads the rest (`SpritesAheadStart` in
`spritecache.cpp`): each call takes every n-th of them, sorted by file and place, into a share of a heap of
16 MB (`OPENTTD_SPRITES_AHEAD=MB`, 0: none), with `VpLoadOne`, the round's own loader. The first core takes
what is made into the cache when it next looks (`SpritesAheadTake`: each picture, before a round's loads,
and at a sprite it lacks), and the cache's entries point into the heap, which a cleared cache lets go
(`SpriteBytesFree`); the calls stop at their next sprite and are joined before the cache is cleared, its
files go, or the program ends (`SpritesAheadEnd`). A sprite the first core loads meanwhile is loaded as
before and what was made of it is left; a glyph asked for as one (`SpriteType::Font`) is made again from its
font's zoom, as upstream makes a sprite first asked for as one (a station sign's letters were a pixel off
otherwise). busy.sav: 8,879 sprites, 8,167 taken, 9.7 MB, in 2.9 s of 45 workers while `AfterLoadGame`
runs (`OPENTTD_AHEAD_LOG=1` says so). On `rvc_cpu` at the card's speed, played (pictures a second): scroll
8.73 to 10.47 (the picture's interval), zoomed out 2.22 to 3.48, windows 6.06 to 6.55.

## The game's own speed

The simulation is measured by `OPENTTD_WARM=100 OPENTTD_SUM=1 openttd -v null:ticks=300 -g
/usr/share/openttd/busy.sav` on `rvc_cpu`. It reports the game loop's ticks per second over
ticks 100 to 300 (the first warm the ships' water regions) and the state's sum. The sum must
stay `6abca409`, and `503e40ee` with 1,300 ticks, as upstream's.

| Build | Game ticks a second (`rvc_cpu`, ticks 100 to 300) |
|---|---|
| before | 2.52 |
| a vehicle's sprite sizes kept while its sprite is the same | 3.49 |
| a vehicle's place on the viewports updated once a tick | 3.62 |
| ships' paths made ahead | 4.12 |
| 64-bit divisions in 32 bits where both fit, no vehicle's frame rate timer | 4.38 |
| the viewports listed once while vehicles are marked, a loading percent out of sight not made | 4.49 |
| nine ships' paths made ahead at once, not six | 4.53 |
| a ship's path's indexes cleared where it logged, its tiles compared by words | 4.60 |
| a ship's path's tiles checked on the workers | 4.79 |
| an income out of sight not formatted, a few dirty blocks stored, not `memset`, a vehicle's new sprites not zeroed first | 4.87 |
| the vehicles' boxes made on the workers | 4.99 |
| the ships listed every 64 ticks | 5.03 |
| a vehicle out of every viewport marks no blocks | 5.21 |

- The frame rate window's timer reads the machine's clock word, not `clock_gettime`.
- `UpdateBoundingBoxCoordinates` keeps the bounds of a sequence of one sprite while the sprite
  and the sprite cache's generation are the same.
- While the vehicles tick, `Vehicle::UpdateViewport` only notes the vehicle. Its box, its place
  in the viewport hash and the dirty blocks are made once, after the last vehicle has ticked
  (`VpDeferBegin`, `VpDeferEnd` in `vehicle.cpp`). A train moves several steps a tick, and each
  step was a box, a hash update and a mark. The blocks marked are those of the old box and the
  last one, the only ones a picture can have shown. A vehicle deleted, or a sound placed at a
  vehicle, makes its update first. `OPENTTD_VP_NOW=1` is the old way.
- `GetAcceleration` divides by the processor's 32-bit division when both sides fit (`SeDiv64`):
  libgcc's 64-bit one was 50 instructions, 78 thousand calls in 200 ticks. The station and
  industry ticks take the 64-bit tick counter's remainder once, not once a station.
- A vehicle's tick has no `PerformanceAccumulator`: the machine's clock is in milliseconds, so a
  vehicle's tick read 0 or 1 ms for 56 instructions. The frame rate window shows no vehicle times.
- While the vehicles' places are marked (`VpDeferEnd`), the viewports are listed once
  (`ViewportsFixed`), not by walking every window for each vehicle.
- A text effect changed where no viewport can show it (a loading vehicle's percent) keeps its new
  text and makes its string and width only when a viewport can show it (`RefreshStaleTextEffects`,
  before the dirty blocks are drawn). Each was a string formatted and measured, 5,900 instructions.
- An income added where no viewport can show it is made the same way (`AddTextEffect`): its string
  was formatted and measured, 48 thousand instructions each.
- `AddDirtyBlock` stores the bytes of up to four blocks a column itself: `memset`'s start alone was
  70 instructions. A vehicle's new sprite sequence is not zeroed before `GetImage` sets it
  (`SpecializedVehicle::UpdateViewport`): only the sprites it counts are read.
- In `VpDeferEnd` the boxes of the vehicles whose sprite's bounds are kept are made on the workers
  (`VpBoxes`, `Vehicle::UpdateViewportBox`), with whether any viewport shows the area that would be
  marked (`ViewportsFixedSee`, the tests of `MarkViewportDirty`). Each stores to its own vehicle.
  The first core then puts each vehicle in the viewport hash and marks its blocks in the same order
  as before, and skips the marking of a vehicle no viewport shows: 68.7 thousand calls of
  `MarkAllViewportsDirty` in 200 ticks became 6.7 thousand.

What was left of a tick's 0.61 million instructions on the first core (`RVC_PROF`, the same run, 29 October
2026): trains 246 thousand, road vehicles 115, loading and unloading 27, aircraft 36, the tile loop 31, the
ships' own ticks 21, the vehicles' places 26. The vehicles tick in one order on one core. Their random
numbers come from one state in that order, so they cannot tick on workers and keep the same
game.

## The link graph on the workers

Upstream runs each link graph job (cargo distribution: demands, the multi-commodity flow, the flows
mapped) in a thread of its own, on a copy of the graph made when it is spawned, and joins it at a
date set then. Here a job is handed to one of four slots (`SeHand` in `linkgraphjob.cpp`,
`OPENTTD_LG=N` slots, 0 the old way), the first core gives the slot a free worker or says it has
none (`SeLinkGraphService`, from `TwService`; never while a flush's pieces are out), and the job
is finished where it is first asked about, as upstream joins its thread. A job on a worker
allocates from its slot's heap (1 MB, `OPENTTD_LG_HEAP`; busy.sav's take 112 KB at most) through
the program's `operator new`, its blocks of up to 1 KB used again by size; what it leaves, its
nodes' annotations with their flows, is copied out of that heap by the core that finishes it
(`SeFinish`), each tick for a job whose worker has ended (`SeReap`), so the slot takes the next
job. A job with no worker is handed again the next tick, and one whose heap runs out is run where
it is asked about. The jobs a load leaves are handed from the game's first tick on
(`SeRunOnePending`), not at the load: a paused game's pictures pay nothing for them. The game is the same: `6abca409` after 300 ticks of busy.sav, `503e40ee` after
1,300 (17 jobs on workers). Played (`OPENTTD_PLAY=1` on `rvc_cpu`), the 16 jobs a load leaves were
600 thousand instructions each in the tick's first 16 ticks, and the longest hold of the first
core by the tick 350 to 1,550 ms; with them on workers the tick's worker spends 226 to 285 ms a
tick in the scroll and zoomed out, not 294 to 383, and the longest hold is 18 ms.

## Ships' paths made ahead

A ship asks the pathfinder (YAPF) when it enters a tile with its cached path empty. Each call
is about a million instructions, and the expensive ones are those that make water regions
anew. After the vehicles have ticked, each ship that will ask at its next tile has that path
made ahead on a worker (`YapfShipAhead` in `yapf_ship.cpp`). The worker starts from a copy of
the ship and of the pathfinder's settings. The ships counted on each docking tile come from a
snapshot. The station's docking tiles and the region patches the region search starts from
are found by the first core. The worker reads no pool.

- **The map is still while the worker reads.** The worker reads the map only while the first
  core draws the dirty blocks (`DrawDirtyBlocks`), the one time the map does not change. At
  the end of drawing the first core waits until every worker has stopped at its next node
  (`YapfShipAheadQuiet`).
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
first (`YapfShipAheadQuiet`): the jobs launched at the last tick otherwise held up to nine workers
through every paused picture, and the first core waited at each picture's end for them to stop.
Paused, on `rvc_cpu` (`OPENTTD_PLAY_SHORT=1 OPENTTD_PAUSE=1`, pictures a second, the 8-bit blitter):
scroll 1.85 with the paths kept to 7.42 (7.81 before the paths were made ahead), zoomed out 1.06 to
1.67, windows 1.82 to 5.52.

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
the link graph's workers, and `OPENTTD_SHIP_AHEAD=check` says 6 checked, 0 differ.
With no job out the stillness ends without a pass (a job posted later starts in a later pass and
sees the word), and in a paused game the tick gives every path out up (`YapfShipAheadDrop`): a
paused game takes none, and a job out made every hold of the tick wait a pass or two for it, 2 ms
of each paused picture (the card's paused scroll 8.9 pictures a second to 7.9).

The water regions made invalid are made at the end of each tick's vehicles, one region a job on the
workers (`WaterRegionsMakeAll` in `water_regions.cpp`, from main's 5a3b851: the labeling `Make` into a
`WaterRegionMade` of its own, `Keep` on the first core). A loaded game has none made, and each ship's
first path made them one by one, half a million instructions each, on the first core or in a ship's
job while the first core waited at the picture's end. A ship's job that still makes a region of its own
waits at every sixteenth tile of it (`SeShipAheadPoll`). Played on `rvc_cpu` (`OPENTTD_PLAY_SHORT=1`,
pictures a second): scroll 0.60 to 1.57, zoomed out 0.48 to 0.77; with `OPENTTD_SHIP_AHEAD=0` the
same pictures are 1.52 and 0.83 (they were 0.83 and 0.56).

`OPENTTD_SHIP_AHEAD=check` makes every path taken again on the first core and compares the two:
0 differ. `OPENTTD_SHIP_AHEAD=0` is without. The check of the tiles logged is cut in parts of at
least 128 tiles, each part on a free worker and the first on the first core (`AheadValidTiles`): the
map is still while the first core waits there, and the reason given is the first part's that has one,
as in one pass. A slot's heap gives the same places every job, so a job clears its indexes only where
the last job logged (`clean_size`). Ships are listed again every 64 ticks, not walked in the whole
vehicle pool each tick. Over 1,300 ticks, 144 of about 210 calls were taken.
17 were invalid, 13 of them because a docking tile's count had changed. 49 had none made,
because they were not foreseen.

## At exit

The program gives its workers back itself at exit (`VpWorkersAtExit` in `viewport.cpp`,
registered with `atexit` at the first `mcw_open`): the ships' paths made ahead are cancelled
and joined, a lingering team is ended, and `mcw_close` runs while the memory is still the
program's. A kernel from before its own fix (`patch_exitpark.py`) parked the workers only when
`/dev/gpu` closed, after the memory had been given back. A ship's path waiting for the drawing
stored its answer every pass, into pages the next program got. The second start of the game
then found its sprites broken ("Tried to load  sprite #0 as a normal sprite", the 'query'
sprite's error). The wait stores its answer only when it changes. The GPU's memory is handed out
from the same place to every program (`seglMemory`), so the screen's picture is cleared at start:
the second start showed the first one's last picture until it drew. `test_all.py openttd-twice`
plays busy.sav, then plays it again paused with every picture checked (0 pixels, the workers
drawing), then starts it a third time for the state's sum after a tick (`e49e18e3`).

### Scrolling at 10 pictures a second

`OPENTTD_PLAY_PART=scroll` is the script's 60 scrolling pictures alone, `follow` its 60 with the
windows and the view following a train; `OPENTTD_PAUSE=1` pauses the game. `rvc_cpu --ips 2000000`
with the card's model (`RVC_ADFAULT`, `RVC_WCACHE`, `RVC_SOUND`), `OPENTTD_PLAY=8`, rounds 2 to 8;
pictures a second, game ticks a second and the first core's instructions a picture (21 October):

| Part | Before | After |
|---|---|---|
| scroll | 0.75, 0.75 ticks, 2,199k | 10.46, 1.17 ticks, 145k |
| scroll, paused | 2.57, 581k | 10.56, 110k |
| follow | 0.49, 0.49 ticks, 3,454k | 10.84, 1.50 ticks, 110k |

A scroll's picture in ms: the flush 70 (the pieces' round 44, the dearest piece 75k instructions,
the copy 3.5, the texts 3.3), the move 6.5, the windows' timers and the rest about 10, the tick's
part of the first core (its sync) 9. The tick's worker waits about half the time, held by the pace:
without it (`OPENTTD_PACE=0`) the game makes more ticks and the picture is late by their syncs.
The picture's interval is 66 ms (`OPENTTD_FRAME_MS`; 95 before 29 October 2026, which held the paused
scroll to 10.5 pictures a second) and the pace keeps the picture's work to 60 ms (90 before). On `rvc_cpu`
at the card's speed (3.7 MIPS, 0.25 ms passes) the played round, 200 pictures: 95 and 90 ms 7.32 pictures
a second and 1.46 ticks a second, 66 and 60 ms 9.17 and 1.28 (66 and 90: 8.49 and 1.27; 9d77da6 4.74 and
1.16); the played scroll 10.48 to 15.30 pictures a second (2.45 to 2.04 ticks a second), windows 8.97
to 12.18 (0.90 to 1.01 ticks a second).

On the card (rv_compare `openttd-play`, round 0 only, 29 October 2026; om3 is 9d77da6's image, of8 this tree's):
scroll 7.16 to 12.31 pictures a second, zoomed out 1.47 to 2.46, windows 5.5 to 7.85, start to the game 53.9 to
55.0 s, but game ticks while played 1.40 to 1.13 ticks a second. `rvc_cpu` at the card's speed said the ticks rose
(1.09 to 1.28, the same metric): it does not model what holds the tick on the card, so the pace is to be tuned on
the card (`OPENTTD_PACE`, the most between two ticks, 1000 ms; `OPENTTD_PACE_MS`) until the ticks are not lower.
Round 1 of both and the paused part (`openttd`) were not measured yet.

## Not done

- Pitch bend and other controllers in the music.
- The tick is one worker's ("The tick on a worker"): its vehicles, paths, tile loop, stations and
  towns are not split over more workers (but for ships' paths made ahead and the link graph's jobs), and the first core's sync of the vehicles it moved bounds how many ticks a scrolling
  picture can take. Signs, texts and windows' widgets are the first core's.
- A NewGRF chain other than canals' is resolved by the first core: only canals' resolver was
  read through for stores outside a core's own registers (`WorkerSafe` in `newgrf_spritegroup.h`).
- One order is not proved for every scene: sprites whose boxes order them in a circle ("One order").
- Online content, the network game and screenshots in PNG (no libpng) are not built.
