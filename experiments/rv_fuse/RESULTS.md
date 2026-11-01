# A faster ISA by translating RISC-V: what was built and measured (1 November 2026, RTX 5070 Laptop, D3D11)

Every number below is the card with no other run beside it unless marked. Logs: `alt2.log` (ticks alternating in one run), `ab.log` (A B A B whole runs), `shapes.log`,
`classes.log`, `h2h/results.jsonl` (`h2h/show.py`). Snapshots and code files lie outside the repository (`C:\Users\micha\agents_isa`), made by `xlate3.py` from the snapshots' profiles.

## Against gpu_vm, today (`h2h/h2h.py`, cold boot of each machine, its own compiler, same programs; tick ms between marks)

| Load | Flat | World (11.1 ms, 1.4 ms, a round) | VR (11.1 ms, 7.1 ms, 1) |
|---|---|---|---|
| shell 4,000 turns | rv 13.2-14.0 s, gvm 10.6-11.2 s: gvm 1.25x | 13.6 / 10.8: 1.26x | 14.2 / 12.5: 1.14x |
| seq 1..200000 | 70.8 / 57.5: 1.23x | 73.0 / 57.6: 1.27x | 77.2 / 66.8: 1.16x |
| gzip twice | 13.3 / 8.2: 1.62x | 13.7 / 8.3: 1.65x | 14.2 / 9.5: 1.51x |
| Doom timedemo, realtics | 1904-1952 / 1379-1428: gvm 1.35-1.42x | 2309 / 3023: rv 1.31x | 6820 / 10350: rv 1.52x |
| Quake timedemo, s | 9.2-9.4 / 8.7-9.2: gvm up to 1.08x | 12.9 / 12.9: level | 30.3 / 53.4: rv 1.76x |
| nxray, 255 workers, ms a picture | 53-69 / 22-25: gvm 2.3x or more | 155 / 22 | 145 / 66: gvm 2.2x |

gpu_vm's Doom: 231 million bundles against 292 million RV instructions (21% fewer) at 196 ns against 213, and 126 thousand passes against 50 thousand, about 1,830 bundles a pass.

## The loops, whole runs against the tree (`ab.log`)

`wide3.h` and `xlate3.py`: each block list-scheduled, three ops a bundle, memory ops first (two at most), both successors fetched, tiles of 16 x 16 for the code, no x0 or unused reads, restart at every bundle
from a table (`--check` runs each block both ways on random registers, from every restart; the guests ran with no oops). Budget in RV instructions, so a pass is the same work.

| Load | Tree | Best translated | | RV a bundle | Write cache full, tree / translated |
|---|---|---|---|---|---|
| shell | 5.25 M/s | 4.74 (`w3x`) | -10% | 2.04 | 0 / 0 |
| Doom | 5.07 M/s | 4.76 (`w3x`) | -6% | 2.04 | 3.9% / 3.8-4.1% of passes |
| Quake | 1.84 M/s on core 0 | 1.74 (`w3y`, floats as ops) | -6%, drawn 14 tenths against 17 | 2.00 | none of either; 80% of passes end at the program's pause |
| nxray | 628 M/s all cores | 614 (`w3y`) | -2%; same instructions | 1.86 | 1 pass of each |

## What each tried form did (ms a pass of 8192 RV on shell / Doom; tree 1.41 / 1.46)

| Form | shell | Doom | |
|---|---|---|---|
| Earlier `wide.h` as committed: no reordering | | | -13% / -14% whole |
| `wide.h` in gpu_vm's form: both successors, six reads first, selects (`WIDE_F`) | 2.73 at 12,740 RV = 214 ns an RV | | level with its own branching form (2.73 / 2.32) |
| Reordered, one op a bundle, no chains | 2.45 | 2.38 | 1.24 RV a bundle |
| Three ops, one memory | 1.87 | 1.90 | 1.84 |
| Three ops, two memory, one fetch (`w3`) | 1.86 | 1.84 | 2.04 |
| The same, both successors fetched (`w3f`) | 1.72 | 1.68 | fetching both is cheaper than branching to one |
| Old step out of the loop (`w3a`) | 1.69 | | 2% |
| One memory op a bundle, then only one memory path in the shader (`w3b`) | 1.74 at 1.84 RV | | 7% a bundle |
| Old loop gone, general step for what is left (`w3c`), floats out too (`w3d`) | 1.84, 1.81 | | worse: leaves cost more than the shader's size |
| Hot layout | 1.73, 1.67 with tiles | 1.71, 1.66 with tiles | no gain |
| Code in tiles of 16 x 16 (`w3t`) | 1.63 | 1.62 | 3 to 4% |
| An empty op not read or written (`w3u`), control's registers only with control (`w3v`) | 1.57, 1.58 | 1.56, 1.56 | 3%, the second nothing |
| x0 and a load's data not read (`w3x`) | 1.59-1.62 | 1.57-1.58 | level |
| x8 to x15 in shader temporaries as well as the array, a read by selects (`w3h`) | 1.79 | 1.78 | 13% worse |

