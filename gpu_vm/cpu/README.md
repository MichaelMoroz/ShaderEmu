# gvm_cpu

Runs programs for gpu_vm (`gpu_vm/isa.h`, `gpu_vm/GPU_VM.md`) on the host, around the reference machine
(`gpu_vm/ref/gvm_ref.h`): the test bed of the LLVM backend, and later of Linux. One C99 file, `gvm_cpu.c`.

## Build and test

- WSL / Linux (gcc or clang): `make -C gpu_vm/cpu` makes `build/gvm_cpu/gvm_cpu`; `make -C gpu_vm/cpu test` also
  makes the tests' programs (`tests/mk.cpp`, assembled with `gvm_asm.h`) and runs `tests/run.sh` (12 cases: hello, a
  sum by `addbnz` with a call and the bss, the same as a raw image, the same in passes of one bundle, stdin, argv, a
  load fault, `break`, a program's own trap handler, the timer's interrupt from `wait`, `--max-bundles`, `--trace`).
- Windows (MSVC through vswhere): `gpu_vm\cpu\build.bat` makes `bin\gvm_cpu.exe` and `bin\gvm_cpu_mk.exe`; then
  `bin\gvm_cpu_mk.exe DIR` and `sh gpu_vm/cpu/tests/run.sh bin/gvm_cpu.exe DIR` in Git Bash.

## Running

    gvm_cpu [options] PROGRAM.elf [ARGS...]
    gvm_cpu [options] --bin FILE --at ADDR [--entry ADDR] [ARGS...]

| option | |
|---|---|
| `--ram MB` | RAM from `0x02000000` (default 64) |
| `--rom FILE` | the ROM at `0x40000000` (read only) |
| `--dtb FILE` | a device tree at `0x1000` |
| `--budget N` | bundles a pass, a core (default 8192) |
| `--pass-time N` | TIME's ticks (100 kHz) a pass (default 20, 0.2 ms) |
| `--cores N` | cores (default 1; the others are made but parked: workers are not done yet) |
| `--max-bundles N` | stop after N bundles, exit 3 |
| `--trace` | each bundle on stderr: `[core] pc control-word  ops | control`, then the registers it wrote |
| `--strace` | each semihosted call on stderr: `[core] syscall N(a0, a1, a2) = result` |
| `--stats` | bundles, ops, passes and TIME at the end, on stderr |

Exit code: the program's `exit` code; 1 a bad option or file; 2 an unhandled trap (`break` too: cause, pc, tval,
the bundle and every register are printed), or every core waiting for an interrupt that cannot come; 3 `--max-bundles`.

## The machine

- **Memory**: the reference's RAM array is address 0 up to `0x02000000` + `--ram`: the boot area (0-0xffff, read only
  to the cores, the device tree at 0x1000) and the device window are words of it. An ELF's `PT_LOAD` segments go to
  their `p_vaddr` (RAM, or the ROM at `0x40000000`-`0x5fffffff`), and memsz past filesz is zeroed. The ELF must be
  32-bit little-endian with `e_machine` `EM_GVM` (0x4756) or 0; `pc` = `e_entry`. `--bin` puts a raw image at `--at`
  and starts there (or at `--entry`).
- **Passes**: each pass runs every core `gvm_run(budget)` from the RAM the last commit left, then `gvm_commit` (a
  core's stores are seen by the others only after it), then TIME += `--pass-time`. When every core is in a
  supervisor's `wait`, TIME jumps to the earliest timer compare (TCMP) that can wake one.
- **Cores** start in supervisor mode with TVEC 0, as `gvm_core_reset` makes them.

## Semihosting (bare metal)

A `syscall` op while the core's TVEC is 0 (no handler installed) is made by gvm_cpu instead of trapping: nothing of
a trap is made (EPC, CAUSE, STATUS stay), and the core goes on at the op after the syscall (the same as a handler's
`eret` to EPC + 4: the rest of the bundle, then its control word). With a TVEC set, a syscall traps as the reference
says, and a program (or Linux) handles it itself.

- number in **r12**, arguments **r5-r10**, result in **r5**: a negative errno on failure (Linux's numbers).
- The calls (Linux's generic numbers, as musl's):

| n | call | |
|---|---|---|
| 63 | `read(fd, buf, len)` | fd 0 is the host's stdin (a partial read returns what came); others -9 |
| 64 | `write(fd, buf, len)` | fd 1 stdout, 2 stderr; others -9 |
| 66 | `writev(fd, iov, n)` | iov is {base, len} words |
| 93, 94 | `exit(code)`, `exit_group(code)` | the whole machine stops; gvm_cpu exits with code & 255 |
| 57 | `close` | 0 |
| 80 | `fstat` | -38 |
| 29 | `ioctl` | -25 (not a tty) |
| 96, 172, 178 | `set_tid_address`, `getpid`, `gettid` | 1 |
| 214 | `brk(addr)` | the heap starts after the highest loaded RAM segment (16-aligned); moved to addr when it lies between that and the mmap area; returns the break |
| 222 | `mmap(addr, len, prot, flags, fd, off)` | anonymous (flags & 0x20) only: pages from below the stack down, never reused; `munmap` (215) returns 0 |
| 113, 403 | `clock_gettime(clk, ts)` | from TIME (100 kHz); 113 writes {u32 sec, u32 nsec}, 403 (time64) {s64 sec, s64 nsec} |
|  | anything else | -38 |

- The buffers are read as the core sees memory (its write log first), and what `read` and `clock_gettime` store is
  seen by the core at once.

## The program's start

`r2` (sp) points at Linux's initial stack, 16-aligned, under the argument strings at RAM's top: `argc`, `argv[0..]`
(argv[0] is the program's path), 0, envp (none) 0, auxv `AT_PAGESZ` 4096, `AT_NULL`. Also `r5` = argc, `r6` = argv,
`r7` = envp. A megabyte below that is kept for the stack (the mmap area starts under it). Every other register is 0.

## Not done yet (hooks in place)

- Linux: the kernel's entry convention for the device tree's address (`--dtb` loads it at `GVM_DTB`); a console
  device and input in the device window and the external interrupt line (`gvm_core.ext`) are read after each commit
  (TODO in the pass loop). The timer interrupt (TCMP) and `wait` work now.
- Workers: cores past 0 are made parked; mailboxes at `GVM_MBOX` (docs/machine/multicore.md's layout) are TODO.

## The reference's hooks

`gvm_core` has `host_trap` (called for a trap while TVEC is 0; nonzero: dealt with, nothing of the trap is made),
`host_step` (before each bundle; nonzero halts the core) and `host`. They are NULL after `gvm_core_reset`, so the
tick's bench and checks run as before. gvm_cpu uses them for semihosting, the trap report, `--trace` and
`--max-bundles`.
