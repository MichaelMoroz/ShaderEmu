# Games from checkpoints: the RISC-V machine against gpu_vm

The goal (`gpu_vm/HANDOFF.md`, 23 October 2026) is twice the RISC-V machine's frame rate in real games. This folder
runs the games from checkpoints on both machines, on any tick step, and prints one table.

    python gpu_vm/bench_games/run.py save  --machine gvm|rv [--games quake3,halflife,nxray]   (once per image)
    python gpu_vm/bench_games/run.py bench [OUT] --machine rv,x18,x21,x22 [--games ...] [--rounds N]
    python gpu_vm/bench_games/run.py table OUT
    python gpu_vm/bench_games/paths.py FILE.prof        (a gvm_cpu --profile's bundles by path)

- **Checkpoints** (machine-local, not in git, about 250 MB each; a checkpoint is bound to its image):
  `build/checkpoints/gvm/GAME.snap` (saved with x22; any step loads it) and `build/checkpoints/rv/GAME.snap`.
  - gpu_vm: `build/gvm_apps/img`, made again on 23 October 2026 from the committed apps (`image.sh`).
  - RISC-V: `build/images/nommu_hl` of this worktree, which is `linux/prebuilt/nommu` with Uplink's data
    (`python linux/nommu/image.py nommu_hl`, with a junction `rvc` to the main checkout's `rvc`): the main checkout's
    `build/images/nommu` has no Half-Life data. It runs on the main checkout's `bin/rvc_harness.exe` and
    `experiments/rvc_opt` with `--define NO_PAGING` (from a snapshot the harness otherwise takes the full machine).
- **The games and their marks**: Quake III `q3dm17` with a bot, `fixedtime 33`, from `q3stat: frame 100:` (fps from
  its `q3stat:` windows); Half-Life `hldemo1`, `HL_MARK=31,43`, from `hlmark: begins` (frames over the level's 12 s;
  40 s runs, since the RISC-V machine takes longer than 15 s of the wall for them); nxray 320 x 240 with 255 workers,
  from its first `raystat: 320 x 240` (the mean of its pictures-a-second lines).
- **Columns**: core 0's bundles a pass (RISC-V: instructions) and why passes ended come from `--frame-log` (gpu_vm's
  backend puts the bundles of the pass above bit 8 of the "stall" word; RISC-V's budget is a pass of 8,192); "a worker
  ran its whole budget" is the share of passes where some worker counted 8,192 or more (RISC-V instructions, gpu_vm
  ops); the GPU's tick, commit and device times are the harness's timestamps (one frame in 64).

## Baseline, RTX 5070 Laptop, D3D11, fxc2, 23 October 2026 (one round each, alternating machines)

| | RISC-V `rvc_opt` | x18 | x21 | x22 | x22 / RISC-V | 2x goal |
|---|---|---|---|---|---|---|
| Quake III, fps | **83.2** | 27.6 | 33.3 | **44.6** | 0.54 | 166 |
| Half-Life, fps of the level | **9.8** | 5.7 | 6.6 | **7.5** | 0.77 | 19.6 |
| nxray, pictures a second | **24.6** | 6.9 | 15.7 | **19.0** | 0.77 | 49 |
| core 0 a pass: Q3 / HL / nxray | 4,135 / 6,016 / 2,783 instr. | 566 / 754 / 1,067 | 548 / 807 / 1,026 | 1,070 / 1,502 / 1,914 | | |
| passes a second: Q3 / HL / nxray | 488 / 644 / 348 | 1,248 / 1,283 / 144 | 1,498 / 1,538 / 292 | 783 / 831 / 223 | | |
| tick ms a pass: Q3 / HL / nxray | 1.74 / 1.40 / 2.65 | 0.67 / 0.67 / 5.56 | 0.54 / 0.56 / 3.18 | 1.14 / 1.05 / 4.23 | | |
| Q3 passes ended | budget 29%, pause 70% | full 73%, sync 27% | full 72%, sync 28% | full 73%, sync 27% | | |
| HL passes ended | budget 47%, pause 53% | full 96% | full 94% | full 91% | | |
| workers, M a second: Q3 / HL / nxray | 11.9 / 12.4 / 559 | 5.0 / 7.1 / 116 | 5.9 / 8.7 / 260 | 7.9 / 9.7 / 324 | | |

The card here is slower than the 5090 (Quake III x22 44.6 against 50, Half-Life 7.5 against 18), and the RISC-V
machine more so for Half-Life; the ratios are what to read. On this card x22 is 0.54x of RISC-V in Quake III and
0.77x in Half-Life and nxray.

## x23 (23 October 2026, the write cache in buckets of four ways; same checkpoints, x22 run again beside it)

| | RISC-V | x22 (again) | **x23** | x23 / RISC-V |
|---|---|---|---|---|
| Quake III, fps | 83.2 | 41.7 | **63.2** | 0.76 |
| Half-Life, fps of the level | 9.8 | 7.3 | **10.6** | 1.08 |
| nxray, pictures a second | 24.6 | 18.9 | **20.9** | 0.85 |
| core 0 a pass: Q3 / HL / nxray | 4,135 / 6,016 / 2,783 | 1,089 / 1,420 / 1,922 | 2,294 / 2,735 / 3,090 | |
| passes a second: Q3 / HL / nxray | 488 / 644 / 348 | 757 / 837 / 218 | 416 / 509 / 159 | |
| tick ms a pass: Q3 / HL / nxray | 1.74 / 1.40 / 2.65 | 1.18 / 1.03 / 4.52 | 2.16 / 1.77 / 5.98 | |
| passes ended, Q3 | budget 29%, pause 70% | full 72%, sync 28% | sync 61%, full 38%; a worker at its whole budget in 88% | |
| passes ended, HL | budget 47%, pause 53% | full 92% | full 83%, sync 13% | |

x22's second run is within 6.5% of its first (Quake III 41.7 against 44.6). With x23 the bound moves: in Quake III
a worker (the server's or the scene's) now runs its whole budget in 88% of the passes (11,342 ops a pass for the
busiest one), so the pass is that worker's 8,192 bundles, about 260 ns each (2.16 ms).

## Where a pass's time goes (23 October 2026)

**There is no 0.34 us bundle.** That figure was a pass's tick time over core 0's bundles. Fitting each pass's wall time
(`--frame-log`'s microseconds) against core 0's bundles in it gives a marginal cost of **80-290 ns a core 0 bundle**
(Quake III x21 140, x22 79; Half-Life x21 191, x22 291), close to the bench's real cases on this card (`gvmbench`
x22: p_int 104 ns, CoreMark 193, gzip 257). Most of a pass is a cost that is not core 0's bundles:

- An idle machine with no workers: tick 21 us a pass (x21 and x22 alike). CoreMark at the shell: tick 204-222 us for
  about 1,500 bundles (~135 ns a bundle). So one core running real code is as cheap as the bench says.
- Quake III x21, the passes in which no worker ran: 413 us of wall (324 median) for 673 core 0 bundles. Each worker
  that runs in the pass adds 50-140 us (1 worker 468, 2 605, 3 647, 4 711 us), although the workers run in parallel
  warps and do 900-1,300 ops each. Half-Life alike (0 workers 255 us, 7 workers 1,029 us), with about 40 idle workers
  drawn every pass (awake, no job: `freeze` keeps them drawn).
- A pass in which some worker reaches its whole budget costs 1.3-1.9 ms (5% of Quake III's passes on x21, 21% on x22).
- x22 doubled both core 0's bundles a pass and the tick's time a pass (Quake III 0.54 -> 1.14 ms): its gain in fps
  (1.34x) comes from fewer passes ending on a full cache, not from cheaper bundles.

With the busiest worker's ops a pass (`--frame-log`'s `.most` file, added for this) a pass fits as about 0.35-0.75 ms
fixed + 250 ns x core 0's bundles + 115-120 ns x the busiest worker's ops (Half-Life x22 and x23; R^2 0.5, the wall's
per-frame times are noisy); "core 0 + the busiest worker" fits a little better than "the larger of the two", so the
cores do not overlap fully. The fixed part is not the tick's own (an idle machine's tick is 21 us); `GVM_NO_FREEZE=1`
(every worker drawn every pass, a measure) adds 0.55 ms a pass to Quake III x21.

Pass ends: gpu_vm's core 0 ends 73-96% of its passes on a full write cache after 550-1,500 bundles, while RISC-V's
core 0 runs 4,000-6,000 instructions a pass and its passes end on the budget or the program's own pause.

Which bundles leave the fast loop (gvm_cpu `--profile` between two of the game's lines, 32 cores, `paths.py`):

| | core 0 leaves the fast loop | of which general | workers leave | of which general |
|---|---|---|---|---|
| Quake III, frames 200-300 | 27.8% | 1.8% | 39.4% | 17.2% (the firmware's loop) |
| Half-Life, level 31-35 s | 20.1% | 1.6% | 26.7% | 4.5% |
| nxray, one picture | 36.9% | 23.2% (the kernel) | 50.6% | 0.4% (step12 50.2%: floats) |

`gvm_cpu --profile-at TEXT --profile-until TEXT` counts between two console lines (added for this).
