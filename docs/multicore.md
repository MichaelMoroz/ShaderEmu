# More than one core

The machine with worker cores beside it, all run by the one tick pass. Linux on core 0 starts
the workers and gives them work, and every result is right. Core 0 and three workers cost
1.17 times the pass of core 0 alone, so four cores do 3.4 times the work of one; past that a
pass grows with the cores (eight: 2.1 times, sixteen: 3.2 times). It took two things to get
there, both about how the GPU runs pixels: the state as full tiles of pixels, and workers that
keep no more than they need ("What a pass costs" and the sections after it).

    bin\rvc_harness.exe --d3d11 --image linux-net --cores 4
    / # mctest            (3 workers; "mctest N LIMIT" for other numbers, "mctest N LIMIT bench" for the cost of a pass)

`--cores N` compiles the shader with `CORES=N` (D3D11 backend). Without it nothing changes: the
default build is the one-core machine (the code is under `#if CORES > 1`; its tick time and
the Linux checksums are as before).

## How it is made

- **A block of state a core.** Core 0's state is the 64 texels wide block it always was. Core k's is
  a block `CORE_PITCH` (256) texels further right in the state rows, which were empty there.
  `STATE_TEX` adds the block's place (`state_off`); RAM is read with `RAM_TEX`, which nothing
  moves. The tick pass draws core 0's rectangle and two small zones a worker (below), all
  before any is copied back.
- **The same RAM.** Every core reads RAM as the last commit left it, plus its own writes (its
  write cache). The commit pass looks a RAM texel up in every core's cache, the highest core
  last. So a core sees another's stores one pass later.
- **Sixteen bytes, one writer.** The write cache holds whole texels: a core that stores one word
  carries the texel's other three as it read them, and would undo another core's store to them
  in the same pass. What changes hands is therefore 16 bytes with one writer a pass. There are
  no atomic instructions between cores and no locks; `mc.h` (programs/mc) is the convention.
- **Workers** have no devices and take no traps; they have the float registers and paging.
  A worker is a thread of a Linux program that the kernel does not know of: it runs in user
  mode on the program's own page table, so the program's functions and pointers are its own.
  It is parked until core 0 writes its start word, the texel at `0x86C00000 + 16 k`:
  `{ "MCSU", pc, sp, root }`, the page table's first page being number `root` (the kernel
  tells a program its own: an ioctl of `/dev/gpu`). It starts there with its number in `a0`.
  `pause` is its sleep until the first word of its job changes, and `ebreak` parks it again.
- **A trap stops a worker where it is.** There is no kernel on a worker to take one, so the
  machine writes `{ count, cause, address, pc }` into the worker's mailbox and the worker
  waits. A page the program has not touched yet is the usual cause: the library, on core 0,
  touches the page (which the kernel answers as it would for the program itself) and writes
  the count back, and the worker runs the instruction again. So a job cannot make a system
  call, and anything else that traps ends the program with a line saying where.
- **No TLB kept.** A worker starts every pass with empty TLBs and walks the page table for
  each page it uses in the pass ("What paging costs a worker", below).
- **The mailboxes** are one page of the GPU device's memory at `0x86C00000`, which `/dev/gpu`
  maps: the start words, and 64 bytes a core (its job, its answer, its fault and the word
  that lets it go on). `programs/mc/mc.h` is the layout.
- **A program that ends** has its workers parked by the kernel (the driver's release writes
  "MCST" into the start words of the cores it had): a worker must not go on in memory that is
  being given away.
