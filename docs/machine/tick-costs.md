# What an instruction costs

For whoever next tries to make the tick faster: what its cost is made of, how to measure it, and what shape the code
must keep. The machine is the no-MMU one (`docs/system/nommu.md`): `NO_PAGING` with `SBI_HLE`, so no TLBs and no page
walks, CSRs as fields of core 0's first texels, a write cache of 336 texels (64 + 32 + 16 buckets of three), eight
targets, 256 cores, 8,192 instructions a pass.

## What a pass is made of

Calibrated, 31 October 2026 (`tools/card_model.json`, fitted to an RTX 5090 and fxc2; the skill `calibrated-measurements`):

    ms a pass = 1.4005 + 0.0111 a thousand of core 0's instructions + 0.00079 a thousand of the workers' + 0.5705 a GPU list

A full pass of core 0 alone is then 1.49 ms, 5.49 million instructions a second. A shell loop run under the model
(`time sh -c` of 20,000 turns of a `while` with `$((i+1))`, 69.2 s of the guest's, about 380 million instructions) gave
5.5 million. `glxgears` on the desktop, a list every pass, is 2.05 ms a pass. Games: `docs/machine/speed.md`, "Calibrated".
The model is fitted to games whose passes are mostly full, so its first term is not what a short pass costs: on the
card an idle machine's tick is 0.15 to 0.19 ms a frame (`docs/machine/speed.md`, "Measured in one run").

On the card, D3D11, fxc2, `RVC_PASS_PROF=1`, flat out, one round a frame, 29 October 2026, RTX 5070 Laptop, milliseconds of
a round's each draw:

| workload | tick | unpack and the state rows' copy | gather | state rows | RAM points | devices, once a frame |
|---|---|---|---|---|---|---|
| a shell loop (core 0 alone: 260 points) | 1.41 | 0.023 | 0.006 | 0.009 | 0.036 | 0.076 |
| Doom's timedemo (workers on: 33,410 points) | 1.44 | 0.022 | 0.006 | 0.009 | 0.037 | 0.088 |
| Quake standing at e1m1 | 1.41 to 1.44 | 0.026 | 0.006 | 0.009 | 0.038 | 0.118 |

So the draws after the tick are 0.074 ms a round, 5% of it, and the tick is nearly all. On an RTX 5090 (28 October 2026,
`RVC_TICK_ALT`) a frame's tick is 1.43 ms on the shell loop, 1.47 on Doom's timedemo, 1.44 on Quake: about 175 ns an
instruction. The RAM points cost the same with 260 points as with 33,410, so their cost is not their geometry shader's
work: the first draw of that shader in a round pays about 0.03 ms that an identical second draw does not.

To fit a pass's time to kinds of event on the card: `tools\pass_costs.ps1` times one pass of each of seven kinds of work
on a frozen state and counted the same pass with `--profile`, which this tick does not have (the script stops there now), and `python tools\pass_regress.py logs\pass_costs.csv` fits a
cost to each kind. A load or store costs several times an instruction that is neither, and an instruction on the general
path many times either: run it for today's figures.

## Where an instruction's operations are

The tick compiled on 31 October 2026 (`python tools\cso_loops.py`, the largest blob of `build\shadercache`, the one with
eight outputs) is 13,396 bytecode instructions, its fast loop 2,253 of them. An instruction walks a small part of that
loop: fetch, decode and the kinds' tests, the two register reads, its kind's own block (a load's or a store's write cache
search, a float's block), the result's write and the loop's end.

`python tools/dxbc_run.py TICK.asm SNAPSHOT CONSTANTS --lines FILE` runs one pixel of one pass of the bytecode and writes
how often each line ran; `tools/tick_path.py TICK.asm [TICK2.asm]` counts along the path of each kind without running
anything. Both read `dxbc_disasm`'s text (`docs/world/linux.md`), and `dxbc_run.py` also a snapshot and `RVC_DUMP_CB=FILE`
from the harness. `tick_path.py`'s turns are of a tick with paging: for this one its strings want rewriting (no test of
the fetch's page).

A count is a guide, never a verdict: only the two builds run in turn say what a change is worth.

## The shape the code must keep

These are the fast step's rules (`shader_vm/tick.hlsl`). The text's form is the gain, and most of the time goes to which blocks an instruction passes on its way, which
operations counted do not predict.

- **The kinds are a tree of tests written in the `if`s** (`K_ALU`, `K_FP`): arithmetic, loads, stores and floats a group,
  in it arithmetic and loads one. A row of tests one after another costs every kind after the first; the same tests as
  bools worked out first lose what the tree gains. No `switch` on this path.
- **Arithmetic computes every result and selects one.** It has no blocks of its own to pass.
- **A load has one way out**: one texel chosen, one word of it taken, not a return from each place it can be answered.
- **A data texel is asked of the texture before the write cache is searched.** A read is not waited for until its texel
  is used, so the search runs while it is under way. Asking later, or only after the remembered texels are looked at, is
  worth less.
- **The instruction window is kept turned** (`fw_cur`: its first word is the instruction to run), and entering a texel reads
  the next one ahead. An instruction within its texel passes no fetch. A jump within the texel reads it again, as no entry
  asks whether it is the window's own. Fewer texture reads here are slower: the read ahead is not waited for.
- **The write cache is searched as little as can be**: the two texels found or made last and the one looked for last and not
  found are remembered (`L1_KNOWN`), and 1,024 bits say which texels the cache was given (`L1_HAS`).
- **Common floats each by themselves** (`fp_light`): a sum, difference or product, moves, signs and comparisons, where
  `fp_exec` works out every result there is. A float result goes to its register by a write with no branch, and nothing
  an instruction of another kind passes is touched for one kind's sake.
- **The loop is left where a thing is found out**, by a `break` at the place that decides it. A return value in D3D11
  bytecode is a flag written, copied and tested. The step and the loop's end stay two tests.
- **`l1_occ` is a `uint4` picked by selects, not an array of four words**: the array was 3 to 5% of every load on the
  card (28 October 2026). A change to the local arrays moves the others, so what gains alone may lose with it: try local
  array changes together, not one at a time.
- **HLSL evaluates both sides of `&&` and `||`**: a call with an `out` argument or a side effect behind one runs always. A
  tick's last loop that asked for a texel this way wrote zeros over state nothing counted.
- **A static set before a pass's loop and read after it is carried through every core's loop**: keep such a value in a
  word carried already, or read it again after the loop.
- **Every test an instruction passes is a level, and a level costs more than the operations in it** (3 November 2026,
  below): an `if` with an `else` in front of a kind was about 45 cycles for every kind behind it on an RTX 3090, an
  operation on the path 2.5 to 7. Arithmetic and loads are one group, the first test; a load reads RAM whether or not it
  may (a read changes nothing, and one that may not is the general step's); a select of a texel the cache may have is a
  level less than an `if`. A store stays under its test: it changes things.
- **A function inlined in the loop has no `return` after its first test** (`ram_store`'s ways are an if and an else): fxc2
  makes a flag of a return, tested after.
- **The loop is left by a bare break** (`pre_w` kept every turn, `pre_valid` worked out after the loop).
- **No test the loop's end waits for on a common kind that only an encoding no program has would fail** (3 November
  2026, second round): arithmetic's funct7 table was 2% of the shell loop. Arithmetic reads bit 30 of funct7 alone,
  a branch's funct3 2 and 3 are beq and bne, and rvc_cpu makes such words as the tick does.
- A smaller pass is not a faster one, and taking code out of the function can cost as much as adding it.
- After any change to the fast loop's control flow, bring the function to Unity's copy verbatim.

## Measure with the card's clock high

The emulator's speed is the card's clock, and the card's governor runs it low for the first seconds of a load: a short
benchmark after a pause measures the governor, and a cold boot on an idle card takes far longer than on a busy one. Warm
the card first and watch `nvidia-smi --query-gpu=clocks.gr` while it runs.

- Compare two builds by running them in turn (A B A B), long runs, from one and the same snapshot: two snapshots of "the
  same" work differ by more than most changes are worth, and a snapshot made by each build for itself measures nothing.
- `RVC_TICK_ALT=DIR;DIR...` is better still: other ticks beside the machine's own in one run, each frame's picked by a
  hash, every frame timed with `--stats-after` (`docs/machine/speed.md`, "Measured in one run"; `--replay` from a snapshot).
- Run the three loads: a shell loop, Doom's timedemo (`DOOM_VIEW=told doom -1 -timedemo demo1`), Quake at e1m1 (`quake +map
  e1m1`). They disagree, and a change is judged whole on all of them.
- Look first for an `rvc_harness.exe` with no arguments, which holds the card at 100%.
- The calibrated clock is fitted to one tick: a change to the tick is measured on the card.

## What things cost

A way to time one thing: `RVC_FREEZE=1` in the environment and the D3D11 harness draws the tick and nothing after it, so
the state is never written back and every frame runs the same pass (give it `--fixed-dt 0.000000001` for guest time to
stand still too). `--bench` then prints the fastest frames' time. Work added to every fast instruction (`--define
X_P_ALU=8`, `X_P_TEX=1`: `emu.h`; `X_P_READ` reads the TLB's array, which this machine does not have) is then so many
nanoseconds an instruction. `RVC_TICK_SCISSOR="x0 y0 x1 y1"` draws only those pixels of the tick; `--ticks 2` shows what
a pass costs besides its instructions.

`programs/classes` (`--image classes`) is loops of one kind each with a mark between them, timed with `--stats-from` and
`--until`.

What such timings say, and which to rely on:

- An operation is about a nanosecond, whether or not anything waits for it. Nothing overlaps for nothing: work begun
  before it is known to be wanted is paid in full.
- A test and its block that an instruction has to pass cost far more than the operations inside, and about the same
  whatever the block's size; a small block may be made a write with no branch, and costs nothing.
- An array element read lately is cheap, one among many costs a read each, and a `uint4` element chosen by a number is four
  reads. A write costs next to nothing.
- A texture read that is used at once is the dearest thing on the path, and one not used yet is next to nothing.
- How many pixels run the tick does not matter, but which pixels share a warp does (`AGENTS.md`, "One core a wave").

## 3 November 2026: levels out of the fast loop

One RTX 3090 at 1,950 MHz, D3D11, fxc2 67. Every figure is ms of a frame's tick, two or more ticks alternating by frame in
one run (`RVC_TICK_ALT`), from one snapshot each: the shell loop and Quake at e1m1 always, Quake III, `nxphys` and `nxray`
for what was kept. A copy of the tick against itself differed by 0.0 to 1.5% (Quake the noisier), so anything under 1.5% was
run again before it was believed.

**The kept set against `15c1e2a7`** (the new tick stamped and primary, the old one beside it):

| Load | Before | After | |
|---|---|---|---|
| Shell loop | 1.7955 | 1.7078 | -4.88% |
| Shell loop, again | 1.7995 | 1.7117 | -4.88% |
| Quake, e1m1 | 1.9406 | 1.8277 | -5.82% |
| Quake III | 2.0062 | 1.9000 | -5.29% |
| `nxphys` | 0.4113 | 0.3887 | -5.49% |
| `nxray` (its snapshot nearly idle) | 0.1286 | 0.1239 | -3.65% |

What it is, in commits `04060757` to `2863bfc0`: arithmetic and loads a group of their own, the first test; a load reads RAM
whether or not it may; `ram_word` takes the remembered texels and the cache's texel by selects and searches under one test;
the fast loop's stores go to `ram_store_nc`, which does not test RAM's end (sw and fsw test it with the rest); `ram_word` does
not look for the store with no room (it ends the pass); the loop is left by a bare break; `ram_store` has no returns.
`rvc_cpu` is unchanged: the machine is the same.

**What a pass is made of, on this card.** 8,192 instructions of the shell loop are 1.80 ms, 220 ns or about 430 cycles an
instruction; a pass of two instructions is 0.028 ms. Only the warp of the busiest core counts: discarding every pixel but
core 0's changed nothing (-0.27%), and in a frozen pass (`RVC_FREEZE=1 --bench`, a copy of the tick given its own `warp_ok`)
core 0 with half or a quarter of its pixels was 1.7955 and 1.7823 against 1.8036 ms: its two warps do not slow each other.
A pass is one warp's latency.

**The frozen kinds.** In a frozen pass the word can be made one instruction for every turn (`w = WORD ^ (cur.x & _Init)`),
and `same = true` leaves the fetch out: then a pass is 8,192 of one kind, and repeats within 0.3%. ms a pass:

| | addi | add | lw 0(sp) | sw 0(sp) | bne, not taken |
|---|---|---|---|---|---|
| no fetch | 1.009 | 1.017 | 1.540 | 1.724 | 1.093 |
| with the fetch (a texel every 4) | 1.351 | | 1.895 | 2.053 | 1.434 |

The fetch of a new texel is about 240 cycles, and the read ahead (`fw1`) is waited for where it is asked: a crossing with the
read ahead 1.350, reading the texel then and there 1.374, reading nothing 1.102. In the addi pass with no fetch, taking out
arithmetic's `ok` saved 0.092 ms (22 cycles an instruction), the group's tests 0.131 (31), the eight results and their selects
0.136 (32); `budget` against a constant, nothing. An `if` with an `else` put in front of the tree (addi first) made addi 0.633
ms and cost every other kind 0.19 (45 cycles): add 1.018 to 1.213, lw 1.538 to 1.726, bne 1.090 to 1.276. A dependent local
read on the path was +6.2% (about 27 cycles), a dependent texture read of a texel just read +23% (about 100), three dependent
operations +1%, six +4.7%, a small `if` that is never taken +0.4%.

**Experiments**, ms a tick before and after (shell, then Quake), against the tree of that moment; "frozen" rows are the table
above's passes. Kept rows are in bold.

| What | Shell | Quake | Verdict |
|---|---|---|---|
| x0 written after every instruction instead of a sink register | 1.8396 to 1.8442 | 1.9616 to 1.9553 | level |
| A full-word store by `put()` into the cache's texel | | 1.9616 to 2.0852 | lost, +6.3% |
| No geometry read for core 0 | 1.8396 to 1.8389 | 1.9616 to 1.9528 | level |
| The discard test as nested branches | 1.8407 to 1.9478 | 1.9616 to 1.9747 | lost, +5.8% shell |
| Branches first in the tree | 1.8396 to 1.8789 | 1.9616 to 1.9752 | lost |
| Probe: every pixel but core 0's discarded | 1.8407 to 1.8358 | | they cost nothing |
| Probe: the field's rows discarded at once | 1.8407 to 1.8418 | | level |
| The overflow word not looked at by loads or stores | 1.8222 to 1.8302 | 1.9600 to 1.9681 | level |
| Loads: `va != 0` and the like dropped where the range test covers them | 1.8222 to 1.8262 | 1.9600 to 1.9782 | level |
| `LOAD_UP` as a table | 1.8222 to 1.8257 | 1.9600 to 1.9688 | level |
| The store's word written by four flattened ifs | 1.8222 to 1.8496 | 1.9600 to 1.9642 | lost |
| The data texel read without the `dr_t` test | | 1.9600 to 2.0176 | lost, +2.9% |
| A 32-bit filter by a hash of the texel beside `bloom` | 1.8222 to 1.8462 | 1.9600 to 1.9838 | lost |
| Arithmetic, branches and jumps one block of selects | 1.7693 to 2.0833 | 1.9838 to 2.2274 | lost, +12 to 18% |
| Arithmetic and branches one block of selects | 1.7693 to 1.9104 | 1.9838 to 2.0893 | lost |
| Arithmetic alone first, then loads and the rest | 1.7693 to 1.7782 | 1.9838 to 1.9962 | lost |
| Probe: lanes never go on to a next index | | 1.9513 to 1.9481 | level: their statics are not the cost |
| Probe: no lane or field output after the loop | | 1.9513 to 2.0469 | +4.9%: taking code out moves the loop |
| The group tested by a mask of opcode bits | | 1.9513 to 1.9712 | lost |
| Probe: general steps counted (the clock +1000 a step) | about 16 a pass | 6 a pass core 0, 2 a worker | not where the time is |
| `[unroll(2)]` on the fast loop | | | wrong: it caps the loop at two turns |
| CSRs of the fast loop read and written by their own short code | | 1.9540 to 1.9535 | level |
| The word taken by index each turn, no rotated window | | 1.9550 to 2.0849 | lost, +6.6% |
| The next word's two registers read during this one, forwarded | 1.8126 to 1.9740 | 1.9479 to 2.1209 | lost, +8.9% |
| Arithmetic's results by a tree of branches on f3 (two levels; one) | 1.8077 to 1.8812; 1.8710 | 1.9658 to 1.9941; 2.0095 | lost |
| add/sub under its own branch | 1.8077 to 1.8327 | 1.9658 to 1.9877 | lost |
| Probe: 16 and 32 more registers live through the loop | 1.8049 to 1.8997; 1.7992 | | +5.3% and -0.3%: placement, not pressure |
| Code texels kept in a local array of 32 (`ic`) | 1.8039 to 2.1366 | 1.9889 to 2.1883 | lost, +18% |
| Data texels kept in a local array of 32, the read after or before the test | 1.8039 to 2.2305; 2.2087 | 1.9889 to 2.5870; 2.5964 | lost, +22 to 30% |
| Both | 1.8039 to 2.3296 | 1.9889 to 2.4429 | lost |
| `ot` not an indexed array (its writes unrolled) | 1.8065 to 1.8055 | 1.9828 to 1.9725 | level |
| The last result forwarded to the next instruction by a branch (both registers; the first) | 1.8323 to 1.8331; 1.8736 | 1.9824 to 1.9839; 1.9859 | level, lost |
| Three, four and one remembered texels (two now) | 1.8211 to 1.9170; 1.8454; 1.8402 | 1.9697 to 2.0634; 2.0053; 2.0031 | lost |
| fxc2 switches: `VKD3D_HLSL_FLATTEN` 0 and 30, `VKD3D_PACK_REGISTERS` 0 and 1, `VKD3D_REGISTER_SPREAD=8`, `VKD3D_SIMPLIFY_CONDITIONS=0`, `VKD3D_HLSL_THREAD_FLAGS=0` | 1.8253 to 1.8477, 1.8238, 1.8288, 1.8286, 1.8232, 1.8245, 1.8209 | | level, flatten 0 +1.2% |
| `xr` declared one component wide (the DXBC patched and signed again) | 1.7974 to 1.7976 | | level |
| Probe: a pass of two instructions | 0.028 | | the fixed part is 1.5% |
| Probe, frozen: core 0's two warps (half, a quarter of its pixels) | 1.8036 to 1.7955, 1.7823 | | no contention |
| The next texel read at the top of every turn (select; branch) | 1.7948 to 2.0092; 2.0574 | | lost, +12 to 15% |
| A window of two and of four texels read together | 1.7914 to 1.8785; 1.9022 | | lost (frozen sequential -3.4%, -5.0%) |
| addi first, before the tree | 1.8000 to 1.8467 | 1.9408 to 2.0297 | lost: frozen above |
| `same` from pc, not from next | 1.7948 to 1.7932 | 1.9408 to 1.9367 | level (again on the kept set: level) |
| The kinds by a `[forcecase] switch` on opcode bits 6-2 | frozen addi 1.011 to 1.131, lw 1.530 to 1.681 | | lost on every kind |
| Arithmetic, branches and jumps by selects, frozen | addi 1.009 to 1.43, lw 1.54 to 1.73 | | lost (what the real runs said) |
| Code texels in registers, newest first: 2, 4, 8 | 1.8026 to 1.9339, 1.9877, 2.0361 | 1.9699 to 2.0986, 2.1697, 2.1709 | lost |
| **The loop left by a bare break** | 1.8083 to 1.8053 | 1.9229 to 1.9350 | level alone (frozen: a jump a turn 2.521 to 1.755); kept with the levels, -0.2 to -0.5 |
| The atomics out of the fast loop | 1.7999 to 1.8923 | 1.9399 to 2.0513 | lost, +5% |
| The CSRs out of the fast loop | 1.7999 to 1.8052 | 1.9399 to 1.9376 | level |
| **Arithmetic and loads one group, stores and floats the next** | 1.7853 to 1.7689; 1.7977 to 1.7814 | 1.9185 to 1.9057; 1.9601 to 1.9513 | kept, -0.9% shell |
| **A load reads RAM whether or not it may** (on the above) | 1.7911 to 1.7426 | 1.9325 to 1.8973 | kept, -2.7% and -1.8% |
| **The cache's texel by a select** (on the above) | 1.7924 to 1.7299 | 1.9460 to 1.8725 | kept, -3.5% and -3.8% together |
| The fetch reading its texel always, no `fw1` | 1.7924 to 1.8339 | 1.9460 to 1.9829 | lost |
| Branches and jumps one block of selects | 1.7928 to 1.7800 | 1.9421 to 1.8898 | lost against -3.7%, -4.8% |
| **Stores by `ram_store_nc`** | 1.7928 to 1.7216 | 1.9421 to 1.8296 | kept, -4.0% and -5.8% together |
| The store's word by four sequential branches | 1.7998 to 1.7265 | 1.9475 to 1.8552 | level against -3.9%, -4.7% |
| **The remembered texels by a select, one test for the search** | 1.7998 to 1.7191 | 1.9475 to 1.8477 | kept, -4.5% and -5.1% together |
| The data texel read unconditionally (again) | 1.7998 to 1.9247 | 1.9475 to 2.0988 | lost, +7% |
| Arithmetic alone first again (loads the second test) | 1.7833 to 1.7427 | 1.9224 to 1.9059 | lost against -4.3%, -3.9% |
| Stores without their test, a mask of 0 when they may not | 1.7833 to 1.7692 | 1.9224 to 1.9354 | lost |
| Branches before stores and floats | 1.7862 to 1.6956 | 1.9254 to 1.8620 | mixed: shell -0.7, Quake +0.6; on the kept set +1.8% Quake |
| The fetch's two reads side by side under a select | 1.7955 to 1.7524 | 1.9449 to 1.9025 | lost against -4.7%, -5.6% |
| **No overflow word in `ram_word`** | 1.7955 to 1.7042 | 1.9449 to 1.8383 | kept, -5.1% and -5.5% together |
| **`ram_store` without returns** | 1.7951 to 1.7040 | 1.9360 to 1.8355 | kept, -5.1% and -5.2% together |
| The first bucket's tags read by a select, in `find`; in the store's search | 1.7923 to 1.7033; 1.7050 | 1.9390 to 1.8330; 1.8334 | level against -4.9%, -6.4% |
| Jumps before branches | 1.7082 to 1.7346 | 1.8164 to 1.8360 | lost |
| On the kept set: x0 written after every instruction; `va != 0` dropped; the store's word by four branches; `LOAD_UP` as a table | 1.7098 to 1.7154, 1.7147, 1.7169, 1.7107 | 1.8272 to 1.8631, 1.8389, 1.8323, 1.8388 | level or lost |
| Nsight GPU Trace on the D3D11 harness | | | nothing: it traces D3D12 and Vulkan only |

**What was learnt.** The tree's levels are the cost to go after: a test passed with an `else` behind it is about as dear
as twenty operations, and taking one out of a frequent kind's way is worth what a fast path for that kind is (addi first
won 0.38 ms a frozen pass for addi and lost 0.19 for everything else). Selects instead of a tree lose when they add
operations to every kind; they win where they replace a level and add a read or two (the cache's texel). Local arrays cost
far more than their reads (a 32-entry cache of texels was +18 to +30%), and a texture read is waited for where the branch it
is in ends, so a read ahead across a turn of the loop buys nothing on this card. Real runs move by 1 to 5% for changes far
from what an instruction runs (registers, code taken out); frozen passes of one kind are exact and show what a change does to
each kind, but not what it does to the fetch of real code (a bare break: -30% on a frozen jump a turn, level in real loads).

## 3 November 2026, second round: tests the fast loop did not need

The same RTX 3090 at 1,950 MHz, D3D11, fxc2 67, `RVC_TICK_ALT` from the snapshots of the round before (made with that
round's image). ms of a frame's tick.

**The kept set against `15c1e2a7` and `5ea93cfd`** (the new tick stamped and primary, the two beside it, one run each line):

| Load | `15c1e2a7` | `5ea93cfd` | Now | against `15c1e2a7` | against `5ea93cfd` |
|---|---|---|---|---|---|
| Shell loop (40 s) | 1.7673 | 1.6807 | 1.6312 | -7.70% | -2.95% |
| Shell loop, again | 1.7739 | 1.6888 | 1.6385 | -7.63% | -2.98% |
| Quake, e1m1 (50 s) | 1.9240 | 1.8153 | 1.7949 | -6.71% | -1.12% |
| Quake, again | 1.9169 | 1.8246 | 1.7812 | -7.08% | -2.38% |
| Quake III (40 s) | 1.9819 | 1.8765 | 1.8493 | -6.69% | -1.45% |
| `nxphys` (30 s) | 0.4181 | 0.4021 | 0.3945 | -5.64% | -1.89% |

What it is, three changes:

- **Arithmetic looks at no funct7 but bit 30.** The fast loop tested which funct7 each op and op-imm has (a table of 16
  bits picked by funct7, shifted by funct3 and bit 5) so that an encoding of no extension of ours trapped as illegal. That
  test was 2.0 to 2.6% of the shell loop. Now sub, sra and srai are bit 30, M is still funct7 1 of op (the group's test),
  and anything else is made as the base instruction it resembles (`andn` is `and`, `slli` with a shift of 32 or more shifts
  by its low five bits). rvc_cpu does the same (the line that made such encodings illegal is gone). A program built for
  Zba or Zbb (`docs/plans/riscv-isa.md`) is now wrong on the tick, not stopped by SIGILL: such a plan changes the tick first.
- **A branch's ok is from its word.** funct3 2 and 3 are `beq` and `bne` (in the general step and rvc_cpu too), and an
  offset not on a word is bit 8 of the word, tested without waiting for `next`. Shell -0.3 to -0.7%, Quake -1.3%.
- **Arithmetic shifts by `y`, not `y & 31`**: D3D's shifts take the amount's low five bits. Shell -0.2 to -0.4%, Quake -1.0
  and -1.5%.

**How noisy Quake is.** `[fastopt]` on the fast loop gave bytecode identical to the tree's (the same blob hash), and in 30
to 45 s runs of three ticks it came out 0.7% and 1.7% faster than the tree on Quake, 0.4% slower on Quake III and 0.1% on
the shell loop. The shell loop repeats within 0.1 to 0.3%; Quake wants 50 s runs of two ticks and a second run, and a gain
under 1.5% on Quake is a guess until then. Two changes kept on 30 s runs (M before the atomics; op_m without returns and
one division for its four) were level in 50 s runs (1.7463 against 1.7436, 1.7658 against 1.7653, a copy of the tick
1.7476 and 1.7541) and were backed out. `nxphys` moved by 2 to 5% between runs of the same pair.

**Experiments**, ms before and after, against the tree of that moment (the shell loop, then Quake; 30 s runs of three or
four ticks unless said). Kept rows are in bold.

| What | Shell | Quake | Verdict |
|---|---|---|---|
| The write cache as words (`uint cw[1792]`; a word stored and loaded by index, the tags four reads) | 1.6849 to 1.6873 | 1.7961 to 1.8164 | lost |
| lui and auipc in the arithmetic group (`(w & 0x5b) == 0x13`), validity kept | 1.6849 to 1.7091 | 1.7961 to 1.8126 | lost |
| **No validity test for arithmetic** | 1.6849 to 1.6488; 1.6887 to 1.6443; 1.6857 to 1.6512; 1.6941 to 1.6570 | 1.7961 to 1.7906; 1.8073 to 1.8026; 1.8240 to 1.8208 | **kept**, -2.0 to -2.6% shell; Quake III 1.8915 to 1.8769, `nxphys` 0.4161 to 0.4118 |
| The validity as the group's test (M fails it) | 1.6887 to 1.7344 | | lost, +2.7% |
| The validity by tests, no table | 1.6887 to 1.6802 | | -0.5% against -2.6% with none |
| The validity ending the loop by the budget (no break of its own); the same by tests | 1.6857 to 1.6941; 1.6880 | 1.8073 to 1.8132; 1.8062 | lost: the loop's test waits for it as the break did |
| A branch's ok from the word, funct3 tested too | 1.6975 to 1.6937 | | level |
| Stores and branches one group, floats the next; the same with the branch first in it | 1.6975 to 1.6992; 1.7036 | | level |
| (on no validity) lui and auipc in the arithmetic group | 1.6600 to 1.6882 | | lost |
| (on no validity) mul in the arithmetic group by a select | 1.6614 to 1.7180 | | lost, +3.4% for one product and a select |
| (on no validity) `pre_w` from the window after the loop, not written every turn | 1.6570 to 1.6606; 1.6720 to 1.6773 | | level |
| (on no validity) a jump's ok from the word for jal | 1.6570 to 1.6590 | | level |
| Probe: no ok for branches at all; none for lui, auipc and jal | 1.6727 to 1.6594; 1.6687 | | -0.8%, level (on the kept set, see below: level) |
| Arithmetic with a tail of its own and a `continue` | 1.6720 to 1.7001 | 1.7948 to 1.8452 | lost |
| Loads with no funct3 table (lwu is lw) | 1.6720 to 1.6604; 1.6694 to 1.6663 | 1.7971 to 1.8136 | level |
| M under the arithmetic group, by a branch of its own | | 1.7948 to 1.8504 | lost |
| **A branch's ok from bit 8 of its word, funct3 2 and 3 beq and bne** | 1.6694 to 1.6584; 1.6650 to 1.6601 | 1.7971 to 1.7724; 1.7952 to 1.7718 | **kept**; Quake III 1.8817 to 1.8758 |
| The fetch taking the texel read ahead as it is when it enters at its first word | 1.6609 to 1.6665 | 1.7937 to 1.8172 | lost |
| The loop's test at its end (`do ... while`) | 1.6609 to 1.6742 | 1.7937 to 1.8142 | lost |
| A store's break in its own block; a load's after its read | 1.6552 to 1.7241; 1.6966 | 1.7981 to 1.8743; 1.8345 | lost: a break where it is found out is not always cheaper |
| Arithmetic's results made for loads too, the load an `if` after them | 1.6511 to 1.6902 | 1.8045 to 1.8100 | lost |
| The load the group's inner `if`, arithmetic its `else` | 1.6511 to 1.6884 | 1.8045 to 1.7942 | lost |
| Arithmetic's selects by `f3 & 2` first | 1.6511 to 1.6544 | 1.8045 to 1.7877 | level |
| Two texels remembered as absent; the absent texel forgotten only when it is the one stored | 1.6526 to 1.6675; 1.6553 | 1.7753 to 1.8089; 1.7831 | lost, level |
| **Shifts by `y`, not `y & 31`** | 1.6663 to 1.6597; 1.6431 to 1.6402 | 1.7795 to 1.7615; 1.7748 to 1.7490 | **kept**; Quake III 1.8653 to 1.8592 |
| The data texel read only when the remembered texels do not have it | 1.6663 to 1.6760 | | lost (as the round before found) |
| A part of a word into a texel the cache has, written with no test of a change; the same with `at != 0` outside | 1.6658 to 1.6635; 1.6688 | 1.7795 to 1.7802; 1.7766 | level |
| A load's shifts by `va << 3` and `up` from a table shifted by `f3 << 3` | 1.6154 to 1.6169 | 1.7535 to 1.7541 | level |
| A store's shifts by `sa << 3` | 1.6154 to 1.6170 | 1.7535 to 1.7674 | level |
| `ram_word`'s word picked by `o & 8` and `o & 4`; the fetch's turn by `pc & 8` and `pc & 4`; the store's word branches by them | 1.6154 to 1.6182; 1.6309 to 1.6302; 1.6321 | 1.7535 to 1.7739; 1.7620 to 1.7799; 1.8038 | level or lost |
| slt by two compares, not signs turned over | 1.6309 to 1.6397 | | lost |
| flw with the loads (`(w & 0x7b) == 0x03`), its funct3 tested; not tested | 1.6343 to 1.6540, 1.6379 to 1.6547; 1.6497 | 1.7716 to 1.7300 | lost: Quake -2.3%, the shell loop +1.0% and Quake III 1.8416 to 1.8598 |
| flw with the loads and fsw with the stores | 1.6343 to 1.6868 | 1.7716 to 1.7870 | lost |
| M before the atomics | 1.6343 to 1.6364; 1.6379 to 1.6371 | 1.7716 to 1.7519; 1.7652 to 1.7495; 50 s: level | level, backed out |
| op_m without returns; and its four divisions one unsigned division of the magnitudes | 1.6335 to 1.6321; 1.6338, 1.6430 to 1.6431 | 1.7608 to 1.7550; 1.7503, 1.7783 to 1.7505; 50 s: level | level, backed out |
| `[fastopt]` on the fast loop | 1.6175 to 1.6194 | 1.7728 to 1.7432; 1.7669 to 1.7538 | the same bytecode: the noise |
| Probe: the general step without branches and jumps | 1.6175 to 1.6175 | 1.7669 to 1.7629 | level: the general step's size does not move the loop |
| fp_all without returns, its dead sum gone | 1.6352 to 1.6361 | 1.7593 to 1.7694 | level (Quake III 1.8323 to 1.8337) |
| Loads with no funct3 table and no `va != 0` | 1.6163 to 1.6178 | 1.7579 to 1.7621 | level |
| On the kept set: no ok for branches; none for jumps; both | 1.6261 to 1.6352; 1.6259; 1.6252 | | level or lost |
| `load_state`'s registers by an unrolled loop | 1.6292 to 1.6315; 1.6356 to 1.6367 | 1.7816 to 1.7694 | level (`nxphys` 0.4198 to 0.3967, 0.4155 to 0.4113, 0.3698 to 0.3782) |
| rs2 read only by the kinds that use it | 1.6295 to 1.6870 | 1.7704 to 1.8284 | lost, +3.5%: the register reads want to be first |

**What was learnt.** A test whose answer the loop's end waits for is dear on the commonest kind even when its operations
are few: arithmetic's validity was 8 operations and 2% of the shell loop, and taking it off the loop's control (ending by
the budget instead of a break) saved nothing, as the loop's own test then waited for it. An instruction's validity is
cheaper given up where no program of ours has the encoding than tested. Moving work into the arithmetic path, even one
product and a select, costs that path 1 to 3%; taking one operation out (`& 31`) was worth 0.2 to 0.4%. On Quake a run of
30 s does not tell 1% from nothing: run 50 s, two ticks, twice.

## 3 November 2026, third round: checks given up, small shapes, and FAST

The same RTX 3090, D3D11, fxc2 67, `RVC_TICK_ALT` from the second round's snapshots (made with its image). ms of a
frame's tick. The owner gave leave to drop checks that only an invalid or rare program would notice.

**The kept set against `af803662` and `15c1e2a7`** (the new tick stamped and primary, the two beside it, one run each line):

| Load | `15c1e2a7` | `af803662` | Now | against `15c1e2a7` | against `af803662` |
|---|---|---|---|---|---|
| Shell loop (40 s) | 1.7585 | 1.6260 | 1.6114 | -8.36% | -0.90% |
| Shell loop, again | 1.7657 | 1.6304 | 1.6178 | -8.38% | -0.77% |
| Quake, e1m1 (50 s) | 1.9029 | 1.7661 | 1.7569 | -7.67% | -0.52% |
| Quake, again | 1.8986 | 1.7677 | 1.7505 | -7.80% | -0.97% |
| Quake III (40 s) | 1.9862 | 1.8454 | 1.8350 | -7.61% | -0.56% |
| `nxphys` (30 s) | 0.4277 | 0.3985 | 0.3813 | -10.85% | -4.32% (it moves 2 to 5% between runs) |

What it is (`rvc_cpu` makes every relaxed one as the tick does; `test_all.py` 220 of 220, no value moved; on the card
`test_card.py machine fptest nxpath-balls nxpaint` 11 of 11):

- **The relaxed checks**, level alone (shell 1.6132 to 1.6118, Quake 1.7663 to 1.7705, Quake III 1.8539 to 1.8503), kept
  with the shapes they were measured under:
  - a plain `fence` is no instruction in the fast loop (it left the loop: 0.008% of the shell loop's and Quake's
    instructions, `0820000f`, `0ff0000f`, `0310000f` and `0550000f`, counted from `RVC_PROF`'s `pcs.txt`); `pause` and
    `fence.i` are the general step's;
  - loads look at no funct3 (lwu is lw, ld and funct3 7 fail the size test) and not at 0 (the range test excludes it);
    stores no `f3 < 3` (the size test has it); flw and the atomics no 0 test;
  - an atomic's funct5 under 16 that is none of ours is amoswap, and lr's rs2 is not looked at (the general step too);
  - jalr's funct3 is not looked at (fast loop and general step; rvc_cpu never did);
  - fadd, fsub and fmul keep the card's NaN: an NVIDIA card's is `7fffffff` whatever the operands' were (a probe of
    quiet, signalling, negative and payload NaNs, `0 * inf` and `inf - inf`), not RISC-V's `7fc00000`; `rvc_cpu`'s
    `card_nan` makes the same. Division, root, the fused ones and min/max still make `7fc00000`;
  - `op_m` keeps only what D3D does not give: on the card `divu` by 0 is all ones, `div` of `INT_MIN` by -1 is `INT_MIN`
    and `rem` of it 0, but `div` of a negative number by 0 is 1 and every remainder by 0 is all ones: `div` and the
    remainders keep their zero test.
- **Five shapes**: a branch's less-than as slt's (signs turned over), `ram_word` taking the remembered texels whatever
  the bloom says (they are the cache's), the inline floats' result by funct7's bits, `fp_all` testing the conversions
  first, the pass's loop testing `!quiet` with no `[branch]`. Each was -0.1 to -0.3% of the shell loop; the five without
  the relaxed checks shell -0.72 and -0.57%, Quake +0.48, +0.40 and +0.04%; with them shell -0.81 and -0.76%, Quake +0.05,
  -0.64 and +0.02%, Quake III -0.33 and -0.72%.
- **Float comparisons by themselves** before the light floats (Quake's most common float, 2.6% of its instructions; feq,
  flt and fle with funct3 3 to 7 are fle, as rvc_cpu always made them), **a load's bytes by one shift** (`(va & 3) * 8` is
  never over `up` for a load that may be), **a store's mask by `LOAD_UP`** and **its immediate from rd**: on the set above
  Quake -1.25, -1.64 and -0.50%, the shell loop and Quake III level.

**FAST** (`--define FAST` on the harness; `rvc_cpu --fast` or `RVC_TICK_FAST=1`, and `RVC_TICK_FAST_PARTS=1|2|4` for
the sizes, the opcodes of no group, the sc alone): off by default. What it is: a load or store in RAM not on its size takes
the bytes of the word it is in from its address on (a load has zeros above them; ld and lwu are lw; a store's funct3 3 to 7
go by their low bits), instead of the general step's two words or bytes; fdiv is the card's `a / b`, in the fast loop with
the sum and product; an sc always succeeds; an opcode of no group is no instruction instead of an illegal one. ms of the
tick, the same run:

| | Shell | Quake | Quake III | `nxphys` |
|---|---|---|---|---|
| All of FAST, on the kept set | 1.6240 to 1.6176, -0.39% | 1.7427 to 1.7391, -0.21% | 1.8282 to 1.8134, -0.81% | 0.3819 to 0.3813, level |
| All of FAST, on `af803662` | 1.6399 to 1.6344, -0.34% | 1.7823 to 1.7638, -1.04% | 1.8516 to 1.8386, -0.70% | 0.3794 to 0.3833, +1.0% |
| The sizes alone (on `af803662`) | 1.6399 to 1.6308, -0.55% | 1.7823 to 1.7662, -0.90% | 1.8516 to 1.8405, -0.60% | 0.3794 to 0.3862, +1.8% |
| fdiv alone | | 1.7823 to 1.7700, -0.69% | | |
| The sc alone | | 1.7823 to 1.7787, -0.20% | | |
| The opcodes of no group alone | 1.6399 to 1.6405, level | | | |

What FAST breaks (`test_all.py` under `rvc_cpu --fast`: 211 of 220; `test_card.py machine fptest nxpath-balls nxpaint`
with `--with=--define --with=FAST`: 10 of 11):

- **The sizes**: Red Alert's state (`ralert-allies-300`, `ralert-soviet-1300`, `ralert-allies-1300`, `ralert-paths`,
  `ralert-attack`, `ralert-won`: other units and buildings by frame 300) and the NES (`nes-nova-120`,
  `nes-nova-120-lists`: every frame's RAM `aece4b1b` for `3a2e5c47`). Both are correct C that reads and writes across a
  word, which the general step made right. (`RVC_TICK_FAST_PARTS=1` alone fails these.)
- **The opcodes of no group**: the IDE's debugger (`nxide-c`), whose breakpoint is the word 0 (an illegal instruction the
  kernel turns into SIGILL, `linux/ide/debug.h`), runs through every stop (`RVC_TICK_FAST_PARTS=2` alone). `machine.h`
  and `nxsettings` find F and Zb by a SIGILL too: F's instruction is in a group, and Zb's has run as the base instruction
  since the second round, so `nxsettings` says Zba and Zbb are there with or without FAST. Nothing in the image emulates
  doubles by a trap: a double is compiler-rt's integer routines (`fpu.md`).
- **fdiv**: `fptest` on the card, 533 of 200,000 divisions wrong (a divisor over 2^126 gives 0: `7f4fac9c / ff4fac9d`
  gave `80000000`, not `bf7fffff`), 40,462 off in the last place (40,605 without FAST). `rvc_cpu` cannot make the card's
  division: its own is exact.
- **The sc**: nothing in either set (`RVC_TICK_FAST_PARTS=4` alone passes them). An sc succeeds whether or not its lr
  was made: a program that relies on a failed sc after a trap or a context switch would not see one.

**Experiments**, ms of the tick against `af803662` in the same run (the shell loop 30 s, Quake 45 s, four to six ticks a
run; a copy of a tick moved up to 0.2% on the shell loop and up to 1.4% on Quake in these). The rows from "A store's
mask" on are on the relaxed checks. Kept rows are in bold.

| What | Shell | Quake | Verdict |
|---|---|---|---|
| Plain fence a no-op | 1.6433 to 1.6449 | 1.7741 to 1.7813 | level |
| Loads' funct3 and 0, stores' `f3 < 3` | 1.6433 to 1.6407 | 1.7741 to 1.7771 | level (Quake III 1.8539 to 1.8493) |
| Atomics' funct5 and lr's rs2, jalr's funct3 | 1.6433 to 1.6429 | 1.7741 to 1.7814 | level |
| The card's NaN in fadd, fsub, fmul | 1.6132 to 1.6214 | 1.7425 to 1.7696; 50 s 1.7663 to 1.7787 | level or +0.5 to +0.7% (Quake III 1.8539 to 1.8662) |
| `op_m` without what D3D gives | 1.6132 to 1.6153 | 1.7425 to 1.7558 | level |
| **All of them** | 1.6132 to 1.6118 | 1.7425 to 1.7523; 50 s 1.7663 to 1.7705 | level alone (Quake III 1.8539 to 1.8503, `nxphys` 0.3864 to 0.3925); kept with the shapes |
| A store's mask by `LOAD_UP` | 1.6372 to 1.6342 | 1.7767 to 1.7748 | level |
| A store's size test by a table | 1.6372 to 1.6394 | 1.7767 to 1.7818 | level |
| A load's size test by a table | 1.6372 to 1.6396 | 1.7767 to 1.7870 | level |
| flw before opcode 0x53 | 1.6525 to 1.6503 | 1.7734 to 1.7716 | level |
| Opcode 0x53 first in its group, stores after | 1.6525 to 1.6614 | 1.7734 to 1.7893 | lost |
| The floats' format bits not tested | 1.6525 to 1.6494 | 1.7734 to 1.7738 | level |
| The light floats' funct3 and rs2 not tested | 1.6366 to 1.6390 | 1.7700 to 1.7680 | level |
| flw's and fsw's funct3 not tested | 1.6366 to 1.6370 | 1.7700 to 1.7908 | level or lost |
| **A branch's less-than as slt's** | 1.6366 to 1.6346 | 1.7700 to 1.7671 | level alone; kept in the five |
| The store's immediate from rd; the branch's | 1.6424 to 1.6434; 1.6421 | 1.7788 to 1.7839; 1.7959 | level |
| `ram_word` without the texel looked for last and not found | 1.6424 to 1.7555 | 1.7788 to 1.9174 | lost, +6.9% and +7.8%: the search is dear |
| A jump's target as one sum | 1.6380 to 1.6414 | 1.7815 to 1.7556; 60 s 1.7588 to 1.7690 | level (Quake III 1.8386 to 1.8420) |
| The jumps' group by two masks | 1.6380 to 1.6372 | 1.7815 to 1.7756 | level |
| **`ram_word`'s remembered texels whatever the bloom says** | 1.6380 to 1.6350 | 1.7815 to 1.7829 | level alone; kept in the five |
| fsw with the stores (flw by itself) | 1.6390 to 1.6455 | 1.7786 to 1.7795 | level or lost |
| **Float comparisons by themselves** | 1.6390 to 1.6378 | 1.7786 to 1.7552; 60 s 1.7588 to 1.7461 | level alone (Quake III 1.8386 to 1.8386); kept with the next ones |
| fmv by itself before the light floats | 1.6390 to 1.6384 | 1.7786 to 1.7859 | level |
| A subtraction as a sum with y negated | 1.6515 to 1.6578 | 1.7859 to 1.7650; 60 s 1.7777 to 1.7786 | level |
| slt's turn by `(~f3 & 1) << 31` | 1.6515 to 1.6531 | 1.7859 to 1.7876 | level |
| The window turned by two selects, not three | 1.6515 to 1.6556 | 1.7859 to 1.7781 | level |
| The light floats before the sum, difference and product | 1.6396 to 1.6408 | 1.7675 to 1.7740 | level |
| The atomics after M and the CSRs | 1.6396 to 1.6456 | 1.7675 to 1.7982 | lost |
| Probe: no floats in the fast loop; no CSRs; no atomics | 1.6396 to 1.6273 | 1.7675 to 1.7709; 1.7751 | the float group costs the shell loop, which has none, 0.75% |
| **The inline floats' result by funct7's bits** | 1.6405 to 1.6370 | 1.7826 to 1.7787 | kept in the five |
| **`fp_all`: the conversions first** | 1.6405 to 1.6362 | 1.7826 to 1.7778 | kept in the five |
| **The pass's `!quiet` with no `[branch]`** | 1.6405 to 1.6362 | 1.7826 to 1.7821 | kept in the five |
| A store's bloom bit at its start | 1.6416 to 1.6404 | 1.7676 to 1.7893 | level |
| An atomic reads its word whatever it is | 1.6416 to 1.6395 | 1.7676 to 1.7858 | level |
| **A load's bytes by one shift** | 1.6416 to 1.6387 | 1.7676 to 1.7711 | level alone; kept with the comparisons |
| The atomics' address test as the loads' | 1.6540 to 1.6568 | 1.7771 to 1.7725 | level |
| The fast loop's CSRs: no read-only test | 1.6540 to 1.6543 | 1.7771 to 1.7867 | level |
| `[branch]` on the loop's `if (!ok) break` | 1.6540 to 1.6530 | 1.7771 to 1.7840 | level |
| Stores in the first group, after arithmetic and loads | 1.6540 to 1.6762 | 1.7771 to 1.7530 | lost, +1.3% shell |
| The float checks all given up (funct3, format, flw's) | 1.6286 to 1.6272 | 1.7484 to 1.7646 | level |
| **A load's one shift, a store's mask by `LOAD_UP`, its immediate from rd** | 1.6286 to 1.6218 | 1.7484 to 1.7623 | level alone; kept with the comparisons |
| **The five shapes** | 1.6286 to 1.6178 | 1.7484 to 1.7502 | **kept**, figures above |
| Loads do not move the remembered texels | 1.6299 to 1.6591 | 1.7959 to 1.7915 | lost, +1.8% shell |
| Two data texels kept from the texture | 1.6299 to 1.7457 | 1.7959 to 1.9315 | lost, +7.1% and +7.6% |
| The CSRs before M | 1.6299 to 1.6305 | 1.7959 to 1.7929 | level |
| The fast loop's body twice a turn | 1.6299 to 1.7449 | 1.7959 to 1.9025 | lost, +7.1% and +5.9% |
| The five without the relaxed checks | 1.6433 to 1.6315; 1.6371 to 1.6277 | 1.7657 to 1.7742; 1.7680 to 1.7751; 1.7729 to 1.7736 | shell -0.6%, Quake +0.4%: with them instead |
| The five with the comparisons | 1.6433 to 1.6323 | 1.7657 to 1.7607 | level against the five |
| On the kept five: the comparisons; the one shift, mask and immediate; the light floats' checks | 1.6309 to 1.6232; 1.6168; 1.6221 (the five 1.6202) | 1.7608 to 1.7466; 1.7395; 1.7394 (the five 1.7549) | Quake -0.5 to -0.9% against the five |
| On the kept five: an atomic's read; the mask; the one shift; **the comparisons with the one shift, mask and immediate** | 1.6251 to 1.6158; 1.6137; 1.6109; 1.6141 (the five 1.6145) | 1.7904 to 1.7747; 1.7621; 1.7608; 1.7337 (the five 1.7556) | **kept**: again, 60 s, Quake 1.7713 to 1.7422 and 1.7455 to 1.7367, shell 1.6118 to 1.6110, Quake III 1.8270 to 1.8280 |

**What was learnt.** The checks given up this round were cheap already: none was more than noise alone, where the second
round's funct7 table was 2% of the shell loop. A test is dear when the loop's end waits for it on a common kind; these were
on rare kinds or folded into tests the loop makes anyway. What is still dear is a search of the write cache (without the
absent texel +7%) and anything that adds a local array or a second copy of the body (+6 to +8%). On Quake, 45 s runs of
five ticks moved a copy of the tick by up to 1.4%: a change was believed after three runs with the same sign. The commit
and the other shaders cannot be timed with `RVC_TICK_ALT`, which swaps the tick pass alone.

## Fourth round: the write cache's searches and reads, and sstatus in the fast loop

The same RTX 3090, D3D11, fxc2 67, `RVC_TICK_ALT` from the snapshots of the rounds before (made with their image), the
shell loop 35 to 45 s and Quake 45 to 55 s, four to six ticks a run, `af803662` in every run. ms of a frame's tick. A copy
of the tree against itself moved up to 0.2% on the shell loop and 0.8% on Quake in these runs. The owner's mark: 5% under
`af803662`.

**The kept set against `af803662` and `15c1e2a7`** (the new tick stamped and primary, the two beside it, one run each line):

| Load | `15c1e2a7` | `af803662` | Now | against `15c1e2a7` | against `af803662` |
|---|---|---|---|---|---|
| Shell loop (40 s) | 1.7671 | 1.6318 | 1.5627 | -11.57% | -4.23% |
| Shell loop, again | 1.7674 | 1.6311 | 1.5623 | -11.60% | -4.22% |
| Quake, e1m1 (50 s) | 1.8996 | 1.7646 | 1.6862 | -11.23% | -4.44% |
| Quake, again | 1.8994 | 1.7573 | 1.7056 | -10.20% | -2.94% |
| Quake III (40 s) | 1.9693 | 1.8350 | 1.7456 | -11.36% | -4.87% |
| `nxphys` (30 s) | 0.4145 | 0.3967 | 0.3681 | -11.19% | -7.21% (it moves 2 to 5% between runs) |

Short of the mark of 5%: 4.2% on the shell loop, 2.9 to 4.4% on Quake (the kept set's runs this round against `af803662`
ranged from -2.4 to -5.1%), 4.9% on Quake III. On the card `test_card.py machine fptest nxpath-balls nxpaint` 11 of 11,
and WARP's mctest-3 in parts.

What it is, four commits (`rvc_cpu` unchanged: the machine is the same):

- **A store of the fast loop is not under `if (ok)`.** `ram_store_nc` is called with a mask of 0 when the store may not
  be (out of RAM or across its word), and a mask of 0 changes nothing: the value is the word's own, so nothing is
  stored; what it may do is ask the texel and search the cache, which is the cache's own bookkeeping. One level fewer
  for every store, with the call's blocks behind it: shell -0.7 to -0.9%, Quake -1.5%.
- **sstatus while no interrupt is pending, timeh and instret are the fast loop's CSRs.** The kernel turns interrupts
  off and on around every lock (`csrci sstatus, 2`, `csrs sstatus, a5`, `csrrc`, `csrw`): 12 of the shell loop's 15
  general steps a pass (`RVC_PROF`'s pcs), each 1,640 cycles (a frozen pass of nothing else). With `mip & mie` 0 no
  write of sstatus can let an interrupt in, and nothing else makes one pending within a pass. Level alone; with the
  store, shell -1.0 to -1.3% and Quake -1.6%.
- **A store's RAM texel is asked at the start of its search**, not after it: a store to a texel the two remembered ones
  are not (the first store to a texel in a pass, a new entry) needs the texel's other words, and the read then runs while
  the buckets are looked in. Shell -1.6%, Quake -1.0%. With it the store's partial word takes its texel by a select, and
  `ram_store` is `ram_store_nc` behind its range test (its own copy gone): shell level, Quake -0.8%.
- **A load searches the cache only where a 16 KB region of RAM has a texel of it** (`regs`, bit `(o >> 14) & 31`, set
  where a texel enters the cache), not where the bloom lets it: the bloom, the stored addresses or-ed, let nearly every
  load through once a pass had stored to the stack and the heap, and a texel looked for and not there is a search. Shell
  -0.6%, Quake -1.3 and -1.5%. 4, 8 and 16 KB were alike, 32 and 64 KB a little worse on the shell loop.

**Experiments**, ms of the tick before and after in the same run (b1: the store and the CSRs; b2: and the read at the
search; b3: and the select and the wrapper; b4: and the regions, the kept set). Kept rows are in bold.

| What | Shell | Quake | Verdict |
|---|---|---|---|
| A branch's and a jump's target texel read where it is decoded, the fetch taking it | 1.6049 to 1.7752 | | lost, +10.6% |
| The same for branches alone | 1.6049 to 1.7587 | | lost, +9.6% |
| The two remembered texels' values in registers: a load reads no `c[]`, a store writes the whole texel | 1.6166 to 1.6429 | | lost, +1.6% (frozen lw 1.432 to 1.455, sw 1.749 to 1.899) |
| One test at a turn's end for an instruction left to the general step, the budget and a fetch (the write, pc and the count by selects) | 1.6002 to 1.6842 | 1.7448 to 1.8326 | lost, +5% (frozen addi 0.921 to 0.880, lw 1.447 to 1.398) |
| Arithmetic's next word too, when it is arithmetic of the texel, past the loop's tests | 1.5995 to 1.6045 | | lost |
| The same as an inner loop over arithmetic of the texel | 1.5995 to 1.6229 | | lost, +1.5% |
| `same` from `pc & 12` | 1.5995 to 1.5994 | | level |
| A store: a whole word into a remembered texel tested first, the search in its else | 1.5977 to 1.6131 | | lost, +1.0% |
| add and addi one select from their sum | 1.5977 to 1.6091 | | lost, +0.7% |
| A load's data texel's test and its search's under one test | 1.5977 to 1.6125 | | lost, +0.9% |
| **A store not under `if (ok)`, a mask of 0 when it may not** | 1.6020 to 1.5882; 1.6049 to 1.5937 | 1.7513 to 1.7255 | **kept** |
| The same with the whole-word test first | 1.5977 to 1.5966 | | level |
| The same and fsw likewise | 1.6020 to 1.5892 | | as the store alone |
| **sstatus while nothing is pending, timeh and instret in the fast loop** | 1.6049 to 1.6022 | 1.7513 to 1.7526 | level alone; **kept** with the store |
| **Both (b1)** | 1.6049 to 1.5889; 1.6127 to 1.5916; 1.6263 to 1.6068 | 1.7513 to 1.7240; 1.7539 to 1.7260; 1.7406 to 1.7433 | **kept** |
| b1, fp's rs1 read by a select, not an `if` | 1.5916 to 1.5920 | 1.7260 to 1.7218 | level (on b3 Quake 1.7147 to 1.6941, on b4 1.7024 to 1.7115, Quake III level) |
| b1, a whole word into the cache's texel by `put` (read and written back) | 1.5916 to 1.6131 | 1.7260 to 1.7522 | lost, +1.4% |
| b1, fsw not under `if (ok)` | 1.5916 to 1.5943 | 1.7260 to 1.7423 | level or lost (on b4 level) |
| b1, flw a load (`(w & 0x7b) == 3`, its funct3 in `ok`) | 1.6068 to 1.6310 | 1.7433 to 1.7256 | lost: shell +1.5% |
| b1, the load's search and the fetch's inner test without `[branch]` | 1.5905 to 1.5900; 1.5927 | | level |
| **b1, a store's RAM texel asked at the start of its search (b2)** | 1.5905 to 1.5652 | 1.7339 to 1.7157 | **kept**, -1.6% and -1.0% |
| **b2, the partial word's texel by a select; `ram_store` a wrapper of `ram_store_nc` (b3)** | 1.5718 to 1.5719, 1.5747; both 1.5721 | both 1.7126 to 1.6993 | **kept** |
| b2, a load's RAM texel asked inside its search's block (and in its else) | 1.5718 to 1.5824 | | lost, +0.7% |
| b3, atomics not under `if (ok)` | 1.5688 to 1.5808 | 1.7122 to 1.7013 | mixed (on b4: lost +0.9%, +1.8%) |
| b3, branches before stores and floats | 1.5688 to 1.5585 | 1.7122 to 1.7216 | mixed (on b4: shell -0.2%, Quake +1.6%) |
| b3, the store's word by a `[forcecase] switch` | 1.5688 to 1.5922 | 1.7122 to 1.7295 | lost |
| b3, a load's search with no branches: the three tables' tags read together, chosen by selects; a store's; both | 1.5745 to 1.6381; 1.6880; 1.7291 | 1.7087 to 1.7736; 1.7942; 1.8085 | lost, +4 to +10% |
| b3, a load's RAM texel not asked for a remembered texel | 1.5663 to 1.5719 | 1.7147 to 1.7133 | lost |
| b3, a load's `maybe` also asking its first table's bucket's bit | 1.5657 to 1.5722 | 1.6930 to 1.7308 | lost, +2.2% Quake |
| b3, the write cache as words with the tags apart (a whole word stored by index, no branches) | 1.5660 to 1.5645 | 1.6888 to 1.7020 | level |
| b3, a store's new texel tested before a part into a cached one | 1.5739 to 1.5817 | 1.6943 to 1.7170 | lost |
| b3, a straight run reading two code texels every other crossing; with jumps reading three | 1.5739 to 1.6717; 1.6157 | 1.6943 to 1.7731; 1.7232 | lost, +3 to +6% |
| b3, the store's first bucket's tags by a select | 1.5695 to 1.5704 | 1.7035 to 1.7186 | level or lost |
| b3, the budget's test and the fetch's under one at a turn's top | 1.5695 to 1.5932 | 1.7035 to 1.7172 | lost, +1.5% |
| b3, two loops: an outer turn a texel entered, an inner one an instruction | 1.5684 to 1.6421 | 1.7032 to 1.7724 | lost, +4.7% |
| b3, the write cache zeroed at a pass's start; no `static_noinit` at all | 1.5588 to 1.5661; 1.5572 | 1.7044 to 1.6965; 1.7134 | level |
| b3, a load searching where a 64 KB region has a texel of the cache, not by the bloom | 1.5571 to 1.5538; 1.5798 to 1.5762 | 1.7172 to 1.6898; 1.7330 to 1.7067 | shell -0.2%, Quake -1.5% |
| The same with the bloom too | 1.5571 to 1.5586 | 1.7172 to 1.6974 | Quake -1.2% |
| **By 16 KB regions (b4)** | 1.5571 to 1.5476; 1.5798 to 1.5706 | 1.7172 to 1.6949; 1.7330 to 1.7068 | **kept**, shell -0.6%, Quake -1.4% |
| By 4 KB; 8 KB; 32 KB | 1.5798 to 1.5693; b4 1.5710 to 1.5685; 1.5743 | 1.7330 to 1.7072; b4 1.7100 to 1.6987; 1.7129 | as 16 KB, 32 KB a little worse |
| By 16 KB, 64 regions in a `uint2`; a second word by 512 KB regions | 1.5798 to 1.6583; b4 1.5532 to 1.5624 | 1.7330 to 1.8316; b4 1.6820 to 1.7067 | lost, +5%; +0.6% and +1.5% |
| b4 without the absent texel | 1.5560 to 1.5672 | 1.7024 to 1.7263 | lost |
| b4, lw's word as it is (no shifts) | 1.5691 to 1.5733 | 1.7049 to 1.7039 | level |

**Probes**, frozen passes (`RVC_FREEZE=1 --bench`, a copy of the tick given its own `warp_ok`, the word made one instruction
for every turn), ms a pass of 8,192:

| Probe (on the tree of 16d32ca7) | ms a pass |
|---|---|
| addi, the code texels read at crossings as the tree does; a fixed texel read instead; none read | 0.994; 0.969; 0.909 |
| No code read, kinds: addi, add, lw 0(sp), sw 0(sp), bne not taken, lui | 0.911, 0.909, 1.432, 1.749, 1.162, 1.263 |
| addi: no register reads; no register write; neither; arithmetic one add | 0.867; 0.900; 0.845; 0.792 |
| addi: the fetch's block gone; never entered (no crossing); never entered and no texture read in it | 0.750; 0.879; 0.852 |
| addi: the code read ahead at the top of every turn | 1.090 |
| sw: the word's two branches gone (`.x` always); no `ram_store_nc` | 1.624; 1.106 |
| lw: no `ram_word`; no data texel test; the data texel read always; no search block; no cache select; `idx` gone; no `ok`; no sign shift | 0.929; 1.338; 1.733; 1.439; 1.397; 1.354; 1.403; 1.395 |
| A general step every instruction (csrr sstatus) | about 6.9 (1,640 cycles a step) |
| addi, lw: `discard` as an early return | 0.921 to 0.929, 1.429 to 1.447 |
| `--ticks 2`: the tree; a fragment shader that returns at once | 0.040; 0.045 |

**What was learnt.**

- The fetch's read ahead is not free: a crossing that reads the next code texel and does not use it until four
  instructions later costs about 80 cycles (frozen addi with the reads 0.994 ms, without 0.909), a read of a fixed texel 57;
  read at the top of every turn, 40 cycles an instruction. Every way tried of reading code earlier or less often lost:
  a branch's or jump's target read where it is decoded (+10%), two texels ahead every other crossing (+3 to +6%), the
  fetch at a turn's end under one test with the budget and an instruction left to the general step (+5%; frozen -4%),
  two loops, a texel an outer turn (+4.7%), the budget's test and the fetch's under one (+1.5%).
- A read the block it is in can run beside pays: the store's texel asked before its search (-1.6%). The same for a load,
  the texel asked inside the search's block, lost (+0.7%): a load's read is already asked before its search.
- Local memory read that is not needed is dear: the three tables' tags read together and chosen by selects, no branches,
  +4 to +10%; a whole word stored by reading the texel and writing it back +1.4%; the cache as words with the tags apart
  level.
- A test with a call behind it is more than a level: the store's `if (ok)` (-0.9 to -1.5% without it), where the same for
  the atomics and fsw were level or lost (they are rare).
- What a search is asked for matters more than how it is made: the bloom let most loads through; a bit a 16 KB region
  spares them (-0.6% shell, -1.4% Quake). A finer filter in a uint2 (+5%) or a second word by another region size (+1.5%)
  cost more than they spared; the first table's bucket's bit (`OCC`) as the filter +2.2% on Quake.
- The general step is 1,640 cycles: worth taking out of it what is common, when the fast loop can make it the same.
- The pass's fixed part is the draw's: a frozen pass of two instructions is 0.040 ms, a fragment shader that returns at
  once 0.045. `discard` costs nothing (an early return instead: level).
- Quake's tick is its drawing worker's (core 1, 7,088 instructions a pass to core 0's 3,362): what workers do (loads of
  textures, stores of new texels) is what moves it.

## Fifth round: the driver's modes, the frozen kinds, and floats

The same RTX 3090, D3D11, fxc2 67, `RVC_TICK_ALT` from the snapshots of the rounds before (made with their image), the
shell loop 35 to 40 s, Quake 50 s, Quake III 40 s, three to six ticks a run, `af803662` in every run. ms of a frame's
tick. A copy of the tree against itself: shell -0.14%, Quake +0.18%. Across the round `af803662` stood 4.05 to 5.01% over
the tree on the shell loop, 2.33 to 5.60% on Quake and 4.23 to 5.30% on Quake III: Quake's ratio alone moves by 3%
between runs of the same pair. 48 experiments.

**The kept set against `af803662` and `15c1e2a7`** (the new tick stamped and primary, the others beside it, one run each line):

| Load | `15c1e2a7` | `af803662` | `219de239` (round four) | Now | against `15c1e2a7` | against `af803662` |
|---|---|---|---|---|---|---|
| Shell loop (40 s) | 1.8304 | 1.6942 | 1.6211 | 1.6206 | -11.46% | -4.34% |
| Shell loop, again | 1.8367 | 1.7093 | 1.6290 | 1.6322 | -11.13% | -4.51% |
| Quake, e1m1 (50 s) | 1.9600 | 1.8303 | 1.7599 | 1.7622 | -10.09% | -3.72% |
| Quake, again | 1.9536 | 1.8295 | 1.7595 | 1.7542 | -10.21% | -4.12% |
| Quake III (40 s) | 2.0432 | 1.9174 | 1.8255 | 1.8209 | -10.88% | -5.03% |
| `nxphys` (30 s) | 0.4150 | 0.3915 | 0.3734 | 0.3851 | -7.20% | -1.63% |

Short of the mark of 5% on the shell loop (4.3 to 4.5%) and Quake (3.7 to 4.1% in these two runs); Quake III 5.0%. The
kept change is level against round four's tick in these final runs (shell +0.0, -0.2%; Quake +0.1, -0.3%; Quake III
-0.25%); over the round's runs it was Quake -0.95% on average (six runs, -2.0 to +0.1%), Quake III -0.2%, shell +0.1%.
On the card `test_card.py machine fptest nxpath-balls nxpaint` 11 of 11 and `mctest-255 --parts` passed; WARP's mctest-3
in parts.

What it is, one commit (`rvc_cpu` unchanged: the machine is the same):

- **fmv.w.x and fmv.x.w by themselves** in the float block, after the comparisons and before `fp_light`: `v = fa`, the
  integer register for fmv.w.x. Through `fp_light` a frozen pass of fmv.w.x was 2.36 ms, more than a float sum's (1.54);
  by itself 1.79. It is 14% of Quake's floats (1.3% of its drawing worker's instructions).
- **rs1's float register by a select, read beside rs2's**, not under an `if` after it: under the `if` the second read
  waited for the first. A frozen fadd 1.557 to 1.519 ms.

**The driver's modes.** A frozen pass of addi (`RVC_FREEZE=1 --bench`, the word made one instruction every turn) is about
1.01 ms on the tree, and most changes leave it there; some put it at 1.18 (+16%) and whole runs at +6%, for nothing in the
code: `pre_w = w` moved from before the loop's break to the turn's top (one `mov` moved, the rest the same bytecode),
`pre_w` taken from `cur.x` after the loop (the tree's bytecode with its temps renumbered), fxc2's
`VKD3D_HLSL_ELIDE_LOADS=0`, `VKD3D_FORWARD_STORES=0` and `VKD3D_UBFE=1` (+5 to +8% whole). Keeping 48 words the loop does
not use (CSRs, timer, the lanes' and the field's) in `xr`'s tail instead of carried registers left the mode as it was
(frozen addi 1.012) and cost the shell up to 1.3%: it is not register pressure. A frozen addi pass tells the mode in 20 s.

**The frozen kinds** (ms a frozen pass of 8,192 of one kind, the tree, within 1% between sessions): addi 1.006 to 1.021,
lw 0(sp) 1.541, lbu 1.539, sw 0(sp) 1.737, bne not taken 1.252, flw 1.789, fadd 1.542, flt 1.800, fmv.w.x 2.361. A float
costs what a load does over addi, an flw 60 cycles more than an lw (it is the tree's fifth test).

**The fetch keeps its shape.** Every change to how `fetch` reads cost 6 to 13% of every load: a third texel kept (the one
before, for a jump back into it), a jump within the texel reading nothing (both reads under a test of their own; or only
the first). A frozen straight run of addi with the third texel was 1.300 ms against 1.021: the read ahead (`fw1`) moved
into a nested branch is waited for where that branch ends. Yet the shell loop's string loops jump within their texel
(1.13% of its instructions, estimated from `RVC_PROF`'s pcs) or back into the one before (1.33%), each two reads waited
for; Quake's drawing worker 0.23% and 0.27%. What they cost is still there for a form that keeps `fw1`'s read where it is.

**Experiments**, ms of the tick before and after in the same run (frozen: ms a frozen pass of the kind named). Kept rows
are in bold.

| What | Shell | Quake | Verdict |
|---|---|---|---|
| fp's rs1 by a select, read beside rs2 | 1.6275 to 1.6250 | 1.7378 to 1.7396 | level alone (frozen fadd 1.5566 to 1.5194); kept with fmv |
| Regions of 8 KB | 1.6275 to 1.6325 | 1.7378 to 1.7640 | lost, +1.5% Quake |
| flw a load (`(w & 0x7b) == 3`, dst by bit 2, funct3 2 tested) | 1.6275 to 1.6485 | 1.7378 to 1.7381 | lost, +1.3% shell (frozen flw 1.7885 to 1.5890, lw 1.5408 to 1.5901, addi +2.3%) |
| Branches before stores and floats | 1.6399 to 1.6328 | 1.7634 to 1.7699 | level (with fsh: shell -0.35%, Quake +0.6%, +0.1%, Quake III +1.2%) |
| `!ok` and the budget one test at the turn's end, the write, pc and count by selects | 1.6399 to 1.6603 | 1.7634 to 1.7844 | lost, +1.2% |
| `pre_w` taken from `cur.x` after the loop | 1.6399 to 1.7401 | 1.7634 to 1.8697 | lost, +6.1%: the same bytecode, temps renumbered (frozen addi 1.18) |
| `same` known early, from pc at the turn's top, cleared by a branch taken or a jump | 1.5552 to 1.5863 | 1.7120 to 1.7308 | lost, +2.0%, +1.1% |
| Regions by `(o >> 14) ^ (o >> 19)` | 1.5552 to 1.5581 | 1.7120 to 1.7238 | level or lost |
| `xr` not zeroed (under `static_noinit`) | 1.5552 to 1.5612 | 1.7120 to 1.7189 | level |
| `pre_w = w` at the turn's top, not before the break | 1.6020 to 1.7067 | 1.7380 to 1.8498 | lost, +6.5%: one `mov` moved (frozen addi 1.0135 to 1.1815) |
| fast's locals declared in another order | 1.6020 to 1.6030 | 1.7380 to 1.7328 | level |
| fxc2 `VKD3D_HLSL_IEEE_STRICT=1` (no `refactoringAllowed`) | 1.6020 to 1.6049 | 1.7380 to 1.7416 | level |
| fxc2 `VKD3D_HLSL_ELIDE_LOADS=0`; `VKD3D_FORWARD_STORES=0`; `VKD3D_UBFE=1` | 1.6176 to 1.7164; 1.6969; 1.7176 | 1.7446 to 1.8780; 1.8342; 1.8425 | lost, +5 to +8% |
| The CSRs and timer words in `xr`'s tail, not registers carried through the loop; the lanes' and field's; both (48 words) | 1.5989 to 1.6151; 1.6057; 1.6201 | 1.7429 to 1.7283; 1.7370; 1.7390 | shell +0.4 to +1.3%, Quake level |
| The decode's fields and register reads in another order; the fetch's and remembered texels' statics; `ok` declared first | 1.6116 to 1.6118; 1.6214; 1.6142 | 1.7369 to 1.7303; 1.7447; 1.7274 | level |
| The fetch at the turn's end, the first before the loop | frozen addi 1.0062 to 1.1703 | | lost, not run whole |
| No `pre_w`: the general step reads its word again; `pre_w` at the top and before the break | frozen addi 1.0062 to 1.0155; 1.0190 | | level frozen |
| Float registers read at the top with the integer ones | frozen addi 1.0129 to 1.0261, lw 1.5408 to 1.5711, fadd 1.5566 to 1.4568 | | floats -6%, integers +1 to +2%: not run whole |
| A load reads the two remembered texels before its address is known | frozen lw 1.5403 to 1.6653 | | lost, +8% a load |
| A store: a whole word into a remembered texel tested first | 1.6115 to 1.6140; 1.6113 to 1.6155 | 1.7446 to 1.7225; 1.7404 to 1.7269; 1.7463 to 1.7448; Quake III 1.8113 to 1.8210, 1.8155 to 1.8276 | shell level, Quake -0.7%, Quake III +0.6% (frozen sw 1.7342 to 1.6181): dropped |
| The same by `put` into the remembered texel read ahead | 1.6115 to 1.6278 | 1.7446 to 1.7645; Quake III 1.8361 | lost |
| A whole word into the cache's texel by `put` (read, written back) | frozen addi 1.1590, sw 1.9981 | | lost |
| fmv.w.x and fmv.x.w by themselves, before `fp_light` | 1.6115 to 1.6197; 1.6123 to 1.6231 | 1.7446 to 1.7353; Quake III 1.8092 | shell +0.5%, Quake -0.5% (frozen fmv.w.x 2.3609 to 1.7882) |
| **fmv by themselves and fp's rs1 by a select (kept)** | 1.6115 to 1.6155; 1.6113 to 1.6096; 1.6142 to 1.6161; 1.6146 to 1.6171; 1.6123 to 1.6123 | 1.7446 to 1.7190; 1.7404 to 1.7311; 1.7463 to 1.7117; 1.7502 to 1.7427; 1.7551 to 1.7301; 1.7284 to 1.7303; Quake III 1.8113 to 1.8063, 1.8155 to 1.8066, 1.8045 to 1.8069 | **kept**: shell +0.08% on average, Quake -0.95%, Quake III -0.21%; the final runs level |
| The kept pair with the store test (#above) | 1.6113 to 1.6171; 1.6138 to 1.6176 | 1.7404 to 1.7353; 1.7463 to 1.7273; 1.7572 to 1.7390; Quake III +0.2% | level |
| The kept pair with M before the atomics | 1.6123 to 1.6161 | 1.7284 to 1.7125; Quake III 1.8099 | level |
| A window of three texels: the one before kept, a jump within the texel or back into the one before reads nothing | 1.6138 to 1.8127 | 1.7572 to 1.9503 | lost, +12% (frozen addi 1.0213 to 1.3000) |
| A jump within the texel reads nothing: both reads under a test of their own | 1.6142 to 1.8147 | 1.7502 to 1.9317 | lost, +12%, +10% |
| The same, the read ahead where it was, only the first read under the test | 1.6146 to 1.7473 | 1.7551 to 1.8676 | lost, +8%, +6% |

**What was learnt.**

- A frozen pass of one kind is exact to 1% and takes 20 s: it shows what a change does to each kind and whether it moved
  the driver's mode, which a whole run of 50 s cannot tell from noise. Run it on addi, lw and sw before a whole run.
- A kind's own test moved earlier helps that kind and costs those behind it about as much: flw a load (flw -11%, lw +3%),
  float registers read at the top (floats -6%, integers +1 to +2%), a whole word into a remembered texel tested first
  (sw -6.7%, Quake III +0.6%).
- What a kind computes and does not use is worth taking out of its path: fmv through `fp_light` was 330 cycles over addi.
  Each such kind is a small share of a load, so the whole runs say less than the frozen pass.
- The shell loop's time left is in the fetch of short loops and in the stack's loads and stores; every form tried for
  those moved the driver's mode or the read ahead.

## Sixth round (5 November 2026): fusions, deferred writes, inline floats, and the pass's length

The same RTX 3090, D3D11, fxc2 67; the tree of `62c2cd86`, a stamped copy of it the base (`EXP_BASE`), and a second copy
beside the variants in every run (`same2`): ms of a frame's tick against that copy, from one snapshot each (Quake walking
e1m1 with `QUAKE_TRACK=5`, Tiberian Dawn's scenario 3 at frame 2,113, the shell loop), 40 to 70 s, up to ten ticks in one run
(`RVC_TICK_ALT`). Copies of the tree differed by 0.2 to 0.9% from each other (Tiberian Dawn up to 1.7% once), so a variant is
believed only where it holds in two runs or more. Another session's Unity editor on the card made some runs worthless
(a base twice its usual time): those were dropped and run again. The numbers are means over the runs (in brackets).

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| **f2**: arithmetic's next word of the texel, when it is arithmetic or a branch, in the same turn (its registers forwarded from the first) | +0.2% (4) | -1.1% (4) | +0.1% (4) | Tiberian Dawn only |
| f1 (the next arithmetic word only, read after the write); f1b (forwarded); f5 (a branch only) | +0.2%; -; +0.2% | -1.3%; -0.5%; -0.2% | +0.9%; 0.0%; -0.2% | level |
| f4: the next word fused after any kind, at the turn's end | +14.8% | +7.2% | +13.4% | lost: one level more on every instruction |
| la1 (arithmetic after a load); ld2 (a lw of the same texel after a load, from the texel taken); st2 (a sw into the texel remembered last after a sw); br5 (arithmetic after a branch not taken) | -0.2%; +2.8%; +2.4%; +1.8% | +0.5%; +3.7%; +2.0%; +2.1% | +0.4%; +2.9%; +1.4%; +2.3% | lost |
| **fp1**: fdiv inline with the sum, difference and product, as the card's `a / b` (FAST's division, out of FAST) | -0.2% (3) | -0.9% (3) | -0.4% (3) | a little |
| **f2 + fp1** | -1.2% (3) | -1.0% (3) | -0.2% (3) | about -1% |
| **f2 + fp1 + ffma** (fmadd, fmsub, fnmsub, fnmadd in the fast loop, one `mad`, the loop's last test) | -0.7% (2) | -1.0% (2) | -0.7% (2) | about -1%; ffma costs nothing measurable |
| On f2 + fp1 + ffma (k2): la1; a load fused after arithmetic (k2al); a loop over the texel's run of arithmetic (k2t); no `n + 1 < budget` (k2nb); br3; br1 | +1.2%; +1.8%; +0.2%; +0.3%; +0.2%; +1.6% | -0.5%; +2.5%; 0.0%; -0.8%; -1.0%; -2.8% | -0.4%; +1.9%; +0.8%; -0.4%; -0.1%; +0.2% | level or lost; br1 Tiberian Dawn only again |
| k2 again, in two more runs | +0.2%, -0.5% | -0.9%, -0.8% | -0.6%, -0.3% | about -0.5%: the noise's edge |
| k2 with fdiv inline as `fp_all` makes it (exponents to the middle) and ffma rounded twice as the general step (k3: the results unchanged) | +1.7% | +0.5% | +0.8% | lost: the division's work is done for every sum, product and difference |
| The same with the division under a branch of its own in the inline block (k3b); f2 + ffma rounded twice, fdiv where it was (k3c) | +0.7%; -0.25% | +0.1%; -0.1% | +0.4%; +0.4% | level: what k2 had over them was the card's quotient and the fused `mad`, or layout |
| fp3 (the conversions before the light floats); pfp (no `precise` on the sum, difference and product); fcmpn (no NaN test in comparisons: the card's compare is false on a NaN too) | -0.5%; -0.7%; +0.8% | -0.3%; -0.5%; +0.1% | -0.6%; 0.0%; -0.4% | level |
| m1: mul among arithmetic's selects (as round two found) | +0.5% (2) | -1.4% (2) | +1.8% (2) | lost but on Tiberian Dawn |
| br1 (branches before stores and floats); br2, br3 (branches in the first group, after loads; after arithmetic) | +0.9%; +0.5%; -0.9% | -1.7%; -0.5%; -0.4% | -0.6%; +0.6%; 0.0% | br1 Tiberian Dawn only, as before |
| jl1: jal with the branches | +0.9% | +3.2% | +2.4% | lost |
| fb1: a branch fused after a float comparison | -1.5% (2) | +1.2% (2) | +1.1% (2) | Quake only |
| wb1: the result written a turn late, after the next turn's register reads, which take it by selects | +0.5% | +1.5% | +2.4% | lost |
| wb2: wb1, and an instruction that may not ends the loop by the budget (no break); wb3: and the budget's test under the fetch's | +2.4%; +6.0% | +3.0%; +8.3% | +3.9%; +7.8% | lost |
| prew2: `pre_w` written only on the way out (`if (!ok) { pre_w = w; break; }`) | +4.5% | +1.7% | +6.0% | lost: one `mov` moved |
| fxc2's switches, each in a folder of its own so the cache does not give it the tree's blob: `VKD3D_HLSL_VALUE_NUMBERING=0`, `VKD3D_HLSL_FLATTEN=30`, `VKD3D_HLSL_SPLIT_STRUCTS=0` | -0.6%, +0.3%, +1.1% | +0.3%, -0.1%, -0.2% | 0.0%, -0.4%, +0.1% | level, as round one found |
| Probe: the core a uniform (`select_core(_Init)`), to see whether the driver's branches cost less when it can tell a warp's lanes agree | | | | not run: every pixel then makes core 0's pass, the workers stop, and WARP's check fails, so no stamp |

The cache key of `build/shadercache` is the preprocessed text without the folder, so a copy of the tree compiled under an fxc2
switch gets the tree's blob: give it a token of its own (`static const uint tag = 0;`) and compile it in a run that has the
switch in its environment (the trees already cached keep their compile).

**The pass's length.** Not the tick's code but its budget (`--ticks`, `_Ticks`): one run a length from the same snapshots,
alternating twice. Tiberian Dawn's instructions a second, the shell loop's, Quake's frames in 30 s (`QUAKE_TRACK`):

| Instructions a pass | Shell | Tiberian Dawn | Quake |
|---|---|---|---|
| 8,192 | 4.62 M/s | 5.12 M/s | 2,217 frames |
| 10,240 | +1.7% | +2.3% | -2.9% |
| 12,288 | +3.7% | +3.6% | +4.7% |
| 14,336 | +4.6% | +4.4% | +2.5% |
| 16,384 | +5.5% | +5.3% | +2.9% |
| 20,480 | +5.8% | +6.4% | +0.5% |

Where core 0's passes end with a budget of 32,768 (`--frame-log`, `python tools/pass_ends.py`):

| Load | Ended before 8k | before 12k | before 16k | before 24k | write cache full | waiting | ran to the budget |
|---|---|---|---|---|---|---|---|
| Quake, e1m1 | 52.4% | 62.4% | 78.5% | 96.0% | 0.8% | 98.3% | 0.9% |
| Tiberian Dawn | 2.2% | 10.2% | 24.7% | 32.7% | 29.8% | 1.2% | 67.0% |
| Shell loop | 0 | 0 | 0 | 0 | 0 | 0 | 100% |

- Tiberian Dawn's passes that the write cache ends begin at about 9k instructions, are most at 12k to 13k and reach 18k: the
  cache's 336 texels are a pass of about 12k of its code. At 8,192 none ends there; at 12,288 4.5% of passes do (0.4% of the
  instructions), at 16,384 13.3% (2.7%).
- Quake's core 0 waits for its clock, not for a worker: `main` (`sys_shaderemu.c`) pauses until `Sys_Milliseconds()` moves,
  and `Host_FilterTime` runs a frame every 10 ms of the guest's (`host.c`); the server's frame and part of the drawing are
  the workers'. Its pauses come in clusters (0-1k, 4-5k, 11-13k, 16-18k instructions into a pass), and its frames a
  second are how those line up with the clock, so they say little of the length's worth. A program whose cores wait for
  each other every frame (nxphys, Doom's view, Jedi Outcast's drawing worker) pays every wait longer with a longer pass,
  as stores reach RAM only at the commit; none was measured here.
- 12,288 is where the gain of fewer rounds' fixed costs (about 6% of a round at 8,192) meets the write cache and the waits:
  +3.7 to +4.7% on all three. Not taken: it is the machine's setting for every program and every world, measured on two.

**What a round is made of on this card** (`RVC_PASS_PROF=1`, 600 frames from each snapshot, ms a round):

| Load | tick | unpack | gather | commit rows | commit points | devices | devices with `RVC_LIST_LAZY=1` |
|---|---|---|---|---|---|---|---|
| Shell loop | 1.55 | 0.03 | 0.006 | 0.005 | 0.015 | 0.110 | 0.008 |
| Tiberian Dawn | 1.39 | 0.03 | 0.006 | 0.005 | 0.015 | 0.111 | 0.009 |

The devices' 0.11 ms is the GPU device's list pass, drawn every round whether a list is due or not: 6.6 to 7.4% of a
round in the harness, which draws the devices every round. The world draws them once a Unity frame (about 8 rounds), so
there they are under 1% of a frame. `RVC_LIST_LAZY` (opt-in, `a4ff4d46`) still has the bug `riscv-backport-plan.md` names:
a list submitted in a frame where none was drawn is never drawn; its fix is gpu_vm's, under `GVM_RAM` in `src/gpu.h`. It
also means the pass's length is worth less in the world than in the harness: of a round's fixed 0.17 ms here only about
0.056 (unpack, gather, the commit's rows and points) is a round's in the world, so 12,288 there would gain a third of 3.5%
and the tick's own fixed part, not the harness's 4 to 6%.

| Variant (changes no result) | Quake | Tiberian Dawn | Shell | |
|---|---|---|---|---|
| k3c (f2 + fmadd rounded twice) with `LOAD_UP` by selects (lu1) and no budget test on the fused word (nb): k4 | +0.1% | -1.25% | -0.45% | one run: to be run again |
| lu1 alone (on k3c); mh1 (mulh by `mulhi`); id1 (a texel's word by masks) | -0.8%, -0.05%; -0.1%; +0.2% | -0.5%, -0.4%; +0.3%; +0.6% | -0.2%, -0.1%; +0.4%; +0.8% | level |
| aui1: an addi of lui's or auipc's register fused into the jumps' block | +1.2% | +0.3% | +1.2% | lost |

**Kept** (`shader_vm/tick.hlsl` and Unity's copy, stamp `0a78efe277ddf37f`; `test_card.py mctest-3 fptest lanetest` pass on
the card): k4lb, no result changed. The texel's next word in the same turn when it is arithmetic or a branch after
arithmetic, or a branch after a load (its registers taken from the first word's result where it names it), with no test of
the budget (a pass may run one instruction past it: `pre_valid` reads false, the outer loop ends, a lane's `lane_left`
floors at 0); fmadd, fmsub, fnmsub and fnmadd in the fast loop as the general step makes them (a product and a sum, each
rounded: `rvc_cpu`'s `fmaf` rounds once, as before); `LOAD_UP` by selects. Against a copy of the tree in each of three runs:

| | Quake | Tiberian Dawn | Shell |
|---|---|---|---|
| k4lb | -0.5%, +0.6%, -0.2% | -2.2%, -2.2%, -1.9% | -0.3%, -0.25%, -0.35% |
| k6 (the same with `n + 1 < budget` on every fused word) | +0.9% | -0.7% | +0.8% |
| k5la, k5fb, k5x (k4lb and arithmetic after a load; a branch after a float comparison; both) | +1.9%, +0.3%, +1.5% | -1.9%, -2.2%, -2.2% | -0.3%, -0.25%, -0.25% |

On the kept tree (a copy of it in each run):

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| **mul as the fused second word** (its product only in the fused copy: a lone arithmetic word pays nothing) | -0.4%, -0.7% | -2.5%, -1.7% | +0.15%, +0.15% | **kept** (stamp `e0da98763fad1fdc`; `test_card.py mctest-3 fptest lanetest tdawn-300` pass on the card) |
| Arithmetic fused after a lone mul (t2mm, on the above) | -0.8% | -1.6% | +0.3% | level against it |
| A branch as the third word after a fused pair (t2t3) | -0.1% | +1.2% | +0.3% | lost |
| The fused branch tested before the fused arithmetic (t2ord) | +1.4% | +1.5% | +0.3% | lost |
| The fused word reading its registers after the write (t2nf); `LOAD_UP` as before (t2lu0); no fast fmadd (t2ffma0); fb1 on the kept tree | +0.4%; -0.3%; +0.5%; +0.2% | 0.0%; -0.1%; -0.2%; +0.4% | -0.5%; -0.45%; -0.35%; +0.15% | level: the kept parts are worth what the whole is, not each alone |

The two kept changes together against the tree of `62c2cd86`, in one run (a copy of the new tree beside it):

| Load | Before | After | |
|---|---|---|---|
| Tiberian Dawn | 1.4185 | 1.3570 | -4.3% a tick (4.5% more instructions a second) |
| Quake, e1m1 | 1.6944 | 1.6815 | -0.8% |
| Shell loop | 1.5905 | 1.5855 | -0.3% |

A jal as the fused second word after arithmetic (its target's bit 21 in the test, so a pass never commits half a pair):
Quake +1.2%, Tiberian Dawn +2.0%, the shell +0.6%: lost.

On the tree with both (each copy of the tree made again from the merged `shader_vm` after the GPU device changed: a copy
with the old `gpu.shader` serves no list under the new harness, and a game waiting for one runs a few instructions a
frame):

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| **M before the atomics** (round two found it level on the shell and Quake: Tiberian Dawn has 4.5% of mul) | -0.4%, +0.1% | -1.2%, -1.2% | -0.4%, -0.45% | **kept**: stamp `40eaa083dd0828bc`, `test_card.py mctest-3 fptest lanetest tdawn-300` pass |
| M before the jumps | +1.0% | -1.1% | +0.4% | lost on Quake |
| mulhi; fdiv inline as the card's (fp1); fcmpn; pfp; fp3; la1; br1 | -0.4%, +0.3%, +0.5%, -0.1%, +0.8%, +0.7%, +1.0% | +0.4%, +0.2%, +0.6%, -0.1%, +0.2%, -0.8%, -0.9% | 0.0%, +0.35%, +0.15%, -0.1%, +0.3%, +0.7%, - | level or lost |
| Each kept part out alone: the fused arithmetic only for op-imm; no fused branch after arithmetic; no branch after a load | +1.4%; -0.75%; +0.2% | +4.3%; +0.2%; +0.8% | +0.1%; +0.2%; 0.0% | the parts are kept together |
| lui or auipc as the fused second word; the second word's fields read before the first's result | +1.2%; +0.9% | +2.1%; +0.9% | +1.9%; +0.5% | lost |

On the tree with M before the atomics:

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| The CSRs before the atomics; mul in the second group, before the branches | +0.6%; +1.7% | +1.2%; +0.6% | +0.7%; +0.85% | lost |
| st2 (a sw into the texel remembered last, fused after a sw); an op-imm word fused after a load | +1.9%; +1.4% | +2.6%; +2.4% | +1.7%; 0.0% | lost |
| fb1; fp1 | -0.8%; -0.4% | +0.7%; -0.1% | +0.5%; 0.0% | level |
| Another first hash of the write cache | | | | not run: `src/fb.h` and `src/types.h` find texels in the cores' caches by the same hash (`L1_H1`), so the tick's alone cannot change |

More on the tree with M first (Quake, Tiberian Dawn, shell, against a copy of it):

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| mul among arithmetic's selects (m1); la1; fp3; pfp | +2.0%; +1.7%; +1.1%; +0.4% | +0.1%; +0.3%; +0.9%; -0.3% | +1.0%; +1.5%; +0.3%; +0.6% | lost or level |
| mul as a plain branch, not op_m's return (t4om), in three runs | -1.1%, -0.25%, -0.3% | -0.3%, -0.4%, +0.4% | 0.0%, +0.1%, 0.0% | level |
| The branch after a load only for beq and bne (t4nl2), in two runs; with t4om | -0.75%, +0.6%; +0.7% | -1.1%, +0.1%; +0.2% | -0.1%, +0.3%; +0.1% | level |
| The fused branch after arithmetic only for beq and bne; a jalr fused after arithmetic, committed only on a word | -0.2%; +1.1% | +1.3%; +2.0% | +0.2%; +1.45% | lost |
| fmadd as one `mad`; fmadd before the atomics | -0.9%; -0.3% | +0.1%; +2.0% | -0.1%; +1.0% | level or lost |
| The fused branch forwarding only its first register; a load's check by alignment alone; `imm_b` from the word's sign; flw before 0x53 | -1.1%; +1.1%; +0.5%; -0.4% | +1.3%; +2.2%; +1.3%; +1.8% | +0.45%; +1.0%; +0.5%; +1.1% | lost |
| The fused word's sub by one mask; its rd 0 by an or; a branch's less-than by two compares; arithmetic's M test by funct7; the fused word from the texel, not the turned window; the same for the load's; shifts by `y & 31`; slt by two compares; the write before the forwarding selects | +0.4%; +1.9%; +2.0%; +1.4%; +7.6%; +7.3%; +2.0%; +1.8%; +0.4% | +0.7%; +0.2%; +0.5%; +0.4%; +3.5%; +1.2%; +0.3%; +0.4%; -0.3% | +0.9%; +1.0%; +1.2%; +0.3%; +7.7%; +6.5%; +0.6%; +0.6%; +0.3% | lost: the kept text's form is what it gains by |

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| **The fused second word from the next texel** (the one read ahead, `fw1`, while `fw_t` is this texel), for arithmetic's and the load's fused words; the window becomes that texel (t4x3) | -0.1%, -0.2% | -1.1%, -1.4% | +0.15%, +0.3% | **kept**: stamp `c405174991cbf98e`, `test_card.py mctest-3 fptest lanetest tdawn-300` pass |
| The same for arithmetic's only (t4x) | +1.3%, +0.25% | -2.4%, -2.7% | +0.9%, +0.7% | the shell pays: not taken |
| The same, the fetch's state moved on with it and the texel after read ahead | +0.8% | -1.3% | +1.5% | lost |

**The four kept changes together** against `62c2cd86`'s tick, in one run (the tick alone swapped, the device's files the
merged tree's for both):

| Load | Before | After | |
|---|---|---|---|
| Tiberian Dawn | 1.4404 | 1.3495 | -6.3% a tick (6.7% more instructions a second) |
| Shell loop | 1.6365 | 1.6267 | -0.6% |
| Quake, e1m1 | 1.7582 | 1.7521 | -0.35% |

On the tree with all four (against a copy of it in each run; a variant that stops the machine spoils every tick of its run,
so a patch applied with fuzz to the moved text was looked at first: br3's and la1's landed in the wrong blocks and were
dropped):

| What | Quake | Tiberian Dawn | Shell | Verdict |
|---|---|---|---|---|
| op_m's mul as a branch; mulhi; pfp; fp1; beq/bne after a load; M before the jumps; fb1 | -0.5%; +0.1%; -0.4%; -0.1%; +0.15%; +0.4%; -1.1% | -0.2%; 0.0%; +0.3%; +0.4%; +0.2%; +0.3%; -0.3% | +0.3%; +0.4%; +0.6%; +0.8%; +0.65%; +1.3%; +0.5% | level |
| Pairs and all three of op_m's branch, fb1 and pfp; op_m's branch with mulhi; the crossing's test as one comparison; a load's range test first; crossing only after arithmetic | -0.6 to +0.5% | -0.6 to +0.1% | -0.8 to +0.2% | level |
| The local arrays larger: the cache's 456, 464, 480, 512; the registers' 66, 68, 72, 80, 96 | -0.8 to +1.0% | -0.5 to +0.2% | -0.2 to +1.6% | level |

**What was learnt.** Nothing of the fast loop's shape gave more than 1% on every load: f2 + fp1 (+ ffma) about 1%, at the
noise's edge; the kept fusion 2% of Tiberian Dawn, a little of the shell, level on Quake. fp1 changes what fdiv makes (the card's quotient, which `rvc_cpu` does not make: a model of the card's
division is owed first), and ffma's `mad` is fused where the general step rounds twice: neither is in the tree. The ways
to more are outside the loop's text: the pass's length (with the write cache, which caps it at about 12k of Tiberian Dawn's
code), a larger write cache, and fewer instructions (Zba and fmadd in the programs: `docs/plans/riscv-isa.md`).
