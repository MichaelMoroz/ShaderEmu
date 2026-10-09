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
- **Workers** have no devices, no paging and no float registers, and take no traps. A worker is parked until core 0 writes its
  mailbox, the texel at `0x86C00000 + 16 k`: `{ "MCST", pc, sp, a0 }`. It starts there in machine
  mode with paging off and its number in `a1`. `wfi` ends its pass; `ebreak` parks it again, and
  so does any trap, with the cause in its state word, texel (41,0).a. `mhartid` is the core.
- **The arena** is 4 MiB of the GPU device's memory at `0x86C00000`, which `/dev/gpu` maps: a
  Linux program and a worker with paging off see the same bytes there, and no kernel change
  was needed. (Nothing keeps a GPU client from using that range too; this is a prototype.)

At exit the harness prints a line a core: its pc, instructions and whether it runs or what
parked it.

## The test (`programs/mc`)

`worker.c` is the test's jobs, linked at the arena; `mctest.c` is a Linux program that
carries them, has the library start the workers and checks them (`programs\mc\build.bat`, then
`python tools\make_linux_image.py`):

| Test | What it shows |
|---|---|
| primes | a count split four ways equals the count on core 0 alone |
| fill | each worker fills 64 KiB of its own; core 0 reads every word (13 passes: the write cache is 6 KiB) |
| stripe | three workers fill one buffer, every third 16 bytes each: no store undoes a neighbour's |
| sum | each worker reads what the other two wrote |
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

A worker has no paging and no float registers, so the 409 texels of the TLBs and the 8 of the
float registers were 43% of its pixels running the whole tick to write back what never
changes. It keeps the CPU's 44 texels and the write cache's 512 and nothing else, and they
are laid out as whole tiles, which is how the GPU runs pixels: the first 512 where they are
(64 x 8), and the cache's last 44 in a block of 11 x 4 under those, four to a column
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
| `mc.h` | the arena's layout and the mailboxes: what the machine, the library and a worker agree on |
| `mcw.h`, `mcw.c` | the library a program links: the arena, the workers' start, jobs, the wait |
| `mcw_worker.h` | the entry and the loop a worker runs, around the program's own jobs |
| `worker.ld` | links a worker's code to run in the arena |
| `worker.c`, `mctest.c` | the test: its jobs, and the program that gives them out |

A program supplies two things. Its **jobs**, in a file of its own that is built bare (no
library, no paging, no float instructions, `-march=rv32ima`) and carried in the program as
bytes:

    #include "mc.h"
    static uint32_t worker_job(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t core) {
        if (fn == MY_SUM) return sum((const uint32_t *)a0, a1);
        return 0;
    }
    #include "mcw_worker.h"

and the **calls** that give them out:

    int n = mcw_open(worker_code, sizeof worker_code, 3);    // how many workers there are: 0 without --cores
    uint32_t *data = mcw_alloc(bytes);                        // arena memory: the same address for a worker
    ...fill data...
    for (k = 1; k <= n; k++) mcw_post(k, MY_SUM, (uint32_t)part(k), words(k));
    ...this core's own share...
    for (k = 1; k <= n; k++) { mcw_wait(k); total += mcw_result(k); }
    mcw_close();                                              // before leaving: atexit(mcw_close) will do

`programs/mc/build.bat` and `linux/ralert/build.sh` show the two builds (clang with the small
runtime, and the image's toolchain).

What a program has to keep to:

- **A worker sees only the arena and what it is given by physical address.** It has no paging:
  a pointer into the program's own memory means nothing to it. What a job reads or writes is
  allocated with `mcw_alloc()` (4 MiB in all, less the first megabyte), or is GPU memory, whose
  physical address the program passes (`seglAddress()`: a picture, a texture).
- **A store is seen by the other cores one pass later.** `mcw_post()` followed by this core's
  own work is the usual order: the pass that ends next carries the job and its data over
  together. `mcw_wait()` ends passes until the worker has answered.
- **Sixteen bytes, one writer a pass.** Give each core whole texels: rows of a picture whose
  width is a multiple of 16, buffers from `mcw_alloc()` (they are 16 bytes aligned). A header
  that this core writes must not share its last 16 bytes with data a worker writes.
- **A worker keeps 6 KB of stores a pass**, as core 0 does, and runs on until its job is done
  however many passes that takes. For work that is all stores, the cores' number is the gain.
- **One program at a time.** The arena carries its owner's process number; `mcw_open()` gives 0
  while another program that is still alive has it, and a program that gets 0 does the work
  itself.
- **No workers is the common case**: Unity runs one core, and so does the harness without
  `--cores`. Every use has to fall back on the program doing the job itself.

What it is good for, and what not:

- **Work that is the same for many pieces of data and writes much**: decoding, unpacking,
  filling, scaling, mixing. Red Alert's movies are the example (below): rows of a frame to each
  core, and the next frame unpacked ahead on a worker of its own.
- **A pipeline a frame ahead**: what the next step needs is begun on a worker while this core
  does the present one, and waited for only when it is needed (`workers_lcw` in
  `linux/ralert/workers.c`).
- **Not a program's own logic as it stands.** A game's objects are in its own memory, which a
  worker cannot reach, and its code expects to see its own stores at once. To give a worker
  such work its data has to be moved into the arena, or copied there for the job.
- **Not small jobs.** Giving a job out and taking the answer costs a pass or two (some
  milliseconds): below some tens of thousands of instructions the core does it sooner itself.

What it costs: nothing to speak of while the workers sleep; 5% of a pass for the first busy
one and 17% for three; and the commit pass looks a RAM texel up in each busy core's cache.

## What paging would cost a worker

Nothing in pixels. A worker that runs a program's own code needs the program's page table,
and the question was whether it then needs core 0's 409 texels of TLBs, which would make it
a full-size core again (17% of a pass for three thin workers, a second pass for three full
ones). It does not: core 0 itself made to forget its TLBs at the start of every pass (an
experiment, not kept: the second level ignored and the megapage entries cleared in
`tlb_state_load`) runs a Linux boot no slower, 0.476 ms against 0.548 and 0.625 for 2,048
instructions a pass and 2.91 ms against 3.01 and 3.06 for 16,384, with the same instructions
a pass. A page's first use in a pass is a walk of two table entries, and a pass touches few
pages. So a worker with paging keeps the CPU's texels, the write cache and the eight texels
of float registers, 564 in the 576 pixels a thin worker has now, and starts every pass with
empty TLBs.

## A use: Red Alert's movies

`docs/ralert.md` ("Movies, and the worker cores"; `linux/ralert/workers.c` and `worker.c` over the
library above): the opening movie's frames are decoded by
core 0 and three workers into a picture the GPU shows, and played in 18.6 s where they took
41.4. It is the kind of work the workers are for: every pixel stored once, and a core keeps
only 6 KB of stores a pass. The window system's pool of window buffers ends 4 MiB lower for
it (`POOL_SIZE` in `linux/nanox/scr_shaderemu.c`): the arena is above it.

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

A program killed while its workers run leaves them running: the next `mctest` finds no
worker to start ("0 worker cores") until the machine is switched off and on.

## Switches

    --cores N          N cores (1 to 16)
    --core-pitch N     the blocks are N texels apart (256)
    RVC_MC_FULL=1      draw a worker's whole 64 x 16, as before (to compare)
