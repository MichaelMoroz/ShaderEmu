# What was slow, and what fixed it

Every item was measured in a port; the doc named has the numbers. Look for each of these in a
new port's first profile (`tools/prof.py report`; its pattern library is
`tools/prof_patterns.ini` - add a section when a profile shows a new slow form).

## The machine's costs to keep in mind

- **A pass** is up to 8,192 instructions; a wait, `fence.i`, a parallel copy or a full write
  cache ends it early. In the world's fixed rounds an early end costs the rest of the pass
  (`docs/machine/boot.md`, "Passes that end early"; `pwsh tools\tick_util.ps1`).
- **The write cache**: core 0 keeps about 6 KB (some 384 texels) of distinct 16-byte texels a
  pass, a worker 1.5 to 3 KB. More distinct texels stored in a pass ends it.
- **A system call** ~200-400 instructions; `GrGetWindowInfo` and other Nano-X questions two
  system calls and two task switches each.
- **Doubles** are library calls (~100 add, 200+ divide); **64-bit division** ~400.
- **A ROM read** ~8,000 instructions of copy and page cache however small; misaligned reads
  copied a byte at a time.

## Start-up

| Slow | Fix | Port |
|---|---|---|
| Case-insensitive file search walking folders from `/` (6,282 listings, 212M) | match the own name only; read a folder's names once (`ShaderEmu_Folder_Of`), invalidate on the game's own writes | Tiberian Dawn, Red Alert |
| A file opened to see if it exists (1,370 times) | look it up in the folder's names (`ShaderEmu_File_Listed`) | Tiberian Dawn |
| Archive read into the heap | map it from its file, pages present (`ShaderEmu_Mix_Map`) | Red Alert |
| Hash of all data at load (SHA-1 24 MB ~500M; MD5 20 MB) | drop it | Red Alert, OpenTTD |
| Every zoom level of every sprite made at load (a third of a start) | make levels from the shown zoom only | OpenTTD |
| Small reads from ROM files | 8 KB buffered, aligned to 8 bytes, seek inside the buffer, stdio unbuffered under it | OpenTTD |
| Sort comparator building strings each comparison | names made once, then sorted | OpenTTD |
| A hash set for a search on a grid | a bit a tile | OpenTTD |
| A big title/demo game loaded at start (226M) | a small one made by a native build (`make_title.sh`) | OpenTTD |
| Title picture unpacked five times | keep the last one | Red Alert |
| Fading/lookup tables by brute search | search only plausible candidates; make lazily on first use | Red Alert |
| Lump/texture name lookups by linear search | tables | Doom |
| zip inflated at every start | store entries uncompressed at build (`store_zip.py`) | ClassiCube |
| Tables of a renderer not used | made when that renderer is wanted | Doom |
| Clearing a table cell by cell | one piece | Tiberian Dawn |

Measure a start as a one-tick run (`openttd -v null:ticks=1 -g`) under `prof.py`, or
`rvc_cpu --count-from` from the command to the first frame's line. A frame drawn at every step
of a long load is a cost too: draw progress once a second.

## Every frame

