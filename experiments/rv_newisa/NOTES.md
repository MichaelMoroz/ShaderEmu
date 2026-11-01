# Where a new ISA's gain could come from, priced on the card (1 November 2026)

The question: +25% wall on Linux boot, media and Tiberian Dawn from a new instruction set and its tick, passes as long and as rarely ended full as lean's. **Not reached, and no route here came near.** The best whole-load result is png 1.056x; the same form is 0.825x on mp3. A from-scratch tick with a new encoding was **not built**: what it could give was priced first, on lean's own body and on real snapshots, and the prices leave no room for it (below; that step is inference from those prices, not a run).

RTX 5070 Laptop through `tools\gpu_queue.ps1`, D3D11, fxc2. Snapshot windows repeat to 0.1 to 0.3% on the same load (3 side by side, `ab.jsonl`); that is why these are small numbers worth stating. Clock 2,835 MHz, except the first hour's runs (`e2.jsonl`, the lanes): the card was at 1,327 MHz, 5.5 W, reason not found; it left that state by itself. Those rows compare only within themselves.

| script | what |
|---|---|
| `ab.py`, `ab.jsonl` | variants against the tree in turn, one snapshot, warm |
| `snaprun.ps1` | one window with the clock read halfway |
| `whole.ps1`, `whole_show.py`, `whole.jsonl` | whole loads, cold boot, in turn (`rv_vm/h2h.py`) |
| `traces.ps1`, `trace.patch`, `features.py`, `features.json`, `stackwin.py` | core 0's real traces under `rvc_cpu` and what they count |
| `lean_ni.patch` | the variants, as `#ifdef NI_*`, on a copy of `rvc_lean` (`experiments/rvc_lean_ni`, not committed: git ignores `rvc_lean_*`; apply to a copy, and `--define NI_X`) |

Every tick source that ran on the card passed `mctest-3` on WARP as stamped, with the define named in the run. Defines tried later on the same stamp ran on the card checked by their sums only (`whole.jsonl`, `ab.py` has no sums).

## Step 1: what the traces count (measured, `features.py`; 6 M instructions of core 0, boot 4.9 M)

| | boot | Tiberian Dawn | gzip | mp3, one core | jpeg, one core |
|---|---|---|---|---|---|
| arithmetic | 43% | 38% | 52% | 19% | 55% |
| loads | 20% | 27% | 13% | 22% | 18% |
| stores | 13% | 8% | 11% | 8% | 12% |
| branches | 15% | 19% | 15% | 4% | 8% |
| floats | 0 | 0 | 0 | 42% | 0 |
| anything taken | 12% | 11% | 13% | 2% | 3% |
| entering a texel by a jump | 10% | 6% | 11% | 2% | 3% |
| instructions a block | 8.3 | 8.7 | 7.8 | 51 | 33 |
| loads in the last memory op's texel | 39% | 17% | 43% | 21% | 9% |
| loads hitting a direct-mapped 64 texels | 83% | 63% | 91% | 79% | 87% |
| loads that find a texel stored this pass | 47% | 22% | 77% | 21% | 30% |
| memory ops off sp | 36% | 23% | 69% | 9% | 10% |
| store texels a pass, mean / 95th | 148 / 390 | 133 / 296 | 46 / 107 | 142 / 332 | 75 / 121 |
| store words a pass, mean / 95th | 451 / 1,218 | 340 / 945 | 130 / 306 | 482 / 1,223 | 255 / 358 |
| arithmetic then arithmetic, adjacent | 23% | 16% | 32% | 11% | 33% |
| compare or arithmetic into the next branch | 4.1% | 7.9% | 5.5% | 0.6% | 1.7% |
| lui or auipc then addi | 0.7% | 3.7% | 1.4% | 2.8% | 1.9% |
| arithmetic into the next memory op's address | 2.3% | 3.1% | 1.1% | 2.5% | 7.6% |
| two loads in one texel | 4.8% | 3.2% | 3.8% | 3.0% | 1.4% |
| two stores in one texel | 4.8% | 2.8% | 3.9% | 1.7% | 3.2% |

Word-sized write cache: **against the trace, no.** A word a pass is 2.5 to 3.5 times a texel, so at 8 bytes a word it holds what texels hold at about the same bytes. A stack window fixed at a pass's start (`stackwin.py`, [sp - 128, sp + 384)): loads 11%, stores 30% on Tiberian Dawn, 21 / 33 on boot, 8 / 21 on mp3: about 5 to 8% of instructions (inferred at most 3 to 5% of the wall): not built.

