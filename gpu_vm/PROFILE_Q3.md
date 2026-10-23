# Quake III on gpu_vm and on RISC-V: where the work per bundle goes (23 October 2026)

This page measures where gpu_vm's advantage in work per unit is lost. Its bundles carry 1.7 to 2.4 RISC-V instructions'
work, and its passes are a third the length of RISC-V's, yet the two machines draw Quake III at nearly the same fps.

The setup:

- **Machines.** gpu_vm runs tick w2/v0009 (`run.py` machine `w2_v0009`, `harness/apps/gvm_backend11.cpp`, image
  `build/gvm_apps/img-q3fix`, checkpoint `gvm-q3fix`). RISC-V runs `rvc_opt` (`rvc_backend11.cpp`, its `rv` checkpoint).
- **Workload.** Both run the same Quake III checkpoint: q3dm17 with three bots and fixed time 33. Every number is taken
  over the game's frames 300 to 1300, unless a section says otherwise.
- **Card runs.** Every card run went through `tools\gpu_queue.ps1`. gpu_vm runs with the new default
  `GVM_LIST_LAZY=1`; `=0` is shown beside it. RISC-V runs with its default (`RVC_LIST_LAZY` off); `RVC_LIST_LAZY=1` is
  shown beside it.
- **Raw files.** They are in `build/profile_q3/` (CSVs in `csv/`).
- **Tools.** The scripts are in `gpu_vm/bench_games/`:
  - `profile_card.py` runs a machine with the logs.
  - `profile_q3.py` analyses a run.
  - `profile_handoffs.py` reads the watched words.
  - `profile_cpu.py` and `profile_syms.py` produce the interpreters' per-core profiles.

  The store replay is `gpu_vm/cpu/wc_replay.c`.

## The answer in one table

| clean card runs (2 each) | gpu_vm | RISC-V |
|---|---|---|
| fps | 89.2 / 90.8 (`GVM_LIST_LAZY=0`: 86.9 / 79.2) | 83.4 / 82.9 (`RVC_LIST_LAZY=1`: 79.8 / 76.5) |
| passes a frame | 16.3 / 15.9 | 5.83 / 5.87 |
| wall a pass | 0.69 ms | 2.06 ms |
| core 0 a frame | 16.2k bundles (24.2k ops) | 23.6k instructions |
| workers a frame | 109k ops | 145k instructions |
| scene / client's game / items a frame (game's own count) | 19k / 20-22k / 7k bundles | 45k / 39k / 15k instr. (2.4x / 1.8x / 2.1x) |
| core 0's passes ending: full / sync or wfi / wait / budget | 44% / 30% / 25% / 0.7% | 0.6% / 72% / - / 27% |
| workers' passes that ran, ending full or early / at the budget | 99% / 0.8% (median 2.5k bundles) | 15% / 85% |
| the busiest worker at its whole budget | 2% of passes | 99% of passes |
| the critical job runs, of its budget | client 22k of 16.3 x 8,192 = 16% | scene 45k of 5.83 x 8,192 = 94% |

