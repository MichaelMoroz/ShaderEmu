# Linux 5.17.11 for gpu_vm (`arch/gpuvm`, no MMU)

The port's sources live here; the kernel tree is `~/gpu_vm/linux` in WSL (the kernel.org 5.17.11 tarball, committed
pristine in git, so `git -C ~/gpu_vm/linux diff` is the port's change to generic files).

| | |
|---|---|
| `arch/gpuvm/` | the architecture, copied into the tree by `sync.sh` |
| `patches/*.patch` | generic files: `EM_GVM` 0x4756 in `elf-em.h`, `CLANG_TARGET_FLAGS_gpuvm` in `scripts/Makefile.clang` |
| `sync.sh` | rsync `arch/gpuvm` into `$KDIR`, apply each patch once |
| `build.sh [sync config image dtbs headers]` | `make ARCH=gpuvm LLVM=1 O=~/gpu_vm/linux-build` (`Image`, `gvm.dtb`) |
| `build.sh check` | until clang compiles C for gpuvm: every C file compiled to assembly by riscv32 clang standing in (`standin-cc.sh`) |

Clang only: `LLVM=1`, the triple `gpuvm-unknown-linux` (`GPUVM_TRIPLE=` to build.sh, or `CLANG_TARGET_FLAGS_gpuvm=` to
make). LLVM from `~/gpu_vm/llvm-build/bin` (`LLVM_BIN=`).

## Boot protocol

- The image (`vmlinux`, an ELF linked at 0x02000000, or `arch/gpuvm/boot/Image`, raw) is loaded at **GVM_RAM,
  0x02000000** and started at its first byte (`_start`, the ELF's entry): `gvm_cpu vmlinux` or
  `gvm_cpu --bin Image --at 0x02000000`.
- Core 0, supervisor mode. **r5 (a0) = the device tree's address**; 0 means GVM_DTB (0x1000, where `gvm_cpu --dtb`
  puts it). Nothing else is read from the registers. Another core that starts there `wait`s forever.
- The tree: `arch/gpuvm/boot/dts/gvm.dts` (`build.sh dtbs`: `~/gpu_vm/linux-build/arch/gpuvm/boot/dts/gvm.dtb`).
  RAM from its memory node (64 MB there: gvm_cpu's default `--ram`); with none, `CONFIG_GPUVM_RAM_SIZE` from GVM_RAM.
  bootargs from `/chosen` (`console=hvc0`).
- Until `trap_init` TVEC is 0, so an early trap is gvm_cpu's trap report. Power off and halt set TVEC 0 and make
  `exit_group` (94) by `syscall`: gvm_cpu exits; a machine with no such host spins.

## Console (device window; the RISC-V machine's console words moved down by 0x86000000)

At **0x016b8280** (`docs/machine/console.md`, "Words in memory"), plain memory read and written by both sides:

| address | owner | |
|---|---|---|
| 0x016b8280 | host | characters delivered so far |
| 0x016b8284 | host | the last delivery: up to four characters, the first in the low byte (0 bytes are skipped) |
| 0x016b8290 | guest | characters taken so far |
| 0x016b8294 | guest | bytes written so far |
| 0x016b82a0 | guest | the output ring, 256 bytes: byte n of the output at n mod 256 |

- Output: after each commit the host prints the bytes from its last count to the guest's written count (mod 256 in
  the ring). The guest writes at most 255 bytes a pass (it counts a pass by TIME, the same for a whole pass) and ends
  the pass with `sync` when its ring is full and the bytes must go (printk).
- Input: when taken == delivered, the host may put up to four new characters at +4 and add their count to +0. No
  interrupt: the kernel polls from the timer tick (100 Hz, HZ_PERIODIC) and hvc's poll.
