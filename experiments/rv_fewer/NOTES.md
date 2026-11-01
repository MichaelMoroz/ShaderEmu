# Fewer instructions on Tiberian Dawn's first core (1 November 2026)

The question: Tiberian Dawn at least 25% faster on the card with passes not ending full more often or much sooner, by taking
instructions away rather than making the step faster. Result, of a build from the committed tree: **27,387 ms to 21,952 ms on h2h's load, 1.248x: just under the 25% asked.**
Play 1.150x. No tick, kernel or library change: the game, its build's counts, one `sprintf`.

## Correction: the first result did not hold outside its folder

Below, g14's 1.250x was a real binary but not what the commit built elsewhere: main's build of the same commit, 23,005 23,003 23,060 ms (1.19x), core 0 119.4 M on the card, state the same.

- **Cause.** GCC knows a function that is not public, in the counts, by a sum with its source's whole path and object in it (`coverage_compute_profile_id`, from memory of its source, not read here; what is measured is the effect). Counts made in `~/agents/fewer` lost those functions in any other folder, silently. `-Wno-missing-profile` hides it. A clean build of one commit in a second home at another path: core 0 113.6 M against 108.84 M, and the same kind of difference from the counted build as main's (`Keyboard_Process`, `ShaderEmu_Stats`, `Sync_Delay`).
- **Not the cause.** Stale objects: main had 108 of 31 October, as make does not see `.gcda` files, but the merge built over a clean old tree here was 21,955 ms, 108.82 M. A build of 35 s is normal (39 s clean here). `libc.a` differs in member times only: linked in, the game's bytes are the same. Compiler flags and the other inputs were the same but for paths.
- **Fix.** `tdawn-relative`, a compiler launcher in `linux/tdawn/build.sh`: paths from where the compiler runs. The counts' sum in the flags (`SHADEREMU_COUNTS`) compiles every file when counts change, in both games. Counts made again under it.
- **Proof.** Flat game 88327D99, 6,334,756 bytes: one home; a second home clean; the second home with the old tree built first and this one over it: the stripped ELF has the same sum (a3c1fa71) in all three. `~/nm` itself was not touched: **its build is still to be seen.** The bytes there are 88327D99 only if the merged tree's other inputs (`programs/mc`, `gles.c`, libmedia, ...) are these.
- **Red Alert** keeps whole paths, as its counts were made under them: `ralert.profile` holds only in `~/nm`. Its game's bytes did not move with the flag (9f26724f). Not measured under `~/nm` or made relative.

| final, from the committed tree | tree | final |
|---|---|---|
| h2h wall, 3 runs each, in turn | 27,375 27,388 27,399 (27,387) | 21,957 21,964 21,936 (21,952): **1.248x** |
| passes | 18,913 | 15,160 |
| core 0 on the card | 143.7 M | 114.4 M |
| a pass | 7,599 | 7,549 |
| ended full | 4.3% | 1.8% |
| play, 2 runs each | 19,622 19,631 | 17,075 17,068: **1.150x** |
| rvc_cpu core 0 (play) | 137.67 M | 108.78 M (87.29 M) |
| state | `1a9a814b` | `1a9a814b` |

`test_all.py tdawn` 7 of 7, `--races` 7 of 7, `ralert` 12 of 12, checks 0 wrong. g14 and this build have the same instructions on `rvc_cpu` and differ about 0.2% on the card by where things lie (a clean rebuild without relative paths, A45A9818, was 21,912): the line is within that, and on this build it is not reached.

Everything below is the history as first written; its 1.250x and "reached" are g14's.

## On the card

`experiments/rv_vm/h2h.py rv tdawn` (no-MMU image, lean tick), each image a private one with only the game's file changed, tree and
change in turn through `tools\gpu_queue.ps1`. Every line is measured, two runs of each side unless said; `card.jsonl` has them.

| image | what | wall ms (runs) | x | passes | core 0 on the card | a pass | ended full | rvc_cpu core 0 |
|---|---|---|---|---|---|---|---|---|
| tree | 50b8fdd0's parent | 27,355 27,401 ... 27,369 (12 runs, 27,355 to 27,439) | 1 | 18,91x | 143.6 M | 7,594 | 4.3% (2,270 a pass) | 137.66 M |
| g3 | first two batches | 24,070 24,051 | 1.138 | 16,285 | 123.9 M | 7,608 | 1.7% | 117.95 M |
| g6 | + made-again counts, limbo and theme | 22,628 22,663 | 1.209 | 15,530 | 117.7 M | 7,580 | 1.8% | 111.60 M |
| g12 | + Coord_Cell, sort, house pointers | 22,200 22,197 | 1.234 | 15,248 | 115.2 M | 7,555 | 1.8% | 108.95 M |
| g13 | + redraw bits | 21,969 21,959 | 1.248 | 15,160 | 114.6 M | 7,558 | 1.8% | 109.05 M |
| **g14** | + the frame log, counts again | **21,903 21,924** | **1.250** | 15,149 | 114.5 M | 7,556 | **1.8% (2,860 a pass)** | **108.77 M** |

