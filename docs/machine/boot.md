# Booting Linux

A cold boot of this project's image to the shell prompt takes 1.13 s of the guest's clock (calibrated, 31 October 2026:
`rvc_cpu --pass-model`, `cat /proc/uptime` typed at the prompt) and 7.18 million instructions in 933 passes (`rvc_cpu`,
`--until "/ # "`, the same day). `docs/machine/speed.md` has the other machines. On the card, `tools/boot_profile.py` measures it:
run the harness with `--frame-log FILE` and give it the file.

## No firmware

Our image has no firmware in it: with `SBI_HLE` (the harness defines it for our image and for snapshots) the machine does the
firmware's part itself.

- It starts at the kernel (`0x82000000`, `KERNEL_AT` in `src/layout.h`) in supervisor mode with the device tree's address in
  `a1`. The tree has to be in RAM, so `make_linux_image.py` and `image.py` put it at `0x83e00000` (`TREE_AT`).
- An `ecall` from supervisor mode is an SBI call answered in place (`sbi_call` in `emu.h`): set_timer and the base extension's
  version and probe calls. Anything else returns "not supported". The console is memory the kernel writes itself (`console.md`,
  "Words in memory"), so no console call is answered (`CSR_FIELDS`, `docs/system/nommu.md`).
- The timer raises the supervisor's timer interrupt directly, and every trap is the supervisor's: nothing runs in machine mode.
  There is no CLINT: the timer is the host's time against set_timer's compare.

## Kernel and init

- `linux/kernel/config.extra`: no self-test of the hash behind `/dev/random`, no sysfs entry per slab cache, no virtual terminals, no
  zeroing of stack frames. Turning those off needs `CONFIG_EXPERT`, and with it a minimal config silently leaves out everything else
  EXPERT makes visible (pseudo-terminals, futexes, tmpfs and more), so the file lists all of those as kept. After changing it, compare
  the enabled options of the old and new `.config`.
- `linux/nommu/patch_memmap.py`: the boot's `struct page`s are made once and copied, and frees store only what changes.
  `bootpages_hook.py`: a page starts with the count the freeing wants, and a page that is kept gets its own when it is marked reserved. `patch_sysfs.py`:
  no sysfs for the boot's slab caches and built-in parameters (`docs/system/nommu.md`).
- The root device is sized to the image, not to 256 MiB.
- `linux/userland/emuinit.c` is the kernel's init, one static program. Build it with `linux/userland/build.sh`; the image builder makes
  it the init. `linux/userland/busybox.sh` builds the shell and tools against our musl.
- `linux/kernel/phram_hook.py`: the ROM driver reads through the machine's parallel copy (CSRs `0x0b0`-`0x0b3`, whose source may be the ROM).

## Passes that end early

A pass runs 8,192 instructions unless something ends it: a parallel copy or fill, `fence.i`, a full write cache, `wfi` or `pause`. What is
left of the pass is not run. `tools\tick_util.ps1` runs a workload on the card and prints its utilization (instructions run over what the busy passes could have
run) and the passes that ended early by reason; a pass that ended in the kernel's idle loop had nothing to run and is counted apart, and
so is one in which a program asked the kernel for a rest (`mcw_rest()`, the `REST` line: `multicore.md`, "A core that rests"); a program's
own `pause` is a busy pass cut short. The worker cores have a line of their own (how much of their passes the workers that had a job ran; those
awake with no job are counted apart). In the harness a short pass is also a short time, so what it costs there is a pass's own draws; in
the world's fixed mode it costs the instructions (`tick-costs.md`, "What a pass is made of").

