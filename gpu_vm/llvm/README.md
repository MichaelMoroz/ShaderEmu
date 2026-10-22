# gpu_vm's LLVM target (GPUVM)

LLVM 23.1.4 with an experimental target `GPUVM` (triple `gpuvm`, ELF `EM_GVM` 0x4756) for gpu_vm (`gpu_vm/isa.h`,
`gpu_vm/GPU_VM.md`): the MC layer (assembler with bundle packing, object files, disassembler, lld with range thunks),
the code generator, and clang's `gpuvm-unknown-elf` (bare metal, gvm_cpu's semihosting) and `gpuvm-unknown-linux`
(the kernel). The assembly syntax is `ASM.md`.

## Compiling and running a C program

    B=~/gpu_vm/llvm-build/bin
    $B/clang --target=gpuvm-unknown-elf -O2 -c prog.c -o prog.o
    $B/ld.lld -e _start -o prog.elf crt0.o prog.o libc.a libclang_rt.builtins.a
    build/gvm_cpu/gvm_cpu prog.elf                 # make -C gpu_vm/cpu CC=gcc

or in one step `clang --target=gpuvm-unknown-elf -O2 prog.c crt0.o ... -nostdlib -o prog.elf` (the bare-metal
driver links with ld.lld, `-m elf32gpuvm`, image base 0x02000000). There is no libc or compiler-rt for gpu_vm yet:
the tests use `test/exec/rt` (below): `crt0.s` (call main, exit with its result), `libc.c` with headers in
`rt/include` (stdio to gvm_cpu's stdout, string, stdlib with a bump allocator, ctype, a few of math; over
gvm_cpu's semihosted syscalls: r12 the number, r5-r10 the arguments) and compiler-rt's generic builtins compiled
from `~/gpu_vm/llvm-project/compiler-rt/lib/builtins` (i64 division, double in software). `test/exec/run.sh` builds
them into `build/gvm_exec/{crt0.o,rt/librt.a}`.

The Linux kernel: `bash gpu_vm/linux/build.sh` (`make ARCH=gpuvm LLVM=1`, triple `gpuvm-unknown-linux`).

## Build (WSL)

    JOBS=12 bash gpu_vm/llvm/build.sh                      # clang, lld, llc, llvm-mc, llvm-objdump ...
    bash gpu_vm/llvm/build.sh llc clang                     # some targets

The source is `~/gpu_vm/llvm-project` (llvmorg-23.1.4), the build `~/gpu_vm/llvm-build` (Release + assertions, shared
libraries, X86 RISCV and GPUVM). `build.sh` links `Target/GPUVM` into `llvm/lib/Target/GPUVM`, `test/MC/GPUVM` and
`test/CodeGen/GPUVM` into `llvm/test/`, and applies `patches/*.patch` once each (an applied one is skipped):

| patch | |
|---|---|
| `0001-triple-gpuvm.patch` | `Triple::gpuvm` (32-bit, little-endian, ELF) |
| `0002-elf-em-gvm.patch` | `ELF::EM_GVM`, `ELFRelocs/GPUVM.def` (R_GVM_*, isa.h's numbers), objdump's arch, readelf's name, yaml |
| `0003-lld-gpuvm.patch` | `lld/ELF/Arch/GPUVM.cpp`: the relocations, TLS variant 1, no PLT or GOT, image base 0x02000000; `Thunks.cpp`: a range thunk for R_GVM_J18 beyond +-2 MB (one bundle: `addi t26, zero, dest; jr t26`) |
| `0004-gpuvm-intrinsics-libcalls-datalayout-relocs.patch` | `llvm.gpuvm.copy`/`llvm.gpuvm.fill` (`IntrinsicsGPUVM.td`), gpuvm's runtime libcalls (`RuntimeLibcalls.td`: compiler-rt's and libm's), the data layout `e-m:e-p:32:32-i64:64-n32-S128` (`TargetDataLayout.cpp`), R_GVM_32/REL32 for llvm-objdump and llvm-dwarfdump (`RelocationResolver.cpp`) |
| `0005-clang-gpuvm.patch` | clang: `Basic/Targets/GPUVM.*` (TargetInfo), `CodeGen/Targets/GPUVM.cpp` (the ABI), `BuiltinsGPUVM.td`, the bare-metal driver for `gpuvm-unknown-elf` (ld.lld, no math errno), frame pointer omitted when optimizing, float denormals preserve-sign |

A change to a patched LLVM file: edit it in `~/gpu_vm/llvm-project` and write the patch again (0004 and 0005:
`bash gpu_vm/llvm/patches/regen.sh`; the others `git diff -- PATHS > ...`, new files need `git add -N` first).
0001, 0002 and 0004 touch headers most of LLVM includes (`Triple.h`, `ELF.h`, `Intrinsics.td`, `RuntimeLibcalls.td`):
applying them to a clean tree rebuilds most of LLVM (an hour on 12 jobs); 0005's `TargetBuiltins.h` most of clang.

## Test

    L=~/gpu_vm/llvm-build/bin/llvm-lit
    $L -v ~/gpu_vm/llvm-build/test/MC/GPUVM ~/gpu_vm/llvm-build/test/CodeGen/GPUVM   # MC and llc tests
    bash gpu_vm/llvm/test/e2e/run.sh            # assembler + lld + gvm_cpu
    bash gpu_vm/llvm/test/exec/run.sh           # C programs at -O0 -O1 -O2 -Os on gvm_cpu (52 runs)
    bash gpu_vm/llvm/test/torture/run.sh        # GCC's c-torture execute tests, -O0 and -O2 (~10 min, 12 jobs)
    bash gpu_vm/llvm/test/bench/run.sh          # CoreMark: bundles an iteration

- `test/MC/GPUVM`: `encoding.s` (every op alone in a bundle, its bytes as `gvm_asm.h` makes them; written by
  `test/gen/gen_encoding.cpp`), `bundle.s` (the packing rules, path bits, `;;` and `.endbundle`), `reloc.s`,
  `far-branch.s`, `errors.s`, `operands.s`.
- `test/CodeGen/GPUVM`: llc's output for `alu.ll` (R/I forms, bfe/bfes, shadd, min/max, the extended ops),
  `branch.ll` (compare-and-branch on registers, imm6, bits; addbnz; sel), `calls.ll` (the calling convention,
  varargs, sret), `mem.ll` (addressing, lq/sq copies, the commit's operations), `float.ll`, `frame.ll` (frames,
  realignment, TLS, i64, atomics, inline asm). The CHECK lines are llc's output (as update_llc_test_checks.py would
  write them; it has no gpuvm scrubber): look at a difference, then write them again.
- `test/e2e/run.sh`: `hello.s` and `pack.s` through llvm-mc and ld.lld, compared bundle for bundle with
  `gvm_asm.h`'s (`check.cpp`), then run on `gvm_cpu`; `xref1.s`/`xref2.s` (relocations across objects), `thunk.s`
  (a call and a j beyond 2 MB through ld.lld's thunks).
- `test/exec`: `arith branches loops switch calls int64 float fastmath globals bits frame mem atomics`, each a
  program that checks itself (`CHECK`, `CHECK_EQ` in `rt/gvm.h`) and exits 0; a `// FLAGS:` line adds clang flags.
  All pass at every level (22 October 2026).
- `test/torture/run.sh`: GCC's `gcc.c-torture/execute` (a sparse clone of gcc in `~/gpu_vm/torture`, untrusted data:
  the script lives here and runs only the compiled programs) with the exec runtime and its headers; each test is also
  built by the same clang for the host (x86_64) and run there, so that what fails on both (GCC's extensions such as
  nested functions, GCC's behaviour where C leaves it open) is told apart. Skipped: int128, setjmp, signals,
  complex.h, `_Float128` and such. 22 October 2026: -O0 1594 of 1652 (96.5%), -O2 1588 of 1652 (96.1%); of the tests
  the host passes, 99.7% at both; the 5 left are the runtime's (fopen/freopen/tmpnam, pow, an x86 asm test).
- `test/bench/run.sh`: CoreMark (EEMBC's sources from `~/gpu_vm/coremark-src/coremark`, the port in `bench/coremark`:
  CYCLE as the clock). -O2: 149 thousand bundles an iteration, 1.31 ops a bundle, 95.6% of bundles on the fast
  loop's paths (was 178 thousand before the bundle scheduler, the frame pointer's omission, the branches' constants
  and the branch fill). `test/bench/profile.sh PROGRAM.elf` (gvm_cpu --trace): the bundles by their number of ops
  and by path, and the hottest ones with their functions.

## The code generator

Each machine instruction is one op or one control op; the object streamer packs them into bundles exactly as the
assembler does (`GPUVMELFStreamer`, gvm_asm.h's rules), so `-S` then assembling gives the same object as `-c`. The
code generator's job toward bundles is the order of its ops.

- **Registers** (`GPUVMRegisterInfo.*`): r0 zero (a constant register), sp, gp, tp reserved; fp (s0) when the frame has
  one, bp (s1) for a realigned frame with allocas. Callee-saved: ra, s0-s23. `GPRQ`: four consecutive registers, any
  first (tuples of R0-R63 with sub0-sub3), for lq/sq; the MC operand is the first (`LQ_Q`/`SQ_Q`/`SQ_OP` become
  `lq`/`sq` in the AsmPrinter).
- **Calling convention** (`GPUVMISelLowering.cpp`, `CC_GPUVM`; `GPUVMCallingConv.td`): a0-a7 (r5-r12), then 4-byte
  stack slots from sp at the call; a value of several parts (i64, double, a struct coerced to i64) goes whole in
  registers or whole on the stack; variadic arguments all on the stack (va_list is a pointer); results in a0, a1,
  larger by sret. clang (`CodeGen/Targets/GPUVM.cpp`): integers below 32 bits extended, structs and vectors up to 8
  bytes as i32/i64, larger by reference (the caller's copy), va_arg in 4-byte slots.
- **Frames** (`GPUVMFrameLowering.cpp`): `addi sp, sp, -N` once (any N: an immediate is 32 bits), the callee-saved
  registers at the frame's top, `addi fp, sp, N` when there is an fp (allocas, realignment, -fno-omit-frame-pointer),
  `andi sp, sp, -align` for over-aligned locals; shrink wrapping. No CFI is emitted for the frame (`.cfi_startproc`
  only): no unwinding.
- **The fast loop's ops**: the tick runs a bundle in its fast loop when op 0 is a mid integer op (functions 0-19)
  or a load or store of a word or less and op 1 lite (0-7 and the shifts) or such a memory op (`isa.h`, the paths);
  shadd, bfe, sel, the carries, ror/rol/seq/sne and the extended class go to the full or general step. So a field is
  `srli; andi`, a sign extension `slli; srai`, a scaled add `slli; add`, x == 0 `sltui x, 1` (two ops that share a
  fast bundle rather than one op of the general step: CoreMark has the same bundles, 4.3% of them general against
  9.8%); `-gpuvm-general-ops` makes bfe, bfes and shadd instead.
- **Selection** (`GPUVMPatterns.td`, `GPUVMISelDAGToDAG.cpp`): every constant and symbol is one op (`li`/`la`: addi
  from zero; any 32-bit immediate); an address folds into a load's or store's immediate (`lw a0, sym+8(t0)`), and
  base + (index << s) is `lwx`/`swx`...; compares (`slt`... with any immediate; a > k as a >= k + 1), `sel`,
  compare-and-branch in the control word (registers; imm6 forms; `bbs`/`bbc` for a bit), `andn`/`orn`/`xnor`,
  min/max, abs, mul/mulh/mulhu,
  div/rem (the ISA's; division by zero is UB in C), clz/ctz/popc/bswap/brev, rotates. i64: `add`+`carry`+`addcy`,
  `sub`+`borrow`+`subby`, products by mulhu, shifts inline, division by compiler-rt (`__divdi3`...).
- **float** in the integer registers on the card's units: fadd fsub fmul fdiv fmin fmax fsqrt (always: the doc's
  rule), floor ceil trunc round (to even, for rint/nearbyint/roundeven), copysign, neg, abs, compares (unordered ones
  by inversion), conversions with i32/u32 (ftoi/ftou saturate), I forms with the constant's bits; `fmad`/`fmsb`/
  `fnmsb` for an fmul and an fadd (the ops round the product, as separate ops would: no contraction needed);
  llvm.fma is libm's `fmaf`. Under `-ffast-math` (afn): fsin fcos fexp2 flog2, exp/exp10/log/log10 through them,
  frcp/frsqrt for 1/x and 1/sqrt x; otherwise libm. Denormals flush (clang marks float preserve-sign). **double** is
  software (compiler-rt's `__adddf3`...).
- **Memory intrinsics** (`GPUVMSelectionDAGInfo.cpp`): memcpy/memmove/memset of a known size from 4096 bytes up
  (`-gpuvm-op-threshold`) are the commit's operation: the descriptor {1 copy (memmove) or 2 fill (the byte four
  times), dst, src or word, bytes} built in a register quad and stored by one `sq` to 0xf0000000 (`SQ_OP`, which keeps
  its place among other accesses); no sync: the core goes on and an access waits only where the operation is still at
  work (GPU_VM.md, "Operations"). 16-aligned copies and fills of up to 8 texels (`-gpuvm-inline-texels`) are lq/sq;
  other small ones words (MaxStoresPerMemcpy 8); the rest and unknown sizes call memcpy/memmove/memset.
  `__builtin_gpuvm_copy(dst, src, n)` and `__builtin_gpuvm_fill(dst, word, n)` (llvm.gpuvm.copy/fill) always make the
  operation.
- **Atomics**: words by the amo ops (`amoadd`... `amocas` for cmpxchg), bytes and halves by AtomicExpand's word
  cmpxchg loops, nand by a loop; i64 atomics by `__atomic_*` libcalls; `fence` for fences; a word's load or store is
  atomic. Lock-free up to 4 bytes (`__GCC_HAVE_SYNC_COMPARE_AND_SWAP_1/2/4`).
- **Globals and TLS**: absolute addresses (static links; bFLT relocates them); TLS is local exec whatever the model:
  `lw a0, %tprel(x)(tp)`. Jump tables in .rodata, `lw` and `jr`. Computed goto works.
- **Inline asm**: `r` (`%0` prints `rN`), `i`, `n`, `m` (`off(rN)`), `{a0}`/`{r5}`/`{zero}` register constraints,
  `register int x asm("tp")` for reserved registers, the `z` modifier.
- **Ordering for bundles** (`GPUVMBundleScheduler.cpp`): the post-RA machine scheduler's strategy simulates the open
  bundle: with one op in it, a ready op that fits as op 1; starting one, an op another can follow, a non-lite one
  first; the writers of the registers the block's branch compares go early (a branch whose bundle writes one needs a
  bundle of its own). `-gpuvm-bundle-sched=0` turns it off.
- **Counted loops** (`GPUVMCountedLoops.cpp`): `addi r, r, k; bnei r, 0, L` (k in imm6) becomes `addbnz r, k, L`
  (also `beqi` with a `j`) when L is within 200 instructions (`-gpuvm-addbnz-reach`; addbnz has no inverse for branch
  relaxation).
- **Constants of branches** (`GPUVMHoistConstants.cpp`): a compare-and-branch's constant beyond imm6 is made once in
  the entry block (`li` is rematerializable, so the register allocator makes it again where registers are short).
- **Branch relaxation**: a conditional branch reaches +-256 bundles; LLVM's BranchRelaxation with 16 bytes an
  instruction (the streamer never makes more than a bundle of one) branches over a `j` (+-2 MB) when farther. A
  function beyond 2 MB is an error.
- **Calls**: `call sym` (R_GVM_J18, +-2 MB; ld.lld's thunks beyond, through t26, which is caller-saved), `callr` for
  indirect ones, `jr ra` to return. No tail calls.
- **Debug info**: `-g` gives the same code: the code generator's labels (debug info's, CFI's, a function's end) are
  placed after the open bundle instead of closing it (`setSoftLabels`); a block that is branched to closes it; a
  line entry is a bundle's (the last `.loc` in it).

## The target's files (`Target/GPUVM`)

- MC layer: `GPUVMRegisterInfo.td`, `GPUVMInstrFormats.td`, `GPUVMInstrInfo.td` (one instruction per op of isa.h and
  per control op; TSFlags carry the opcode byte and the operand `Form`, the code emitter and disassembler do the
  bits; an I form's `imm32` takes any value or symbol, short or long), `MCTargetDesc/` (`GPUVMBaseInfo.h`: isa.h's
  fields and gvm_asm.h's packing rules; `GPUVMMCCodeEmitter`: a bundle is an MCInst of `GPUVM::BUNDLE` with the ops
  as `MCOperand::createInst`; `GPUVMELFStreamer`: the packing; `GPUVMAsmBackend`, `GPUVMInstPrinter`),
  `AsmParser/`, `Disassembler/`.
- Code generator: `GPUVM.td`, `GPUVMPatterns.td` (target nodes, pseudos, patterns), `GPUVMCallingConv.td`,
  `GPUVMTargetMachine`, `GPUVMSubtarget`, `GPUVMISelLowering`, `GPUVMISelDAGToDAG`, `GPUVMSelectionDAGInfo`,
  `GPUVMInstrInfo` (copies, spills incl. quads, branch analysis over the control word's branches), `GPUVMRegisterInfo`,
  `GPUVMFrameLowering`, `GPUVMMachineFunctionInfo.h`, `GPUVMAsmPrinter` (MCInst lowering, inline asm operands),
  `GPUVMBundleScheduler`, `GPUVMCountedLoops`, `GPUVMHoistConstants`.

## Linux

`bash gpu_vm/linux/build.sh` (sync config image) with this compiler (22 October 2026): every C and .S file of the
gpuvm_defconfig kernel compiles (warnings only: clang's `-Wattribute-alias` on the syscall wrappers, as on other
arches); the link of vmlinux fails on the port's side: `arch/gpuvm/lib/Makefile` has `obj-y` where `libs-y` in
`arch/gpuvm/Makefile` wants `lib-y` (no `lib.a`); with that changed, `calibrate_delay` and `lpj_fine` (no
`GENERIC_CALIBRATE_DELAY`) and `show_unhandled_signals` (traps.c) are undefined.

## Known gaps

- No libc, compiler-rt or crt for gpu_vm yet (the tests' runtime stands in); no C++ runtime.
- No unwind information (CFI) for frames, so no C++ exceptions and no unwinding; `-g` line tables and variables work
  (a line entry is a bundle's).
- No tail calls; no `bump` (the control word's add) and no `ld`/`sd` pairs from the code generator; i64 loads and
  stores are two words.
- double is software only; `llvm.fma` is a libcall (fmad rounds the product).
- TLS is local exec only (static links); no PIC (`-fPIC` makes the same absolute code).
- A branch to a block more than +-2 MB away inside one function is an error (no indirect branch relaxation).
- Branch-only bundles: a branch on a register written in its own bundle (a value just loaded or computed) takes a
  bundle of its own; ops are not moved across blocks to fill them.
- Branch relaxation in the assembler: a conditional branch beyond +-4 KB in hand-written assembly is an error;
  R_GVM_BR9 has no thunks in lld (a branch to another section must be within 4 KB).
- `R_GVM_IMM12` is in lld and the writer but the assembler makes no fixup of it (every symbol is a long immediate).
- No `%pcrel`/GOT/PLT forms: static links only (bFLT later).
- A disassembly prints targets as addresses: it is for reading, not for assembling again. `llvm-objdump -d` skips
  runs of zero bytes (`...`) and then decodes off the bundle grid: use `-z`.
- `-S` with `-g`: the printed labels close bundles when assembled again, so that object can differ from `-c`'s.
