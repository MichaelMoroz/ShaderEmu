# Handoff: gpu_vm ISA, tick, LLVM, Linux (23 October 2026)

## For the next machine (RTX 5070 Laptop), 23 October 2026: real workloads, and the way to 2x (read this first)

### The goal, in the user's words

The goal is **twice the RISC-V machine's frame rate on most games, measured on real workloads**, with multiple workers
working correctly and nothing slowing down. The bench's loops already pass 2x; they are not the goal. **ISA changes and
compiler changes are completely allowed.** How it gets done does not matter, as long as it fits in a fragment shader
**without UAVs**: the machine runs in VRChat, where a pixel shader writes only its render targets. So no compute
shaders and no one-lane UAV ticks (x19u's `fragu` is a dead end for this reason). It may need a slight redesign of the
ISA and the virtual machine. One idea the user raised: a memory model with an explicit cache, so that LLVM can manage
the cache itself.

### Where it stands (RTX 5090, D3D11, fxc2, from game checkpoints, 15 s a run)

| | RISC-V (`rvc_opt`) | gpu_vm x18 | x21 (geometry) | x22 (geometry + texel cache) |
|---|---|---|---|---|
| Quake III, guest fps | 77 | 28 (100 frames in 3.6 s) | 42 (2.35 s) | **50** (1.94-2.02 s) |
| Half-Life, frames in 15 s | ~495 (397 in 12 s) | 243 | 246 | **275** |
| nxray 320 x 240, 255 workers, pictures/s | 37-42 | 7.4-8.9 | 25-27 | **38-39** |
| Red Alert, game frames/s | 20.8 | 20.7 | 20.7 | 20.7 (idle-paced: no measure of speed) |
| core 0 bundles a pass: Q3 / HL / RA | | 955 / 1,293 / 1,855 | 951 / 1,300 / 1,851 | 1,581 / 2,738 / 3,183 |
| nxray workers, ops/s (all cores) | | 127 M | 448 M | 641 M |

So multi-core work is at parity with RISC-V, Quake III is 0.65x of it, and Half-Life 0.55x. **2x means Quake III at
about 154 fps and Half-Life at about 66**, roughly 3.1x and 3.6x where x22 is now.

### x23 on the 5070 (23 October 2026, plan item 2 and pitfall 6)

`gpu_vm/tick/steps/x23.hlsl` (README "x23"): x22's state layout, but the cache in two tables of 32 buckets of four ways,
a lookup reading the tags and then only the word that hits, and the bloom word for loads. Half-Life from a checkpoint
(`build/gvm_snap/hl.snap`, made here at `hlmark: begins`; `build/gvm_snap/hl.ps1 -Tick T -Name N` measures 15 s and
`fl.py` prints bundles a pass and why passes end, the end reason being the frame log's third word & 255):
core 0 2,120 -> 4,020 bundles a pass, `hlmark` frames 108 -> 154; gzip 258 -> 219 ns a bundle, gunzip 212 -> 182,
mc_int/mc_load at 255 as x22 (1.60/2.84 ns); the loads and stores loops 14% slower than x22. HL still ends 84% of its
passes full (capacity now: 256 texels in at most 384 state texels). Not made the harness's default (the user's call).
Not done: Quake III, nxray and Red Alert checkpoints on this machine, a larger block for core 0.

### What changed this session (committed)

- **x21: the RISC-V tick's geometry** (docs/machine/multicore.md, "The geometry"). Core c's 38 state pixels sit in a
  block of 5 of 8 columns and 8 of 16 rows, at x = (c & 127) * 8 + j / 8, y = (c >> 7) * 16 + j % 8. In x18 a core
  was a row and adjacent rows were adjacent cores, so every warp held 4 to 8 cores and cost all their work. This
  change alone made nxray 3.2x faster. It is a step constant (`GEO_BLOCKS = 1`) that the harness, gvmbench and the
  control pass all read.
- **x22: RISC-V's write cache** (experiments/rvc_opt/src/mem.h). An entry is a RAM texel whose tag carries a 4-bit
  mask of the words stored, so the commit still writes words and the cross-core semantics are x18's. There are two
  tables of 128 entries with independent hashes (the second is mem.h's `L1_H1`), then two overflow slots. That is 258
  entries in 363 state texels (46 pixels, inside the 6 columns a block may run). The step's `NTEX` gives the harness
  the state size.
- **Harness gvm snapshots** (`--save-state` / `--load-state` with `--gvm`): RAM, both state sets, the operations'
  textures and the GPU picture. Loading converts between the row and block layouts and between cache sizes, keeping
  texels below T_WC.
- **x20, a cuckoo cache on the word entries**, was measured and rejected: it costs 5-29% per bundle. It is kept in
  steps/ as a record.

### Pitfalls with real workloads (each one cost a round here)

1. **The uncapped harness hides the cost that matters.** `rvc_harness` draws as many frames as it can, one tick pass
   each. VRChat runs about one pass per display frame, so there **bundles per pass is the speed**, and a pass that
   ends at 1,300 of its 8,192-bundle budget throws away 84% of the frame. Report bundles per pass (`--frame-log`;
   the STATS `per_frame` field) next to guest fps, every time. In the harness the fixed cost of a pass is only about
   11 µs, so longer passes look free there, and that is misleading.
2. **The many-core bench (mc_int, mc_load) runs the same code on every core**, which hides warp sharing: a shared
   warp costs nothing when its cores run the same instructions. On it x18 beat x21 (0.48 against 0.60 ns a bundle at
   255 cores) while nxray ran 3.2x faster on x21. Judge geometry with nxray from its checkpoint.
3. **The control pass reads core state by layout** (`experiments/rvc_opt/src/gpu.h`, `mc_stat_count` and
   `mc_stat_bits`, through `gvm_s16()` and `_GvmGeo`). Its running and asleep bits drive the harness's freezing of
   idle workers. Change the layout without updating it and the worker instruction counts turn absurd (10^13): that
   was the symptom here.
4. **Every gvmbench loop is a tight loop.** Real code costs about 0.34 µs a bundle (HL: 0.49 ms for 1,290 bundles)
   against about 0.05 µs for the loops. Nobody has explained that gap yet. It is the biggest unknown, and measuring
   it is the first job (Nsight on `gvmtrace12`, D3D12 with debug info, real code from a checkpoint).
5. **Why passes end** (`frames.py`-style histogram of `--frame-log`: pc, bundles, why). On x22 Half-Life still ends
   93.5% of its passes on a full write cache, at 2,331 bundles. Quake III ends 66% that way and 34% on sync, its
   `shaderemu_gpu_rest` at 0x0230fe4c. Red Alert waits 18% of the time and is idle-paced: it needs a heavier scene
   to measure anything.
6. **x22's lookup reads both candidate entries' 16-byte texels on every memory op**: the bench's loads lose 15%,
   stores 26% and stride 25% against x21. Read the tag texel first and the data only on a hit (the tags already sit
   in texels of their own), and add RISC-V's bloom word (`mem_cache_bloom`: loads skip the lookup when the address
   has a bit no store had).
7. **HLSL's `&&` and `||` do not short-circuit.** A side-effecting call in a condition runs every time. x20's first
   version committed placeholders this way. Nest the ifs.
8. **nxray captures are not deterministic**: two runs of one tick differ in 0.9% of pixels (it animates by the host's
   clock). Correctness comes from gvmbench against the reference on WARP, then on the card at 255 cores, which
   crosses the second band.
9. **WARP at many cores runs past 5 minutes** (mc_int at 160 cores was stopped). Check the full default set on WARP
   (about 2 minutes), then many cores on the card.
10. **The checkpoints are bound to their image.** Rebuilding `build/gvm_apps/img` invalidates them. They are 254 MB
    each, machine-local and not in git, so the 5070 makes its own (see below).

### What should be done to get to 2x (in order)

1. **Find where a real bundle's 0.34 µs goes** (pitfall 4): Nsight per-shader on real code. Candidates are the 148 KB
   fragment shader's instruction-cache misses on paths real code touches, local-array traffic (x22 keeps 5 KB of
   arrays a pixel), and the general step for bundles the fast loop does not take. Before any of that, measure the
   share of bundles that leave the fast loop.