Without the card, `RVC_PASSLOG=FILE` on `rvc_cpu` (`docs/machine/cpu-harness.md`) gives how each of core 0's passes ended. With `RVC_WCACHE=1
RVC_ADFAULT=1 RVC_SOUND=1`, 31 October 2026 (this count has idle passes among those that ended themselves):

| | passes | to their budget | ended themselves (their mean) | write cache full (their mean) | instructions of all |
|---|---|---|---|---|---|
| cold boot to the prompt | 933 | 831 | 66 (3,349), and 4 waits, 4 copies | 28 (5,357) | 94.0% |
| ten `ls` at the shell | 531 | 407 | 82 (2,124) | 42 (4,260) | 84.8% |
| `glxgears` on the desktop, 20 s, calibrated | 9,734 | 9,734 | 0 | 0 | 100.0% |

`linux/kernel/*_hook.py` take the kernel's own passes out, in `build.sh`'s order, and `linux/nommu/kernel.sh` puts them in the no-MMU tree. What
each does there:

- **`stall_hook.py`.** A fill under 2 KB is stored by the processor (`memset.S`) and does not end the pass. A file of the ROM is read
  ahead: a window's pages are allocated as one block in the file's order (`mm/readahead.c`), and romfs reads a run of whole pages that follow
  one another in memory with one call, which is one copy.
- **`romwindow_hook.py`.** A file of up to 32 pages read from its start comes whole, and its last, partial page rides in the same copy
  (`romfs_read_run`): whole texels in the copy, the last bytes and the zeros stored. Read by itself, a part of a page is stored by the
  processor (`phram_read` copies in the commit pass from a page up). A file mapped is mapped where it lies in the ROM, with no copy.
- **`romfs_hook.py`, `patch_romfs.py`.** A directory is listed and a name found where they lie in the ROM.
- **`poke_hook.py`.** A debugger's `POKETEXT` into another program sets a flag, and the `fence.i` is made at the next switch of tasks
  (`context_switch`). A program writing its own code flushes at once.
- **`unixwake_hook.py`.** A reader of a Unix socket sleeps through a wake that is only for room to write (`unix_data_wake`): a program that
  writes a question and sleeps on the answer has two switches of process fewer an answer (`docs/system/nanox.md`).
- `faultaround_hook.py` and `icache_hook.py` patch code the no-MMU kernel does not compile (`mm/memory.c`, `flush_icache_pte`).

A program starts by a `vfork` and a flat load (`docs/system/nommu.md`): no pages are copied at `fork` and none is faulted in. What is left is
a program's text and data being read and relocated (a full write cache: the `ls` above), and its `fence.i` (`fs/binfmt_flat.c`), which ends a pass.

### Memory cleared ahead (`zeropool_hook.py`)

A fill asked for while a program wants to run ends its pass; the same fill asked for by the idle loop ends a pass that had nothing left to
run. So the kernel keeps cleared memory:

- **The pool** (`zeroed_pages` in `mm/page_alloc.c`) is up to four blocks of a megabyte, each cleared by one fill, and one more being
  handed out. Any request of up to eight pages wanted zeroed is cut from it: pages for `vzalloc` and a new task's kernel stack among them.
- **The idle loop fills it** (`zeroed_idle`, called from `do_idle`, and from the GPU driver's rest call when a program's first core has
  nothing to do and nothing else can run): one block each time the loop is entered while the pool lacks any. The fill is asked for there and not in
  `memset`, so `tick_util.py` sees the pass end in the idle loop and counts it idle.
- **With the pool empty** (a boot, before anything was idle) the request clears a megabyte itself, or 256 KB or 64 KB when memory has no such
  block free, and hands out the rest.
- **Memory that is short** takes the blocks back through a shrinker, and the idle loop leaves them be for ten seconds; a block the idle loop
  cannot have is tried again a second later, with kswapd woken.
- **A short file of the ROM** (under a page) has its page from the pool, and only the file's bytes are copied in. A longer file's last page is
  cleared past its end in pieces `memset` stores itself when a copy has just ended the pass.
- **A device's announcement** (`kobject_uevent_env`): only the pointers before its text are cleared, not 4 KB.

`linux/kernel/zerotest.c` and `tailtest.c` check that what is handed out is zero: megabytes of fresh anonymous memory after memory of other
contents was freed, and every file's last page past its end. They are in no image: `python tools/test_all.py zero-tail --long` builds them
into a private image and runs them (copies of the files in `/tmp`: a mapped ROM file has the next file's bytes after its end). Both must say PASS.

### A fill or a copy that does not end the pass (`LAZY_MEMOP`)

Only for the tick with paging (`rvc_harness --lazy` compiles it when the machine is the full one, and `types.h` undefines it under `NO_PAGING`): the machine that
runs Linux does not have it. With it a parallel fill or a copy, from RAM or from the ROM, is taken down and the pass goes on; the commit makes it when the
pass ends and lays what the pass stored to its destination afterwards on top. Its destination's pages leave every TLB the fast path reads (`pend_unmap` in `emu.h`),
so only the general path reaches them (`mem_get_slow`, `pend_texel` in `mem.h`), and it is two a pass, whole texels, a source apart from the destination, core 0. With
it undefined the tick is the same blob, and it stays off because compiled in it costs every instruction ("The same loop, compiled slower" in
`experiments/rvc_opt/README.md`). `rvc_cpu` makes every fill and copy at once.

`programs\lazy` is its test, a program in the kernel's place (supervisor mode, paging on) that fills and copies in every order that could show, against
plain loops. `python tools\make_lazy_image.py DIR` makes its image folder; `python tools/test_all.py lazy` must print twenty lines with `ok` and `lazy: sum 1a4a1226`.

### The console (`console_hook.py`, `patch_console.py`)

`linux/nommu/patch_console.py` makes the console's driver (`hvc_riscv_sbi.c`) write a ring of 256 bytes in memory, which the host reads between two of
its clock word's values (`console.md`, "Words in memory"). The driver counts what it has put since the clock word changed and stops at 254. A write to the tty puts
what there is room for and keeps the rest in its buffer (1 KB), and the idle loop wakes the console's thread at the next pass
(`arch_cpu_idle_exit`): a program that prints is not stopped, and a pass that waits for the console is an idle one. The kernel's own messages are not put off: with no
room left they end the pass (`pause`) and go on in the next. Nothing of the machine changes for it, and it wants no call of the machine's own: a call added to `sbi_call`
costs every instruction (`experiments/rvc_opt/README.md`, "The same loop, compiled slower").

## What an operation costs in the tick shader

`tick-costs.md`. So the fast path has no large switch: arithmetic instructions compute every result and select one, and instruction classes are told apart by
a tree of tests. Cache entries are read a word at a time, addresses before values. An instruction that follows another in the same texel skips the fetch checks.
Atomics on RAM are on the fast path.

## Still open

With `--dxc` the harness compiles the tick shader with DXC on every start; there is no cache for it as there is for the D3D11 build's compiler (`fxc2.md`).
