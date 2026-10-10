# More than one core

The machine with worker cores beside it, all run by the one tick pass. Linux on core 0 starts
the workers and gives them work, and every result is right. Core 0 and three workers cost
1.17 times the pass of core 0 alone, so four cores do 3.4 times the work of one; past that a
pass grows with the cores (eight: 2.1 times, sixteen: 3.2 times). It took two things to get
there, both about how the GPU runs pixels: the state as full tiles of pixels, and workers that
keep no more than they need ("What a pass costs" and the sections after it).

    bin\rvc_harness.exe --rvc experiments\rvc_opt --image linux-net
    / # mctest            (3 workers; "mctest N LIMIT" for other numbers, "mctest N LIMIT bench" for the cost of a pass)

The machine has 64 cores wherever it can have workers: the D3D11 harness with its eight
targets, `rvc_cpu` and Unity compile `CORES=64`, and there is no switch for fewer (there was
`--cores N`). The one-core machine is what is left where there are no workers: D3D12,
`--no-mrt`, `--profile` (the code is under `#if CORES > 1`). The 64 cost core 0 about 12%
while no worker runs: a shell loop is 3.6M instructions a second, 4.1M on one core.

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
  the count back, and the worker runs the instruction again. Anything else that traps ends
  the program with a line saying where.
- **A system call is such a stop too.** The machine puts the call's number and arguments in
  the mailbox (cause 8), core 0 makes the call in the program they share and writes the
  answer back, and the worker goes on after its `ecall` with the answer in `a0`: memory from
  the kernel, a file read, a line printed. Two passes each, when core 0 next looks. Not the
  calls that are about which thread is calling (a new thread, a new program).
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
| calls | a worker asks the kernel for memory, fills it, writes a line and gives the memory back |
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

## The geometry

Which pixel of the tick is which texel of which core is only a function, so how many workers
there are and how large each is need not be compiled in. The workers' state is a band of the
state rows beside core 0's block, 8 texels high and 64 tiles of 8 x 8 long; each worker has
a strip of whole tiles of it, one worker's after another's (`mc_core_at()`,
`mc_state_read()`, `mc_texel_of()` in `src/types.h` and `main.shader`). A worker keeps the
CPU's 44 texels, its write cache and the float registers' 8, and nearly all of that is the
cache, so the cache's size is the worker's size:

| Tables of 2^bits buckets | New stores a pass | Texels | Tiles | Fit in the band |
|---|---|---|---|---|
| 6 (core 0's, and a worker's until now) | 6 KB | 564 | 9 | 7 |
| 5 | 3 KB | 308 | 5 | 12 |
| 4 | 1.5 KB | 180 | 3 | 21 (the machine has 15 workers at most) |
| 3 | 0.75 KB | 116 | 2 | 32 |

So it is few workers that store much in a pass or many that store little, or any mix. A
worker whose cache is full ends its pass there, as any core does, and goes on in the next.

The geometry is a texel of the control words (`0x870003d0`): a word with bit 0 set while the
square is laid out anew, then four bits a worker for workers 1 to 8 and for 9 to 15 (its
bits; 0 ends the list). **It is set while every worker is parked, and at no other time**: a
worker's registers and cache are those pixels. The kernel does it (`SHADEREMU_GPU_SHAPE`,
`workers_shape()` in `linux/kernel/shaderemu_gpu.c`; `mcw_shape()` in the library): it
refuses with EBUSY while any program has a worker, parks every core, sets the bit, during
which every pixel of the band is zero and the commit takes no worker's stores (a worker
with no state is a parked one), writes the two words and clears the bit. The control pass
says in `0x870003c0` how many cores there are now, and in the word at `0x870003cc` how many
there could be (64). Until a program asks, the kernel
lays out three workers of the full size and twelve of size 4 the first time workers are
asked for. `mctest N LIMIT shape 6,6,5,4,4` sets a geometry and runs the tests on it.

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
640 x 480 (`docs/nanox.md`, the viewer), whose strips of pixels are shared out between the
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
targets 264 wide and as high as core 0's rows. Core 0's pixel (x, y) keeps texels 8x to
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