- **The kernel hands the cores out** (an ioctl of `/dev/gpu`: so many wanted, a bit a core
  given, and the page table's root): several programs can have workers at once, a game one
  and the window system another, and a core is free again when its program closes it or ends.

At exit the harness prints a line a core: its pc, instructions and whether it runs or what
parked it.

## The test (`programs/mc`)

`mctest.c` is a Linux program whose own functions are the jobs: it has the library start the
workers and checks them (`programs\mc\build.bat`, then `python tools\make_linux_image.py`):

| Test | What it shows |
|---|---|
| primes | a count split four ways equals the count on core 0 alone |
| fill | each worker fills 64 KiB of its own; core 0 reads every word (18 passes: the write cache is 6 KiB, and the buffer's pages are new) |
| stripe | three workers fill one buffer, every third 16 bytes each: no store undoes a neighbour's |
| sum | each worker reads what the other two wrote |
| pages | a worker writes and reads 64 pages nobody has touched: 64 stops, each answered by core 0 in two passes |
| floats | float arithmetic kept in registers over many passes gives what it gives on core 0 |
| park | `ebreak` parks them; the next run starts them again |

All pass. With four cores and the workers parked, Linux boots in the same number of
instructions as with one and the same checksums, at the same speed.

## What a pass costs

`mctest 3 300000 bench M`, the tick pass's GPU time at 16,384 instructions a pass (RTX 5070
Laptop, D3D11 + fxc2):

| Busy | A zone a core, 64 apart | 256 apart | One draw, a quad a core, 512 apart | Workers 4,032 rows down |
|---|---|---|---|---|
| core 0 | 3.42 ms | 3.17 | 3.53 | 3.53 |
| one worker | 3.78 | 3.55 | 3.84 | 3.81 |
| core 0 and one worker | 6.77 | 6.49 | 6.35 | 6.93 |
| three workers, the same work | | | 7.35 | |
| core 0 and three workers | 10.53 | 10.43 | 10.16 | 10.47 |

- **Busy cores add up.** Two busy cores cost two passes, four cost three. How far apart the
  blocks are, in which rows, and whether they are one draw or several makes no difference, so
  it is not pixels of two cores sharing a warp (a block is 64 x 64 and the nearest other one
  64 to 512 pixels away).
- **Cores that do the same work add up too**: one, two, four and seven workers on one job
  (core 0 waiting) are 3.8, 7.4, 11.8 and 17.1 ms a pass. It is not what the cores do that
  costs, it is that there are more of them.
- **One wide zone over every block is worse**: a lone worker then costs 6.85 ms, twice core 0
  alone, and blocks 256 apart 5.1 ms with only core 0 busy (`RVC_MC_ONE_ZONE=1`).
- The commit pass grows from 0.12 to 0.23 ms with four busy cores.

So in wall time the primes test is 12.3 s on core 0 alone and 9.4 s with three workers, where
the count of passes says 3.2 times fewer.

## Why: how many pixels the GPU runs at once (`tools/warpbench`)

A program of its own, with no emulator in it: one pixel shader with two unrelated loops (an
interpreter of sorts on integers, with registers, a local array and a branch every step; an
iteration on floats), drawn as rectangles with a kind and a seed each, sized so that one
64 x 64 rectangle takes 3 ms. Same seed: the same work; another seed: the same code on other
data; the other kind: other code. `tools\warpbench\build.bat`, then `bin\warpbench.exe`
(`--only size|pair|count|shape`, `--live N`, `--arr N`). On the RTX 5070 Laptop:

- **What the rectangles do does not matter, once they are 8 pixels apart.** Two 64 x 64
  rectangles cost 1.04 to 1.28 times one, whether the second has the same seed, another, or is
  the float loop; right, down, diagonal, 8 or 2,048 pixels away, one draw or two. Only two
  8 x 8 rectangles of different kinds that touch cost the sum (2.4 times): there pixels of both
  are in one warp. A seed a pixel, every pixel on its own path, costs 1.6 times.
- **There is a number of pixels the GPU runs at full speed, and past it the time is the
  pixels'.** One rectangle is flat to 64 x 64 (4,096 pixels); 128 x 128 is 1.9 times, 256 x 256
  5.6 times, 1,024 x 1,024 72 times. Eight 64 x 64 rectangles are 3 times one, thirty-two 9
  times. Sixty-four 8 x 8 rectangles (4,096 pixels) are 1.6 times one.
- **The number falls with what a pixel keeps in registers.** `--live N` gives the interpreter
  N more values of four words that every step may change:

  | Live values more | Two 64 x 64 | Four | Eight | One 128 x 128 |
  |---|---|---|---|---|
  | 0 | 1.24x | 1.77x | 3.14x | 1.85x |
  | 16 | 1.30x | 2.05x | 3.91x | 2.43x |
  | 64 | 1.90x | 3.06x | 6.01x | 3.05x |
  | 128 | 2.15x | 3.50x | 6.41x | |

  With 64 of them a second rectangle costs a second pass, which is what a second core costs
  the emulator: its tick keeps some hundreds of values and two write caches' worth of local
  arrays in a pixel.
- **The size of the local array does not matter** (16 to 2,048 words: the same figures).

So the tick's pixels are too heavy for the GPU to run two cores' worth of them at once, and
no placing of the blocks changes that. What would: a worker whose shader keeps little (no
paging, no CSRs, no float registers, a small write cache), in a pass of its own beside core
0's. Two draws do run side by side (the pairs above, "a draw each"); whether a light draw
runs beside a heavy one at no cost to it is the next thing to measure.

The earlier figure that 4,096 pixels of one block running the whole tick cost only 4 to 8%
more than 973 does not fit this and is not to be relied on: those extra pixels' results were
thrown away, and the driver may have dropped most of their work.

## The state as one rectangle

Idle pixels of a warp take their share: in `warpbench --live 64 --only shape`, 4,096 busy
pixels as four 32 x 32 blocks, 128 x 8 strips or 8 x 128 columns cost 1.13 times one block,
as four lines of 1,024 x 1 they cost 2.17 times. And the tick's 973 texels were four bands of
a 64 x 64 block (the first 44, the write cache from row 16, the TLBs from row 33, the float
registers in row 39), which the pass drew all 4,096 pixels of.

They are now one 64 x 16 rectangle at the top of the block, with the CSR area in the rows
under it, and the tick pass draws the rectangle only (`STATE_ROWS` and the layout at the top
of `src/types.h`). The first 44 texels did not move. There is no other layout: a snapshot
from before does not load, and the Unity side's tick texture has the same rows ("In Unity").

| | Four bands of 64 x 64 | One rectangle |
|---|---|---|
| One core: tick at 2,048 instructions a pass | 0.539 to 0.548 ms | 0.494 to 0.515 ms |
| One core: a Linux boot and the checksum run (606,105,379 instructions) | 3,259k a second | 3,478k to 3,534k |
| Core 0 busy, of four | 3.2 to 3.5 ms a pass | 3.08 |
| Core 0 and one worker busy | 6.4 to 6.9 | 3.66 |
| Core 0 and three workers busy | 10.2 to 10.5 | 6.60 |
| `mctest 3 240000`, primes alone and with three workers | 12.3 s, 9.4 s (1.3 times) | 11.7 s, 6.0 s (1.9 times) |
| `mctest 1 240000`, with one worker | | 11.6 s, 7.8 s |

The same instructions run and the guest's checksums are the same, on D3D11 and (from the new
snapshot) on D3D12. So the one core is 5 to 9% faster for it and a second core comes almost
free; a third and a fourth cost a pass between them again, so **two cores are what makes
sense as things are**. What limits it from there is the number of pixels a core has: a worker
without the TLBs' 409 texels and with a smaller write cache would be half the size or less.

