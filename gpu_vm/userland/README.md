# gpu_vm's userland toolchain

C programs for gpu_vm's Linux (`gpu_vm/linux`, no MMU) as bFLT flat programs: musl 1.2.5 ported to `arch/gpuvm`,
compiler-rt's builtins, a flat converter and a compiler driver, all over the gpuvm clang and lld (`gpu_vm/llvm`).

| | |
|---|---|
| `build.sh [syscalls musl builtins]` | the sysroot `~/gpu_vm/sysroot`: musl's `libc.a`, `crt1.o`, `crti.o`, `crtn.o` and headers (and the kernel's `linux/`, `asm/`), `lib/libclang_rt.builtins-gpuvm.a` |
| `musl/` | the port, laid over the release tarball by `build.sh musl`: `arch/gpuvm/**`, `src/*/gpuvm/*` |
| `flat.ld` | the link: one image from address 0x10, `__gvm_data_start` on 64 bytes |
| `elf2flt` | the linked ELF (relocations kept) to a bFLT (Python 3) |
| `bin/gpuvm-linux-cc` | the compiler driver (gcc's options) |
| `bin/gpuvm-linux-{ar,ranlib,nm,objdump,objcopy,readelf,strip,size}` | LLVM's tools under target names; `strip` copies a bFLT unchanged |
| `test.sh`, `test/` | programs run under the kernel on gvm_cpu |

## Building

In WSL, with the LLVM build (`gpu_vm/llvm/build.sh`) and the kernel's headers (`bash gpu_vm/linux/build.sh headers`):

    bash gpu_vm/userland/build.sh                  # musl and the builtins into ~/gpu_vm/sysroot (a minute)
    export PATH=$PWD/gpu_vm/userland/bin:$PATH
    gpuvm-linux-cc -O2 hello.c -o hello            # hello is a bFLT program
    gpuvm-linux-cc -O2 -c a.c && gpuvm-linux-cc a.o b.c -lm -o prog
    ./configure --host=gpuvm-linux CC=gpuvm-linux-cc AR=gpuvm-linux-ar RANLIB=gpuvm-linux-ranlib

`build.sh musl` unpacks `~/gpu_vm/dl/musl/musl-1.2.5.tar.gz` (downloaded and checked by its sha256 when missing) into a
clean `~/gpu_vm/musl`, copies `musl/` over it, adds `gpuvm*) ARCH=gpuvm` to `configure`, and builds with
`clang --target=gpuvm-unknown-linux -O2 -fno-pic`, static only. `build.sh syscalls` writes
`musl/arch/gpuvm/bits/syscall.h.in` again from the kernel's `asm/unistd.h` (the file is kept in git).

The driver: `-c`/`-S`/`-E` compile with musl's headers (`-nostdinc -isystem $SYSROOT/include -isystem` clang's own);
otherwise sources are compiled first, then `ld.lld -static -q -T flat.ld crt1.o crti.o ... -lc builtins crtn.o`, then
`elf2flt`. Its own options: `-Wl,-z,stack-size=N`, `-Wl,-elf2flt=-sN` or `GVM_STACK=N` (default 65536), `GVM_KEEP_ELF=1`
(the ELF beside as `PROG.gdb`, for llvm-objdump), `-v`. `-shared` is an error; `-static`, `-pie`, `-s` are ignored.
`-print-libgcc-file-name` gives the builtins. `SYSROOT`, `LLVM_BIN` override the paths. No C++ runtime.

## The flat format (what binfmt_flat takes)

bFLT version 4, `FLAT_FLAG_RAM` (the file read whole into one allocation: text, data, bss, then the stack), no
shared libraries, no GOT (`CONFIG_BINFMT_FLAT_NO_DATA_START_OFFSET`). The header's fields are big-endian.

