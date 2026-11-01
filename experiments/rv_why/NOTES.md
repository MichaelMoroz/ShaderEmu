# Why gpu_vm loses on Tiberian Dawn, and hot code translated into the tick (1 November 2026)

The target: +25% on Tiberian Dawn against lean, passes as long and as rarely ended full. **Not reached.** Best measured on Tiberian Dawn under the constraint is lean itself; the routes tried here give 0.96x (translated code) and 0.98x (gpu_vm with a new write cache form). Every card number is the RTX 5070 Laptop through `tools\gpu_queue.ps1`, one run unless it says otherwise.

| script | what |
|---|---|
| `gvmprof.py` | gpu_vm's bundles on a load, from `gvm_cpu --profile` (the reference, not the card) |
| `snap.ps1`, `run.py`, `row.py` | a load's snapshot and profile on the processor; from that snapshot on the card to the load's end; `results.jsonl` |
| `aot.py`, `mk_tick.py`, `aot_loop.inc`, `try.sh` | hot RISC-V as HLSL in a copy of lean; makes `experiments/rvc_lean_aot` (ignored by git) |
| `wcform.py`, `wcform.jsonl` | gpu_vm's write cache forms in a private copy of `gvm_cpu`'s model (`C:\Users\micha\agents_isa\gvmref`) |
| `mk_sil.py`, `h2hshow.py`, `h2h.jsonl` | gpu_vm's tick with a store of what is there taking no entry; `rv_vm/h2h.py`'s runs of it |

## The result table

Two windows. "Snapshot" is from just after the program started to its end (cold boot is gone), lean and the variant from one snapshot. "Cold" is `rv_vm/h2h.py`'s. `x` is lean's time over this one's.

| load | window | lean ms (runs) | lean a pass, full | this ms (runs) | x | this a pass, full | what |
|---|---|---|---|---|---|---|---|
| Tiberian Dawn | snapshot | 24,532 (1) | 7,735, 1.9% | 25,603 (1) | 0.96 | 7,740, 1.9% | translated, 2,000 words |
| Tiberian Dawn | snapshot | 24,532 (1) | 7,735, 1.9% | 32,263 (1) | 0.76 | 7,749, 1.9% | the hook and no code |
| gzip | snapshot | 13,169 (1) | 8,146, 0.3% | 13,109, 13,141 (2) | 1.00 | 8,143, 0.3% | translated, 1,000 words |
| gzip | snapshot | 13,169 (1) | 8,146, 0.3% | 15,875 (1) | 0.83 | 8,143, 0.3% | the hook and no code |
| Tiberian Dawn | cold | 27,384 (2) | 7,596, 4.3% | 28,024 (1) | 0.98 | 6,536 ops, 30.6% | gpu_vm, silent stores |
| gzip | cold | 14,542 (2) | 8,130, 0.3% | 9,859 (1) | 1.47 | 9,795 ops, 11.3% | gpu_vm, silent stores |
| png, no sum | cold | 2,661 (1) | 8,079, 0.6% | 1,787 (1) | 1.49 | 10,485 ops, 25.8% | gpu_vm, silent stores |
| jpeg, no sum | cold | 414 (1) | 5,643, 23.0% | 430 (1) | 0.96 | 3,796 ops, 52.5% | gpu_vm, silent stores |
| boot | cold | 1,604 (2) | 3,790, 1.3% | 2,254 (1) | 0.71 | 1,844 ops, 21.3% | gpu_vm, silent stores |
| mpeg, 24 | cold | 12,258 (1) | 6,151, 8.1% | 10,636 (1) | 1.15 | 9,435 ops, 14.1% | gpu_vm, silent stores |
| avi, 64 | cold | 2,303 (1) | 2,858, 3.5% | 5,240 (1) | 0.44 | 2,816 ops, 13.6% | gpu_vm, silent stores |

gzip's and png's 1.47x and 1.49x still break the constraint (11% and 26% full against 0.3% and 0.6%). State `1a9a814b` on every Tiberian Dawn row. The mpeg and avi rows printed their fnv lines, not compared here with lean's by hand.

## Lead 1: what Tiberian Dawn does that gzip does not