Longer passes with a larger write cache, which are the harness's defaults now (`--ticks`
32,768; `L1_TABLE_BITS=7`, 1,485 texels of state, which with one target is over what the
card runs for nothing). A pass ends early when the cache is full, so passes of 32,768
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
90 blocks of 4 x 8. The tick is not what stands in the way of that many workers: the commit is (it looks
into every core's cache for every texel it writes: 0.06 ms a pass with one core busy,
1.1 ms with sixteen), and the geometry's texel, which has room for fifteen.

In Unity the tick is a camera's pass, which is how a pass with several targets is drawn in
VRChat ("In Unity", below).

## More than sixteen cores

The machine has 64 (the D3D11 harness with the eight targets, `rvc_cpu`, Unity). What is
different above 16:

- the workers' band is the state rows to their end, 248 tiles of 8 x 8 in place of 64;
- the geometry is eight words, four bits a worker: the geometry texel's second to fourth
  words and two texels of the mailbox page (`0x86c00f00`, `0x86c00f10`; they were the two
  control texels after it, which are the network's);
- cores 16 to 63 have their mailboxes after the first sixteen's (`programs/mc/mc.h`: three
  pages now), so a program built before knows the first fifteen workers and is not disturbed;
- the kernel has two more calls on `/dev/gpu` for them (`SHADEREMU_GPU_SHAPE_ALL`, eight
  words; `SHADEREMU_GPU_WORKERS_ALL`, the cores it gives as two words of bits). The older
  calls give out the first fifteen. The library (`mcw.c`) uses whichever the kernel has;
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

Point n is core 0's entry n (768 of them, then its last store and its copy), and from 770 on
386 a worker: 770 points for one core, 6,560 for 16, 25,088 for 64, most of which are no
entry and come to nothing. Where the stores are is read from the tick's targets. The pixel
shader is the commit's own, so a texel is what it always was, whichever cores stored to it
and in what order. The state rows and the bands that something else wrote (the GPU's
control words and its picture copied back) are still drawn whole, in the draw before.

