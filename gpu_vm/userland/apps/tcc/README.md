# TinyCC for gpu_vm

A gpu_vm back end for TinyCC (jrrk2's riscv32 branch, which `linux/tcc/build.sh` builds for the RISC-V machine).
It gives `cc`, `tcc`, the `cc-bytes` case and nxide's C build and debugger on gpu_vm: `gpu_vm/userland/apps/tcc.sh`,
in `build.sh`'s ALL, builds the cross compiler, its sysroot, the machine's compiler and the examples.

## The files

| | |
|---|---|
| `gpuvm-gen.c` | The code generator. It writes one op per 16-byte bundle, so every address TinyCC keeps is the start of a bundle. Values live in the 64 registers: TinyCC allocates a0-a7 and t0-t7, and keeps t8-t10 as scratch. A float is native single precision; a double is a compiler-rt call. The calling convention is clang's CC_GPUVM. A conditional branch is the inverse branch over a far `j` (cc 0, off18), and a jump chain keeps its next link in word 2. The bundle's path follows `GPUVMBaseInfo.h`'s `bundle::path` with fx1 and late branches off. |
| `gpuvm-link.c` | lld's R_GVM_* relocations. A program is linked at 0 as a library (`tcc -shared`), and each absolute word (R_GVM_32, R_GVM_IMM32) becomes R_GVM_RELATIVE. There is no GOT and no PLT. |
| `gpuvm-asm.c` | GCC's inline asm for what the headers use (`wait`, `sync`, `mfsr rd, cycle\|time_lo`), plus `mtsr`, `mv`, `li`, `nop`, `break`, `fence`, `syscall` and the integer ops (`add rd, ra, rb\|imm` ...). Operands are given a0-a7. |
| `gpuvm_hook.py` | Puts the three files into a tinycc tree and makes `TCC_TARGET_GPUVM` a variant of `TCC_TARGET_RISCV32`. It edits `tcc.h`, `libtcc.c` (unwind tables off), `tcc.c`, `tccelf.c` (the dynamic relocation count, e_flags, 16-byte code alignment), `include/tccdefs.h` (va_arg: 4-byte stack slots, an aggregate over 8 bytes by pointer) and `tccgen.c` (a pointer initializer's addend goes in the RELA relocation). Run it after linux/tcc's `fpu_hook.py`, `speed_hook.py` and `pass_hook.py`. It is idempotent. |
| `elf2flt.c` | Turns the `tcc -shared` ELF into a bFLT. It is linux/nommu/elf2flt.c for EM_GVM, taking R_GVM_RELATIVE and R_GVM_32. |

## The build (tcc.sh)

`tcc.sh` runs linux/tcc's `fpu_hook.py`, `speed_hook.py` and `pass_hook.py` and `gpuvm_hook.py` on the tinycc tree,
puts `TCC_TARGET_GPUVM 1`, `CONFIG_TCC_BCHECK 0` and `CONFIG_TCC_BACKTRACE 0` in config.h, and then:

1. The cross compiler (`$WORK/build/tcc-gvm-cross`): `make tcc`, then `libtcc1.a` with `RISCV32_O=builtin.o LIN_O=`
   (`libtcc1.c` stops on `#error unsupported CPU type`; compiler-rt covers it).
2. A sysroot as linux/tcc/build.sh stages one (`$WORK/build/tcc-gvm-sysroot`): the headers; `libtcc1.a` made of
   compiler-rt's builtins, tcc's `builtin.o` and an `alloca.o`; musl's crt1.o (gpu_vm's flat start already: no
   flatcrt, though a copy is installed under that name for nxide's link), crti.o, crtn.o, libc.a, libm.a; libnano-X,
   libgles (`-DSEGL_FLOAT`), libmcw, libhlsl and libmedia with their headers; then `pack.py`.
3. The machine's compiler (`tcc-gvm-guest`: rv32-cc, `mapped.c`, `ONE_SOURCE=yes`) and `elf2flt` into /usr/bin.
4. `cc` (linux/nommu/cc with crt1.o for flatcrt.o), `example`, the examples and their nxapps entries.

## Where it stands (23 October 2026)

- **A hello program runs.** It uses printf, malloc and strcpy, passes a struct of 5 ints and a double by value, and
  does double and float arithmetic. Linked as above, it runs on gvm_cpu under Linux (image `build/gvm_apps/img-t`)
  and prints the right values and exit code.
- **Exec tests: 10 of 15 pass.** These are `gpu_vm/llvm/test/exec/*.c` compiled by tcc with `-c`, linked by ld.lld
  with the tests' runtime and run on gvm_cpu. clang's builtins that tcc lacks were mapped to C by a header given with
  `-include`. The five that fail are not code generator bugs:
  - `atomics`: tcc has no `__ATOMIC_*`, and riscv32's tcc has no atomic library.
  - `bits`: a field of a packed struct at an unaligned offset. A b/h/w access stays in its address's word, and tcc
    reads such a field with one `lw`. This could be fixed by reading such fields a byte at a time, the way tcc already
    reads unaligned packed bitfields.
  - `frame`: `alloca`. It needs a function in libtcc1 (`sp -= (n + 15) & -16; return sp`) written in assembly.
  - `fastmath`: clang's `__builtin_sinf` and the like.
  - `fconv`: the test expects clang's native double-to-float truncation, which flushes denormals to ±0. compiler-rt's
    `__truncdfsf2`, which tcc calls, gives the exact denormal.

## In the machine (24 October 2026)

- `cc-bytes` passes on gvm_cpu and on the card: 12 md5s, digest `825f76ab`, the same with 17 and 32 cores.
- The examples hello, cube, home and shader build and run with the machine's `cc` (shader's "0 worker cores" on gvm_cpu
  is `mcw_most()`, which reads a word gvm_cpu does not fill: clang's build says the same).
- `nxide-c` passes on gvm_cpu and on the card: debug.h's stop on gpu_vm is a control word of three ops on the general
  path (illegal before any op of the bundle runs; the trap's pc is word 3, and the debugger sets it back to the
  bundle), a step decodes isa.h's control words, and a `sync` comes before the program is let run (the kernel's poke
  is a store of the pass, and the fetch reads RAM as committed: without it a stop poked was run past).

## Not done in the compiler

- No bounds checking (`-b`) and no `-run`. A long long asm operand is refused.
- Code is four times RISC-V's size: one op a bundle, and every conditional branch is two bundles. Packing two ops a
  bundle and near branches are the obvious next steps.
- The exec tests above: atomics, a packed struct's unaligned field (`bits`), `__builtin_sinf` and the like, and clang's
  native double-to-float truncation.