`w3x` by shape on Doom, 1.22 to 2.04 RV a bundle: 1.94, 1.73, 1.70, 1.57 ms. A straight line: 135 ns a bundle and 1.03 ms a pass of what no bundling takes away (126 ns an RV instruction; the tree's whole
instruction is 178). So at any density the loop has 1.03 ms; +25% is 1.17 ms, which leaves 0.14 ms for 1,000 bundles: 8 RV a bundle. `pack2.py`'s ceilings with reordering: 2.7-3.1 at four to six ops, 3.3-3.9 at eight
(inferred: at 3 RV a bundle, if a bundle stayed at 135 ns, 1.03 + 0.37 = 1.40 ms, +4%; a second texel a trip is not paid for there).

## Prices

Tree (`classes.ps1`, cached tight loops): ALU 143 ns, load 174, store 190, two-instruction loop 146. gpu_vm's own (`--case`): add 55 ns (50 an RV instruction), lw 129, sw 167. So memory is within 1.2 to 1.35x, arithmetic is 2.9x: yet
in real code a bundle of gpu_vm's is 196 ns (Doom) and this loop's empty trip is 135.

`promo.py`, what no translation of a block sees but a promotion could: saves of callee-saved registers 5% of instructions, restores 5%: at most about 7% more if none of it were needed (inferred).

## Not built