Instructions a pass 7,595 to 7,556 (-0.5%) over the whole window; ended full 4.3% to 1.8%, and a full pass ends later than before. The
state is `1a9a814b` on all, on the card too.

### Load and play apart

`h2h.py rv tdawn_play --env "RVC_MARK=: mark "`: frames 1 to 1299, two runs each (`TDAWN_MARK`).

| | tree | g14 | x |
|---|---|---|---|
| play, wall ms | 19,621 19,643 | 17,041 17,056 | **1.152** |
| play, core 0 on the card | 102.3 M | 88.2 M | -13.8% |
| play, a pass | 7,805 | 7,592 | -2.7% (not full: 0.1% to 0.15%; pauses for the GPU's list come oftener) |
| the window's rest, the level's load (wall less play: inferred) | 7,761 | 4,866 | **1.59** |
| rvc_cpu: play | 101.23 M | 87.28 M | -13.8% |
| rvc_cpu: load (the rest) | 36.43 M | 21.49 M | -41.0% |

**A load gain is not a play gain: the +25% is +15% in play and +59% in the level's load.**

### Others, on the card, in turn (two runs each): level, as they must be, only the game's file differs

| load | tree | g14 |
|---|---|---|
| boot, to the prompt | 1,217 1,230 ms | 1,229 1,218 ms |
| gzip | 14,573 14,569 ms | 14,575 14,568 ms |
| png | 5,711 5,710 ms | 5,709 5,712 ms |

### A mission the counts did not see

Counts are of missions 1, 3 and 5, and mission 3 is the measure. Mission 10, `rvc_cpu`, 1,300 frames: core 0 124.55 M to 98.47 M (-20.9%),
state `57373b7a` on both. No card run of it.

## What each change saved (rvc_cpu, core 0, whole window; each against the one before; measured unless "inferred")

| change | M | note |
|---|---|---|
| no scenario names at a start, no digest of a scenario | part of -12.2 (batch 1, together) | 137.66 to 125.48 |
| trees asleep in the logic's loop, the two scan passes one walk, the palette's few words, Map.Logic by pointer | | play 101.23 to 95.13 |
| `sprintf`, `strcspn`, `strpbrk` of our own (`cxxrt.cpp`) | -1.34 | 125.48 to 124.14 |
| scans from a table, radar icons when asked, no SHA of a scenario, cells cleared once, fading search stops at a worse red | -6.19 | to 117.95 |
| a scan's generation raised only by a change, a techno's limbo | -1.65 | to 116.30; the table is good for 1,275 of 1,300 frames |
| themes' files when first asked, TDAWN_SLOW's numbers | -2.00 | to 114.31 |
| counts made again | -2.71 | to 111.60; text 6.34 to 6.24 MB |
| `Coord_Cell` by shifts, inline; counts | -1.05 | to 110.55 |
| layers walked by pointer in `Behind_Note` and `ShaderEmu_GL_Changed` | -0.59 | to 109.97 |
| sort copies what it moved, a house's place its pointer; counts | -1.01 | to 108.95 |
| redraw bits cleared only when one was set | +0.10 instructions, **-235 ms** (22,199 to 21,964) | 1,045 pass ends fewer on `rvc_cpu` (2,554 to 1,490): 2,048 bytes is exactly where `memset` hands to the machine's fill |
| the frame log's numbers only for it; counts | -0.28 | to 108.77 |

The card's slope over these: about 180 ms a million of core 0 (27,393 - 21,914 over 28.9 M, the redraw bits apart).

## What was tried and lost

- **Compiler settings, none kept.** Against the tree: `-flto-partition=one` 137.59, `-fipa-pta` 137.72, `-fno-strict-aliasing` 138.62. Against g3 (117.95):
  `hot-bb-count-ws-permille=999` 117.95, `=1000` 117.25 (text +1.5 MB), `early-inlining-insns=40` 121.98, `inline-min-speedup=0` 116.37 (text +1.8 MB). On fresh counts
  (g8, 110.55): `inline-min-speedup=0` 110.66, both together on g5 (114.31) 115.02, `-fsched-pressure` 110.61, `-fno-tree-vectorize` 110.71, `-fno-schedule-insns2` 110.47, `-fno-tree-pre` 112.03,
  `-fno-ivopts` 111.98, `-fno-gcse` 111.99. `-frename-registers` in the first list ran 2,974 M in one core and never got into a mission: stopped, not looked into.
  `max-inline-insns-small=80`, `-fno-exceptions`, `-fno-rtti` gave the same bytes as without: inconclusive.
- **A list of who is awake in the logic's loop** (made again by a generation): 110.73 against 110.55, a loss with fresh counts for a new invariant. Reverted. The trees were not what the loop costs.
- **`Is_Ally` forced inline**: 27,400 and 21,918 ms, level. Reverted. `always_inline` on `operator[]`, `Land_Type`, `Time`, `Lepton_To_Pixel`, `Adjacent_Cell` stayed in (they cost nothing) but moved nothing: the
  profile's "innermost frame" counts inlined lines under their own name, so a name there is not a call. I took it for one, and said so, for a while.
- **`Coord_Cell` with 6 bits a side** (my mistake, the map is 128 x 128): state `deb32f20` and 517 M. The state sum found it at once.
- **Fades** (`Fade_Palette_To`, 5 calls, about 3 M, a good part of `host_ms` and `Set_DD_Palette`): they wait by the clock. Under `TDAWN_NO_DELAY` they could go, and the measure would improve with no player the better. Left in on purpose.
- **Splitting `Find_Path`'s 2,048-byte clear to keep it from the machine's fill** (182 pass ends, 36 ms): inferred about 28 ms of stores against it. Not built.
- **The kernel**: 8.6 M of 118 M on g3 (3.9 M in play), flat, its largest function 0.39 M (`kernel.py`). Nothing to take.

## ISA: not built, on evidence that is partly others'

From the trace (`rewrite.py`, g0, adjacent pairs, % of core 0, overlapping and so upper bounds; inferred, nothing ran): load pair from sp +4 1.97, store pair other 1.75, `lbu;andi` 1.65,
load pair other 1.42, store pair sp 1.38, `andi;b` 1.31, `slli;add` 1.26, `add;mem` 1.02, `li;b` 0.90, `load;b` 0.62, `addi sp;ret` 0.59, `slli;sr` 0.56; optimistic sum about 14%.
`docs/plans/riscv-isa.md`: Zb is -3.81% of Tiberian Dawn's instructions, -2.6% modelled in time, within the card's noise when it was run; load and store pairs gave 0.99x on a bundled ISA and no texel fewer; and nearly every tick change costs 1 to 3% of
every instruction, which gzip, png and boot would pay for nothing. With +25% reached by the game alone, and a GCC pass, a tick, `rvc_cpu`, a WARP stamp and Unity's copy as the price, not tried. **This is a judgement from estimates, not a measurement.**
`auipc` is mostly `auipc;addi` (2.2%), and there is no GOT left to take.

## How far to trust it

- 1.250 is the line itself: the four g14 and g15 walls lie 21,901 to 21,935 with the tree 27,369 to 27,418, so the mean's ratio is 1.250 and the extremes' 1.248 to 1.252. **Reached, with no margin: call it level with +25%, not past it.**
- The counts contain the mission measured. Mission 10's -20.9% (no card) says the instructions are not its alone; the walls of other missions were not taken.
- Measured in the harness, not in the world. `TDAWN_NO_DELAY=1` as the load has always been.
- Play is +15%. Someone who plays and never loads gets that, not 25%.

## Tests on g14

`test_all.py tdawn` 7 of 7, `--races` 7 of 7, `test_all.py ralert` 12 of 12 (Red Alert applies this patch and takes `cxxrt.cpp`; rebuilt and made flat into the same image). `TDAWN_CHECK_SCANS=1` 1,275 looked at, 0 wrong;
`TDAWN_CHECK_SORT=1` 1,300, 0 wrong; `TDAWN_CHECK_PATHS=1` 0 of them wrong. **Not run:** the whole `test_all.py` (nothing outside these two games' files changed), Unity (no tick change), `tools/app_fuzz.py`, a save and load played through
(`saveload.cpp` keeps the marks but no case loads a game), the map editor's paths.

## The tools here

`count.ps1 IMAGE [-One] [-Env "..."] [-Scen N]`, `try.ps1 NAME [-Flags "..."]` (stops if the program was not made anew: a failed build otherwise counts the old one), `card.ps1 -Images a,b -Rounds 2 [-Load tdawn_play]`, `counts.ps1` (counts anew; clears the old
logs, `--uart-log` adds), `passes.ps1 IMAGE ELF` (what ended passes), `prof2.ps1`, `attrib.py`, `attrdiff.py`, `kernel.py`, `rewrite.py`, `vc.sh init|save` (the game's tree, apart from builds), `sweep.ps1`, `ra.sh`, `flat_ra.sh`.