RAM is still two buffers, which a copy needs (it reads RAM and writes RAM), so the buffer
not drawn into must be given the same texels: the same points are drawn once more into it
as plain copies of what the commit made them. (A worker that did not run has its last
pass's stores in the targets: those texels are written again as they are.)

A pass's commit on the graphics card, one core busy: 0.024 ms where it was 0.062. `nxray`'s
picture of 640 x 480 with 63 workers of the smallest size is 1.19 s where it was 1.35 s
(677 thousand rays a second, 58 times core 0 alone), and with 15 workers 4.29 s where it
was 4.50 s. `nxpath`, 16 cores: 100 thousand rays a second where it was 89.

In Unity this is a mesh of points, so a camera's pass, as the tick in eight targets is ("In Unity").

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

- **A job computes, reads and writes memory, and returns.** It may make system calls, which
  core 0 makes for it (two passes each). What it may not is use what the C library keeps one
  of for the whole program while core 0 uses it too: `malloc`'s heap, `stdio`'s buffers. One
  side at a time: a job may allocate and print while core 0 only waits for it.
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
- **And memory nobody has touched stops a worker** until the first core looks: a page at a
  time, a frame of the program each if it only looks once a frame. ClassiCube's worker did
  3 million instructions in 20 seconds that way, its copy of the builder being variables
  that begin as zeros. `mcw_touch()` on the program's data before the first job mended it.
- **And the two sides must have their own data.** A game whose drawing reads and marks the
  objects its logic moves cannot have the two on different cores.

| Program | A frame | What a worker has, or why none |
|---|---|---|
| Quake (e1m5) | 515 thousand instructions, 4.85 frames a second | the server: 13.3 frames a second (`docs/quake.md`) |
| Red Alert | 220 to 240 thousand: logic 110 to 127, drawing 88 | its movies (three workers, given back when the movie ends), and in a mission the scene: the frame's rectangles are made GPU commands by a worker while the game goes on to its next frame's logic (`docs/tdawn.md`, "The scene on a worker core"). Drawing 91 to 67 thousand, 16.4 to 17.8 frames a second. The logic and the game's own drawing stay one core's: the drawing clears the marks the logic sets, object by object |
| Tiberian Dawn | 170 to 230 thousand: logic 100 to 145, drawing 45 to 70 | the scene, as Red Alert (the same file): drawing 51 to 43 thousand. Its start was looked at for a long job and had a slow one instead: a table sorted whole for every entry read from a file, 27% of the start, mended on one core (159 to 114 million instructions to a mission's first frame, Red Alert's too) |
| Doom | a tic 33 to 39 thousand, 35 a second; a frame 45 to 55 thousand | none, and it is at the 35 frames a second the game draws at most as it is (its demo, drawn as fast as it goes: 31 to 52). The view is the one large piece (46 thousand a frame), and it walks the things the tics make and free: on a worker beside the next tic it would read a thing while its picture and frame number change, which the game ends on. The status bar's patches (6%) and the GPU's copy of the sectors (9.5%) are calls into the one state of the GPU's library, in among the view's own. Its start is 52 million instructions with the machine's own, and no piece of it over 5% |
| ClassiCube | 60 thousand, 48 frames a second | the meshes of chunks that have none yet: a second copy of the builder on a worker, beside the first core's, and the world's height map and strata made by every core, rows each (`docs/classicube.md`). From the command to the whole world drawn: 38 s on four cores where it was 52 |
| The window system | | none: see below |

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
| `0x87000380 + 4 k` | core k's instructions so far (k from 0 to 15; 0 for a core the machine lacks) |
| `0x870003c0` | how many cores the machine has (0 on a machine built for one) |
| `0x870003c4` | a bit a core: it runs (a worker that was started and has not parked) |
| `0x870003c8` | a bit a core: it is asleep on its job word |
| `0x876b8280 + 4 (k - 16)` | core k's instructions so far, k from 16 to 63 (`MC_STATS_MORE`: the network's row, after the guest's window) |
| `0x876b8340`, `0x876b8344` | the two words of bits for cores 32 to 63: runs, asleep |

`nxmon` reads them: the workers' instructions a second are stacked on the processor's plot
and each worker has a figure, or "asleep", "parked" or "idle", in the line under the busiest
programs. The harness adds them up from the rows it reads back anyway: with `--cores` its
`STATS` and closing lines say what the workers ran and the rate of all cores together.

## In Unity

The VRChat machine has the harness's 64 cores, the guest's geometry, the tick in eight
targets, the commit as points, the shader's own write cache (384 texels) and passes of 8,192
(`CORES`, `L1_TABLE_BITS` and the sizes in `MachineBlit.cginc`; the tick and the commit are
`experiments/rvc_opt/main.shader`'s, line for line where they deal with cores).

- **The tick is a camera's pass.** `MachineTick.shader` is on a mesh of four triangles (one for every
  sixteen cores) that only the tick's camera sees (a layer of its own); `EmuMachine` gives the
  camera eight targets of 1024 x 16 once (`Camera.SetTargetBuffers`, which Udon has) and renders it once a round.
  `tick_geom` draws core 0's pixels and a block of columns for each worker with something to
  run, as the harness's does, and draws nothing for any other camera (it looks at the target's
  size).
- **The unpack** (`Machine.shader`'s third pass, a Blit into the 2048 x 16 texture the commit
  reads) puts the targets' texels where the state has its rows, and takes the state as it
  was for whatever the tick did not draw: a worker with nothing to run, the band at start.
- **The commit** is three draws: a Blit of the state rows and the bands something else wrote
  (the control pass, the GPU's picture copied back); then `MachineCommitPoints.shader` on a
  mesh of 25,088 points that a second camera draws into the same texture, each point the
  rectangle it stands for and each texel the commit's own (`MachineCommit.cginc`, which both
  share); and, before the control pass, the same points once more into the other state
  texture as copies. A copy out of the ROM is a point too: the points' material needs the
  ROM's textures (without them Linux started and could not read `/emuinit`).
- **The readback** has the first 64 control texels and 64 of the network's row, where the
  counts are: the panel's statistics have a line for the workers and for all cores together.

"ShaderEmu/Add the tick's and the commit's cameras to the open scene" puts the cameras, their
meshes and the targets into a scene without building the world again.

`mctest 15 60000 shape 6,6,6,5,5,4,4,4` passes there, and `mctest 63 20000 shape 3,3,...`. `RAY_FRAMES=1 RAY_STILL=1 nxray` gives the
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

    RVC_MC_FULL=1      draw a worker's whole 64 x 16, as before (to compare)