| Slow | Fix | Port |
|---|---|---|
| Time asked by system call + 64-bit division (45k a frame) | clock word `0x81000034`, 32-bit math | Tiberian Dawn, Doom, Quake |
| Spinning on the clock while waiting | end the pass (`GrGetNextEventTimeout`, `mcw_rest`, pause) | Doom (177M of 402M), Quake |
| `seglSwap` waiting for the GPU | `seglSwapEarly` + `seglSettle` before the next write (93.4% to 99.8% of passes used) | Doom, Quake, OpenTTD |
| Copying the hidden page to the visible one | show the hidden page; copy only if something reads/draws the visible one (`Catch_Up`) | Tiberian Dawn |
| Redrawing unchanged status numbers, messages, plates | draw on change only (`st_lib.c` sed, `ShaderEmu_GL_Messages`, radar plate) | Doom, C&C, Red Alert |
| Scanning every sector/cell/wall for changes (9k a frame) | the game says what it changes (`doom_sector_changed`, `SHADEREMU_MOVED`) | Doom, C&C |
| Walking the BSP / sending the whole level every frame | level kept in GPU memory (`seglKeep`); visible set from PVS when the viewer changes leaf | Doom, Quake, Quake III |
| Doubles in timers and cvars | ms counter, floats, value kept beside the text | Quake |
| Pointer motion events (each wakes the window system) | read the pointer words; ask for events only on button/key change and a few times a second | all; Red Alert 10 to 17 fps |
| Window system asked every frame | when input words moved, or once a second | C&C, glxgears |
| Lines 64 bytes apart marked "visited" in place (filled cache sets) | marks in separate arrays | Doom |
| Texture copied upright: a column's bytes a row apart | store textures on their side (a column is a run of bytes) | Doom |
| `particle_t` 44 bytes at any alignment | three 16-byte texels, aligned, by what is written together | Quake |
| A whole map's animation stepped each tick (a third of a tick) | animate only what the view shows | Micropolis |
| Scroll copying rows | the machine's copy over the whole block (CSRs `0x0b0`-`0x0b3`) | OpenTTD |
| A scan asking every cell of growing boxes | a bit a cell by rows and columns, ask only set bits, same order | Tiberian Dawn |
| Logic that undoes itself (lift and put down a unit) | hold one back so the two cancel; check sums unchanged | Tiberian Dawn |
| Things at rest run through their thinker | asleep until told (`doom_thing_changed` wakes) | Doom |
| `R_PointToAngle` and the like every frame per thing | kept while the answer cannot change | Doom |
| Fixed-point division one bit a step / as doubles | as many bits a step as fit; integer division | Doom |
| `memcpy` of textures into a pool (6 KB a pass is the cache) | make the piece where it will lie, not copied; megabytes to workers (`ui_bulk.h`) | Quake, `nxplay` |

Compiler-level: `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`, LTO with raised inline
limits, PGO - each a few percent to a fifth (`docs/games/tdawn.md`, "What the compiler is told").
The fast loop's form matters more than operation counts: measure, do not reason.

## Worker-core patterns that paid

| Pattern | Port, file |
|---|---|
| Server's frame on its own worker, paced (given a frame only when the last is done) | Quake `server_shaderemu.c`, Quake III, Half-Life `sv_shaderemu.c` |
| Drawing a frame behind from copies the first core hands over between ticks | Doom `view_take` in `doom_gl.c`, Quake `scene_shaderemu.c`, C&C scene, Half-Life `R_SceneGive` |
| Work begun a frame ahead, joined when needed | Red Alert LCW (`workers_lcw`), Doom sight checks (`doom_ahead`), C&C second path search |
| One long job shared out by rows/pieces | Red Alert VQA rows, ClassiCube height map, libmedia PNG/JPEG/MP3/inflate |
| A second copy of a builder on a worker | ClassiCube chunk meshes |
| Lists for the GPU made by a worker, swapped with `seglSwapHand`/`seglSwapTake` | NES `NES_LIST=worker`, Quake's picture |
| Decode once on a worker and keep | Half-Life's logo film (`film_give`), NES DMC samples |

Not paying: jobs under some tens of thousands of instructions; splitting a ten-pass frame;
game logic the drawing reads and marks (copies first); one job a round for many workers (use a
team waiting on a round word: `mcw_for`).

## Worker checks, every time

- `mcw_open()` may return 0: keep the program correct with no workers, and keep a switch that
  forces the inline path (`DOOM_VIEW=inline`, `TDAWN_SCENE=inline`, `QUAKE3_SIDE=inline`).
- Sums equal between inline and worker runs; a `*_CHECK` mode recomputing the worker's answers.
- `RVC_RACES=FILE bin\rvc_cpu.exe ...`: 0 undone. Typical faults found: two variables in one
  texel (`mcw.c`'s own `page` and `count` once), a "settled" word written by every share
  (Doom: 29,685 undone, fixed by the first share clearing it), shared renderer stats (`r_stats`).
- `PHYS_COUNT=1`-style counters of pages brought for workers must say 0 (first-core stores
  and `mcw_touch` first).
- Code a worker reaches must store nothing outside its own state (`ShaderEmu_Core[who]` in
  Tiberian Dawn's path search; `SE_OnWorker()` in Quake III).
- Compile what a worker writes apart with 16-byte section alignment and `zz_pad`; inside a
  whole-program LTO link the library's worker-written statics land beside the program's
  (the NES had `line_news` in one texel with `matrices_dirty`).
- `mctest 3 120000` PASS before and after the game, and after `killall -9` mid-run.