- The program is linked at 0 (`flat.ld`: text from 0x10, so that no symbol is 0: binfmt_flat leaves a word of 0
  unrelocated), and its bytes follow the 64-byte header: file offset = address + 0x40. binfmt_flat relocates a word r
  to load + 0x40 + r (text's start plus r below `data_start - 0x40`, data's start plus r less that above; the same
  when data starts on 32 bytes, which `flat.ld` makes sure of). binfmt_flat (5.17) also stores the data's start in
  the word below it (its shared-library table, even with `NO_DATA_START_OFFSET`): `flat.ld` leaves that word empty
  and `elf2flt` refuses an image with a section there (it was the last word of `.rodata`, musl's `decfloat.p10s`,
  and strtod went wrong).
- Header: `entry` = 0x40 + `_start`, `data_start` = 0x40 + `__gvm_data_start`, `data_end` = the file's image end,
  `bss_end` = 0x40 + the end of `.bss`, `stack_size` (the driver's), `reloc_start` = `data_end`, `reloc_count`,
  `flags` 1 (RAM), `build_date`.
- Relocations: one big-endian word each after the image, the address of a word to relocate; the word itself is
  stored big-endian (binfmt_flat swaps it). `ld.lld -q` keeps the link's relocations; `elf2flt` makes one of every
  `R_GVM_32` (data words: pointers, jump tables, `.init_array`) and `R_GVM_IMM32` (a bundle's long immediate:
  `la`, `lw a0, sym`, a call through a constant) whose symbol is not absolute or undefined-weak, and of the long
  immediate of each of ld.lld's range thunks (`__gpuvm_thunk_*`, which carry no relocation). PC- and
  tp-relative ones (`REL32 PCIMM32 BR9 J18 TPREL32`) need nothing. An `R_GVM_IMM12` to an address (never made by
  the assembler) is an error, as is a GOT, a PLT or a dynamic section. A hello is 45 KB with about 190 relocations.
- The kernel's stack has argc, argv and envp but no auxiliary vector: `crt1.o`'s `_start` (`crt_arch.h`) calls
  `__gvm_flat_start` (`src/env/gpuvm`), which copies them onto its own stack with an auxiliary vector after them
  (`AT_PAGESZ`, the uid and gid, and `AT_PHDR` of one program header) and calls musl's `_start_c`. That header,
  `__gvm_phdr`, is the ELF's `PT_TLS`, written by `elf2flt` (its address relocated), so `__thread` works; its
  alignment must be at most 64. The stack: the driver's size; binfmt_flat puts it after the bss and the brk room.
- `flush_icache_range` (the `sync` that a loaded program's code needs) is the kernel's.

## musl's port (`musl/`)

- `bits/`: riscv32's (asm-generic, 32-bit with 64-bit time) for `alltypes`, `stat`, `ipc`, `msg`, `sem`, `shm`,
  `posix`, `reg`, `stdint`; `syscall.h.in` from the kernel; `float.h`: long double is double; `signal.h`: no
  `SA_RESTORER` (a handler returns through the kernel's `__user_rt_sigreturn`, which it puts in ra), `mcontext_t` the
  kernel's 64 words (the pc in word 0), asm-generic's `ucontext` (the mask last); `setjmp.h` 26 words; `user.h`.
  `fenv.h` is the generic one (no rounding modes or flags: the card's float ops round to nearest).
- `syscall_arch.h`: the number in a7 (r12), arguments a0-a5, `syscall`, result a0. `atomic_arch.h`: `amocas`,
  `amoswap`, `amoadd`, `amoand`, `amoor`, `fence`; clz/ctz as the ops. `pthread_arch.h`: tp is r4, TLS above tp
  (variant 1 as lld lays it out, as RISC-V), `MC_PC` word 0. `reloc.h`: only what `dynlink.h` needs (static only).
- `src/`: `setjmp`/`longjmp` (ra, sp, s0-s23), `sigsetjmp` (as RISC-V's), `__clone` (the kernel's `CLONE_BACKWARDS`
  order), `__syscall_cp_asm`, `__set_thread_area` (mv tp), `__unmapself`, `vfork` (`clone(CLONE_VM | CLONE_VFORK |
  SIGCHLD, 0)`), `_Fork` (ENOSYS: the kernel runs a child of clone() without CLONE_VM on the parent's own memory and
  stack, both at once; vfork, posix_spawn, system and popen are the ways), `__gvm_flat_start`. Variadic arguments are
  on the stack in gpu_vm's convention (unlike RISC-V's): `__clone(func, stack, flags, arg, ...)` reads ptid, tls,
  ctid from 0, 4, 8(sp). Everything else is musl's generic C: math (double in
  software through compiler-rt, float on the card's ops: denormals flush), string, malloc (mallocng over mmap).

## Tests

    bash gpu_vm/userland/test.sh [-O 0]

builds `test/*.c` (hello; `libc`: printf of every kind and doubles, scanf, strtod, malloc, string, qsort, i64;
`files`: the initramfs's root and a tmpfs, read/write/seek/stat, stdio, directories, rename, symlinks, pipes, dup;
`proc`: vfork and execve, exit codes, a pipe to and from a child, posix_spawn, a child killed by a signal; `signals`:
raise, kill, sigaction with SA_SIGINFO and the context, the mask, alarm and pause, EINTR, SIGCHLD, longjmp and
siglongjmp out of handlers, sigsuspend; `timeenv`: argv and the environment, clocks, nanosleep, settime, gmtime,
strftime, mktime, 2100, uname; `threads`: create and join, a contended mutex, a condition variable, `__thread`, once,
keys, detached; `math`: libm in double and float), puts them in an initramfs's `/bin` with `test/init.c` as `/init`
(`INIT` and `INITRAMFS_EXTRA` of `gpu_vm/linux/build.sh`), builds a kernel in `~/gpu_vm/linux-build-ut` (whole the
first time, a relink after), boots it on gvm_cpu and looks for init's line `gvm-userland-test: 8 passed, 0 failed`.

`GVM_CPU=PATH` runs another gvm_cpu (the tree's is made and used otherwise). 22 October 2026: all 8 pass at -O2 and
-O0 (gvm_cpu of commit 6aad9c7: the reference from 44e96df on keeps a b/h/w access in its word, and the compiler and
kernel of that day still make unaligned words: the kernel panics at 0.03 s).

## libc-test

    bash gpu_vm/userland/libc-test.sh              # MATH=1 adds src/math

clones libc-test (`~/gpu_vm/dl/libc-test`, only compiled), builds its functional and regression tests (no dlopen or
shared objects; `functional/tls_local_exec` does not link: TLS aligned to 4096) with `test/ltinit.c` as `/init` (each
program with a limit of 20 s of the machine's time; /dev, /dev/shm, /tmp and /proc mounted), boots, prints the
failures. 22 October 2026: 111 of 135 pass. The 24 others: fork (6: ipc_msg, ipc_sem, daemon-failure, fflush-exit,
pthread_exit-dtor, raise-race), no /bin/sh (5: popen, spawn, vfork, execle-env, wordexp), no networking in the
kernel (socket), allocations the no-MMU kernel's OOM killer answers instead of ENOMEM (5: malloc-brk-fail, malloc-oom,
pthread_create-oom, setenv-oom, sscanf_long's 8 MB), the root being a ramfs (statvfs), musl 1.2.5's own (strtod and
strtold of two hex strings rounded at the end of a 53-bit long double, as the host computes the same arithmetic;
strptime's %F %s %z and mntent, newer than 1.2.5), clocale_mbfuncs (over the 20 s), ipc_shm (shmat on the no-MMU
kernel: shm_nattch 0, then a fault).

## Gaps

- C only: no C++ runtime (libc++, libunwind; the compiler emits no CFI).
- No shared libraries or dlopen; TLS local-exec, alignment at most 64.
- No fork: programs must use vfork/posix_spawn (busybox: its NOMMU configuration).
- memcpy/memset are musl's C: not yet the commit's copy and fill operations (the kernel's string.S uses them from
  4 KB up).
- float math in libm runs on the card's float ops (denormals flush to zero); double is compiler-rt's software.
- The kernel config has no networking, epoll, eventfd, timerfd or signalfd; FUTEX was turned on for musl's timed
  waits (`gpuvm_defconfig`).
