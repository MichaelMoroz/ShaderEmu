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
| `busybox.sh`, `rootfs.sh`, `systest.sh` | busybox, the root file system with the kernel, the system test ("The root file system") |
| `romroot.sh`, `mkromfs.py` | the root file system on the ROM ("The root file system", "The root on the ROM") |

## Building

In WSL, with the LLVM build (`gpu_vm/llvm/build.sh`) and the kernel's headers (`bash gpu_vm/linux/build.sh headers`):

    bash gpu_vm/userland/build.sh                  # musl and the builtins into ~/gpu_vm/sysroot (a minute)
    export PATH=$PWD/gpu_vm/userland/bin:$PATH
    gpuvm-linux-cc -O2 hello.c -o hello            # hello is a bFLT program
    gpuvm-linux-cc -O2 -c a.c && gpuvm-linux-cc a.o b.c -lm -o prog
    ./configure --host=gpuvm-linux CC=gpuvm-linux-cc AR=gpuvm-linux-ar RANLIB=gpuvm-linux-ranlib

`build.sh musl` unpacks `~/gpu_vm/dl/musl/musl-1.2.5.tar.gz` (downloaded and checked by its sha256 when missing) into a
clean `~/gpu_vm/musl`, copies `musl/` over it, adds `gpuvm*) ARCH=gpuvm` to `configure`, and builds with
`clang --target=gpuvm-unknown-linux -O2 -fPIE` (globals addressed from the bundle: code with no relocations; `GVM_PIC=-fno-pic` for absolute addresses, here and in `gpuvm-linux-cc`), static only. `build.sh syscalls` writes
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

## The root file system (busybox, init, a shell; on gvm_cpu and on the card)

    bash gpu_vm/userland/rootfs.sh                 # busybox (when missing), the programs, the kernel with them (WSL)
    bash gpu_vm/userland/systest.sh cpu            # the system test on gvm_cpu (WSL, or bin\gvm_cpu.exe from Git Bash)
    pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\bash.exe" gpu_vm/userland/systest.sh gpu [--check]
    build/gvm_cpu/gvm_cpu --dtb build/gvm_rootfs/gvm.dtb build/gvm_rootfs/vmlinux                    # a shell, by hand
    pwsh tools\gpu_queue.ps1 bin\gvm_gpu.exe --dtb build\gvm_rootfs\gvm.dtb build\gvm_rootfs\vmlinux   # the same on the card

