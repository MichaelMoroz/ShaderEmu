# OpenTTD

`openttd` is [OpenTTD](https://www.openttd.org/) 15.3, the open remake of Transport Tycoon
Deluxe, on the desktop, with the open base sets in place of the original game's files:
OpenGFX 8.0 (pictures, GPL-2.0), OpenSFX 1.0.3 (sounds, CC BY-SA 3.0) and OpenMSX 0.4.2 (music,
GPL-2.0). Nothing of the 1995 game is in the image. The engine is upstream's, compiled with
the patch below; the window, the sound, the music and the mixer are new (`linux/openttd/shaderemu_v.cpp`,
`shaderemu_s.cpp`, `shaderemu_m.cpp`, `mixer_shaderemu.cpp`). The picture is drawn by the 8-bit blitter into a
paletted texture that the GPU draws as one quad; sounds are voices of the sound card, and the
music is its FM voices.

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
- While it scrolls, the ticks are spaced out to keep the picture's work at 90 ms
  (`OPENTTD_PACE_MS`): every tick costs the first core its sync, about 370 instructions a vehicle
  moved, and the windows its texts and the status bar change; the space grows by an eighth while
  pictures are late, to a tick a second at most (`OPENTTD_PACE`, 0 never), and is gone eight
  pictures after the view stops (`OPENTTD_PACE_LOG=1` prints it).
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
The paused scroll is held to 10.5 pictures a second by the picture's interval (`OPENTTD_FRAME_MS`,
95); at 20 it is 13.9.

## Not done

- Pitch bend and other controllers in the music.
- The tick is one worker's ("The tick on a worker"): its vehicles, paths, tile loop, stations and
  towns are not split over more workers, and the first core's sync of the vehicles it moved bounds
  how many ticks a scrolling picture can take. Signs, texts and windows' widgets are the first core's.
- A NewGRF chain other than canals' is resolved by the first core: only canals' resolver was
  read through for stores outside a core's own registers (`WorkerSafe` in `newgrf_spritegroup.h`).
- One order is not proved for every scene: sprites whose boxes order them in a circle ("One order").
- Online content, the network game and screenshots in PNG (no libpng) are not built.