- Promoting stack slots to more registers across calls: needs each slot proved unread by anything else, a signal or a context switch's view of the stack; 7% at most by the counts above.
- Two memory ops from one texel at one read (26-36% of memory ops are in the last one's texel): not separately built; two memory paths cost what `w3b` shows and came out worth it by density alone.
- memcpy and loop ops: the machine's copies already go by the GPU; a loop's own form was not built.
- Two texels a bundle: not built, see the line above.
- The 8 x 8 core: gpu_vm's own RESULTS has its levels; here the pass already holds at 8192.
- `tcc`, `hlsl`, the NES, `rvc_cpu`: nothing passed, so nothing brought in.

## What the 1.03 ms is (1 November 2026, the lean tick; scripts and logs in `acct/`)

The old tick is gone from the tree (`experiments/rvc_lean` is the only one), so the tree here is lean and `w3x`/`w3y` are `wide3.h` ported to it (`make_w3_lean.py`, shapes 1001 and 2025, the same code files). Every run had its guest's output read: 0 oops, doomstat, quakestat and raystat lines going on. Numbers from `rvc_opt` are marked as such.

**It is not a pass's cost: the fit took a cost per RV instruction for a cost per pass.** The tick against the pass's budget (`--ticks`, `acct/ladder.py`, `ladder_lean.log`; `ladder_rvc_opt.log` the same for the old tick), ms of the tick by the GPU's timestamps:

| Budget | lean Doom | `w3x` Doom | lean shell | `w3x` shell |
|---|---|---|---|---|
| 2 | 0.036 | 0.037 | 0.036 | 0.037 |
| 1,024 | 0.210 | 0.229 | 0.201 | 0.232 |
| 4,096 | 0.728 | 0.825 | 0.691 | 0.813 |
| 8,192 | 1.465 | 1.625 | 1.358 | 1.562 |

Straight lines through 0.036 ms: 175 ns an RV instruction on Doom and 161 on the shell for lean, 194 and 186 for `w3x` (`rvc_opt` through 0.050 ms: 165 to 177 on Doom at every step from 256 to 8,192). A pass with nothing in it costs 0.036 ms of tick. So the 1.03 ms of the line by density is about 0.04 ms a pass and about 0.99 ms that goes with the count of RV instructions, 121 ns each (inferred: the old fit with the measured intercept); `w3x` at 2.04 RV a bundle is about 390 ns a trip against 135 for an empty one. Texture-cache misses by data reached in a pass are not the part either: the slope does not fall as a pass gets longer.

**Where a pass goes, ms** (measured on the card, D3D11, `ladder_lean.log`; whole frames, one timed in 64):

| | lean Doom | `w3x` Doom | lean shell | `w3x` shell |
|---|---|---|---|---|
| Tick with no instructions | 0.036 | 0.037 | 0.036 | 0.037 |
| The instructions | 1.429 | 1.588 | 1.322 | 1.525 |
| Commit (unpack, gather, rows, points) | 0.052 | 0.053 | 0.051 | 0.052 |
| Devices (in the world: once a Unity frame, not a pass) | 0.078 | 0.084 | 0.076 | 0.077 |
| Left (wall against the timestamps) | 0.021 | 0.025 | 0.013 | 0.034 |
| **Wall** | **1.616** | **1.787** | **1.498** | **1.725** |

Inside "the instructions", from the card's PC samples (below). **Inferred for lean**: the samples are of `rvc_opt` on the shell's snapshot under D3D12, and the shares are carried to lean's 1.322 ms by hand:

| What the warp was at | Share | ms of lean's shell (inferred) |
|---|---|---|
| Arithmetic and selects running (some 1.4 clocks an instruction) | 56% | 0.74 |
| Branches, their NOPs and barriers (a branch about three times a plain instruction) | 18% | 0.24 |
| Waiting for a texture read | 17.5% | 0.23 |
| LDL and STL, running and waited for | 6.6% | 0.09 |
| Other | 1.5% | 0.02 |

So 74% is the length of the path in the card's own instructions, some 200 to 260 for an RV instruction (inferred from lines run once an instruction at 0.28% each). The register file is under 7%. An older translated loop (`rvc_opt_s1`, not `w3x`'s shape) has texture waits at 32.5% of its samples and arithmetic at 35%: a bundle's fetch waits where the tree's does not. No sample exists of `w3x` or of Doom.

