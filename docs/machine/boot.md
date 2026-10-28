# Booting Linux

A cold boot of this project's image to the shell prompt takes about 2.8 s and 10.5 million
instructions on an RTX 5090 with DXC (it was 10 s and 35 million). On an RTX 5070 laptop with
D3D11 and fxc2 (19 October 2026, `docs/machine/speed.md`, `/proc/uptime` at the prompt): 5.0 s on the
shader machine, geotune's layout of the workers with it; 1.6 s on the compute machine, 1.1 s on the
CPU harness, and 107.6 s for upstream rvc's own image on its shader. `tools/boot_profile.py`
measures it: run the harness with `--frame-log FILE` and give it the file.

## No firmware

Upstream boots OpenSBI, which then starts the kernel and answers its calls. That was a third
of the boot's instructions. Our image has no firmware in it: with `SBI_HLE` (the harness
defines it for our image and for snapshots) the machine does the firmware's part itself:

- It starts at the kernel (`0x82000000`, `KERNEL_AT` in `src/layout.h`) in supervisor mode with the
  device tree's address in `a1`. The tree has to be in RAM for a 32-bit kernel, so
  `make_linux_image.py` and `image.py` put it at `0x83e00000` (`TREE_AT`).
- An `ecall` from supervisor mode is an SBI call answered in place (`sbi_call` in `emu.h`):
  set_timer, console putchar and getchar, and the base extension's version and probe calls.
  Anything else returns "not supported". Without the MMU (`CSR_FIELDS`, `docs/system/nommu.md`) the
  console calls are not answered: the console is memory the kernel writes itself
  (`console.md`, "Words in memory").
- The timer raises the supervisor's timer interrupt directly, and every trap is the
  supervisor's: nothing runs in machine mode. Without the MMU there is no CLINT: the timer is
  the host's time against set_timer's compare.

Upstream's own images still boot their OpenSBI: they are compiled without `SBI_HLE`.

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
- `linux/userland/busybox.sh` builds the shell and tools static against our musl, in place of
  upstream's dynamically linked ones: the boot runs 10.5 million instructions instead of 11.6,
  and a command starts in 75 ms instead of 175.
- `linux/kernel/phram_hook.py`: the ROM driver reads through the machine's parallel copy
  (CSRs `0x0b0`-`0x0b3`, whose source may be the ROM), and a commit with a copy now draws only
  the bands of RAM the copy writes (since then the rows of its destination: `multicore.md`,
  "The commit as points"). It changed little: 0.99 s to 0.89 s for a 5 MB cold read,
  boot the same. A page read costs about 2,000 instructions of page-cache work besides the copy.

## Passes that end early

A pass runs 8,192 instructions unless something ends it: a parallel copy or fill, `fence.i`, a
full write cache, 64 characters of console output (the machine with the MMU; without it the
console is a ring in memory: `console.md`), `wfi` or `pause`. What is left of the pass
is not run. `tools\tick_util.ps1` runs a workload and prints its utilization (instructions
run over what the busy passes could have run) and the passes that ended early by reason; a
pass that ended in the kernel's idle loop had nothing to run and is counted apart, and so is
one in which a program asked the kernel for a rest (`mcw_rest()`, the `REST` line:
`multicore.md`, "A core that rests"); a program's own `pause` is a busy pass cut short. In the
harness a short pass is also a short time (a pass is about 0.2 ms and 0.24 us an instruction),
so the cost there is the 0.2 ms; in the world's fixed mode it is the instructions. The worker
cores have a line of their own (how much of their passes the workers that had a job ran;
those awake with no job are counted apart).

`linux/kernel/stall_hook.py` takes the kernel's own out of three paths:

- **Zeroed pages.** A page wanted zeroed was one parallel fill, so one pass. The allocator now
  clears a block of sixty-four in one fill and hands them out (`zeroed_page` in
  `mm/page_alloc.c`); a page fault's zero page is asked for as one. A block that cannot be
  had is not waited for: the page is cleared the old way.
  tmpfs and shared memory, which cleared each new page themselves, ask for theirs the same
  way: the picture viewer on a PNG went from 237 fills to 14 (95.2% to 98.7%).
- **`fence.i` for a page of the ROM.** A program's page read from the ROM is written by the
  commit pass, which ends the pass: none of it waits in the write cache, so mapping it to run
  needs no `fence.i`. The ROM driver says when a read was a whole page copied so, and romfs
  marks the page (`PG_dcache_clean`). A file's last, partial page still gets its `fence.i`.
- **Small fills.** A fill under 2 KB is stored by the processor (some hundred instructions
  and at most a third of the write cache) instead of ending the pass.

