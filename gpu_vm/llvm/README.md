# gpu_vm's LLVM target (GPUVM)

LLVM 23.1.4 with an experimental target `GPUVM` (triple `gpuvm`, ELF `EM_GVM` 0x4756) for gpu_vm (`gpu_vm/isa.h`,
`gpu_vm/GPU_VM.md`). The assembly syntax is `ASM.md`. So far the MC layer: assembler, bundling, object files,
disassembler, and lld; no code generator yet.

## Build (WSL)

    JOBS=12 bash gpu_vm/llvm/build.sh                      # clang, lld, llc, llvm-mc, llvm-objdump ...
    bash gpu_vm/llvm/build.sh llvm-mc llvm-objdump lld      # some targets

The source is `~/gpu_vm/llvm-project` (llvmorg-23.1.4), the build `~/gpu_vm/llvm-build` (Release + assertions, shared
libraries, X86 RISCV and GPUVM). `build.sh` links `Target/GPUVM` into `llvm/lib/Target/GPUVM` and `test/MC/GPUVM` into
`llvm/test/MC/GPUVM`, and applies `patches/*.patch` once each (an applied one is skipped):

| patch | |
|---|---|
| `0001-triple-gpuvm.patch` | `Triple::gpuvm` (32-bit, little-endian, ELF) |
| `0002-elf-em-gvm.patch` | `ELF::EM_GVM`, `ELFRelocs/GPUVM.def` (R_GVM_*, isa.h's numbers), objdump's arch, readelf's name, yaml |
| `0003-lld-gpuvm.patch` | `lld/ELF/Arch/GPUVM.cpp`: the relocations, TLS variant 1, no PLT or GOT, image base 0x02000000; `Thunks.cpp`: a range thunk for R_GVM_J18 beyond +-2 MB (one bundle: `addi t26, zero, dest; jr t26`) |

A change to a patched LLVM file: edit it in `~/gpu_vm/llvm-project` and write the patch again
(`git diff -- PATHS > gpu_vm/llvm/patches/NNNN-....patch`; new files need `git add -N` first). The patches touch
`Triple.h` and `ELF.h`: applying them to a clean tree rebuilds most of LLVM (an hour on 12 jobs).

## Test

    ~/gpu_vm/llvm-build/bin/llvm-lit -v ~/gpu_vm/llvm-build/test/MC/GPUVM     # the MC tests
    bash gpu_vm/llvm/test/e2e/run.sh                                          # end to end

- `test/MC/GPUVM/encoding.s`: every op alone in a bundle, its bytes as `gvm_asm.h` makes them; written by
  `test/gen/gen_encoding.cpp` (run it again after a change to isa.h).
- `bundle.s` (the packing rules, path bits, `;;` and `.endbundle`, the printed text assembling to the same object),
  `reloc.s` (relocations), `far-branch.s` (branch reach, near and far `j`), `errors.s` (the parser's diagnostics), `operands.s` (operand forms).
- `test/e2e/run.sh`: `hello.s` and `pack.s` through llvm-mc and ld.lld (`-Ttext=0x02000000`), compared bundle for
  bundle with `gvm_asm.h`'s (`check.cpp`), then run on `gvm_cpu` (hello prints, pack exits 68); `xref1.s`/`xref2.s`
  (relocations across objects), `thunk.s` (a call and a j beyond 2 MB through ld.lld's thunks).

## The target (`Target/GPUVM`)

- `GPUVMRegisterInfo.td`: R0-R63 (alt names the ABI's), one class `GPR` for i32 and f32.
- `GPUVMInstrFormats.td`, `GPUVMInstrInfo.td`: one instruction per op of isa.h, and per control op. The encoding is
  not TableGen's: TSFlags carry the opcode byte (`Opc`; a control op's cc | sub << 4) and the operand layout (`Form`,
  `GPUVMII::Form` in `MCTargetDesc/GPUVMBaseInfo.h`), and the code emitter and disassembler do the bits. An I form's
  immediate (`imm32`) takes any 32-bit value or symbol: the encoder makes it short (imm12) or long (word 2).
  `ld`/`lq`/`sd`/`sq` (and the x forms) name the first of 2 or 4 registers; the MC operand is that first GPR.
- `MCTargetDesc/GPUVMBaseInfo.h`: isa.h's fields, and `GPUVM::bundle::` (`path`, `fits1`, `readsRd0`, `writes`,
  `touches`): gvm_asm.h's rules on op words. The tick trusts a bundle's path bits: keep these as gvm_asm.h has them.
- `GPUVMMCCodeEmitter`: encodes a bundle, an MCInst of opcode `GPUVM::BUNDLE` (TargetOpcode::BUNDLE) whose operands
  are an immediate (-1; the disassembler's note of unexpected path bits) and up to two ops and a control op
  (`MCOperand::createInst`). A lone op or control op is encoded as a bundle of its own (`llvm-mc -show-encoding`).
- `GPUVMELFStreamer`: the bundling (ASM.md, "Bundles"). Each MCInst the parser or the codegen emits is one op or
  control op; it packs them and emits one BUNDLE an MCInst. A BUNDLE emitted from outside is passed through (closing
  the open one first): the codegen may bundle by itself. `-gpuvm-no-pack`: one op a bundle.
- `GPUVMAsmBackend`: fixups at their word of the bundle (long immediate +8, control word +12); `fixup_gvm_j` makes a
  near `j` (beq r0, r0, the ops' path) or a far one (cc 0, the general path, R_GVM_J18 when unresolved).
- `GPUVMInstPrinter`: a bundle prints on one line, `op; op; control ;;`.
- `AsmParser`, `Disassembler`: as ASM.md says.
- `GPUVMTargetMachine.cpp`: an empty `LLVMInitializeGPUVMTarget`, no TargetMachine yet.

## For the codegen (next)

- TargetMachine (data layout `e-m:e-p:32:32-i64:64-n32-S128`), Subtarget, ISel lowering, frame lowering, register
  info, instr info, AsmPrinter with MCInstLowering: emit ops and control ops as single MCInsts; the streamer packs.
  The .td has no patterns: add `def : Pat<...>` beside the instructions (operands `imm32`, `simm6`, `uimm5`, `len32`,
  `simm12` are ImmLeafs).
- Scheduling for the packing: two ops a bundle and one long immediate; op 1 lite (or float lite after float or memory,
  or a memory op after a memory op 0 that reads nothing it writes); a branch after an op that writes its compared
  register costs a bundle (the control word reads registers as the bundle found them); `addbnz` and `bump` fuse a
  counter's step. Branch relaxation: a branch reaches +-256 bundles; `j` and `call` +-2 MB.
- Wide loads and stores (`ld` `lq` `sd` `sq`, the commit's copy and fill descriptors stored by `sq` to 0xf0000000 then
  `sync`): add register pair and quad classes (consecutive registers, any first) and codegen-only defs on them, or
  pass the first register and model the rest as implicit operands.
- The Linux port asks for: inline asm with `r` constraints (`%0` prints `rN`/ABI names), `tp` (r4) and `gp` (r3)
  reserved, functions aligned to 16 (`-falign-functions=16`; a function starts a bundle anyway). t26 (r63) is
  clobbered by any `call`/`j` across sections (ld.lld's range thunks): never live across one.
- clang's target info (`clang/lib/Basic/Targets/GPUVM.*`) and driver as a patch `0004-...`; compiler-rt builtins.

## Known gaps

- Branch relaxation in the assembler: a conditional branch beyond +-4 KB is an error (`j` becomes the far form);
  R_GVM_BR9 has no thunks in lld (a branch to another section must be within 4 KB).
- `R_GVM_IMM12` is in lld and the writer but the assembler makes no fixup of it (every symbol is a long immediate).
- No `%pcrel`/GOT/PLT forms: static links only (bFLT later).
- A disassembly prints targets as addresses: it is for reading, not for assembling again.
