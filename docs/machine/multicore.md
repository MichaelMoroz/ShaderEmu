# More than one core

The machine with worker cores beside it, all run by the one tick pass. Linux on core 0 starts
the workers and gives them work, and every result is right. Core 0 and three workers cost
1.17 times the pass of core 0 alone, so four cores do 3.4 times the work of one; past that a
pass grows with the cores (eight: 2.1 times, sixteen: 3.2 times). It took two things to get
there, both about how the GPU runs pixels: the state as full tiles of pixels, and workers that
keep no more than they need ("What a pass costs" and the sections after it).

    bin\rvc_harness.exe --rvc experiments\rvc_opt --image linux-net
    / # mctest            (every worker; "mctest N LIMIT" for other numbers, "mctest N LIMIT bench" for the cost of a pass)

The machine has 256 cores wherever it can have workers: the D3D11 harness with its eight
targets, `rvc_cpu` and Unity compile `CORES=256` ("Up to 256 cores"), and there is no switch
for fewer but the harness's `RVC_CORES=N`, a measurement (there was `--cores N`). The one-core
machine is what is left where there are no workers: D3D12,
`--no-mrt`, `--profile` (the code is under `#if CORES > 1`). The 64 of before cost core 0 about 12%
while no worker ran: a shell loop was 3.6M instructions a second, 4.1M on one core (parked
cores cost nothing now: "Up to 256 cores").

## How it is made

- **A block of state a core.** Core 0's state is the 64 texels wide block it always was. The
  workers' is a strip of whole 8 x 8 tiles in bands beside it, in the state rows, which were
  empty there ("The geometry", "Up to 256 cores"; it was once a block `CORE_PITCH` (256)
  texels further right a core). `STATE_TEX` adds the core's place (`state_off`); RAM is read
  with `RAM_TEX`, which nothing moves. The tick pass draws core 0's rectangle and a quad for
  each worker with something to run, all before any is copied back.
- **The same RAM.** Every core reads RAM as the last commit left it, plus its own writes (its
  write cache). The commit draws every core's stores, the highest core's last ("The commit as
  points"). So a core sees another's stores one pass later.
- **Sixteen bytes, one writer.** The write cache holds whole texels: a core that stores one word
  carries the texel's other three as it read them, and would undo another core's store to them
  in the same pass. What changes hands is therefore 16 bytes with one writer a pass. There are
  no atomic instructions between cores and no locks; `mc.h` (programs/mc) is the convention.
- **Workers** have no devices and take no traps; they have the float registers and paging.
  A worker is a thread of a Linux program that the kernel does not know of: it runs in user
  mode on the program's own page table, so the program's functions and pointers are its own.
  It is parked until core 0 writes its start word, the texel at `0x80C00000 + 16 k`:
  `{ "MCSU", pc, sp, root }`, the page table's first page being number `root` (the kernel
  tells a program its own: an ioctl of `/dev/gpu`). It starts there with its number in `a0`.
  `pause` is its sleep until the first word of its job changes, and `ebreak` parks it again.
  On the machine without paging (`NO_PAGING`, Linux without an MMU: `docs/system/nommu.md`) a worker
  runs untranslated, as core 0 does, and its root is 0.
- **A trap stops a worker where it is.** There is no kernel on a worker to take one, so the
  machine writes `{ count, cause, address, pc }` into the worker's mailbox and the worker
  waits. A page the program has not touched yet is the usual cause: the library, on core 0,
  touches the page (which the kernel answers as it would for the program itself) and writes
  the count back, and the worker runs the instruction again. Anything else that traps ends
  the program with a line saying where.
- **A system call is such a stop too.** The machine puts the call's number and arguments in
  the mailbox (cause 8), core 0 makes the call in the program they share and writes the
  answer back, and the worker goes on after its `ecall` with the answer in `a0`: memory from
  the kernel, a file read, a line printed. Two passes each, when core 0 next looks. Not the
  calls that are about which thread is calling (a new thread, a new program).
- **No TLB kept.** A worker starts every pass with empty TLBs and walks the page table for
  each page it uses in the pass ("What paging costs a worker", below).
- **The mailboxes** are eight pages of the GPU device's memory at `0x80C00000`, which `/dev/gpu`
  maps: the start words, the kernel's words, a system call's words, and from `0x4000` 64 bytes
  a core (its job, its answer, its fault and the word that lets it go on). `programs/mc/mc.h`
  is the layout ("Up to 256 cores").
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
workers and checks them (`linux/nommu/root.sh` builds it, then `pwsh tools\make_image.ps1`):

