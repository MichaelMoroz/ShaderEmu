# The RISC-V machine, machine against machine, every application on the card

`tools/rv_compare/` (GPU-VM's `compare/` with RISC-V alone). Every part runs on the card (RTX 5070 Laptop, D3D11, fxc2)
as a VRChat world runs it: `rvc_harness --unity 11.1,1.4,0.75`, the guest's clock 11.1 ms a host frame (90 Hz), as many
rounds (a tick of 8,192 instructions and its commit) a frame as fit 9.7 ms of the card, each round costing at least
0.75 ms (Unity's CPU a round in RISC-V's world). Under it the guest's clock is the world's, so every time and rate
below is what a player in the world sees.

- **The start of every part**: the machine's shell snapshot with `nano-X -p` up, made on the processor
  (`cmp.py shell MACHINE`). The command is typed after `echo GO`; times are from that line.
- **How it is measured**: the console's lines and the count of GPU lists drawn are logged by host frame
  (`RVC_LINE_LOG`, `RVC_FRAMES_LOG`). "start to first frame" is GO to the first list drawn; "frames" are lists drawn
  a second (90 is the ceiling: one a host frame); other numbers are the programs' own prints, on the guest's clock.
  A run longer than 33 s of the guest goes on in sections of under a minute, saved and loaded.
- **Frame pacing** (every part that draws): the gaps between frames drawn over the second half of the run (or from
  the part's own mark), median, 95th and 99th percentile in ms, and hitches (a gap over twice the median) a second.
- **mean**: of the rounds run (each round a whole run of the part); **spread**: (largest - smallest) / mean over the
  rounds, in percent. **ratio**: how many times better B is than A (A / B for a time, B / A for a rate): above 1 B
  is better; 3% either way is level.
- **card ms**: the card's time a host frame, the run's mean (the budget is 9.7); **rounds**: rounds a frame.
  **ratio at the budget**: the ratio with each side's card ms a frame brought to the budget (A's card ms over B's
  times the ratio), as GPU-VM's `grid.py`: a machine whose rounds overshot the budget is not paid for it.
- Machines: `tools/rv_compare/machines.py`; `before` is frozen in the backport's `build/before` (its README.md).

**Parts measured: before 51, after 51 of 51. after better than before in 121 metrics, worse in 46, level in 75 (3% either way).**

- Run-to-run spread on before over 255 metrics with two rounds or more: median 1.5%, 75th percentile 11.1%, largest 221.1%.
- Run-to-run spread on after over 243 metrics with two rounds or more: median 1.1%, 75th percentile 6.5%, largest 300.0%.
- Rounds left out of the means (8, `spoiled.txt`): before quake-server round 1; before quake3-server round 1; before quake3-menu round 1; before halflife round 1; before classicube round 1; before tdawn round 1; before ralert round 1; before nes round 1.

## Before (28 October 2026)

The machine at riscv-backport's base, 7624f55, frozen in `ShaderEmu-rvbp/build/before` (its README.md): the harness
built from 7624f55 with the backend half of `--unity` (7624f55 alone does not build), `experiments/rvc_opt` of 7624f55,
and an image made of 7624f55's `linux/prebuilt` with Uplink's data. Every one of the 51 parts ran in two rounds, the
second with the parts in the other order (`run.py --machines before --rounds 2`, 06:54 to 08:52); 8 parts ran a third
time (below). Host: two Unity editors open and idle, no developer's harness, CPU load about 1%; another session's
`rvc_cpu` runs took the harness lock now and then (156 refusals, each waited out and run again).

**A slower card spoils a round.** From about 08:15 to 08:50 the card took up to 1.75x as long a round: the rounds a frame
fell at the same card ms a frame (Red Alert 7.7 to 4.4, Tiberian Dawn 8.3 to 5.1, the NES 7.6 to 4.7, Quake III's menu
5.2 to 3.9), and the guest ran that much slower (Red Alert's mission 20.7 s to 33.1, Quake III's menu 75 fps to 42). The
eight parts in that window ran again as round 2 (08:52 to 09:02), which agrees with round 0 within the usual spread;
their round 1 is in `spoiled.txt` and out of the means. The cause was not found (no other harness held the card).
**Compare a part's rounds a frame between runs before its numbers**: a change of more than about 8% at the same card ms
is the card, not the machine.

Run-to-run spread (largest less smallest over the mean, the rounds kept): the programs' own metrics median 1.4%,
75th percentile 3.9%, 90th 10.4%; the pacing metrics (frame time percentiles and hitches, which move in steps of 11.1
ms) median 0, 90th 28%. Steady: the timed benchmarks (cc, qjs, micropython, gzip, the kernel's copies, decode-avi,
nxpath, nxray, video) within 0.5%. Noisy: a window's time from its Start-menu key (Dear ImGui 310 / 66 ms, the Music
player 300 / 133), OpenTTD's first frame (5.0 / 3.7 s), Pinball's frames (23.1 / 20.0), Tiberian Dawn's game frames
(65.7 / 59.1), Quake III's server jobs (10-14%): judge these by several rounds.

| part | metric | unit | before: mean | each round | card ms a frame (ms) | rounds a frame (rounds) |
|---|---|---|---|---|---|---|
| doom | timedemo tics a second | tics/s | 91.62 | 91.54, 91.69 | 9.29 | 9.75 |
| quake | lists drawn in the level | fps | 89.44 | 89.53, 89.35 | 9.48 | 6.90 |
| quake | start to the level | s | 20.08 | 19.89, 20.26 | 9.48 | 6.90 |
| quake-timedemo | timedemo | fps | 68.00 | 69.40, 66.60 | 9.84 | 7.40 |
| quake-server | server ticks | ticks/s | 56.73 | 58.04, 55.43 | 9.50 | 7.15 |
| quake3 | frames in the level | fps | 64.50 | 65.10, 63.89 | 9.04 | 5.60 |
| quake3 | start to the level | s | 33.48 | 33.11, 33.84 | 9.04 | 5.60 |
| quake3-server | the world moves (a server job taken) every | ms | 98.95 | 103, 94.60 | 8.82 | 5.15 |
| halflife | frames in the level | fps | 16.09 | 16.59, 15.58 | 9.39 | 6.90 |
| halflife | start to the level drawn | s | 81.46 | 80.64, 82.28 | 9.39 | 6.90 |
| classicube | frames when steady | fps | 86.29 | 86.91, 85.67 | 9.02 | 6.05 |
| tdawn | game frames | frames/s | 62.40 | 65.70, 59.10 | 9.79 | 8.25 |
| ralert | mission start to frame 1300 | s | 20.77 | 20.69, 20.85 | 9.41 | 7.65 |
| nes | the NES's speed | % | 502 | 511, 492 | 9.33 | 7.50 |
| nxphys | 600 substeps | s | 6.96 | 6.71, 7.21 | 9.49 | 9.50 |
| pinball | frames drawn | fps | 21.52 | 23.08, 19.96 | 9.38 | 6.70 |
| micropolis | start to the end | s | 54.72 | 54.58, 54.86 | 9.04 | 7.15 |
| openttd | start to the game | s | 102 | 103, 100 | 9.25 | 6.80 |
| openttd | scroll | pictures/s | 2.71 | 2.76, 2.67 | 9.25 | 6.80 |
| openttd-title | title frames | fps | 26.18 | 25.75, 26.60 | 9.45 | 7.50 |
| video-mpg | frames drawn | fps | 19.41 | 19.41, 19.41 | 9.08 | 8.25 |
| decode-avi | decode | s | 36.55 | 36.54, 36.56 | 9.17 | 7.60 |
| nxray | pictures | pictures/s | 19.19 | 19.19, 19.20 | 8.55 | 3.10 |
| nxpath | the picture | ms | 144 | 144, 144 | 9.58 | 8.80 |
| nxweb | start to the page laid out | s | 0.21 | 0.21, 0.21 | 10.17 | 9.80 |
| desktop | Calculator window shown after | ms | 144 | 144, 144 | 9.28 | 10.10 |
| memory | memcpy-1MB | MB/s | 5.72 | 5.63, 5.81 | 9.07 | 9.10 |
| memory-kernel | /dev/zero to /dev/null, 32 MB | s | 7.73 | 7.73, 7.73 | 9.08 | 11.55 |
| memory-kernel | into /tmp, 8 MB | s | 3.96 | 3.97, 3.96 | 9.08 | 11.55 |
| cc | compile | s | 5.75 | 5.74, 5.76 | 9.20 | 7.70 |
| qjs | run | s | 0.77 | 0.77, 0.77 | 9.62 | 9.60 |
| python | run | s | 5.25 | 5.25, 5.25 | 9.15 | 7.50 |
| gzip | gzip three times | s | 27.12 | 27.13, 27.11 | 9.24 | 7.20 |
| glxgears | lists drawn from 3 s | fps | 44.34 | 44.03, 44.64 | 9.33 | 8.85 |

Against GPU-VM's comparison of 26 October (RISC-V on `nommu_hl`, an older image): Doom 91.6 tics/s against 91.3-91.4,
nxray 19.19 pictures/s against 19.17-19.21, Quake III's level 64.5 fps against 64.7-66.5. The tool measures what that
comparison measured.

## After (28 October 2026)

