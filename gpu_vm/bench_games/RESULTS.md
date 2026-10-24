# gpu_vm against the RISC-V machine, program by program (24 October 2026)

How much faster gpu_vm runs the machine's programs than the RISC-V machine, on the card, for every kind of program
the image has: games, the path tracer's scenes, physics, the emulators, the interpreters, libmedia, unpacking,
CoreMark and gzip, and how long the desktop's programs take to start.

**The ratio is gpu_vm's speed over RISC-V's: above 1, gpu_vm is faster.** For a timed run it is RISC-V's seconds over
gpu_vm's; for a game it is gpu_vm's frames a second over RISC-V's.

## The setup

- **Card:** RTX 5070 Laptop, D3D11, the tick compiled by fxc2, `bin\rvc_harness.exe`, every run through
  `tools\gpu_queue.ps1`. The guest's clock follows the wall clock.
- **gpu_vm:**
  - Image: **`build/gvm_apps/img-cur`**.
  - Ticks: **v0009**, the fastest (`gpu_vm/tick/steps/v0009.hlsl`), and **x23**.
  - Checkpoints: `build/checkpoints/gvm-cur` for the games, `build/checkpoints/wide/gvm-cur/desk.snap` for the rest.
- **RISC-V:**
  - `rvc_opt` of the main checkout.
  - The games run on image `build/images/nommu_hl` with checkpoints `build/checkpoints/rv`.
  - The rest run on image `build/images/nommu_hlb`: nommu_hl with the bench's CoreMark and minigzip, the same sources
    and the same clang as gpu_vm's. Its checkpoint is `build/checkpoints/wide/rv/desk.snap`.
- **Quake III** runs uncapped (`+set com_maxfps 0`) on both machines, from the checkpoints saved again on 24 October.
- **Commands:**
  - The games: `python gpu_vm/bench_games/run.py bench --machine rv,v0009,x23 --games quake3,halflife,nxray,doomt,quake1`.
    Each runs from its checkpoint for 15 s and reports the game's own figure. Logs: `build/bench_games/20261024-012206`.
  - The rest: `python gpu_vm/bench_games/wide.py bench OUT --progs ...`. Each program is typed into the desktop
    checkpoint, and it times the guest's uptime around the command; a start is the busy CPU time over 4 s, less an idle
    machine's. Logs: `build/bench_games/wide-a` and `wide-b`.
- **Rounds:**
  - The programs of `wide-a` ran **two interleaved rounds**, in the other order the second time. The table gives their
    mean, and each run's figure is in the logs.
  - Everything else ran **one round**. The second rounds were cut short at the user's request.
- **Correctness:** every row was checked against the program's own output.
  - The checks agree between the machines: sums, CRCs, digests and "done at frame N".
  - The exception is the path tracer's picture sums, which differ between gpu_vm and RISC-V. gpu_vm compiles programs
    with the card's approximate special functions (`GVM_MATH`). Its pictures are close, not bit-equal.

## Games (frames a second, one round)

| game | RISC-V | gpu_vm v0009 | gpu_vm x23 | v0009 / RISC-V | x23 / RISC-V |
|---|---|---|---|---|---|
| Quake III (uncapped) | 79.4 | 115.9 | 79.8 | **1.46** | 1.01 |
| Half-Life (fps of the level) | 10.1 | 13.2 | 11.8 | **1.31** | 1.17 |
| nxray (pictures a second) | 24.2 | 37.1 | 21.2 | **1.53** | 0.88 |
| Doom's timedemo (tics a second; realtics 1190 / 935 / 970) | 147.8 | 188.1 | 181.4 | **1.27** | 1.23 |
| Quake's timedemo | 95.5 | 97.5 | 96.8 | **1.02** | 1.01 |

## Everything else (seconds of the guest's clock; lower is better)