| | utilization before | after |
|---|---|---|
| ten `ls` in a row | 77.9% (156 fills, 20 `fence.i`, 90 copies) | 90.0% (29, 1, 89) |
| the desktop coming up (`nx`) | 83.2% (176 fills, 131 `fence.i`) | 30 fills, 1 `fence.i` |
| cold boot to the prompt | 79.2% | 86.5% |

- **Reading ahead.** A file of the ROM was read a page at a time, a parallel copy and a pass
  each. The ROM's device now reads ahead (32 pages), the window's pages are allocated as one
  block in the file's order (`mm/readahead.c`), and romfs reads a run of whole pages that
  follow one another in memory with one call, which is one copy. `cat` of 4 MB, a 5 MB `cp`
  to `/tmp` and their `md5sum`: 89.4% to 95.7%, the ROM's copies 2,330 to about 75.

What is left of a program's start (90.8% with `romfs_hook.py` and `faultaround_hook.py` in
as well: 6 copies and 3 fills a start) is the pages a program writes of its own file (its
data) and after `fork`, each copied when it is first written, and the blocks of zeroed pages.
Running the child first after `fork` changed nothing. Each such copy has to be there when the
fault returns, so it cannot be put off; several could only share a pass if the machine took
more than one copy a commit.

### Memory cleared ahead (`zeropool_hook.py`)

A fill asked for while a program wants to run ends its pass; the same fill asked for by the
idle loop ends a pass that had nothing left to run. So the kernel keeps cleared memory:

- **The pool** (`zeroed_pages` in `mm/page_alloc.c`) is up to four blocks of a megabyte, each
  cleared by one fill, and one more being handed out. Any request of up to eight pages
  wanted zeroed is cut from it: a page fault's page, page tables, tmpfs, pages for `vzalloc`,
  and a new task's kernel stack (two pages, a fill of its own at every `fork` before).