| Test | What it shows |
|---|---|
| primes | a count split four ways equals the count on core 0 alone |
| fill | each worker fills 64 KiB of its own; core 0 reads every word (18 passes: the write cache is 6 KiB, and the buffer's pages are new) |
| stripe | three workers fill one buffer, every third 16 bytes each: no store undoes a neighbour's |
| sum | each worker reads what the other two wrote |
| pages | a worker writes and reads 64 pages nobody has touched: 64 stops, each answered by core 0 in two passes |
| calls | a worker asks the kernel for memory, fills it, writes a line and gives the memory back |
| floats | float arithmetic kept in registers over many passes gives what it gives on core 0 |
| loop | eight rounds of `mcw_for` over 256 KB; 64 small ones back to back, the team waiting on the round word; a loop asked inside a piece |
| async | 24 `mcw_call`s, more than there are workers, joined last to first |
| effects | jobs print, defer a call, take scratch memory, and one fails: core 0 sees each at the join |
| counters | every core adds to two counters in the same passes; the totals are exact |
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
of `src/types.h`; without an MMU there is no CSR area and no TLBs, and the tick draws 8 rows
of core 0's: `docs/system/nommu.md`, "The state"). The first 44 texels did not move. There is no other layout: a snapshot
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
texels, the write cache's 512 and the float registers' 8 and nothing else, and they were
laid out as whole tiles, which is how the GPU runs pixels: the first 512 where they are
(64 x 8), and the cache's last 44 and the float registers in a block of 13 x 4 under those, four to a column
(`MC_ROWS` in `src/types.h` then; "The geometry", below, replaced it). The pass draws two zones for a worker, 64 x 8
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
up in every core's write cache). With more than eight cores the blocks had to be closer
than 256 texels: `--core-pitch 128` (the harness ignores it now: the workers are one strip,
"The geometry").

## The geometry

(Since 11 October 2026 the kernel sets one geometry, once: as many workers as the machine has
cores for, 63 of 64 then, each with 1.5 KB of stores a pass; now 255 of 256 with 3 KB, or the
hybrid one `geotune` lays out at boot ("Up to 256 cores", "Packing the small workers"). Programs share them ("Shared between
programs") and none lays them out: `mcw_shape`, `mctest ... shape` and the programs' own
variables for it are gone. What follows is how the machine takes a geometry, which it
still does, and what the sizes were found to be worth.)

Which pixel of the tick is which texel of which core is only a function, so how many workers
there are and how large each is need not be compiled in. The workers' state is a band of the
state rows beside core 0's block, 8 texels high and 64 tiles of 8 x 8 long; each worker has
a strip of whole tiles of it, one worker's after another's (`mc_core_at()`,
`mc_state_read()`, `mc_texel_of()` in `src/types.h` and `main.shader`). A worker keeps the
CPU's 44 texels, its write cache and the float registers' 8, and nearly all of that is the
cache, so the cache's size is the worker's size:

| Tables of 2^bits buckets | New stores a pass | Texels | Tiles | Fit in the band |
|---|---|---|---|---|
| 6 (core 0's, and a worker's until now) | 5.25 KB | 500 | 8 | 8 |
| 5 | 2.6 KB | 276 | 5 | 12 |
| 4 | 1.3 KB | 164 | 3 | 21 (the machine has 15 workers at most) |
| 3 | 0.66 KB | 108 | 2 | 32 |

The tables are 2^bits buckets, half and a quarter of that (17 October 2026; the third was half
before, 564 texels and 9 tiles at 6 bits): core 0 without paging (`NO_PAGING`, no TLBs:
`docs/system/nommu.md`) is then 8 x 8 pixels of the tick's targets, as a worker. Under `RVC_WCACHE=1` it ended more passes full, Doom's timedemo 549 to 880 of
about 1.4 million, Quake III to frame 200 1,165 to 1,465, nxray 119 to 180.

So it is few workers that store much in a pass or many that store little, or any mix. A
worker whose cache is full ends its pass there, as any core does, and goes on in the next.

The geometry is a texel of the control words (`0x810003d0`): a word with bit 0 set while the
square is laid out anew, then four bits a worker for workers 1 to 8 and for 9 to 15 (its
bits; 0 ends the list). **It is set while every worker is parked, and at no other time**: a
worker's registers and cache are those pixels. The kernel did it (`SHADEREMU_GPU_SHAPE`,
`workers_shape()` in `linux/kernel/shaderemu_gpu.c`; `mcw_shape()` in the library; the calls
do nothing now, and `workers_layout()` writes the kernel's one geometry the same way): it
refuses with EBUSY while any program has a worker, parks every core, sets the bit, during
which every pixel of the band is zero and the commit takes no worker's stores (a worker
with no state is a parked one), writes the two words and clears the bit. The control pass
says in `0x810003c0` how many cores there are now, and in the word at `0x810003cc` how many
there could be (256). Until a program asked, the kernel
laid out three workers of the full size and twelve of size 4 the first time workers were
asked for. `mctest N LIMIT shape 6,6,5,4,4` sets a geometry and runs the tests on it.
A geometry a program asked for lasts while anybody has workers: when the last one is given
back the kernel forgets it, and the next program to ask has the usual one again. (A ray
tracer's 63 small workers used to stay, and were what a game's one large job then got.)

**The tick's geometry shader draws the strips** (`tick_geom` in `main.shader`). The pass is
handed one quad; the shader reads the geometry and puts out a quad for core 0's rectangle
and one for the strip of each worker that has something to run. A worker that is parked,
asleep or stopped gets none (`mc_idle()`, asked there), so it costs no pixel, and every
core's pixels are a rectangle of their own. While the band is laid out anew the shader
draws all of it, as zeros. The host knows none of this: before the pass it copies core 0's
rectangle and the band into the buffer the pass draws into, so that what is not drawn stays
what it was, and copies both back afterwards (`CustomRenderTexture::copyIn`,
`rvc_backend11.cpp`). A machine with every worker parked runs as fast as one compiled for
one core (3,378k instructions a second with one core compiled in, 3,229k with sixteen:
what two runs differ by). The D3D12 backend, which runs one core, draws core 0's rectangle
itself and does not use the shader.

Why whole tiles: pixels of two cores in one 8 x 8 tile cost the GPU both cores' work for
each ("Why", above). Laid out as rows of 64 in a square (3, 5 or 9 rows a worker, so that
neighbours shared tiles) seven workers decoded the JPEG below in 3.4 s; as tiles, 2.4 s.

`mctest` passes on every geometry tried (three of size 6 and twelve of 4; seven of 6; four
of 5, eight of 4 and three of 3), changed between runs without a restart. The primes below
120,000, shared: 5.2 s on core 0 alone, 0.56 s with the default fifteen (9.3 times; fifteen
workers of the full size, each a block of its own, made it 4.7 times) and 1.06 s with seven
of the full size (4.9 times). A JPEG picture of
640 x 480 (`docs/system/nanox.md`, the viewer), whose strips of pixels are shared out between the
workers after the first, which reads the codes: 6.8 s on core 0, 2.4 s with seven workers
(three of 6, four of 4), 2.2 s with eleven (one of 6, ten of 5); the one core that reads the
codes is what the others wait for there.

A program is not told yet which of its workers are small; the kernel hands out the lowest
numbers first, which are the first in the geometry. The VRChat world has the same geometry
("In Unity", below).

## Eight texels a pixel

A pixel of the tick need not be one texel of state: with eight render targets it is eight,
and the pass runs an eighth of the pixels. The D3D11 harness does that (`TICK_MRT`;
`--no-mrt` for one target, which D3D12 still has; Unity's is below, "In Unity"): the tick draws into eight
targets 264 wide and as high as core 0's rows (1,040 wide and 16 rows for every 128 workers
since "Up to 256 cores"). Core 0's pixel (x, y) keeps texels 8x to
8x + 7 of row y. A worker's tile of 8 x 8 is a column of eight pixels, in a block of columns
that is the worker's own; the quads are the geometry shader's as before. The same pass
compiled with `TICK_UNPACK` then draws the state's quads into the state texture and copies
each texel from its target, so the commit, the control pass and every reader find the state
where it was.

Where the pixels are matters more than how many there are. The card runs pixels in groups
(4 x 8 here), and a group with pixels of two cores takes as long as both together, unless
the cores happen to run the same instructions: `mctest`'s bench, the same primes on every
worker, hides all of it, and `nxray`, where no two cores do the same, shows it. One picture
of `nxray`, 15 workers:

| The workers' pixels | Size 3 | Size 4 |
|---|---|---|
| one target, tiles of 8 x 8 | 1.35 s | 2.12 s |
| eight targets, one column after another | 2.19 s | 2.52 s |
| a block of 4 columns a worker (`TICK_BLOCK`) | 1.33 s | 2.13 s |
| the same, and two columns of it or more left empty (`TICK_PAD`) | 1.33 s | 1.33 s |

A worker of size 4 is three columns, and with one empty column beside them it still shared;
with two (a block of eight) it does not. So a block is the worker's columns and two more,
rounded up to four: one block for size 3, two for sizes 4 and 5, three for size 6. Core 0's
pixels need eight empty columns between them and the first worker's (`TICK_GAP`): with four,
`nxpath`, where core 0 draws a panel while the workers trace, ran 56 passes a second in
place of 84 (one target: 79).

With that, a larger worker costs no more than a small one: 12 of size 5 trace the picture in
1.63 s (2.6 s with one target) and 7 of size 6 in 2.56 s (4.1 s). `mctest`'s primes with the
kernel's 15 workers are 12.5 times core 0 alone (9.3 with one target).

What it gains one core by itself is little, 16,384 instructions a pass:

| | One target | Eight |
|---|---|---|
| a shell loop | 3,497k instructions/s | 3,649k (4%) |
| Doom's timedemo | 3,124k | 3,297k (6%) |
| Quake's timedemo | 3,288k | 3,342k (2%) |
| Tiberian Dawn, mission 10 | 3,591k | 3,684k (3%) |

A pass is as long as its longest pixel, the instructions one after another, and 973 pixels
were already about what the card runs side by side for nothing. What the eight targets buy
is that pixels stop counting, which shows in three places.

Power (an RTX 5070 laptop card, `nvidia-smi`'s reading, 18 W with nothing running): a shell
loop on one core is 46 W with one target and 37 W with eight, a third of what the machine
itself draws; fifteen of the smallest workers busy, 55 W and 41 W.

Longer passes with a larger write cache, which were the harness's defaults then (`--ticks`
32,768; `L1_TABLE_BITS=7`, 1,485 texels of state, which with one target is over what the
card runs for nothing; now 8,192 and the shader's own cache, as in the world: "In Unity"). A pass ends early when the cache is full, so passes of 32,768
gained little before (Doom's were 14.4k long); with the cache twice the size they are 24k
long. One core, instructions a second:

| | One target, 16,384, the cache as it was | One target, 32,768, twice the cache | Eight targets, 32,768, twice the cache |
|---|---|---|---|
| a shell loop | 3,497k | 3,698k | 3,902k (12%) |
| Doom's timedemo | 3,124k | 3,369k | 3,694k (18%) |
| Quake's timedemo | 3,288k | | 3,575k (9%) |
| Tiberian Dawn, mission 10 | 3,591k | 3,719k | 3,971k (11%) |

(65,536 a pass adds nothing to that: Doom's passes are 26.5k long there, a frame of the game
ending them. A cache four times the size, `L1_TABLE_BITS=8`, is a shader the driver refuses.
The ray tracers like the longer pass too: 1.34 s a picture at 32,768, 1.50 s at 16,384,
1.68 s at 8,192.)

More cores. `TICK_LOAD=N` draws every worker's quad N times (the copies under the targets'
rows, read by nothing): what the tick would cost with N times the busy workers. Fifteen of
the smallest workers on the same primes, core 0 waiting, each in two columns (so 15 pixels
a worker, and the same work on all of them, which does not show what sharing costs):

| Busy workers' quads | Pixels | The same work | The card |
|---|---|---|---|
| 15 (one target) | 1,740 | 14.67 s | 55 W |
| 15 | 225 | 14.53 s | 41 W |
| 30 | 450 | 14.56 s | 44 W |
| 60 | 900 | 14.62 s | 49 W |
| 120 | 1,800 | 14.70 s | 58 W |
| 240 | 3,600 | 16.97 s | 67 W |

And with work of its own on every core, each worker in its block (`nxray`, 15 workers of size
3, their blocks drawn N times): 1.32 s a picture for 15 blocks, 1.34 s for 30, 1.36 s for
60, 1.40 s for 90, 2.21 s for 120.

So this card runs some 2,000 to 3,000 pixels of the tick in the time of one, which is 60 to
90 blocks of 4 x 8. The tick was not what stood in the way of that many workers: the commit was (it looked
into every core's cache for every texel it wrote: 0.06 ms a pass with one core busy,
1.1 ms with sixteen), and the geometry's texel, which had room for fifteen (both since mended:
"The commit as points", "Up to 256 cores").

In Unity the tick is a camera's pass, which is how a pass with several targets is drawn in
VRChat ("In Unity", below).

## Up to 256 cores

(11 October 2026. What "The geometry" and "More than sixteen cores" say of four bits a worker,
of one band of 248 tiles and of the mailboxes' places is how it was before this.)

The machine has 256 cores: the harness and `rvc_cpu` (`kCores`), and the world (`CORES` in
`MachineBlit.cginc`, `MachineCores` in the builder). It had 64. What made that possible:

- **The workers are all alike**, so the geometry is two words of the texel at `0x810003d0`:
  how many workers (`.g`) and the size of their write caches (`.b`, 3 to 6); bit 0 of `.r` is
  still "being laid out". Where a worker's state and its pixels are is arithmetic from its
  number, with no loop over the workers before it (`mc_tile_of`, `mc_block_of`, `mc_core_at`,
  `mc_core_in` in `src/types.h`). The kernel writes it once: every core but the first, size 5
  (unless `geotune` has laid out its hybrid one at boot: "Packing the small workers").
- **A worker is five tiles** (3 KB of stores a pass; it was three, 1.5 KB). Its block in the
  tick's targets is eight columns either way, and up to six of them may run before it costs:

  | Columns of the eight that run | 3 | 4 | 5 | 6 | 7 | 8 |
  |---|---|---|---|---|---|---|
  | `nxray` 640 x 480, 63 workers | 1.13 s | 1.13 | 1.13 | 1.13 | 1.92 | 1.97 |

  A block the other way up (a tile a row of eight pixels, the worker 8 x 3) was tried: 2.0 s
  with blocks side by side, with four empty columns between them, and one under another
  with one or five empty rows between; 1.11 s only with eight empty columns and nothing else
  in the 16 rows. The card does not run aligned 8 x 4 groups here.
- **The state strip is bands.** A band of eight state rows beside core 0's block holds 248
  tiles, 49 workers; the bands are one under another (`MC_STRIP_BANDS`: two for 64 cores, six
  for 256), in rows of the state area that nothing else had.
- **The tick's targets are bands too**: 1,040 columns (core 0's eight, eight empty, 128
  blocks) and 16 rows for every 128 workers. Blocks 16 rows apart do not share: every
  worker's pixels drawn twice cost nothing, four times 6%, eight times 94% (`TICK_LOAD`).
- **The mailboxes are eight pages** at `0x80c00000` (`programs/mc/mc.h`): a start texel a core
  from 0, the kernel's words from `0x1000` (cores asked back, the offer count, a word a
  program that posts) and `0x1400` (a core's owner), a system call's words from `0x2000` and
  the mailboxes from `0x4000`. A bit a core is eight words, in the calls and in memory.
  `/proc/gpu` says whose a tag is: a line an open of `/dev/gpu` (`pid workers wanted lent tag
  draws lists`), which `nxmon` matches to the owner words to give each program its cores.
- **What the cores ran** is at `0x816ba000` (`MC_STATS_ALL`, the network's row after its ring):
  a word a core, then from `+0x400` a texel for every 32 cores: a word of which of them run
  and a word of which sleep.
- **The commit's points are a bucket each**, not an entry: 258 for core 0 and 130 a worker.

What it is worth (D3D11, an RTX 5090; the sums are the same at every size):

| | 64 cores | 256 |
|---|---|---|
| `nxray` 640 x 480, a picture | 0.97 s | 0.28 |
| `nxpath` 320 x 240, 8 samples | 6.8 s | 1.77 |
| core 0 alone, a shell loop, workers parked | 3.74M/s | 3.74M/s |
| every core at the same integer loop (`mctest N 3000000 bench 4`) | 292M/s | 1,095M/s |
| every core at its own float work (`nxpath`) | 176M/s | 621M/s |

A worker with a core runs a whole pass in every pass (counted pass by pass, 255 of 255). What
the last two rows differ by is the pass: 1.9 ms while the cores run the same instructions,
3.4 ms at 256 (2.9 at 64) when each runs its own. `--stats-after` does not show that: its
frame rate stays near 500 while the workers' counts say 300 passes a second.

**Parked cores cost nothing.** They did: 13% of core 0 at 256. None of it was the amount of
work. A pixel is slow (a pass of 8,192 instructions is 2 ms), so one pixel that loops over
every core takes 0.05 ms by itself, however few such pixels there are. Three did:

- a RAM texel a core stored to asked every core for its store. A point now carries its core
  (`commit_core`, the vertex's `primitiveID`) and asks that one: two cores may not store to
  one texel in a pass anyway. A band drawn whole asks none (the stores are points, drawn after).
  A point whose core stored nothing there draws nothing (`discard`): a worker that sat the
  pass out has its last pass's tags still, and its point put the texel as it was over what
  another core had just stored (Quake, whose server and client share memory, crashed);
- the texel of which cores run looped over all of them, and so did the two of bits: now a
  texel for every 32 cores, and the number of cores is read from the geometry;
- the commit's vertices each asked every core which bands it changed, which with the stores
  as points is the same answer from all of them.

And of a worker's state the commit changes three texels (8, 28 and 41): the rest is returned
as it is, without the state being decoded. With that the 64-core machine's commit went from
0.037 to 0.018 ms a pass and its control pass from 0.059 to 0.032.

**A share, not a tile.** `nxray` and `nxpath` handed out a tile at a time: core 0 walked every
worker to give the next, and a worker waited some passes between tiles (255 of them worked a
fifth of the time). A worker now has all of its share in one job (`trace_share`, `pt_share`):
every n-th tile of an order that scatters them over the picture (tile j is number j x stride,
the stride with no factor in common with the number of tiles), small enough tiles that each
has two dozen. The path tracer's share is every sample of its tiles, one after another; it
writes how far it is into 16 bytes of its own, ends early when the kernel wants its core
(`mcw_recalled`) and is posted again for the rest. Core 0 only shows the picture as it fills.

Two faults of the sharing were found on the way. A program looked at the mailbox of a core
the kernel had given to another, and took that program's page fault for its own (`nxmon`
crashed when its workers were borrowed for long): `serve()` looks only at a core that is the
program's still, and the kernel takes back no core whose answer the program has not taken
(`MC_TAKEN_AT`). And a worker still on its way when the first job was posted was never served
its first page: `look()` serves it.

## Packing the small workers (`geotune`)

(14 October 2026; the tick's own since 15 October, `--define MC_UNIFORM` for the geometry of
before.) The geometry is hybrid:
workers 1 to 16 big (caches of 2^5 buckets), which keeps a game's few workers at full speed
(small caches cost Doom 15 to 38%), and the rest small (2^3). How the small ones' blocks lie
in the tick's targets depends on the card, and `geotune` measures it at boot.

**The field.** The geometry's texel (`0x810003d0`) is the same four words, and `.a` says the
rest:

| Bits of `.a` | What |
|---|---|
| 0-7 | how many big workers (0: the uniform geometry, every worker as `.b` says) |
| 8-15 | the big ones' bits (3 to 6) |
| 16-23 | the small ones' blocks' width in columns (0: `TICK_BLOCK` with `TICK_PAD`, as compiled) |
| 24-31 | the big ones' (0: `TICK_BLOCK_BIG` with `TICK_PAD_BIG`) |

A width the field gives is the worker's tiles rounded up to a multiple of it, with no column
more: a small worker (2 tiles) is 2 columns at width 2, 4 at width 4, 8 at width 8. The big
workers have the first band of the state strip and of the targets; the small ones the bands
after, and those the targets' later bands do not hold go after the big ones' blocks in the
first band (with eight columns a worker the second band holds 128 of 239). `mc_geo_read()` in
`src/types.h` reads it for the tick and the commit, and `mc_geo_count()` in `src/gpu.h` the same
for the control pass (`gpu.shader` has no `types.h`), whose count of cores (`0x810003c0`) is
what the kernel shares out: a worker the targets have no block for is no worker. With
`MC_UNIFORM` defined the shader is that of before, preprocessed text for text.

**Who sets it.** `geotune` (`linux/userland/geotune.c`), which the init runs before any
program asks for workers (the layout may only change while every worker is parked, and the
kernel keeps a layout it finds set). It first lays out a geometry that only a tick with
`MC_HYBRID` gives 47 cores (small blocks 64 columns wide); one that gives every core is a tick
without it, and `geotune` leaves the layout to the kernel. Then:

1. **Capacity, not a race.** Every worker small and in eight columns of its own, the same job
   (a loop whose branches go by the worker's number, so no two run the same instructions) on
   an eighth of them and on all of them, in turn, three times each; the medians are compared.
   A card that runs every block side by side takes as long for all as for the eighth; one that
   does not takes longer, and all over the time is roughly how many it runs at once. The job is
   sized to some 200 ms for the eighth.
2. Under 1.5 times: eight-column blocks, and the small workers get the big ones' caches (2^5),
   so every worker is as the uniform geometry has it: a card that runs them all at once gains
   nothing from small caches and loses `nxphys` to them. Otherwise two columns at 2^3. If
   either set of three times is more than 30% apart, three rounds more are run and the middle of
   six decides: in the VRChat world, whose frames come unevenly, a 5090's three were 233 to 346 ms
   and 244 to 340, and "too unsteady, two columns" chose the layout that costs it `nxphys`.
3. The registry keeps the choice (`geotune.width`, `.workers`, `.iterations`, `.us`, `.ratio`;
   `geotune.force` 2, 4 or 8 is a width with nothing measured: `docs/system/persist.md`). So that a
   kept registry is there to be read, `geotune` loads the kept folder itself when the desktop
   is to start (`nxpersist load`, and `/tmp/nxpersist.early` tells the `nx` script it is done).
   On a later boot only the eighth's job is timed again (three times, the median): within 25%
   of the kept time it is the same card and the kept width is laid out; otherwise it measures
   again. The host does not say which GPU it has, so another card is told by its speed.

`geotune again` measures though a choice is kept (before the desktop, when no program has
workers).

**What it says and who may choose.** At boot it prints each step on the console: what it
measures, each of the three rounds' times, the ratio and its pick, and the layout it made. The
host may ask for a layout in bits 2-3 of the host flags (`0x8100003c`: 0 the pick, 1, 2, 3 the
small workers in 2, 4 or 8 columns; the harness's `--workers auto|2|4|8`); `geotune` still
measures, lays out what was asked and says both. Its pick and ratio are then in the geometry
texel's first word, which the tick reads only bit 0 of: bits 8-15 the width it picked, 16-31
the ratio in hundredths. The world's "Worker cores" strip under the control panel has the four
choices (Auto, Dense, Medium, Wide), takes effect at the next Reset, and shows what Auto picked
and what runs. As the tick's own on the 5090 it costs nothing that shows: a shell loop 3.96 and
3.92 M a second against 3.69 and 4.30 with `MC_UNIFORM`, Doom's timedemo 1,569 and 1,476
realtics against 1,675 and 1,417, in turn.

What it found here (RTX 2060 Mobile, harness under Wine and DXVK, 255 workers), and what the
width is worth:

| | |
|---|---|
| first boot | 255 workers took 3.22 times as long as 31 (188 ms, the threes 2% and 0% apart): about 79 side by side; 2 columns (and 3.13, 81, on a second cold boot) |
| later boot, the registry kept (`--persist`) | 173 ms against 190: 2 columns as before |
| a tick without `MC_HYBRID` | "all alike": the kernel's layout, `nxray` on 255 workers |
| `nxray` 640 x 480, 2 columns (as chosen) | best of six 643 ms, sum `fd92a71e` (the hybrid by hand: 611 to 722; the uniform L1: 940) |
| `nxray`, 8 columns (`geotune.force` 8) | best 758 ms, sum `fd92a71e`, 255 workers (128 in the second band, 111 after the big ones) |
| `nxphys` scene 1, 720 substeps | 33,670 to 33,839 ms (L1 33,731), the end state the same: highest 5030 mm, deepest 1, fastest 395 mm/s, 10 without room, sum `d8f2fca2` |
| Doom `-timedemo demo1` | 3,274 realtics (L1 3,175 to 3,301) |

About 79 at once agrees with "Up to 256 cores" (60 to 90 blocks on this card). An RTX 5090
(D3D11) measured 1.08 to 1.10 times, about 231 to 236 at once, on every one of eight runs, and
lays out eight columns with every worker big. The first boot spends about three seconds measuring (a warm-up, the
sizing run, 3 x (190 + 600) ms and four layouts of 0.2 s); a later boot about one.

The three widths forced in turn, round after round (medians; `nxray` best of six, sums
`fd92a71e` and `d8f2fca2` in every run):

| | 2 columns | 4 | 8 |
|---|---|---|---|
| RTX 2060 Mobile, `nxray` 640 x 480 | 616 ms | 594 | 742 |
| RTX 2060 Mobile, `nxphys` 720 substeps | 33.8 s | 36.5 | 47.3 |
| RTX 5070 Laptop (D3D11, `geotune`: 3.80 and 3.83 times, about 66 at once), `nxray` | 177 ms | 251 | 315 |
| RTX 5070 Laptop, `nxphys` | 10.1 s | 12.2 | 14.7 |
| RTX 5090 (small workers 2^3), `nxray` | 150 ms | 106 | 108 |
| RTX 5090 (small workers 2^3), `nxphys` | 8.5 s | 6.9 | 7.9 |
| RTX 5090, `geotune`'s pick (8 columns, every worker 2^5), `nxray` / `nxphys` | | | 118 ms / 4.42 s |

Doom, Tiberian Dawn: the same at every width within the cards' drift (their work is on the big
workers and the first core). Four columns win 3.6% of `nxray` on the 2060 and lose 8% of
`nxphys`; two is the dense width on both laptops. On the 5090 eight columns and four are level
within its drift, and what costs is the small caches: with every worker at 2^5 `nxphys` is 4.42
to 4.60 s against 7.49 to 7.66 at four columns of 2^3, `nxray` the same (112 to 120 ms against
105 to 119), as the uniform L1 (114 ms, 4.75 s). `nxphys`'s quads (`PHYS_QSUM`) were `8f79c97a`
at every width there.

## More than sixteen cores

The machine had 64 then (the D3D11 harness with the eight targets, `rvc_cpu`, Unity; 256
since "Up to 256 cores"). What is different above 16:

- the workers' band is the state rows to their end, 248 tiles of 8 x 8 in place of 64;
- the geometry is eight words, four bits a worker: the geometry texel's second to fourth
  words and two texels of the mailbox page (`0x80c00f00`, `0x80c00f10`; they were the two
  control texels after it, which are the network's);
- cores 16 to 63 have their mailboxes after the first sixteen's (`programs/mc/mc.h`: three
  pages now), so a program built before knows the first fifteen workers and is not disturbed;
- the kernel has two more calls on `/dev/gpu` for them (`SHADEREMU_GPU_SHAPE_ALL`, eight
  words; `SHADEREMU_GPU_WORKERS_ALL`, the cores it gives as two words of bits). The older
  calls give out the first fifteen. The library (`mcw.c`) uses whichever the kernel has (the
  two `SHAPE` calls do nothing now: the layout is the kernel's);
- the tick's geometry shader draws sixteen cores' quads a primitive (a primitive is 64
  vertices at most), and the pass is given a primitive for every sixteen cores.

The commit looks into a core's cache for every texel of RAM it writes, which with 64 cores
was 4.5 ms a pass (1.2 ms with 16). A worker keeps all the addresses it stored to or-ed
together in one word (it is how a load knows the cache has nothing for it); the commit now
passes over a worker for a texel whose address has a bit that word lacks: 2.2 ms.

`nxray`, one picture of 640 x 480 (807,158 rays, 300 tiles; the same sum every time),
workers of the smallest size, an RTX 5070 laptop card:

| Workers | A picture | Rays a second | Against core 0 alone |
|---|---|---|---|
| none | 69.0 s | 11 thousand | |
| 15 | 4.50 s | 179 thousand | 15 times |
| 31 | 2.38 s | 336 thousand | 29 |
| 47 | 1.72 s | 467 thousand | 40 |
| 63 | 1.35 s | 594 thousand | 51 |

(63 of the next size up, with twice the write cache each: 1.40 s.) With 63 a pass is 9.5 ms
of tick, as with 15, and 2.2 ms of commit. `mctest`'s primes below 480,000: 45.3 s on core 0
alone, 0.79 s with 63 workers, 57 times. `mctest 63 60000 shape 3,3,...` passes, and so does
a mixed geometry of 40.

## The commit as points

The commit drew whole every 4 MB band of RAM a core had stored to, and looked for every
texel of it in every core's write cache. With the tick in eight targets (D3D11 harness;
`--define NO_SCATTER` for the bands) what the cores stored is drawn as points instead, a
list of them in one draw, and a geometry shader (`commit_geom`) makes each point the
rectangle it stands for:

- an entry of a write cache: the texel its tag names, 1 x 1;
- the store a full cache had no room for: its texel;
- a copy or a fill, which is megabytes at a time: the rows of its destination, three
  rectangles at most (a first row from where it begins, whole rows, a last row to where it ends).

Point n was core 0's entry n (768 of them, then its last store and its copy), and from 770 on
386 a worker: 770 points for one core, 6,560 for 16, 25,088 for 64, most of which are no
entry and come to nothing (a point is a bucket now: 258 for core 0 and 130 a worker, 33,408
for 256, "Up to 256 cores"). Where the stores are is read from the tick's targets. The pixel
shader is the commit's own, so a texel is what it always was, whichever cores stored to it
and in what order. The state rows and the bands that something else wrote (the GPU's
control words and its picture copied back) are still drawn whole, in the draw before.

RAM was two buffers, which a copy needed (it reads RAM and writes RAM), so the buffer
not drawn into had to be given the same texels: the same points were drawn once more into it
as plain copies of what the commit made them (`--ping-pong` still does). RAM is one texture
now, what the commit needs of it as it was gathered first into a small one
(`docs/machine/gpu.md`, "RAM in one texture"). (A worker that did not run has its last pass's
stores in the targets: it draws no points.)

A pass's commit on the graphics card, one core busy: 0.024 ms where it was 0.062. `nxray`'s
picture of 640 x 480 with 63 workers of the smallest size is 1.19 s where it was 1.35 s
(677 thousand rays a second, 58 times core 0 alone), and with 15 workers 4.29 s where it
was 4.50 s. `nxpath`, 16 cores: 100 thousand rays a second where it was 89.

In Unity this is a mesh of points, so a camera's pass, as the tick in eight targets is ("In Unity").

## Workers with nothing to do

A worker's `pause` is a sleep until the first word of its job changes, and the machine does not
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
| `mcwx.h`, `mcwx.c` | over it: a parallel loop, calls joined later, a job's prints, deferred calls, scratch and failures, counters ("The second layer") |
| `mctest.c` | the test |

A job is a function of the program and two words for it:

    static uint32_t sum(uint32_t words_at, uint32_t count) {
        const uint32_t *p = (const uint32_t *)words_at;      // the program's own memory
        ...
    }

    int n = mcw_open(3);                                      // how many workers there are: 0 on a machine of one core
    for (k = 1; k <= n; k++) mcw_post(k, sum, (uint32_t)part(k), words(k));
    ...this core's own share...
    for (k = 1; k <= n; k++) { mcw_wait(k); total += mcw_result(k); }
    mcw_close();                                              // before leaving: atexit(mcw_close) will do

`mcw.c` uses nothing of the C library (it makes its system calls itself), so it links into a
program with any runtime: `programs/mc/build.bat` and `linux/ralert/build.sh` show two (clang
with the small runtime, and the image's toolchain with musl).

What a program has to keep to:

- **A job computes, reads and writes memory, and returns.** It may make system calls, which
  core 0 makes for it (two passes each). What it may not is use what the C library keeps one
  of for the whole program while core 0 uses it too: `malloc`'s heap, `stdio`'s buffers. One
  side at a time: a job may allocate and print while core 0 only waits for it.
- **A store is seen by the other cores one pass later.** `mcw_post()` followed by this core's
  own work is the usual order: the pass that ends next carries the job and its data over
  together. `mcw_wait()` ends passes until the worker has answered. Code that both cores run
  at once must not expect to see the other's stores at all.
- **A job posted to a worker still at another runs after it**: the worker looks at its job
  word again before it sleeps (`mcw_loop`). A worker sleeps until that word differs from what
  it was when the pass began, so in a program linked before 19 October 2026 such a post was
  never run (Half-Life's logo film on a worker, then a libmedia loop that took that worker).
- **Sixteen bytes, one writer a pass**, and this holds for all of the program's memory now:
  two variables side by side are one texel. Give a worker whole texels to write: rows of a
  picture whose width is a multiple of 16, buffers from `mcw_alloc()` or `posix_memalign(16)`
  rounded up to 16, its own stack. Globals that a job writes want `aligned(16)` and a size
  of whole 16s.
- **Memory the program has not touched yet stops a worker** (with paging, as a program's first touch was; without it a
  program's memory is all there from its start, `docs/system/nommu.md`) until core 0 looks
  (`mcw_wait()`, `mcw_done()`): two passes a page. Touch a job's buffers first where that
  matters (`workers_alloc` in `linux/ralert/workers.c`).
  The mailboxes and GPU memory (`seglMemory`) are there from the start, but not the program's
  to write until it has stored there: a worker's first store stops it. `mcw_open()` stores
  to the mailboxes' pages (every worker stopped at its own at its first step, 255 stops at
  a program's start); a program stores to the GPU memory its workers will write.
- **A job that waits for another core's stores ends its pass** (`mc_end_pass()` in `mc.h`:
  `fence.i`, which ends a pass and does no more) and looks again in the next. Its pause is a
  sleep until its job changes, so it used to turn in place: 8,192 instructions a pass for
  nothing, and a pass in which some worker runs to the end is 3.2 to 3.4 ms where one in
  which none does is 2.0 to 2.3 (`physics.md`, `raytrace.md`).
- **A worker keeps 3 KB of stores a pass** (a small one of the hybrid geometry 0.75 KB, core 0
  6 KB: "Packing the small workers"), and runs on until its job is done
  however many passes that takes. For work that is all stores, the cores' number is the gain.
- **Ask for what is needed, not for all.** `mcw_open(n)` gives up to n of the cores no other
  program has, and they are that program's until it closes them: a program that takes every
  core leaves none for the next. It may get fewer than it asked for, or none, and does the
  work itself then.
- **No workers is the common case to allow for**: D3D12, `--no-mrt` and `--profile` have one core.
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
- **Not a job a round for many workers.** Giving 255 jobs out is four passes of the first
  core, and the last worker begins that much later: a worker's job is all the rounds, and it
  goes from one to the next by a word the first core writes (`trace_pictures` in `nxray.c`,
  `share_job` in `nxphys.c`).
- **A worker's time is its instructions.** Fewer loads for more instructions (float constants
  from integer registers) is faster by what an instruction costs the first core and was 4 to
  5% slower on 255 workers (`raytrace.md`).

What it costs: nothing to speak of while the workers sleep; 5% of a pass for the first busy
one and 17% for three; and the commit draws each busy core's stores ("The commit as points").

## Lanes

A worker's pixels can each run a function on an index of their own, and a field of 3,968 lanes of no
core's takes a job from any program's first core (`docs/machine/lanes.md`): `mcw_lanes`, `mcw_field_begin`,
`mcw_shade`. The lean tick has them; `rvc_cpu` and `rvc_opt` read their CSRs as 0, and the library makes plain
calls there.

## The second layer (`mcwx.h`)

Fourteen programs open workers, and each wrote the same things again round `mcw_post()`:
the shares of a loop, rounds driven by a word, buffers touched first, a log a worker fills for
core 0 to print, calls kept for core 0, errors raised after the job (`server_shaderemu.c` is
the fullest). `programs/mc/mcwx.h` and `mcwx.c` are those, over `mcw.h`, with no C library
(`linux/tcc/build.sh` puts them in the machine's own `libmcw.a`):

- **`mcw_for(n, fn, ctx)`**: `fn(ctx, from, to)` over pieces of `0..n-1`, every core taking
  every so many (four pieces a core unless `mcw_for_opts.grain` says), core 0 too unless
  `first_idle`. `out_bytes` makes a piece whole 16 bytes of an output in a row; `touch` is
  made present first. The workers are a team: a job posted once that waits on a round word
  (`mc_end_pass()` between looks) for `mcw_for_linger()` passes, 16 at first, and leaves;
  the next round posts again only to those that left. A worker with no core now has no
  piece. With no workers, asked on a worker, or asked inside a round's piece, it is one call.
- **`h = mcw_call(fn, a0, a1)` ... `mcw_join(h)`**: the call on a worker with nothing to do
  (not one holding a call, nor one still in the team: `mcw_for_end()` sends the team away
  and waits until it has gone, as before a plain `mcw_post()`),
  or made at once on core 0 when there is none. `mcw_ready(h)` asks without waiting.
  Which worker: the lowest free one whose core has the largest write cache (`call_pick`; `mcw_fast(k)`: cores 1 to 16;
  `geotune`'s hybrid has 16 cores of 2^5 buckets and the rest of 2^3), then the lowest free one.
  A job that stores much ends its passes on a full cache on a small core: Quake III's server ran
  2.9 thousand instructions a pass on core 26 against 7.6 on core 4 (28 October 2026; its parked
  picture decoders had taken the lowest workers, `docs/games/quake3.md`). Round the row (before
  830fbdb, and again 19561b9) spread a program's calls over small cores too. `mcw_call_worker(h)`
  and `mcw_core(k)` say where a call went, for a program's statistics.
- **What a job may do that only core 0 may**: `mcw_print()` (a log of 2 KB a worker, written
  at the join; a full one is written then and there by a system call), `mcw_defer(fn, a0, a1)`
  (made by core 0 at the join, in order), `mcw_scratch(bytes)` (memory of the core's own, in
  chunks of 64 KB it maps; `mcw_scratch_reset()`), and `mcw_fail(what, value)`: a jump back
  to the job's start, and core 0 calls `mcw_on_failure()`'s function at the join (by default
  it says where and the program ends). On core 0 all of these are done at once.
- **Counters** (below).

`mctest` has a test for each, and `mctest N 0 race` stores to one 16 bytes from two workers
for the race checker. With three workers and with 255, on `rvc_cpu` and on the shader
machine, all pass. On the shader machine eight rounds over 256 KB take 1.05 s (three workers)
and 1.07 s (255), and 64 rounds of 16 KB back to back 0.23 s and 0.65 s.

### Counters

A counter is a word every core may add to in the same pass, with no lock: 1024 of them, 64 for
each program that has workers (its slot, the low four bits of its tag). Worker k adds to its
own region of the counters (`MC_COUNT_REGION_AT(k)` at `0x80C10000`, core 0 the first) with
`amoadd.w`, which is one instruction on one core, and sets the word "added" of the counters'
head, a control word at `0x81000320` (`{ added, "ERGM", merges, passes }`: the last, the
commits so far, is what a monitor measures a core's 8,192 instructions a pass by). Regions only grow. The
host reads the control words back every frame; when it sees "added" it draws the 256 texels of
the totals after the commit, each the sum of its word over the 256 regions, and the control
pass clears "added" and counts the merge (`count_total` in `src/gpu.h`, `kCountZone`). A merge
at any time is right, since it adds up everything there is: it comes a frame or two after the
adds. `mcw_get()` is the last merge's total and this core's own adds since (it keeps them,
counter by counter, with the merge count it saw); `mcw_counters_settle()` waits for a merge
after the last add. Two cores may get the same answer, so a counter is a sum, not a ticket.

A machine that does not merge (the world's: no `_CountMerged`) never writes "ERGM", and the
library adds the 256 regions itself at each `mcw_get()` (`RVC_COUNT_MERGE=0` runs `rvc_cpu`
so). `rvc_cpu` merges after every pass in which a core added.

What it costs: the merge drawn every frame, as the first try did over the regions too (65,536
texels cleared each frame), was 8.5 us a frame, measured as 50 draws a frame against none
(0.438 and 0.013 ms of the device's GPU time, two runs each): half a percent of a shell loop's
1.8 ms frame. Over the totals alone, every frame, it was still 6 us (0.019 ms): a draw's own
cost, whatever its size. Drawn only after "added" it is nothing while no program counts (0.013 ms).

## Races (`RVC_RACES`)

On the shader machine every core reads what the last commit left, and the highest core's
16 bytes win at the next; on `rvc_cpu` the cores run one after another and see each other's
stores at once. So a program right on `rvc_cpu` may be wrong on the card, now and then.
`RVC_RACES=FILE bin\rvc_cpu.exe ...` gives the cores memory as the card has it, and says where
that made a difference:

- **as it was**: a core read bytes another core had changed in the same pass, and is given
  them as the pass began. A core that waits on a word another writes (a round, a publish
  count) does this every round, and is right to: it sees the word a pass later.
- **undone**: a core changed 16 bytes another, lower core had changed in the same pass. The
  higher core's 16 bytes, as it saw them, are what is left, as at the GPU's commit: the
  lower core's change is lost. This is always a fault.

Since the program then runs as on the card, its own checks (sums, `mctest`) say whether what
it was given mattered. A store that changes nothing is no store (the machine keeps none); the
mailboxes and the counters are left out, being written by several cores by design. A worker is
woken by its mailbox as the pass found it (a start, a job or an answer core 0 wrote in the pass
is seen in the next), and reads its mailbox's words so too: a worker that had not slept since its
last job read its job word itself, and took a job core 0 posted in the same pass at once, a pass
before the card's would, beside what core 0 stored before posting (ClassiCube's frame job against
the next frame's set-up: 4 undone in one build's layout, 29 October 2026; none with `RVC_BESIDE=1`,
which always kept the mailboxes so). At the end it prints the counts and the eight commonest places; the file
has them all, and `python tools\mc_candidates.py CALLS ELF --races FILE` names them.

`mctest 3 0 race`: two workers store to the two halves of one 16 bytes, pass after pass. Plain
`rvc_cpu` keeps both (`49 0 49 0`); the shader machine and `rvc_cpu` with `RVC_RACES` lose the
first worker's (`0 0 49 0`), and it reports 49 undone at `race_job+0x1c`. `mctest` with 3 and
255 workers has nothing undone and passes; its reads as they were are all `team_job`'s look at
the round word, one a worker a round (192 and 16,320). nxpath has nothing; nxray has 1,275 reads
as they were, all of its `go` words in `trace_pictures`, and the same four sums.
Not covered: the parallel copy and fill (`memop`), which only the kernel makes.
Before the bytes were compared, two places were a word read beside a word written (`page`
and `count` of `mcw.c`, in one 16 bytes); before the reads were given as the card gives them,
every read of a picture after its publish word was reported too.

**Passes apart** (`RVC_RACES_NEAR=N`): `rvc_cpu` ends every core's pass at its
budget, but on the card a pass ends where it ends (the write cache full, a sync, a rest), so two stores of different
cores that fall a pass or two apart here may fall in one pass there, and the whole-texel commit lose one. With it every
change to a texel that another core changed other words of within N passes is counted and named (`near` lines in the
file: the core, pc and words of each). Half-Life's three shared texels (632e982: `world_ahead` beside
`iTouchLinkSemaphore`, the loopback's `get` and `send`, `net_drop`) are found so, never by `rvc_cpu`'s own schedule.
It is a report, not a fault: a hand-over by a job word (core 0 fills a texel, the worker it wakes writes the next words)
is one, and every game has thousands. `mctest 3 0 near` stores the two halves of one 16 bytes in turns, a pass each:
`RVC_RACES` alone finds nothing (`50 0 50 0`, 0 undone), with `RVC_RACES_NEAR=4` 99 changes at `near_job`, 1 pass apart.
`tools/test_all.py --races` runs every case with `RVC_RACES_NEAR=4` and prints each case's count beside its PASS.

**A part of a long program** (`RVC_RACES_AT=TEXT`, `RVC_RACES_UNTIL=TEXT`): full speed to where the console says the
first text, the check from there, and the run's end where it says the second (`[races] RVC_RACES from pass N`, `until
pass N`); a run whose console never said the first prints `not checked` and no count. A case of `tools/test_all.py`
names them as `race_at` and `race_until`.

## Finding the work (`RVC_CALLS`)

A job pays from some tens of thousands of instructions (a pass each way, above). `RVC_CALLS=FILE
bin\rvc_cpu.exe ...` counts, for every call core 0 makes, its own instructions until it returns
(a stack for each program and one for the kernel; what the call calls is in it, the kernel's
work for it is not, and a function that recurses counts its outermost call), and
`python tools\mc_candidates.py FILE ELF` names the functions from the unstripped program and
ranks them by instructions a call, the program being the context whose calls are most in its
functions. Quake's `timedemo demo1` (the unstripped build in WSL, the same bytes as `quake.bin`
stripped): `CL_ParseServerInfo` 45.0 million in its one call, `GL_BuildLightmaps` 27.5
million, `Mod_LoadAliasModel` 255 thousand a call (41), `R_ParticleExplosion` 171 thousand (17),
`Host_Frame` 114 thousand on average and 48.5 million the first. A run is three seconds.

## A core that rests

The first core of a program whose workers have the work has nothing to do until the next
pass, and said so with the pause hint in user mode, pass after pass. The pass ends either
way; but the kernel took the program for busy (nothing else got the core, and the idle
loop's own work was not done), and `tick_util` counted every such pass as the program's.

- **`mcw_rest()`** (`mcw.h`) is what that core calls instead: the `REST` call of `/dev/gpu`
  (`shaderemu_gpu_rest` in `linux/kernel/shaderemu_gpu.c`, `_IO('G', 11)`). The kernel lets
  whatever else can run have the core (`yield()`, when another task is runnable); with
  nothing to run it does the idle loop's part (a block of the zero pool cleared ahead,
  `boot.md`) and ends the pass there with `pause`, then wakes the console's thread as the
  idle loop does. It comes back in a later pass, never the same one.
- **The library rests where it waited**: `mcw_wait()`, the waits of `mcw_open()` and
  `mcw_close()`. `nxphys`, `nxvideo`, `nxray`, `nxpath` and `nxplay` rest in their own loops,
  and the viewer's first core while the codes of its strips are not there yet.
- **With no workers open, or a kernel from before the call, it is the pause hint** (the
  call answers ENOTTY once and is not made again). It is the first core's only: a job on a
  worker has the pause hint, which is its sleep.
- **What it costs**: 322 instructions from the call to the return against 194 for a system
  call that does nothing (`getppid`), where the pause was one. The core had nothing else to
  do with them, and what another core stored is seen at the pass's end either way: 120
  substeps of `nxphys`'s wall took 1,344 ms before and 1,329 after.
- **`tools\tick_util.py` counts a pass that ended in `shaderemu_gpu_rest` apart**, as it
  does one that ended in the idle loop, and says how many on a `REST` line: utilization is of
  the passes in which the core had work. Only that call counts: a program's own pause is
  still a busy pass that ended early. A pass in which the rest cleared a block ahead ends
  in `zeroed_idle` and is an idle one.

| the first core | before | after |
|---|---|---|
| `nxphys`, the wall on 207 workers | 27.4% (2,724 of 3,086 passes ended in its pause) | 99.8% of 367 busy passes; 2,862 rested |
| `nxphys`, 120 substeps and out | 62.6% | 98.2%; 333 rested |
| `nxview` on a JPEG | 65.0% (429 passes paused) | 96.2% of 732; 347 rested; 418 passes with a worker at work, as before |
| `nxvideo`, the sample, 2 ms a pass | 47.2% (10,680 of 11,892 passes paused) | 98.0% of 1,387; 10,521 rested; none left out |

(12 October 2026, with another program on the card: passes and fixed timesteps, not seconds.
`nxray` and `nxpaint`'s start were 98.2% and 97.2% before and were not measured again.)

Left with the pause hint, because they are not worker programs and have no `/dev/gpu` of
the library's open: the OpenGL library's wait for the GPU to have drawn (`next_frame()` in
`gles.c`, `gl.c`, `scr_shaderemu.c` and ClassiCube's), the NES's last part of a frame
(`frame_paced`) and Doom's (`doom_video.c`). The same call would serve them.

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
registers (564 in the 576 pixels of its two zones then, 500 now: "The geometry"), and starts every pass with empty TLBs
(`tlb_state_load` in `src/mmu.h`). There was a kind of worker without paging before this
one, which ran bare code in an arena of its own; it is gone, and nothing was lost in speed:
`mctest`'s figures and Red Alert's movie are the same. On the machine without paging
(`NO_PAGING`, `docs/system/nommu.md`) a worker walks nothing: every address is physical.

## A use: Red Alert's movies

`docs/games/ralert.md` ("Movies, and the worker cores"; `linux/ralert/workers.c` over the
library above): the opening movie's frames are decoded by
core 0 and three workers into a picture the GPU shows, and played in 18.6 s where they took
41.4. It is the kind of work the workers are for: every pixel stored once, and a core keeps
only 6 KB of stores a pass. The window system's pool of window buffers ends below the
mailbox pages (`POOL_SIZE` in `linux/nanox/scr_shaderemu.c`).

## A use: Quake's server

`docs/games/quake.md` ("The server on a worker core"; `linux/quake/server_shaderemu.c`): a part of
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

## Shared between programs

The kernel shares the worker cores out (`linux/kernel/shaderemu_gpu.c`); the library every
program links (`programs/mc/mcw.c`) does its part, and no program had to change.

- **A share, not a claim.** A program says how many workers it wants (`mcw_open(n)`; the
  `WORKERS` calls on `/dev/gpu`, as often as it likes). The cores are divided evenly among the
  programs that ask, nobody more than it wants, the small wants filled first: of 63 cores,
  one program that wants 63 has them all; a second that wants 40 has 31 and leaves the first
  32; a game that wants one has one whoever else is there. A program that only borrows
  (`mcw_lend()` before `mcw_open()`: the monitor, which draws its plots with them) has what
  the others leave, down to none.
- **A core is taken back between jobs, never during one.** A core over its program's share
  that has no job (its mailbox's job and answer numbers are the same) is parked and given on
  at once: the kernel sets its word at `MC_OWNER_AT` to 0, which the library sees before it
  writes the next job there. One at a job has its bit set at `MC_RECALL_AT`, and the library
  gives it up (`MC_GIVE`) when the job has ended. A program sets its word at
  `MC_POSTING_AT` while it writes to a mailbox, and the kernel takes none of its cores then:
  so a job is never written to a core that has become another program's.
- **A program's workers keep their numbers.** `mcw_open()` answers with the share. A worker
  whose core is gone, or not there yet, has none: a job posted to it is run by the core that
  posts it, there and then, so everything still happens, slower. When cores come free the
  count at `MC_OFFER_AT` goes up, and the library asks for its share again the next time such
  a worker is posted to or asked about; the core is started, and is the worker's some passes
  later. `mcw_bound(k)` says whether worker k has a core, for a program that would rather not
  have its own core do the job (the monitor shades nothing itself: `mcw_asked()`).
- **A core can come back to the program it was taken from.** A core parked while idle and
  given back in the same program's next claim has the same number but a cleared mailbox:
  before 18 October 2026 the library still took it for the worker it had been, posted jobs to
  it that no core ran, and a second viewer started beside a first waited for ever. `claim()`
  now drops a worker's core that its answer gives back unless the core's mailbox still says
  it is alive for it, and `look()` checks the core's owner word before its answer numbers. A
  team round's worker with no core (`post_team` in `mcwx.c`) has its share run by the first
  core, never in the job a post of the round runs inline.
- **One layout, the kernel's.** A machine of 256 cores has 255 workers, each with 3 KB of
  stores a pass, laid out the first time a program asks for any if `geotune` has not laid
  out its own at boot (`workers_layout`; "Packing the small workers"; a worker nobody has
  cost a pass 0.04 ms of commit at 64). Two layouts cannot be at once, and one
  program's would be every other's too, so no program has one: all 255 are anybody's.
- **What the sizes were worth, with passes of 8,192** (the kernel built with three workers of
  6 KB, twelve of 3 KB and 48 of 1.5 KB, and with 63 all of 1.5 KB; fixed timestep): Quake's server on its worker makes 25
  frames in five seconds on a 6 KB worker and 23 on a 1.5 KB one; a Red Alert mission, whose
  scene is on a worker, is 17.5 game frames a second either way, with the same state; the
  music player decodes fastest on 63 of 1.5 KB; the NES with its picture on twelve workers
  runs at 126% of its speed where 3 KB workers gave 135%, with as many frames drawn.
- A program that holds workers and never calls the library (a tracer between two pictures)
  loses the idle ones all the same: that is what the kernel's own look at the mailbox is for.

`mctest N SECONDS share [lend]` keeps N workers at short jobs and says every second how many
have cores (`mctest 63 14 share & sleep 4; mctest 40 5 share`: 63, then 32 while the other has
31, then 63 again; with `lend`, 20 then 0 while another wants 63, then 20; never a wrong
answer). After a change to the kernel's part or the library's: that, `mctest 3 60000` and
`mctest 63 20000` (PASS), and two programs at once whose sums are known (`nxplay` with
`NXPLAY_SUM=1` and `nxpath`: `e10b94ac` for 8 s and `2c64fd53` for two samples on the shader
machine, the path tracer with 31 workers of the two programs' 63).

What it does not do: move a job that has begun (the machine keeps a worker's registers
where no program sees them), or give a program more workers than it had when it opened them
(a program started in a crowd stays the size it began at).

## Where workers pay, and where they do not

Every program of the image was looked at for work a worker could take (October 2026). What
decides it is the pass: a core sees another's stores one pass later, and a pass is 8,192
instructions in the harness and in the world (it was 16,384 in the harness).

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
- **And memory nobody has touched stops a worker** until the first core looks: a page at a
  time, a frame of the program each if it only looks once a frame. ClassiCube's worker did
  3 million instructions in 20 seconds that way, its copy of the builder being variables
  that begin as zeros. `mcw_touch()` on the program's data before the first job mended it.
- **And the two sides must have their own data.** A game whose drawing reads and marks the
  objects its logic moves cannot have the two on different cores.

| Program | A frame | What a worker has, or why none |
|---|---|---|
| Quake (e1m5) | 515 thousand instructions, 4.85 frames a second | the server: 13.3 frames a second (`docs/games/quake.md`); since 14 and 15 October 2026 also the frame's drawing, by a worker that stays and draws a frame late, the 2D screen recorded by another, and the frame's jobs (particles, light under models, light maps, water) |
| Red Alert | 220 to 240 thousand: logic 110 to 127, drawing 88 | its movies (three workers, given back when the movie ends), and in a mission the scene: the frame's rectangles are made GPU commands by a worker while the game goes on to its next frame's logic (`docs/games/tdawn.md`, "The scene on a worker core"). Drawing 91 to 67 thousand, 16.4 to 17.8 frames a second. The logic stays one core's; since 14 October 2026 the objects are drawn a frame behind by a worker from copies, the first core keeping the marks the drawing sets (`docs/games/tdawn.md`, "The objects a frame behind") |
| Tiberian Dawn | 170 to 230 thousand: logic 100 to 145, drawing 45 to 70 | the scene, as Red Alert (the same file): drawing 51 to 43 thousand. Its start was looked at for a long job and had a slow one instead: a table sorted whole for every entry read from a file, 27% of the start, mended on one core (159 to 114 million instructions to a mission's first frame, Red Alert's too) |
| Doom | a tic 33 to 39 thousand, 35 a second; a frame 45 to 55 thousand | the view, made by up to four workers from what the first core tells them between tics (`docs/games/doom.md`, "The view on worker cores"), and since 14 October 2026 the sight checks of the tic after next, walked by a fifth (81% of the first demo's answered so). The tics stay the first core's. Its start is 52 million instructions with the machine's own, and no piece of it over 5% |
| ClassiCube | 60 thousand, 48 frames a second | the meshes of chunks that have none yet: a second copy of the builder on a worker, beside the first core's, and the world's height map and strata made by every core, rows each (`docs/games/classicube.md`). From the command to the whole world drawn: 38 s on four cores where it was 52. Since 14 October 2026 also the rest of a frame's list (the sky, the map, the outline, the held block, the 2D screen), a worker's beside the next frame's ticks |
| The window system | | none for its drawing: see below. One lent core for the pointer (`docs/system/nanox.md`, "The pointer on a core of its own") |

Loading is where the long jobs are. Quake's start to a level's first frame is 160 million
instructions (55 s), with no piece that is the server's or the client's alone: the models'
skins filled round their edges 10%, places found for the light maps 9%, the models' triangle
strips 7%, the level made for the GPU 5.5%. A model's skin is now filled on the worker while
the first core reads the rest of the model and builds its strips (`se_fill_job` in
`gl_model.c`): 17 million instructions leave the first core, and the start is one second
shorter of 54, because the fill is the longer of the two and the first core waits for it
before it makes the skin a texture. To gain what was moved, the textures would have to be
made a model later.

**The window system.** Composition is the GPU's already, and a drawing request is mostly not
computing: of the frames of the machine while `nxbench` draws (25 fills, 100 lines of text, 15
scrolls, 200 buttons: 14 million instructions in 6.4 s), 46% are the processor idle, waiting
for the GPU to have drawn a list; the kernel is 26% (sockets, the scheduler, timers) and the
server and the program together 28%. A worker makes no system calls, so the kernel's part
is not its to take, and the wait is a pass of the machine whoever waits. The desktop's own
programs were looked at too: a web page's layout asks the server for the size of every piece
of text, pictures are decoded by the host and drawn by the GPU from the file, and none has a
piece of computing that takes more than a few frames.

## What the cores did, for the guest and the host

The control pass publishes each core's count of instructions among the machine's control
words (`MC_STATS` in `src/gpu.h`), where a program reads them through `/dev/gpu` like any
other:

| Address | Contents |
|---|---|
| `0x81000380 + 4 k` | core k's instructions so far (k from 0 to 15; 0 for a core the machine lacks) |
| `0x810003c0` | how many cores the machine has (0 on a machine built for one) |
| `0x810003c4` | a bit a core: it runs (a worker that was started and has not parked) |
| `0x810003c8` | a bit a core: it is asleep on its job word |
| `0x816ba000 + 4 k` | core k's instructions so far, every core (`MC_STATS_ALL`: the network's row, after its ring) |
| `0x816ba400 + 16 n`, `+ 4` | cores 32 n to 32 n + 31, a bit a core: runs, asleep |

`nxmon`, a task manager (`docs/system/nanox.md`), reads them: on its Performance tab the workers'
instructions a second, all 255 of them, are stacked on core 0's plot (or Per core, a 16 x 16
grid of every core's load), and the numbers under it say how many work, sleep, are idle and
are parked (`NXMON_PRINT=1 nxmon` prints that line at every sample, for a test); its
Processes tab gives each program's workers from `/proc/gpu`. Its plots are the workers' own work, as a graphics card's pixel
shader would do them: it keeps eight workers while it runs, and once a sample (since 17 October 2026;
some sixteen times a second before) each writes some rows of the plots from the samples (`shade` in `nxmon.c`) straight into
the window's own buffer in GPU memory, mapped through `/dev/gpu` (`GrGetWindowInfo` says
where it is). The display shows that buffer as it is, and no drawing request is made for the plots.
The workers cost about 2.3M instructions a second (17M when the plots slid between samples),
and the Performance tab costs core 0 about 266 thousand a second over an idle desktop. Sent through the window system instead (`GrArea`, 207 KB a second)
the same picture cost core 0 1.5M instructions a second. What stands in the way of more
of this: the window system draws by taking the whole buffer to the GPU and copying it back
(`docs/system/nanox.md`), which undoes what a worker wrote meanwhile; the monitor asks for
something with an answer after each of its own drawing requests and only then posts a
frame. `NXMON_CHECK=1` shades every frame again on core 0 and must print `0 of 67200
pixels differ`; `NXMON_WORKERS=0` leaves the workers alone (core 0 then shades the newest
columns once a second). The monitor's workers are only lent to it (`mcw_lend()`: "Shared
between programs" below): when a program that keeps its workers wants them, the monitor's
are taken back; it sees that (`mcw_asked()`), closes them and shades on core 0, and takes
workers again when some have been nobody's for five seconds. So a path tracer started
beside the monitor has all 255. The harness adds them up from the rows it reads back anyway: its
`STATS` and closing lines say what the workers ran and the rate of all cores together.

## Every core's share of a game's frame

`python tools\game_par.py GAME` (or `all`) runs a fixed workload of a game from power-on on
`rvc_cpu` (no card, a few seconds each) and says its instructions a frame on core 0 and on
all cores together; the factor is the second over the first. `rvc_cpu --count-from TEXT`
takes every core's count when the console says TEXT and prints what each ran from there
(`COUNT from ...`); `--until-times N` stops at the Nth `--until` after it. `--show` prints
the command line, `--runs 3` takes the mean over three timings (`--salt`: the command typed
after `: N;`), `--profile PC.LOG` samples core 0 every 256 instructions for
`tools\pc_profile.py`, and `--calls FILE` is `RVC_CALLS`. On 14 October 2026, the mean of
three runs (passes of 8,192):

| Game | Workload | Frames | Core 0 a frame | All cores a frame | Factor | Workers that ran |
|---|---|---|---|---|---|---|
| Doom | `-timedemo demo1`, from its first rate line to the end | 4,574 | 27.2k | 56.9k | 2.09 | 4 |
| Quake | `+timedemo demo1`, from the level's first frame | 969 | 68.8k | 99.9k | 1.45 | 9 |
| Quake III | q3dm17, `addbot sarge 3`, `fixedtime 33`, put at 488 -968 378 at frame 100; frames 300 to 700 | 400 | 56.6k | 168.0k | 2.97 | 2 |
| ClassiCube | seed 7, block physics off, walking and jumping into a hill; five 2 s windows from `steady` | 1,049 | 31.8k | 31.8k | 1.00 | 0 |
| Tiberian Dawn | mission 3, seed 7, frames 301 to 1,200 | 900 | 84.1k | 122.4k | 1.45 | 24 |
| Red Alert | Soviet mission, seed 7, frames 301 to 1,200 | 900 | 52.8k | 85.1k | 1.61 | 4 |
| Red Alert | Allied mission, the same | 900 | 80.7k | 133.9k | 1.66 | 4 |
| NES | Thwaite, `docs/games/nes.md`'s 1,000 frames, `NES_LIST=worker`; frames 100 to 1,000 | 900 | 24.6k | 31.0k | 1.26 | 2 |
| nxphys | the wall, 120 substeps (a frame here is a substep) | 120 | 15.1k | 2,443k | 161 | 255 |

Three timings agree within 0.5% but for Quake III (3%) and nxphys (1%). Quake III left to
where it starts is two games: with the same build, one more word in the command line took
core 0 from 36k to 59k a frame (the player and the bot elsewhere); put at one place it is
steady. A worker that waits for its next job counts the instructions it waits in, so the
factor depends on the pass: Doom's is 2.42 with passes of 256.

Where core 0's instructions go in those windows (`--profile`, the program's symbols; LTO
puts what is inlined under its caller):

- **Doom**: the tic: `sight_walk` 16%, `P_CheckPosition` 14%, `P_RunThinkers` 10%,
  `R_PointInSubsector` 6%, `P_LookForPlayers` 3%; the view's part is 5%. The kernel 6.5%.
- **Quake**: the client: `RecursiveLightPoint` 7%, `CL_ParseUpdate` 6%, `CL_RelinkEntities`
  5%, the GL calls into the list (`qglText`, `qglEntity`, `qglRect`, `qgpuReplay`) 12%,
  `R_DrawAliasModel` 4%, particles 5%. The kernel 6%.
- **Quake III**: the scene into the GPU's list: `scene_draw` 17%, `stage_moved` 7%,
  `entity_matrix` 7%, `gpuDrawAt` and `gpuDrawRoom` 13.5%, `light_at` 6%; the server and the
  client's game are on their workers already. The kernel 7.6%.
- **ClassiCube**: no worker runs once the world is built: `BuildMeshIn` 11% (chunks meshed
  on core 0 while walking), the selection outline 7%, `MapRenderer_Update` 6%, `Matrix_Mul`
  5%, `DrawUnready` 4%, `SineFrom` 4%.
- **Tiberian Dawn**: the logic: `InfantryClass::AI` 14%, `LogicClass::AI` 13%,
  `Per_Cell_Process` 11%, `FootClass::Mark` 8%; `PowerClass::Draw_It` 11.5% (the sidebar).
- **Red Alert** (Allied): `BuildingClass::AI` 13.5%, `LogicClass::AI` 12%, `InfantryClass::AI`
  10%, the radar 8%, `TechnoClass::AI` 6%.
- **NES**: the 6502's translated code 32%, `nes_renderframe` 7%, the interpreter 5%,
  translating 4%. The kernel 7.5%.
- **nxphys**: core 0 hands out the substeps and rests (`shaderemu_gpu_rest` 23.5%); the work
  is the workers'. Its factor can only go down; what is left is the instructions.

### After (15 October 2026)

Every game was then given more work for its workers, through the second layer (each game's
document has a section of 14 or 15 October 2026 saying what). All the games built from the one
tree into one private image, `game_par.py all --runs 3 --image` it, the mean of three runs
(the figure before each arrow is the table above's):

| Game | Frames | Core 0 a frame | All cores a frame | Factor | Workers that ran | Guest seconds |
|---|---|---|---|---|---|---|
| Doom | 4,526 | 27.2k to 25.4k (-6.6%) | 56.9k to 61.4k (+7.9%) | 2.09 to 2.42 | 5 | 38.6 to 36.5 |
| Quake | 969 | 68.8k to 29.3k (-57%) | 99.9k to 102.2k (+2.3%) | 1.45 to 3.49 | 13 | 20.1 to 10.6 |
| Quake III | 400 | 56.6k to 18.4k (-67%) | 168.0k to 149.6k (-11%) | 2.97 to 8.12 | 3 | 5.7 |
| ClassiCube | 2,132 | 31.8k to 11.0k (-65%) | 31.8k to 25.7k (-19%) | 1.00 to 2.33 | 15 | 10.0 (five 2 s windows) |
| Tiberian Dawn | 900 | 84.1k to 79.2k (-5.8%) | 122.4k to 124.0k (+1.3%) | 1.45 to 1.56 | 42 | 22.9 to 21.6 |
| Red Alert, Soviet | 900 | 52.8k to 51.9k (-1.7%) | 85.1k to 78.2k (-8.1%) | 1.61 to 1.51 | 10 | 14.4 to 14.2 |
| Red Alert, Allied | 900 | 80.7k to 76.5k (-5.2%) | 133.9k to 129.5k (-3.3%) | 1.66 to 1.69 | 10 | 21.9 to 21.0 |
| NES | 900 | 24.6k to 3.3k (-87%) | 31.0k to 31.0k | 1.26 to 9.31 | 3 | 6.7 to 5.9 |
| nxphys | 120 | 15.1k to 15.4k | 2,443k to 1,427k (-42%) | 161 to 92.5 | 255 | 1.1 |

ClassiCube's frames are those of its fixed ten seconds: 1,049 before. Three runs agree within
0.2% but for Quake III (1.9%: the first run differs from the others, as above) and nxphys
(1.0%). Each figure is within 1.3% of the one its game's own image gave just before (Quake III
149.6k all cores where its image says 147.9k, Red Alert's Allied mission 129.5k where 127.9k);
a Quake III built with `rest()` in `gles.c` as it was before (a worker waiting for the GPU
sleeps, not ending its pass) is 154.9k, so it is not that.

Checked on the same image, under `rvc_cpu`: `RVC_RACES` 0 undone in Doom's first demo, Quake's
demo and e1m5, Quake III's 1,000 frames, Tiberian Dawn's and both Red Alert workloads, both
1,000-frame NES runs, the wall and `mctest` 3 and 255 (both PASS); ClassiCube's walk has 3
undone with the command as `game_par.py` types it and 0 with one word more, its own image the
same: the frame's worker in `DrawUnready` and `Gfx_SetVertexFormat` and the first core in
`Gfx_EndFrame` store to `draw` in one pass, between the 16th and 20th second. Doom's state
sums of the three demos as its image's, told and with `DOOM_PORT=rest`, 0 views not as told;
Quake's particle sums at 969 frames as its image's, e1m5's server sums the same `inline`,
`late` and `wait`, `QUAKE_LIGHTPOINT=check` 0; Quake III's scene and list sums 572 of 572
with `QUAKE3_SCENE=inline`; ClassiCube's 52 meshes as its image's, in order inline and as a
set with workers, `CLASSICUBE_CHECK_KEPT` 0 wrong; Tiberian Dawn's and Red Alert's state at
frames 300 and 1,300 as the shared image's (before), `TDAWN_CHECK_PATHS` 0 wrong, the movie 0
of 156 frames different; the NES's sums of "Testing" with the emulation and lists on workers,
on the first core and the lists there, and its 1,000-frame RAM sums, `NES_CELLS_CHECK` 0 of
1,000; nxphys `b28b77b4` with 0, 7 and 255 workers and 255 shares on none, its four other
sums, 0 pages brought for workers.

**What that table did not say.** Its core-0 figures and guest seconds were not the games' speed
on the shader machine, and in the world (the image of that commit, uploaded and played at
01:49 on 15 October 2026) neither the frame rates nor the machine's total instructions a second
rose. Three things were missing from `rvc_cpu` as it was run (now `game_par.py`'s default,
`--plain` for the old way; "A game's speed" below): the sound card (no `/dev/sound`, so a
game's mixing code never ran: Quake's `S_Update` and `heard`, some 9 thousand instructions of
core 0 a frame), the write caches and memory a pass old (`RVC_WCACHE`, `RVC_RACES`). And the
Quake row is the timedemo, which has no server: the game played is a client at its frame limit
and a server on one worker that the round did not touch (its frames a second fell, 10.7 to
10.0 on the card). Core 0's instructions a frame are not the measure; passes a frame are.

## A game's speed: passes a frame (`game_par.py --card`)

A pass is the machine's step: every core runs in it at once, and what one core stores another
sees in the next. So a game's frames a second are the passes a second over its passes a frame.
How long a pass is depends little on the cores (1.9 to 2.6 ms on an RTX 5090, by the game: the
busiest core's 8,192 instructions, floats and loads costing more; 255 busy workers of nxphys
add a fifth); and in the world the passes a second are its rounds a frame times Unity's frames
a second, whatever the guest does. What a change can move is the passes a frame: the longest
chain of work in a frame on any one core, and the passes spent waiting between cores (a job
posted is begun in the next pass, its answer seen in the pass after it ends).

`python tools\game_par.py GAME --image A --image B --card` runs the workload of `rvc_cpu` (below)
on the card in real time through `gpu_queue.ps1` and the harness lock, for each image in turn
from a folder of junctions (`build/game_par/card_NAME`; the guest's `md5sum` of the game says
which image ran), and prints the game's own frames a second, passes a second and a frame, and
core 0's, the workers' and all cores' millions a second. It will not run while VRChat or
SteamVR does. Plain `game_par.py GAME` is `rvc_cpu` with `RVC_SOUND=1 RVC_WCACHE=1 RVC_ADFAULT=1
RVC_RACES` and prints passes a frame beside the counts, and how often the workers stopped from
the mark (on a page's written bit, and core 0's faults on one: both must be 0).

The workloads before the round (`5680284^`'s `linux/prebuilt`) and after it (`5680284`'s), each
as an overlay on the same root (`build/images/perf_before`, `perf_after`), 15 October 2026, RTX
5090, D3D11 + fxc2; passes a frame on the card against `rvc_cpu` as it is now:

| Game | Card fps before | after | Passes a frame, card | `rvc_cpu` | All cores M/s, card | Core 0 M/s |
|---|---|---|---|---|---|---|
| Quake e1m5, client | 85.5 | 92.8 | 5.50 to 5.13 | 5.02 to 4.86 | 9.1 to 10.4 | 3.06 to 1.89 |
| Quake e1m5, server | 10.7 | 10.0 | | 10.0 to 9.4 a second | | |
| Quake `timedemo demo1` | 45.3 | 60.3 | 9.87 to 7.04 | 9.83 to 5.69 | 5.0 to 6.7 | 3.55 to 2.41 |
| Quake III, frames 300 to 700 | 40.5 | 55.5 | 9.86 to 7.19 | 9.66 to 6.83 | 7.6 to 8.8 | 2.64 to 1.33 |
| ClassiCube, walking | 109 | 152 | 3.83 to 3.33 | 3.93 to 3.72 | 3.4 to 4.0 | 3.37 to 1.92 |
| NES, Thwaite | 173 | 190 | 3.03 to 2.72 | 3.03 to 2.72 | 5.3 to 5.8 | 4.23 to 0.60 |
| nxphys, the wall (substeps) | 140 | 163 | 4.03 to 3.98 | 4.08 to 4.04 | 259 to 168 | 1.65 to 1.86 |
| Tiberian Dawn, mission 3 | 44.8 | 44.6 | 10.58 to 10.44 | 10.61 to 10.39 | 5.5 to 5.6 | 3.78 to 3.60 |

So `rvc_cpu`'s passes a frame are the card's within 5% in every row but Quake's after the round
(the timedemo 19% under, e1m5's client 6%), and its core 0's instructions are the card's (the
timedemo: 37.5 and 75.7 million against 38.2 and 76.0). Quake's after are short because core 0
waits for its workers more on the card (2,498 of 6,818 passes rested, 771 of them with no core
at work, mostly in the demo's first third; `rvc_cpu` 2,073 of 5,103). The card's page walk
faults on a clear accessed or written bit (`src/mmu.h`) where `rvc_cpu` set it, and a worker
stops there until core 0 serves the page: Quake's drawing worker stopped 344 times at pages of
GPU memory, mapped clean. With `RVC_ADFAULT=1` `rvc_cpu` says 7,067 passes to the card's 7,072;
Quake now writes its GPU memory once at the start (`docs/games/quake.md`, "The server's frame made
shorter"). A program whose workers first write memory core 0 only read meets the same.

Every port met it, at GPU memory: `/dev/gpu` mapped its pages with the written bit clear, and
`seglSprites`, `glDrawArrays`, `seglCompact` and the C&C games' shape decoding on workers
stopped at each page they wrote first; core 0 took the same fault for the pages it wrote first.
Since 15 October 2026 the driver maps them accessed and written (`shaderemu_gpu_mmap`), so no
program has to touch its GPU memory first (writing all 11 MB of `gles.c`'s mapping at
`seglInit` was the alternative: 2,816 faults on core 0 at every start, about a second at an estimated 1,300 instructions each, and not
for programs that map `/dev/gpu` themselves). `game_par.py` (`rvc_cpu`, card model, mean of 3,
stops from the mark: the workers' on a written bit / core 0's on one):

| Game | Stops before | after | Passes a frame before | after | fps before | after |
|---|---|---|---|---|---|---|
| Quake `timedemo demo1` (no `qglOpen` touch) | 344 / 378 | 0 / 0 | 7.06 | 5.66 | 68.0 | 75.6 |
| Quake e1m5, client | 26 / 47 | 0 / 0 | 4.86 | 4.87 | 93.7 | 93.8 |
| Red Alert, Allies | 59 / 60 | 0 / 0 | 10.51 | 10.33 | 40.6 | 41.1 |
| Red Alert, Soviet | 10 / 11 | 0 / 0 | 7.35 | 7.32 | 59.9 | 60.1 |
| Tiberian Dawn | 23 / 29 | 0 / 0 | 10.47 | 10.40 | 39.8 | 40.0 |
| NES, Thwaite | 1 / 1 | 0 / 0 | 2.76 | 2.76 | 149.0 | 149.1 |
| Doom | 0 / 54 | 0 / 0 | 4.04 | 4.09 | 111.0 | 110.0 |
| nxphys | 0 / 12 | 0 / 0 | 4.06 | 4.06 | 151.2 | 152.0 |
| Quake III, ClassiCube | 0 / 0 | 0 / 0 | 6.98, 3.71 | 6.82, 3.72 | 57.9, 129.8 | 60.1, 129.8 |

On the card (RTX 5090, D3D11 + fxc2, `game_par.py --card`, once each): the timedemo 7.07 to
5.70 passes a frame, 77.5 to 83.0 frames a second, all cores 8.65 to 9.07 million a second;
Red Alert's Allies 10.48 to 10.25 passes a frame (52.3 and 51.8 frames a second, the card's
passes a second 548 and 532). Doom's 1% more passes holds at every salt; it had no worker stop before either (its cause not found). Elsewhere a change is judged on
`rvc_cpu` by its passes a frame (and the game's own fps, as guest seconds it is within 15%
of real ones), the card being the check of a round's end. The total instructions a second
rise only as far as workers run beside a core 0 that stays busy: 10 to 20% here.

Where the critical path is now, on the card: Quake III's server, 342 thousand instructions a
frame on one worker, makes 16 frames to the client's 100 (`q3stat:`); Quake's server 300
thousand on one worker, 10 a second; nxphys goes no faster than the boxes' time (60 substeps a
second, `PHYS_HZ`) and drew at most 25 pictures a second (now one a substep: `physics.md`, "A
picture every substep"), while unpaced a substep is its dearest share's one body, 3.08 passes
to `physstat: done` (2.4 in its steady run; `game_par.py`'s 4.04 has the jobs' start and the
program's exit in it), the shares waiting for each other and not for core 0; the NES's emulation is one job of 21 thousand instructions
a frame on one worker (2.7 passes); Tiberian Dawn's frame is core 0's logic, 79 thousand.

## In Unity

The VRChat machine is the one without an MMU (`NO_PAGING`, `docs/system/nommu.md`) with the harness's
256 cores, the guest's geometry, the tick in eight targets, the commit as points, the shader's
own write cache (336 texels: 64 + 32 + 16 buckets of three) and passes of 8,192 (the control panel's `tickSlider` sets 1,024 to 32,768)
(`CORES`, `L1_TABLE_BITS` and the sizes in `MachineBlit.cginc`; the tick and the commit are
`experiments/rvc_opt/main.shader`'s, line for line where they deal with cores).

- **The tick is a camera's pass.** `MachineTick.shader` is on a mesh of sixteen triangles (one for every
  sixteen cores) that only the tick's camera sees (a layer of its own); `EmuMachine` gives the
  camera eight targets of 1040 x 32 once (`Camera.SetTargetBuffers`, which Udon has) and renders it once a round.
  `tick_geom` draws core 0's pixels and a block of columns for each worker with something to
  run, as the harness's does, and draws nothing for any other camera (it looks at the target's
  size).
- **The unpack** (`Machine.shader`'s third pass, a Blit into the 2048 x 64 texture the commit
  reads) puts the targets' texels where the state has its rows, and takes the state as it
  was for whatever the tick did not draw: a worker with nothing to run, the band at start.
- **The commit** writes RAM in place (`docs/machine/gpu.md`, "RAM in one texture"), in three
  draws: `MachineGather.shader` on 257 points, which the points camera draws into the stage
  (2048 x 21); a Blit of the state rows from the tick's texture and the stage; and
  `MachineCommitPoints.shader` on 33,410 points that the same camera draws into the state, each
  point the rectangle it stands for and each texel the commit's own (`MachineCommit.cginc`,
  which all three share). The two meshes are on one layer and `EmuMachine.Commit` enables one
  at a time. A copy out of the ROM is a point too: the points' material needs the ROM's
  textures (without them Linux started and could not read `/emuinit`), and RAM at start is
  point 0, from the image's textures.
- **The control pass** draws its 11 rows of RAM into a window of their own (2048 x 11:
  `MachineWindow.cginc`), which the `WindowBack` pass copies into the state.
- **The readback** has state row 0, the first 64 control texels and 130 texels of the
  network's row, its window, what the cores ran and which sleep, then the console's words and
  ring (`Readback.shader`, 1032 words; `console.md`, "Words in memory"), where the counts are: the panel's statistics have a line for the workers and for all cores together.

- **The host's clock moves every round** (`HostClock` in `EmuMachine.cs`): a frame's length
  spread over its rounds, as the harness's moves every pass. It moved once a Unity frame, and a
  program that waits for it (Quake after each of its frames) ran one frame of its own a Unity
  frame and ended the other rounds at once: a third of the rounds on e1m1.
- **The workers' counts are read ten times a second**, from a frame's last row. Read every
  round, 255 workers were 5.5 ms of an 8-round frame in Udon, and 43 ms of a 32-round one.
- **The three cameras are preview cameras** (`Camera.cameraType`, which Udon may set: `SetPaths`
  in `EmuMachine.cs`). A game camera's `Render` prepares every canvas of the room first, 82
  of them: 0.30 to 0.33 ms a call on the main thread, of which the canvases were half; a
  preview camera's is 0.13 ms and draws its mesh all the same.
- **The GPU's camera draws every round only while lists come.** When none has come for a
  second it draws in a frame's last round, and the control pass takes a list in no other
  (`_GpuDrawn` in `Machine.shader`): the first list after a pause waits a frame at most. A
  round is then three `Render` calls where it was four.
- `oldPaths` of the machine's behaviour is the switch for both (bit 0 game cameras, bit 1 the
  GPU's camera every round): set from editor code in play mode it gives before and after
  from one session. Bit 4 is a test: that camera in a frame's last round only, whatever
  comes, so that every list is drawn late.
- **Pictures under those switches.** After a report of stray pieces beside a unit on some
  scrolls of Tiberian Dawn in the world, its scripted scroll (`docs/games/tdawn.md`, "A scrolled
  picture", stopped at frame 150) was run in play mode with `oldPaths` 3 twice, 0, 2, 16 and
  17, every new picture of the decoded display kept (some 175 a run): the held last picture
  is the same in all six but the pointer's pixels, the pixels that are one thing for a single
  picture (the scroll taken out) are the same game's events in each (13 pictures over 300
  such pixels, one over 1,000, in every run), and no picture has a row of 24 red pixels. With
  `oldPaths` 1 the pointer lay at the window's edge and dragged the map all the run: nothing
  either. While a game plays lists come every frame, so the camera is as it was before these
  switches: only the first list after a second without one is drawn late. Doom's menu over a
  running demo, 141 pictures one after another with `oldPaths` 0, 3 and 16: the logo's 23,072
  pixels are the same in every one of a palette. The report was not reproduced.

What those two are worth (13 October 2026, RTX 5090, the Game view in front at 910 x 583, a
shell's `while :; do :; done` beside the idle desktop, windows of 4 s in turn, `oldPaths` 3
against 0): 8 rounds of 8,192, 3.82M instructions a second to 3.91M (2%); 16 rounds of 4,096,
2.49M to 3.79M (52%); 4 rounds of 16,384, 4.01M to 4.05M. At 8,192 a round the frame was the
card's before and after: a round more is 1.93 ms (the slope from 8 rounds to 16), 4.24M a
second, and a frame has 1.2 ms besides its rounds (the room, the editor). With two
instructions a round a round is 0.87 ms of the main thread's where it was 1.67 (1.5 and 3.2
with builds on all the processor's cores beside it). So what is left of Unity's own in a
frame of 8 rounds of 8,192 is 8%, and the rest is the tick. With the processor busy
elsewhere the main thread is the limit at any round length and one window differs from the
next by a third: compare only windows taken in turn.
Quake's e1m1 is 3.1M either way (lists come all the time, the frame is the card's).

All the cores together, 8 rounds of 8,192: `nxphys` 660 to 680 million instructions a second
on 207 workers (every worker a full pass every round: 400 rounds a second; it asks for 255
now), `nxray` 295 to
318 million on 255. A round with the workers busy is 2.5 ms where core 0's alone is 2.1, so
what limits the workers in the world is how many rounds a second there are, nothing of their
own.

The tick's changes of that day (the instruction window turned, the data texel read before
the cache is searched, the float instructions split: `cpu.h`, `emu.h`, `mem.h`, `mmu.h`) in
the world, the tick before against the tick after as two shaders on the tick's material,
changed in turn in one play session, four windows of 4 s each, 8 rounds of 8,192: a shell
loop 3.884M to 3.896M (0.3%), Doom's `-timedemo demo1` 3.554M to 3.700M (4.1%), Quake's e1m1
3.107M to 3.304M (6.3%); 4 rounds of 16,384: 3.954M to 4.002M, 3.555M to 3.613M (one pair),
3.139M to 3.238M. The harness had 0.7%, 4.6% and 7.3%. With them both `mctest` lines pass,
`nxray`'s sum is `69ee69f8` and `fptest` says `0 of 15 instructions wrong`.

The second lot of that day (`fast_exec_l1` telling the kinds apart by a tree of tests, one
way out of `mem_get_cached_or_tex_l1`, the float result written without a branch: `emu.h`,
`mem.h`), measured the same way over the first lot, 8 rounds of 8,192: a shell loop 4.025M
to 4.192M (4.2%), Doom's timedemo 3.648M to 3.780M (3.6%), Quake's e1m1 3.300M to 3.527M
(6.9%); 4 rounds of 16,384: a shell loop 4.115M to 4.289M (4.2%), Quake 3.250M to 3.405M
(4.8%). The harness had 4.1%, 4.1% and 8.8%. Doom at 4 rounds of 16,384 says nothing in
windows of 4 s: a window is 2.6M to 3.7M by where the demo is, and eight pairs gave from 12%
under to 14% over. The same checks pass. A session's level differs from another's by 3%
(the first lot's tick was 3.90M on the shell loop in its own session and 4.02M in this one):
only the two sides of one session compare. From the morning's tree to this one, each step
within its own session: a shell loop 3.82M to 4.19M, Doom 3.55M to 3.78M, Quake 3.11M to
3.53M.

**What is between the world and the harness is a frame's, not a round's.** The harness the
same day, the same tick, `--fixed-dt 0.004 --bench` from a snapshot: a shell loop 4.58M to
4.70M (1.74 to 1.79 ms a pass), Doom's timedemo with `-nodraw` 4.52M (1.81 ms), Quake's e1m1
3.95M to 3.96M. In the world a frame of 8, 16 and 24 rounds of 8,192 is 15.64, 29.89 and
43.4 to 44.1 ms on the shell loop (4.17M, 4.32M, 4.47M a second) and 15.75, 29.82 and 43.46
on Doom with `-nodraw` (4.16M, 4.39M, 4.47M): a round more is 1.78 ms and 1.75 ms, the
harness's pass, and a frame has 1.4 ms that is no round's, 9% of a frame of 8. Of it the
room's two cameras are 0.65 ms (both off: 4.16M to 4.33M; no one thing of theirs is it: the
machine's screens off 0.4%, the city 0.3%, the effects 0.9%, the menu's camera 0.5%, no
MSAA 1.2%, the tick's, the points' and the GPU's meshes out of their culling masks nothing),
and 0.75 ms is left with them off (the editor's own drawing, the frame's end; not taken
apart). The main thread is not it: it waits 5.4 ms of such a frame for the render thread.
The tick's bytecode is the harness's: the editor's compiler is the same file as the
harness's fxc2 (`Data\Tools\fxc2_d3dcompiler.dll`), and the two fragment programs are the
same 26,740 instructions but seven at the start, where the constants are read. So the same
rounds a frame cannot give the harness's speed in the world; more rounds a frame can (24
rounds: 3% under it).

With the image of 13 October 2026, 15:03, in the world at 8 rounds of 8,192: Quake's e1m1
92 frames a second of its own (22 thousand instructions a frame: its frame limit, the first
core at 2.04M a second and resting), Doom's `-timedemo demo1` 5,026 tics in 43 s (117 a
second; the first core 3.36M to 3.73M), ClassiCube 150 lists a second (3.66M), `nxphys` 62
substeps and 21 pictures a second on 255 workers.

Tried and not kept, on Quake at 8 rounds of 8,192, in turn: the display decoded one frame
in eight (3.30M and 3.35M against 3.30M to 3.34M: nothing, so a decode skipped while the
display stands still would gain nothing); one `Render` more of the points a round, to see
what one fewer would give (3.33M and 3.29M against 3.30M: nothing, so the points drawn by
`Blit`, a patch tessellated into them, was not written).

Quake's e1m1 from where it starts, 8 rounds a frame, the Game view 2276 x 1406 (13 October
2026, RTX 5090): 2.3 to 2.5M instructions a second on the first core before those two and
Quake's frame limit (`docs/games/quake.md`), 3.08 to 3.15M after; the harness 3.46M. A frame there
is the card's: eight ticks of 2.3 to 2.7 ms (RenderDoc: every other draw of a round together
0.13 ms), the room 0.6 ms, and the main thread alone would need 15 ms (four `Camera.Render` a
round at 0.26 ms, 0.10 of it the room's canvases). In the editor, measure with the Game view
in front: play mode started by a script leaves the Scene view there, and the room is then
drawn small.

"ShaderEmu/Add the tick's and the commit's cameras to the open scene" puts the cameras, their
meshes and the targets into a scene without building the world again.

`mctest 15 60000 shape 6,6,6,5,5,4,4,4` passed there, and `mctest 63 20000 shape 3,3,...` (`mctest` has no `shape` now: "The geometry"). `RAY_FRAMES=1 RAY_STILL=1 nxray` gives the
sum `69ee69f8` on core 0 alone (18.9 s) and with the kernel's fifteen workers (1.46 s; with
the tick in one target and the commit in bands, 20.0 s and 2.0 s).

On an RTX 5090, the editor not in front, frame cap off, a shell's `while :; do :; done` on
core 0 (the slope between two numbers of rounds is a round):

| Instructions a round | A round | Core 0, in rounds alone |
|---|---|---|
| 2 (what a round costs with nothing to run) | 1.0 to 1.4 ms | |
| 4,096 | 1.35 ms | 3.0M a second |
| 8,192 | 2.6 to 2.8 ms | 2.9 to 3.1M |
| 16,384 | 4.4 ms | 3.7M |
| 32,768 | 8.7 to 9.3 ms | 3.5 to 3.8M |

The harness on the same card and the same loop: 3.86M a second (a tick of 8.45 ms, a commit
of 0.016 ms), and 3.47M with one target, passes of 16,384 and the cache as it was. So a
round in Unity is the harness's tick and about a millisecond of the script's and the three
cameras' own work, which a pass of 32,768 hides and a pass of 4,096 does not. The default
there is 8,192 a round all the same (`TicksPerRound`), as everywhere: one pass length and
one cache for the harness, `rvc_cpu` and the world. The table above is of the build before
that (16 cores, 768 texels of cache).

(Before this, with the tick a Blit into one target, a round of 8,192 was 1.65 to 1.72 ms with
core 0 on `mctest`'s primes, 4.5 to 4.85M a second. That is another load than the shell's
loop and was not measured again.)

## Switches

    RVC_RACES=FILE     rvc_cpu: memory as the GPU has it, and where that mattered ("Races")
    RVC_CALLS=FILE     rvc_cpu: core 0's calls by their instructions ("Finding the work")
    RVC_COUNT_MERGE=0  rvc_cpu: no merge of the counters, as the world's machine
    RVC_SOUND=1        rvc_cpu: the sound card's clock and voices move, so games' sound code runs ("A game's speed")
    --count-from T     rvc_cpu: every core's instructions from when the console said T
    --until-times N    rvc_cpu: stop at the Nth --until after that ("Every core's share")