| program | what | RISC-V | v0009 | x23 | v0009 / RISC-V | x23 / RISC-V |
|---|---|---|---|---|---|---|
| CoreMark | `coremark 0x0 0x0 0x66 300` (its own figure: 22.4 / 43.0 / 47.2 iterations/s) | 14.16 | 6.80 | 7.39 | **2.08** | 1.92 |
| gzip | minigzip of a 925 KB file | 26.87 | 15.03 | 16.00 | **1.79** | 1.68 |
| gunzip | minigzip -d of it, md5 | 5.89 | 3.71 | 3.97 | **1.59** | 1.48 |
| QuickJS | `pi.js 2000` | 10.55 | 8.12 | 8.43 | **1.30** | 1.25 |
| MicroPython | a sum over 30,000 squares | 7.32 | 5.95 | 6.69 | **1.23** | 1.09 |
| libmedia, sound | FLAC, MP3 and MOD decoded, twice each | 79.50 | 51.94 | 60.13 | **1.53** | 1.32 |
| libmedia, film | `nxvideo` of both films (sum 3129e37f) | 60.54 | 38.42 | 39.01 | **1.58** | 1.55 |
| unarc | a zip and a .tar.gz unpacked | 9.47 | 7.61 | 9.06 | **1.24** | 1.05 |
| nxpath | the default mesh, 160 x 120, 2 samples | 3.91 | 3.17 | 3.81 | **1.23** | 1.03 |
| nxpath cornell | 120 x 120 | 2.47 | 2.17 | 3.01 | **1.14** | 0.82 |
| nxpath cornell-balls | 120 x 120 | 2.60 | 2.27 | 3.11 | **1.15** | 0.84 |
| nxpath balls | | 2.65 | 2.28 | 2.93 | **1.16** | 0.90 |
| nxpath materials | | 6.96 | 5.02 | 5.77 | **1.39** | 1.21 |
| nxpath beer | | 2.42 | 2.30 | 2.85 | **1.05** | 0.85 |
| nxpath mirrors | | 4.21 | 3.20 | 4.11 | **1.32** | 1.02 |
| nxpath glow | | 6.84 | 4.89 | 5.79 | **1.40** | 1.18 |
| nxphys wall | 120 steps (sum e57a98cc) | 2.72 | 2.38 | 2.73 | **1.14** | 1.00 |
| nxphys tower | 13 bodies, 300 steps (sum 0b24fc24) | 1.70 | 1.95 | 2.57 | **0.87** | 0.66 |
| NES, Thwaite | 600 frames, no sound | 2.84 | 3.57 | 4.07 | **0.80** | 0.70 |
| NES, Nova | 600 frames, no sound | 2.65 | 2.81 | 3.13 | **0.94** | 0.85 |
| ClassiCube | start to its steady line | 13.07 | 11.67 | 13.28 | **1.12** | 0.98 |
| Micropolis | three cities simulated (sums 909517ac e14d1a50 75e86f73) | 111.33 | 48.08 | 50.33 | **2.32** | 2.21 |
| Tiberian Dawn | 1300 frames, attract mode | 34.33 | 30.47 | 34.32 | **1.13** | 1.00 |
| Red Alert | 1500 frames, attract mode | 80.89 | 80.90 | 84.39 | **1.00** | 0.96 |
| Quake, light check | `QUAKE_LIGHTPOINT=check` timedemo, load included (its fps 60.4 / 64.7 / 60.0; 0 not as made) | 40.83 | 31.50 | 33.68 | **1.30** | 1.21 |
| HLSL | `hlsl_bench` of the fractal (sum a4b289dd) | 3.10 | 18.76 | 17.95 | **0.17** | 0.17 |
| nxplay | four files with `NXPLAY_SUM=1` | not run | not run | not run | | |
| idle | busy CPU seconds in 4 s, nothing running | 0.27 | 0.43 | 0.49 | | |

The HLSL compiler has no gpu_vm back end yet: it reports 0 bytes of code and runs the shader interpreted, with no
lanes. The 6x is that, not the machine. nxplay's command did not end within the 300 s limit on any machine (its
logs are in `wide-b`), so it has no row.

## A program's start (busy CPU seconds in 4 s, less an idle machine's; one round)

| program | RISC-V | v0009 | x23 | v0009 / RISC-V | x23 / RISC-V |
|---|---|---|---|---|---|
| nxterm | 0.14 | 0.56 | 0.64 | **0.25** | 0.22 |
| nxedit | 0.09 | 0.35 | 0.37 | **0.26** | 0.24 |
| nxfiles | 0.10 | 0.79 | 0.94 | **0.13** | 0.11 |
| nxcalc, nxmon, nxsettings, nxide | not run | not run | not run | | |