2. **The write cache** (Half-Life ends 93% of its passes full). Do pitfall 6 first. Then add a third table (RISC-V has
   tables of 64, 32 and 16 buckets of three ways) or more ways a bucket, sized so a worker stays at 6 columns or
   fewer. Core 0 may have a block of its own, larger than a worker's.
3. **The ISA and the compiler, now allowed**. The user's own idea is a memory model the compiler manages. Some options:
   - a core-private scratch region (the stack, spills) kept in state texels or a per-core RAM region, never hashed
     into the write cache;
   - stores coalesced into 16-byte stores (`GVM_W_Q` exists) so that one entry is one store;
   - fewer spills (64 registers exist).

   Whatever is built must keep worker stores visible to other cores at the commit.
4. **The compiler subagent's work (stopped unfinished, committed separately, every option OFF by default).** It worked
   from `gvm_cpu`, not the card. What it did:
   - `gvm_cpu --profile` counts the bundles run by address, core 0's and the workers'.
   - What it found: real games run 1.32-1.60 ops a bundle. The fast path takes 59% (nxray, whose float bundles take
     step12) to 84% (Doom, Quake III) of bundles. A single-op bundle is most often an op followed by a block start
     (16%), an op that shares its bundle with a branch, or a branch on a register the previous bundle wrote.
   - Three compiler options, all defaulting to false:
     - `-mllvm -gpuvm-sched-kind` pairs ops of one kind first in the bundle scheduler;
     - `-mllvm -gpuvm-mask-select` makes an integer select lite ops on a 0/1 mask;
     - `-mllvm -gpuvm-fnrel` makes PIC addresses function-relative (one `lpc` a function, globals as a base plus a
       `%fnrel` offset), with an assembler specifier, a fixup and a PCIMM32 relocation.
   - Its estimates, from gvm_cpu's profile and a card cost model, never measured on the card:
     - all three on: Doom -3.3% bundles (est. -14.9% card time), Quake III est. -8%, nxray unchanged;
     - masks and kind pairing alone: about 1%, and CoreMark 113,110 to 114,406 bundles an iteration (worse).
   - **Not verified:** c-torture was not rerun with the options on (the baseline is 99.7% of what the host passes at
     -O0 and -O2); the `nxray` test case failed in its runs with the options; whether the defaults-off build
     matches the baseline was not checked either. A `build/gvm_apps/img-cc` image from its last iteration exists on
     the 5090 machine only.