**Where the advantage goes.** A gpu_vm pass costs about 145 us fixed (the commit, rounds and zones, about the idle
pass's 29 us, plus a cold texture cache inside the tick worth about 110 us), plus 150 to 210 ns for each bundle of the
busiest core. RISC-V costs about 250 ns for each instruction at its full budget. The bundle itself therefore keeps
gpu_vm's advantage. What loses it is that every chain stops early:

- Core 0 ends a pass full at a median of 1.3k bundles.
- The client's game ends full at about 2k and then waits 5 to 6 whole passes a frame.
- A frame is therefore 16 short passes, not 6 long ones.

The write cache ends them, and the cause is its total capacity for the stores it keeps, not one hot bucket. v0009 enters
silent stores, which are 60 to 76% of the program's stores. Its 130 entries hold 108 to 113 at a full end, the same 85%
that RISC-V's 168 hold (141 to 147). Replayed through RISC-V's design, the same stores end 5 passes a frame full,
against 33 under v0009.

## 1. Work

The numbers below are per core, from the interpreters at 128 cores (`gvm_cpu --profile`, `GVM_PROF_CORES=1`; `rvc_cpu`
with `RVC_PROF_PCS=1`), over frames 300 to 400 (`csv/cpu_gvm.csv`, `csv/cpu_rv.csv`).

| a frame | gpu_vm (bundles) | RISC-V (instructions) |
|---|---|---|
| core 0, everything | 38.4k | 20.4k |
| of it polling and waiting (`mcw_*`, `*Poll`, `Sys_Milliseconds`, `SE_WindowClock`, `NET_Sleep`) | 26.3k (69%) | 5.2k (25%) |
| of it kernel and firmware | 5.5k | 3.5k |
| core 0's program work (effective) | 6.5k | 11.7k (1.8x) |
| workers | 82k | 109k |

gvm_cpu gives core 0 its whole budget, so the time waited shows there as a spin on the clock. On the card, core 0's
14 to 16k bundles a frame (the game's own count says 13.5k to 15.7k) are its work plus the polls it makes before a pass
ends. Effective ops per job are the game's own counts in the table above. The ratio of RISC-V instructions to gpu_vm
bundles is 1.67 (core 0) to 2.37 (scene).

## 2. Rates

| | gpu_vm | RISC-V |
|---|---|---|
| core 0 a second | 2.15 M ops (1.4 M bundles) | 1.97 M instr. |
| workers a second | 9.75 M ops | 12.08 M instr. |
| the busiest worker a pass | median 3.05k bundles in 0.69 ms (4.4 M bundles/s, about 9 M RISC-V-instruction equivalents) | 8,192 in 2.06 ms (4.0 M/s) |

A gpu_vm core that runs is still about twice as fast. Over a frame it runs a sixth of the time.

## 3. Utilization

In use, a core's share is the bundles or instructions it ran over passes x 8,192 (`profile_q3.py`, "use").

- **gpu_vm.** Core 0 uses 17.9% of its budget, and the busiest workers 11 to 13%.
- **RISC-V.** Core 0 uses 49%, and the busiest workers 33 to 78%. The critical worker (the scene, core 7 in that run)
  is at its whole budget in 76% of passes.

**The critical job.** On RISC-V the scene's 45k instructions a frame fill 94% of the frame's 5.83 x 8,192. On gpu_vm
the client's game has 22k bundles to run in 16.3 passes, which is 16% of the budget. It runs in about 10 of them,
ending full at about 2k, and waits in about 6.

## 4. How passes end

The ends are logged by `GVM_ENDS_LOG`; RISC-V's come from its stall word and its count.

- **gpu_vm core 0.** Its passes end:

  | end | share | p10 / 50 / 90 bundles at the end |
  |---|---|---|
  | full | 44% | 708 / 1,319 / 2,643 |
  | sync | 30% | 444 / 504 / 1,473 |
  | wait | 25% | median 0 |
  | budget | 0.7% | |

- **gpu_vm workers.** Of the passes they ran, 61% (logged run) to 99% (clean) end early, mostly full, at
  1.1k / 1.8k / 2.8k bundles.
- **RISC-V core 0.** Its passes end on wfi or pause in 72% (median 934 instructions), the budget in 27%, and full
  in 0.6%.
- **RISC-V workers.** 85% end at the budget.

Passes a frame are 16.3 against 5.83. Core 0's state while it ends, from the watch log (outstanding client, scene and
items frames) is in `csv/handoffs_gvm.csv`. Most of its full ends come while the client and the scene are both still
working on their frames.

### The GPU's time a pass

| | gpu_vm (`GVM_TIME_LOG`) | RISC-V (`RVC_PASS_PROF`) |
|---|---|---|
| tick | 660 us (logged run: 672) | 1.8 ms |
| commit | 9.5 us | 0.05 ms (and unpack 0.03) |
| rounds, list, zones | 8.4, 6.9, 6.8 us | devices 0.2 ms |
| total | 691 us (p10/50/90 460/662/894) | 2.06 ms wall |

#### The restart cost

The fixed part comes from the `GVM_LIST_LAZY=1` `LOGS=time` run, `watch1/quake3.w2_v0009_LOGStime`, with 13,172 timed
passes.

- **The fit.** The GPU's time is 140 us + 69 ns x core 0's bundles + 216 ns x the busiest worker's (R^2 0.83).
- **Outside the tick.** The commit, rounds, list and zones are 31.6 us. That is the idle pass's 28.9 us, as it should
  be.
- **The tick by the most bundles of any core.** Under 1,000 bundles it is -11 us + 326 ns a bundle. From 1,000 on it is
  149 us + 217 ns.
- **The cold refill.** The first 1,000 to 1,400 bundles of a pass cost about 110 ns more each than later ones, about
  110 to 150 us of refill. After that it is flat: 228 ns a bundle at the whole 8,192 against 313 ns at 850.

So restarting a tick costs about 140 to 180 us. That is about 30 us of draws and about 110 to 150 us of a cold cache,
which is the cost of 650 to 700 more bundles. At 16 passes a frame it is 2.3 to 2.9 ms of an 11.2 ms frame. RISC-V pays
its own restart (about 1.1 ms by the fit, confounded with its always-full busiest worker) 5.8 times a frame.

## 5. The write cache

**On the card (v0009, `GVM_WC_LOG`, 1,000 passes).**

- At a full end 96 to 99 of the 130 entries are in use.
- The first table is 63 of 64 full, the second 32 to 35 of 64, and the two overflow entries 0.6 to 0.9.
- 18 of the 32 bucket pairs are full and about 2 are empty.
- At a sync end core 0 holds about 34.

The first table fills completely while the second still has a quarter free. Placement ends the pass, not one bucket.

**On `rvc_cpu` (RISC-V, `RVC_WCACHE=1`, `RVC_WC_LOG`, frames 300 to 400).**

| | core 0 (b6, 336 entries) | workers (b5, 168 entries) |
|---|---|---|
| stores a pass | 627 | 1,032 |
| silent (no entry) | 55% | 62% |
| new entries a pass | 65 | 77 |
| passes ending full | 0% | 8.4% |
| entries at a full end | | 147 of 168 (table 0 93% full buckets, table 1 68%, table 2 27%) |

The distinct texels stored per pass are in `rvc_cpu/prof_q3/wcache.txt`.

### The same stores through each design

`wc_replay.c` replays gvm_cpu's stores and loads of frames 300 to 400 (`wc_replay_ld.txt`, `.csv`). The full ends are
summed over the jobs, a frame.

| design | entries | full ends a frame | median bundles to full (core 0 / worker / client / server) | tags read a store / a load |
|---|---|---|---|---|
| (a) v0009 2x16x4 + 2 | 130 | 33 | 1,850 / 2,175 / 1,981 / 1,928 | 2.0 / 1.4-1.9 |
| (a') v0009 skipping silent stores | 130 | 12 | 3,134 / 3,921 / 4,436 / 3,159 | 2.0 / 1.2-2.0 |
| (b) RISC-V 64+32+16 x3 (core 0) / 32+16+8 (workers) | 336 / 168 | 5 | - / 5,437 / 2,610 / 4,985 | 0.7-1.1 / 0.5-1.2 |
| (c) RISC-V at 168 for every core | 168 | 5 | 4,662 / 5,437 / 2,610 / 4,985 | 0.8-1.1 / 0.6-1.2 |
| (c') RISC-V at 84 | 84 | 23 | 2,036 / 2,345 / 2,039 / 2,167 | 0.8-1.3 / 0.6-1.3 |

Further results of the replay:

- The silent stores are 60 to 76% of the program's stores, and 96% of the kernel's and firmware's.
- At a full end, entries in use are 108 to 113 of 130 under v0009 and 141 to 147 of 168 under RISC-V's design: both are
  about 85% full.
- At 84 entries RISC-V's design is as bad as v0009. So total capacity for the stores that need an entry ends the
  passes, and no hot bucket pair does.
- Skipping silent stores is worth a factor of 2.75 in full ends, and the rest of the gap to (b) is 30% more entries.
- Loads that hit an entry are 25 to 52%.

IDEAS 39 tried silent stores in the tick and they cost more in the tick than they saved in Doom.

## 6. Handoffs

Quake III's jobs here are long-lived `mcwx` jobs. They move between cores and last 23 to 280 passes, so a frame's
handoff is read from the game's own words (`GVM_WATCH` and `RVC_WATCH`, one record a pass):

- the client's asked and made frames
- the items' task and done
- the scene's recorded and drawn

| passes | gpu_vm | RISC-V |
|---|---|---|
| a frame (one client post to the next) | 16.4 (median 15) | 5.8 (median 5) |
| client: posted to made | 19.6 (median 16, 90% 32): 1.19 frames in flight | 7.9 (median 7): 1.36 |
| scene: posted to drawn | 13.0 (median 12), posted 6.1 into the frame: 0.79 | 10.4 (median 10), posted 3.3 in: 1.79 |
| items: posted to done | 4.6 (posted 5.7 in) | 2.0 (posted 2.2 in) |
| the job finishing last | client 46%, scene 54% of frames | scene 97% |

The chain's length in passes is the frame. On gpu_vm the client and the scene are both critical, and each is a chain of
passes ended by the write cache. On RISC-V the scene is critical and runs at its full budget.

## 7. The budget a pass (`--ticks N`, both machines)

Each budget is one clean run a machine, `LOGS=min`, in `build/profile_q3/sweep1/`. The window is frames 300 to 1100:
RISC-V at 16K and 32K runs few passes in its 15 s. Every draw stayed under 5 ms.

- **fps** is the guest's frames a second.
- **passes** is passes a frame.
- **core 0 ends** are its passes ending full, sync or wfi, wait, and at the budget.
- **busiest at budget** is the share of passes in which the busiest worker ran its whole budget.
- **critical job's share** is its work a frame over passes x budget: the client's game on gpu_vm (19-20k bundles), the
  scene on RISC-V (44-46k).

| budget | gpu_vm fps | passes | wall a pass | core 0 ends (full/sync/wait/budget) | busiest at budget | critical job's share |
|---|---|---|---|---|---|---|
| 2,048 | **90.8** | 18.6 | 0.59 ms | 29 / 12 / 44 / 16% | 78% | 50% |
| 4,096 | 89.6 | 14.6 | 0.77 ms | 43 / 32 / 24 / 0.9% | 26% | 33% |
| 8,192 (default) | 89.9 | 14.3 | 0.78 ms | 43 / 34 / 22 / 0.8% | 2% | 17% |
| 16,384 | 80.5 (53.7-90.2, one slow window) | 13.9 | 0.94 ms | 42 / 39 / 18 / 0.8% | 1% | 9% |
| 32,768 | 89.6 | 14.1 | 0.79 ms | 42 / 35 / 21 / 0.8% | 0.2% | 4% |

| budget | RISC-V fps | passes | wall a pass | core 0 ends (wfi/budget/full) | busiest at budget | critical job's share |
|---|---|---|---|---|---|---|
| 2,048 | 63.4 | 23.0 | 0.69 ms | 56 / 44 / 0% | 99.8% | 98% |
| 4,096 | 73.5 | 11.8 | 1.16 ms | 58 / 43 / 0% | 99% | 95% |
| 8,192 (default) | 81.0 (clean runs 82.9-83.4) | 5.89 | 2.10 ms | 72 / 28 / 0.4% | 99.6% | 95% |
| 16,384 | about 90 (72-166 over a short window) | 3.67 | 3.57 ms | 85 / 11 / 3.8% | 74% | 73% |
| 32,768 | 61.1 | 3.47 | 4.73 ms | 87 / 0.9 / 11.7% | 10% | 40% |

**gpu_vm.** The budget does not matter from 4K up. The passes end full or on a handoff long before 8,192, and the
busiest worker is at its budget in 2% of passes.

At 2K a pass is cut where the cache would have filled, and core 0 waits more (44%). The frame comes to 90.8 fps
against 89.9: the same within the noise of one run. The best budget for gpu_vm is therefore not a budget question. A
pass of gpu_vm's runs about 2.5 to 3k bundles at the most, whatever it is allowed, until the write cache holds more.

**RISC-V.** The budget is the lever up to the point where the critical job stops filling it:

- From 2K to 8K to 16K it goes 63, 81, about 90 fps.
- At 32K its own write cache ends 12% of core 0's passes, and a pass is so long (4.7 ms) that the handoffs wait:
  61 fps.

Its best is 16K (to be measured over a longer window). gpu_vm's best is any budget, once its passes can run longer than
about 3k bundles.

## Not done

- The rvlat loops written for gpu_vm.
- Half-Life and Doom under the sweep.
