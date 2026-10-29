# Linux without an MMU

The same system and programs on a kernel built without the MMU (`CONFIG_MMU` not set): every
program is a flat (bFLT) one loaded whole into RAM, and the shader machine can run with no page
walk at all (`NO_PAGING`). It is the machine everything runs by default: the harness, `rvc_cpu`,
`tools/test_all.py` and the world. `linux/nommu/` builds it beside the MMU build, which it is
made from and does not change; Linux with the MMU is legacy (`--kernel mmu`).

## Building it

Without a compiler: `pwsh tools\make_image.ps1` makes the MMU image from `linux/prebuilt` and
then this one from `linux/prebuilt/nommu` (its kernel and the files that differ from the MMU
root). `pwsh tools\make_image.ps1 --save-prebuilt` keeps a build there.

In WSL, after the MMU build (`linux/kernel/build.sh` and the programs' build scripts):

- `bash linux/nommu/setup.sh` once: `~/nm/work`, a build folder of the MMU one's sources with
  nothing compiled, a copy of the built root in `~/nm/repo`, and compiler drivers (`rv32-cc`,
  `rv32-c++`) that compile every program position-independent and link it as a flat program
  (`flatcrt.c` is its start). `fork` is `vfork` there (`nommu_fork.h`, included in every file).
  `RV32_MARCH=rv32imaf_zba_zbb` builds with Zba and Zbb.
- `bash linux/nommu/kernel.sh`: the MMU kernel's tree copied with its hooks, seven patches
  (`patch_kconfig.py`: RV32 without the MMU, still in supervisor mode under the machine's SBI and
  linked at 0x82000000; `patch_signal.py`, `patch_drivers.py`, which also keeps `/proc/cpuinfo`'s
  "mmu" line out, `patch_ovl.py`, `patch_ramfs.py`, `patch_console.py`: the console in memory,
  `patch_procmaps.py`: a read of `/proc/PID/maps` gives PID's mmap lock back, as upstream since
  6.8; before, PID's next `mmap` waited forever) and its configuration without the MMU, into
  `build/nommu/Image`. Then the ones of the RISC-V backport (28 October 2026; each commit has its
  numbers): `patch_uaccess.py` (`access_ok` checks the ROM and RAM, a `get_user` or `put_user` is
  one load or store), `patch_memmap.py` (the boot's `struct page`s copied from one, frees that
  store only what changes: the boot 14.5 to 10.2 million instructions), `patch_fastrun.py`
  (a program's memory one block of the buddy allocator, freed in aligned blocks),
  `patch_memcpy.py` (no byte loops for misaligned copies; the commit's parallel copy for any
  large copy whose two ends lie alike in a texel), `patch_flatreloc.py` (relocations a
  word at a time), `patch_sysfs.py` (no sysfs for the boot's slab caches and built-in
  parameters: the boot 9.9 to 8.0 million), `patch_zeroread.py` (`/dev/zero` a megabyte a step,
  no fault-in of a write's buffer) and `patch_timekeeping.py` (with one processor the tick
  advances the timekeeper in place, not a shadow copied whole), and `patch_exitpark.py`. That last
  one calls `shaderemu_gpu_exiting` in `do_exit` before `exit_mm`. It parks a dying program's
  worker cores and its lanes' job, then ends the pass. A worker reads its start word as its pass
  begins, so one already in that pass stores into the program's memory when the pass ends.
  Parked only at the release, after `exit_mm`, the workers ran on through the passes between, in
  memory already freed. No failure was traced to it: the shell's crash in nes-kill and nxphys-kill
  under `RVC_BESIDE=1` was `rvc_cpu`'s, whose core 0 went on past a `fence.i`. The configuration has no VM event
  counters, kallsyms, AIO, io_uring or core dumps and a 16 KB log, and trusts the device tree's
  `rng-seed`, which `image.py` writes (128 bytes; `RNG_SEED` in hex gives a fixed one): without
  it a `getrandom()` without `GRND_NONBLOCK` never returned.
- `bash linux/nommu/build.sh [script ...]`: the sources copied to `~/nm/repo` again and the
  repository's build scripts run there with the flat drivers, a line each.
- `bash linux/nommu/root.sh`: mctest and the machine's `elf2flt`, then every program of the root
  made flat (`flat.py`, `elf2bflt.py`) into `build/nommu/flatroot`. binfmt_flat writes the data's start into
  the word just below it (its shared-library table, `NO_DATA_START_OFFSET` or not; it can overwrite a variable such as
  musl's `decfloat`): both converters start the data at the first 32 bytes, from the first writable segment down to the
  text's end, with no section in that word, and refuse a program with none. The linker leaves a page between text
  and data, so every program of the root keeps its bytes; `tcc -shared` does not, and the example cube's
  `.eh_frame_hdr` ran into its `.dynamic`: its data now starts at 0x27500 for 0x2d000 (only the header's word
  differs), and `cc-bytes`' digest moves once the image's `elf2flt` is built from this.
- `python linux/nommu/image.py [--save-prebuilt]`: `build/images/nommu`, the shared image with
  that root as its overlay, the kernel, and RAM in the device tree in one range from the kernel's
  address (0x82000000) to 0x92000000 ("Memory", below); without
  `build/nommu`, from `linux/prebuilt/nommu`.
- `python tools/test_all.py`: the test set, on this image once it is made (`linux/nommu/test.py`
  names it); `cc-bytes` takes the flat programs' digest on an image whose RAM starts at
  0x82000000.

`rvc_harness` boots this image once it has been made, on the machine without paging
(`NO_PAGING`), with its window, desktop and sound; so does `rvc_cpu`, whose workers with a page
table root of 0 run untranslated unless `RVC_BARE_WORKERS=0`. `--kernel mmu|nommu` and
`--isa base|zb` choose the kernel and the instruction set (`zb` takes `build/images/nommu-zb` and
the shader's `ZB`), as `k` and `i` do in the start menu.

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
  place (before, a rename was "Invalid argument", a folder's removal "I/O error", and `mv` of a
  folder copied it). An attribute costs about 60 bytes of slab, on what was copied up or renamed.
- A program's whole memory is taken at its start: its data, bss and stack, and every anonymous
  mapping, cleared. A worker program gives its workers' stacks back when it closes them
  (`mcw_close`); a large pool is mapped at its size, not `calloc`'d (Quake III's zone and hunk).
  Quake III's pools are what q3dm17 takes with headroom (`build.sh`): the hunk 8 MB (5.3 at
  most), the zone 4 (42 KB), the server's 6 (3.2 with seven bots); `meminfo` says what is used.
  The starter sets the hunk and the zone on the command line: a `q3config.cfg` the MMU world
  kept in `/persistence` says 32 and 16 MB, and 32 in one run failed once memory was in pieces.
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
with a dot, still in the ROM), `/usr/local` (bcc and beluga, `docs/system/ports.md`), screen's
encodings, GLib's valgrind list, sshd's `moduli`, a folder of `ORPHANS` whose program is gone
(upstream zsh's functions when no flat zsh is built), and the links to any of them. Object files
(`crt1.o`, `flatcrt.o`) and every flat program stay. Files of `SQUEEZE` are stored as deflate in
segments (`docs/system/media.md`) when their reader says it inflates them: `magic.mgc` for
`file` (`linux/terminal/file-mdz.patch`). The kept files are the same bytes, in the same order,
and the boot arguments' root device takes the new size.

On 18 October 2026 that was 296,400,896 bytes to 255,211,520 (41.2 MB of files, 258 entries)
and 248,428,544 with `magic.mgc` (7,254,168 bytes as 422,324): 7,582 rows of the world's ROM
texture (2048 texels of 16 bytes) where it was 9,046; without zsh (6.1 MB) 242,377,728 bytes,
7,397 rows. The MMU image keeps all of it: its
upstream programs run there.

## The state

With `NO_PAGING` and `SBI_HLE` (the world's machine, and `rvc_harness`'s for this kernel) the
CSRs Linux uses are fields of core 0's first three texels: mstatus, mie, mip, stvec; sscratch,
sepc, scause, stval; scounteren and fcsr (`CSR_FIELDS` in `types.h`, `csr.h`). There is no CSR
table under the state rows and no CSR cache, so a CSR write never stalls; the machine's own
CSRs read as 0 and a write to one is dropped. `NO_PAGING` also leaves the TLBs' 409 texels
out: the float registers follow the write cache, and the tick draws 8 of core 0's 16 rows
(`CORE0_ROWS`), 500 texels, a worker's layout with a cache of 2^6 (8 x 8 pixels of the tick's
targets; 9 rows and 564 texels before the cache's third table was a quarter, `docs/machine/multicore.md`). `CSR_TABLE` and
`CORE0_TLB_ROWS` compile the old ones. On the card (D3D11, fxc2, 17 October 2026, two runs
each, alternating): a shell loop 4,889k instructions a second before and 5,025k after (+2.8%),
Doom's timedemo 1,176 realtics before and 1,165 after (+1%); mctest with 255 workers, fptest and
Doom's state sums as before. Unity's commit (`MachineCommit.cginc`) has the tick's defines.

`CSR_FIELDS` also leaves out what only the MMU machine has:

- A trap or interrupt goes to supervisor mode only: no delegation, no machine-mode entry.
  Interrupts are SEIP, SSIP and STIP.
- No UART and no CLINT. The console is memory the kernel reads and writes itself
  (`docs/machine/console.md`, "Words in memory"). The timer is the host's time for the pass against
  SBI `set_timer`'s compare, looked at once a pass and at the call.
- The RTC is read from the host's uniforms.
- No `mmu` words, no single step, no debug words but the last instruction and stall.
- The copy's destination for the commit's geometry comes from texel 37 (no `memop_*_p`).

The tick's device reads and writes are the MMU machine's still (`mem.h`), but for the RTC: the
paths cut down were 2 to 4% slower a pass under fxc. On the card, against the tree before
(17 October 2026): 3,000 full passes from one Quake III snapshot (q3dm17 with three bots,
frame 100) took 5.32 s against 5.56, means of two. Whole runs of a game vary 4% from run to
run and did not tell the two apart. mctest with 255 workers, fptest, doom-1, nxray's sums and
the boot's clock were as before.

## Memory

The layout since 28 October 2026, in one place for the tick (`experiments/rvc_opt/src/layout.h`)
and one for the host (`harness/apps/rvc_layout.h`):

| from | to | what |
|---|---|---|
| 0x80000000 | 0x81e00000 | the devices: the GPU's memory (a program's buffers, the windows', the display's), the workers' mailboxes at 0x80c00000, the control words at 0x81000000 (the network's and the console's row at 0x816b8000, a fetch's answer at 0x816c0000), the sound card's memory from 0x81b00000 |
| 0x81e00000 | 0x82000000 | the device words no GPU copy may reach |
| 0x82000000 | 0x92000000 | RAM in one range, 256 MB: the kernel from its start, where the machine enters it, then Linux's |

The boot image still loads from 0x80000000 (its first 32 MB are zeros), the kernel at +32 MB and the
device tree at +62 MB (0x83e00000; `tools/make_linux_image.py`, `image.py`'s `TREE_AT`), whose
memory node is that one range. The state texture is 8192 x 2368 (`RAM_W` and `STATE_TEX_HEIGHT` in
`src/layout.h`, `kStateWidth` and `kStateHeight` in `harness/apps/rvc_backend.h`; 2048 x 9280 until
29 October 2026: `docs/machine/gpu.md`, "Rows of 8192"), RAM in it what is under `RAM_MAX` (0x12000000
bytes), and `rvc_cpu` has 288 MB. A copy of the GPU's picture goes only to 0x80000000 to 0x81e00000,
as a GPU program's buffers do (`gpu_writeback` in `src/gpu.h`), and a program's parallel copy or fill
only there or to RAM (`UOP_IN_RAM` in `linux/userland/uop.h`). The drivers find their devices with
`page_is_ram` (`patch_drivers.py`), false below the kernel. A snapshot is marked with the layout it
was taken in (`SX86SNP2`, `RVCSNAP2`): one of the layout before is refused, to be made again. (Since 29
October 2026 the harness writes `SX86SNP3`, rows of 8192, and lays an `SX86SNP2` one out anew as it loads.)

Before 28 October 2026 the devices lay at 0x86000000 to 0x88000000 and RAM was two ranges, from the
kernel at 0x80400000 to them (92 MB) and from 0x88000000 to 0x92000000 (160 MB): `MemTotal` was
250,956 kB and 240,936 of it free at the shell (18 October 2026), where they had been 88,752 and
78,740 when RAM ended at the devices, as the MMU kernel's does.

The boot to the shell is 14.5 million instructions where it was 10.0, 1.4 s more of the guest's
time: the kernel sets up a page structure for each of 49,152 more pages (the gap between the
ranges was 8,192 of them) and frees each (`memmap_init_range`, `memset` of each structure,
`__free_pages_core`, `__free_pages_ok`). A test that presses a button once a game is loaded
presses it at 20 s where it was 16 (`classicube-seat-*`), and ClassiCube's `classicube-pack`
draws one text fewer by `classicube: steady`.

The commit's bands of RAM stay 4 MB (32 rows of 8192 texels) and a mask of 32 bits: bit b stands for bands b, b + 32 and
b + 64 (`ram_bands`), so the tick's store marks its band as it did, at no cost an instruction. A
band the commit must draw draws its two others too; the control words' band (4, and 5 for a fetch's
answer) is drawn every commit apart from the mask (`_CommitAlways` in the harness, `CONTROL_BANDS` in Unity's
`MachineBlit.cginc`, which draws runs of bands). The state texture is 290 MiB where it was 128,
twice that with its other buffer: 580 MiB of video memory a machine where it was 256 (296 MiB and one
buffer since: "RAM in one texture" and "Rows of 8192" in `docs/machine/gpu.md`).

A program's memory is taken whole and in runs of pages: a mapping wants one run as long as
itself. Quake III is the largest, 46 MB in four runs (its data and bss 22, the hunk 8, the
server's zone 6, the zone 4). `python linux/nommu/memory.py` runs each large program and then
Quake III, and all of them one after another in order and reversed, a machine each under
`rvc_cpu`, in about two and a half minutes (18 s on 18 October): on 16 October 2026 all twelve fit,
Quake III with 21.4 MB free at the least; with RAM to 0x92000000 (18 October) all twelve fit with
178 MB free at the least (16 MB on the same image with RAM to 0x86000000). The script then flagged
four runs whose console had a program's first line written into the echo of its command, where
the `|` that marks an echo was on the next line: no allocation failed. Over 26 programs one after another the shell grew 25 KB, Nano-X 29 KB and
the slab 156 KB. What a program leaves in a tmpfs file stays where it lies until the file goes:
Red Alert and Tiberian Dawn run from a folder of their own in the ROM, no copy in `/tmp`.

## Speed on the card

RTX 5090, D3D11, fxc2, 16 October 2026; one run a cell, guest time (repeated runs varied by 1 to 3%;
`docs/machine/speed.md` has the no-MMU machine on an RTX 5070 laptop on 19 October against the compute
machine, the CPU harness and upstream rvc, which has the MMU):

| Program | MMU | no MMU, `NO_PAGING` | and Zba/Zbb |
|---|---|---|---|
| Doom (timedemo demo1) | 129.1 fps | +11.2% | +13.2% |
| Quake (timedemo demo1) | 80.7 fps | +14.6% | +17.2% |
| Quake III (q3dm17, 3 bots, frames 101 to 300) | 20.9 fps | +15.6% | +17.9% |
| nxray (8 pictures 320 x 240) | 26.1 a second | +16.4% | +14.1% |
| Red Alert (game frames 300 to 900) | 75.6 a second | +15.9% | +14.0% |

Zba and Zbb take 1 to 2.5% of the instructions away (the programs built with them, musl not);
over `NO_PAGING` alone they are within the noise. Without `NO_PAGING` the no-MMU kernel gave
Doom 5.5% over the MMU one. On the card the float programs' sums are the card's (its division),
the same with either kernel.
