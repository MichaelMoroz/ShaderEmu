# Linux without an MMU

The same system and programs on a kernel built without the MMU (`CONFIG_MMU` not set): every
program is a flat (bFLT) one loaded whole into RAM, and the shader machine can run with no page
walk at all (`NO_PAGING`). It is the machine everything runs by default: the harness, `rvc_cpu`,
`tools/test_all.py` and the world. What it builds on is shared: `linux/kernel/build.sh`'s toolchain
and kernel tree, and the shared root, which `linux/nommu` takes and makes flat.

## Building it

Without a compiler: `pwsh tools\make_image.ps1` makes the shared image from `rvc/` (a clone of
upstream rvc: its root file system is the ROM's start) and `linux/prebuilt`, and then this one from
`linux/prebuilt/nommu` (its kernel and the files that differ from the shared root).
`pwsh tools\make_image.ps1 --save-prebuilt` keeps a build there.

In WSL, from nothing: `linux/kernel/build.sh` (the cross compiler and the kernel's tree, in
`~/shaderemu-linux`), `linux/userland/build.sh` (musl's sysroot, init), and the shared image above, then
the steps below. WSL's `python3` wants numpy (Quake III's bake, ClassiCube's sound; `python3-numpy`), and
`ffmpeg` is wanted once for ClassiCube's tune (`linux/classicube/sound/make_sound.py`, then cached) and for
Half-Life's data. The address `linux/pathtrace/build.sh` fetches the teapot from no longer answers: put
`linux/prebuilt/root/root/Models/teapot.obj` in `~/nm/work/src/pathtrace-data/` first.

- `bash linux/nommu/setup.sh` once: `~/nm/work`, a build folder of the shared sources with
  nothing compiled, a copy of the built root in `~/nm/repo`, and compiler drivers (`rv32-cc`,
  `rv32-c++`) that compile every program position-independent and link it as a flat program
  (`flatcrt.c` is its start). `fork` is `vfork` there (`nommu_fork.h`, included in every file).
  `RV32_MARCH=rv32imaf_zba_zbb` builds with Zba and Zbb. It can be run again: `libc.a` goes back to musl's and
  toolchain.sh's members are put in anew by the new drivers, and a `__FILE__` says `/nm/...` wherever the folder is, so
  the libraries' bytes and `cc-bytes` do not go by the folder's name (31 October 2026).
- `bash linux/nommu/kernel.sh`: the tree of `linux/kernel/build.sh` copied with this repository's drivers and hooks put in again
  (that tree's driver can be old), and a failed make stops it, seven patches
  (`patch_kconfig.py`: RV32 without the MMU, still in supervisor mode under the machine's SBI and
  linked at 0x82000000; `patch_signal.py`, `patch_drivers.py`, which also keeps `/proc/cpuinfo`'s
  "mmu" line out, `patch_ovl.py`, `patch_ramfs.py`, `patch_console.py`: the console in memory,
  `patch_procmaps.py`: a read of `/proc/PID/maps` gives PID's mmap lock back, as upstream since
  6.8) and its configuration without the MMU, into
  `build/nommu/Image`. Then the ones of the RISC-V backport (28 October 2026; each commit has its
  numbers): `patch_uaccess.py` (`access_ok` checks the ROM and RAM, a `get_user` or `put_user` is
  one load or store), `patch_memmap.py` (the boot's `struct page`s copied from one, frees that
  store only what changes), `patch_fastrun.py`
  (a program's memory one block of the buddy allocator, freed in aligned blocks),
  `patch_memcpy.py` (no byte loops for misaligned copies; the commit's parallel copy for any
  large copy whose two ends lie alike in a texel), `patch_flatreloc.py` (relocations a
  word at a time, and since 31 October 2026, in a program whose header has flag `0x40000000`, the entries and the words
  in the machine's order: `elf2bflt.py` and `elf2flt.c` write them so, no byte swap twice a relocation),
  `patch_romfs.py` (a directory listed where it lies in the ROM, not an entry's three reads; a name found by its directory's table of names, which `slim.py` writes at the ROM's end for a directory of 32 entries or more, in place of a walk of the list), `patch_sysfs.py` (no sysfs for the boot's slab caches and built-in
  parameters), `patch_zeroread.py` (`/dev/zero` a megabyte a step,
  no fault-in of a write's buffer) and `patch_timekeeping.py` (with one processor the tick
  advances the timekeeper in place, not a shadow copied whole, and at every 16th tick: readers add
  the clock's cycles since anyway, and the coarse clocks, a file's date, are a third of a second old at most), and `patch_exitpark.py`. That last
  one calls `shaderemu_gpu_exiting` in `do_exit` before `exit_mm`. It parks a dying program's
  worker cores and its lanes' job, then ends the pass. A worker reads its start word as its pass
  begins, so one already in that pass stores into the program's memory when the pass ends: parked
  only at the release, after `exit_mm`, the workers would run on in memory already freed. Once they
  are parked, their answers' alive word is cleared, or the next program to start one would take a
  parked core's `MC_ALIVE` for its own worker there (`arrived()` in `programs/mc/mcw.c`, which also
  wants the program's tag). The configuration has no VM event
  counters, kallsyms, AIO, io_uring or core dumps and a 16 KB log, and trusts the device tree's
  `rng-seed`, which `image.py` writes (128 bytes; `RNG_SEED` in hex gives a fixed one): without
  it a `getrandom()` without `GRND_NONBLOCK` never returns.
- `bash linux/nommu/build.sh [script ...]`: the sources copied to `~/nm/repo` again and the
  repository's build scripts run there with the flat drivers, a line each.
- `bash linux/nommu/root.sh`: mctest and the machine's `elf2flt`, then every program of the root
  made flat (`flat.py`, `elf2bflt.py`) into `build/nommu/flatroot`. binfmt_flat writes the data's start into
  the word just below it (its shared-library table, `NO_DATA_START_OFFSET` or not; it can overwrite a variable such as
  musl's `decfloat`): both converters start the data at the first 32 bytes, from the first writable segment down to the
  text's end, with no section in that word, and refuse a program with none. The linker leaves a page between text
  and data, so every program of the root keeps its bytes; `tcc -shared` does not, so a program of
  `cc`'s has its data start moved down.
- `python linux/nommu/image.py [--save-prebuilt]`: `build/images/nommu`, the shared image with
  that root as its overlay, the kernel, and RAM in the device tree in one range from the kernel's
  address (0x82000000) to 0x92000000 ("Memory", below); without
  `build/nommu`, from `linux/prebuilt/nommu`.
- `python tools/test_all.py`: the test set, on this image once it is made (`linux/nommu/test.py`
  names it); `cc-bytes` takes the flat programs' digest on an image whose RAM starts at
  0x82000000.

`rvc_harness` boots this image once it has been made, on the machine without paging
(`NO_PAGING`), with its window, desktop and sound; so does `rvc_cpu`, whose workers with a page
table root of 0 run untranslated unless `RVC_BARE_WORKERS=0`. `--isa base|zb` chooses the instruction
set (`zb` takes `build/images/nommu-zb` and the shader's `ZB`), as `i` does in the start menu.

`pwsh linux/nommu/all.ps1 [-Kernel]` is the whole round. The machine's own `cc` links a flat
program as a library and makes it flat with `elf2flt` (`linux/nommu/cc`). `linux/nommu/card.ps1`
times the games on the card, a configuration a run (an image and the shader's defines); a Zb
image is `RV32_MARCH=rv32imaf_zba_zbb` through `build.sh` and `root.sh`, then `image.py nommu-zb`.

## In the world

The VRChat world runs this Linux and no other: `MachineTick.shader` defines `NO_PAGING` (with
`SBI_HLE` and `FPU`, no `ZB`), and "ShaderEmu/Import boot images from the repository" takes the
"linux" image from `build/images/nommu`. After a no-MMU build: `python linux/nommu/image.py`,
then "Sync shader sources from the repository" and "Import boot images from the repository".

## What differs

- The kernel: the GPU's and the sound card's memory are mapped where they lie; a large mapping
  takes the first run of free pages long enough, not a buddy block, and drops the page cache
  before it fails; the kernel's early allocations come from the bottom of RAM; a ROM file is
  mapped from the ROM through overlayfs; a signal's trampoline on the stack is made visible to
  the instruction fetch; a program in the ROM is copied into RAM by one parallel copy.
- tmpfs is ramfs without the MMU, and it is the writable layer of the root's overlay. It has
  `RENAME_WHITEOUT` and `trusted.*` attributes as shmem has them (`patch_ramfs.py`), and the
  overlay has `redirect_dir`: a file or a folder of the ROM is renamed, moved and removed in
  place. An attribute costs about 60 bytes of slab, on what was copied up or renamed.
- A program's whole memory is taken at its start: its data, bss and stack, and every anonymous
  mapping, cleared. A worker program gives its workers' stacks back when it closes them
  (`mcw_close`); a large pool is mapped at its size, not `calloc`'d (Quake III's zone and hunk).
  Quake III's pools are what q3dm17 takes with headroom (`build.sh`): the hunk 8 MB, the zone 4,
  the server's 6; `meminfo` says what is used. The starter sets the hunk and the zone on the command
  line, so a `q3config.cfg` kept in `/persistence` cannot ask for more than memory in pieces gives.
- The shader machine: `BARE_WORKERS` starts a worker whose page table root is 0 with no
  translation; `NO_PAGING` takes every address as physical. `ZB` runs Zba and Zbb;
  `X_NO_FAST_AMO` sends atomics to the general path (for measuring).
- `rvc_cpu`: bare workers as `BARE_WORKERS` (`RVC_BARE_WORKERS=0` turns them off), `RVC_ZB=1` as `ZB`, `RVC_GPU_COPY=1` marks
  every pixel of a drawn list's copy into RAM and names each new target.
- Upstream's programs in the ROM are dynamic ELF programs and do not run here. The flat ones
  are added beside them and hide them by name (`tools/make_linux_image.py`): BusyBox's applets
  for `bc`, `dc`, `ed`, `traceroute`, `arping`, `ping` and `ip` (links to `/bin/busybox`,
  `linux/userland/busybox.sh`), QuickJS and MicroPython (`docs/system/scripts.md`), the
  terminal programs (`docs/system/terminal.md`) and the network and system ones (`docs/system/ports.md`). A link of the image (`NAME.romlink`) hides
  upstream's file of that name as a file does. Upstream's ELF files leave the no-MMU ROM
  ("The ROM", below).
- Programs: libmedia makes a picture in memory and writes it where a file cannot be mapped for
  writing; nxfiles knows a flat program; tcc copies a section that cannot grow in place; the
  NES reads its leave word PC-relative.

## The ROM

`image.py` ends with `slim.py`, which writes the romfs anew without what this machine can neither
run nor read: upstream's dynamic ELF programs and shared libraries (the kernel has
`CONFIG_BINFMT_FLAT` and no ELF loader), the copies of upstream's files that ours hid (renamed
with a dot, still in the ROM), `/usr/local` (bcc and beluga, a C compiler for i386), screen's
encodings, GLib's valgrind list, sshd's `moduli`, a folder of `ORPHANS` whose program is gone
(upstream zsh's functions when no flat zsh is built), and the links to any of them. Object files
(`crt1.o`, `flatcrt.o`) and every flat program stay. Files of `SQUEEZE` are stored as deflate in
segments (`docs/system/media.md`) when their reader says it inflates them: `magic.mgc` for
`file` (`linux/terminal/file-mdz.patch`). The kept files are the same bytes, in the same order,
and the boot arguments' root device takes the new size.

On 31 October 2026 `rootfs.bin` of `build/images/nommu` is 304,336,384 bytes, Half-Life's data in it.

## The state

With `NO_PAGING` and `SBI_HLE` (the world's machine, and `rvc_harness`'s for this kernel) the
CSRs Linux uses are fields of core 0's first three texels: mstatus, mie, mip, stvec; sscratch,
sepc, scause, stval; scounteren and fcsr (`CSR_FIELDS` in `types.h`, `csr.h`). There is no CSR
table under the state rows and no CSR cache, so a CSR write never stalls; the machine's own
CSRs read as 0 and a write to one is dropped. `NO_PAGING` also leaves the TLBs' 409 texels
out: the float registers follow the write cache, and the tick draws 8 of core 0's 16 rows
(`CORE0_ROWS`), 500 texels, a worker's layout with a cache of 2^6 (8 x 8 pixels of the tick's
targets; `docs/machine/multicore.md`). `CSR_TABLE` and `CORE0_TLB_ROWS` compile the other layouts.
Unity's commit (`MachineCommit.cginc`) has the tick's defines.

`CSR_FIELDS` also leaves out what only a machine with paging needs:

- A trap or interrupt goes to supervisor mode only: no delegation, no machine-mode entry.
  Interrupts are SEIP, SSIP and STIP.
- No UART and no CLINT. The console is memory the kernel reads and writes itself
  (`docs/machine/console.md`, "Words in memory"). The timer is the host's time for the pass against
  SBI `set_timer`'s compare, looked at once a pass and at the call.
- The RTC is read from the host's uniforms.
- No `mmu` words, no single step, no debug words but the last instruction and stall.
- The copy's destination for the commit's geometry comes from texel 37 (no `memop_*_p`).

The tick's device reads and writes are the general ones still (`mem.h`), but for the RTC: paths cut
down further are slower a pass under fxc.

## Memory

The layout (28 October 2026), in one place for the tick (`experiments/rvc_opt/src/layout.h`)
and one for the host (`harness/apps/rvc_layout.h`):

| from | to | what |
|---|---|---|
| 0x80000000 | 0x81e00000 | the devices: the GPU's memory (a program's buffers, the windows', the display's), the workers' mailboxes at 0x80c00000, the control words at 0x81000000 (the network's and the console's row at 0x816b8000, a fetch's answer at 0x816c0000), the sound card's memory from 0x81b00000 |
| 0x81e00000 | 0x82000000 | the device words no GPU copy may reach |
| 0x82000000 | 0x92000000 | RAM in one range, 256 MB: the kernel from its start, where the machine enters it, then Linux's |

The boot image still loads from 0x80000000 (its first 32 MB are zeros), the kernel at +32 MB and the
device tree at +62 MB (0x83e00000; `tools/make_linux_image.py`, `image.py`'s `TREE_AT`), whose
memory node is that one range. The state texture is 2048 x 9280 (`STATE_TEX_HEIGHT` in `src/types.h`,
`kStateHeight` in `harness/apps/rvc_backend.h`), RAM in it what is under `RAM_MAX` (0x12000000
bytes), and `rvc_cpu` has 288 MB. A copy of the GPU's picture goes only to 0x80000000 to 0x81e00000,
as a GPU program's buffers do (`gpu_writeback` in `src/gpu.h`), and a program's parallel copy or fill
only there or to RAM (`UOP_IN_RAM` in `linux/userland/uop.h`). The drivers find their devices with
`page_is_ram` (`patch_drivers.py`), false below the kernel. A snapshot is marked with the layout it
was taken in (`SX86SNP2`, `RVCSNAP2`): one of the layout before is refused, to be made again.

The kernel sets up a page structure for each of RAM's pages: the boot to the shell is 7.5 million
instructions, about 2 s of the guest's time, and `MemTotal` at the shell is 256,060 kB, 245,852 of it
free (`rvc_cpu`, 31 October 2026).

The commit's bands of RAM stay 4 MB and a mask of 32 bits: bit b stands for bands b, b + 32 and
b + 64 (`ram_bands`), so the tick's store marks its band as it did, at no cost an instruction. A
band the commit must draw draws its two others too; the control words' band (4, and 5 for a fetch's
answer) is drawn every commit apart from the mask (`_CommitAlways` in the harness, `CONTROL_BANDS` in Unity's
`MachineBlit.cginc`, which draws runs of bands). The state texture is 290 MiB of video memory.

A program's memory is taken whole and in runs of pages: a mapping wants one run as long as
itself. Quake III is the largest, with its data and bss, the hunk 8 MB, the server's zone 6 and the
zone 4. `python linux/nommu/memory.py` runs each large program and then Quake III, and all of them
one after another in order and reversed, a machine each under `rvc_cpu` (minutes; all must fit). What
a program leaves in a tmpfs file stays where it lies until the file goes: Red Alert and Tiberian Dawn
run from a folder of their own in the ROM, no copy in `/tmp`.

## Speed on the card

`docs/machine/speed.md` has the machine against the compute machine, the CPU harness and upstream
rvc. `linux/nommu/card.ps1` times the games on the card, a configuration a run (an image and the
shader's defines), which is how `--isa zb` is told from the base set: the programs are built with
Zba and Zbb, musl not. On the card the float programs' sums are the card's (its division).
