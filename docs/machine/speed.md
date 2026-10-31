# The machine's speed

Numbers from the tree of 31 October 2026, without a card: `rvc_cpu --pass-model` with `tools/card_model.json`, fitted to an
RTX 5090 and fxc2, one run each. Good to about 7%. A calibrated figure says nothing of the world's fixed rounds, and programs
that lean on many workers (nxray, nxphys) are left out: the model's workers' term rests on one game. See the skill
`calibrated-measurements`, "How far to trust it".

## Calibrated

`python tools/game_par.py GAME --calibrated`. Frames a second by the guest's clock, passes a frame, core 0's and all cores'
thousands of instructions a frame; beside them the card's run the fit has, where it has one:

| | calibrated | passes a frame | core 0, all cores | RTX 5090 |
|---|---|---|---|---|
| Doom, `-timedemo demo1` | 145.9 | 4.25 | 26.3, 64.3 | 145.4 |
| Quake, `+timedemo demo1` (969 frames in 8.8 s) | 109.7 | | | |
| Quake III, q3dm17 with a bot, frames 300 to 700 | 77.4 | 8.52 | 26.2, 165.5 | 79.4 |
| Tiberian Dawn, mission 3, frames 301 to 1200 | 60.8 | 10.67 | 80.1, 125.2 | 60.4 |
| Red Alert, the Soviet mission, frames 301 to 1200 | 86.8 | 7.38 | 54.4, 78.2 | 86.2 |
| ClassiCube, seed 7, walking into a hill | 176.2 | 3.51 | 12.9, 70.5 | |
| the NES, Thwaite, its frames 100 to 1000 (378% of a NES) | 226.8 | 2.76 | 2.7, 30.3 | |

Half-Life and OpenTTD have theirs in their docs, of the same day.

The shell, by the guest's clock, with the model and `RVC_SOUND`, `RVC_WCACHE` and `RVC_ADFAULT` set as `game_par.py` sets them:

| | seconds |
|---|---|
| Linux to its prompt (`cat /proc/uptime` typed there; a longer line adds its own typing) | 1.13 |
| `time factor 999999999989` | 2.71 |
| `time sh -c 'seq 1 20000 \| gzip -c >/dev/null'` | 16.85 |
| `time sh -c 'i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done'` (about 380 million instructions) | 69.2 |

Core 0 alone at full passes is about 5.5 million instructions a second, as the model says: 8,192 instructions in
1.4005 + 8.192 x 0.01105 ms.

A figure from `rvc_cpu`'s plain clock is work, not speed: use it for instructions, and a calibrated run or the card for
frames a second.

## Measured in one run

These are for the card (`rvc_harness`, through `tools\gpu_queue.ps1`).

`RVC_MARK=TEXT` prints the card's tick time a frame each time TEXT comes on the console, since the last one
(`[harness] mark N: tick X ms a frame over N frames`), so two programs run in turn are timed on the same card's clock;
`RVC_STATS_EVERY=1` times every frame, not one in 64. From a shell snapshot, `echo MARK; sleep 2; echo MARK` gave 0.191 ms
over 2,959 frames and 0.154 over 6,368 (card, 28 October 2026).

`--unity F,U[,R]` gives the tick the rounds a world frame of F ms would, U of it the world's own. With `--frame-log FILE` it
also writes `FILE.rounds` (a frame's rounds, uint32) and `FILE.timed` (each card timing as it came: the frame, its card ms
and its rounds, three floats). `RVC_UNITY_PACE=world` paces the rounds as `EmuMachine.SteadyRounds` does (a frame's time
smoothed a tenth a frame, the cap one down or an eighth more every sixth frame over F, one up under 0.8 F) rather than
fitting each frame to the card's last time: a shell loop under `--unity 11.1,1.4` had 2 to 55 rounds a frame the plain way
and 4 to 24 the world's (card, 28 October 2026).

`RVC_TICK_ALT=DIR;DIR...` puts other ticks of the same layout beside the machine's own, and each frame's tick is picked by a
hash of the frame (`RVC_TICK_ALT_SHIFT=N`: 2^N frames a pick); with `--stats-after`, every frame is timed and each timing is
kept by its tick (`STATS tick N DIR: X ms a frame's tick`). Compare ticks this way, in one run on one card's clock: runs taken
in turn drift with the host's and the card's clocks. `--replay` runs each tick from one snapshot. On Doom's timedemo, the
tree's tick and a copy of it in the second slot gave 0.9548 and 0.9545 ms over some 6,000 frames timed each (card,
28 October 2026).
