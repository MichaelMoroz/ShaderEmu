# gpu_vm with a cache that holds a pass, against the lean tick (1 November 2026)

The question: start from gpu_vm, which is faster flat, and remove its one fault, passes that end early with a full write cache. Is +25% on real cases left under the constraint (passes last about as long as lean's and do not end full much more often)?

**Answer: no.** With the cache large enough to hold the constraint, gpu_vm is 1.56x on gzip and 1.25x on serial mp3, and behind lean on Tiberian Dawn (0.78x), boot (0.72x), mpeg (0.89x) and the sound decoded on workers (0.55x to 0.74x). Tiberian Dawn is only 1.03x even without the constraint, at 59% of passes ended full.

Everything is flat on the card (RTX 5070 Laptop), cold boot each, through this repository's `tools\gpu_queue.ps1`. Time is wall between the guest's marks, from `--frame-log`. Scripts here; ticks and host changes on GPU-VM's branch `isa-cache`.

| script | what |
|---|---|
| `h2h.py`, `all.ps1` | one load on one machine, each load on each machine in turn; `results.jsonl` |
| `table.py`, `show.py` | the tables below |
| `model.py` | gvm_cpu's cache model over loads and shapes; `model.jsonl`, `model6200.jsonl` |
| `mk_c0.py` | makes `tick/exp_c0_N.hlsl` from GPU-VM's tick, run in `tick/` |

## The table

`x` is lean's time over this one's: above 1 gpu_vm is faster. "full" is the share of passes that ended full. **Measured.** Lean and the tree tick are 2 runs each, which agree to about 1% (mpeg 2%); c722 is 1 run (c370 and c514 agree with it within 2% where all ran, on gzip, mpeg, Tiberian Dawn and boot).

| load | lean ms | lean full | tree ms | tree x | tree full | core 0 at 722 entries ms | x | full |
|---|---|---|---|---|---|---|---|---|
| boot to the shell | 1,604 | 1.3% | 2,025 | 0.79 | 29.5% | 2,226 | 0.72 | 2.5% |
| gzip | 14,542 | 0.3% | 8,506 | 1.71 | 21.3% | 9,338 | 1.56 | 1.0% |
| jpeg, nxview | 3,563 | 2.7% | 3,111 | 1.15 | 41.1% | 3,193 | 1.12 | 1.8% |
| png, nxview | 5,714 | 0.2% | 4,361 | 1.31 | 27.9% | 5,498 | 1.04 | 0.2% |
| mp3, no workers | 46,118 | 10.7% | 30,390 | 1.52 | 83.0% | 36,909 | 1.25 | 0.1% |
| mp3, 63 workers | 3,982 | 3.6% | 4,466 | 0.89 | 54.1% | 5,356 | 0.74 | 0.6% |
| flac, 63 workers | 1,586 | 1.7% | 2,022 | 0.78 | 40.2% | 2,902 | 0.55 | 0.7% |
| mpeg, 8 workers | 61,540 | 1.5% | 45,207 | 1.36 | 19.1% | 69,368 | 0.89 | 0.5% |
| avi, 16 workers | 29,827 | 0.2% | 28,867 | 1.03 | 18.4% | 30,449 | 0.98 | 0.2% |
| Tiberian Dawn, mission 3 to frame 1300 | 27,384 | 4.3% | 26,550 | 1.03 | 59.1% | 35,140 | 0.78 | 7.9% |

Work per pass, in each machine's own unit, so not directly comparable: lean is instructions (budget 8,192). gpu_vm's log counts ops, and its budget is 8,192 bundles, which is 10,000 to 16,000 ops depending on the load.

| load | lean a pass | tree a pass | 722 entries a pass |
|---|---|---|---|
| boot | 3,790 | 1,823 | 2,503 |
| gzip | 8,130 | 8,533 | 10,256 |
| jpeg | 7,899 | 7,115 | 9,874 |
| mp3, no workers | 7,727 | 6,713 | 14,016 |
| Tiberian Dawn | 7,596 | 4,416 | 6,654 |

The same work on both where a sum exists: Tiberian Dawn's state `1a9a814b`, the jpeg's `95b8fd40`, the sound and fnv sums of mpeg and avi, also at 722 entries. gzip and boot have no sum. **The exp ticks were not run through gvm.exe's WARP check**: vm.exe does not ask for the stamp, and those sums are the only check on them.

Boot is the least reliable row as a speed: 57 to 70% of gpu_vm's boot passes and 54% of lean's are idle waits, so it is partly bound by the guest's clock and not the tick. **Inferred**, not measured apart.

## Why its passes end full

**Measured in the model** (`gvm_cpu --wc`, core 0, the load's window; not the card). It agrees with the card on Tiberian Dawn (59.5 against 59.1% full at 168 entries) and less well elsewhere: gzip 10 against 21%, boot 71 against 30%.

- **It is the size, not what the ISA stores.** Texels a pass at a budget end with a cache that never fills: Doom 395, jpeg 280, boot 185, mp3 serial 201, Tiberian Dawn 100, gzip 48. Those are of the order lean's cache holds, in 8,192 bundles, where lean's 8,192 instructions are about 5,800 to 7,100 bundles (from the two machines' totals on the same work).
- **The rest is bursts**: Tiberian Dawn's full ends come at about 1,000 texels in 3,000 bundles, which no cache of this size holds.
- **Budget ends, entries: share of passes ended full** (8,192 bundles a pass):

| load | 168, 2 x 4 | 368 | 512 | 336, 3 x 3 | 504, 3 x 3 | 672, 3 x 3 | 1024 | 2048 |
|---|---|---|---|---|---|---|---|---|
| boot | 71.4 | 32.2 | 16.6 | 24.7 | 11.4 | 3.7 | 1.9 | 1.1 |
| gzip | 10.3 | 2.6 | 1.6 | 2.4 | 1.5 | 0.9 | 0.7 | 0.7 |
| jpeg | 72.5 | 45.0 | 31.2 | 40.0 | 24.8 | 14.2 | 7.3 | 0.8 |
| mp3, no workers | 78.4 | 24.9 | 1.6 | 24.3 | 0.4 | 0.0 | 0.0 | 0.0 |
| mpeg | 4.9 | 1.1 | 0.7 | 1.0 | 0.6 | 0.3 | 0.1 | 0.1 |
| Tiberian Dawn | 59.5 | 22.2 | 12.8 | 18.0 | 10.5 | 4.8 | 3.2 | 2.0 |
| Doom | 90.9 | 75.0 | 57.0 | 68.9 | 35.8 | 13.2 | 7.7 | 0.4 |

- Three tables of three (`GVM_WC3`) hold what two tables of four hold with about two thirds of the entries: at 336 entries they fill to 87% before a full end, and two tables of four at 512 to 72%. Modelled only, not built on the card.
- A budget of 6,200 bundles, about lean's work a pass, moves little: Tiberian Dawn at 720 entries 3.9%, jpeg 11%, Doom 11%, boot 4% (`model6200.jsonl`).

## The large cache

Core 0 has its own compile (`-D CORE0`) and its own draw, a column 4 pixels wide with its warps down the first rows; workers stay 4 x 8 with 170 entries. `harness/vm/backend.cpp` changes three lines for this.

- 786 entries lost the device on the first draw. 722 runs. The limit is inferred to be the arrays' size; only that one size was seen to fail.

**Where the time goes, measured, each cause alone:**

| variant | gzip | mp3 serial | jpeg | mp3 63 | mpeg | Tiberian Dawn |
|---|---|---|---|---|---|---|
| tree | 1.71 | 1.52 | 1.15 | 0.89 | 1.36 | 1.03 |
| core 0 apart, same 170 entries (`exp_c0_170`, `GVM_SPLIT0=1` on the tree) | 1.71 | | | | 1.35 to 1.36 | 1.03 |
| 2 warps for core 0, same 170 entries (`exp_c0_lan`) | 1.60 | | | | 1.35 | 0.97 |
| arrays of 722 in core 0's compile, still 170 entries used (`exp_c0_arr`) | 1.71 | 1.54 | 1.14 | 0.78 | 0.81 | 0.87 |
| 370 entries, used | 1.57 | 1.27 | 1.12 | | 0.89 | 0.80 |
| 514 entries, used | 1.57 | | | | 0.89 | 0.79 |
| 722 entries, used | 1.56 | 1.25 | 1.12 | 0.74 | 0.89 | 0.78 |

- **Drawing core 0 apart costs nothing.** So the second draw is not the cost.
- **A step in the workers, by the array's size alone.** Only core 0's compile has the larger arrays, no entry more is used, and the workers' own compile is the same. mpeg, by the array's entries: 200 gives 1.35x, 225 gives 1.36x, 250 gives 0.81x, 300 gives 0.81x, 722 gives 0.81x. Loads with no workers do not feel it (gzip, serial mp3). So above about 225 to 250 entries (about 5 KB a lane of cache arrays, 20 bytes an entry) every load with workers drops; at mpeg, workers go from 11.0 to 7.4 million instructions a second, and Tiberian Dawn's from 2.95 to 2.29. It does not grow past the step.
  **Inferred**: the driver sizes per-lane local memory by the largest shader, and past a size fewer warps fit an SM at once. Not checked in Nsight. RESULTS.md's row of 25 October rejected a one-draw core 0 for "the arrays are core 0's size in every core"; two compiles do not escape it.
- **Serial loads lose by more bundles a pass being dearer**: 6 to 18% an op from tree to 370 (gzip 0.158 to 0.172 us an op, serial mp3 0.162 to 0.192), and no more to 722. Second warp alone: gzip 1.71 to 1.60.

## What bounds it

1. 225 to 250 entries is the most core 0 can have before workers lose a third of their speed; the constraint wants 700 or more (about 500 with three tables, modelled). There is no size that gets both.
2. Keeping the constraint costs serial work 6 to 18% more an op, so 1.71x and 1.52x become 1.56x and 1.25x. These two are the only rows at +25% under the constraint.
3. Tiberian Dawn has no +25% to give: 1.03x with passes ended full at 59%, and it is one of the cases asked for. Its workers run 2.3 to 3 million instructions a second through the run, so the step reaches it.

Not tried, and each is a new route, not tuning:

- A word-sized cache for core 0 (tag and word 8 bytes, against 20), to get more stores under the step. Whether stores are sparse enough in a texel was not measured.
- Choosing core 0's compile per pass: 722 while no worker runs, 170 while any does. The state's write cache is empty at a pass's start, so it could switch; but passes with workers would end full as today, which breaks the constraint where workers run, and whether the driver's size falls back is unknown.
- Nsight on the step, to see what actually changes.

## Queue

GPU-VM's `tools\gpu_queue.ps1` on its main has another mutex's name (`...-gpu-run` under an old name of this project), so the two queues do not exclude each other. All runs here went through this repository's copy, one at a time, so none overlapped. Branch `isa-cache` changes GPU-VM's copy to the same mutex; GPU-VM's main still has the old one.
