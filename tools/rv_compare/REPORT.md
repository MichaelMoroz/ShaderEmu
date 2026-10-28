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
- Machines: `tools/rv_compare/machines.py`; `before` is frozen in the backport's `build/before` (its README.md).

**Parts measured: before 51, after 0 of 51. after better than before in 0 metrics, worse in 0, level in 0 (3% either way).**

- Run-to-run spread on before over 255 metrics with two rounds or more: median 0.9%, 75th percentile 6.3%, largest 200.0%.
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

## After

    python tools/rv_compare/cmp.py shell after                       # its own shell snapshot (RAM's layout may differ)
    python tools/rv_compare/run.py --machines before,after --rounds 2  # before and after alternating, part by part
    python tools/rv_compare/report.py before after

`machines.py`'s `after` is the backport's checkout (`bin\rvc_harness.exe`, `build/images/nommu`, `experiments/rvc_opt`);
`RVCMP_AFTER_ROOT` and `RVCMP_AFTER_IMAGE` move it. Rows are kept by (machine, part, round): run `after` as rounds 0 and
1 and `before` again beside it as rounds 2 and 3 (`--rounds 4`: the done rounds are skipped), or start a fresh
`results.jsonl` for the final pair.

## Every metric

| group | program and part | metric | unit | better | before: mean | before: each round | before: spread (%) | after: mean | after: each round | after: spread (%) | ratio (after x before) | card ms a frame before / after (ms) | rounds a frame before / after (rounds) | runs before / after (guest s, sections) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| game | doom: Doom: timedemo demo1 (as fast as it goes) | start to first frame | s | lower | 1.46 | 1.45, 1.47 | 1.37 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | timedemo tics a second | tics/s | higher | 91.62 | 91.54, 91.69 | 0.16 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frames drawn | fps | higher | 89.35 | 89.47, 89.23 | 0.27 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frame time p95 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | frame time p99 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom: Doom: timedemo demo1 (as fast as it goes) | hitches a second | hitches/s | lower | 0.10 | 0.10, 0.10 | 0.00 | - | - | - | - | 9.29 / - | 9.75 / - | 41.1/2, 41.1/2 / - |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | start to first frame | s | lower | 1.46 | 1.45, 1.48 | 2.05 | - | - | - | - | 9.38 / - | 10.40 / - | 16.6/1, 16.6/1 / - |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frames drawn | fps | higher | 29.85 | 30.03, 29.67 | 1.21 | - | - | - | - | 9.38 / - | 10.40 / - | 16.6/1, 16.6/1 / - |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frame time median | ms | lower | 33.30 | 33.30, 33.30 | 0.00 | - | - | - | - | 9.38 / - | 10.40 / - | 16.6/1, 16.6/1 / - |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frame time p95 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.38 / - | 10.40 / - | 16.6/1, 16.6/1 / - |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | frame time p99 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.38 / - | 10.40 / - | 16.6/1, 16.6/1 / - |
| game | doom-attract: Doom: the title and its demos as a player sees them (35 tics a second) | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.38 / - | 10.40 / - | 16.6/1, 16.6/1 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | start to first frame | s | lower | 0.81 | 0.82, 0.81 | 1.23 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | start to the level | s | lower | 20.08 | 19.89, 20.26 | 1.84 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | server ticks in the level (QUAKE_TRACK) | ticks/s | higher | 53.34 | 51.20, 55.47 | 8.01 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | lists drawn in the level | fps | higher | 89.44 | 89.53, 89.35 | 0.20 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | server's game time against the world's | % | higher | 100 | 100, 100 | 0.10 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | frame time p95 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | frame time p99 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake: Quake: e1m1 loaded and played (standing) | hitches a second | hitches/s | lower | 0.03 | 0.00, 0.05 | 200 | - | - | - | - | 9.48 / - | 6.90 / - | 44.1/2, 42.8/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | start to the level | s | lower | 20.03 | 19.84, 20.22 | 1.90 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | server ticks | ticks/s | higher | 56.73 | 58.04, 55.43 | 4.60 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | client frames | fps | higher | 87.52 | 87.68, 87.35 | 0.38 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | client frames drawn behind the older server answer | % | lower | 36.51 | 37.37, 35.65 | 4.71 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | server instructions a tick | k | lower | 48.61 | 48.36, 48.86 | 1.03 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | frame time p95 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | frame time p99 | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-server: Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s) | hitches a second | hitches/s | lower | 0.05 | 0.03, 0.07 | 80.00 | - | - | - | - | 9.50 / - | 7.15 / - | 50.0/2, 50.0/2 / - |
| game | quake-timedemo: Quake: timedemo demo1 | start to first frame | s | lower | 0.81 | 0.81, 0.82 | 1.23 | - | - | - | - | 9.84 / - | 7.40 / - | 32.8/1, 33.8/2 / - |
| game | quake-timedemo: Quake: timedemo demo1 | timedemo | fps | higher | 68.00 | 69.40, 66.60 | 4.12 | - | - | - | - | 9.84 / - | 7.40 / - | 32.8/1, 33.8/2 / - |
| game | quake-timedemo: Quake: timedemo demo1 | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.84 / - | 7.40 / - | 32.8/1, 33.8/2 / - |
| game | quake-timedemo: Quake: timedemo demo1 | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 9.84 / - | 7.40 / - | 32.8/1, 33.8/2 / - |
| game | quake-timedemo: Quake: timedemo demo1 | frame time p99 | ms | lower | 33.30 | 33.30, 33.30 | 0.00 | - | - | - | - | 9.84 / - | 7.40 / - | 32.8/1, 33.8/2 / - |
| game | quake-timedemo: Quake: timedemo demo1 | hitches a second | hitches/s | lower | 2.21 | 2.68, 1.75 | 41.99 | - | - | - | - | 9.84 / - | 7.40 / - | 32.8/1, 33.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | start to first frame | s | lower | 1.65 | 1.64, 1.65 | 0.61 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | start to the level | s | lower | 33.48 | 33.11, 33.84 | 2.18 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | frames in the level | fps | higher | 64.50 | 65.10, 63.89 | 1.88 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | lists drawn in the level | fps | higher | 60.55 | 61.35, 59.75 | 2.64 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | frame time p99 | ms | lower | 33.30 | 33.30, 33.30 | 0.00 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3: Quake III: q3dm17 with a bot | hitches a second | hitches/s | lower | 2.28 | 2.15, 2.41 | 11.40 | - | - | - | - | 9.04 / - | 5.60 / - | 44.9/2, 45.8/2 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | server game frames (sv_fps 20) | frames/s | higher | 20.18 | 20.67, 19.68 | 4.91 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | the world moves (a server job taken) every | ms | lower | 98.95 | 103, 94.60 | 8.79 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | a server job, posted to taken | ms | lower | 90.10 | 95.00, 85.20 | 10.88 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | game frames a job | frames | lower | 2.00 | 2.14, 1.87 | 13.47 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | the server's worker while it runs | M instructions/s | higher | 3.12 | 3.15, 3.08 | 2.25 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | server crashes |  | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | instructions a job | k | lower | 261 | 280, 243 | 14.25 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | frame time p99 | ms | lower | 38.85 | 33.30, 44.40 | 28.57 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-server: Quake III: the server's ticks on its worker (a q3srv: line a job) | hitches a second | hitches/s | lower | 1.71 | 1.53, 1.88 | 20.53 | - | - | - | - | 8.82 / - | 5.15 / - | 99.9/3, 99.9/3 / - |
| game | quake3-menu: Quake III: its main menu | start to first frame | s | lower | 1.65 | 1.63, 1.68 | 3.02 | - | - | - | - | 8.51 / - | 5.15 / - | 59.9/2, 59.9/2 / - |
| game | quake3-menu: Quake III: its main menu | menu frames (from 30 s) | fps | higher | 74.86 | 75.08, 74.64 | 0.59 | - | - | - | - | 8.51 / - | 5.15 / - | 59.9/2, 59.9/2 / - |
| game | quake3-menu: Quake III: its main menu | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 8.51 / - | 5.15 / - | 59.9/2, 59.9/2 / - |
| game | quake3-menu: Quake III: its main menu | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 8.51 / - | 5.15 / - | 59.9/2, 59.9/2 / - |
| game | quake3-menu: Quake III: its main menu | frame time p99 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 8.51 / - | 5.15 / - | 59.9/2, 59.9/2 / - |
| game | quake3-menu: Quake III: its main menu | hitches a second | hitches/s | lower | 0.13 | 0.13, 0.13 | 0.00 | - | - | - | - | 8.51 / - | 5.15 / - | 59.9/2, 59.9/2 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | start to first frame | s | lower | 3.39 | 3.39, 3.39 | 0.00 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | start to the level drawn | s | lower | 81.46 | 80.64, 82.28 | 2.01 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frames in the level | fps | higher | 16.09 | 16.59, 15.58 | 6.28 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | start to second 31 of the level | s | lower | 111 | 110, 112 | 1.53 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frame time median | ms | lower | 139 | 133, 144 | 8.00 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frame time p95 | ms | lower | 222 | 222, 222 | 0.00 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | frame time p99 | ms | lower | 239 | 233, 244 | 4.65 | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | halflife: Half-Life: hldemo1 played from second 31 to 43 of the level | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.39 / - | 6.90 / - | 128.2/4, 131.0/4 / - |
| game | classicube: ClassiCube: a new world, standing | start to first frame | s | lower | 0.68 | 0.67, 0.68 | 1.48 | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | classicube: ClassiCube: a new world, standing | start to steady | s | lower | 8.56 | 8.42, 8.70 | 3.27 | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | classicube: ClassiCube: a new world, standing | frames when steady | fps | higher | 86.29 | 86.91, 85.67 | 1.44 | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | classicube: ClassiCube: a new world, standing | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | classicube: ClassiCube: a new world, standing | frame time p95 | ms | lower | 16.65 | 11.10, 22.20 | 66.67 | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | classicube: ClassiCube: a new world, standing | frame time p99 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | classicube: ClassiCube: a new world, standing | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.02 / - | 6.05 / - | 50.0/2, 50.0/2 / - |
| game | tdawn-menu: Tiberian Dawn: its start and menu | start to first frame | s | lower | 1.08 | 1.08, 1.08 | 0.00 | - | - | - | - | 8.66 / - | 6.05 / - | 40.0/2, 40.0/2 / - |
| game | tdawn-menu: Tiberian Dawn: its start and menu | start to the menu (its last list; it holds still) | s | lower | 32.70 | 32.70, 32.70 | 0.00 | - | - | - | - | 8.66 / - | 6.05 / - | 40.0/2, 40.0/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | start to first frame | s | lower | 1.08 | 1.07, 1.08 | 0.93 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | game frames | frames/s | higher | 62.40 | 65.70, 59.10 | 10.58 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | start to frame 1300 | s | lower | 40.20 | 40.06, 40.35 | 0.72 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | frame time median | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | frame time p95 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | frame time p99 | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | tdawn: Tiberian Dawn: mission 3 played by itself, as fast as it goes | hitches a second | hitches/s | lower | 0.70 | 0.65, 0.74 | 12.95 | - | - | - | - | 9.79 / - | 8.25 / - | 42.4/2, 42.7/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | start to first frame | s | lower | 1.08 | 1.04, 1.11 | 6.51 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | film (154 or 156 frames shown) | s | lower | 12.83 | 12.80, 12.87 | 0.51 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | start to the mission | s | lower | 36.13 | 35.89, 36.37 | 1.33 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | mission start to frame 1300 | s | lower | 20.77 | 20.69, 20.85 | 0.77 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | frame time p95 | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | frame time p99 | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | ralert: Red Alert: its film, then the Soviet mission played by itself | hitches a second | hitches/s | lower | 2.69 | 2.70, 2.68 | 0.74 | - | - | - | - | 9.41 / - | 7.65 / - | 58.5/2, 59.2/2 / - |
| game | nes: NES: thwaite | start to first frame | s | lower | 0.12 | 0.12, 0.12 | 0.00 | - | - | - | - | 9.33 / - | 7.50 / - | 6.3/1, 6.5/1 / - |
| game | nes: NES: thwaite | the NES's speed | % | higher | 502 | 511, 492 | 3.72 | - | - | - | - | 9.33 / - | 7.50 / - | 6.3/1, 6.5/1 / - |
| game | nes: NES: thwaite | frame time median | ms | lower | 94.35 | 88.80, 99.90 | 11.76 | - | - | - | - | 9.33 / - | 7.50 / - | 6.3/1, 6.5/1 / - |
| game | nes: NES: thwaite | frame time p95 | ms | lower | 105 | 99.90, 111 | 10.53 | - | - | - | - | 9.33 / - | 7.50 / - | 6.3/1, 6.5/1 / - |
| game | nes: NES: thwaite | frame time p99 | ms | lower | 105 | 99.90, 111 | 10.53 | - | - | - | - | 9.33 / - | 7.50 / - | 6.3/1, 6.5/1 / - |
| game | nes: NES: thwaite | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.33 / - | 7.50 / - | 6.3/1, 6.5/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | start to first frame | s | lower | 0.02 | 0.02, 0.02 | 0.00 | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | 600 substeps | s | lower | 6.96 | 6.71, 7.21 | 7.18 | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | a picture while it runs | ms | lower | 60.56 | 58.39, 62.74 | 7.18 | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | frame time median | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | frame time p95 | ms | lower | 66.60 | 66.60, 66.60 | 0.00 | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | frame time p99 | ms | lower | 72.15 | 77.70, 66.60 | 15.38 | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | nxphys: nxphys: the wall, 600 substeps | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.49 / - | 9.50 / - | 8.1/1, 8.6/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | start to first frame | s | lower | 0.02 | 0.02, 0.02 | 0.00 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | frames drawn | fps | higher | 21.52 | 23.08, 19.96 | 14.50 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | start to frame 1800 | s | lower | 12.34 | 12.34, 12.35 | 0.08 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | frame time median | ms | lower | 33.30 | 33.30, 33.30 | 0.00 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | frame time p95 | ms | lower | 139 | 144, 133 | 8.00 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | frame time p99 | ms | lower | 222 | 222, 222 | 0.00 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | pinball: Pinball: its demo, 1,800 frames | hitches a second | hitches/s | lower | 4.11 | 3.87, 4.35 | 11.68 | - | - | - | - | 9.38 / - | 6.70 / - | 13.3/1, 13.3/1 / - |
| game | micropolis: Micropolis: Haight, 1,000 ticks simulated | start to the end | s | lower | 54.72 | 54.58, 54.86 | 0.51 | - | - | - | - | 9.04 / - | 7.15 / - | 56.5/2, 56.8/2 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | start to first frame | s | lower | 4.32 | 4.98, 3.65 | 30.82 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | start to the game | s | lower | 102 | 103, 100 | 2.90 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | scroll | pictures/s | higher | 2.71 | 2.76, 2.67 | 3.31 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | zoomed out | pictures/s | higher | 1.76 | 1.78, 1.74 | 2.27 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | windows | pictures/s | higher | 16.36 | 16.43, 16.28 | 0.92 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | frame time median | ms | lower | 278 | 278, 278 | 0.00 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | frame time p95 | ms | lower | 1,482 | 1,576, 1,388 | 12.73 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | frame time p99 | ms | lower | 4,396 | 4,274, 4,518 | 5.56 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd: OpenTTD: a busy game loaded, scrolled, zoomed, its windows | hitches a second | hitches/s | lower | 0.31 | 0.32, 0.30 | 6.45 | - | - | - | - | 9.25 / - | 6.80 / - | 166.5/5, 166.2/5 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | start to first frame | s | lower | 3.67 | 3.66, 3.67 | 0.27 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | start to the title drawn | s | lower | 60.66 | 60.58, 60.73 | 0.25 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | title frames | fps | higher | 26.18 | 25.75, 26.60 | 3.25 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | frame time median | ms | lower | 22.20 | 22.20, 22.20 | 0.00 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | frame time p95 | ms | lower | 94.35 | 99.90, 88.80 | 11.76 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | frame time p99 | ms | lower | 161 | 155, 166 | 6.90 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| game | openttd-title: OpenTTD: its title screen (a game behind the menu) | hitches a second | hitches/s | lower | 6.74 | 6.60, 6.89 | 4.30 | - | - | - | - | 9.45 / - | 7.50 / - | 74.9/3, 74.9/3 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | start to first picture | s | lower | 1.42 | 1.42, 1.42 | 0.00 | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | pictures left out | pictures | lower | 1.00 | 1.00, 1.00 | 0.00 | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frames drawn | fps | higher | 19.41 | 19.41, 19.41 | 0.00 | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frame time median | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frame time p95 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | frame time p99 | ms | lower | 49.95 | 55.50, 44.40 | 22.22 | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-mpg: nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.08 / - | 8.25 / - | 33.3/1, 33.3/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | start to first picture | s | lower | 0.39 | 0.39, 0.39 | 0.00 | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | pictures left out | pictures | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frames drawn | fps | higher | 25.34 | 25.34, 25.34 | 0.00 | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frame time median | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frame time p95 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | frame time p99 | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | video-avi: nxvideo: Big Buck Bunny, the AVI (MPEG-4) | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.46 / - | 8.70 / - | 7.4/1, 7.4/1 / - |
| app | decode-avi: libmedia: the AVI decoded whole on 16 workers | decode | s | lower | 36.55 | 36.54, 36.56 | 0.05 | - | - | - | - | 9.17 / - | 7.60 / - | 38.0/2, 38.0/2 / - |
| app | nxray: nxray: 320 x 240 on 255 workers | start to first picture | s | lower | 1.41 | 1.40, 1.42 | 1.42 | - | - | - | - | 8.55 / - | 3.10 / - | 30.0/1, 30.0/1 / - |
| app | nxray: nxray: 320 x 240 on 255 workers | pictures | pictures/s | higher | 19.19 | 19.19, 19.20 | 0.04 | - | - | - | - | 8.55 / - | 3.10 / - | 30.0/1, 30.0/1 / - |
| app | nxray: nxray: 320 x 240 on 255 workers | frame time median | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | - | - | - | - | 8.55 / - | 3.10 / - | 30.0/1, 30.0/1 / - |
| app | nxray: nxray: 320 x 240 on 255 workers | frame time p95 | ms | lower | 66.60 | 66.60, 66.60 | 0.00 | - | - | - | - | 8.55 / - | 3.10 / - | 30.0/1, 30.0/1 / - |
| app | nxray: nxray: 320 x 240 on 255 workers | frame time p99 | ms | lower | 66.60 | 66.60, 66.60 | 0.00 | - | - | - | - | 8.55 / - | 3.10 / - | 30.0/1, 30.0/1 / - |
| app | nxray: nxray: 320 x 240 on 255 workers | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 8.55 / - | 3.10 / - | 30.0/1, 30.0/1 / - |
| app | nxpath: nxpath: the default scene, 160 x 120, 2 samples | start to the picture | s | lower | 2.93 | 3.00, 2.86 | 4.78 | - | - | - | - | 9.58 / - | 8.80 / - | 4.9/1, 4.8/1 / - |
| app | nxpath: nxpath: the default scene, 160 x 120, 2 samples | the tree (BVH) | ms | lower | 422 | 433, 411 | 5.21 | - | - | - | - | 9.58 / - | 8.80 / - | 4.9/1, 4.8/1 / - |
| app | nxpath: nxpath: the default scene, 160 x 120, 2 samples | the picture | ms | lower | 144 | 144, 144 | 0.00 | - | - | - | - | 9.58 / - | 8.80 / - | 4.9/1, 4.8/1 / - |
| app | nxweb: nxweb: its start page | start to first frame | s | lower | 0.03 | 0.03, 0.03 | 0.00 | - | - | - | - | 10.17 / - | 9.80 / - | 0.9/1, 0.9/1 / - |
| app | nxweb: nxweb: its start page | start to the page laid out | s | lower | 0.21 | 0.21, 0.21 | 0.00 | - | - | - | - | 10.17 / - | 9.80 / - | 0.9/1, 0.9/1 / - |
| app | nxedit: nxedit: a text of 175 lines made in /tmp, opened to edit | file made to the window still | s | lower | 0.26 | 0.26, 0.26 | 0.00 | - | - | - | - | 9.04 / - | 11.80 / - | 20.0/1, 20.0/1 / - |
| app | nxide: nxide: started, pi.js opened and run | start to first frame | s | lower | 0.03 | 0.03, 0.03 | 0.00 | - | - | - | - | 9.34 / - | 8.75 / - | 4.6/1, 4.5/1 / - |
| app | nxide: nxide: started, pi.js opened and run | start to its keys | s | lower | 0.12 | 0.12, 0.12 | 0.00 | - | - | - | - | 9.34 / - | 8.75 / - | 4.6/1, 4.5/1 / - |
| app | nxide: nxide: started, pi.js opened and run | start to the result | s | lower | 2.63 | 2.66, 2.61 | 1.90 | - | - | - | - | 9.34 / - | 8.75 / - | 4.6/1, 4.5/1 / - |
| app | desktop: the desktop started, the Calculator launched from its icon | desktop up (first frame after the old server) | s | lower | 3.18 | 3.13, 3.23 | 3.14 | - | - | - | - | 9.28 / - | 10.10 / - | 15.6/1, 15.7/1 / - |
| app | desktop: the desktop started, the Calculator launched from its icon | Calculator window shown after | ms | lower | 144 | 144, 144 | 0.00 | - | - | - | - | 9.28 / - | 10.10 / - | 15.6/1, 15.7/1 / - |
| app | desktop: the desktop started, the Calculator launched from its icon | frame time median | ms | lower | 77.70 | 88.80, 66.60 | 28.57 | - | - | - | - | 9.28 / - | 10.10 / - | 15.6/1, 15.7/1 / - |
| app | desktop: the desktop started, the Calculator launched from its icon | frame time p95 | ms | lower | 1,016 | 1,021, 1,010 | 1.09 | - | - | - | - | 9.28 / - | 10.10 / - | 15.6/1, 15.7/1 / - |
| app | desktop: the desktop started, the Calculator launched from its icon | frame time p99 | ms | lower | 1,016 | 1,021, 1,010 | 1.09 | - | - | - | - | 9.28 / - | 10.10 / - | 15.6/1, 15.7/1 / - |
| app | desktop: the desktop started, the Calculator launched from its icon | hitches a second | hitches/s | lower | 0.77 | 0.77, 0.76 | 1.31 | - | - | - | - | 9.28 / - | 10.10 / - | 15.6/1, 15.7/1 / - |
| app | nxview: nxview: a JPEG picture opened | start to first frame | s | lower | 0.02 | 0.02, 0.02 | 0.00 | - | - | - | - | 9.58 / - | 9.30 / - | 2.5/1, 2.5/1 / - |
| app | nxview: nxview: a JPEG picture opened | start to the picture | s | lower | 1.12 | 1.12, 1.12 | 0.00 | - | - | - | - | 9.58 / - | 9.30 / - | 2.5/1, 2.5/1 / - |
| app | nxpaint: nxpaint: a rectangle drawn, filled and taken back | start to the script done | s | lower | 0.63 | 0.63, 0.63 | 0.00 | - | - | - | - | 9.26 / - | 11.30 / - | 4.3/1, 4.3/1 / - |
| app | nxfiles: nxfiles: /usr/share opened | start to first frame | s | lower | 0.02 | 0.02, 0.02 | 0.00 | - | - | - | - | 9.03 / - | 11.80 / - | 20.0/1, 20.0/1 / - |
| app | nxfiles: nxfiles: /usr/share opened | start to the folder shown (its last list) | s | lower | 0.41 | 0.41, 0.41 | 0.00 | - | - | - | - | 9.03 / - | 11.80 / - | 20.0/1, 20.0/1 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-seq-word | MB/s | higher | 2.88 | 2.91, 2.86 | 1.60 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | load-seq-word | MB/s | higher | 3.50 | 3.46, 3.53 | 1.92 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-seq-byte | MB/s | higher | 0.50 | 0.50, 0.50 | 0.60 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | memcpy-1MB | MB/s | higher | 5.72 | 5.63, 5.81 | 3.16 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | memset-1MB | MB/s | higher | 4.87 | 4.87, 4.87 | 0.00 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | memcpy-8KB | MB/s | higher | 9.03 | 8.58, 9.48 | 10.00 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | no-memory-same-loop | MB/s | higher | 0.80 | 0.80, 0.79 | 1.76 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-rand-64KB | MB/s | higher | 0.53 | 0.53, 0.53 | 0.00 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-rand-1MB | MB/s | higher | 0.57 | 0.56, 0.58 | 3.88 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | store-rand-16MB | MB/s | higher | 0.54 | 0.54, 0.54 | 0.00 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory: memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill | load-rand-16MB | MB/s | higher | 0.61 | 0.61, 0.62 | 1.31 | - | - | - | - | 9.07 / - | 9.10 / - | 42.2/2, 42.2/2 / - |
| memory | memory-kernel: the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times | /dev/zero to /dev/null, 32 MB | s | lower | 7.73 | 7.73, 7.73 | 0.00 | - | - | - | - | 9.08 / - | 11.55 / - | 17.6/1, 17.6/1 / - |
| memory | memory-kernel: the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times | into /tmp, 8 MB | s | lower | 3.96 | 3.97, 3.96 | 0.25 | - | - | - | - | 9.08 / - | 11.55 / - | 17.6/1, 17.6/1 / - |
| memory | memory-kernel: the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times | out of /tmp, 32 MB | s | lower | 1.04 | 1.05, 1.03 | 1.92 | - | - | - | - | 9.08 / - | 11.55 / - | 17.6/1, 17.6/1 / - |
| terminal | cc: cc (tcc): an example compiled and linked | compile | s | lower | 5.75 | 5.74, 5.76 | 0.35 | - | - | - | - | 9.20 / - | 7.70 / - | 7.4/1, 7.4/1 / - |
| terminal | qjs: qjs: 200 digits of pi | run | s | lower | 0.77 | 0.77, 0.77 | 0.00 | - | - | - | - | 9.62 / - | 9.60 / - | 1.9/1, 1.9/1 / - |
| terminal | python: micropython: a sum of 20,000 squares (big integers past 2^31) | run | s | lower | 5.25 | 5.25, 5.25 | 0.00 | - | - | - | - | 9.15 / - | 7.50 / - | 7.1/1, 7.1/1 / - |
| terminal | gzip: gzip: seq 1 20000 (109 KB of text) made, then gzip three times | seq 1 20000 into /tmp | s | lower | 14.70 | 14.67, 14.73 | 0.41 | - | - | - | - | 9.24 / - | 7.20 / - | 45.2/2, 45.2/2 / - |
| terminal | gzip: gzip: seq 1 20000 (109 KB of text) made, then gzip three times | gzip three times | s | lower | 27.12 | 27.13, 27.11 | 0.07 | - | - | - | - | 9.24 / - | 7.20 / - | 45.2/2, 45.2/2 / - |
| terminal | htop: htop: started, quit by q once its keys are drawn | start to its screen and out | s | lower | 1.85 | 1.84, 1.86 | 1.08 | - | - | - | - | 9.61 / - | 8.00 / - | 2.7/1, 2.7/1 / - |
| app | tetris: Tetris: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 89.00 | 89.00, 89.00 | 0.00 | - | - | - | - | 9.23 / - | 10.65 / - | 24.0/1, 24.1/1 / - |
| app | tetris: Tetris: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.78 | 1.70, 1.86 | 8.99 | - | - | - | - | 9.23 / - | 10.65 / - | 24.0/1, 24.1/1 / - |
| app | tetris: Tetris: started from the Start menu, 8 s | frame time median | ms | lower | 289 | 522, 55.50 | 162 | - | - | - | - | 9.23 / - | 10.65 / - | 24.0/1, 24.1/1 / - |
| app | tetris: Tetris: started from the Start menu, 8 s | frame time p95 | ms | lower | 4,068 | 4,063, 4,074 | 0.27 | - | - | - | - | 9.23 / - | 10.65 / - | 24.0/1, 24.1/1 / - |
| app | tetris: Tetris: started from the Start menu, 8 s | frame time p99 | ms | lower | 4,068 | 4,063, 4,074 | 0.27 | - | - | - | - | 9.23 / - | 10.65 / - | 24.0/1, 24.1/1 / - |
| app | tetris: Tetris: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.35 | 0.20, 0.51 | 87.32 | - | - | - | - | 9.23 / - | 10.65 / - | 24.0/1, 24.1/1 / - |
| app | mines: Mines: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 333 | 333, 333 | 0.00 | - | - | - | - | 9.22 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | mines: Mines: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.93 | 1.99, 1.87 | 6.22 | - | - | - | - | 9.22 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | mines: Mines: started from the Start menu, 8 s | frame time median | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.22 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | mines: Mines: started from the Start menu, 8 s | frame time p95 | ms | lower | 5,067 | 5,073, 5,062 | 0.22 | - | - | - | - | 9.22 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | mines: Mines: started from the Start menu, 8 s | frame time p99 | ms | lower | 5,067 | 5,073, 5,062 | 0.22 | - | - | - | - | 9.22 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | mines: Mines: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.51 | 0.51, 0.51 | 0.00 | - | - | - | - | 9.22 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | terminal: the Terminal: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 182 | 177, 188 | 6.03 | - | - | - | - | 9.21 / - | 10.60 / - | 24.1/1, 24.1/1 / - |
| app | terminal: the Terminal: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.79 | 2.76, 2.82 | 2.15 | - | - | - | - | 9.21 / - | 10.60 / - | 24.1/1, 24.1/1 / - |
| app | terminal: the Terminal: started from the Start menu, 8 s | frame time median | ms | lower | 405 | 377, 433 | 13.70 | - | - | - | - | 9.21 / - | 10.60 / - | 24.1/1, 24.1/1 / - |
| app | terminal: the Terminal: started from the Start menu, 8 s | frame time p95 | ms | lower | 544 | 544, 544 | 0.00 | - | - | - | - | 9.21 / - | 10.60 / - | 24.1/1, 24.1/1 / - |
| app | terminal: the Terminal: started from the Start menu, 8 s | frame time p99 | ms | lower | 610 | 622, 599 | 3.64 | - | - | - | - | 9.21 / - | 10.60 / - | 24.1/1, 24.1/1 / - |
| app | terminal: the Terminal: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.00 | 0.00, 0.00 | - | - | - | - | - | 9.21 / - | 10.60 / - | 24.1/1, 24.1/1 / - |
| app | clock: the Clock: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 83.50 | 78.00, 89.00 | 13.17 | - | - | - | - | 9.25 / - | 10.45 / - | 24.9/1, 24.8/1 / - |
| app | clock: the Clock: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.83 | 3.05, 2.61 | 15.55 | - | - | - | - | 9.25 / - | 10.45 / - | 24.9/1, 24.8/1 / - |
| app | clock: the Clock: started from the Start menu, 8 s | frame time median | ms | lower | 200 | 166, 233 | 33.33 | - | - | - | - | 9.25 / - | 10.45 / - | 24.9/1, 24.8/1 / - |
| app | clock: the Clock: started from the Start menu, 8 s | frame time p95 | ms | lower | 1,054 | 1,010, 1,099 | 8.42 | - | - | - | - | 9.25 / - | 10.45 / - | 24.9/1, 24.8/1 / - |
| app | clock: the Clock: started from the Start menu, 8 s | frame time p99 | ms | lower | 1,110 | 1,110, 1,110 | 0.00 | - | - | - | - | 9.25 / - | 10.45 / - | 24.9/1, 24.8/1 / - |
| app | clock: the Clock: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.94 | 0.98, 0.89 | 9.63 | - | - | - | - | 9.25 / - | 10.45 / - | 24.9/1, 24.8/1 / - |
| app | monitor: the Monitor: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 327 | 310, 344 | 10.40 | - | - | - | - | 9.28 / - | 10.45 / - | 24.1/1, 24.1/1 / - |
| app | monitor: the Monitor: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.54 | 2.57, 2.51 | 2.36 | - | - | - | - | 9.28 / - | 10.45 / - | 24.1/1, 24.1/1 / - |
| app | monitor: the Monitor: started from the Start menu, 8 s | frame time median | ms | lower | 155 | 144, 166 | 14.29 | - | - | - | - | 9.28 / - | 10.45 / - | 24.1/1, 24.1/1 / - |
| app | monitor: the Monitor: started from the Start menu, 8 s | frame time p95 | ms | lower | 877 | 888, 866 | 2.53 | - | - | - | - | 9.28 / - | 10.45 / - | 24.1/1, 24.1/1 / - |
| app | monitor: the Monitor: started from the Start menu, 8 s | frame time p99 | ms | lower | 882 | 899, 866 | 3.77 | - | - | - | - | 9.28 / - | 10.45 / - | 24.1/1, 24.1/1 / - |
| app | monitor: the Monitor: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.91 | 0.91, 0.91 | 0.00 | - | - | - | - | 9.28 / - | 10.45 / - | 24.1/1, 24.1/1 / - |
| app | network: Network: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 144 | 144, 145 | 0.69 | - | - | - | - | 9.19 / - | 10.65 / - | 24.0/1, 24.0/1 / - |
| app | network: Network: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.86 | 1.98, 1.75 | 12.33 | - | - | - | - | 9.19 / - | 10.65 / - | 24.0/1, 24.0/1 / - |
| app | settings: Settings: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 460 | 477, 444 | 7.17 | - | - | - | - | 9.23 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | settings: Settings: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.81 | 1.81, 1.82 | 0.55 | - | - | - | - | 9.23 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | settings: Settings: started from the Start menu, 8 s | frame time median | ms | lower | 255 | 255, 255 | 0.00 | - | - | - | - | 9.23 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | settings: Settings: started from the Start menu, 8 s | frame time p95 | ms | lower | 4,046 | 4,052, 4,040 | 0.27 | - | - | - | - | 9.23 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | settings: Settings: started from the Start menu, 8 s | frame time p99 | ms | lower | 4,046 | 4,052, 4,040 | 0.27 | - | - | - | - | 9.23 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | settings: Settings: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.51 | 0.51, 0.51 | 0.00 | - | - | - | - | 9.23 / - | 10.60 / - | 24.1/1, 24.0/1 / - |
| app | eyes: Eyes: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 289 | 289, 289 | 0.00 | - | - | - | - | 9.18 / - | 10.60 / - | 24.1/1, 24.2/1 / - |
| app | eyes: Eyes: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.79 | 2.46, 3.11 | 23.34 | - | - | - | - | 9.18 / - | 10.60 / - | 24.1/1, 24.2/1 / - |
| app | eyes: Eyes: started from the Start menu, 8 s | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.18 / - | 10.60 / - | 24.1/1, 24.2/1 / - |
| app | eyes: Eyes: started from the Start menu, 8 s | frame time p95 | ms | lower | 1,093 | 1,021, 1,166 | 13.20 | - | - | - | - | 9.18 / - | 10.60 / - | 24.1/1, 24.2/1 / - |
| app | eyes: Eyes: started from the Start menu, 8 s | frame time p99 | ms | lower | 3,052 | 4,063, 2,042 | 66.18 | - | - | - | - | 9.18 / - | 10.60 / - | 24.1/1, 24.2/1 / - |
| app | eyes: Eyes: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.86 | 0.71, 1.01 | 34.88 | - | - | - | - | 9.18 / - | 10.60 / - | 24.1/1, 24.2/1 / - |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 694 | 721, 666 | 7.93 | - | - | - | - | 9.33 / - | 8.85 / - | 24.7/1, 24.7/1 / - |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 44.34 | 44.03, 44.64 | 1.38 | - | - | - | - | 9.33 / - | 8.85 / - | 24.7/1, 24.7/1 / - |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | frame time median | ms | lower | 11.10 | 11.10, 11.10 | 0.00 | - | - | - | - | 9.33 / - | 8.85 / - | 24.7/1, 24.7/1 / - |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | frame time p95 | ms | lower | 16.65 | 22.20, 11.10 | 66.67 | - | - | - | - | 9.33 / - | 8.85 / - | 24.7/1, 24.7/1 / - |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | frame time p99 | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | - | - | - | - | 9.33 / - | 8.85 / - | 24.7/1, 24.7/1 / - |
| app | glxgears: Gears (OpenGL): started from the Start menu, 8 s | hitches a second | hitches/s | lower | 2.76 | 2.86, 2.67 | 6.87 | - | - | - | - | 9.33 / - | 8.85 / - | 24.7/1, 24.7/1 / - |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 188 | 310, 66.00 | 130 | - | - | - | - | 9.02 / - | 8.20 / - | 24.7/1, 24.8/1 / - |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 13.73 | 13.70, 13.77 | 0.51 | - | - | - | - | 9.02 / - | 8.20 / - | 24.7/1, 24.8/1 / - |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | frame time median | ms | lower | 44.40 | 44.40, 44.40 | 0.00 | - | - | - | - | 9.02 / - | 8.20 / - | 24.7/1, 24.8/1 / - |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | frame time p95 | ms | lower | 88.80 | 88.80, 88.80 | 0.00 | - | - | - | - | 9.02 / - | 8.20 / - | 24.7/1, 24.8/1 / - |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | frame time p99 | ms | lower | 139 | 122, 155 | 24.00 | - | - | - | - | 9.02 / - | 8.20 / - | 24.7/1, 24.8/1 / - |
| app | imgui: Dear ImGui: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.94 | 0.99, 0.88 | 11.76 | - | - | - | - | 9.02 / - | 8.20 / - | 24.7/1, 24.8/1 / - |
| app | music: the Music player: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 216 | 300, 133 | 77.14 | - | - | - | - | 9.28 / - | 9.15 / - | 24.1/1, 24.1/1 / - |
| app | music: the Music player: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 2.43 | 2.40, 2.46 | 2.47 | - | - | - | - | 9.28 / - | 9.15 / - | 24.1/1, 24.1/1 / - |
| app | music: the Music player: started from the Start menu, 8 s | frame time median | ms | lower | 194 | 178, 211 | 17.14 | - | - | - | - | 9.28 / - | 9.15 / - | 24.1/1, 24.1/1 / - |
| app | music: the Music player: started from the Start menu, 8 s | frame time p95 | ms | lower | 1,188 | 1,354, 1,021 | 28.04 | - | - | - | - | 9.28 / - | 9.15 / - | 24.1/1, 24.1/1 / - |
| app | music: the Music player: started from the Start menu, 8 s | frame time p99 | ms | lower | 1,504 | 1,354, 1,654 | 19.93 | - | - | - | - | 9.28 / - | 9.15 / - | 24.1/1, 24.1/1 / - |
| app | music: the Music player: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.76 | 0.81, 0.71 | 13.16 | - | - | - | - | 9.28 / - | 9.15 / - | 24.1/1, 24.1/1 / - |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 166 | 155, 178 | 13.81 | - | - | - | - | 9.28 / - | 10.50 / - | 24.5/1, 24.4/1 / - |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 3.92 | 3.88, 3.95 | 1.79 | - | - | - | - | 9.28 / - | 10.50 / - | 24.5/1, 24.4/1 / - |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | frame time median | ms | lower | 55.50 | 55.50, 55.50 | 0.00 | - | - | - | - | 9.28 / - | 10.50 / - | 24.5/1, 24.4/1 / - |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | frame time p95 | ms | lower | 511 | 522, 500 | 4.35 | - | - | - | - | 9.28 / - | 10.50 / - | 24.5/1, 24.4/1 / - |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | frame time p99 | ms | lower | 527 | 544, 511 | 6.32 | - | - | - | - | 9.28 / - | 10.50 / - | 24.5/1, 24.4/1 / - |
| app | nes-nova: Nova the Squirrel (NES): started from the Start menu, 8 s | hitches a second | hitches/s | lower | 1.85 | 1.90, 1.80 | 5.41 | - | - | - | - | 9.28 / - | 10.50 / - | 24.5/1, 24.4/1 / - |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | window shown after (from its key) | ms | lower | 316 | 311, 322 | 3.48 | - | - | - | - | 9.19 / - | 10.60 / - | 24.3/1, 24.2/1 / - |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | lists drawn from 3 s | fps | higher | 1.77 | 1.79, 1.74 | 2.83 | - | - | - | - | 9.19 / - | 10.60 / - | 24.3/1, 24.2/1 / - |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | frame time median | ms | lower | 927 | 955, 899 | 5.99 | - | - | - | - | 9.19 / - | 10.60 / - | 24.3/1, 24.2/1 / - |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | frame time p95 | ms | lower | 3,041 | 3,041, 3,041 | 0.00 | - | - | - | - | 9.19 / - | 10.60 / - | 24.3/1, 24.2/1 / - |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | frame time p99 | ms | lower | 3,041 | 3,041, 3,041 | 0.00 | - | - | - | - | 9.19 / - | 10.60 / - | 24.3/1, 24.2/1 / - |
| app | example-cube: the IDE with the Cube example: started from the Start menu, 8 s | hitches a second | hitches/s | lower | 0.20 | 0.20, 0.20 | 0.00 | - | - | - | - | 9.19 / - | 10.60 / - | 24.3/1, 24.2/1 / - |
