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
