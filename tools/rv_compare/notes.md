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

Against an earlier comparison of 26 October (on `nommu_hl`, an older image): Doom 91.6 tics/s against 91.3-91.4,
nxray 19.19 pictures/s against 19.17-19.21, Quake III's level 64.5 fps against 64.7-66.5. The tool measures what that
comparison measured.

## After (28 October 2026)

The machine at riscv-backport's end, 133e575, frozen in `ShaderEmu-rvbp/build/after` (its README.md): the harness and
tick of c135015 (unchanged to 133e575 but for `rvc_cpu`), and the image of 133e575's `linux/prebuilt` with Uplink's data.
Every part ran three rounds (`run.py --machines after --rounds 3`, 19:50 to 21:14), none spoiled (the rounds a frame
of each part are within the usual spread of before's at the same card ms).

Two "after" images were measured and dropped on the way (`build/after/build/images/nommu.v1`, `nommu.v2`, their rows in
`build/rv_compare/results_v1.jsonl` and `results_v2.jsonl`), each for a regression found and fixed:

- Quake III's server, 20.2 game frames a second to 10.7: `mcwx`'s calls to the lowest free worker (830fbdb,
  25 October) put the server's job and the client's helpers on one core. They go round the row again (19561b9).
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
| quake3-menu | menu frames (fps) | 74.86 | 72.46 | 0.97 | 19561b9's calls round the row (which gave Quake III's server back its 2x): before's binary on after's machine 74.95 and 74.98, 7624f55's source with today's libraries and the lowest free worker 74.79, the same with the calls round the row 72.36 (one round each but q3old's two); kept: menus count for little against loading and play (28 October 2026), and the server's rate is 2x |
| video-mpg | frames drawn (fps) | 19.40 | 19.08 | 0.98 | the machine, not the program: before's `nxvideo` on after's machine 19.13 and 19.08; the card fit 1.6% fewer rounds a frame (8.23 to 8.10) at the same card time a round (1.10 ms); not the kernel's copy (patch_memcpy.py with before's condition, texel-aligned ends and length: 19.08 and 19.12); not found |

Tiberian Dawn's "game frames" reads only the last short `tdstat` window (spread 11% on both sides): its "start to frame
1300" is the measure.

    python tools/rv_compare/cmp.py shell after                       # its own shell snapshot (RAM's layout differs)
    python tools/rv_compare/run.py --machines after --rounds 3
    python tools/rv_compare/report.py before after

`machines.py`'s `after` is the backport's checkout (`bin
vc_harness.exe`, `build/images/nommu`, `experiments/rvc_opt`);
`RVCMP_AFTER_ROOT` and `RVCMP_AFTER_IMAGE` move it (here `RVCMP_AFTER_ROOT=ShaderEmu-rvbp/build/after`).
