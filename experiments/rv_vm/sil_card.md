# Silent stores on the card, against lean (1 November 2026)

gpu_vm's tick with silent stores, `tick/exp_sil.hlsl` (GPU-VM `isa-cache`, 97fb480: a store equal to memory takes no entry of the write cache), against this repository's lean tick.

**Result: silent stores do not pay on the card.** They halve Tiberian Dawn's full passes, from 59% to 31%, but it gets 5% slower. All sums are right. Workers lose 1 to 5%.

**Measured:** flat on the card (RTX 5070 Laptop), cold boot each, alternating sides, through `h2h.py` and `tools\gpu_queue.ps1`. Time is wall between the guest's marks, from `--frame-log`; boot is the time to the first mark. Lean is the image of 1 November 18:20, gpu_vm its own of 31 October. Raw runs: `logs/sil_card.jsonl`, not committed.

| load | lean ms | this ms | x | passes, lean / this | a pass, lean instructions / this ops | full, lean / this | sums |
|---|---|---|---|---|---|---|---|
| Tiberian Dawn, mission 3 to frame 1300 | 21,754 | 27,837 | 0.78 (see below) | 14,843 / 19,366 | 7,698 / 6,536 | 1.8% / 30.7% | state `1a9a814b`, all 6 runs and the tree's 3 |
| mp3, no workers | 46,277 | 34,572 | 1.34 | 30,908 / 23,489 | 7,728 / 8,020 | 10.7% / 77.1% | `fd612a25`, all 6 |
| boot to the shell | 1,210 | 2,227 | 0.54 (see below) | 1,869 / 4,607 | 2,857 / 1,953 | 1.5% / 22.6% | none exists |
| jpeg, nxview | 3,559 | 3,500 | 1.02 | 2,744 / 1,511 | 7,895 / 8,490 | 2.6% / 29.2% | `95b8fd40`, all 6 |

3 runs a side. Instructions and ops are different units: gpu_vm's budget is 8,192 bundles, not 8,192 ops. x above 1 means this tick is faster. No run was discarded: walls within a side agree to 1.4% (boot's lean), 1.0% (boot on this tick), 0.3% or better for the rest. No Oops in any run.

**Lean moved under two rows.** The lean walls for Tiberian Dawn and boot come from an image rebuilt today, with today's guest changes (Tiberian Dawn's `linux/tdawn` commits among them); gpu_vm's image has none of them. So 0.78 and 0.54 are not the tick's alone. Against the lean walls in `NOTES.md` (27,384 and 1,604, from this morning's image) the rows are 0.98 and 0.72. mp3 and jpeg did not move (46,118 and 3,563 then). A fair Tiberian Dawn comparison wants gpu_vm's image rebuilt from today's game.

## Against gpu_vm without silent stores

| load | tree ms | this ms | this against tree | full, tree / this | when the tree ran |
|---|---|---|---|---|---|
| Tiberian Dawn | 26,490 | 27,837 | 5.1% slower | 59.1% / 30.7% | today, alternating, 3 runs |
| mp3, no workers | 30,390 | 34,572 | 12% slower | 83.0% / 77.1% | `NOTES.md`, this morning |
| jpeg | 3,111 | 3,500 | 11% slower | 41.1% / 29.2% | `NOTES.md`, this morning |
| boot | 2,025 | 2,227 | 9% slower | 29.5% / 22.6% | `NOTES.md`, this morning |

The model's 23% full on Tiberian Dawn at 168 entries is not reached: the card says 30.7%. The model's 59.5% without silent stores does match the card's 59.1%.

## Workers: not as without silent stores

| load | work instructions, tree / this | wall ms, tree / this | workers' instructions a second of the work's wall, tree / this | change | full, tree / this |
|---|---|---|---|---|---|
| mpeg, 8 workers (2 runs a side) | 531.8 M / 531.6 M | 45,546 / 47,992 | 11.68 M / 11.08 M | -5.1% | 19.1% / 3.6% |
| mp3, 63 workers (2 runs a side) | 165.1 M / 165.0 M | 4,468 / 4,527 | 36.96 M / 36.45 M | -1.4% | 54.3% / 42.0% |
| Tiberian Dawn (3 runs a side) | 93 M / 92 M | 26,490 / 27,837 | 3.51 M / 3.32 M | -5.4%, bound by core 0 as well | 59.1% / 30.7% |

The harness's own workers' ips, over the whole run with boot, say the same or a little more: mpeg -5.2%, mp3 -3.6%, Tiberian Dawn -5%. This is not the step of `NOTES.md` (a third lost, by the arrays' size): the arrays did not change. **Inferred:** the cost is the texel read that every store now makes. Not measured apart.

Sums of these: mpeg's pictures `d124dc14` and sound `96a296a2`, fnv `31090635`, on all 4 runs; mp3's `fd612a25` on all 4 runs.

## Sums, and the race asked about

Every sum in every run is right: 13 runs of this tick with a sum, on loads with workers among them. A silent store does read memory another core may change in the same pass, and that did not show. This says only that these loads, at these runs, came out right; it is not a proof, and this tick has not been run under a race model. **The tick has no WARP stamp:** `vm.exe` does not ask for one.

## The clock

`nvidia-smi` every 2 to 3 s, `logs/sil_card_clock.csv`: 2,835 MHz on the long loads on both sides (mean of busy samples 2,832 to 2,837; maximum given 3,090), hottest 55 C. Short dips (1,417 to 1,995 MHz) were sampled seven times, one of them in mpeg on this tick (mean there 2,807 against the tree's 2,835), and those two runs differ by 0.8%: that much of mpeg's 5.1% may be the clock, the rest is not. Boot had only 2 or 3 samples a run, too few to say anything of its clock. The reasons word read `0x400` on all samples but one (`0x4`), on both sides; not decoded.

## Not explained

jpeg on this tick took 3,500.1, 3,500.2 and 3,500.1 ms, with passes between 1,509 and 1,514. Walls that close, with passes that differ, suggest something other than the tick bounds the wall. Its median pass is 2.76 ms against lean's 1.28, and about 200 ms lies in passes over 5 ms. Treat 1.02 as a weak number.

## Timed before

`experiments/rv_why/NOTES.md` has one run each of this tick on Tiberian Dawn (28,024 ms, 30.6% full) and boot (2,254 ms, 21.3%), and of jpeg without its sum. These runs agree with them.