## Workers that keep only what they need

A worker keeps no TLBs from one pass to the next, so the 409 texels of the TLBs were 42% of
its pixels running the whole tick to write back what it does not need. It keeps the CPU's 44
texels, the write cache's 512 and the float registers' 8 and nothing else, and they are
laid out as whole tiles, which is how the GPU runs pixels: the first 512 where they are
(64 x 8), and the cache's last 44 and the float registers in a block of 13 x 4 under those, four to a column
(`MC_ROWS`, `mc_state_at()` in `src/types.h`). The pass draws two zones for a worker, 64 x 8
and 16 x 4: 576 pixels for 556 texels. (As a row of their own the 44 would be a line one
pixel high, which the shape sweep above says costs twice its pixels.)

The tick pass's GPU time at 16,384 instructions a pass, the workers on one job:

| Busy | Workers as 64 x 16 | Workers as 64 x 8 and 16 x 4 |
|---|---|---|
| core 0 | 3.08 ms | 3.22 |
| core 0 and one worker | 3.66 | 3.38 |
| core 0 and three workers | 6.60 | 3.76 |
| one worker (core 0 waiting) | | 3.23 |
| three workers | | 3.67 |
| seven workers | | 6.90 |
| fifteen workers | | 10.34 |

And `mctest N 480000` in real time, the primes below 480,000 on core 0 alone and then shared
(the shares are of numbers, not of work: the last one is the longest):

| Workers | Alone | Shared | |
|---|---|---|---|
| 3 | 31.1 s | 11.3 s | 2.7 times |
| 7 | 32.4 s | 9.6 s | 3.4 times |
| 15 | 35.3 s | 7.6 s | 4.7 times |