**The trace.** The UI is not needed: `ngfx-trace extract` gives `gpus.0.SamplingData.PcSamplingPerSM.N.pcSamplesStorage`, every SM's raw samples (16 bytes: pc, a word, a word whose bits 0-5 are the warp and 8-13 the reason, read off what they sit on: 8 is a scoreboard wait; the other reasons' meanings are not known), `query` of `devices.0.ShaderProfilerReport.pcCountersInfo.pcSamplingSession.codeBlocks` gives each shader's `vaRange`, and with `ngfx-trace sass` that is every instruction's time (`acct/pc_samples.py`, `pc_account.py`, `pc_blocks.py`, `pc_classes.py`). `nsight_lines.py` was not fixed: its table is not needed. Limits, all of which bound the table above:
- A new trace could not be taken. One trace of lean ran to its transfer and was killed by the script's 50 s limit (its file is empty); every trace since says `GPU Performance Counters unavailable`, with no such process left, for over an hour and a half. The kill is the likely cause, not a proven one. `sass.ps1` now has 56 s and a shorter capture. It wants a look by the owner: a driver reset or a reboot is not an agent's to do with others on the card.
- `rvc_trace12` on lean reports `instructions=0` and the same state hash at budgets 8,192 and 2,048, at the same frames a second: it runs the tick and does not get on with the guest. So a trace of it may be one pass held. The `rvc_opt` traces used were not checked for this and may be the same: read their shares as those of one place in the shell, not of the load.
- Whether D3D12 and D3D11 compile alike could not be settled: SASS exists only for D3D12, and the runner above gives no time to compare. The two are known to differ in what a change costs (the skill's note).

**The bundles on lean, whole runs in turn** (`acct/final.log`, `acct.jsonl`). The older figures of -6% and -10% were against `rvc_opt`.

| Load | lean | Best bundle tick | |
|---|---|---|---|
| shell | 190.6, 192.0 ns an RV | `w3x` 214.4, 220.8 | -11 to -13% |
| Doom | 208.2, 210.2 | `w3x` 227.5, 233.1 | -8 to -10% (other agents' CPU runs beside: 1.70 ms a pass where the ladder's was 1.62, so a comparison, not a level) |
| Quake | 8.07 M/s all cores | `w3y` 7.36 | -9%; drawn 14-15 tenths against 19-20 |
| nxray | 1,257 M/s all cores | `w3y` 1,090 | -13%; a picture 20-23 ms against 30 |

Lean's write cache: `write cache full` 4.5-4.9% of Doom's passes (`acct/longer.log`). Nothing here clears the bar.

**What was tried to take something away:**
- A longer pass, to spread the fixed 0.17 ms (`acct/longer.py`, two rounds in turn, whole runs, M/s of core 0; nxray all cores). 8,192 / 12,288 / 16,384: shell 5.33 / 5.46-5.50 / 5.57-5.78, Doom 4.97-5.03 / 5.07-5.09 / 4.91-5.10, Quake 1.85 / 1.79-1.82 / 1.74-1.78, nxray 1,313-1,333 / 1,266-1,280 / 1,128-1,160. The shell gains 3 to 8%, Doom up to 2%, Quake and nxray lose: not taken. (The first round's shell at 8,192 ran at a low clock and is left out.)
- The last K texels a jump fetched kept in registers, so that a jump back does not wait for the texture (`acct/make_jt.py`, in one run with the tree, one round): shell -3.1 / -3.1 / -4.2% at K of 1, 2, 4, Doom -1.6 / -2.1 / -1.4%, Quake -0.2 / +0.3 / -0.6%, nxray -0.7 / -3.6 / -3.6%. Not taken.
- Nothing fixed is left worth taking: tick, commit and devices together are 0.17 ms of a pass, and the devices' 0.08 is not a pass's in the world.

**nxray.** The gap was the old tick's: `rvc_opt` has no lanes. On lean it is the other way round (`acct/nxray.jsonl`; `h2h.py nxray_free` has no test's sums, which were most of core 0 in `RAY_FRAMES` runs: 135 ms a picture with the test, 6 of them the picture; that the sum is what fills the difference is read from the source, not measured apart):

| | Flat | World (11.1, 1.4) | VR (11.1, 7.1) |
|---|---|---|---|
| lean, the field's lanes (default) | 116-122 pictures a second, 23-24 M rays/s, 8-9 ms | 90, 18 M rays/s, 11-12 ms | 45-48, 9 M rays/s, 22 ms |
| lean, workers' lanes (`RAY_FIELD=0`) | 14 ms | 11-12 ms | 11-22 ms |
| lean, no lanes (`RAY_LANES=0`) | 47 ms | | |
| `rvc_opt`, 31 October | 53-69 ms | 100-155 ms | 111-145 ms |
| gpu_vm | 35-37 pictures a second, 7.1-7.5 M rays/s, 24-36 ms | 22 ms (31 October) | 44-66 ms (31 October) |

In the flat column lean is 3.3 times gpu_vm. The field is wired but not yet run in Unity (`AGENTS.md`), so its World column is the harness's, not the world's. Without lanes the old gap is still there, from counts over 60 pictures: 1,485 M worker instructions against 1,097 M (24.8 M a picture against 18.3 M, 1.35 times) and 2.59 ns of the card an instruction of all cores against 1.82 (1.42 times), with 18.5 passes a picture against 30. That second factor fits gpu_vm's cores of one warp (its own RESULTS: 1.98 to 0.93 ns a bundle by shape alone; inferred, not measured here), and `docs/machine/multicore.md`'s layouts move nxray from 93 ms to 58. Compiled float code and a float's time were not compared apart.

**+25%.** By bundles it is out of reach on these figures. +25% of the tick is 1.17 ms against lean's 1.465: 139 ns an RV instruction. With 135 ns a trip and 121 ns an RV (inferred), 3 RV a bundle, more than `pack2.py` gives at six ops, is 166 ns: +5% on lean. Reaching 139 at 3 RV a bundle wants the op's cost down from 121 to 94 with the trip as it is; at today's 2.04 RV a bundle, to 73. What would do it is fewer card instructions on the way through an op, the 74% above, and that was not built. Real gain on real programs from this work: none that passes; lanes already give nxray more than gpu_vm was ahead by.
