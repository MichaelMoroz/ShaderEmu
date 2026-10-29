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

## The viewport on worker cores

While a `ViewportKeep` lives (round a frame's dirty blocks and viewport scrolls in
`UpdateWindows`, round `DrawDirtyBlocks` and a scroll's strips), each part `ViewportDoDraw` is
asked for is kept with the clip it had (`ViewportKept`). At the flush (`ViewportFlush`: the last
keep's end, a scroll whose rectangle holds a kept part (`ViewportFlushOver`), the mouse pointer
drawn, round a viewport drawn without transparency or shaded, and a part asked for over one
kept, which must be drawn after it, its texts too) the parts are cut in pieces and every core
finds, sorts and draws pieces (`mcw_for`, the dearest first):

- A piece's tiles reach 128 pixels to its sides and 250 below it (buildings stand above their
  tile; at normal zoom), so a piece costs (width + 128) x (height + 250). The dearest piece of the
  flush is cut again, in columns or in rows, while the flush gets shorter, the flush being its
  dearest piece or all the work over the cores. A strip a scroll exposed, 24 pixels by 458, is so
  cut in rows: as one column it was one core's.
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
A part asked for over a kept one now flushes first (`ViewportKept`), as upstream draws one after
the other.

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
- The link graph's jobs a load starts are not run at once. One runs each tick, and each runs
  before it is first asked about (`SeRunPending`). The game state is the same:
  `503e40ee` after 1,300 ticks.
- The missing-glyph check reads every string of the language (11.5 million instructions). It is
  skipped for the image's English, whose base font has every glyph.
- The music's fine tune is a `pow` once a tone, not once a note. A song's notes are put in
  buckets by voice, not looked through once a voice (`shaderemu_m.cpp`).

What is left of the first core's 275 million instructions: the savegame's objects read
(`SlObject`) 104 million, `AfterLoadGame` 35, the empty world made before the load 34, the
sprites loaded twice 16 each, and the music's first song 23.

## Sprites loaded

A sprite's levels closer than the normal zoom are shown nowhere in the image's settings. Their
pixels are not decoded; only their sizes are recorded, so that `ResizeSprites` pads the normal
level exactly as before (`SeCloseLevelsUnseen` in `grf.cpp`). The pictures are the same: every
picture's sum matched, skipped against whole. `OPENTTD_SPRITES=whole` is the old way.

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

## Not done

- Pitch bend and other controllers in the music.
- The game's tick (but for ships' paths made ahead), signs, texts and windows' widgets are the first core's.
- A NewGRF chain other than canals' is resolved by the first core: only canals' resolver was
  read through for stores outside a core's own registers (`WorkerSafe` in `newgrf_spritegroup.h`).
- One order is not proved for every scene: sprites whose boxes order them in a circle ("One order").
- Online content, the network game and screenshots in PNG (no libpng) are not built.