A start costs gpu_vm 4 to 8 times RISC-V's CPU time by this measure. The idle machine is busier too (0.43 s against
0.27 in 4 s). Both are mostly the measure and romfs (below, "Where a start went").

### Where a start went (24 October 2026)

- **The measure undercounts RISC-V.** Its kernel's cpu line adds up to less than the time that passed: in the window of
  nxterm's start, 421 jiffies of 4.71 s, and an idle window 427 of 4.58 s. gpu_vm's add up (458 of 4.58 s). The time
  lost is busy time: a tick that comes late counts one jiffy for several. Uptime less idle time is the fair measure,
  from the same logs:

  | uptime less idle, s | RISC-V | v0009 before | v0009 after |
  |---|---|---|---|
  | idle, 4 s | 0.58 | 0.40 | 0.40 |
  | nxterm's start | 0.33 | 0.52 | **0.32** |
  | nxedit's start | 0.17 | 0.33 | **0.16** |
  | nxfiles's start | 0.22 | 0.79 | **0.22** |

  RISC-V's figures are from `wide-b`, which ran from a checkpoint. gpu_vm's are new: two interleaved rounds booted fresh
  on the card (`nano-X -p & sleep 2` first), with the before and after kernels on img-cur's ROM. Each start is less the
  idle line.
- **Counted, the work was already even.** On `gvm_cpu` (32 cores, `--budget 4096 --wc 16,4`, v0009's write cache) and
  `rvc_cpu`, core 0 over the start's 4 s less an idle 4 s: nxterm 1.41 M bundles against 1.51 M instructions.
- **Half of gpu_vm's start was romfs through MTD.** Each lookup walked its directory an entry at a time, every entry
  two or three reads through `mtd_read`, `mtd_read_oob`, `mtd_check_oob_ops` and `phram_read`, about 150 bundles each.
  nxfiles made 11 lookups, 4,475 such reads and 0.61 M bundles: 43% of its start, 22% of nxterm's. The RISC-V kernel
  has done these where they lie in the ROM since `linux/kernel/romfs_hook.py`. gpu_vm's `drivers.py` now applies that
  hook as well.
- **The hook's effect, counted (bundles less idle):** nxterm 1.22 M to 0.82 M, nxedit 0.83 M to 0.44 M, nxfiles 1.41 M
  to 0.51 M. Core 0's passes in nxfiles's window went from 1,131 to 832, not counting waits.
- **What is left of a start:**
  - The program's own code, 45-55% of it.
  - `alloc_pages_run`, which scans the page structures from the zone's bottom: 35 k bundles a call, twice for a
    start. This is the same code as RISC-V's.
  - slob, at 6-9%.
- **What is left of the idle machine:** core 0 ends a pass with its write cache full at every timer tick, in
  `timekeeping_update`'s copy of the timekeeper. That is 100 extra passes a second on every load: 480 of the idle 4 s's
  929 passes end full. The RISC-V cache keeps 336 texels; v0009's keeps 128.

## What stands out

- **gpu_vm v0009 is faster on most programs:**
  - Micropolis 2.3x, CoreMark 2.1x, gzip 1.8x.
  - libmedia 1.5-1.6x, Quake III 1.46x, nxray 1.53x, Half-Life 1.31x, Doom 1.27x.
  - The interpreters and the path tracer 1.05-1.4x.
- **gpu_vm is slower** on the NES (0.80 and 0.94), nxphys's tower (0.87) and the HLSL bench (no back end). Red Alert
  and Quake's timedemo are even. Program starts measured 0.13-0.26. Since the romfs hook, and measured as uptime less
  idle, they are even with RISC-V ("Where a start went").
- **x23 trails v0009 everywhere.** It is below RISC-V on nxray, on most nxpath scenes and on ClassiCube.
- **Missing from this table:** nxplay (did not finish), four of the seven starts, and a second round of the games and
  of `wide-b`'s programs (cut).
