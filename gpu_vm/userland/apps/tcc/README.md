# TinyCC for gpu_vm

A gpu_vm back end for TinyCC (jrrk2's riscv32 branch, which `linux/tcc/build.sh` builds for the RISC-V machine).
It is meant to give `cc`, `tcc`, the `cc-bytes` case and nxide's C build on gpu_vm. That work is not finished: so far
there is a cross compiler that runs on the host, and nothing in `build.sh` builds it.

## The files

| | |
|---|---|
| `gpuvm-gen.c` | The code generator. It writes one op per 16-byte bundle, so every address TinyCC keeps is the start of a bundle. Values live in the 64 registers: TinyCC allocates a0-a7 and t0-t7, and keeps t8-t10 as scratch. A float is native single precision; a double is a compiler-rt call. The calling convention is clang's CC_GPUVM. A conditional branch is the inverse branch over a far `j` (cc 0, off18), and a jump chain keeps its next link in word 2. The bundle's path follows `GPUVMBaseInfo.h`'s `bundle::path` with fx1 and late branches off. |
| `gpuvm-link.c` | lld's R_GVM_* relocations. A program is linked at 0 as a library (`tcc -shared`), and each absolute word (R_GVM_32, R_GVM_IMM32) becomes R_GVM_RELATIVE. There is no GOT and no PLT. |
| `gpuvm-asm.c` | GCC's inline asm for what the headers use (`wait`, `sync`, `mfsr rd, cycle\|time_lo`), plus `mtsr`, `mv`, `li`, `nop`, `break`, `fence`, `syscall` and the integer ops (`add rd, ra, rb\|imm` ...). Operands are given a0-a7. |
| `gpuvm_hook.py` | Puts the three files into a tinycc tree and makes `TCC_TARGET_GPUVM` a variant of `TCC_TARGET_RISCV32`. It edits `tcc.h`, `libtcc.c` (unwind tables off), `tcc.c`, `tccelf.c` (the dynamic relocation count, e_flags, 16-byte code alignment), `include/tccdefs.h` (va_arg: 4-byte stack slots, an aggregate over 8 bytes by pointer) and `tccgen.c` (a pointer initializer's addend goes in the RELA relocation). Run it after linux/tcc's `fpu_hook.py`, `speed_hook.py` and `pass_hook.py`. It is idempotent. |
| `elf2flt.c` | Turns the `tcc -shared` ELF into a bFLT. It is linux/nommu/elf2flt.c for EM_GVM, taking R_GVM_RELATIVE and R_GVM_32. |

## Building the cross compiler (by hand, for now)

In WSL, with `SRC` a tinycc tree (`$GVM_APPS_WORK/src/tinycc`):

    python3 linux/tcc/fpu_hook.py $SRC; python3 linux/tcc/speed_hook.py $SRC; python3 linux/tcc/pass_hook.py $SRC
    python3 gpu_vm/userland/apps/tcc/gpuvm_hook.py $SRC
    mkdir B && cd B && $SRC/configure --cpu=riscv32 --triplet=riscv32-linux-musl --config-musl \
        --sysincludepaths=/usr/include:/usr/lib/tcc/include --libpaths=/usr/lib:/usr/lib/tcc --crtprefix=/usr/lib --tccdir=/usr/lib/tcc

Then, in config.h, put `#define TCC_TARGET_GPUVM 1`, `#define CONFIG_TCC_BCHECK 0` and
`#define CONFIG_TCC_BACKTRACE 0` before `#define TCC_TARGET_RISCV32 1`, and run:

    make tcc ONE_SOURCE=yes
    make libtcc1.a ONE_SOURCE=yes RISCV32_O=builtin.o LIN_O=

The second make builds TinyCC's `__builtin_clz` and the like with the new compiler. Its `libtcc1.c` stops on `#error
unsupported CPU type`, and compiler-rt covers what that file would give.

A program is linked against gpu_vm's musl sysroot (`~/gpu_vm/sysroot`):

    tcc -nostdinc -I$SRC/include -I$S/include -shared -nostdlib $S/lib/crt1.o $S/lib/crti.o prog.c -L$S/lib -lc \
        B/libtcc1.a $S/lib/libclang_rt.builtins-gpuvm.a $S/lib/crtn.o -o prog.elf
    elf2flt prog.elf prog

The sysroot's `crt1.o` already has gpu_vm's flat start (`__gvm_flat_start`), so no flatcrt is needed.

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

## What is left

1. **A build script.** `gpu_vm/userland/apps/tcc.sh` should run the hooks, build the cross compiler and libtcc1, and be
   added to `build.sh`'s ALL. It also stages a sysroot, as linux/tcc/build.sh does:
   - The headers, without the kernel's.
   - tcc's own include folder.
   - `libtcc1.a`: compiler-rt's builtins, tcc's `builtin.o` and an `alloca.o` assembled by clang.
   - crt1.o, crti.o, crtn.o, libc.a and libm.a.
   - libnano-X.a, libgles.a (gles.c with `-DSEGL_FLOAT`), libmcw.a, libhlsl.a (from hlsl.sh's build) and libmedia.a,
     with their headers.
   - Then pack.py, which accepts `-mfpu` and `-static` here.
2. **The guest compiler.** Configure it with `--cc=rv32-cc`, use mapped.c, apply the same config.h edit, and build it
   with `ONE_SOURCE=yes`. Also build `elf2flt.c` with rv32-cc into /usr/bin.
3. **gpu_vm's `cc`.** Base it on linux/nommu/cc, using `/usr/lib/crt1.o` in place of flatcrt.o and compiler-rt in
   libtcc1.a. Then the `example` script, the examples and the nxapps entries.
4. **The cases.**
   - `cc-bytes`: set gpu_vm's md5s as an OVERRIDE in `test_all.py`, the same over two runs, on gvm_cpu and on the card.
   - The examples hello, cube, home and shader.
   - `nxide-c`, which needs nxide's debugger on gpu_vm: a `break` poked into a bundle (it stops with pc at the op,
     `epc`), stepping by bundles, and stabs, which tcc's `-g` writes (tccdbg.c's line offsets are in bytes, now 16 a
     line's op).
   - The hlsl cases that build with cc.
5. **Not done in the compiler.**
   - No bounds checking (`-b`) and no `-run`.
   - A long long asm operand is refused.
   - Code is four times RISC-V's size: one op a bundle, and every conditional branch is two bundles. Packing two ops a
     bundle and near branches are the obvious next steps.