- **The idle loop fills it** (`zeroed_idle`, called from `do_idle`, and from the GPU driver's
  rest call when a program's first core has nothing to do and nothing else can run): one
  block each time the loop is entered while the pool lacks any. The fill is asked for there and not in `memset`,
  so `tick_util.py` sees the pass end in the idle loop and counts it idle.
- **With the pool empty** (a boot, before anything was idle) the request clears a megabyte
  itself, or 256 KB or 64 KB when memory has no such block free, and hands out the rest.
- **Memory that is short** takes the blocks back through a shrinker, and the idle loop
  leaves them be for ten seconds; a block the idle loop cannot have is tried again a second
  later, with kswapd woken.
- **A short file of the ROM** (under a page) has its page from the pool, and only the file's
  bytes are copied in: the rest of the page was cleared by some 900 stores, for every
  shortcut on the desktop. A longer file's last page is cleared past its end in pieces
  `memset` stores itself when a copy has just ended the pass.
- **A device's announcement** (`kobject_uevent_env`) cleared 4 KB for its text, 109 fills in a
  boot: only the pointers before the text are cleared now.

`linux/kernel/zerotest.c` and `tailtest.c` check that what is handed out is zero: megabytes of
fresh anonymous and tmpfs memory after memory of other contents was freed, and every file's
last page past its end. They are in no image: build them with `rv32-cc -O2` into a private
image's overlay (`tools/private_image.py`) and run `zerotest 12 4; tailtest /usr/share/*
/usr/bin/* /etc/*` on `rvc_cpu` after a change to the pool: both must say PASS.

`bootpages_hook.py`: the boot set every page of memory up as used and then, to free it, wrote
its count again, a store to each page's own 16 bytes: the write cache was full sixty times.
A page starts with the count the freeing wants, and a page that is kept gets its own when it
is marked reserved.

`icache_hook.py`: a page the kernel wrote (a program just compiled into `/tmp`) is mapped to
run with a `fence.i`, a pass each. The `fence.i` ends the pass and so commits every store there
is: the sixteen pages of the file round that one are marked clean with it. `cc` on the hello
example: 7 to 2.

`romwindow_hook.py` and `poke_hook.py` (12 October 2026; in `build.sh` since, first checked
in a private kernel on `rvc_cpu`, and their parts on the shader machine):

- **A file's reads of the ROM are whole stretches.** The kernel's windows began at four pages
  and lay round the page wanted: the editor's 31 pages came in five copies (0-3, 4-11, 13-29,
  the last page by itself, then 12). A page wanted of a mapped file, or of a program being
  started (`__FMODE_EXEC`), brings the 32 pages it is in, aligned (`rom_window` in
  `mm/readahead.c`); a file of up to 32 pages read from its start comes whole; other reads
  (a program's icon at its file's end, a lump of a large file) are the kernel's as before.
- **A file's last page rides in the copy of the pages before it** (`romfs_read_run`): whole
  texels in the one copy, the last bytes and the zeros stored. Read by itself, a part of a
  page is stored by the processor (`phram_read` copies in the commit pass from a page up):
  some 1,300 instructions instead of the rest of the pass.
- **A debugger's pokes share one `fence.i`**: `POKETEXT` into another program notes it, and
  the `fence.i` is made at the next switch of tasks (`context_switch`).
- The shell starts `command &` with `vfork` too when its words are names only
  (`linux/userland/ash_vfork_hook.py`): the fork's five copied pages are gone.
- **A reader of a Unix socket sleeps through a wake for room to write**
  (`linux/kernel/unixwake_hook.py`, 13 October 2026; `unix_data_wake` in `net/unix/af_unix.c`).
  A program writes a question and sleeps reading the answer; the other side taking the
  question freed the socket's room, which woke the reader for nothing: two switches of
  process an answer, 1,400 instructions of the window system's 12,500 (`docs/system/nanox.md`).
  `zerotest`, `tailtest` and `mctest` say PASS with it, `nxray` 255 workers.

Copies from the ROM, `rvc_cpu`: the editor's start 6 to 2, Settings' 8 to 3, the desktop
with a terminal and the calculator 50 to 31 (before lone parts of pages went to the
processor: 13 of the 31 were programs' icons), each in fewer instructions. Passes' instructions used on the shader machine, the kernel
alone: that desktop 96.1% to 98.6%; the IDE's debug script 47 `fence.i` to 34.

| | before | after | what ends passes early now |
|---|---|---|---|
| cold boot | 87.8% | 97.0% | the console 43, copies 15, fills 15 (173), full cache 16 (79) |
| ten `ls` typed at the shell | 90.4% | 93.5% | copies 69, fills 0 (22) |
| the desktop coming up with a terminal and the calculator | 92.4% | 95.5% | copies 112 (146), fills 6 (38), pauses 8 (42) |
| `nxbench` | 78.8% | 96.4% | pauses 8 (118) |
| `glxgears` on the desktop | 92.6% | 100.0% | nothing |
| `cc` on the hello example | 90.4% | 91.6% | copies 128, full cache 69, `fence.i` 2 (7), fills 1 (11) |

(The pauses of `nxbench`, the desktop and `glxgears` are the window system's and the OpenGL
library's: `nanox.md`, `gpu.md`.) What is left under 95%:

- **The shell's `fork`**: six copies a command typed at the shell (three pages of the C
  library's heap, one of the stack, one of the shell's data, one of `brk`), and the new
  program's first data page. Each has to be there when its fault returns. A program the
  desktop starts has none of the six (`vfork`).
- **`cc`'s single pages of the ROM**: the compiler maps its headers and libraries with
  `MADV_RANDOM` (`linux/tcc/mapped.c`), and a fault reads one page, a copy and a pass. A window
  of 4, 8, 16 or 32 pages for such faults was tried: 8 to 20 fewer passes of 93 for 70 to 325
  thousand more instructions (the 57 pages `libc.a` gives are spread over 754, and a page
  into the page cache is 2,000 instructions). A page read where it lies in the ROM would cost
  no copy, but a load from the ROM is the tick's general path, three times a load from RAM.

### A fill or a copy that does not end the pass (`LAZY_MEMOP`)

Off unless the shader is compiled with `LAZY_MEMOP` (`rvc_harness --lazy`, D3D11). With it a
parallel fill or a copy, from RAM or from the ROM, is taken down and the pass goes on; the
commit makes it when the pass ends, and lays what the pass stored to its destination
afterwards on top.

- Two a pass. A third, and one that cannot be left (below) while another waits, end the pass
  without being made and are asked for again when the next begins (the instruction runs again).
- The fast loop knows nothing of it. When one is taken down, its destination's pages leave
  every TLB the fast path reads (`pend_unmap` in `emu.h`: the first level, the megapages that
  hold them, the second level's entries of this pass and those the last one left), and a
  page walk puts no such page back and no megapage either while one is pending. So only the
  general path reaches a destination, and it reads it as its result (`mem_get_slow`,
  `pend_texel` in `mem.h`: the cache's texel if there is one, else the fill word or the
  source's texel as the texture, or the ROM, has it) and makes cache entries from the same.
- It is only left to the commit when nothing can tell: whole texels, a source in RAM apart
  from the destination, neither with a texel in the write cache when it is asked for (the
  texture is then the source as it is, and stays so for this purpose whatever is stored to
  it later), paging on, core 0.
- A page being run is never a destination, and a page that becomes one is not run before the
  commit: the fetch finds it in no TLB, and the general path ends the pass there, with that
  instruction still to run. The kernel may map a page of the ROM to run without `fence.i`
  (above) for the same reason as before.
- What the commit needs is in words of the state that only counted things nobody read
  (`debug_arb_0`, `trap_count`, `debug_last_ins` for the first; `stall_count`, `memop_dst_p`,
  `memop_src_p` for the second: `decode()` in `types.h`).
- The kernel knows nothing of it: the same CSRs. `rvc_cpu` makes every fill and copy at once.

`programs\lazy` is its test: a program in the kernel's place (supervisor mode, paging on) that
fills and copies in every order that could show (then read, stores before and after, bytes,
two and three in a row, a copy of a pending destination, a copied page of code run at once,
a destination reached through small pages mapped before, a page table that is one, a copy
from the ROM) against plain loops. `python tools\make_lazy_image.py DIR` makes its image
folder; it must print twenty lines with `ok` and `lazy: sum 1a4a1226` on `rvc_cpu`, and on
the shader machine without `--lazy` and with it.

| the image of 12 October, 10:36 | off | on |
|---|---|---|
| ten `ls` | 90.4%, 624 passes | 98.5%, 574 |
| the desktop coming up with a terminal and the calculator | 93.2% | 98.2% |
| cold boot | 87.9% | 94.4% |
| `nxsettings`, `nxedit` | 92.5%, 93.4% | 96.5%, 98.1% |
| `cc` on the hello example (single pages of the ROM) | 90.9%, 1,139 passes | 97.4%, 1,065 |
| a shell loop, instructions a second | 3,582k | 3,540k |
| Doom's timedemo, instructions a second | 3,059k | 2,964k |

It is off because of the last lines, 1 to 1.5% of every instruction of a shell loop and 3%
of Doom's,
and that is not the code's own cost: with it off the tick is the same blob as without the
code. The first version had 324 instructions of the fast
loop's 3,239 and cost 2.4%; with this one the loop's bytecode is the tree's, instruction for
instruction (2,915; `tools\cso_loops.py` disassembles a cached blob and compares loops with
registers renamed), the commit pass takes 0.020 ms either way, and cutting the purge, the
cache scan or the whole reading side out changes nothing (0.8 to 1.2% under, all of them).
The card's driver compiles the same loop about 2.7 ns an instruction slower inside the
larger function. The tree is on the good side of that: taking work out can cost as much
(the registers decoded straight into the tick's array: 1.2% under; the console's 64 fields
as an array: 3.1% under; three state words written in one rare branch: 2.0% under), and so
can nothing at all (the timer's value no longer written at every general tick: level). Code
added after the loop in the source, or one more local before it, costs nothing. A host
that runs a fixed number of passes a frame gains more from `--lazy` than it loses.

### The console (`console_hook.py`)

The console's buffer is 64 characters a pass, and the character that fills it ends the pass.
`linux/kernel/console_hook.py` has the console's driver (`hvc_riscv_sbi.c`) stop before that:
it counts what it has put in this pass and stops at 62 (the timer's value is one pass's, so a
new value is a new pass with room again). A write to the tty puts what there is room for and
keeps the rest in its buffer (1 KB, it was 16 bytes), and the idle loop wakes the console's
thread at the next pass (`arch_cpu_idle_exit`): a program that prints is not stopped, and a
pass that waits for the console is an idle one. The kernel's own messages are not put off:
with no room left they end the pass (`pause`) and go on in the next. Nothing of the machine
changes for it.

A boot followed by `seq 1 3000`, a `cat` and `dmesg` (25 KB): the console's log the same byte
for byte apart from times, on `rvc_cpu` and on the shader machine; passes the console ended
92 to 51 (30 before the driver is there, 21 the kernel's own messages), the run as long as
before. Two calls of the machine's own for this (put a character if there is room, and say
so) were tried first and taken out: level on a shell loop, 1.9% under on Doom's timedemo
(`experiments/rvc_opt/README.md`, "The same loop, compiled slower").

The kernel without an MMU has no such buffer: `linux/nommu/patch_console.py` makes the same
driver write a ring in memory, at most 255 bytes a pass (`console.md`, "Words in memory").

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
- With `--dxc` the harness compiles the tick shader with DXC on every start (2.3 s); there
  is no cache for it as there is for the D3D11 build's compiler (`fxc2.md`).
