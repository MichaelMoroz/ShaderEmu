# Handoff: gpu_vm ISA, tick, LLVM, Linux (22 October 2026)

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
- Next: the general step, traps and SRs in the tick; `sincos` and a 64-bit mul as ops of two results; then the LLVM
  target, gvm_cpu and Linux.
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
Linux arch/gpuvm; the tick's general step, traps/interrupts/SRs, D/Q loads, atomics, the operations region, workers;
integration into the harness/Unity (RAM row 64, device window moved to 0x10000, mailboxes 0x00c00000).
