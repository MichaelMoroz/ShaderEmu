---
name: calibrated-measurements
description: Measure how fast a game or program would run on the graphics card without using the card, with rvc_cpu's clock fitted to a card (--pass-model, tools/calibrate.py, tools/game_par.py --calibrated, tools/card_model.json). Use when asked for frames a second, ticks a second or a before-and-after speed comparison on the CPU emulator, when the card is busy or a card run is not worth its time, when checking or fitting the model again for another card or fxc2, or when deciding between rvc_cpu's plain clock, the calibrated clock and a real card run. Triggers on: calibrated, calibrate, pass model, card model, calibrated cpu runner, game_par --calibrated, how fast on the card, fps without the card.
---

# Calibrated measurements

`rvc_cpu --pass-model A,B,C,D` makes each pass take as long as a card's would:

    ms a pass = A + B a thousand of core 0's instructions + C a thousand of the workers' + D a GPU list

The model goes into the run, not over its counts, because programs read the clock: frame pacing,
tick pacing and how much work a frame does all change with it. So instruction counts under the model
differ from a plain run's. Compare a calibrated run only with another calibrated run.

## Which measurement

| Question | Use |
| --- | --- |
| Instructions a frame, what a change costs in work | plain `rvc_cpu` (`tools/game_par.py GAME`), or `tools/prof.py` for where they go |
| Frames or ticks a second, before and after, no card | calibrated, below |
| A change to the tick, or the final number for a claim | the card (`game_par.py GAME --card`); the model is fitted to the tick it was measured on |
| Speed in the world | Unity (`docs/machine/multicore.md`, "In Unity"), not this |

## Limits to respect

From the user's rules and `AGENTS.md`:

- Nothing over a minute: one command, wait or test run. A `game_par.py` run may take up to its
  `wall` (280 s by default), so if a run could be longer, shorten it or ask first.
- One emulator at a time, with `RVC_THREADS=8`, to keep CPU and GPU under half.
- "A few apps" means a few. Do not widen to `all` without asking.
- A card run goes through `tools/gpu_queue.ps1`, which `--card` does for you. Look first for an
  `rvc_harness.exe` with no arguments: it is the developer's own and spoils any timing.

## A game in `game_par.py`'s list

Use PowerShell. Git Bash mangles `/ # ` and WSL paths.

```powershell
$env:RVC_THREADS='8'; python tools\game_par.py doom --calibrated
```

Read the game's frames a second and passes a frame from its line. A timing shifts with where the
program's work falls: for a small difference, compare means with `--runs N` (it uses `--salt`) and
take turns between the change and the tree. `--runs` multiplies the time, so mind the minute.

The games listed are in `GAMES` at the top of `tools/game_par.py`.

## Anything else

Pass the model to `rvc_cpu` yourself. OpenTTD is not in `GAMES`, for example, so its scroll is:

```powershell
$m = (Get-Content tools\card_model.json -Raw | ConvertFrom-Json).model -join ','
$env:RVC_THREADS='8'; $env:RVC_SOUND='1'; $env:RVC_WCACHE='1'; $env:RVC_ADFAULT='1'
bin\rvc_cpu.exe --quiet --pass-model $m --uart-log build\perf\ottd.log --expect "/ # " --send "nano-X -p & sleep 2; OPENTTD_PLAY_PART=scroll OPENTTD_PLAY=1 openttd -g /usr/share/openttd/busy.sav; echo LX-''DONE\n" --until LX-DONE --wall 50
```

Those three variables are what `game_par.py` sets to match the card's machine. Leave out `--ips`:
the model replaces that clock. The game's own lines then give the rates (`Select-String` the log for
`tick on a worker` and `ms a picture`).

## How far to trust it

Fitted on 30 October 2026 to an RTX 5090 with the new fxc2:

    ms a pass = 1.4005 + 0.0111 core 0 + 0.00079 workers + 0.5705 a GPU list

| Game | Card fps | Model fps | Error | Note |
| --- | --- | --- | --- | --- |
| doom | 145.4 | 153.2 | +5.4% | in the fit; the old clock said 106.9 |
| tdawn | 60.4 | 59.3 | -1.8% | in the fit |
| ralert | 86.2 | 92.7 | +7.5% | in the fit |
| ralert-allies | 61.2 | 64.7 | +5.7% | not in the fit |
| quake3 | 79.4 | 78.8 | -0.8% | not in the fit; passes a frame 7.28 on the model, 8.56 on the card |

- Treat a result as good to about 7%. Report smaller differences only from alternating runs, and
  as a difference between two builds, not as an absolute speed.
- The workers' term rests on one game: nxphys is the only point with many workers, and leaving it
  out gives a 462% error for it (`worst_left_out` in `card_model.json`). Do not trust the absolute
  speed of a program that leans on many workers until a card run has checked it.
- Say "calibrated" with any number you report, and give the card run instead when you have one.
- The model belongs to one card and one fxc2. It does not describe the AMD iGPU or the world's
  fixed rounds.

## Fitting again

Needed for another card, another fxc2 or a change to the tick's cost.

```powershell
python tools\calibrate.py card doom nes tdawn classicube quake-timedemo ralert nxphys
python tools\calibrate.py fit
python tools\calibrate.py check doom ralert-allies quake3
```

- `card` appends to `build/calibrate/card.jsonl`, one run per game of up to 240 s, and refuses while
  VRChat or SteamVR runs. This is well over a minute, so ask first and give one game at a time.
  That file is not in git. Delete it when fitting for another card, or the runs are averaged together.
- `fit` writes `tools/card_model.json` and prints each game's left-out error. It runs each game once
  on `rvc_cpu` for its GPU lists, so it is long as well.
- `check` compares the model with the card's runs. Include games that were not in the fit.

Commit a new `card_model.json` and update the table above.