So three workers come almost free and are the place to be; more still gain, less and less,
and the parked ones cost core 0 a little (31 to 35 s alone: the commit pass looks a RAM texel
up in every core's write cache). With more than eight cores the blocks have to be closer
than 256 texels: `--core-pitch 128`.

## Workers with nothing to do

A worker's `wfi` is a sleep until the first word of its job changes, and the machine does not
run a worker that is asleep or parked: each of its pixels reads its state word and its
mailbox and returns what it holds, before anything of the CPU is decoded (`mc_idle()` in
`src/mc.h`), and the commit pass passes over a core that stored nothing. Three parked workers
were 4.5% of a pass and are now within what two runs differ by (0.505 and 0.519 ms against
0.502 and 0.505 for one core, 2,048 instructions a pass); Red Alert's mission with its three
workers asleep runs at 3,826k instructions a second where it ran at 3,576k with them awake
and waiting, and at about 3,900k on one core.

(A quad a core that the vertex shader throws away would save those pixels too, but what a
pass does not draw is two passes old in the buffer it draws into, here and in Unity: the
copy back, or the commit that reads it, would have to know.)

## Using the workers from a program

`programs/mc` is everything a Linux program needs, and is the same for every program:

| File | What it is |
|---|---|
| `mc.h` | the mailboxes: what the machine, the library and a worker agree on |
| `mcw.h`, `mcw.c` | the library a program links: the workers' start and stacks, jobs, the wait, the pages a worker stops at |
| `mctest.c` | the test |

A job is a function of the program and two words for it:

    static uint32_t sum(uint32_t words_at, uint32_t count) {
        const uint32_t *p = (const uint32_t *)words_at;      // the program's own memory
        ...
    }

    int n = mcw_open(3);                                      // how many workers there are: 0 without --cores
    for (k = 1; k <= n; k++) mcw_post(k, sum, (uint32_t)part(k), words(k));
    ...this core's own share...
    for (k = 1; k <= n; k++) { mcw_wait(k); total += mcw_result(k); }
    mcw_close();                                              // before leaving: atexit(mcw_close) will do

`mcw.c` uses nothing of the C library (it makes its system calls itself), so it links into a
program with any runtime: `programs/mc/build.bat` and `linux/ralert/build.sh` show two (clang
with the small runtime, and the image's toolchain with musl).

What a program has to keep to:

- **A job computes, reads and writes memory, and returns.** No system calls, and nothing
  that makes one: `printf`, file reading, a `malloc` that has to ask the kernel for more. The
  C library is not made for a second thread it does not know of either (its thread pointer
  is shared): allocate before the job, print after it.
- **A store is seen by the other cores one pass later.** `mcw_post()` followed by this core's
  own work is the usual order: the pass that ends next carries the job and its data over
  together. `mcw_wait()` ends passes until the worker has answered. Code that both cores run
  at once must not expect to see the other's stores at all.
- **Sixteen bytes, one writer a pass**, and this holds for all of the program's memory now:
  two variables side by side are one texel. Give a worker whole texels to write: rows of a
  picture whose width is a multiple of 16, buffers from `mcw_alloc()` or `posix_memalign(16)`
  rounded up to 16, its own stack. Globals that a job writes want `aligned(16)` and a size
  of whole 16s.
- **Memory the program has not touched yet stops a worker** until core 0 looks
  (`mcw_wait()`, `mcw_done()`): two passes a page. Touch a job's buffers first where that
  matters (`workers_alloc` in `linux/ralert/workers.c`).
- **A worker keeps 6 KB of stores a pass**, as core 0 does, and runs on until its job is done
  however many passes that takes. For work that is all stores, the cores' number is the gain.
- **Ask for what is needed, not for all.** `mcw_open(n)` gives up to n of the cores no other
  program has, and they are that program's until it closes them: a program that takes every
  core leaves none for the next. It may get fewer than it asked for, or none, and does the
  work itself then.
- **No workers is the common case to allow for**: the harness without `--cores` has one core.
  Every use has to fall back on the program doing the job itself.

What it is good for, and what not:

- **Work that is the same for many pieces of data and writes much**: decoding, unpacking,
  filling, scaling, mixing. Red Alert's movies are the example (below): rows of a frame to each
  core, and the next frame unpacked ahead on a worker of its own.
- **A pipeline a frame ahead**: what the next step needs is begun on a worker while this core
  does the present one, and waited for only when it is needed (`workers_lcw` in
  `linux/ralert/workers.c`).
- **A part of a program's own logic**, since a worker has the program's memory: a step that
  can run a frame behind or ahead of the rest, on data the rest leaves alone meanwhile.
- **Not small jobs.** Giving a job out and taking the answer costs a pass or two (some
  milliseconds): below some tens of thousands of instructions the core does it sooner itself.

What it costs: nothing to speak of while the workers sleep; 5% of a pass for the first busy
one and 17% for three; and the commit pass looks a RAM texel up in each busy core's cache.

## What paging costs a worker

Nothing in pixels. A worker that runs a program's own code needs the program's page table,
and the question was whether it then needs core 0's 409 texels of TLBs, which would make it
a full-size core again (17% of a pass for three thin workers, a second pass for three full
ones). It does not: core 0 itself made to forget its TLBs at the start of every pass (an
experiment, not kept: the second level ignored and the megapage entries cleared in
`tlb_state_load`) runs a Linux boot no slower, 0.476 ms against 0.548 and 0.625 for 2,048
instructions a pass and 2.91 ms against 3.01 and 3.06 for 16,384, with the same instructions
a pass. A page's first use in a pass is a walk of two table entries, and a pass touches few
pages. So a worker keeps the CPU's texels, the write cache and the eight texels of float
registers, 564 in the 576 pixels of its two zones, and starts every pass with empty TLBs
(`tlb_state_load` in `src/mmu.h`). There was a kind of worker without paging before this
one, which ran bare code in an arena of its own; it is gone, and nothing was lost in speed:
`mctest`'s figures and Red Alert's movie are the same.

## A use: Red Alert's movies

`docs/ralert.md` ("Movies, and the worker cores"; `linux/ralert/workers.c` over the
library above): the opening movie's frames are decoded by
core 0 and three workers into a picture the GPU shows, and played in 18.6 s where they took
41.4. It is the kind of work the workers are for: every pixel stored once, and a core keeps
only 6 KB of stores a pass. The window system's pool of window buffers ends below the
mailbox page (`POOL_SIZE` in `linux/nanox/scr_shaderemu.c`).

## A use: Quake's server

`docs/quake.md` ("The server on a worker core"; `linux/quake/server_shaderemu.c`): a part of
a program's own logic on a worker, which only a worker in the program's memory can have. The
server's physics and QuakeC run on a worker as the function they are, and the client draws at
its own pace from what the server last sent: 12.6 frames a second where the game drew 4.85.
What it took is the list of what two cores have in common in a program written for one:

- console output, commands and settings made by the server's code (kept, and done by core 0);
- errors that leave by `longjmp` (caught on the worker, raised on core 0);
- the C library's `rand` (the server has its own);
- a time step both sides read (the server has its own);
- variables side by side in one 16 bytes (every variable its own section, aligned to 16);
- commands typed at the console that reach into the server (they wait for its frame).

And a way to know it is right: the same job run on core 0 in the same order (`QUAKE_SERVER=late`)
gives the same sums over the server's state as on the worker.

## Where workers pay, and where they do not

Every program of the image was looked at for work a worker could take (October 2026). What
decides it is the pass: a core sees another's stores one pass later, and a pass is 16,384
instructions in the harness (8,192 in the world).

- **A job given out and waited for costs about a pass at each end.** The worker begins in
  the pass after the one the job was posted in, and the core that waits sees the answer in
  the pass after the one it was finished in. Splitting a piece of a frame between cores
  ("this half of the map's cells on a worker") gains the piece less up to two passes: for a
  game whose whole frame is ten passes that is nothing. Shorter passes would mend it and
  cost everything else: at 4,096 instructions a pass one busy core is 12% slower and four
  are 8% slower than at 16,384.
- **What pays is work that needs no answer in the same frame.** Two parts of a program each
  at its own pace (Quake's server and client), work begun ahead of its use (the next movie
  frame unpacked), or one long job shared out (a movie frame's rows; loading).
- **And the two sides must have their own data.** A game whose drawing reads and marks the
  objects its logic moves cannot have the two on different cores.

| Program | A frame | What a worker has, or why none |
|---|---|---|
| Quake (e1m5) | 515 thousand instructions, 4.85 frames a second | the server: 13.3 frames a second (`docs/quake.md`) |
| Red Alert | 220 to 240 thousand: logic 110 to 127, drawing 88 | its movies (three workers, given back when the movie ends). In a mission the drawing is the game's own, object by object, and clears the marks the logic sets; the profile has no single piece over 12% |
| Tiberian Dawn | 170 to 230 thousand: logic 100 to 145, drawing 45 to 70 | none: the same game |
| Doom | a tic 33 to 39 thousand, 35 a second; a frame 45 to 55 thousand | none: the renderer walks the things and sectors the tics change, and writes into them |
| ClassiCube | 60 thousand, 48 frames a second | none while playing. Its start is 133 million instructions, of which making the world is 40 and building its meshes 40: jobs for workers, not done (the mesh builder has one set of buffers, and the world's maker allocates as it goes) |
| The window system | | none: see below |

Loading is where the long jobs are. Quake's start to a level's first frame is 160 million
instructions (55 s), with no piece that is the server's or the client's alone: the models'
skins filled round their edges 10%, places found for the light maps 9%, the models' triangle
strips 7%, the level made for the GPU 5.5%. The first two are loops to make cheaper on one
core before any is shared out; a model's skin and strips are a job a worker could have while
the next file is read.

**The window system.** Composition is the GPU's already, and a drawing request is mostly not
computing: of the frames of the machine while `nxbench` draws (25 fills, 100 lines of text, 15
scrolls, 200 buttons: 14 million instructions in 6.4 s), 46% are the processor idle, waiting
for the GPU to have drawn a list; the kernel is 26% (sockets, the scheduler, timers) and the
server and the program together 28%. A worker makes no system calls, so the kernel's part
is not its to take, and the wait is a pass of the machine whoever waits.

## What the cores did, for the guest and the host

The control pass publishes each core's count of instructions among the machine's control
words (`MC_STATS` in `src/gpu.h`), where a program reads them through `/dev/gpu` like any
other:

| Address | Contents |
|---|---|
| `0x87000380 + 4 k` | core k's instructions so far (k from 0 to 15; 0 for a core the machine lacks) |
| `0x870003c0` | how many cores the machine has (0 on a machine built for one) |
| `0x870003c4` | a bit a core: it runs (a worker that was started and has not parked) |
| `0x870003c8` | a bit a core: it is asleep on its job word |

`nxmon` reads them: the workers' instructions a second are stacked on the processor's plot
and each worker has a figure, or "asleep", "parked" or "idle", in the line under the busiest
programs. The harness adds them up from the rows it reads back anyway: with `--cores` its
`STATS` and closing lines say what the workers ran and the rate of all cores together.

## In Unity

The VRChat machine has four cores (`CORES` in `MachineBlit.cginc`, for all three passes).

- **The tick's texture** is 832 x 16: the state rows' own places, core k's block 256 texels
  right of the one before. A geometry shader puts core 0's 64 x 16 and a worker's 64 x 8 and
  16 x 4 in place of Blit's two triangles, so the pixels between are not run.
- **The commit** takes a texel of a core's block from that texture where the tick drew it
  (`state_after_tick`) and merges every core's writes into RAM, as the harness's does; the
  bands it draws are every core's.
- **The readback** has the first 64 control texels (it had 48), which is where the counts
  are: the panel's statistics have a line for the workers and for all cores together.

`mctest` passes there. On an RTX 5090, frame cap off, 8 to 16 rounds of 8,192 a frame:

| | A round | Core 0 | Workers | All cores |
|---|---|---|---|---|
| core 0 busy, three workers asleep (`mctest 3 900000 bench 0`) | 1.61 to 1.76 ms | 4.7 to 4.9M a second | | |
| core 0 and three workers busy (`bench 2`) | 1.78 to 1.81 ms | 4.5 to 4.6M | 13.6 to 13.8M | 18.1 to 18.4M |

(Those figures are of the workers before they had paging; the world's shader sources and
boot images have to be synced and imported again for the kind described here.)

## Switches

    --cores N          N cores (1 to 16)
    --core-pitch N     the blocks are N texels apart (256)
    RVC_MC_FULL=1      draw a worker's whole 64 x 16, as before (to compare)