5. **Tune for the card in use** (not done on the 5090 either): `--tick-rounds`, the `--ticks` budget, and whether
   core 0 should be a larger block.

### The benchmark: the state it is in, and how to run it

- **Tick correctness**: `bin\gvmbench.exe --warp --shader gpu_vm/tick/steps/x22.hlsl` reads "same as the reference"
  (129 s). On the card: `pwsh tools\gpu_queue.ps1 bin\gvmbench.exe --variants gpu_vm/tick/variants/x22a.txt`, and
  the same with `--case mc_int,mc_load --cores 255`: all three steps are the same as the reference.
- **The bench's latest numbers** (5090; loops in M RISC-V-equivalent instructions a second, mc in ns a bundle):

  | | arith | loads | stores | mix | stride | x lean | mc_int 255 | mc_load 255 |
  |---|---|---|---|---|---|---|---|---|
  | x18 | 20.63 | 11.91 | 10.85 | 18.39 | 14.27 | 2.10 | 0.48 | 0.76 |
  | x21 | 20.81 | 11.66 | 10.90 | 17.90 | 13.91 | 2.10 | 0.60 | 1.07 |
  | x22 | 20.91 | 9.90 | 8.02 | 16.08 | 10.46 | 1.89 | 0.59 | 1.07 |

