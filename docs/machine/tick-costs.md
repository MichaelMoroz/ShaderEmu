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
on a frozen state and counts the same pass with `--profile`, and `python tools\pass_regress.py logs\pass_costs.csv` fits a
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

These are the fast step's rules (`fast_run_l1` in `cpu.h`, `fast_exec_l1` in `emu.h`, `mem_get_cached_or_tex_l1` in
`mem.h`). The text's form is the gain, and most of the time goes to which blocks an instruction passes on its way, which
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
- A smaller pass is not a faster one, and taking code out of the function can cost as much as adding it: see "The same
  loop, compiled slower" in `experiments/rvc_opt/README.md`.
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
a pass costs besides its instructions, `--no-mrt` the same with one target and one core.

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
