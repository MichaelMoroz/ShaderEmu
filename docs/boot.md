# Booting Linux

A cold boot of this project's image to the shell prompt takes about 3.1 s and 11.5 million
instructions on an RTX 5090 with DXC (it was 10 s and 35 million). `tools/boot_profile.py`
measures it: run the harness with `--frame-log FILE` and give it the file.

## No firmware

Upstream boots OpenSBI, which then starts the kernel and answers its calls. That was a third
of the boot's instructions. With `SBI_HLE` (the harness defines it for our image and for
snapshots; `--firmware` turns it off) the machine does the firmware's part itself:

- It starts at the kernel (`0x80400000`) in supervisor mode with the device tree's address in
  `a1`. The tree has to be in RAM for a 32-bit kernel, so `make_linux_image.py` puts it at
  `0x82200000`, where OpenSBI would have copied it.
- An `ecall` from supervisor mode is an SBI call answered in place (`sbi_call` in `emu.h`):
  set_timer, console putchar and getchar, and the base extension's version and probe calls.
  Anything else returns "not supported".
- The timer raises the supervisor's timer interrupt directly, and every trap is the
  supervisor's: nothing runs in machine mode.

A snapshot made with firmware does not run without it, and the other way round.

## Kernel and init

- `linux/kernel/config.extra`: no self-test of the hash behind `/dev/random` (1 s of the old
  boot), no sysfs entry per slab cache, no virtual terminals, no zeroing of stack frames.
  Turning those off needs `CONFIG_EXPERT`, and with it a minimal config silently leaves out
  everything else EXPERT makes visible (pseudo-terminals, futexes, tmpfs and more), so the
  file lists all of those as kept. After changing it, compare the enabled options of the old
  and new `.config`.
- The root device is mapped a page at a time, so the image builder sizes it to the image
  (56 MiB) instead of 256 MiB.
- `linux/userland/emuinit.c` does what upstream's `/rvcinit` script does, as one static
  program. The script started a dozen dynamically linked processes: 12 million instructions.
  Build it with `linux/userland/build.sh`; the image builder then makes it the kernel's init.

## What an operation costs in the tick shader

Measured by adding work to every instruction of a loop and timing the difference:

| | |
|---|---|
| arithmetic, compare, select | 0.7 ns each |
| a two-way branch | about 4 ns |
| reading an array element (a register, a cache entry) | 10 to 15 ns |
| a texture read | 25 to 30 ns |
| a `switch` with eight cases | 33 ns |

So the fast path has no large switch: arithmetic instructions compute every result and
select one, and instruction classes are told apart by tests, most frequent first. Cache
entries are read a word at a time, addresses before values. An instruction that follows
another in the same texel skips the fetch checks. Atomics on RAM are on the fast path, and
the UART is polled for input only when the host has a character waiting.

| loop | before | after |
|---|---|---|
| register arithmetic | 5.4M/s | 8.8M/s |
| loads | 4.2M/s | 5.1M/s |
| stores | 3.0M/s | 3.7M/s |
| atomics | 0.72M/s | 2.5M/s |
| glxgears under Nano-X | 500 frames/s | 555 frames/s |
| boot, median over 0.1 s windows | 3.3M/s | 3.8M to 3.9M/s |

## Still open

- Starting the shell takes 0.6 s of the boot, with the machine idle for about 0.25 s of it.
  All of it is inside the start of `/bin/sh` (build the init with `EMUINIT_FLAGS=-DMARKS` to
  see); why is not known yet.
- Frames that end early (write cache full, memory copies) make the per-frame cost (commit,
  readback: about 0.24 ms) a sixth of the boot.
- The harness compiles the tick shader with DXC on every start (2.3 s); there is no cache
  for it as there is for FXC.