- Kernel side: `arch/gpuvm/kernel/console.c` (a boot console from setup_arch's first line, then hvc0).

## Other devices and the machine

- Timer: TIME (SR 10-11, 100 kHz) is the clocksource and sched_clock; TCMP (SR 8-9) a one-shot clockevent on IRQ line
  1, set to 0 (off) when it fires. Interrupt lines (IEN/IPEND bits, CAUSE = 0x80000000 | the line's number): 0 software,
  1 timer, 2 external; one irq domain (`gpuvm,cpu-intc`). Idle is `wait` with IE off, then IE on.
- ROM: `gpuvm,rom` (reg = 0x40000000 and the image's size) is the read-only disk `gvmrom` (`kernel/romblk.c`); a read is
  a memcpy from the ROM. Disabled in gvm.dts until a size is filled in.
- memcpy, memmove, memset (`lib/string.S`), so copy_page, clear_page and the ROM's reads: from 4 KB up (`GVM_OP_MIN`) one
  `sq` of a copy or fill descriptor to 0xf0000000 and return (the commit makes it; the core stops only on touching what
  is still being made); below that loops of `lq`/`sq`. `flush_icache_range` is `sync` (a loaded flat program's code,
  which an operation may have written, runs after it).
- Traps: SCRATCH is the current task in user mode, 0 in the kernel; tp is the current task in the kernel. pt_regs is
  r0-r63 with the pc in slot 0, then STATUS, CAUSE, TVAL, orig a0 (saved with `sq`, four registers a store). A syscall:
  r12 the number, r5-r10 the arguments, r5 the result, resumed at EPC + 4 (a restart sets EPC back by 4).
- User ABI: asm-generic syscalls, 32-bit with 64-bit time only (as rv32: no time32 calls, no stat64, clone3,
  CLONE_BACKWARDS); `user_regs_struct` is `gpr[64]` with the pc in `gpr[0]`; `sigcontext` is that; a signal handler
  returns to `__user_rt_sigreturn` in the kernel's text (`ra` is set to it). Programs are bFLT (BINFMT_FLAT, generic
  `asm/flat.h`, `BINFMT_FLAT_NO_DATA_START_OFFSET`).

## What the compiler must give

- `--target=gpuvm-unknown-linux` (or what `GPUVM_TRIPLE` says), ILP32, char signed, `-falign-functions=16` accepted.
- tp (r4) and gp (r3) never allocated. Inline asm with `r` constraints and the operand names of ASM.md
  (`%0` printing `rN`); ops used: mfsr, mtsr, srset, srclr, amoswap, amoadd, amoand, amoor, amoxor, amocas, wait, sync,
  fence, syscall, mv, li.
- Calls reach +-2 MB: the kernel's text may be larger, so lld needs range-extension thunks for R_GVM_J18 (or the
  compiler calls through `la`+`callr` when far). The assembly files here already call out by `la t0, sym; callr t0`.
- `__builtin_ctz`, `__builtin_clz` (bitops) and `__builtin_bswap32` as the ops.

## Booting it

    bash gpu_vm/linux/build.sh                 # sync config initramfs image dtbs: ~/gpu_vm/linux-build
    make -C gpu_vm/cpu CC=gcc
    build/gvm_cpu/gvm_cpu --dtb ~/gpu_vm/linux-build/arch/gpuvm/boot/dts/gvm.dtb ~/gpu_vm/linux-build/vmlinux

The initramfs (`initramfs/`): `/dev/console` and a first `/init` (`init.S`, made flat with no relocations by
`mkflat.py`) that prints a line and writes back every line typed. On 22 October 2026 the boot reaches it:
`Run /init as init process`, its line, and a typed line back (the tty's echo, then init's), about 70 million bundles
to `/init`.

## State (22 October 2026)

Built with clang for gpuvm (`gpu_vm/llvm`) and booted on gvm_cpu to a user program on the console: traps, syscalls,
the timer, the console both ways, flat programs. Not done: a libc and busybox for gpuvm (and an elf2flt that writes
bFLT relocations for long immediates), the ROM disk tried with a real image, SMP (stores reach other cores only at the
commit, an amo is atomic on its own core only), the shaderemu drivers (GPU, input, sound, net), workers. The
"console [hvc0] enabled" line shows twice: the boot console and hvc0 both print it into the same ring.
