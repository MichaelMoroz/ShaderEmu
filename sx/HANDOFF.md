# Handoff: SX ISA, tick, LLVM, Linux (22 October 2026)

## The task (the user's words, condensed)

Starting from `sx/SX.md`: (1) download and build LLVM, then an SX target for it; (2) meanwhile design the new
tick shader with everything SX asks for (FPU, special functions, the operations region in memory, the new memory
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
- **The ISA is called gpu_vm** (the user, 22 October 2026), not SX (nor SVM, AMD's virtualization): the folder
  `gpu_vm/`, the macros `GVM_`, the LLVM target and Linux's arch `gpuvm`. The rename of `sx/`, the `SX_` names,
  `sxbench` and the docs is to be done once this round's variant agents are finished. The machine is
  not x86: the name ShaderX86 found in the tree (the GPU queue's mutex, the porting skill) means nothing for it.
- **The ISA is free to change** (the user, 22 October 2026): encodings, fields, paths, new ops (fused ones too)
  wherever that makes the tick faster. `sx/isa.h`, `sx/ref/sx_ref.h` and `sx/ref/sx_asm.h` change with it, and the
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
3. **Fused ops**: new SX ops that do two things in one slot where common pairs show up (e.g. compare-and-select,
   add-then-load / loads with a post-increment, shift-and-add already exists as `shadd`, add-and-mask as s18 had,
   load+extend, a branch's decrement). Look at what the lean tick's loops and real code pair most.
4. Then keep going: the tick to 2-3x, then the LLVM SX target, then sx_cpu and Linux.

## Where things are

Worktree `E:\Development\Emu\ShaderEmu-custom_isa`, branch `custom_isa` (tracks origin/custom_isa), clean, two
commits on top of origin: `7a8f63f` (reference, bench, steps x1-x11), `f9a2857` (floats, x12, batches, sxtrace12,
trace.ps1). Not pushed.

- `sx/SX.md` — the original design. **Out of date**: SX now has two op slots, the control word's path/long bits,
  renumbered integer and float functions, branches 9 bits, j/call 18 bits. `sx/isa.h` is the truth; update SX.md.
- `sx/isa.h` — the encoding. Control word: n (0-2 ops) bits 1:0; bit 2 word 2 is a long immediate, bit 3 it is
  op 1's; path bits 6:4 (0 general, 1 int, 2 mem+lite, 3 mem+mem, 4 float+lite, 5 float+float-lite, 6 mem+float-lite;
  "lite" = integer f 0-10, "float lite" = float f 0-7); cc 10:7; cc 0: sub 13:11, rs 19:14, off18 31:14; branches:
  ra 16:11, rb/imm6 22:17, off9 31:23. **Branches compare the registers as the bundle found them** (user's choice);
  jr/callr read after the ops.
- `sx/ref/sx_ref.h` — the reference machine (every op, traps, SRs, passes, write log, commit, copy/fill/blit).
- `sx/ref/sx_asm.h` — test assembler; packs ops two a bundle and sets path/long bits.
- `sx/tick/steps/x1..x12.hlsl` + `README.md` — every step and probe with numbers. **x12** (428 lines) is the
  current full fast-path tick; best variant `-D OK_MIN=1 -D INT_FIRST=1`.
- `sx/tick/draft_full.hlsl` — the first big tick (correct, slow); a sketch of semantics only (incl. the general
  step, traps, SRs, the operations' stage/write/progress passes).
- `sx/tick/sxbench.cpp` + `cases.h` — bench: `bin\sxbench.exe --shader F [-D ..] | --variants FILE`, `--case`,
  `--rounds N`, `--passes N`, `--nocheck`. Checks against the reference, times isaloops' loops, prints x lean.
  Build: `sx\tick\build.bat`. Run on the card through `pwsh tools\gpu_queue.ps1`.
- `sx/tick/variants/x12a-d.txt` — the batches so far.
- `sx/tick/sxtrace12.cpp` — the tick on D3D12, DXC `-Zi -Qembed_debug -O3 -HV 2016` sm 6.6 (or `--dxbc`).
- `sx/tick/trace.ps1` — Nsight GPU Trace (real-time shader profiler, Blackwell GB20x) of sxtrace12;
  exports under `logs\nsight-sx\`; the per-line SASS view is only in the Nsight UI (.ngfx-gputrace).
- `sx/llvm/build.sh` — LLVM 23.1.4 in WSL (`~/sx/llvm-project`, build `~/sx/llvm-build`): clang, lld, llc, llvm-mc
  for X86+RISCV are **built**. No SX target yet (`sx/llvm/Target/SX` gets linked in when it exists).
- Lean baseline image: main checkout's `build/images/sxlean` (isaloops built by `~/nm/work/bin/rv32-cc -O2`,
  `linux/nommu/elf2bflt.py`; script in the old scratchpad `lean.sh`). Run:
  `bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_lean --image build\images\sxlean --no-stdin --expect "laid out 255" --send "isaloops 16; isaloops 16; echo SX-D''ONE\n" --until "SX-DONE"`.

## State at the end of 22 October 2026 (the goal "2x on the most frequent code" is met)

- **x15m q4ovf** (`sx/tick/steps/x15m.hlsl -D MEM4=1 -D MOP2=1 -D EXT2=1 -D OVF=1 -D WCN=130`) is two times the lean
  tick on all seven integer and memory loops, the reference's on the card: arith 21.0, loads 12.6, stores 11.7,
  adds 22.6, tight 19.8, mix 17.6, stride 18.0 (2.21 to 2.83 times; 2.55 overall). q4w1ovf (one way) is 2.65.
  Timed with `--fused 1` (the loops end in addbnz). History and every batch: `sx/tick/steps/README.md` from "x13".
- Next: make q4ovf's switches the default of a step x16 (and the overflow slots' `cycle` fix), port round 2's
  integer inner loop (x14i I_LOOPS=2: arith +15% on x14) and the structure agent's x15s; then the rename to gpu_vm;
  then `sincos` (rd, rd + 1) and a 64-bit mul (low, high) as ops of two results (the user asked; the full step's);
  then the LLVM target, sx_cpu and Linux.
- The encoding is x15's (`sx/isa.h`): x1-x14 no longer compile. `bin\sxbench.exe` must be rebuilt
  (`cmd /c sx	ickuild.bat`) after any change to isa.h, the reference, the assembler or cases.h; a WARP check of a
  step against a bench built for another encoding hangs until each case gives up (60 s).
- `precise` does nothing under fxc2 (byte-identical DXBC; vkd3d drops it); kept for DXC (sxtrace12) and FXC.
  Float mad, msb and nmsb round the product first (D3D11 promises no fused float FMA): every card gives the same bits.

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

LLVM SX target; `sx_cpu` (reference machine as an emulator for Linux); compiler-rt, musl arch/sx, bFLT;
Linux arch/sx; the tick's general step, traps/interrupts/SRs, D/Q loads, atomics, the operations region, workers;
integration into the harness/Unity (RAM row 64, device window moved to 0x10000, mailboxes 0x00c00000).