- **Game checkpoints** (make them once per image on this machine; each run goes through `tools\gpu_queue.ps1`):
  1. Boot each machine once per game with `--save-state` when the game prints its mark. The marks are
     `q3stat: frame 100:`, `rastat: frame`, `raystat: 320 x 240` and `hlmark: begins`.
     - gpu_vm: `bin\rvc_harness.exe --gvm build\gvm_apps\img --no-viz`.
     - RISC-V: the main checkout's `rvc_harness --d3d11 --rvc experiments\rvc_opt --define NO_PAGING --image
       linux-net`, from a root whose `build/images/linux` is the nommu image.
  2. Then measure with `--load-state X.snap --seconds 15 --stats-after 2 --frame-log F --no-stdin --uart-log L`, and
     for gpu_vm `--gvm-tick gpu_vm/tick/steps/x22.hlsl`. Read the guest's own lines from the UART log (`q3stat:
     frame`, `hlmark: ends`, `raystat:`) and the harness's STATS line.
  3. A 15 s run from a checkpoint costs about 20 s, so all four games on one tick take under 2 minutes.
- **The default tick is still x18** in the harness (`gvm_backend11.cpp`). Pass `--gvm-tick
  gpu_vm/tick/steps/x22.hlsl` to measure x22. Making it the default is the user's call.

## Where it stands at the end of 22 October 2026 (wrapped up; read this first)

- **The ISA** is gpu_vm (`gpu_vm/isa.h`, the reference `gpu_vm/ref/gvm_ref.h`); **the tick** is x18
  (`gpu_vm/tick/steps/x18.hlsl`, plain, no switches); x19's experiments found no better default (README "x19").
- **The toolchain**: LLVM 23.1.4's GPUVM target and clang (`gpu_vm/llvm/README.md`), lld, musl, compiler-rt, a libm
  on the card's float ops and libmfloat, gpuvm-linux-cc (PIE), elf2flt (`gpu_vm/userland/README.md`). c-torture 99.7%
  of what the host passes; CoreMark 113k bundles an iteration (2.39 RISC-V instructions a bundle).
- **Linux 5.17.11 `arch/gpuvm`** (`gpu_vm/linux`), the machine's drivers (fb, gpu with workers, input, sound, net),
  boots on gvm_cpu, on the card in gvm_gpu, and as a whole machine in `rvc_harness --gvm` (`gpu_vm/platform.md`:
  devices, up to 255 workers through a firmware, the desktop); busybox userland (`gpu_vm/userland`).
- **The programs and games** (`gpu_vm/userland/apps/README.md`): everything builds but OpenTTD (no C++ exceptions:
  no unwind info) and TinyCC (no gpu_vm back end); 102 of 131 test_all cases pass on gvm_cpu (more alone), ~35 on the
  card (Doom's digests, Micropolis, the C&C games, the NES, workers, ray tracers by PSNR, media).
- **Speed against the lean RISC-V tick on the card** (`gpu_vm/bench_real`): CoreMark 2.11x and gzip 2.02x against
  its best flags, gunzip 1.62x; the bench's seven loops 2.24-2.74x. A bundle's fixed cost (~105 ns) is the floor
  x19 could not move; memory-heavy code ends passes on a full write cache (a bigger cache helps one core and costs
  many cores).
- **Open**: the game frame-rate comparison against the RISC-V machine (the games run; not yet timed side by side);
  gunzip's 1.6x; Red Alert's movie with more than one worker; Half-Life's races (0 or 7 undone); compiler bugs
  far_call.c (a call over 2 MB in one section: worked around with -ffunction-sections) and weak_undef.c (an
  undefined weak function's address under PIE); C++ exceptions; TinyCC's back end; exact Newton-refined
  sqrt/div and real double ops in the ISA (proposed, not done).

## The task (the user's words, condensed)

Starting from `gpu_vm/GPU_VM.md`: (1) download and build LLVM, then an gpu_vm target for it; (2) meanwhile design the new
tick shader with everything gpu_vm asks for (FPU, special functions, the operations region in memory, the new memory
layout); (3) port Linux to it from scratch. The tick must be **2-3x faster than the RISC-V lean tick in effective
work a second** (`experiments/rvc_lean`).

Rules the user gave along the way:
- Build the tick **in small steps** from the last verified fast step, one feature at a time, measured each time.
  Keep it **under 500 lines** and smaller than the RISC-V tick (1,192 lines).
- Compile with **fxc2** (`tools/fxc2/d3dcompiler_47.dll` beside the exe).
- Keep **every test run under 5 minutes**.
- Try **~20 variants at a time** in one run and keep the best.
- Use **Nsight Graphics** for profiling; debugging the shader properly needs **D3D12 and debug symbols**
  (the earlier sessions' method: `tools/gpu_trace.ps1`, `rvc_trace12`, the harness's `RVC_DUMP_PRE`).
- Commit with no Claude/AI attribution (global CLAUDE.md). Don't push.
- **The ISA is called gpu_vm** (the user, 22 October 2026; first SX, and not SVM, AMD's virtualization): the folder
  `gpu_vm/`, the macros `GVM_`, functions `gvm_`, the tools `gvmbench` and `gvmtrace12`, the LLVM target `GPUVM` and
  Linux's arch `gpuvm`. Renamed on 22 October 2026 (no SX name is left in the tree; old commits say SX). In WSL the
  LLVM folders are `~/gpu_vm/...`; `~/sx` is a link to it, kept because the build tree has the old absolute paths
  (remove it with the next clean LLVM configure). The lean baseline image is `build/images/lean_isaloops`. The machine is
  not x86: the name ShaderX86 found in the tree (the GPU queue's mutex, the porting skill) means nothing for it.
- **GPU safety** (22 October 2026: the machine crashed with a GPU fault during an unchecked tick variant's batch,
  x18e, and the drivers had to be reinstalled): every new tick variant passes the full check on WARP first (a hang
  there is harmless); never time a variant on the card with `--nocheck` unless that source passed WARP's checks; every
  loop of the tick has a hard iteration cap independent of the budget logic.
- **Floats: close enough** (the user, 22 October 2026): float results may differ between the card and gvm_cpu as
  long as they are close; programs use the card's special functions (-fapprox-func) and fmad where faster, and their
  checks compare with tolerances (pixels, relative error), exact sums only where logic must stay deterministic.
- **Performance first** (the user, 22 October 2026): an ISA change that makes the tick faster, or keeps it 2-3x the
  lean RISC-V tick, takes priority over what it costs the toolchain and the built programs (the reference, the
  assembler, LLVM's MC layer and lld, the rebuilt runtimes, userland and kernel follow it).
- **The ISA is free to change** (the user, 22 October 2026): encodings, fields, paths, new ops (fused ones too)
  wherever that makes the tick faster. `gpu_vm/isa.h`, `gpu_vm/ref/gvm_ref.h` and `gpu_vm/ref/gvm_asm.h` change with it, and the
  check cases must still agree with the reference.
- Subagents propose tick variants and ideas (each in its own copy of the step, checked on WARP), and the lead runs
  the whole batch on the card, then repeats (the user, 22 October 2026).

## The user's last message (to act on next)

> So the obvious question is, what fraction of instructions do floats have in a realistic use case? In the Linux
> system, I think there's like zero. In games like Quake, maybe some percentage. Maybe we should have two
> branches: one fast one with only integer instructions and one slower one with all instructions. Continue trying
> to get higher performance. Also, I would suggest to somehow try fusing instructions.

So next:
1. **Measure the float share** of real programs: `RVC_PROF` / `tools/prof.py` on the RISC-V machine
   (docs/machine/profiling.md) for a shell loop, gzip, Doom, Quake. Expect ~0 for Linux, some % for Quake.
2. **Two loops**: a fast loop with integer + memory only (no float code compiled into it at all), and a slower
   loop with everything; a bundle the fast loop can't make leaves it for the slow one. x12c's probes showed that
   code which isn't run still slows the bundles that are (no store code: loads 6.05 -> 7.86; no float body: arith
   13.7 -> 14.2). Separate loops (not separate branches) are the way to keep that code out.
3. **Fused ops**: new gpu_vm ops that do two things in one slot where common pairs show up (e.g. compare-and-select,
   add-then-load / loads with a post-increment, shift-and-add already exists as `shadd`, add-and-mask as s18 had,
   load+extend, a branch's decrement). Look at what the lean tick's loops and real code pair most.
4. Then keep going: the tick to 2-3x, then the LLVM gpu_vm target, then gvm_cpu and Linux.

## Where things are

Worktree `E:\Development\Emu\ShaderEmu-custom_isa`, branch `custom_isa` (tracks origin/custom_isa), clean, two
commits on top of origin: `7a8f63f` (reference, bench, steps x1-x11), `f9a2857` (floats, x12, batches, gvmtrace12,
trace.ps1). Not pushed.

- `gpu_vm/GPU_VM.md` — the original design. **Out of date**: gpu_vm now has two op slots, the control word's path/long bits,
  renumbered integer and float functions, branches 9 bits, j/call 18 bits. `gpu_vm/isa.h` is the truth; update GPU_VM.md.
- `gpu_vm/isa.h` — the encoding. Control word: n (0-2 ops) bits 1:0; bit 2 word 2 is a long immediate, bit 3 it is
  op 1's; path bits 6:4 (0 general, 1 int, 2 mem+lite, 3 mem+mem, 4 float+lite, 5 float+float-lite, 6 mem+float-lite;
  "lite" = integer f 0-10, "float lite" = float f 0-7); cc 10:7; cc 0: sub 13:11, rs 19:14, off18 31:14; branches:
  ra 16:11, rb/imm6 22:17, off9 31:23. **Branches compare the registers as the bundle found them** (user's choice);
  jr/callr read after the ops.
- `gpu_vm/ref/gvm_ref.h` — the reference machine (every op, traps, SRs, passes, write log, commit, copy/fill/blit).
- `gpu_vm/ref/gvm_asm.h` — test assembler; packs ops two a bundle and sets path/long bits.
- `gpu_vm/tick/steps/x1..x12.hlsl` + `README.md` — every step and probe with numbers. **x12** (428 lines) is the
  current full fast-path tick; best variant `-D OK_MIN=1 -D INT_FIRST=1`.
- `gpu_vm/tick/draft_full.hlsl` — the first big tick (correct, slow); a sketch of semantics only (incl. the general
  step, traps, SRs, the operations' stage/write/progress passes).
- `gpu_vm/tick/gvmbench.cpp` + `cases.h` — bench: `bin\gvmbench.exe --shader F [-D ..] | --variants FILE`, `--case`,
  `--rounds N`, `--passes N`, `--nocheck`. Checks against the reference, times isaloops' loops, prints x lean.
  Build: `gpu_vm\tick\build.bat`. Run on the card through `pwsh tools\gpu_queue.ps1`.
- `gpu_vm/tick/variants/x12a-d.txt` — the batches so far.
- `gpu_vm/tick/gvmtrace12.cpp` — the tick on D3D12, DXC `-Zi -Qembed_debug -O3 -HV 2016` sm 6.6 (or `--dxbc`).
- `gpu_vm/tick/trace.ps1` — Nsight GPU Trace (real-time shader profiler, Blackwell GB20x) of gvmtrace12;
  exports under `logs\nsight-gvm\`; the per-line SASS view is only in the Nsight UI (.ngfx-gputrace).
- `gpu_vm/llvm/build.sh` — LLVM 23.1.4 in WSL (`~/gpu_vm/llvm-project`, build `~/gpu_vm/llvm-build`): clang, lld, llc, llvm-mc
  for X86+RISCV are **built**. No gpu_vm target yet (`gpu_vm/llvm/Target/GPUVM` gets linked in when it exists).
- Lean baseline image: main checkout's `build/images/lean_isaloops` (isaloops built by `~/nm/work/bin/rv32-cc -O2`,
  `linux/nommu/elf2bflt.py`; script in the old scratchpad `lean.sh`). Run:
  `bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_lean --image build\images\lean_isaloops --no-stdin --expect "laid out 255" --send "isaloops 16; isaloops 16; echo GVM-D''ONE\n" --until "GVM-DONE"`.

## State at the end of 22 October 2026 (the goal "2x on the most frequent code" is met)

- **x15m q4ovf** (`gpu_vm/tick/steps/x15m.hlsl -D MEM4=1 -D MOP2=1 -D EXT2=1 -D OVF=1 -D WCN=130`) is two times the lean
  tick on all seven integer and memory loops, the reference's on the card: arith 21.0, loads 12.6, stores 11.7,
  adds 22.6, tight 19.8, mix 17.6, stride 18.0 (2.21 to 2.83 times; 2.55 overall). q4w1ovf (one way) is 2.65.
  Timed with `--fused 1` (the loops end in addbnz). History and every batch: `gpu_vm/tick/steps/README.md` from "x13".
- **x16** (`gpu_vm/tick/steps/x16.hlsl`) is the tick as it should be kept: no switch and no macro (the user: as simple
  as possible, the same DXBC), sq_sw2's structure, RAM 8192 texels wide, the ROM a read-only texture of its own read by
  every load (bit 30 chooses; free, so kept). 2.62 times the lean tick, the reference's on the card (`rom` case too).
  Variants are tried as copies with switches (x15m, x15sq) and the winner written into the plain step.
- **x18** (`gpu_vm/tick/steps/x18.hlsl`, plain, 22 October 2026; README "x18") is x17 made cheaper on real code: the fast
  loop tests the integer path first and the one-op memory body before the pairs, and step12 (paths 1 and 2) makes cc 0's
  bundles (calls, returns, far j, the add), sel and lpc, which x17 left to the general step. An ISA change of **paths
  only** (`isa.h`, `gvm_asm.h`'s `path()` and `path12_of()`; LLVM's MC layer must follow): a cc 0 bundle but eret takes
  path 1 when its ops are 1's or 7's (or none), 2 when one b/h/w load or store and lite or float lite, else 0; path 1's
  op 0 may be sel or lpc. Old binaries run as before. On the card's bench (the real programs timed by gvmbench itself:
  `gpu_vm/tick/real`, cases coremark, coremark_pie, gzip, gunzip): CoreMark 173 -> 150 ns a bundle, PIE CoreMark 188 ->
  155, gunzip -3%, gzip as before; the seven loops 2.24-2.72x lean (loads the least). Target 138 not reached. What
  failed: anything added to the fast loop's control path (cc 0 there: -22 to -40% on every loop), sel or lpc in the fast
  loop, write cache organisations that fill better on scattered stores (four ways, a skewed second way: the loops -7 to
  -22%). Every variant is checked on WARP before the card; every loop of the tick is capped (free). gvm_gpu still
  defaults to x17; x18 passes its 19 tests with `--check` on WARP and on the card.
- **x17** (`gpu_vm/tick/steps/x17.hlsl`, 800 lines, plain) is the tick that runs everything: x16's fast loop, the general
  step (every op, any address, d and q, atomics), traps, eret, the special registers, interrupts, wait, sync, the
  operations' queue (8 in flight a core) and the commit's rounds (stage, write, progress), many cores, x16's full step
  for paths 1 and 2. Its layout, cbuffers and draws: `gpu_vm/tick/STATE.md` (the GPU harness's contract). Every bench
  case is the reference's on WARP and on the card, the new `memx`, `irq` and `opq` among them. 2.55 times the lean
  tick on the seven loops (x16 2.63 in the same batch; the least loads 2.22); floats 7.5 (x16 8.1). The write cache is
  514 entries since the boot (passes ended on a full cache: init's prompt 2.10 s to 1.71 s). README "x17".
- x17 changed the ISA twice (`isa.h`, `gvm_ref.h`, gvm_cpu's tests pass): a b, h or w access stays in its address's word
  (a crossing test cost the memory loops 6-9%; the compiler splits one it does not know to be in a word), and the paths
  are renumbered (memory 3-6, integers 7: with an operation in flight the fast loop takes path 7 alone, for nothing).
  A load or store the fast loop does not make (a fault, the boot area, the operations region) is noted and the pass is
  made again in the next with memory in the general step. Read the operations' counts with ld.d (a plain lw there
  makes its pass again).
- Next for the tick: the boot's bundles are 10% path 0 (mostly cc 0 control words: calls, returns, the control word's
  add), made op by op by the general step; a faster way for them (a step for whole cc 0 bundles, or the fast loop
  taking calls and returns) is the next lever on real code. `sincos` and a 64-bit mul as ops of two results.
- Candidate ops the user raised (22 October 2026), to decide by measurement, not added yet:
  - 64-bit integers on register pairs (add, sub, compare, the product's two halves): the kernel's u64 (time, loff_t,
    do_div). Measure libgcc's `__muldi3`, `__udivdi3` and the like under `RVC_PROF` / `tools/prof.py` first.
  - Doubles as full-step ops (dadd, dmul, dfma are correctly rounded on D3D11, so exact against the reference;
    division optional in 11.1): only if `__adddf3`, `__muldf3` and the like are a real share (printf, strtod, math.h,
    old game code). Removing doubles at the source stays the first choice.
  - SIMD: v4f32 on register quads (add, sub, mul, mad, min, max, dot4; `ld.q`/`st.q` are one texel already), and
    packed u8x4 / i16x2 add and sub with saturation and average (audio, blending, libmedia). An op's cost is mostly
    fixed, so four lanes cost about one op. LLVM's loop vectorizer uses v4f32 well, SLP float3 poorly; our own hot
    code can use clang's vector types. After the LLVM target exists.
- The encoding is x15's (`gpu_vm/isa.h`): x1-x14 no longer compile. `bin\gvmbench.exe` must be rebuilt
  (`cmd /c gpu_vm\tick\build.bat`) after any change to isa.h, the reference, the assembler or cases.h; a WARP check of a
  step against a bench built for another encoding hangs until each case gives up (60 s).
- `precise` does nothing under fxc2 (byte-identical DXBC; vkd3d drops it); kept for DXC (gvmtrace12) and FXC.
  Float mad, msb and nmsb round the product first (D3D11 promises no fused float FMA): every card gives the same bits.

## The toolchain and Linux (goal set 22 October 2026: "the LLVM backend working, Linux compiled with it, bare-metal test apps")

Phases, each one agent's (one writer of the LLVM tree in WSL at a time):
1. `gpu_vm/cpu/gvm_cpu.c`: the reference machine as a runner of ELF files, with bare-metal semihosting (a syscall with
   TVEC 0 is the host's: r12 the number, Linux's: 64 write, 63 read, 93 exit; r5-r10 arguments, r5 the result).
2. The LLVM target GPUVM, MC layer (`gpu_vm/llvm/Target/GPUVM`, patches in `gpu_vm/llvm/patches`, syntax in
   `gpu_vm/llvm/ASM.md`): bundling in the streamer by `gvm_asm.h`'s rules, ELF, asm parser, disassembler, lld.
3. Codegen and clang (`gpuvm-unknown-elf`): ISel, calling convention, frame lowering, branch relaxation.
4. compiler-rt builtins, a small bare-metal libc and crt0; test apps (hello, isaloops, CoreMark) on gvm_cpu.
5. Linux 5.17 `arch/gpuvm` (no MMU), `make ARCH=gpuvm LLVM=1`; then boot on gvm_cpu.
- **All programs, the devices and the games** (the user, 22 October 2026): every user program and game compiled for
  gpu_vm (with the card's special functions and fused multiply-add), the drivers working (GPU, display, input, sound,
  ROM, LAN where feasible), and a game comparison against the RISC-V machine. Workers exactly as the RISC-V machine
  has them (docs/machine/multicore.md, lanes.md): up to 255 worker cores, the mailboxes, lent cores, /dev/gpu's
  ioctls incl. REST, the mcw/mcwx API unchanged, and gvm_cpu running workers (with a race check like RVC_RACES).
  No lanes (the user): programs take their plain-call path, as on rvc_cpu. Agents: platform (harness gpu_vm mode, devices, drivers, workers), apps, math (libm on the card's
  functions), ISA and latency (x19).
- **Denser code** (22 October 2026, gpu_vm/llvm: GlobalFill, BundleModel, ZeroExt, LoadHoist, UnrollHint, inlining
  x3, PIE globals): CoreMark 149.3k -> 113.0k bundles an iteration (2.39 RISC-V instructions a bundle), gzip 19.38M ->
  15.25M, busybox's relocations 16,973 -> 758. Fair comparisons give the RISC-V build its best flags too (inlining and
  unrolling cut the work itself). ABI: the last 64 bytes of RAM stay unused (the fill's speculative loads reach 16
  bytes past an access).
- **Userland on the card** (22 October 2026): busybox 1.37 (hush, 286 applets), init, a root file system as the
  initramfs, Lua, QuickJS, minigzip, CoreMark, gvm-tests (`gpu_vm/userland/README.md`); `systest.sh cpu|gpu` types a
  46-command session and checks it: PASS on gvm_cpu and on the card, lockstep with the reference clean over whole
  sessions. Rebuilt with the compiler of `c837089` (PIE: busybox 758 relocations) and tick x18: 5.9 s to the prompt,
  0.16-0.25 s a command, the session 28 s and 85 M bundles (49 s and 156 M before); `--check` clean. The root on the
  ROM (`romroot.sh`: romfs through MTD's phram, as the lean machine's) gives 4.0 s to the prompt.
- **On real work gpu_vm is now 2.0-2.1x the lean RISC-V tick on CoreMark and gzip, 1.6x on gunzip**
  (`gpu_vm/bench_real/README.md`, the evening of 22 October 2026: compiler `c837089`, tick x18, alternating rounds on
  the card, the RISC-V side also at its best flags from the same clang: CoreMark 56.9 against 26.9 iterations/s (23.8
  at -O2), gzip 3.30 against 6.65 s, gunzip 0.458 against 0.742 s). A CoreMark bundle costs what a lean instruction
  does (155 ns) and carries 2.4 of them; gzip's and gunzip's cost 206-210 ns because their passes end on a full write
  cache (97% and 70%, after 2,200-3,400 bundles). **The next lever is the write cache under scattered stores** (gzip
  2.7x and gunzip 2.2x at CoreMark's cost a bundle); the boot (87% of passes full) and the system test (72%) are bound
  by it too. The kernel's boot to init is now as long as the lean machine's (1.56 s from the first line, root on the
  ROM on both); what follows (busybox init and rcS, each exec loading the 2 MB busybox whole) is 2 s more.
- **Linux boots on the card** (22 October 2026): `gvm_gpu` (`gpu_vm/gpu`) with tick x17 runs vmlinux to init's line in
  2.1 s of card time (6.2M bundles, 15,148 passes, 2.96M bundles/s), lockstep with the reference clean (`--check`);
  gvm_cpu's tests, the 56 exec tests and CoreMark (5.7M bundles/s) run on the card. 15,110 of the boot's passes end on
  a full write cache (~414 bundles a pass): the next speed-up is there and in the kernel's operation threshold.
- **The kernel and userland on the GPU** (goal set 22 October 2026): tick x17 makes the whole ISA (general step,
  traps, SRs, interrupts, wait/sync, the operations), then a GPU harness boots Linux on the card. A hard requirement
  (the user): the tick stays 2-3x the lean RISC-V tick's effective work a second: every one of the seven loops at 2x
  or more on the bench, and on real work: CoreMark's iterations a second and a Linux boot on both machines on the card.
- **State at the end of 22 October 2026: all five phases work.** gvm_cpu (`gpu_vm/cpu`, 19 tests); the LLVM target
  GPUVM (`gpu_vm/llvm/README.md`): MC layer, codegen and clang (`gpuvm-unknown-elf`, `gpuvm-unknown-linux`); exec
  tests 56/56, GCC c-torture 96.5% (-O0) and 96.2% (-O2), 99.7% of what the host passes; zlib and Lua as the host's;
  CoreMark 149k bundles an iteration at -O2, 95.6% of bundles on the fast loop's paths. Linux 5.17.11 `arch/gpuvm`
  (`gpu_vm/linux/README.md`): vmlinux built by the gpuvm clang and lld, booted on gvm_cpu to a user-mode `/init` (a
  flat binary in the initramfs) that echoes the console. Next: musl and busybox for gpuvm, an elf2flt with
  relocations, the ROM disk with a real image, the shaderemu drivers, workers, SMP; then the tick runs it all.
- The user (22 October 2026): devices stay at the start of the address space below RAM (0x0001_0000-0x01ff_ffff), the
  operations region high (0xf000_0000 up). The compiler knows the commit's copy and fill: a constant-size memcpy,
  memmove or memset from a threshold up (a few KB: an operation ends the pass) is the operation inline (the descriptor
  texel by st.q to 0xf000_0000, then sync), a small one 16-byte ld.q/st.q, an unknown size libc's (which chooses);
  builtins for code that wants the hardware's (the kernel's ROM reads, page copies and clears).
- An idea of the user's for later (22 October 2026): a micro-VM on the commit's path. The operations' stage draw
  (RAM into a small stage texture, then a write draw back: no ping-pong RAM) could run a short program on each texel
  instead of copying it: an operation "map" {4, dst, src, bytes} + {program, count, constants}, a few dozen micro-ops
  (word, packed u8x4 / i16x2 with saturation, float4, shuffles, reads at fixed texel offsets), the same for every
  texel (no divergence), bounded by the stage texture a pass. For format conversion, blending, audio mixing, byte
  swaps, filters, patterned fills; not reductions. A library call / builtin (`gvm_map`), programs from a tiny
  assembler or the in-machine HLSL compiler; measured against lanes.

## Numbers (RTX 5070 Laptop, fxc2, M RISC-V instructions of work a second)

| | arith | loads | stores | adds | tight | mix | stride | floats | fmix |
|---|---|---|---|---|---|---|---|---|---|
| lean tick | 8.26 | 5.70 | 4.91 | 8.18 | 7.28 | 7.22 | 6.37 | 6.11 | 5.31 |
| s21 (prototype) | 20.6 | 12.1 | 9.8 | 19.0 | 19.1 | 16.8 | 16.7 | | |
| x6 (integers only) | 16.9 | | | 15.9 | 16.0 | | | | |
| x12 best (1.41x) | 13.7 | 6.05 | 5.22 | 12.9 | 12.9 | 11.3 | 10.2 | 7.87 | 7.04 |

The card drifts ~5% between sessions: compare within one run (batches do).

## What is known about the cost

- A bundle costs ~0.7 ns a DXBC instruction; instruction count and dependency chains are the cost.
- Nsight on x12 `loads`: waiting on ALU dependency 40.5%, issuing 35.2%, texture/local memory 9.5%,
  no instruction 8.5%, branch 5.4%. 12 of 32 lanes active a warp instruction.
- A path branch around the integer unit costs ~20 ns a bundle; code present but not run still costs.
- Loads are fastest as s21 does them: both ops' texels and write-cache entries read at once, chosen by selects.
- What didn't help: breaks vs stop flags, late register write-back, array sizes, one-trip-loop fences,
  a switch on the path, loads apart from stores (inside one loop), per-count bodies.

## Not started

LLVM gpu_vm target; `gvm_cpu` (reference machine as an emulator for Linux); compiler-rt, musl arch/gpuvm, bFLT;
Linux arch/gpuvm; workers;
integration into the harness/Unity (RAM row 64, device window moved to 0x10000, mailboxes 0x00c00000).

## x19 (22 October 2026, the ISA and latency agent): no new default

x18 stays the tick; no ISA change. `gpu_vm/tick/steps/README.md`, "x19", has every variant and batch: the bundle's ops
cost nothing over its fixed work (105 ns with 0, 1 or 2 integer ops); a texel cache, prefetching the next bundle,
hoisting the data fetch, the branch after the ops (+7.5% a bundle: the LLVM agent's proposal (1) does not pay), one lane
a core, a core on several rows, an overflow list, predicated operation rounds: none faster. The write cache's 2-way sets
end a pass after about 80 scattered words (a shell loop's and gzip's passes about 1,000 bundles); 1,026 entries double
that and make a shell loop in `rvc_harness --gvm` 15-25% faster, but cost many cores (nxray with 255 workers 165 -> 190-207
ms a picture, more state pixels and commit points a core): `steps/x19w1026.hlsl` is a drop-in step for a harness that
wants it (STATE.md holds with WCN 1026, 70 state pixels). For the harness: more passes a frame (`--tick-rounds`) is the
lever on early-ending passes; one operation round a pass instead of two saves about 6 us a pass in the bench.
