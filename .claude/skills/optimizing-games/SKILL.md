---
name: optimizing-games
description: Make a game or large program that already runs on the ShaderX86 machine faster - what bounds its frame, how to profile it with tools/prof.py, what to fix on the first core, what to move to worker cores, drawing a frame behind, the write cache and texel layout of what is stored, per-core state, compiling whole with played counts (LTO, PGO), and the checks that say it is still the same game (state sums, RVC_RACES, RVC_RACES_NEAR, walking instead of standing). Learnt on Doom, Quake, Quake III, Half-Life, Tiberian Dawn, Red Alert, ClassiCube, the NES and OpenTTD. Use when asked to optimize, speed up or raise the frames or ticks a second of a game or program on the machine, or to decide whether work should go on a worker. Triggers on: optimize a game, frames a second, too slow, core 0 a frame, passes a frame, prof.py, game_par, worker core for X, frame behind, write cache full, texel layout, PGO, profile.gz, LTO.
---

# Optimizing a game on the machine

Depth: `docs/machine/multicore.md` ("Where workers pay", "A game's speed: passes a frame"), `docs/machine/profiling.md`,
and the game's own `docs/games/NAME.md`. `porting-games/hotspots.md` has the table of what was slow in each port.
Tools run from a checkout with `bin\` and `build\`: see `running-tests` for a worktree, and use PowerShell.

## 1. Set the baseline and the measure

A game's speed is passes a second over **passes a frame**. Core 0's instructions a frame are not the measure.

1. Keep the "before": a private image of the tree, or `linux/prebuilt` of the last commit. `tools/private_image.py` reads
   the shared root each time, so a "before" made again after a rebuild is no longer before.
2. Measure. `$env:RVC_THREADS='8'; python tools\game_par.py GAME --calibrated` for frames and passes a frame (the
   `calibrated-measurements` skill), and the same without `--calibrated` for instructions. Games not in `GAMES` have a
   mark of their own: `NAME_MARK`/`HL_MARK` for `--count-from` and `--until`.
3. A run shifts with its timing. Compare means (`--runs 3`). OpenTTD's played runs vary by about 10% in instructions, so
   a small change is judged on a paused or fixed run. Quake III's server sums need `QUAKE3_ENTER=1000`, or they go by load time.
4. Say which load it is. Standing still hides most of a game: Half-Life's fight and the level's first 30 s disagree (24.3 and
   50.5 fps, calibrated, 31 October 2026) and are bound by different things: core 0 waits in the one and not in the other.

## 2. Find what bounds the frame

Before reading any flat profile, ask which of these it is, as the answer decides what is worth doing:

| What the numbers say | What bounds it | What pays |
|---|---|---|
| Core 0 is busy all the frame, workers short | core 0's chain | section 3, or move a part off it |
| Core 0 waits at the swap or for a job | the worker's job plus about 2 passes of handover | shorten the job; core 0 savings buy no frames (Half-Life's fight) |
| Passes end early (`RVC_PASSLOG`, `tools\tick_util.ps1`) | waits, full caches, copies | section 5 |
| Passes a frame near the picture's interval | the pacing itself | nothing here: OpenTTD stands at its 66 ms (`OPENTTD_FRAME_MS`); look at its ticks instead |

`RVC_PASSLOG=FILE` gives a line a pass from the mark (`pass end ran pc ra ms`; end 0 budget, 1 ended itself, 2 waited, 4 full,
5 a copy), `RVC_PASSLOG_STACK=N` who ended it. Half-Life's `HL_RENDER_CALLS=1` prints its job's timeline: a game with a job a
frame behind wants such a line of its own.

## 3. Profile

    python tools/prof.py run OUT -- --image build/images/nommu --quiet --expect "/ # " --send "echo FL-''GO; nano-X -p & sleep 2; doom -1 -timedemo demo1; echo LX-''DONE\n" --count-from FL-GO --until LX-DONE --seconds 120
    python tools/prof.py report OUT --elf ~/nm/work/src/doom-build/doom --top 40 [--lines]
    python tools/prof.py auto OUT --elf ~/nm/work/src/doom-build/doom -- <the same rvc_cpu arguments>

- `report` must print `doom: at 0x83800040 ..., N of the hottest pcs match (100%)`. A low match or a refusal means the ELF is not
  the build the image has: rebuild the image or use the matching ELF. `--find NAME` looks under `~/nm/work/src`.
- Read "texels new to a pass" and "where core 0 found the write cache full" (with `RVC_WCACHE=1`), not only the time.
- `auto` gives verdicts on loops and calls: `INDEPENDENT`, `INDEPENDENT but neighbours write one 16 bytes`, `A SUM carried in memory`,
  `DEPENDENT`, `about one instance a run`. It is a list of candidates, not a plan: a run of a few thousand instructions is not
  worth a pass whatever it says.
- Profile without test sums: `NXVIDEO_SUM` and `NXVIEW_SUM` were most of core 0, and `HL_SCRIPT`'s `atof` costs 8k a frame in test runs only.
- A whole-program build's flat profile hides what it inlined: `--lines` needs `-g`; `addr2line -f -i` names what was inlined.
- A window late in a long program: before `--count-from` the run is plain speed. `RVC_PROF_PASS=N` skips a load after the mark.
- `RVC_CALLS=FILE` with `python tools\mc_candidates.py FILE ELF` ranks core 0's calls for a job.
- `python tools/prof.py doubles OUT --find NAME` is the share of soft doubles (a double sum is 80 to 100 instructions).
- A start is measured like a frame: OpenTTD's `-v null:ticks=1 -g`. Starts are worth it (OpenTTD 222 M to 110 M).

## 4. Cheap first: stop doing the work

In order of what most often paid, with the port it came from:

1. **Do not do it.** Redrawing what did not change (Doom's status bar, C&C's plates, OpenTTD's window widgets on a timer,
   `OPENTTD_WINDOW_MS`); hashing data at load; a progress picture four times a second; tables of a renderer not used.
2. **Be told, do not scan.** The game says what it changed (`doom_thing_changed`, `doom_sector_changed`; `UpdateViewport` in
   OpenTTD). Each such call becomes an invariant: a new write that misses it is a bug, so write the check first (section 8).
3. **Skip what is as it was.** Quake III's server: a snapshot entity equal to the old frame's (`se_snap`), a link with the same box
   (`se_boxed`), a view whose entity's leaves did not change (`se_seen`, `se_gen`). Half-Life: a move predicted once.
4. **Only the part that is used.** `SE_LightsGive` copies only lit lights; `any_selection` asked once a viewport part rather than at
   each tile (3.5% of OpenTTD's workers); a slot cleared by its keys, not whole.
5. **The clock word** (`0x81000034`) in place of a system call, 32-bit maths in place of 64-bit, floats in place of doubles.
6. **The machine's copy** for anything large (CSRs `0x0b0` to `0x0b3`; a copy from RAM to RAM is 256 KB a pass). Fills go to borrowed workers (`ui_bulk.h`).

Tried and not worth it, so measure before believing: Half-Life's mesh cache per submodel and skin (no frames), a kernel name table built at the
first lookup (a program that runs once paid more: nxweb +9%; the image carries the table now), `strcmp` by words (calls average 2 to 3 characters).

## 5. What is stored: the write cache and texels

One core keeps about 5.25 KB of distinct 16-byte texels a pass (336 texels: 64 + 32 + 16 buckets of three), a large worker 2.6 KB, a
small one 0.66 KB. A store that finds no room ends the pass. `prof.py` counts against 384 and 192: read those as rough.

- Lay out by what is written together: Quake's `particle_t` is three texels; Doom's line marks moved to their own arrays; Doom's
  textures lie on their side, so a column is a run of bytes.
- Do not clear a table whole at every page or frame (`css_reset` was 168 KB a page). Make a decoded piece where it will lie (`piece_place`).
- A job that stores much wants a core with a large cache: `mcw_call` takes the lowest free of cores 1 to 16 (`mcw_fast`).
- The write cache's stalls in a worker count too: run with `RVC_WCACHE=1` (`game_par.py` does) and read `[cpu] write cache:`.

## 6. Move work to workers

What pays: work that needs **no answer in the same frame**, with its **own data**.

| Pattern | Where to copy it from |
|---|---|
| A server on its own worker, paced: given a frame only when the last is done | Quake `server_shaderemu.c`, Quake III, Half-Life `sv_shaderemu.c` |
| Drawing a frame behind from copies | Half-Life `rnd_shaderemu.c`/`gl_kept.c`, Doom `view_take`, Quake `scene_shaderemu.c`, C&C's scene |
| Copies made while the drawing still draws, into a second set | Half-Life `R_SceneAhead` |
| The game's logic on a worker, yielding, held while the first core reads input | OpenTTD `shaderemu_t.cpp`: `TickYieldPoint`, `TwHold` |
| Work begun ahead and joined or thrown away | Doom's sight checks (`doom_ahead`), C&C's second path search |
| A picture cut in parts, a core a part | OpenTTD `ViewportFlush` |
| A long job shared out | libmedia, Red Alert's movie rows, ClassiCube's meshes |

What does not: a job under some tens of thousands of instructions (a pass each way), splitting a ten-pass frame, a job a round
for many workers (use a team on a round word: `mcw_for`), and logic whose objects the drawing reads and marks, unless the drawing works from copies.

Rules that came from real bugs:

- Keep the way back: `NAME_X=inline`, whose sums must equal the worker's. Half-Life: `HL_RENDER=core`, `HL_SERVER=inline`.
- **Per-core state.** Anything both sides write gets one copy a core, and must be *made* on every core that uses it. Half-Life's hit hulls
  were core 0's only, so every bullet of the server's worker went through monsters. Quake III's zone is the server's own; a static is `x_of[3]`
  behind `SE_Who()`, or `VpCore()` in OpenTTD.
- **Texels apart.** A static of core 0's beside a worker's breaks now and then on the card, only in some layouts. Half-Life's `SE_APART`; a per-core array is 16 bytes a
  core (OpenTTD's `WaterRegionCheck`: 12-byte vectors were undone).
- **LTO mixes statics.** A library a worker calls is compiled apart (`gles.c` in `doom.sh`, `linux/nes/build.sh`), or the whole link keeps `-fdata-sections -fno-common`
  and the 16-byte assembler (OpenTTD's `ottd-as`).
- **A stack of its own is not a worker to the library.** Half-Life's server and drawing are where `mcw_on_worker()` is false: they must not load, inflate or start workers.
  A sequence group first played on the drawing's worker froze a level.
- **A worker cannot wait for a page.** Touch code and buffers from core 0 first (`mcw_touch`, `take_workers` in `nxphys.c`); `[cpu] stops:` must say 0.
- Core 0 waiting polls helpers too (`helpers_poll`): a helper stopped at a page otherwise stops the game.
- More workers is not always faster: OpenTTD 47, a part cut by its share of the cores; measure `prof.py search --param NAME=a,b,c`.
- Bugs of this class: the `worker-core-bugs` skill.

## 7. Compile it whole, with played counts

Late, as they age. Each brought a few percent to a fifth, and two did not pay.

- `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`; hidden globals (`nommu_hidden.h`: a GOT load less at each use); `-flto -flto-partition=1to1 -fno-toplevel-reorder`
  for C++ (constructors keep their order) with larger inlining limits; `OPENTTD_LTO=off` is the way back.
- `NAME_PROFILE=count` builds the counting program in its own folder (`QUAKE_`, `QUAKE3_`, `NES_`, `CLASSICUBE_`, `OPENTTD_`, `TDAWN_`, `RALERT_`, `DOOM_`, `HALFLIFE_`); `=off` is without.
  Counters are `linux/userland/pgo/gcov.c`, written at the end or at `kill -USR2`. C&C has its own in `host.c`.
- Count with **few workers**: stores of many workers to one counter in a pass lose all but one. OpenTTD counts its drawing on two (`linux/openttd/counts.py`), C&C with `TDAWN_SCENE=inline TDAWN_FRAMES_CORE=0`.
- `python3 linux/tdawn/profile.py make OUT LOG ...` makes the file; count with the repository's build script, as a call's targets go by a sum with the source's path in it.
- A function changed since has no counts, without warning. Make them again after a large change (`counts.py` is three minutes). A compiler without TLS needs `gcov.c` with `-D__thread= -DGCOV_ONCE`, or a run ends in 100 MB of counts on the console.
- Doom's and Half-Life's counts gave fewer instructions and no frames on the card: not kept.
- `linux/openttd/build.sh` puts the tree back from the patch on every build: write the tree's diff to `openttd.patch` first, or edits are lost silently.

## 8. Is it still the same game

Run all of these before calling it a gain. A faster wrong game was the commonest result of the first try.

1. **State sums** must not move: `python tools/test_all.py GAME-TAG`. `NAME_SUM`, `DOOM_HASH_TICS=100`, `QUAKE_SUM=10`, `QUAKE3_SUM=1`, `OPENTTD_SUM_AT=36`, with the way back too. A float game's sums on the card differ by design.
2. **Its own checks**: `DOOM_VIEW=told DOOM_VIEW_CHECK=1` (0 not as told), `TDAWN_CHECK_PATHS=1` (`0 of them wrong`), `HL_ROTATION=check`, `HL_PREDICT=check` (0 over 0.1), `OPENTTD_CHECK=1` (0 pixels, paused), `NXWEB_CHECK=1`. A cache or a "told" path gets a check mode before it gets a default.
3. **Races**: `RVC_RACES=FILE` and `RVC_RACES_NEAR=4`, 0 undone, on the play you changed, not only the test set.
4. **Walk it.** Standing in Half-Life's fight never played a sequence group. `HL_SCRIPT` with `+forward`, turns and shots for 1,200 s of the guest's on the desktop with `RVC_RACES`; through a level change with both drawing modes (`docs/games/halflife.md`, "A level that goes"). An elevator recipe that did not run is a check not done: say so.
5. **Close it.** `mctest 3 120000` after `killall -9`; OpenTTD once did not exit, held the GPU and stopped the next two programs.
6. **Swap discipline**: `NAME_HOLD=N NAME_HOLD_LATE=1` under `--gpu-capture` with `NAME_SWAP=wait` and without: the same picture.
7. **The card**: `python tools\game_par.py GAME --card` for the claim, alternating with the tree; a timing beside another agent's work is suspect.

## 9. Report and record

Numbers with their load, tool and date; the same image pair for before and after; what was tried and lost; what is left and what bounds it now. Put them in `docs/games/NAME.md`, and a dense line in `AGENTS.md` only for an invariant a later change could break.