**gpu_vm's tree against lean, from `rv_vm`'s runs and `gvmprof.py`.** Bundles a run on the card are inferred: the log's ops over the reference's ops a bundle. The reference ran more bundles on Tiberian Dawn than the card (118 M against about 88 M inferred: its pass clock differs, so its counts are a mix, not the card's totals).

| | gzip | Tiberian Dawn | |
|---|---|---|---|
| lean's instructions | 90 M | 143.5 M | measured |
| gpu_vm's ops, card | 53.6 M | 128.5 M | measured |
| ops a bundle | 1.27 | 1.45 | reference |
| **lean's instructions a bundle** | **2.13** | **1.62** | inferred |
| wall a bundle | 201 ns | 300 ns | inferred |
| less 0.2 ms a pass | **172 ns** | **233 ns** | inferred |
| memory ops of ops | 45% | 57% | reference |
| stores of ops | 9.3% | 19.1% | reference |
| bundles off the fast loop (`step12`, general; every call, return and far jump) | 3.8% | 15.6% | reference |
| bundles with two memory ops | 11% | 25.8% | reference |
| passes against lean's | 0.57x | 1.54x | measured |
| runs: half of them in | 5 bundles | 258 bundles | reference |
| 90% of them in | 126 | 4,938 | reference |

So the 1.71x on gzip and 1.03x on Tiberian Dawn are three things multiplied, and none is a far branch or i-cache in the guest's sense alone:

1. **Density, 2.13 against 1.62.** gzip's time is a handful of bundles the compiler packs fully. On gzip a gpu_vm op (158 ns of wall) costs what a lean instruction does (161 ns): all of its gain is doing the work in 60% of the ops.
2. **A bundle's price, 172 against 233 ns**: Tiberian Dawn's bundles hold twice the stores, more double memory ops and four times the slow step. Not split further on the card.
3. **The passes**: 29,154 against lean's 18,900, 59% ended full.

What would be left if the third were free, inferred: 233 ns over 1.62 is 144 ns a lean instruction against lean's 164 net, so about 1.14x. That is the ceiling of "gpu_vm with a cache that holds", and gzip's speed on this code is not on offer: gzip's is density, which this code does not have.

## Hot code as HLSL in the tick (measured: rejected)

Not tried before: no fetch, no decode and no register index for hot code. Each stretch of words up to a memory or control op is a case of a `switch`; arithmetic is in the shader with temporaries; a memory op is lean's `ram_word`/`ram_store` on one shared path. Entered at a lookup of a two-way table of the pc (an immediate constant buffer) at every `!same` in lean's fast loop.

It is right: Tiberian Dawn's state `1a9a814b` at each size that finished, and with kernel-only code `mctest-3` passed on WARP (542 s and 506 s, 4x slower than lean there). Only tried from a snapshot, where the code's address is known; a real one needs the program's base as state, not built.

| Tiberian Dawn, words | share of the profile | tick ms a pass | wall ms |
|---|---|---|---|
| lean | | 1.411 | 24,532 |
| 1 (the hook alone) | 0.7% | 1.892 | 32,263 |
| 1,000 | 41.0% | 1.570 | 26,993 |
| 1,500 | 48.6% | 1.533 | 26,346 |
| 2,000 | 54.4% | 1.489 | 25,603 |
| 3,000 | 62.3% | 41.3 | stopped at 60 s |

| gzip | tick ms a pass | wall ms |
|---|---|---|
| lean | | 13,169 |
| 1 (the hook alone) | 1.442 | 15,875 |
| 100 | 1.257 | 13,992 |
| 200, switch | 1.257 | 14,020 |
| 1,000, switch | 1.172, 1.178 | 13,109, 13,185 |
| 1,000, switch, cold cases first | 1.175 | 13,141 |
| 1,000, a Huffman tree of `if`s for the switch | 1.204 | 13,438 |
| 100, 23 memory ops made in the fragment | 1.216 | 13,678 |
| 2,000 | 18.9 | stopped |
| 6,000 | 16.3 | stopped |

What it says:

- **A translated word is about 0.81 of lean's, not a fraction.** Inferred from Tiberian Dawn: 1.892 down to 1.489 for 54% of the words is 1.146 ms a pass against 1.411. Without the hook's cost at this coverage, about 1.11x of the tick (inferred). So no fetch, no decode and no register index buy under a fifth, on code half of whose fragments are one memory op and one dispatch. A trip of the loop (dispatch, a test, the shared path, back) is the cost: gzip's about 240 ns a fragment of 2.4 words (inferred). This agrees with `rv_fresh`: a way out costs more than the operations inside.
- **The hook costs 20 to 31% of everything else.** A table lookup and a compare at every texel or jump; it was not moved to taken jumps only, which would need the fast loop's shape changed again (not built).
- **The `switch` is not a row of compares**: hot cases first and last are 1.172 and 1.175 ms at 465 cases, and it beats a Huffman tree of depth 7.4. So on the D3D11 driver a dense `switch` is constant time. This goes against the tick's "no switch" for a dispatch of many cases; for the fast step's few kinds it was measured otherwise and is not touched.
- **An inlined memory op is about 6 KB of bytecode and 3% at best.** Not the way out.

### The size cliff (measured; new)

At a certain size of the tick's bytecode the whole shader becomes 10 to 25 times slower, all at once, code that runs or not:

| gzip | bytecode | tick ms a pass |
|---|---|---|
| 1,000 words | 486 KB | 1.17 |
| 100 words, 23 inlined | 541 KB | 1.22 |
| 200 words, 53 inlined | 696 KB | 18.9 |
| **200 words entered, 2,000 present and never run** | | **21.0** |

Between 541 and 696 KB (lean itself about 380 KB, inferred from 100 words at 401 KB). The fourth row settles that it is the size and not running: the same hot code with never-entered code beside it. Tiberian Dawn at 2,000 words ran and at 3,000 did not. Cause not looked at: D3D11 only, so Nsight cannot see the same code. So any "large tick" route (translated code, inlined paths, unrolled forms) has about 150 KB of bytecode more than lean to spend: **about 2,000 words of guest code, 54% of Tiberian Dawn.**

## Lead 3: why gpu_vm's passes end full (answered: partly the form)

**A store of what is there already takes no entry** in lean (`if (val == cur) return` in `ram_store`) and takes one in gpu_vm. In the model (`wcform.py`, `gvm_cpu --wc`, not the card; 8,192 bundles a pass):

| full %, Tiberian Dawn | 168 | 219 | 336 | 672 |
|---|---|---|---|---|
| two tables of four | 59.5 | | | |
| three tables of three | 46.1 | 36.8 | 18.0 | 4.8 |
| three tables, silent stores | **17.2** | | **5.2** | **2.7** |

| full %, others, 168 three tables / silent; 336 / silent | |
|---|---|
| gzip | 9.2 / 4.2; 2.4 / 1.0 |
| png | 13.1 / 9.6; 2.3 / 1.2 |
| jpeg | 69.8 / 59.9; 40.0 / 30.1 |

On Tiberian Dawn it is worth more than doubling the entries; texels a budget pass 75 to 50 at 336.

**On the card with two tables of four and 170 entries, silent stores alone (`mk_sil.py`), measured:** Tiberian Dawn 59.1% to 30.6% full, passes 29,154 to 19,384, and the wall **worse**, 26,510 to 28,024 ms. gzip 21.3% to 11.3%, 8,506 to 9,859 ms. png 29.0% to 25.8%, 1,797 to 1,787 ms. Every store must now read RAM and compare, and passes had not been what the time goes to: a pass's fixed cost was already small against the ops. Three tables under the array step (219 entries) not built on the card.

So it is the form, and fixing the form does not pay for itself on the card.

## Lead 2: the 225-entry step (partly answered)

`exp_c0_arr225` and `exp_c0_arr250` with `-D CORE0`, traced under `gvm12` (D3D12, DXIL, one core on `arith`): both 96 registers a thread, both 4,696 card instructions, `LDL` 269 and `STL` 187 both, tick 845.0 us a pass both. The two listings differ in local memory offsets. So **it is not registers, instructions or the code's form**; the D3D12 case had no workers, so the step itself was not reproduced there and this is not proof for D3D11. What is left is the local memory size (inferred). `nsight_table.py` said "no Shader Pipelines table", so warps an SM were not read. Not gone further.

## Not done

- WARP stamp: `warp_ok` of `rvc_lean_aot` covers the hook with kernel-only code. `aot_gen.inc` is not hashed by the stamp on purpose, so each load's translation ran on the card with that stamp, checked by the load's state sum and nothing else. Experiments only.
- gpu_vm's tick `exp_sil.hlsl` was not run through `gvm.exe`'s WARP check (`vm.exe` does not ask); its check is state `1a9a814b` and the fnv lines.
- Nothing in lean, `rvc_cpu`, Unity or the machine changed.
- Lean's rows from snapshot are one run. Cold lean rows are `rv_vm` and `rv_acct`'s, of the same morning, not alternated with these.

## What might still move Tiberian Dawn (not tried; inferred)

- gpu_vm's density comes from its compiler and 64 registers, at a price a bundle. What it gets on Tiberian Dawn is 1.62, and its bundle there costs 233 ns; no cache form changes either.
- The dearest thing everywhere here is a trip of a loop with its branches. Nothing tried makes a branch on this card cheaper; `rv_fresh`'s SASS says `ISETP` before `BRA` 13 clocks and `BSSY` 12, emitted because the driver must assume lanes diverge, and all a core's lanes never do. A way to tell it so under DXBC is what a further attempt should look for. Not found here.
