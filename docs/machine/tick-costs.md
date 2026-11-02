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