The machine at riscv-backport's end, 133e575, frozen in `ShaderEmu-rvbp/build/after` (its README.md): the harness and
tick of c135015 (unchanged to 133e575 but for `rvc_cpu`), and the image of 133e575's `linux/prebuilt` with Uplink's data.
Every part ran three rounds (`run.py --machines after --rounds 3`, 19:50 to 21:14), none spoiled (the rounds a frame
of each part are within the usual spread of before's at the same card ms).

Two "after" images were measured and dropped on the way (`build/after/build/images/nommu.v1`, `nommu.v2`, their rows in
`build/rv_compare/results_v1.jsonl` and `results_v2.jsonl`), each for a regression found and fixed:

- Quake III's server, 20.2 game frames a second to 10.7: `mcwx`'s calls to the lowest free worker (830fbdb, gpu_vm's,
  25 October) put the server's job and the client's helpers on one core. RISC-V goes round the row again (19561b9);
  gpu_vm keeps the lowest.
- Red Alert's film, 12.8 s to 13.9: since 477bcc2 (21 October, lanes) a worker whose team job lingered out was posted
  again only in the round's wait, one worker a pass, and the film's frames are 66 ms apart. It is posted with the round
  now (4c405fe; the film 10.8 s).

Also fixed: memset's fill by the commit still had the layout before RAM in one piece (2ecf76d), and the harness held
the lanes' field 8 rounds, not 8 frames, under `--unity` (2ecf76d). memcpy's commit copy starts at 4 KB, not 2
(366d9fe: on the card a copy of 2 KB by words was faster).

**What is worse, and why** (ratio after x before; spreads in percent, before / after):

| part | metric | before | after | ratio | why |
|---|---|---|---|---|---|
| pinball | frames drawn (fps) | 22.10 | 3.88 | 0.18 | the demo runs flat out and never yields: with the new libm it plays a different (chaotic) game, and the game itself is faster (start to frame 1800: 12.11 s to 9.62) |
| desktop apps | lists drawn from 3 s (fps) | 1.8 to 2.9 | 1.5 to 2.6 | 0.84 to 0.95 | fewer idle redraws; their windows open sooner (Network 137 ms to 89, Monitor 318 to 226) |
| memory | memcpy-8KB (MB/s) | 9.35 | 7.62 | 0.81 | a hot cache copied back to back: the commit's copy loses to words there; from 16 KB and over many sources it wins (memcpy-1MB 5.75 to 160) |
| memory | load-seq-word (MB/s) | 3.49 | 3.24 | 0.93 | a loop compiled by `cc` in the machine; its store twin is 1.13 better; not explained |
| quake-server | client frames drawn behind the older server answer (%) | 36.5 | 45.5 | 0.80 | the server ticks faster (56.7 to 61.7 a second): more answers arrive behind a frame |
| quake3-server | game frames a job, a job posted to taken (ms), instructions a job | 2.00, 90, 261 k | 2.32, 99, 298 k | 0.86 to 0.91 | within the spreads (11 to 14%) on both sides; its rate, server game frames a second, is 1.07 better |
| quake3-menu | menu frames (fps) | 74.86 | 72.46 | 0.97 | not explained: the same rounds a frame, no pass ended by a copy or fill in its window (`rvc_cpu`, `prof_parts.py`); memcpy and memset under 1.5% of core 0 |
| video-mpg | frames drawn (fps) | 19.40 | 19.08 | 0.98 | the card fit 1.6% fewer rounds a frame (8.23 to 8.10); the guest's copies cost 0.5% of core 0 |

Tiberian Dawn's "game frames" reads only the last short `tdstat` window (spread 11% on both sides): its "start to frame
1300" is the measure.

    python tools/rv_compare/cmp.py shell after                       # its own shell snapshot (RAM's layout differs)
    python tools/rv_compare/run.py --machines after --rounds 3
    python tools/rv_compare/report.py before after

`machines.py`'s `after` is the backport's checkout (`bin
vc_harness.exe`, `build/images/nommu`, `experiments/rvc_opt`);
`RVCMP_AFTER_ROOT` and `RVCMP_AFTER_IMAGE` move it (here `RVCMP_AFTER_ROOT=ShaderEmu-rvbp/build/after`).

## Every metric

| group | program and part | metric | unit | better | before: mean | before: each round | before: spread (%) | after: mean | after: each round | after: spread (%) | ratio (after x before) | ratio at the budget (after x before) | card ms a frame before / after (ms) | rounds a frame before / after (rounds) | runs before / after (guest s, sections) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| game | doom: Doom: timedemo demo1 (as fast as it goes) | start to first frame | s | lower | 1.45 | 1.45, 1.47, 1.42 | 3.46 | 1.19 | 1.22, 1.17, 1.17 | 4.21 | 1.22 | 1.22 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | timedemo tics a second | tics/s | higher | 91.57 | 91.54, 91.69, 91.48 | 0.23 | 92.17 | 92.19, 92.08, 92.25 | 0.18 | 1.01 | 1.00 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frames drawn | fps | higher | 89.34 | 89.47, 89.23, 89.32 | 0.27 | 89.61 | 89.63, 89.63, 89.57 | 0.07 | 1.00 | 1.00 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frame time median | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frame time p95 | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frame time p99 | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | hitches a second | hitches/s | lower | 0.10 | 0.10, 0.10, 0.10 | 0.00 | 0.05 | 0.05, 0.05, 0.05 | 0.00 | 2.00 | 2.00 | 9.28 / 9.30 | 9.77 / 10.43 | 41.1/2, 41.1/2, 41.1/2 / 41.1/2, 41.1/2, 41.1/2 |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | start to first frame | s | lower | 1.47 | 1.45, 1.48, 1.47 | 2.05 | 1.18 | 1.18, 1.19, 1.17 | 1.69 | 1.24 | 1.24 | 9.38 / 9.38 | 10.47 / 10.87 | 16.6/1, 16.6/1, 16.6/1 / 16.6/1, 16.6/1, 16.6/1 |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frames drawn | fps | higher | 29.85 | 30.03, 29.67, 29.85 | 1.21 | 31.43 | 31.46, 31.46, 31.37 | 0.29 | 1.05 | 1.05 | 9.38 / 9.38 | 10.47 / 10.87 | 16.6/1, 16.6/1, 16.6/1 / 16.6/1, 16.6/1, 16.6/1 |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frame time median | ms | lower | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 1.00 | 1.00 | 9.38 / 9.38 | 10.47 / 10.87 | 16.6/1, 16.6/1, 16.6/1 / 16.6/1, 16.6/1, 16.6/1 |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frame time p95 | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 1.00 | 9.38 / 9.38 | 10.47 / 10.87 | 16.6/1, 16.6/1, 16.6/1 / 16.6/1, 16.6/1, 16.6/1 |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frame time p99 | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 1.00 | 9.38 / 9.38 | 10.47 / 10.87 | 16.6/1, 16.6/1, 16.6/1 / 16.6/1, 16.6/1, 16.6/1 |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.38 / 9.38 | 10.47 / 10.87 | 16.6/1, 16.6/1, 16.6/1 / 16.6/1, 16.6/1, 16.6/1 |
| game | quake: Quake: e1m1 loaded and played (standing) | start to first frame | s | lower | 0.82 | 0.82, 0.81, 0.82 | 1.22 | 0.49 | 0.49, 0.48, 0.49 | 2.05 | 1.68 | 1.68 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | start to the level | s | lower | 20.01 | 19.89, 20.26, 19.89 | 1.85 | 16.52 | 16.55, 16.53, 16.49 | 0.36 | 1.21 | 1.21 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | server ticks in the level (QUAKE_TRACK) | ticks/s | higher | 53.08 | 51.20, 55.47, 52.57 | 8.04 | 56.95 | 58.26, 55.49, 57.11 | 4.86 | 1.07 | 1.08 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | lists drawn in the level | fps | higher | 89.52 | 89.53, 89.35, 89.69 | 0.38 | 89.61 | 89.60, 89.67, 89.56 | 0.12 | 1.00 | 1.00 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | server's game time against the world's | % | higher | 100 | 100, 100, 100 | 0.10 | 100 | 100, 100, 100 | 0.10 | 1.00 | 1.00 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | frame time median | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | frame time p95 | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | frame time p99 | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake: Quake: e1m1 loaded and played (standing) | hitches a second | hitches/s | lower | 0.03 | 0.00, 0.05, 0.04 | 167 | 0.02 | 0.00, 0.00, 0.05 | 300 | 1.80 | 1.80 | 9.50 / 9.48 | 6.97 / 6.97 | 44.1/2, 42.8/2, 43.6/2 / 38.1/2, 39.1/2, 38.4/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | start to the level | s | lower | 20.03 | 19.84, 20.22 | 1.90 | 16.46 | 16.49, 16.45, 16.45 | 0.24 | 1.22 | 1.22 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | server ticks | ticks/s | higher | 56.73 | 58.04, 55.43 | 4.60 | 63.06 | 62.58, 63.69, 62.92 | 1.76 | 1.11 | 1.12 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | client frames | fps | higher | 87.52 | 87.68, 87.35 | 0.38 | 88.06 | 88.13, 87.98, 88.07 | 0.17 | 1.01 | 1.01 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | client frames drawn behind the older server answer | % | lower | 36.51 | 37.37, 35.65 | 4.71 | 45.46 | 44.37, 47.65, 44.37 | 7.21 | 0.80 | 0.81 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | server instructions a tick | k | lower | 48.61 | 48.36, 48.86 | 1.03 | 45.48 | 45.62, 45.56, 45.25 | 0.82 | 1.07 | 1.07 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | frame time p95 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | frame time p99 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | hitches a second | hitches/s | lower | 0.05 | 0.03, 0.07 | 80.00 | 0.04 | 0.03, 0.03, 0.06 | 75.00 | 1.25 | 1.26 | 9.50 / 9.46 | 7.15 / 7.40 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | quake-timedemo: Quake: timedemo demo1 | start to first frame | s | lower | 0.82 | 0.81, 0.82, 0.82 | 1.22 | 0.49 | 0.49, 0.49, 0.49 | 0.00 | 1.67 | 1.76 | 9.69 / 9.20 | 7.43 / 6.90 | 32.8/1, 33.8/2, 33.0/1 / 28.9/1, 28.8/1, 29.1/1 |
| game | quake-timedemo: Quake: timedemo demo1 | timedemo | fps | higher | 68.33 | 69.40, 66.60, 69.00 | 4.10 | 72.63 | 72.70, 73.20, 72.00 | 1.65 | 1.06 | 1.12 | 9.69 / 9.20 | 7.43 / 6.90 | 32.8/1, 33.8/2, 33.0/1 / 28.9/1, 28.8/1, 29.1/1 |
| game | quake-timedemo: Quake: timedemo demo1 | frame time median | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.05 | 9.69 / 9.20 | 7.43 / 6.90 | 32.8/1, 33.8/2, 33.0/1 / 28.9/1, 28.8/1, 29.1/1 |
| game | quake-timedemo: Quake: timedemo demo1 | frame time p95 | ms | lower | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.05 | 9.69 / 9.20 | 7.43 / 6.90 | 32.8/1, 33.8/2, 33.0/1 / 28.9/1, 28.8/1, 29.1/1 |
| game | quake-timedemo: Quake: timedemo demo1 | frame time p99 | ms | lower | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 1.00 | 1.05 | 9.69 / 9.20 | 7.43 / 6.90 | 32.8/1, 33.8/2, 33.0/1 / 28.9/1, 28.8/1, 29.1/1 |
| game | quake-timedemo: Quake: timedemo demo1 | hitches a second | hitches/s | lower | 2.20 | 2.68, 1.75, 2.17 | 42.27 | 1.18 | 1.13, 1.28, 1.13 | 12.71 | 1.86 | 1.96 | 9.69 / 9.20 | 7.43 / 6.90 | 32.8/1, 33.8/2, 33.0/1 / 28.9/1, 28.8/1, 29.1/1 |
| game | quake3: Quake III: q3dm17 with a bot | start to first frame | s | lower | 1.65 | 1.64, 1.65, 1.65 | 0.61 | 0.95 | 0.94, 0.95, 0.95 | 1.06 | 1.74 | 1.75 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | start to the level | s | lower | 33.44 | 33.11, 33.84, 33.38 | 2.18 | 27.81 | 27.71, 27.84, 27.88 | 0.61 | 1.20 | 1.21 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | frames in the level | fps | higher | 64.95 | 65.10, 63.89, 65.86 | 3.03 | 67.01 | 67.54, 65.75, 67.74 | 2.97 | 1.03 | 1.04 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | lists drawn in the level | fps | higher | 60.91 | 61.35, 59.75, 61.64 | 3.10 | 62.52 | 62.77, 61.71, 63.07 | 2.18 | 1.03 | 1.04 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | frame time median | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.01 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | frame time p95 | ms | lower | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.01 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | frame time p99 | ms | lower | 37.00 | 33.30, 33.30, 44.40 | 30.00 | 40.70 | 33.30, 44.40, 44.40 | 27.27 | 0.91 | 0.92 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3: Quake III: q3dm17 with a bot | hitches a second | hitches/s | lower | 2.31 | 2.15, 2.41, 2.37 | 11.26 | 1.93 | 2.19, 1.40, 2.21 | 41.90 | 1.19 | 1.21 | 9.05 / 8.97 | 5.63 / 5.53 | 44.9/2, 45.8/2, 45.1/2 / 39.4/2, 39.7/2, 39.6/2 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | server game frames (sv_fps 20) | frames/s | higher | 20.18 | 20.67, 19.68 | 4.91 | 21.67 | 21.94, 21.87, 21.20 | 3.41 | 1.07 | 1.07 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | the world moves (a server job taken) every | ms | lower | 98.95 | 103, 94.60 | 8.79 | 107 | 111, 99.90, 110 | 10.36 | 0.92 | 0.92 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | a server job, posted to taken | ms | lower | 90.10 | 95.00, 85.20 | 10.88 | 99.40 | 104, 92.50, 102 | 11.17 | 0.91 | 0.90 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | game frames a job | frames | lower | 2.00 | 2.14, 1.87 | 13.47 | 2.32 | 2.44, 2.19, 2.34 | 10.68 | 0.86 | 0.86 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | the server's worker while it runs | M instructions/s | higher | 3.12 | 3.15, 3.08 | 2.25 | 3.21 | 3.23, 3.19, 3.20 | 1.25 | 1.03 | 1.03 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | server crashes |  | lower | 0.00 | 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | instructions a job | k | lower | 261 | 280, 243 | 14.25 | 298 | 314, 275, 305 | 13.05 | 0.88 | 0.87 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.00 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | frame time p99 | ms | lower | 38.85 | 33.30, 44.40 | 28.57 | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 1.17 | 1.16 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | hitches a second | hitches/s | lower | 1.71 | 1.53, 1.88 | 20.53 | 1.30 | 1.20, 1.16, 1.53 | 28.53 | 1.31 | 1.31 | 8.82 / 8.85 | 5.15 / 5.23 | 99.9/3, 99.9/3 / 99.9/3, 99.9/3, 99.9/3 |
| game | quake3-menu: Quake III: its main menu | start to first frame | s | lower | 1.65 | 1.63, 1.68 | 3.02 | 0.93 | 0.95, 0.95, 0.89 | 6.45 | 1.78 | 1.81 | 8.51 / 8.35 | 5.15 / 5.20 | 59.9/2, 59.9/2 / 59.9/2, 59.9/2, 59.9/2 |
| game | quake3-menu: Quake III: its main menu | menu frames (from 30 s) | fps | higher | 74.86 | 75.08, 74.64 | 0.59 | 72.46 | 72.46, 72.43, 72.50 | 0.10 | 0.97 | 0.99 | 8.51 / 8.35 | 5.15 / 5.20 | 59.9/2, 59.9/2 / 59.9/2, 59.9/2, 59.9/2 |
| game | quake3-menu: Quake III: its main menu | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.02 | 8.51 / 8.35 | 5.15 / 5.20 | 59.9/2, 59.9/2 / 59.9/2, 59.9/2, 59.9/2 |
| game | quake3-menu: Quake III: its main menu | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.02 | 8.51 / 8.35 | 5.15 / 5.20 | 59.9/2, 59.9/2 / 59.9/2, 59.9/2, 59.9/2 |
| game | quake3-menu: Quake III: its main menu | frame time p99 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.02 | 8.51 / 8.35 | 5.15 / 5.20 | 59.9/2, 59.9/2 / 59.9/2, 59.9/2, 59.9/2 |
| game | quake3-menu: Quake III: its main menu | hitches a second | hitches/s | lower | 0.13 | 0.13, 0.13 | 0.00 | 0.12 | 0.07, 0.20, 0.10 | 105 | 1.05 | 1.07 | 8.51 / 8.35 | 5.15 / 5.20 | 59.9/2, 59.9/2 / 59.9/2, 59.9/2, 59.9/2 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | start to first frame | s | lower | 3.39 | 3.39, 3.39 | 0.00 | 1.26 | 1.25, 1.25, 1.27 | 1.59 | 2.70 | 2.69 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | start to the level drawn | s | lower | 81.46 | 80.64, 82.28 | 2.01 | 69.29 | 69.31, 69.42, 69.14 | 0.40 | 1.18 | 1.17 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frames in the level | fps | higher | 16.09 | 16.59, 15.58 | 6.28 | 16.12 | 16.39, 16.16, 15.81 | 3.60 | 1.00 | 1.00 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | start to second 31 of the level | s | lower | 111 | 110, 112 | 1.53 | 98.52 | 98.56, 98.62, 98.38 | 0.24 | 1.13 | 1.12 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frame time median | ms | lower | 139 | 133, 144 | 8.00 | 155 | 155, 155, 155 | 0.00 | 0.89 | 0.89 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frame time p95 | ms | lower | 222 | 222, 222 | 0.00 | 233 | 233, 233, 233 | 0.00 | 0.95 | 0.95 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frame time p99 | ms | lower | 239 | 233, 244 | 4.65 | 248 | 255, 244, 244 | 4.48 | 0.96 | 0.96 | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.39 / 9.43 | 6.90 / 7.10 | 128.2/4, 131.0/4 / 117.1/4, 117.9/4, 117.6/4 |
| game | classicube: ClassiCube: a new world, standing | start to first frame | s | lower | 0.68 | 0.67, 0.68 | 1.48 | 0.31 | 0.34, 0.29, 0.29 | 16.30 | 2.20 | 2.24 | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | classicube: ClassiCube: a new world, standing | start to steady | s | lower | 8.56 | 8.42, 8.70 | 3.27 | 7.94 | 7.99, 7.93, 7.90 | 1.13 | 1.08 | 1.10 | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | classicube: ClassiCube: a new world, standing | frames when steady | fps | higher | 86.29 | 86.91, 85.67 | 1.44 | 85.34 | 85.09, 85.62, 85.31 | 0.62 | 0.99 | 1.01 | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | classicube: ClassiCube: a new world, standing | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.02 | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | classicube: ClassiCube: a new world, standing | frame time p95 | ms | lower | 16.65 | 11.10, 22.20 | 66.67 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 0.75 | 0.76 | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | classicube: ClassiCube: a new world, standing | frame time p99 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.02 | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | classicube: ClassiCube: a new world, standing | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | 0.01 | 0.00, 0.03, 0.00 | 300 | - | - | 9.02 / 8.87 | 6.05 / 5.90 | 50.0/2, 50.0/2 / 50.0/2, 50.0/2, 50.0/2 |
| game | tdawn-menu: Tiberian Dawn: its start and menu | start to first frame | s | lower | 1.08 | 1.08, 1.08, 1.08 | 0.00 | 0.48 | 0.48, 0.48, 0.49 | 2.07 | 2.23 | 2.14 | 8.65 / 9.03 | 6.07 / 11.60 | 40.0/2, 40.0/2, 40.0/2 / 40.0/2, 40.0/2, 40.0/2 |
| game | tdawn-menu: Tiberian Dawn: its start and menu | start to the menu (its last list; it holds still) | s | lower | 32.70 | 32.70, 32.70, 32.70 | 0.00 | 32.67 | 32.67, 32.67, 32.67 | 0.00 | 1.00 | 0.96 | 8.65 / 9.03 | 6.07 / 11.60 | 40.0/2, 40.0/2, 40.0/2 / 40.0/2, 40.0/2, 40.0/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | start to first frame | s | lower | 1.08 | 1.07, 1.08 | 0.93 | 0.49 | 0.49, 0.49, 0.49 | 0.00 | 2.19 | 2.21 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | game frames | frames/s | higher | 62.40 | 65.70, 59.10 | 10.58 | 71.50 | 70.00, 68.40, 76.10 | 10.77 | 1.15 | 1.15 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | start to frame 1300 | s | lower | 40.20 | 40.06, 40.35 | 0.72 | 38.68 | 38.73, 38.82, 38.48 | 0.88 | 1.04 | 1.05 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | frame time median | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 1.00 | 1.01 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | frame time p95 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 1.01 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | frame time p99 | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 1.00 | 1.01 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | hitches a second | hitches/s | lower | 0.70 | 0.65, 0.74 | 12.95 | 0.64 | 0.62, 0.51, 0.78 | 42.41 | 1.09 | 1.10 | 9.79 / 9.73 | 8.25 / 8.80 | 42.4/2, 42.7/2 / 41.3/2, 41.4/2, 41.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | start to first frame | s | lower | 1.08 | 1.04, 1.11 | 6.51 | 0.52 | 0.52, 0.52, 0.52 | 0.00 | 2.07 | 2.07 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | film (154 or 156 frames shown) | s | lower | 12.83 | 12.80, 12.87 | 0.51 | 10.84 | 10.69, 11.04, 10.79 | 3.27 | 1.18 | 1.18 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | start to the mission | s | lower | 36.13 | 35.89, 36.37 | 1.33 | 31.47 | 31.22, 31.73, 31.47 | 1.62 | 1.15 | 1.15 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | mission start to frame 1300 | s | lower | 20.77 | 20.69, 20.85 | 0.77 | 20.52 | 20.55, 20.44, 20.57 | 0.63 | 1.01 | 1.01 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | 25.90 | 22.20, 33.30, 22.20 | 42.86 | 0.86 | 0.86 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | frame time p99 | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.25 | 1.25 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | hitches a second | hitches/s | lower | 2.69 | 2.70, 2.68 | 0.74 | 2.87 | 2.77, 3.18, 2.67 | 17.75 | 0.94 | 0.94 | 9.41 / 9.42 | 7.65 / 7.83 | 58.5/2, 59.2/2 / 53.8/2, 54.2/2, 54.1/2 |
| game | nes: NES: thwaite | start to first frame | s | lower | 0.12 | 0.12, 0.12 | 0.00 | 0.06 | 0.06, 0.06, 0.06 | 0.00 | 2.00 | 1.98 | 9.33 / 9.43 | 7.50 / 7.63 | 6.3/1, 6.5/1 / 6.2/1, 6.2/1, 6.2/1 |
| game | nes: NES: thwaite | the NES's speed | % | higher | 502 | 511, 492 | 3.72 | 515 | 515, 516, 514 | 0.29 | 1.03 | 1.02 | 9.33 / 9.43 | 7.50 / 7.63 | 6.3/1, 6.5/1 / 6.2/1, 6.2/1, 6.2/1 |
| game | nes: NES: thwaite | frame time median | ms | lower | 94.35 | 88.80, 99.90 | 11.76 | 88.80 | 88.80, 88.80, 88.80 | 0.00 | 1.06 | 1.05 | 9.33 / 9.43 | 7.50 / 7.63 | 6.3/1, 6.5/1 / 6.2/1, 6.2/1, 6.2/1 |
| game | nes: NES: thwaite | frame time p95 | ms | lower | 105 | 99.90, 111 | 10.53 | 99.90 | 99.90, 99.90, 99.90 | 0.00 | 1.06 | 1.04 | 9.33 / 9.43 | 7.50 / 7.63 | 6.3/1, 6.5/1 / 6.2/1, 6.2/1, 6.2/1 |
| game | nes: NES: thwaite | frame time p99 | ms | lower | 105 | 99.90, 111 | 10.53 | 99.90 | 99.90, 99.90, 99.90 | 0.00 | 1.06 | 1.04 | 9.33 / 9.43 | 7.50 / 7.63 | 6.3/1, 6.5/1 / 6.2/1, 6.2/1, 6.2/1 |
| game | nes: NES: thwaite | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.33 / 9.43 | 7.50 / 7.63 | 6.3/1, 6.5/1 / 6.2/1, 6.2/1, 6.2/1 |
| game | nxphys: nxphys: the wall, 600 substeps | start to first frame | s | lower | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 1.00 | 1.01 | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | nxphys: nxphys: the wall, 600 substeps | 600 substeps | s | lower | 6.80 | 6.71, 7.21, 6.46 | 11.11 | 6.56 | 6.53, 6.57, 6.59 | 1.02 | 1.04 | 1.04 | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | nxphys: nxphys: the wall, 600 substeps | a picture while it runs | ms | lower | 59.43 | 58.39, 62.74, 57.17 | 9.37 | 57.92 | 57.76, 58.16, 57.84 | 0.69 | 1.03 | 1.03 | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | nxphys: nxphys: the wall, 600 substeps | frame time median | ms | lower | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 1.00 | 1.01 | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | nxphys: nxphys: the wall, 600 substeps | frame time p95 | ms | lower | 66.60 | 66.60, 66.60, 66.60 | 0.00 | 66.60 | 66.60, 66.60, 66.60 | 0.00 | 1.00 | 1.01 | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | nxphys: nxphys: the wall, 600 substeps | frame time p99 | ms | lower | 74.00 | 77.70, 66.60, 77.70 | 15.00 | 77.70 | 77.70, 77.70, 77.70 | 0.00 | 0.95 | 0.96 | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | nxphys: nxphys: the wall, 600 substeps | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.47 / 9.41 | 9.70 / 10.10 | 8.1/1, 8.6/1, 7.9/1 / 7.8/1, 7.9/1, 7.9/1 |
| game | pinball: Pinball: its demo, 1,800 frames | start to first frame | s | lower | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 1.00 | 1.01 | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | pinball: Pinball: its demo, 1,800 frames | frames drawn | fps | higher | 22.10 | 23.08, 19.96, 23.27 | 14.98 | 3.88 | 4.81, 3.42, 3.40 | 36.37 | 0.18 | 0.18 | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | pinball: Pinball: its demo, 1,800 frames | start to frame 1800 | s | lower | 12.11 | 12.34, 12.35, 11.65 | 5.78 | 9.62 | 9.70, 9.57, 9.59 | 1.35 | 1.26 | 1.28 | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | pinball: Pinball: its demo, 1,800 frames | frame time median | ms | lower | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 352 | 377, 355, 322 | 15.79 | 0.09 | 0.10 | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | pinball: Pinball: its demo, 1,800 frames | frame time p95 | ms | lower | 148 | 144, 133, 166 | 22.50 | 622 | 622, 622, 622 | 0.00 | 0.24 | 0.24 | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | pinball: Pinball: its demo, 1,800 frames | frame time p99 | ms | lower | 226 | 222, 222, 233 | 4.92 | 622 | 622, 622, 622 | 0.00 | 0.36 | 0.37 | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | pinball: Pinball: its demo, 1,800 frames | hitches a second | hitches/s | lower | 4.05 | 3.87, 4.35, 3.93 | 11.85 | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.36 / 9.23 | 6.83 / 6.67 | 13.3/1, 13.3/1, 12.6/1 / 10.7/1, 10.5/1, 10.6/1 |
| game | micropolis: Micropolis: Haight, 1,000 ticks simulated | start to the end | s | lower | 53.11 | 54.58, 54.86, 49.89 | 9.36 | 49.29 | 49.23, 49.38, 49.25 | 0.30 | 1.08 | 1.05 | 9.13 / 9.38 | 7.37 / 7.90 | 56.5/2, 56.8/2, 51.8/2 / 51.3/2, 51.5/2, 51.3/2 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | start to first frame | s | lower | 4.09 | 4.98, 3.65, 3.63 | 33.03 | 3.12 | 3.15, 3.11, 3.11 | 1.28 | 1.31 | 1.30 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | start to the game | s | lower | 101 | 103, 100, 98.81 | 4.22 | 95.10 | 95.09, 95.07, 95.14 | 0.07 | 1.06 | 1.05 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | scroll | pictures/s | higher | 2.74 | 2.76, 2.67, 2.79 | 4.38 | 3.21 | 3.19, 3.21, 3.22 | 0.94 | 1.17 | 1.16 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | zoomed out | pictures/s | higher | 1.78 | 1.78, 1.74, 1.82 | 4.49 | 2.26 | 2.33, 2.25, 2.21 | 5.30 | 1.27 | 1.27 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | windows | pictures/s | higher | 16.31 | 16.43, 16.28, 16.23 | 1.23 | 18.00 | 17.90, 18.08, 18.02 | 1.00 | 1.10 | 1.10 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | frame time median | ms | lower | 274 | 278, 278, 266 | 4.05 | 200 | 200, 200, 200 | 0.00 | 1.37 | 1.36 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | frame time p95 | ms | lower | 1,432 | 1,576, 1,388, 1,332 | 17.05 | 1,195 | 1,132, 1,343, 1,110 | 19.50 | 1.20 | 1.19 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | frame time p99 | ms | lower | 4,255 | 4,274, 4,518, 3,974 | 12.78 | 2,779 | 2,864, 2,542, 2,930 | 13.98 | 1.53 | 1.52 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | hitches a second | hitches/s | lower | 0.31 | 0.32, 0.30, 0.30 | 6.52 | 0.45 | 0.44, 0.44, 0.46 | 4.48 | 0.69 | 0.68 | 9.25 / 9.30 | 6.83 / 7.10 | 166.5/5, 166.2/5, 163.8/5 / 145.8/5, 146.6/5, 146.9/5 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | start to first frame | s | lower | 3.67 | 3.66, 3.67, 3.67 | 0.27 | 3.13 | 3.11, 3.12, 3.15 | 1.28 | 1.17 | 1.17 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | start to the title drawn | s | lower | 60.66 | 60.58, 60.73, 60.67 | 0.25 | 58.08 | 58.09, 58.02, 58.13 | 0.19 | 1.04 | 1.04 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | title frames | fps | higher | 26.09 | 25.75, 26.60, 25.92 | 3.26 | 27.23 | 27.06, 27.38, 27.26 | 1.18 | 1.04 | 1.04 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | frame time median | ms | lower | 22.20 | 22.20, 22.20, 22.20 | 0.00 | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 0.67 | 0.66 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | frame time p95 | ms | lower | 96.20 | 99.90, 88.80, 99.90 | 11.54 | 77.70 | 77.70, 77.70, 77.70 | 0.00 | 1.24 | 1.23 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | frame time p99 | ms | lower | 155 | 155, 166, 144 | 14.29 | 178 | 166, 178, 189 | 12.50 | 0.88 | 0.87 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | hitches a second | hitches/s | lower | 6.57 | 6.60, 6.89, 6.21 | 10.36 | 1.97 | 1.95, 2.00, 1.96 | 2.54 | 3.33 | 3.32 | 9.45 / 9.49 | 7.60 / 7.93 | 74.9/3, 74.9/3, 74.9/3 / 74.9/3, 74.9/3, 74.9/3 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | start to first picture | s | lower | 1.42 | 1.42, 1.42, 1.43 | 0.70 | 1.10 | 1.11, 1.12, 1.08 | 3.63 | 1.29 | 1.31 | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | pictures left out | pictures | lower | 1.00 | 1.00, 1.00, 1.00 | 0.00 | 1.00 | 1.00, 1.00, 1.00 | 0.00 | 1.00 | 1.02 | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frames drawn | fps | higher | 19.40 | 19.41, 19.41, 19.38 | 0.15 | 19.08 | 19.10, 19.06, 19.07 | 0.21 | 0.98 | 1.00 | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frame time median | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 1.02 | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frame time p95 | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 1.02 | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frame time p99 | ms | lower | 48.10 | 55.50, 44.40, 44.40 | 23.08 | 48.10 | 55.50, 44.40, 44.40 | 23.08 | 1.00 | 1.02 | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.07 / 8.91 | 8.23 / 8.10 | 33.3/1, 33.3/1, 33.3/1 / 33.3/1, 33.3/1, 33.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | start to first picture | s | lower | 0.39 | 0.39, 0.39, 0.39 | 0.00 | 0.35 | 0.34, 0.37, 0.34 | 8.57 | 1.11 | 1.11 | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | pictures left out | pictures | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frames drawn | fps | higher | 25.34 | 25.34, 25.34, 25.34 | 0.00 | 27.29 | 27.38, 27.11, 27.38 | 0.99 | 1.08 | 1.07 | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frame time median | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 0.99 | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frame time p95 | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 0.99 | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frame time p99 | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 48.10 | 44.40, 44.40, 55.50 | 23.08 | 0.92 | 0.92 | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.50 / 9.55 | 8.77 / 9.23 | 7.4/1, 7.4/1, 7.4/1 / 7.3/1, 7.3/1, 7.3/1 |
| app | decode-avi: libmedia: the AVI decoded whole on 16 workers | decode | s | lower | 36.59 | 36.54, 36.56, 36.66 | 0.33 | 36.41 | 36.37, 36.43, 36.44 | 0.19 | 1.00 | 1.00 | 9.17 / 9.19 | 7.60 / 7.60 | 38.0/2, 38.0/2, 38.1/2 / 38.0/2, 38.0/2, 38.0/2 |
| app | nxray: nxray: 320 x 240 on 255 workers | start to first picture | s | lower | 1.41 | 1.40, 1.42, 1.40 | 1.42 | 1.26 | 1.25, 1.27, 1.27 | 1.58 | 1.11 | 1.06 | 8.55 / 8.99 | 3.10 / 3.10 | 30.0/1, 30.0/1, 30.0/1 / 30.0/1, 30.0/1, 30.0/1 |
| app | nxray: nxray: 320 x 240 on 255 workers | pictures | pictures/s | higher | 19.19 | 19.19, 19.20, 19.18 | 0.08 | 22.10 | 22.11, 22.10, 22.08 | 0.13 | 1.15 | 1.09 | 8.55 / 8.99 | 3.10 / 3.10 | 30.0/1, 30.0/1, 30.0/1 / 30.0/1, 30.0/1, 30.0/1 |
| app | nxray: nxray: 320 x 240 on 255 workers | frame time median | ms | lower | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.25 | 1.19 | 8.55 / 8.99 | 3.10 / 3.10 | 30.0/1, 30.0/1, 30.0/1 / 30.0/1, 30.0/1, 30.0/1 |
| app | nxray: nxray: 320 x 240 on 255 workers | frame time p95 | ms | lower | 66.60 | 66.60, 66.60, 66.60 | 0.00 | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 1.20 | 1.14 | 8.55 / 8.99 | 3.10 / 3.10 | 30.0/1, 30.0/1, 30.0/1 / 30.0/1, 30.0/1, 30.0/1 |
| app | nxray: nxray: 320 x 240 on 255 workers | frame time p99 | ms | lower | 66.60 | 66.60, 66.60, 66.60 | 0.00 | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 1.20 | 1.14 | 8.55 / 8.99 | 3.10 / 3.10 | 30.0/1, 30.0/1, 30.0/1 / 30.0/1, 30.0/1, 30.0/1 |
| app | nxray: nxray: 320 x 240 on 255 workers | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 8.55 / 8.99 | 3.10 / 3.10 | 30.0/1, 30.0/1, 30.0/1 / 30.0/1, 30.0/1, 30.0/1 |
| app | nxpath: nxpath: the default scene, 160 x 120, 2 samples | start to the picture | s | lower | 2.94 | 3.00, 2.86, 2.96 | 4.76 | 2.61 | 2.60, 2.64, 2.59 | 1.92 | 1.13 | 1.14 | 9.57 / 9.50 | 8.77 / 9.07 | 4.9/1, 4.8/1, 4.9/1 / 4.7/1, 4.7/1, 4.7/1 |
| app | nxpath: nxpath: the default scene, 160 x 120, 2 samples | the tree (BVH) | ms | lower | 426 | 433, 411, 433 | 5.17 | 393 | 389, 400, 389 | 2.80 | 1.08 | 1.09 | 9.57 / 9.50 | 8.77 / 9.07 | 4.9/1, 4.8/1, 4.9/1 / 4.7/1, 4.7/1, 4.7/1 |
| app | nxpath: nxpath: the default scene, 160 x 120, 2 samples | the picture | ms | lower | 133 | 144, 144, 111 | 24.81 | 122 | 122, 133, 111 | 18.03 | 1.09 | 1.10 | 9.57 / 9.50 | 8.77 / 9.07 | 4.9/1, 4.8/1, 4.9/1 / 4.7/1, 4.7/1, 4.7/1 |
| app | nxweb: nxweb: its start page | start to first frame | s | lower | 0.03 | 0.03, 0.03, 0.03 | 0.00 | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 1.50 | 1.48 | 10.16 / 10.32 | 9.80 / 10.10 | 0.9/1, 0.9/1, 0.9/1 / 0.9/1, 0.9/1, 0.8/1 |
| app | nxweb: nxweb: its start page | start to the page laid out | s | lower | 0.21 | 0.21, 0.21, 0.21 | 0.00 | 0.18 | 0.19, 0.19, 0.17 | 10.91 | 1.15 | 1.13 | 10.16 / 10.32 | 9.80 / 10.10 | 0.9/1, 0.9/1, 0.9/1 / 0.9/1, 0.9/1, 0.8/1 |
| app | nxedit: nxedit: a text of 175 lines made in /tmp, opened to edit | file made to the window still | s | lower | 0.26 | 0.26, 0.26, 0.26 | 0.00 | 0.27 | 0.28, 0.27, 0.27 | 3.66 | 0.95 | 0.95 | 9.04 / 9.04 | 11.80 / 11.80 | 20.0/1, 20.0/1, 20.0/1 / 20.0/1, 20.0/1, 20.0/1 |
| app | nxide: nxide: started, pi.js opened and run | start to first frame | s | lower | 0.03 | 0.03, 0.03, 0.03 | 0.00 | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 1.50 | 1.49 | 9.34 / 9.38 | 8.73 / 9.00 | 4.6/1, 4.5/1, 4.6/1 / 4.3/1, 4.3/1, 4.3/1 |
| app | nxide: nxide: started, pi.js opened and run | start to its keys | s | lower | 0.12 | 0.12, 0.12, 0.12 | 0.00 | 0.10 | 0.10, 0.10, 0.10 | 0.00 | 1.20 | 1.19 | 9.34 / 9.38 | 8.73 / 9.00 | 4.6/1, 4.5/1, 4.6/1 / 4.3/1, 4.3/1, 4.3/1 |
| app | nxide: nxide: started, pi.js opened and run | start to the result | s | lower | 2.64 | 2.66, 2.61, 2.65 | 1.89 | 2.35 | 2.35, 2.35, 2.35 | 0.00 | 1.12 | 1.12 | 9.34 / 9.38 | 8.73 / 9.00 | 4.6/1, 4.5/1, 4.6/1 / 4.3/1, 4.3/1, 4.3/1 |
| app | desktop: the desktop started, the Calculator launched from its icon | desktop up (first frame after the old server) | s | lower | 3.17 | 3.13, 3.23, 3.14 | 3.16 | 2.30 | 2.33, 2.28, 2.29 | 2.17 | 1.38 | 1.38 | 9.28 / 9.24 | 10.10 / 10.70 | 15.6/1, 15.7/1, 15.5/1 / 14.4/1, 14.3/1, 14.4/1 |
| app | desktop: the desktop started, the Calculator launched from its icon | Calculator window shown after | ms | lower | 144 | 144, 144, 144 | 0.00 | 129 | 122, 133, 133 | 8.51 | 1.11 | 1.12 | 9.28 / 9.24 | 10.10 / 10.70 | 15.6/1, 15.7/1, 15.5/1 / 14.4/1, 14.3/1, 14.4/1 |
| app | desktop: the desktop started, the Calculator launched from its icon | frame time median | ms | lower | 107 | 88.80, 66.60, 166 | 93.10 | 107 | 77.70, 144, 99.90 | 62.07 | 1.00 | 1.00 | 9.28 / 9.24 | 10.10 / 10.70 | 15.6/1, 15.7/1, 15.5/1 / 14.4/1, 14.3/1, 14.4/1 |
| app | desktop: the desktop started, the Calculator launched from its icon | frame time p95 | ms | lower | 1,014 | 1,021, 1,010, 1,010 | 1.09 | 1,014 | 1,010, 1,010, 1,021 | 1.09 | 1.00 | 1.00 | 9.28 / 9.24 | 10.10 / 10.70 | 15.6/1, 15.7/1, 15.5/1 / 14.4/1, 14.3/1, 14.4/1 |
| app | desktop: the desktop started, the Calculator launched from its icon | frame time p99 | ms | lower | 1,014 | 1,021, 1,010, 1,010 | 1.09 | 1,014 | 1,010, 1,010, 1,021 | 1.09 | 1.00 | 1.00 | 9.28 / 9.24 | 10.10 / 10.70 | 15.6/1, 15.7/1, 15.5/1 / 14.4/1, 14.3/1, 14.4/1 |
| app | desktop: the desktop started, the Calculator launched from its icon | hitches a second | hitches/s | lower | 0.66 | 0.77, 0.76, 0.46 | 46.73 | 0.63 | 0.69, 0.52, 0.69 | 26.84 | 1.05 | 1.05 | 9.28 / 9.24 | 10.10 / 10.70 | 15.6/1, 15.7/1, 15.5/1 / 14.4/1, 14.3/1, 14.4/1 |
| app | nxview: nxview: a JPEG picture opened | start to first frame | s | lower | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 1.00 | 0.99 | 9.63 / 9.69 | 9.30 / 10.70 | 2.5/1, 2.5/1, 2.5/1 / 1.9/1, 1.9/1, 1.9/1 |
| app | nxview: nxview: a JPEG picture opened | start to the picture | s | lower | 1.12 | 1.12, 1.12, 1.12 | 0.00 | 0.42 | 0.42, 0.42, 0.41 | 2.40 | 2.69 | 2.67 | 9.63 / 9.69 | 9.30 / 10.70 | 2.5/1, 2.5/1, 2.5/1 / 1.9/1, 1.9/1, 1.9/1 |
| app | nxpaint: nxpaint: a rectangle drawn, filled and taken back | start to the script done | s | lower | 0.63 | 0.63, 0.63, 0.63 | 0.00 | 0.57 | 0.57, 0.57, 0.56 | 1.76 | 1.11 | 1.12 | 9.25 / 9.22 | 11.30 / 11.40 | 4.3/1, 4.3/1, 4.3/1 / 4.5/1, 4.5/1, 4.5/1 |
| app | nxfiles: nxfiles: /usr/share opened | start to first frame | s | lower | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 0.02 | 0.02, 0.02, 0.02 | 0.00 | 1.00 | 1.00 | 9.03 / 9.03 | 11.80 / 11.90 | 20.0/1, 20.0/1, 20.0/1 / 20.0/1, 20.0/1, 20.0/1 |
| app | nxfiles: nxfiles: /usr/share opened | start to the folder shown (its last list) | s | lower | 0.41 | 0.41, 0.41, 0.41 | 0.00 | 0.34 | 0.34, 0.34, 0.34 | 0.00 | 1.21 | 1.21 | 9.03 / 9.03 | 11.80 / 11.90 | 20.0/1, 20.0/1, 20.0/1 / 20.0/1, 20.0/1, 20.0/1 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-seq-word | MB/s | higher | 2.89 | 2.91, 2.86, 2.91 | 1.59 | 3.26 | 3.22, 3.27, 3.27 | 1.75 | 1.13 | 1.13 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | load-seq-word | MB/s | higher | 3.49 | 3.46, 3.53, 3.46 | 1.92 | 3.24 | 3.27, 3.22, 3.22 | 1.76 | 0.93 | 0.93 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-seq-byte | MB/s | higher | 0.50 | 0.50, 0.50, 0.50 | 0.60 | 0.51 | 0.52, 0.50, 0.52 | 3.32 | 1.02 | 1.03 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | memcpy-1MB | MB/s | higher | 5.75 | 5.63, 5.81, 5.81 | 3.15 | 160 | 180, 120, 180 | 37.25 | 27.87 | 27.93 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | memset-1MB | MB/s | higher | 4.87 | 4.87, 4.87, 4.87 | 0.00 | 364 | 364, -, 364 | 0.00 | 74.67 | 74.84 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | memcpy-8KB | MB/s | higher | 9.35 | 8.58, 9.48, 10.01 | 15.33 | 7.62 | 7.51, 7.83, 7.51 | 4.24 | 0.81 | 0.82 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | no-memory-same-loop | MB/s | higher | 0.80 | 0.80, 0.79, 0.80 | 1.75 | 0.80 | 0.80, 0.80, 0.79 | 1.75 | 1.00 | 1.00 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-rand-64KB | MB/s | higher | 0.53 | 0.53, 0.53, 0.54 | 1.13 | 0.63 | 0.64, 0.63, 0.63 | 2.84 | 1.19 | 1.20 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-rand-1MB | MB/s | higher | 0.57 | 0.56, 0.58, 0.57 | 3.87 | 0.56 | 0.59, 0.55, 0.55 | 7.81 | 0.99 | 0.99 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-rand-16MB | MB/s | higher | 0.54 | 0.54, 0.54, 0.54 | 1.29 | 0.55 | 0.56, 0.54, 0.56 | 2.36 | 1.02 | 1.02 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | load-rand-16MB | MB/s | higher | 0.61 | 0.61, 0.62, 0.62 | 1.30 | 0.61 | 0.62, 0.61, 0.62 | 1.30 | 1.00 | 1.00 | 9.07 / 9.05 | 9.10 / 8.93 | 42.2/2, 42.2/2, 42.2/2 / 38.9/2, 39.0/2, 39.0/2 |
| memory | memory-kernel: the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times | /dev/zero to /dev/null, 32 MB | s | lower | 7.73 | 7.73, 7.73, 7.73 | 0.00 | 0.55 | 0.56, 0.54, 0.56 | 3.61 | 13.97 | 13.87 | 9.08 / 9.14 | 11.53 / 11.20 | 17.6/1, 17.6/1, 17.6/1 / 8.8/1, 8.8/1, 8.8/1 |
| memory | memory-kernel: the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times | into /tmp, 8 MB | s | lower | 3.97 | 3.97, 3.96, 3.97 | 0.25 | 2.09 | 2.09, 2.10, 2.09 | 0.48 | 1.89 | 1.88 | 9.08 / 9.14 | 11.53 / 11.20 | 17.6/1, 17.6/1, 17.6/1 / 8.8/1, 8.8/1, 8.8/1 |
| memory | memory-kernel: the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times | out of /tmp, 32 MB | s | lower | 1.04 | 1.05, 1.03, 1.05 | 1.92 | 0.95 | 0.95, 0.94, 0.95 | 1.06 | 1.10 | 1.09 | 9.08 / 9.14 | 11.53 / 11.20 | 17.6/1, 17.6/1, 17.6/1 / 8.8/1, 8.8/1, 8.8/1 |
| terminal | cc: cc (tcc): an example compiled and linked | compile | s | lower | 5.77 | 5.74, 5.76, 5.81 | 1.21 | 5.29 | 5.33, 5.28, 5.25 | 1.51 | 1.09 | 1.09 | 9.20 / 9.19 | 7.70 / 7.90 | 7.4/1, 7.4/1, 7.5/1 / 7.1/1, 7.1/1, 7.0/1 |
| terminal | qjs: qjs: 200 digits of pi | run | s | lower | 0.77 | 0.77, 0.77, 0.77 | 0.00 | 0.65 | 0.62, 0.67, 0.67 | 7.65 | 1.18 | 1.19 | 9.62 / 9.52 | 9.60 / 9.67 | 1.9/1, 1.9/1, 1.9/1 / 1.8/1, 1.8/1, 1.8/1 |
| terminal | python: micropython: a sum of 20,000 squares (big integers past 2^31) | run | s | lower | 5.25 | 5.25, 5.25, 5.25 | 0.00 | 5.16 | 5.16, 5.16, 5.16 | 0.00 | 1.02 | 1.02 | 9.15 / 9.14 | 7.50 / 7.60 | 7.1/1, 7.1/1, 7.1/1 / 7.1/1, 7.1/1, 7.1/1 |
| terminal | gzip: gzip: seq 1 20000 (109 KB of text) made, then gzip three times | seq 1 20000 into /tmp | s | lower | 14.73 | 14.67, 14.73, 14.79 | 0.81 | 10.06 | 10.07, 10.07, 10.05 | 0.20 | 1.46 | 1.44 | 9.24 / 9.38 | 7.17 / 7.17 | 45.2/2, 45.2/2, 45.3/2 / 40.4/2, 40.5/2, 40.3/2 |
| terminal | gzip: gzip: seq 1 20000 (109 KB of text) made, then gzip three times | gzip three times | s | lower | 27.13 | 27.13, 27.11, 27.15 | 0.15 | 26.80 | 26.76, 26.90, 26.73 | 0.63 | 1.01 | 1.00 | 9.24 / 9.38 | 7.17 / 7.17 | 45.2/2, 45.2/2, 45.3/2 / 40.4/2, 40.5/2, 40.3/2 |
| terminal | htop: htop: started, quit by q once its keys are drawn | start to its screen and out | s | lower | 1.85 | 1.84, 1.86, 1.86 | 1.08 | 1.62 | 1.65, 1.60, 1.60 | 3.09 | 1.15 | 1.15 | 9.60 / 9.59 | 7.97 / 8.63 | 2.7/1, 2.7/1, 2.7/1 / 2.5/1, 2.5/1, 2.5/1 |
| app | tetris: Tetris: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 89.00 | 89.00, 89.00, 89.00 | 0.00 | 81.00 | 89.00, 77.00, 77.00 | 14.81 | 1.10 | 1.10 | 9.22 / 9.20 | 10.63 / 11.10 | 24.0/1, 24.1/1, 24.1/1 / 22.6/1, 22.6/1, 22.6/1 |
| app | tetris: Tetris: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.81 | 1.70, 1.86, 1.86 | 8.86 | 1.53 | 1.55, 1.48, 1.55 | 4.59 | 0.85 | 0.85 | 9.22 / 9.20 | 10.63 / 11.10 | 24.0/1, 24.1/1, 24.1/1 / 22.6/1, 22.6/1, 22.6/1 |
| app | tetris: Tetris: started from the Start menu, 8 s | frame time median | ms | lower | 211 | 522, 55.50, 55.50 | 221 | - | -, -, - | - | - | - | 9.22 / 9.20 | 10.63 / 11.10 | 24.0/1, 24.1/1, 24.1/1 / 22.6/1, 22.6/1, 22.6/1 |
| app | tetris: Tetris: started from the Start menu, 8 s | frame time p95 | ms | lower | 4,063 | 4,063, 4,074, 4,052 | 0.55 | - | -, -, - | - | - | - | 9.22 / 9.20 | 10.63 / 11.10 | 24.0/1, 24.1/1, 24.1/1 / 22.6/1, 22.6/1, 22.6/1 |
| app | tetris: Tetris: started from the Start menu, 8 s | frame time p99 | ms | lower | 4,063 | 4,063, 4,074, 4,052 | 0.55 | - | -, -, - | - | - | - | 9.22 / 9.20 | 10.63 / 11.10 | 24.0/1, 24.1/1, 24.1/1 / 22.6/1, 22.6/1, 22.6/1 |
| app | tetris: Tetris: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.41 | 0.20, 0.51, 0.51 | 76.23 | - | -, -, - | - | - | - | 9.22 / 9.20 | 10.63 / 11.10 | 24.0/1, 24.1/1, 24.1/1 / 22.6/1, 22.6/1, 22.6/1 |
| app | mines: Mines: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 333 | 333, 333, 333 | 0.00 | 351 | 322, 366, 366 | 12.52 | 0.95 | 0.95 | 9.21 / 9.19 | 10.63 / 11.10 | 24.1/1, 24.0/1, 24.2/1 / 22.7/1, 22.6/1, 22.6/1 |
| app | mines: Mines: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.97 | 1.99, 1.87, 2.04 | 8.64 | 1.79 | 1.74, 1.81, 1.81 | 3.92 | 0.91 | 0.91 | 9.21 / 9.19 | 10.63 / 11.10 | 24.1/1, 24.0/1, 24.2/1 / 22.7/1, 22.6/1, 22.6/1 |
| app | mines: Mines: started from the Start menu, 8 s | frame time median | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 40.70 | 33.30, 44.40, 44.40 | 27.27 | 1.09 | 1.09 | 9.21 / 9.19 | 10.63 / 11.10 | 24.1/1, 24.0/1, 24.2/1 / 22.7/1, 22.6/1, 22.6/1 |
| app | mines: Mines: started from the Start menu, 8 s | frame time p95 | ms | lower | 4,729 | 5,073, 5,062, 4,052 | 21.60 | 6,427 | 7,093, 6,094, 6,094 | 15.54 | 0.74 | 0.74 | 9.21 / 9.19 | 10.63 / 11.10 | 24.1/1, 24.0/1, 24.2/1 / 22.7/1, 22.6/1, 22.6/1 |
| app | mines: Mines: started from the Start menu, 8 s | frame time p99 | ms | lower | 4,729 | 5,073, 5,062, 4,052 | 21.60 | 6,427 | 7,093, 6,094, 6,094 | 15.54 | 0.74 | 0.74 | 9.21 / 9.19 | 10.63 / 11.10 | 24.1/1, 24.0/1, 24.2/1 / 22.7/1, 22.6/1, 22.6/1 |
| app | mines: Mines: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.54 | 0.51, 0.51, 0.61 | 18.40 | 0.41 | 0.33, 0.45, 0.45 | 29.27 | 1.33 | 1.33 | 9.21 / 9.19 | 10.63 / 11.10 | 24.1/1, 24.0/1, 24.2/1 / 22.7/1, 22.6/1, 22.6/1 |
| app | terminal: the Terminal: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 199 | 177, 188, 233 | 28.09 | 167 | 156, 156, 189 | 19.76 | 1.19 | 1.20 | 9.20 / 9.16 | 10.60 / 11.10 | 24.1/1, 24.1/1, 24.2/1 / 22.8/1, 22.8/1, 22.6/1 |
| app | terminal: the Terminal: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.84 | 2.76, 2.82, 2.93 | 5.99 | 2.60 | 2.59, 2.59, 2.62 | 1.15 | 0.92 | 0.92 | 9.20 / 9.16 | 10.60 / 11.10 | 24.1/1, 24.1/1, 24.2/1 / 22.8/1, 22.8/1, 22.6/1 |
| app | terminal: the Terminal: started from the Start menu, 8 s | frame time median | ms | lower | 400 | 377, 433, 388 | 13.89 | 507 | 511, 511, 500 | 2.19 | 0.79 | 0.79 | 9.20 / 9.16 | 10.60 / 11.10 | 24.1/1, 24.1/1, 24.2/1 / 22.8/1, 22.8/1, 22.6/1 |
| app | terminal: the Terminal: started from the Start menu, 8 s | frame time p95 | ms | lower | 544 | 544, 544, 544 | 0.00 | 544 | 544, 544, 544 | 0.00 | 1.00 | 1.00 | 9.20 / 9.16 | 10.60 / 11.10 | 24.1/1, 24.1/1, 24.2/1 / 22.8/1, 22.8/1, 22.6/1 |
| app | terminal: the Terminal: started from the Start menu, 8 s | frame time p99 | ms | lower | 588 | 622, 599, 544 | 13.21 | 544 | 544, 544, 544 | 0.00 | 1.08 | 1.09 | 9.20 / 9.16 | 10.60 / 11.10 | 24.1/1, 24.1/1, 24.2/1 / 22.8/1, 22.8/1, 22.6/1 |
| app | terminal: the Terminal: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00, 0.00 | - | 0.00 | 0.00, 0.00, 0.00 | - | - | - | 9.20 / 9.16 | 10.60 / 11.10 | 24.1/1, 24.1/1, 24.2/1 / 22.8/1, 22.8/1, 22.6/1 |
| app | clock: the Clock: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 85.33 | 78.00, 89.00, 89.00 | 12.89 | 77.67 | 78.00, 78.00, 77.00 | 1.29 | 1.10 | 1.10 | 9.25 / 9.22 | 10.47 / 11.00 | 24.9/1, 24.8/1, 24.7/1 / 23.3/1, 23.5/1, 23.4/1 |
| app | clock: the Clock: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.80 | 3.05, 2.61, 2.74 | 15.71 | 2.81 | 2.78, 2.75, 2.89 | 4.99 | 1.00 | 1.01 | 9.25 / 9.22 | 10.47 / 11.00 | 24.9/1, 24.8/1, 24.7/1 / 23.3/1, 23.5/1, 23.4/1 |
| app | clock: the Clock: started from the Start menu, 8 s | frame time median | ms | lower | 255 | 166, 233, 366 | 78.26 | 281 | 377, 211, 255 | 59.21 | 0.91 | 0.91 | 9.25 / 9.22 | 10.47 / 11.00 | 24.9/1, 24.8/1, 24.7/1 / 23.3/1, 23.5/1, 23.4/1 |
| app | clock: the Clock: started from the Start menu, 8 s | frame time p95 | ms | lower | 1,054 | 1,010, 1,099, 1,054 | 8.42 | 1,003 | 1,010, 1,010, 988 | 2.21 | 1.05 | 1.06 | 9.25 / 9.22 | 10.47 / 11.00 | 24.9/1, 24.8/1, 24.7/1 / 23.3/1, 23.5/1, 23.4/1 |
| app | clock: the Clock: started from the Start menu, 8 s | frame time p99 | ms | lower | 1,110 | 1,110, 1,110, 1,110 | 0.00 | 1,018 | 1,021, 1,021, 1,010 | 1.09 | 1.09 | 1.10 | 9.25 / 9.22 | 10.47 / 11.00 | 24.9/1, 24.8/1, 24.7/1 / 23.3/1, 23.5/1, 23.4/1 |
| app | clock: the Clock: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.76 | 0.98, 0.89, 0.40 | 76.65 | 0.68 | 0.33, 0.86, 0.86 | 77.56 | 1.11 | 1.11 | 9.25 / 9.22 | 10.47 / 11.00 | 24.9/1, 24.8/1, 24.7/1 / 23.3/1, 23.5/1, 23.4/1 |
| app | monitor: the Monitor: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 318 | 310, 344, 300 | 13.84 | 255 | 311, 233, 222 | 34.86 | 1.25 | 1.25 | 9.27 / 9.25 | 10.43 / 10.90 | 24.1/1, 24.1/1, 24.3/1 / 22.8/1, 22.7/1, 22.6/1 |
| app | monitor: the Monitor: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.58 | 2.57, 2.51, 2.66 | 5.81 | 2.44 | 2.44, 2.33, 2.55 | 9.02 | 0.95 | 0.95 | 9.27 / 9.25 | 10.43 / 10.90 | 24.1/1, 24.1/1, 24.3/1 / 22.8/1, 22.7/1, 22.6/1 |
| app | monitor: the Monitor: started from the Start menu, 8 s | frame time median | ms | lower | 155 | 144, 166, 155 | 14.29 | 104 | 133, 122, 55.50 | 75.00 | 1.50 | 1.50 | 9.27 / 9.25 | 10.43 / 10.90 | 24.1/1, 24.1/1, 24.3/1 / 22.8/1, 22.7/1, 22.6/1 |
| app | monitor: the Monitor: started from the Start menu, 8 s | frame time p95 | ms | lower | 877 | 888, 866, 877 | 2.53 | 1,018 | 1,043, 1,032, 977 | 6.55 | 0.86 | 0.86 | 9.27 / 9.25 | 10.43 / 10.90 | 24.1/1, 24.1/1, 24.3/1 / 22.8/1, 22.7/1, 22.6/1 |
| app | monitor: the Monitor: started from the Start menu, 8 s | frame time p99 | ms | lower | 881 | 899, 866, 877 | 3.78 | 1,021 | 1,043, 1,032, 988 | 5.43 | 0.86 | 0.86 | 9.27 / 9.25 | 10.43 / 10.90 | 24.1/1, 24.1/1, 24.3/1 / 22.8/1, 22.7/1, 22.6/1 |
| app | monitor: the Monitor: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.91 | 0.91, 0.91, 0.90 | 1.10 | 0.92 | 0.88, 0.89, 1.00 | 13.00 | 0.98 | 0.98 | 9.27 / 9.25 | 10.43 / 10.90 | 24.1/1, 24.1/1, 24.3/1 / 22.8/1, 22.7/1, 22.6/1 |
| app | network: Network: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 137 | 144, 145, 122 | 16.79 | 114 | 133, 122, 88.00 | 39.36 | 1.20 | 1.20 | 9.19 / 9.19 | 10.63 / 11.10 | 24.0/1, 24.0/1, 24.1/1 / 22.7/1, 22.5/1, 22.6/1 |
| app | network: Network: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.88 | 1.98, 1.75, 1.91 | 12.23 | 1.63 | 1.66, 1.55, 1.68 | 7.98 | 0.87 | 0.87 | 9.19 / 9.19 | 10.63 / 11.10 | 24.0/1, 24.0/1, 24.1/1 / 22.7/1, 22.5/1, 22.6/1 |
| app | settings: Settings: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 440 | 477, 444, 400 | 17.49 | 381 | 411, 333, 400 | 20.45 | 1.15 | 1.16 | 9.22 / 9.18 | 10.60 / 11.03 | 24.1/1, 24.0/1, 24.3/1 / 22.7/1, 22.8/1, 22.8/1 |
| app | settings: Settings: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.83 | 1.81, 1.82, 1.85 | 2.19 | 1.67 | 1.68, 1.67, 1.67 | 0.60 | 0.92 | 0.92 | 9.22 / 9.18 | 10.60 / 11.03 | 24.1/1, 24.0/1, 24.3/1 / 22.7/1, 22.8/1, 22.8/1 |
| app | settings: Settings: started from the Start menu, 8 s | frame time median | ms | lower | 348 | 255, 255, 533 | 79.79 | - | -, -, - | - | - | - | 9.22 / 9.18 | 10.60 / 11.03 | 24.1/1, 24.0/1, 24.3/1 / 22.7/1, 22.8/1, 22.8/1 |
| app | settings: Settings: started from the Start menu, 8 s | frame time p95 | ms | lower | 4,052 | 4,052, 4,040, 4,063 | 0.55 | - | -, -, - | - | - | - | 9.22 / 9.18 | 10.60 / 11.03 | 24.1/1, 24.0/1, 24.3/1 / 22.7/1, 22.8/1, 22.8/1 |
| app | settings: Settings: started from the Start menu, 8 s | frame time p99 | ms | lower | 4,052 | 4,052, 4,040, 4,063 | 0.55 | - | -, -, - | - | - | - | 9.22 / 9.18 | 10.60 / 11.03 | 24.1/1, 24.0/1, 24.3/1 / 22.7/1, 22.8/1, 22.8/1 |
| app | settings: Settings: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.41 | 0.51, 0.51, 0.20 | 76.23 | - | -, -, - | - | - | - | 9.22 / 9.18 | 10.60 / 11.03 | 24.1/1, 24.0/1, 24.3/1 / 22.7/1, 22.8/1, 22.8/1 |
| app | eyes: Eyes: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 289 | 289, 289, 288 | 0.35 | 292 | 288, 288, 300 | 4.11 | 0.99 | 0.99 | 9.18 / 9.18 | 10.60 / 11.10 | 24.1/1, 24.2/1, 24.1/1 / 22.7/1, 22.7/1, 22.8/1 |
| app | eyes: Eyes: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.94 | 2.46, 3.11, 3.24 | 26.56 | 2.48 | 2.48, 2.48, 2.47 | 0.40 | 0.84 | 0.84 | 9.18 / 9.18 | 10.60 / 11.10 | 24.1/1, 24.2/1, 24.1/1 / 22.7/1, 22.7/1, 22.8/1 |
| app | eyes: Eyes: started from the Start menu, 8 s | frame time median | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.18 / 9.18 | 10.60 / 11.10 | 24.1/1, 24.2/1, 24.1/1 / 22.7/1, 22.7/1, 22.8/1 |
| app | eyes: Eyes: started from the Start menu, 8 s | frame time p95 | ms | lower | 1,069 | 1,021, 1,166, 1,021 | 13.49 | 1,288 | 1,010, 1,010, 1,843 | 64.66 | 0.83 | 0.83 | 9.18 / 9.18 | 10.60 / 11.10 | 24.1/1, 24.2/1, 24.1/1 / 22.7/1, 22.7/1, 22.8/1 |
| app | eyes: Eyes: started from the Start menu, 8 s | frame time p99 | ms | lower | 2,716 | 4,063, 2,042, 2,042 | 74.39 | 4,277 | 4,895, 4,895, 3,041 | 43.34 | 0.63 | 0.64 | 9.18 / 9.18 | 10.60 / 11.10 | 24.1/1, 24.2/1, 24.1/1 / 22.7/1, 22.7/1, 22.8/1 |
| app | eyes: Eyes: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.95 | 0.71, 1.01, 1.12 | 43.31 | 0.67 | 0.67, 0.67, 0.67 | 0.00 | 1.41 | 1.41 | 9.18 / 9.18 | 10.60 / 11.10 | 24.1/1, 24.2/1, 24.1/1 / 22.7/1, 22.7/1, 22.8/1 |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 699 | 721, 666, 711 | 7.86 | 614 | 610, 610, 621 | 1.79 | 1.14 | 1.14 | 9.32 / 9.30 | 8.87 / 9.23 | 24.7/1, 24.7/1, 24.8/1 / 23.0/1, 23.1/1, 23.1/1 |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 44.29 | 44.03, 44.64, 44.21 | 1.38 | 48.68 | 48.58, 48.89, 48.56 | 0.68 | 1.10 | 1.10 | 9.32 / 9.30 | 8.87 / 9.23 | 24.7/1, 24.7/1, 24.8/1 / 23.0/1, 23.1/1, 23.1/1 |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | frame time median | ms | lower | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.00 | 1.00 | 9.32 / 9.30 | 8.87 / 9.23 | 24.7/1, 24.7/1, 24.8/1 / 23.0/1, 23.1/1, 23.1/1 |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | frame time p95 | ms | lower | 14.80 | 22.20, 11.10, 11.10 | 75.00 | 11.10 | 11.10, 11.10, 11.10 | 0.00 | 1.33 | 1.34 | 9.32 / 9.30 | 8.87 / 9.23 | 24.7/1, 24.7/1, 24.8/1 / 23.0/1, 23.1/1, 23.1/1 |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | frame time p99 | ms | lower | 59.20 | 55.50, 55.50, 66.60 | 18.75 | 33.30 | 33.30, 33.30, 33.30 | 0.00 | 1.78 | 1.78 | 9.32 / 9.30 | 8.87 / 9.23 | 24.7/1, 24.7/1, 24.8/1 / 23.0/1, 23.1/1, 23.1/1 |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | hitches a second | hitches/s | lower | 2.70 | 2.86, 2.67, 2.56 | 11.12 | 2.44 | 2.41, 2.40, 2.51 | 4.51 | 1.11 | 1.11 | 9.32 / 9.30 | 8.87 / 9.23 | 24.7/1, 24.7/1, 24.8/1 / 23.0/1, 23.1/1, 23.1/1 |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 218 | 310, 66.00, 277 | 112 | 148 | 133, 155, 155 | 14.90 | 1.47 | 1.48 | 9.04 / 8.99 | 8.20 / 8.60 | 24.7/1, 24.8/1, 24.8/1 / 23.4/1, 23.1/1, 23.2/1 |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 13.78 | 13.70, 13.77, 13.86 | 1.16 | 15.06 | 14.93, 15.15, 15.10 | 1.46 | 1.09 | 1.10 | 9.04 / 8.99 | 8.20 / 8.60 | 24.7/1, 24.8/1, 24.8/1 / 23.4/1, 23.1/1, 23.2/1 |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | frame time median | ms | lower | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.00 | 1.01 | 9.04 / 8.99 | 8.20 / 8.60 | 24.7/1, 24.8/1, 24.8/1 / 23.4/1, 23.1/1, 23.2/1 |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | frame time p95 | ms | lower | 92.50 | 88.80, 88.80, 99.90 | 12.00 | 66.60 | 66.60, 66.60, 66.60 | 0.00 | 1.39 | 1.40 | 9.04 / 8.99 | 8.20 / 8.60 | 24.7/1, 24.8/1, 24.8/1 / 23.4/1, 23.1/1, 23.2/1 |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | frame time p99 | ms | lower | 137 | 122, 155, 133 | 24.32 | 104 | 111, 99.90, 99.90 | 10.71 | 1.32 | 1.33 | 9.04 / 8.99 | 8.20 / 8.60 | 24.7/1, 24.8/1, 24.8/1 / 23.4/1, 23.1/1, 23.2/1 |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.98 | 0.99, 0.88, 1.08 | 20.34 | 0.40 | 0.54, 0.33, 0.32 | 55.46 | 2.48 | 2.49 | 9.04 / 8.99 | 8.20 / 8.60 | 24.7/1, 24.8/1, 24.8/1 / 23.4/1, 23.1/1, 23.2/1 |
| app | music: the Music player: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 200 | 300, 133, 167 | 83.50 | 196 | 211, 199, 178 | 16.84 | 1.02 | 1.03 | 9.28 / 9.22 | 9.13 / 10.53 | 24.1/1, 24.1/1, 24.2/1 / 22.7/1, 22.8/1, 22.6/1 |
| app | music: the Music player: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.48 | 2.40, 2.46, 2.57 | 6.86 | 2.23 | 2.27, 2.20, 2.22 | 3.14 | 0.90 | 0.91 | 9.28 / 9.22 | 9.13 / 10.53 | 24.1/1, 24.1/1, 24.2/1 / 22.7/1, 22.8/1, 22.6/1 |
| app | music: the Music player: started from the Start menu, 8 s | frame time median | ms | lower | 204 | 178, 211, 222 | 21.82 | 99.90 | 99.90, 99.90, 99.90 | 0.00 | 2.04 | 2.05 | 9.28 / 9.22 | 9.13 / 10.53 | 24.1/1, 24.1/1, 24.2/1 / 22.7/1, 22.8/1, 22.6/1 |
| app | music: the Music player: started from the Start menu, 8 s | frame time p95 | ms | lower | 1,132 | 1,354, 1,021, 1,021 | 29.41 | 4,055 | 4,063, 4,040, 4,063 | 0.55 | 0.28 | 0.28 | 9.28 / 9.22 | 9.13 / 10.53 | 24.1/1, 24.1/1, 24.2/1 / 22.7/1, 22.8/1, 22.6/1 |
| app | music: the Music player: started from the Start menu, 8 s | frame time p99 | ms | lower | 1,543 | 1,354, 1,654, 1,621 | 19.42 | 4,055 | 4,063, 4,040, 4,063 | 0.55 | 0.38 | 0.38 | 9.28 / 9.22 | 9.13 / 10.53 | 24.1/1, 24.1/1, 24.2/1 / 22.7/1, 22.8/1, 22.6/1 |
| app | music: the Music player: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.74 | 0.81, 0.71, 0.71 | 13.45 | 0.71 | 0.67, 0.78, 0.67 | 15.57 | 1.05 | 1.06 | 9.28 / 9.22 | 9.13 / 10.53 | 24.1/1, 24.1/1, 24.2/1 / 22.7/1, 22.8/1, 22.6/1 |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 163 | 155, 178, 156 | 14.11 | 81.33 | 77.00, 89.00, 78.00 | 14.75 | 2.00 | 2.01 | 9.28 / 9.24 | 10.50 / 11.03 | 24.5/1, 24.4/1, 24.4/1 / 22.8/1, 22.9/1, 23.0/1 |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 3.97 | 3.88, 3.95, 4.07 | 4.79 | 4.09 | 4.05, 4.15, 4.06 | 2.45 | 1.03 | 1.03 | 9.28 / 9.24 | 10.50 / 11.03 | 24.5/1, 24.4/1, 24.4/1 / 22.8/1, 22.9/1, 23.0/1 |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | frame time median | ms | lower | 55.50 | 55.50, 55.50, 55.50 | 0.00 | 44.40 | 44.40, 44.40, 44.40 | 0.00 | 1.25 | 1.25 | 9.28 / 9.24 | 10.50 / 11.03 | 24.5/1, 24.4/1, 24.4/1 / 22.8/1, 22.9/1, 23.0/1 |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | frame time p95 | ms | lower | 507 | 522, 500, 500 | 4.38 | 500 | 500, 500, 500 | 0.00 | 1.01 | 1.02 | 9.28 / 9.24 | 10.50 / 11.03 | 24.5/1, 24.4/1, 24.4/1 / 22.8/1, 22.9/1, 23.0/1 |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | frame time p99 | ms | lower | 533 | 544, 511, 544 | 6.25 | 559 | 555, 566, 555 | 1.99 | 0.95 | 0.96 | 9.28 / 9.24 | 10.50 / 11.03 | 24.5/1, 24.4/1, 24.4/1 / 22.8/1, 22.9/1, 23.0/1 |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | hitches a second | hitches/s | lower | 1.83 | 1.90, 1.80, 1.80 | 5.45 | 1.98 | 1.99, 1.98, 1.97 | 1.01 | 0.93 | 0.93 | 9.28 / 9.24 | 10.50 / 11.03 | 24.5/1, 24.4/1, 24.4/1 / 22.8/1, 22.9/1, 23.0/1 |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 329 | 311, 322, 355 | 13.36 | 263 | 256, 255, 278 | 8.75 | 1.25 | 1.25 | 9.20 / 9.19 | 10.60 / 11.10 | 24.3/1, 24.2/1, 24.3/1 / 22.8/1, 22.7/1, 22.7/1 |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.75 | 1.79, 1.74, 1.73 | 3.42 | 1.72 | 1.74, 1.75, 1.68 | 4.06 | 0.98 | 0.98 | 9.20 / 9.19 | 10.60 / 11.10 | 24.3/1, 24.2/1, 24.3/1 / 22.8/1, 22.7/1, 22.7/1 |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | frame time median | ms | lower | 936 | 955, 899, 955 | 5.93 | - | -, -, - | - | - | - | 9.20 / 9.19 | 10.60 / 11.10 | 24.3/1, 24.2/1, 24.3/1 / 22.8/1, 22.7/1, 22.7/1 |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | frame time p95 | ms | lower | 3,041 | 3,041, 3,041, 3,041 | 0.00 | - | -, -, - | - | - | - | 9.20 / 9.19 | 10.60 / 11.10 | 24.3/1, 24.2/1, 24.3/1 / 22.8/1, 22.7/1, 22.7/1 |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | frame time p99 | ms | lower | 3,041 | 3,041, 3,041, 3,041 | 0.00 | - | -, -, - | - | - | - | 9.20 / 9.19 | 10.60 / 11.10 | 24.3/1, 24.2/1, 24.3/1 / 22.8/1, 22.7/1, 22.7/1 |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.20 | 0.20, 0.20, 0.20 | 0.00 | - | -, -, - | - | - | - | 9.20 / 9.19 | 10.60 / 11.10 | 24.3/1, 24.2/1, 24.3/1 / 22.8/1, 22.7/1, 22.7/1 |