| | |
|---|---|
| `busybox.sh` | busybox 1.37.0 (`~/gpu_vm/dl/busybox`, checked by its sha256) into `~/gpu_vm/busybox`: defconfig with `NOMMU`, hush as `sh`, without networking, modules, the VT, mail, printers, SELinux, PAM, utmp; 286 applets, 2.06 MB, 15,256 relocations, a stack of 128 KB (`BB_STACK`) |
| `busybox/*.patch` | applied to busybox's sources: `hush-nommu-login.patch` (below) |
| `rootfs.sh [busybox] [apps] [kernel]` | the programs into `~/gpu_vm/rootfs-build/apps`; the cpio list (`INITRAMFS_LIST` of `gpu_vm/linux/build.sh`) and a kernel with it in `~/gpu_vm/linux-build-rootfs`, the initramfs not compressed (gzip's took the kernel 0.7 s of machine time to unpack); `vmlinux`, `gvm.dtb` and `busybox` copied to `build/gvm_rootfs` |
| `rootfs/etc` | `inittab`, `init.d/rcS`, `profile`, `passwd`, `group`, `shadow` (root, no password), `hostname` (gpuvm), `motd`, `hosts`, `fstab` |
| `rootfs/gvm-tests` | `/usr/bin/gvm-tests`: runs `/usr/lib/gvm-tests` (test/'s libc, signals, threads, math, badptr) |
| `systest.sh`, `drive.pl` | the system test: boots, types a session (46 commands), checks the output, prints each command's wall time |

- **The tree**: `/bin` `/sbin` `/usr/bin` `/usr/sbin` (busybox's links), `/etc`, `/proc`, `/sys`, `/dev` (devtmpfs),
  `/tmp` (tmpfs, 8 MB), `/root`, `/home`, `/var`, `/mnt`; `/init` is a link to busybox (its `init`).
- **init**: busybox's, with `/etc/inittab`: `rcS` at sysinit (mounts proc, sysfs, devtmpfs and a tmpfs on /tmp, sets
  the hostname, prints `gpuvm: up in N s`), then `hvc0::respawn:-/bin/sh` (hvc0 rather than `/dev/console`, which can
  never be a controlling terminal: so the shell has job control). `poweroff -f` (or `reboot -f`) ends gvm_cpu and
  gvm_gpu with code 0 (the kernel's power-off: `exit_group` with TVEC 0).
- **The shell is hush**, not ash: busybox's ash needs `fork`, which a kernel without an MMU has not got. hush runs a
  subshell, `$(...)` and a pipeline's builtins by executing `/bin/busybox` again (`BUSYBOX_EXEC_PATH`). It has history,
  tab completion, vi mode, the prompt `root@gpuvm:~# `, job control (`&`, `jobs`, `wait`, `kill %1`), functions and
  64-bit `$((...))`. Applets run by vfork and exec of busybox (`FEATURE_SH_STANDALONE`; echo, printf, test, kill and the
  like are hush's builtins).
- **Other programs**: `lua` (Lua 5.5 from `~/gpu_vm/third/lua`, all its libraries, POSIX), `qjs` (QuickJS 2025-04-26 from
  `~/gpu_vm/dl/quickjs`; its REPL compiled to bytecode by the host's qjsc), `minigzip` (zlib's), `coremark` (the POSIX
  port: `coremark 0x0 0x0 0x66 ITERATIONS`; under 10 s it says "Errors detected" for the time alone, the CRCs are
  checked), `xtime` (test/xtime.c: `xtime exec N PROG`, `xtime read FILE`, in the machine's clock), `hello`.
- **The stack**: binfmt_flat gives a program `max(bss + stack, relocations x 4)` above its bss, so busybox has at least
  61 KB whatever its header says. hush's own recursion (a function calling itself) takes about 380 bytes a level: 170
  deep in 64 KB, 300 in 128 KB (the default), 500 in 256 KB. An overflow runs into the program's data: a SIGSEGV (139)
  at best; there is no MMU to catch it.
- **busybox and clang**: busybox declares the pointer to its globals `const` and assigns it once (`SET_PTR_TO_GLOBALS`);
  clang hoisted the load of it over the store (hush_main stored through NULL + 372): `-DBB_GLOBAL_CONST=` (busybox's
  own switch for that). `hush-nommu-login.patch`: hush re-executed itself with its argv[0], a login shell's
  `-/bin/sh`, so every `$(...)` and subshell read /etc/profile again (the motd in `$(...)`'s output, a `cd $HOME` before
  `(cd /tmp; pwd)` ran): a re-executed hush is never a login shell.
- **Typing into the console**: the emulators deliver stdin four characters at a time, the kernel polls every tick.
  `drive.pl` types the session's first line (`stty -echo`) at the first prompt and the rest at the second. Typed
  earlier, input was echoed by the tty into the boot's output, and what hush's line editor had read past its line was
  lost when `stty -echo` turned the shell to plain reads. (`-echo` keeps the typed-ahead commands out of the output.)

### The system test and the times (22 October 2026)

`systest.sh` boots, types 46 commands and checks 37 things in the output: `uname -a`, `ls /`, `$((6*7))`, `seq | awk`,
/proc (cpuinfo, meminfo, version, uptime), `free`, `df`, `mount`, `ps`, `top -b`, `dmesg`, `date`, `id`, gzip and gunzip,
minigzip both ways, lua, qjs, `md5sum /bin/busybox` (against the host's), sha256sum, sort, uniq, sed, tr, grep, find,
wc, tar and gzip of /etc, mkdir/cp/mv/ls/rm, du, head, tail, xargs, hexdump, od, diff, cmp, expr, test, env, a hush
function, a loop, a subshell with `$(...)`, `kill` of a background job (143), `jobs` and `wait`, `time`, gvm-tests (5
of 5) and 20 CoreMark iterations (CRCs right). All pass on gvm_cpu and on the card (gvm_gpu, tick x17). `--check` with
`SESSION=short` and with the whole session: the card and the reference the same, RAM too (84,008 passes).

| | gvm_cpu (WSL) | the card (RTX 5070 Laptop, x17) |
|---|---|---|
| boot to `Run /init` | 0.39 s | 5.1 s from gvm_gpu's start (the kernel's clock: 1.7 s) |
| boot to the first prompt | 0.69 s | 8.9 s (rcS's line: 2.6 s of the machine's time) |
| a command that runs one program (`uname -a`, `cat`, `date`) | 0.03 s | 0.35-0.4 s |
| `lua -e ...` / `qjs -e ...` | 0.02 / 0.07 s | 0.23 / 0.8 s |
| `seq 1 5 \| awk ...` (two programs) | 0.06 s | 0.8 s |
| `md5sum /bin/busybox` (2 MB) | 0.37 s | 4.0 s |
| tar, gzip, gunzip and md5sum of /etc (7 programs) | 0.29 s | 3.2 s |
| gvm-tests (5 programs: threads, signals) | 0.43 s | 6.9 s |
| CoreMark, 20 iterations | 0.07 s | 0.64 s (282 iterations a second of the machine's time) |
| the whole session | 4.0 s, 153 M bundles | 49 s, 156 M bundles, 84,654 passes |

The card's numbers moved by a quarter between runs of the same day: 6.9 s to the prompt and 0.27 s a command at 2,400
passes a second, against 1,750 for the table's.

**Where a command's time goes on the card**: an exec of busybox is 100 ms of the machine's time (500 passes), Lua's
(768 KB) 84 ms, hello's (45 KB) 13 ms (`xtime exec`); reading the 2 MB file is 14 ms of it. Most of the rest is
binfmt_flat's relocation loop: 15,256 relocations, ~290 passes an exec, each pass ending on a full write cache after
~55 stores (two ways of 256 sets: scattered words fill some set long before 514 entries), and 37-55 bundles a
relocation (calc_reloc is a call; get_user and put_user check the range). Freeing the 2 MB at exit is ~50 passes more.
What would make it faster: code that needs no relocations (globals addressed PC-relative: the compiler makes every
global's address a long immediate, `R_GVM_IMM32`; since 22 October 2026 `-fPIE`, `lpc`, the default of `gpuvm-linux-cc`: busybox 758 relocations, a one-program command on gvm_cpu 0.034 s to 0.022 s, once the sysroot and busybox are built again), a write cache that takes scattered stores, a
smaller busybox.

**Rebuilt, 22 October 2026 evening** (compiler `c837089`: PIE, busybox 758 relocations; tick x18; RAM's last 64 bytes
unused): PASS on gvm_cpu and on the card, `--check` with `SESSION=short` the reference's (16,690 passes, RAM too).

| | gvm_cpu (WSL) | the card (x18) |
|---|---|---|
| boot to `Run /init` | 0.42 s | 4.2 s from gvm_gpu's start (the kernel's clock: 1.85 s) |
| boot to the first prompt | 0.64 s | 5.9 s (6.9 in a slower run of the same build at 0179186) |
| a command that runs one program | 0.02-0.03 s | 0.16-0.25 s |
| `lua -e ...` / `qjs -e ...` | 0.02 / 0.06 s | 0.19 / 0.62 s |
| `md5sum /bin/busybox` | 0.33 s | 3.8 s |
| tar, gzip, gunzip and md5sum of /etc | 0.20 s | 1.6 s |
| gvm-tests / CoreMark 20 iterations | 0.40 / 0.07 s | 4.0 / 0.40 s |
| the whole session | 3.1 s, 83.9 M bundles | 28.4 s, 85.0 M bundles, 67,312 passes (48,501 ended on a full write cache) |

Half the bundles of before (156 M): an exec no longer relocates 15,000 words. The passes are the limit now: 72% of
them end on a full write cache.

### The root on the ROM

    bash gpu_vm/userland/romroot.sh                # after rootfs.sh: build/gvm_romroot (WSL)
    W=build/gvm_romroot bash gpu_vm/userland/systest.sh cpu --rom build/gvm_romroot/rootfs.romfs
    W=build/gvm_romroot pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\bash.exe" gpu_vm/userland/systest.sh gpu --rom build/gvm_romroot/rootfs.romfs

rootfs.sh's tree (its cpio list) as a romfs image (`mkromfs.py`, genromfs's layout) in the ROM at 0x40000000, read
through MTD (phram over the ROM's addresses: a page is one memcpy, the commit's copy from 4 KB up), as the lean RISC-V
machine has its root; the kernel (`~/gpu_vm/linux-build-romroot`: gpuvm_defconfig with MTD, phram and romfs on MTD) has
an initramfs of /root, /dev and /dev/console only, so it mounts `root=mtd:root` (`rootfstype=romfs ro init=/init`) and
unpacks nothing. The system test passes on both machines (/tmp is a tmpfs; nothing else is written). On the card: 4.0 s
to the prompt against 5.9 with the root in the initramfs, `Run /init` 1.5 s after the kernel's first line against 3.8;
the session 28.8 s, 82.9 M bundles. The ROM disk `gvmrom` (a block device) boots too, but romfs on a block device reads
1 KB buffers, each a bio and two copies: the first run of each program was seconds (qjs 3.7 s), the prompt 7.7 s.

### The kernel's part

- `gpu_vm/linux/build.sh`: `INITRAMFS_LIST=FILE`, a whole cpio list.
- `arch/gpuvm/include/asm/uaccess.h` (no `UACCESS_MEMCPY` any more): `access_ok` is true for a range in RAM (or of 0
  bytes), so a system call given a pointer outside RAM (NULL, the boot area, the device window, past RAM's end, the
  operations region) returns EFAULT instead of faulting in the kernel (an oops that killed the caller, holding whatever
  it held); `test/badptr.c` tries nine (all EFAULT). get_user and put_user are one load or store on an aligned pointer
  (asm-generic's made each four byte accesses). test.sh (8 of 8) and libc-test (111 of 135, as before) pass.

### Gaps (the system)

- No ash (hush instead), no `fork` for programs that need it. vi, less and top run, but slowly on the card for
  interactive use: a key waits for the tick's poll, a redraw goes out 255 bytes a pass.
- No networking in the kernel (the networking applets are left out), no modules, no swap, no persistent disk: the root
  is the initramfs or the ROM (`romroot.sh`), changes are lost at power-off.
- A program is loaded into RAM whole (bFLT `FLAT_FLAG_RAM`) and relocated at each exec: 2 MB and 15,000 relocations
  for every busybox command (above). No shared text (no XIP: the code has absolute addresses).
- Stack overflows are silent (no MMU).

## Gaps (the toolchain)

- C only: no C++ runtime (libc++, libunwind; the compiler emits no CFI).
- No shared libraries or dlopen; TLS local-exec, alignment at most 64.
- No fork: programs must use vfork/posix_spawn (busybox: its NOMMU configuration).
- memcpy/memset are musl's C: not yet the commit's copy and fill operations (the kernel's string.S uses them from
  4 KB up).
- float math in libm runs on the card's float ops (denormals flush to zero); double is compiler-rt's software.
- The kernel config has no networking, epoll, eventfd, timerfd or signalfd; FUTEX was turned on for musl's timed
  waits (`gpuvm_defconfig`).