## Step 2: prices of the parts, on the card

**Added to every instruction of lean's fast loop, one snapshot (gzip, `NI_PROBE`):** ns an instruction, base 162.7.

| | ns more |
|---|---|
| a read of `xr`, another register | 0 to 2 |
| two more reads of `xr` | 0 |
| one word of the cache array, a fixed place | 5 to 8 |
| one word of the cache array, a place that changes | 20 |
| a whole texel of the cache array, a place that changes | 53 (four loads) |
| a texture read used at once, reach 1 KB, 64 KB or 16 MB | 47, 48, 49 (the reach does not matter) |

So: a register costs nothing, 64 or 256 of them would be free to read; a texture read is its latency, not the card's caches; and the write cache's array is a real memory, not free as `xr` is. **A frozen pass (`RVC_FREEZE`, every texture's caches warm) is no faster than a pass moving on** (334 frames a second against 358, gzip, at the low clock).

**By pixels:** the frozen pass drawn on 1, 4, 32 or 64 pixels: 341, 341, 334, 334 frames a second. Going from 64 pixels to one warp's 32 gains nothing; 4 or 1 pixels gain 2%. The redundant lanes are not a cost worth a design.

**The local array's step (`rv_vm`'s 225 entries) on lean: none.** Lean with arrays of 224 (168 entries) against 448: avi at 64 workers 4,690 and 4,700 ms, full 3.3 to 9.7%; mpeg 25.0, 28.4 against 28.6, 25.3 s (that row's noise is large), full 8.1 to 17.9%. Lean loses nothing to the size, and a smaller cache only ends more passes.

## Step 3: each shape and feature on real code (x above 1 is faster; 2 or 3 runs a side, each side's runs within 0.3% in one mode)

| variant | what | gzip | Tiberian Dawn |
|---|---|---|---|
| `NI_SWITCH` | one dispatch on the opcode's bits, `[forcecase]` | 0.875 | 0.895 |
| `NI_FETCH1` | no window: a texture read every instruction (2 runs) | 0.870 | 0.858 |
| `NI_CONT` | arithmetic, loads and branches straight back to the loop's head | 1.008 | 1.002 |
| `NI_ONE` | one test an instruction: loop's end and the way out folded into the fetch's test | 0.992 | 0.991 |
| `NI_CHAIN` | a row of tests by frequency: arithmetic one test, a load two | 1.013 | 0.971 |
| `NI_CHAIN,NI_ONE` | both | 1.006 | 0.953 |
| `NI_ADDI` | addi alone, 45 to 60% of arithmetic (inferred share) | 0.993 | 1.009 |
| `NI_PAIR` | a second arithmetic word of the texel in the same trip | 1.022 | 1.018 |
| `NI_ADDI,NI_PAIR` | | 0.997 | 1.006 |
| `NI_ALU_OK` | no legality test of arithmetic (what a validated encoding gives) | 1.021 | 1.014 |
| `NI_LOAD_OK` | fewer tests of a load | 1.005 | 1.005 |
| `NI_CUT_FP` | floats out of the loop to the general step | 1.011 | 0.988 |
| `NI_CUT_FP,NI_CUT_MISC` | M, atomics and CSRs too | 1.023 | 1.010 |
| all four checks and cuts | **the ceiling of what an encoding alone removes** | 1.037 | 1.032 |
| `NI_RC=4`, 6, 8 | a read cache of texels, 16 to 256, own arrays | 0.873, 0.858, 0.822 | 0.876, 0.868, 0.851 |
| `NI_RCC=4`, 5 | the same inside the cache's one array | 0.898, 0.879 | 0.902, 0.878 |
| `NI_RC=4,NI_RCX` | the arrays there, not used | 0.999 | 0.994 |
| `C_N=512` | a larger array, not used | 0.999 | 1.000 |

Tiberian Dawn's window has two speeds run to run from one snapshot with the same instructions a pass to 0.05%: 170 and 216 ns an instruction, 1.25 and 1.63 ms a tick, on the tree itself and every variant (`ab.jsonl`). A pass's count differs in the second decimal between them, so it is read as two guest paths from the timing of the start, not a state of the card: inferred, not proved. Where a side has runs of both, the table compares the runs of the fast one by hand (so `NI_PAIR` and `NI_ADDI,NI_PAIR` rest on one run there). Before 18:20 the same snapshot's window read 191 ns with workers running (23 M instructions in the window) and after it 170 or 216 with none: why it moved is not known, so only sides of one `ab.py` call compare; the table's Tiberian Dawn column mixes calls, each row against its own call's tree.

A read cache loses though the trace says 63 to 91% hits: a hit is a texel of the array, 53 ns, more than the 47 of the read it saves, and a miss pays both.

## Step 4: whole real loads, three each in turn against lean

The best legal mix of the above: `NI_ALU_OK,NI_LOAD_OK,NI_CUT_MISC,NI_PAIR`. Illegal arithmetic would no longer trap: acceptable only under a validated encoding, as an upper bound here.

| load | lean ms | a pass | full | this ms | a pass | full | x | right? |
|---|---|---|---|---|---|---|---|---|
| boot to the prompt and the first mark | 1,209 | 2,848 | 1.4% | 1,242 | 2,943 | 1.5% | 0.973 | no sum, no oops |
| jpeg, no sum | 395 | 5,630 | 23.4% | 396 | 5,648 | 23.0% | 0.998 | decoded, no sum printed |
| png, no sum | 1,620 | 4,416 | 2.5% | 1,534 | 4,415 | 2.5% | 1.056 | decoded, no sum printed |
| mp3, 63 workers | 3,987 | 5,953 | 3.3% | 4,834 | 5,962 | 3.4% | 0.825 | `fd612a25` both |
| Tiberian Dawn, mission 3 to frame 1300 | 21,752 | 7,702 | 1.8% | 22,223 | 7,699 | 1.8% | 0.979 | `1a9a814b` both |

Passes and the share ended full are level with lean's in every row: the constraint holds, there is simply nothing gained. mp3 loses M on the workers leaving the loop (a guess at the cause: not run apart).

## What it says

- **Lean is flat in every direction tried.** Fourteen forms of the loop, from taking tests away to adding a whole second instruction a trip, moved a real load by under 4%, in both directions, and those that win alone do not add (`NI_ADDI` and `NI_PAIR`; whole loads above).
- **A trip costs less than was thought, a test about as much as what it skips.** `NI_PAIR` takes the trip away from about a quarter of gzip's instructions and gives 2.2%: under 30 ns a trip saved (inferred). `NI_ADDI` skips some 40 operations for half of arithmetic and is level. `rv_fresh`'s line, a jump's cost rising with distance to 45 ns, is the likely reason: a branch taken costs what the code it skips would have, roughly. So moving work behind tests buys nothing, and that is what "local optimum" is on this card. Inferred; no PC samples of the tick: the trace of lean made here (`experiments/rv_fuse/sass.ps1`) again sampled no instruction of it (`rvc_trace12`'s known fault, `rv_fuse/RESULTS.md`), only its static SASS.
- **The registers are free and the memory is not.** So an ISA's room is not in the register file (more registers cost nothing to read, which is what gpu_vm's density uses) and not in the fetch or decode: at most 3.7% by the encoding, measured as a ceiling on gzip and 3.2% on Tiberian Dawn.
- **Why a new encoding was not built:** the ways it could win are its checks (3.7%), density (`NI_PAIR`'s form 2%; `rv_fuse`'s measured bundles lose on lean), and fewer memory paths (a window on the stack, 3 to 5% inferred). Summed as if they added: about 10%, and here they did not add. +25% would need a saving nobody has found a price for.

## Not run

- A from-scratch tick with a new encoding on a real kernel: `rv_fresh`, `rv_fuse` and `isa_proto` are that, and were not repeated. Translation not touched. Load-time or ahead-of-time code rewriting was not built.
- The stack window; fused forms other than arithmetic pairs; a flat word array for the cache (one load where the texel takes four, about 35 ns on each load that finds a stored texel: inferred 1 to 3%).
- Doom and Quake were not run: the three-load rule is for a change to keep, and nothing here is kept.
- `RVC_RACES`, the test set, Unity: nothing changed in the machine. `rvc_cpu` and lean are as they were; `trace.patch` was built only to trace and is not applied on this branch.
