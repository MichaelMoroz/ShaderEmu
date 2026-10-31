# More than one core

The machine has 256 cores, all run by the one tick pass: core 0, which runs Linux without an MMU
(`docs/system/nommu.md`), and 255 workers that programs borrow. Two things let a pass carry that many,
both about how the GPU runs pixels: the state as whole tiles of pixels, and workers that keep no more than
they need ("What the card does with pixels" and the sections after it).

    pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --rvc experiments\rvc_opt --image build\images\nommu
    / # mctest            (every worker; "mctest N LIMIT" for other numbers, "mctest N LIMIT bench" for the cost of a pass)

The D3D11 harness with its eight targets, `rvc_cpu` and the world all compile `CORES=256`; the harness's
`RVC_CORES=N` is a measurement, for what the others cost. The one-core machine is what is left where
there are no workers: D3D12, `--no-mrt`, `--profile` (the code is under `#if CORES > 1`).

## How it is made

- **A block of state a core.** Core 0's state is a block 64 texels wide at the left of the state rows. The
  workers' is whole 8 x 8 tiles in bands beside it ("The geometry"). `STATE_TEX` adds the core's place
  (`state_off`); RAM is read with `RAM_TEX`, which nothing moves. The tick pass draws core 0's rectangle and a
  block for each worker with something to run, all before any is copied back.
- **The same RAM.** Every core reads RAM as the last commit left it, plus its own writes (its
  write cache). The commit draws every core's stores, the highest core's last ("The commit as
  points"). So a core sees another's stores one pass later.
- **Sixteen bytes, one writer.** The write cache holds whole texels: a core that stores one word
  carries the texel's other three as it read them, and would undo another core's store to them
  in the same pass. What changes hands is therefore 16 bytes with one writer a pass. There are
  no atomic instructions between cores and no locks; `mc.h` (programs/mc) is the convention.
- **Workers** have no devices and take no traps; they have the float registers. A worker is a thread of a
  Linux program that the kernel does not know of: it runs in user mode at the program's own addresses, which
  on this machine are physical, so the program's functions and pointers are its own. It is parked until core 0
  writes its start word, the texel at `0x80C00000 + 16 k`: `{ "MCSU", pc, sp, root }`, the root 0 here. It starts
  there with its number in `a0`. `pause` is its sleep until the first word of its job changes, and `ebreak` parks it
  again.
- **A trap stops a worker where it is.** There is no kernel on a worker to take one, so the
  machine writes `{ count, cause, address, pc }` into the worker's mailbox and the worker
  waits. Core 0's library looks, and a trap other than a system call ends the program with a line saying
  where. There is no paging, so no worker stops at a page: `mctest`'s `pages` and start lines say 0.
- **A system call is such a stop too.** The machine puts the call's number and arguments in
  the mailbox (cause 8), core 0 makes the call in the program they share and writes the
  answer back, and the worker goes on after its `ecall` with the answer in `a0`: memory from
  the kernel, a file read, a line printed. Two passes each, when core 0 next looks. Not the
  calls that are about which thread is calling (a new thread, a new program).
- **The mailboxes** are eight pages of the GPU device's memory at `0x80C00000`, which `/dev/gpu` maps
  (`programs/mc/mc.h` is the layout): a start texel a core from 0, the kernel's words from `0x1000` (cores asked
  back, the offer count, a word a program that posts, what a first core waits for) and `0x1400` (a core's owner), the
  lanes' field's from `0x1900`, a system call's words from `0x2000`, and from `0x4000` 64 bytes a core (its job, its
  answer, its fault and the word that lets it go on). A bit a core is eight words, in the calls and in memory.
- **A program that ends** has its workers parked by the kernel (the driver's release writes
  "MCST" into the start words of the cores it had): a worker must not go on in memory that is
  being given away.
- **The kernel hands the cores out** (an ioctl of `/dev/gpu`: so many wanted, a bit a core
  given): several programs can have workers at once, and a core is free again when its program closes it or
  ends ("Shared between programs").

At exit the harness prints a line a core: its pc, instructions and whether it runs or what parked it.

## The test (`programs/mc`)

`mctest.c` is a Linux program whose own functions are the jobs: it has the library start the
workers and checks them (`linux/nommu/root.sh` builds it, then `pwsh tools\make_image.ps1`):

| Test | What it shows |
|---|---|
| primes | a count split among the workers equals the count on core 0 alone |
| fill | each worker fills 64 KiB of its own; core 0 reads every word |
| stripe | three workers fill one buffer, every third 16 bytes each: no store undoes a neighbour's |
| sum | each worker reads what the others wrote |
| pages | a worker writes and reads memory nobody has touched: nothing to bring on this machine |
| calls | a worker asks the kernel for memory, fills it, writes a line and gives the memory back |
| floats | float arithmetic kept in registers over many passes gives what it gives on core 0 |
| loop | eight rounds of `mcw_for` over 256 KB; 64 small ones back to back, the team waiting on the round word; a loop asked inside a piece |
| async | 24 `mcw_call`s, more than there are workers, joined last to first |
| effects | jobs print, defer a call, take scratch memory, and one fails: core 0 sees each at the join |
| counters | every core adds to two counters in the same passes; the totals are exact |
| park | `ebreak` parks them; the next run starts them again |

`tools/test_all.py` has `mctest 3` and `mctest 255 30000`.

## What the card does with pixels (`tools/warpbench`)

`tools/warpbench` (`build.bat`, then `bin\warpbench.exe`, `--only size|pair|count|shape`, `--live N`, `--arr N`) is a
program of its own with no emulator in it: one pixel shader with two unrelated loops, drawn as rectangles with a
kind and a seed each. What it shows, and what the machine's layout is made from:

- **A warp is a tile of pixels.** On NVIDIA it is 4 x 8 under one big triangle. Pixels of two different programs in
  one warp cost both programs' work, so cores whose pixels share a wave pay for each other's paths. Blocks of whole
  tiles drawn by one triangle never mix; a primitive edge inside the cores' area does. Padding lanes are kept out of
  the geometry, not discarded.
- **Idle pixels of a warp take their share.** A line a pixel high costs about twice its pixels; the state is kept as
  whole tiles.
- **There is a number of pixels the card runs at full speed, and past it the time is the pixels'.** The number falls
  with what a pixel keeps in registers, and the size of the local array does not matter. It differs from card to card,
  which is why `geotune` measures it ("Packing the small workers").

So a worker keeps only what it must. A worker keeps the CPU's 44 texels, its write cache and the float registers'
8: core 0 on this machine keeps the same (`CORE0_ROWS`, 500 texels; no TLBs, no CSR rows in the tick:
`docs/system/nommu.md`, "The state").

## The geometry

Which pixel of the tick is which texel of which core is only a function, so how many workers
there are and how large each is need not be compiled in. The workers' state is bands of the
state rows beside core 0's block, eight texels high and 248 tiles of 8 x 8 long, one under another
(`MC_STRIP_BANDS`, six), a worker's tiles side by side in one band (`mc_tile_of()`, `mc_core_at()`,
`mc_texel_of()` in `src/types.h` and `main.shader`). Nearly all of a worker is its cache, so the cache's size is the
worker's size:

| Tables of 2^bits buckets | New stores a pass | Texels | Tiles | Fit in a band |
|---|---|---|---|---|
| 6 (core 0's) | 5.25 KB | 500 | 8 | 31 |
| 5 (a big worker's) | 2.6 KB | 276 | 5 | 49 |
| 4 | 1.3 KB | 164 | 3 | 82 |
| 3 (a small worker's) | 0.66 KB | 108 | 2 | 124 |

The tables are 2^bits buckets, half and a quarter of that, three entries a bucket. It is few workers that store much
in a pass or many that store little, or a mix ("Packing the small workers"). A worker whose cache is full ends its pass
there, as any core does, and goes on in the next.

The geometry is the texel of the control words at `0x810003d0`: `.r` bit 0 while it is laid out anew; `.g` how many
workers; `.b` the size of their write caches (3 to 6); `.a` the hybrid's word ("Packing the small workers"). Where a
worker's state and its pixels are is arithmetic from its number, with no loop over the workers before it (`mc_tile_of`,
`mc_block_of`, `mc_core_at`, `mc_core_in`). **It is set while every worker is parked, and at no other time**: a worker's
registers and cache are those pixels. While the bit is set every pixel of the band is zero and the commit takes no
worker's stores (a worker with no state is a parked one). The control pass says in `0x810003c0` how many cores there
are now, and in the word at `0x810003cc` how many there could be (256).

Programs do not lay workers out: the `SHAPE` calls of `/dev/gpu` do nothing. The kernel's `workers_layout()` writes every
core but the first at size 5, in a delayed work of the driver's at boot, or in the first `WORKERS` call if nothing has
laid them out by then.

**The tick's geometry shader draws the strips** (`tick_geom` in `main.shader`). The pass is handed a primitive for every
sixteen cores; the shader reads the geometry and puts out a quad for core 0 and one for each worker that has something to
run. A worker that is parked, asleep or stopped gets none (`mc_idle()`, asked there), so it costs no pixel. While the band
is laid out anew the shader draws all of it, as zeros. The host knows none of this: before the pass it copies core 0's
rectangle and the band into the buffer the pass draws into, so that what is not drawn stays what it was, and copies
both back afterwards (`CustomRenderTexture::copyIn`, `rvc_backend11.cpp`).

## Eight texels a pixel

A pixel of the tick is eight texels of state: with eight render targets the pass runs an eighth of the pixels. The D3D11
harness does that (`TICK_MRT`; `--no-mrt` for one target, as D3D12 has) and so does the world ("In Unity"): the tick
draws into eight targets 1,040 wide (`TICK_MRT_W`: core 0's eight columns, eight empty, 128 blocks of eight) and 16 rows
for every 128 workers, 32 in all. Core 0's pixel (x, y) keeps texels 8x to 8x + 7 of row y. A worker's tile of 8 x 8 is a
column of eight pixels, in a block of columns that is the worker's own. The same pass compiled with `TICK_UNPACK` then
draws the state's quads into the state texture and copies each texel from its target, so the commit, the control pass
and every reader find the state where it was.

Where the pixels are matters more than how many there are:

- A block is the worker's columns and two more, rounded up to four (`TICK_BLOCK`, `TICK_PAD`), or the widths the
  geometry's word gives. Of a block of eight, up to six columns may run before a worker shares with its neighbour.
- Core 0's pixels want eight empty columns between them and the first worker's (`TICK_GAP`).
- Blocks in bands 16 rows apart do not share. The card does not run aligned 8 x 4 groups the other way up: a tile is a
  column, not a row.
- `mctest`'s bench, the same primes on every worker, hides sharing: cores that run the same instructions pay nothing for
  it. Judge a layout by a program in which no two cores do the same (`nxray`). `TICK_LOAD=N` draws every worker's quad
  N times, read by nothing: what the tick would cost with N times the busy workers.

What the eight targets buy is that pixels stop counting: a pass is as long as its longest pixel, and an eighth of them
leaves room for the workers' blocks under the number the card runs at once.

**A share, not a tile.** A worker has all of its share of a picture in one job (`trace_pictures`, `pt_share`): every n-th tile
of an order that scatters them over the picture (tile j is number j x stride, the stride with no factor in common with
the number of tiles). The path tracer's share is every sample of its tiles, one after another; it writes how far it is
into 16 bytes of its own, ends early when the kernel wants its core (`mcw_recalled`) and is posted again for the rest.
Core 0 only shows the picture as it fills. Handing out a tile at a time would have core 0 walk every worker to give the
next, and a worker wait some passes between tiles.

## Packing the small workers (`geotune`)

The geometry is hybrid: workers 1 to 16 big (caches of 2^5 buckets), which keeps a game's few workers at full speed,
and the rest small (2^3) or, on a card that can run them all, big too. How the small ones' blocks lie in the tick's targets
depends on the card, and `geotune again` measures it. `--define MC_UNIFORM` compiles the uniform geometry.

**The field.** The geometry's texel (`0x810003d0`) says the rest in `.a`:

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
first band. `mc_geo_read()` in `src/types.h` reads it for the tick and the commit, and `mc_geo_count()` in `src/gpu.h` the
same for the control pass, whose count of cores (`0x810003c0`) is what the kernel shares out: a worker the targets have no
block for is no worker.

**Who sets it.** `geotune` (`linux/userland/geotune.c`), which the init runs before any program asks for workers (the layout
may only change while every worker is parked). It does nothing if a layout is set already, the kernel's at boot too, and the kernel's work comes first: at a boot `geotune` says nothing and the workers are alike. `geotune again` lays its own out. On the card (31 October 2026, boots in turn): `media_test` on `play-test.mp3` with 63 workers 4.05 s on the kernel's, 7.23 s on `geotune`'s, `play-test.flac` level at 1.6 s, and `geotune again` itself about 6.5 s. It
first lays out a geometry that only a tick with `MC_HYBRID` gives 47 cores (small blocks 64 columns wide); one that gives
every core is a tick without it, and `geotune` leaves the layout to the kernel. Then:

1. **Capacity, not a race.** Every worker small and in eight columns of its own, the same job
   (a loop whose branches go by the worker's number, so no two run the same instructions) on
   an eighth of them and on all of them, in turn, three times each; the medians are compared.
   A card that runs every block side by side takes as long for all as for the eighth; one that
   does not takes longer, and all over the time is roughly how many it runs at once. The job is
   sized to some 200 ms for the eighth.
2. Under 1.5 times: eight-column blocks, and the small workers get the big ones' caches (2^5),
   so every worker is alike: a card that runs them all at once gains nothing from small caches. Otherwise two columns at
   2^3. If either set of three times is more than 30% apart (the world's frames come unevenly), three rounds more are run
   and the middle of six decides.
3. The registry keeps the choice (`geotune.width`, `.workers`, `.iterations`, `.us`, `.ratio`;
   `geotune.force` 2, 4 or 8 is a width with nothing measured: `docs/system/persist.md`). So that a
   kept registry is there to be read, `geotune` loads the kept folder itself when the desktop
   is to start (`nxpersist load`, and `/tmp/nxpersist.early` tells the `nx` script it is done).
   On a later boot only the eighth's job is timed again (three times, the median): within 25%
   of the kept time it is the same card and the kept width is laid out; otherwise it measures
   again. The host does not say which GPU it has, so another card is told by its speed.

`geotune again` measures though a choice is kept (before the desktop, when no program has workers).

**What it says and who may choose.** At boot it prints each step on the console: what it
measures, each round's times, the ratio and its pick, and the layout it made. The
host may ask for a layout in bits 2-3 of the host flags (`0x8100003c`: 0 the pick, 1, 2, 3 the
small workers in 2, 4 or 8 columns; the harness's `--workers auto|2|4|8`); `geotune` still
measures, lays out what was asked and says both. Its pick and ratio are then in the geometry
texel's first word, which the tick reads only bit 0 of: bits 8-15 the width it picked, 16-31
the ratio in hundredths. The world's "Worker cores" strip under the control panel has the four
choices (Auto, Dense, Medium, Wide), takes effect at the next Reset, and shows what Auto picked
and what runs.

A small cache costs a program whose workers store much, and two columns are the dense width for a laptop's card; a
game's work is on the big workers and the first core, and does not care. `rvc_cpu`'s write caches under `RVC_WCACHE` go by
the geometry it was given; `RVC_WC_BITS` sets core 0's.

## The commit as points

With the tick in eight targets what the cores stored is drawn as points, a list of them in one draw, and a geometry
shader makes each point the rectangle it stands for:

- a bucket of a write cache: the texels its tags name;
- the store a full cache had no room for: its texel;
- a copy or a fill, which is megabytes at a time: the rows of its destination, three
  rectangles at most (a first row from where it begins, whole rows, a last row to where it ends).

There are 258 points for core 0 and 130 a worker, 33,408 for 256 cores, most of which come to nothing; the zap's point
before them and the picture's after them make up the list (7,936 more for the lanes' field). Where the stores are is read from
the tick's targets. The pixel shader is the commit's own, so a texel is what it always was, whichever cores stored to it
and in what order. A point carries its core (`commit_core`) and asks only that one: two cores may not store to one texel
in a pass anyway. A point whose core stored nothing there draws nothing (`discard`), and a worker that did not run draws no
points, or its last pass's tags would put a texel as it was over what another core had just stored. RAM is one texture
written in place, what the commit needs of it as it was gathered first into a small one (`docs/machine/gpu.md`, "RAM in one
texture"). The state rows and the bands that something else wrote are drawn whole, in a draw before. Of a worker's state the
commit changes three texels (8, 28 and 41): the rest is returned as it is, without the state being decoded.

In the world the same points come from a blit (`docs/machine/gpu.md`, "Blits in place of cameras").

**One pixel that loops over every core takes its time by itself, however few such pixels there are** (a pixel is slow): the
commit and the control pass have none. A texel for every 32 cores says which run (below), and the number of cores is read
from the geometry.

## Workers with nothing to do

A worker's `pause` is a sleep until the first word of its job changes, and the machine does not
run a worker that is asleep or parked: it gets no quad (`mc_idle()` in `src/mc.h`), and the commit pass passes over a core
that stored nothing. So parked cores cost a program nothing: what a machine with every worker parked runs at is one core's.

## Using the workers from a program

`programs/mc` is everything a Linux program needs, and is the same for every program:

| File | What it is |
|---|---|
| `mc.h` | the mailboxes: what the machine, the library and a worker agree on |
| `mcw.h`, `mcw.c` | the library a program links: the workers' start and stacks, jobs, the wait |
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
  at once must not expect to see the other's stores at all. A flag of a program's own that says data is ready is stored
  after `mc_publish()`: a pass ends at any instruction, and the compiler may put the flag first.
- **A job posted to a worker still at another runs after it**: the worker looks at its job
  word again before it sleeps (`mcw_loop`).
- **Sixteen bytes, one writer a pass**, and this holds for all of the program's memory:
  two variables side by side are one texel. Give a worker whole texels to write: rows of a
  picture whose width is a multiple of 16, buffers from `mcw_alloc()` or `posix_memalign(16)`
  rounded up to 16, its own stack. Globals that a job writes want `aligned(16)` and a size
  of whole 16s.
- **A job that waits for another core's stores ends its pass** (`mc_end_pass()` in `mc.h`:
  `fence.i`, which ends a pass and does no more) and looks again in the next. Its pause is a
  sleep until its job changes, so it must not turn in place: that is 8,192 instructions a pass for
  nothing, and makes a pass as long as one with a worker at full work.
- **A worker keeps 2.6 KB of new stores a pass** (a small one of the hybrid geometry 0.66 KB, core 0
  5.25 KB: "The geometry"), and runs on until its job is done however many passes that takes. For work that is all stores, the
  cores' number is the gain.
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
- **Not small jobs.** Giving a job out and taking the answer costs a pass or two: below some tens of thousands of
  instructions the core does it sooner itself.
- **Not a job a round for many workers.** Giving 255 jobs out takes the first core several passes, and the last worker
  begins that much later: a worker's job is all the rounds, and it goes from one to the next by a word the first core
  writes (`trace_pictures` in `nxray.c`, `share_job` in `nxphys.c`).
- **A worker's time is its instructions.** Fewer loads for more instructions (float constants from integer registers)
  gains on the first core and loses on 255 workers (`docs/apps/raytrace.md`).

What it costs: nothing to speak of while the workers sleep. The card's model (`tools/card_model.json`, calibrated) gives a pass
as 1.40 ms + 0.0111 ms a thousand of core 0's instructions + 0.00079 ms a thousand of the workers' + 0.57 ms a GPU list: a full
pass of core 0 is 1.5 ms, and a full pass of all 255 workers 1.65 ms more. That last term rests on `nxphys` alone.

## Lanes

A worker's pixels can each run a function on an index of their own, and a field of 3,968 lanes of no
core's takes a job from any program's first core (`docs/machine/lanes.md`): `mcw_lanes`, `mcw_field_begin`,
`mcw_shade`. The lean tick has them; `rvc_cpu` and `rvc_opt` have none, and the library makes plain
calls there.

## The second layer (`mcwx.h`)

What every program that opens workers wants round `mcw_post()`: the shares of a loop, rounds driven by a word, buffers made
present first, a log a worker fills for core 0 to print, calls kept for core 0, errors raised after the job.
`programs/mc/mcwx.h` and `mcwx.c` are those, over `mcw.h`, with no C library (`linux/tcc/build.sh` puts them in the machine's
own `libmcw.a`):

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
  Which worker: the lowest free one whose core has the largest write cache (`call_pick`; `mcw_fast(k)`: cores 1 to 16),
  then the lowest free one: a job that stores much ends its passes on a full cache on a small core. `mcw_call_worker(h)`
  and `mcw_core(k)` say where a call went, for a program's statistics.
- **What a job may do that only core 0 may**: `mcw_print()` (a log of 2 KB a worker, written
  at the join; a full one is written then and there by a system call), `mcw_defer(fn, a0, a1)`
  (made by core 0 at the join, in order), `mcw_scratch(bytes)` (memory of the core's own, in
  chunks of 64 KB it maps; `mcw_scratch_reset()`), and `mcw_fail(what, value)`: a jump back
  to the job's start, and core 0 calls `mcw_on_failure()`'s function at the join (by default
  it says where and the program ends). On core 0 all of these are done at once.
- **Counters** (below).

`mctest` has a test for each, and `mctest N 0 race` stores to one 16 bytes from two workers for the race checker.

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
so). `rvc_cpu` merges after every pass in which a core added. The merge is drawn only after "added", so it is nothing
while no program counts.

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
is seen in the next), and reads its mailbox's words so too. `RVC_BESIDE=1` runs core 0 beside the workers as well, the
mailboxes kept so. At the end it prints the counts and the eight commonest places; the file
has them all, and `python tools\mc_candidates.py CALLS ELF --races FILE` names them.

`mctest 3 0 race`: two workers store to the two halves of one 16 bytes, pass after pass. Plain
`rvc_cpu` keeps both (`49 0 49 0`); the shader machine and `rvc_cpu` with `RVC_RACES` lose the
first worker's (`0 0 49 0`), and it reports 49 undone at `race_job`. `mctest` with 3 and
255 workers has nothing undone and passes; its reads as they were are all the team's look at
the round word, one a worker a round (192 and 16,320). The parallel copy and fill (`memop`) are not checked.

**Passes apart** (`RVC_RACES_NEAR=N`): `rvc_cpu` ends every core's pass at its
budget, but on the card a pass ends where it ends (the write cache full, a sync, a rest), so two stores of different
cores that fall a pass or two apart here may fall in one pass there, and the whole-texel commit lose one. With it every
change to a texel that another core changed other words of within N passes is counted and named (`near` lines in the
file: the core, pc and words of each). Half-Life's `world_ahead` beside `iTouchLinkSemaphore`, the loopback's `get` and
`send` and `net_drop` were found so, never by `rvc_cpu`'s own schedule.
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
functions. `python tools/prof.py` is the wider view (`docs/machine/profiling.md`).

## A core that rests

The first core of a program whose workers have the work has nothing to do until the next pass. With the pause hint in
user mode the pass would end, but the kernel would take the program for busy: nothing else would get the core, the idle
loop's work would not be done, and `tick_util` would count the pass as the program's.

- **`mcw_rest()`** (`mcw.h`) is what that core calls: the `REST` call of `/dev/gpu`
  (`shaderemu_gpu_rest` in `linux/kernel/shaderemu_gpu.c`, `_IO('G', 11)`). The kernel lets
  whatever else can run have the core (`yield()`, when another task is runnable); with
  nothing to run it does the idle loop's part (a block of the zero pool cleared ahead,
  `docs/machine/boot.md`) and ends the pass there with `pause`. It comes back in a later pass, never the same one.
  `mcw_rest_for(workers, n)` says which workers it waits for, and is `mcw_rest()` as things are.
- **The library rests where it waited**: `mcw_wait()`, the waits of `mcw_open()` and
  `mcw_close()`. `nxphys`, `nxvideo`, `nxray`, `nxpath`, `nxplay` and the NES rest in their own loops,
  and the viewer's first core while the codes of its strips are not there yet.
- **With no workers open, or a kernel without the call, it is the pause hint** (the
  call answers ENOTTY once and is not made again). It is the first core's only: a job on a
  worker has the pause hint, which is its sleep.
- **What it costs** is a system call's instructions, which the core had nothing else to do with; what another core stored
  is seen at the pass's end either way.
- **`tools\tick_util.py` counts a pass that ended in `shaderemu_gpu_rest` apart**, as it
  does one that ended in the idle loop, and says how many on a `REST` line: utilization is of
  the passes in which the core had work. Only that call counts: a program's own pause is
  still a busy pass that ended early. A pass in which the rest cleared a block ahead ends
  in `zeroed_idle` and is an idle one.

## A use: Red Alert's movies

`docs/games/ralert.md` ("Movies, and the worker cores"; `linux/ralert/workers.c` over the
library above): the opening movie's frames are decoded by core 0 and three workers into a picture the GPU shows. It is the
kind of work the workers are for: every pixel stored once, and a core keeps little of stores a pass. The window system's pool
of window buffers ends below the mailbox pages (`POOL_SIZE` in `linux/nanox/scr_shaderemu.c`).

## A use: Quake's server

`docs/games/quake.md` ("The server on a worker core"; `linux/quake/server_shaderemu.c`): a part of
a program's own logic on a worker, which only a worker in the program's memory can have. The
server's physics and QuakeC run on a worker as the function they are, and the client draws at
its own pace from what the server last sent. What it took is the list of what two cores have in common in a program
written for one:

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
program links (`programs/mc/mcw.c`) does its part, and no program has to do anything.

- **A share, not a claim.** A program says how many workers it wants (`mcw_open(n)`; the
  `WORKERS` calls on `/dev/gpu`, as often as it likes). The cores are divided evenly among the
  programs that ask, nobody more than it wants, the small wants filled first: of 255 cores,
  one program that wants 255 has them all; a second that wants 40 has 40 and leaves the first
  215; a game that wants one has one whoever else is there. A program that only borrows
  (`mcw_lend()` before `mcw_open()`: the monitor, which draws its plots with them) has what
  the others leave, down to none.
- **A core is taken back between jobs, never during one.** A core over its program's share
  that has no job (its mailbox's job and answer numbers are the same) is parked and given on
  at once: the kernel sets its word at `MC_OWNER_AT` to 0, which the library sees before it
  writes the next job there. One at a job has its bit set at `MC_RECALL_AT`, and the library
  gives it up (`MC_GIVE`) when the job has ended. A program sets its word at
  `MC_POSTING_AT` while it writes to a mailbox, and the kernel takes none of its cores then:
  so a job is never written to a core that has become another program's. The kernel takes back no core whose answer the
  program has not taken (`MC_TAKEN_AT`).
- **A program's workers keep their numbers.** `mcw_open()` answers with the share. A worker
  whose core is gone, or not there yet, has none: a job posted to it is run by the core that
  posts it, there and then, so everything still happens, slower. When cores come free the
  count at `MC_OFFER_AT` goes up, and the library asks for its share again the next time such
  a worker is posted to or asked about; the core is started, and is the worker's some passes
  later. `mcw_bound(k)` says whether worker k has a core, for a program that would rather not
  have its own core do the job (the monitor shades nothing itself: `mcw_asked()`).
- **A core can come back to the program it was taken from.** A core parked while idle and
  given back in the same program's next claim has the same number but a cleared mailbox:
  `claim()` drops a worker's core that its answer gives back unless the core's mailbox still says
  it is alive for it, and `look()` checks the core's owner word before its answer numbers. A
  team round's worker with no core (`post_team` in `mcwx.c`) has its share run by the first
  core, never in the job a post of the round runs inline.
- **One layout, the kernel's or `geotune`'s** ("The geometry"). Two layouts cannot be at once, and one
  program's would be every other's too, so no program has one: all 255 are anybody's.
- `serve()` looks only at a core that is the program's still, and a worker still on its way when its first job was posted
  is served by `look()`.
- A program that holds workers and never calls the library (a tracer between two pictures)
  loses the idle ones all the same: that is what the kernel's own look at the mailbox is for.

`mctest N SECONDS share [lend]` keeps N workers at short jobs and says every second how many
have cores: `mctest 255 12 share & sleep 3; mctest 40 4 share` says 255, then 215 while the other has
40, then 255 again; `mctest 20 9 share lend & sleep 3; mctest 255 3 share` says 20, then 0 while the other has 255, then 20;
never a wrong answer (`rvc_cpu`, 31 October 2026). After a change to the kernel's part or the library's: that,
`mctest 3` and `mctest 255 30000` (PASS), and two programs at once whose sums are known.

What it does not do: move a job that has begun (the machine keeps a worker's registers
where no program sees them), or give a program more workers than it had when it opened them
(a program started in a crowd stays the size it began at).

## Where workers pay, and where they do not

What decides it is the pass: a core sees another's stores one pass later, and a pass is 8,192 instructions.

- **A job given out and waited for costs about a pass at each end.** The worker begins in
  the pass after the one the job was posted in, and the core that waits sees the answer in
  the pass after the one it was finished in. Splitting a piece of a frame between cores
  ("this half of the map's cells on a worker") gains the piece less up to two passes: for a
  game whose whole frame is ten passes that is nothing.
- **What pays is work that needs no answer in the same frame.** Two parts of a program each
  at its own pace (Quake's server and client), work begun ahead of its use (the next movie
  frame unpacked), or one long job shared out (a movie frame's rows; loading).
- **And the two sides must have their own data.** A game whose drawing reads and marks the
  objects its logic moves cannot have the two on different cores, unless the drawing works from copies a frame behind.

| Program | What a worker has |
|---|---|
| Quake | the server (`docs/games/quake.md`), the frame's drawing by a worker that stays and draws a frame late, the 2D screen recorded by another, and the frame's jobs (particles, light under models, light maps, water) |
| Quake III | the server, the client's game and level items on helpers (`docs/games/quake3.md`) |
| Half-Life | the server, and the 3D drawn a frame behind (`docs/games/halflife.md`) |
| Red Alert | its movies, and in a mission the scene: the frame's rectangles are made GPU commands by a worker while the game goes on to its next frame's logic (`docs/games/tdawn.md`, "The scene on a worker core"), and the objects drawn a frame behind from copies ("The objects a frame behind"). The logic stays one core's |
| Tiberian Dawn | the scene, as Red Alert (the same file), and a unit's second path search (`docs/games/tdawn.md`, "A unit's path searches") |
| Doom | the view, made by up to four workers from what the first core tells them between tics (`docs/games/doom.md`, "The view on worker cores"), and the sight checks of the tic after next, walked by a fifth. The tics stay the first core's |
| ClassiCube | the meshes of chunks that have none yet, the world's height map and strata made by every core (`docs/games/classicube.md`), and the rest of a frame's list, a worker's beside the next frame's ticks |
| OpenTTD | its viewports' parts and its game tick (`docs/games/openttd.md`) |
| The window system | none for its drawing: see below. One lent core for the pointer (`docs/system/nanox.md`, "The pointer on a core of its own") |

**The window system.** Composition is the GPU's already, and a drawing request is mostly not computing: much of the time the
processor waits for the GPU to have drawn a list, and much of the rest is the kernel's (sockets, the scheduler, timers). A
worker makes no system calls of its own, so the kernel's part is not its to take, and the wait is a pass of the machine whoever
waits.

## What the cores did, for the guest and the host

The control pass publishes each core's count of instructions among the machine's control
words (`MC_STATS` in `src/gpu.h`), where a program reads them through `/dev/gpu` like any
other:

| Address | Contents |
|---|---|
| `0x81000380 + 4 k` | core k's instructions so far (k from 0 to 15) |
| `0x810003c0` | how many cores the machine has now (0 on a machine built for one) |
| `0x810003c4` | a bit a core: it runs (a worker that was started and has not parked) |
| `0x810003c8` | a bit a core: it is asleep on its job word |
| `0x810003cc` | how many cores it could have |
| `0x816ba000 + 4 k` | core k's instructions so far, every core (`MC_STATS_ALL`: the network's row, after its ring) |
| `0x816ba400 + 16 n`, `+ 4` | cores 32 n to 32 n + 31, a bit a core: runs, asleep |

`/proc/gpu` says whose a tag is: a line an open of `/dev/gpu` (`pid workers wanted lent tag draws lists`), which `nxmon`
matches to the owner words to give each program its cores.

`nxmon`, a task manager (`docs/system/nanox.md`), reads them: on its Performance tab the workers'
instructions a second, all 255 of them, are stacked on core 0's plot (or Per core, a 16 x 16
grid of every core's load), and the numbers under it say how many work, sleep, are idle and
are parked (`NXMON_PRINT=1 nxmon` prints that line at every sample, for a test); its
Processes tab gives each program's workers from `/proc/gpu`. Its plots are the workers' own work, as a graphics card's pixel
shader would do them: it keeps eight workers while it runs, and once a sample each writes some rows of the plots from the
samples (`shade` in `nxmon.c`) straight into the window's own buffer in GPU memory, mapped through `/dev/gpu`. The display shows
that buffer as it is, and no drawing request is made for the plots. `NXMON_CHECK=1` shades every frame again on core 0 and
must say 0 pixels differ; `NXMON_WORKERS=0` leaves the workers alone (core 0 then shades the newest columns once a second).
The monitor's workers are only lent to it (`mcw_lend()`: "Shared between programs"): when a program that keeps its workers wants
them, the monitor's are taken back; it sees that (`mcw_asked()`), closes them and shades on core 0, and takes workers again when
some have been nobody's for a while. So a path tracer started beside the monitor has all 255. The harness adds them up from the
rows it reads back anyway: its `STATS` and closing lines say what the workers ran and the rate of all cores together.

## Every core's share of a game's frame

`python tools\game_par.py GAME` (or `all`) runs a fixed workload of a game from power-on on
`rvc_cpu` (no card, a few seconds each) and says its instructions a frame on core 0 and on
all cores together; the factor is the second over the first. `rvc_cpu --count-from TEXT`
takes every core's count when the console says TEXT and prints what each ran from there
(`COUNT from ...`); `--until-times N` stops at the Nth `--until` after it. `--show` prints
the command line, `--runs 3` takes the mean over three timings (`--salt`: the command typed
after `: N;`), `--profile PC.LOG` samples core 0 every 256 instructions for
`tools\pc_profile.py`, and `--calls FILE` is `RVC_CALLS`. Its workloads are in `GAMES` at the top of the tool. By default it runs
with the card's machine (`RVC_SOUND=1 RVC_WCACHE=1 RVC_ADFAULT=1 RVC_RACES`; `--plain` drops them): without the sound card a game's
mixing code never runs, and without the write caches and memory a pass old the passes are not the card's.

Plain clock, one run each, `build/images/nommu`, 31 October 2026 (`Workers` ran in the window; the busiest worker's instructions
are its own and say where the critical path is):

| Game | Frames | Core 0 a frame | All cores a frame | Factor | Workers | Busiest worker a frame | Passes a frame |
|---|---|---|---|---|---|---|---|
| Doom, `-timedemo demo1` | 4,612 | 26.5k | 64.6k | 2.43 | 5 | 15.4k | 4.27 |
| Quake, e1m5 played | 3,760 | 22.5k | 119.7k | 5.31 | 5 | 35.5k | 4.78 |
| Quake, `+timedemo demo1` | 969 | 37.4k | 107.1k | 2.87 | 8 | 31.0k | 5.80 |
| Quake III, q3dm17, one bot, frames 300 to 700 | 400 | 23.3k | 159.4k | 6.83 | 4 | 41.8k | 5.93 |
| ClassiCube, walking, five 2 s windows | 1,094 | 14.6k | 90.3k | 6.19 | 6 | 19.5k | 3.81 |
| Tiberian Dawn, mission 3, frames 301 to 1,200 | 900 | 81.4k | 128.5k | 1.58 | 23 | 18.2k | 10.83 |
| Red Alert, Soviet, the same | 900 | 55.0k | 77.0k | 1.40 | 4 | 14.7k | 7.49 |
| Red Alert, Allied, the same | 900 | 80.6k | 129.3k | 1.60 | 5 | 34.0k | 10.65 |
| NES, Thwaite, frames 100 to 1,000 | 900 | 3.0k | 30.6k | 10.29 | 3 | 21.9k | 2.77 |
| nxphys, the wall (a frame is a substep) | 120 | 10.6k | 1,473.9k | 139 | 255 | 14.2k | 3.60 |

In every run the workers stopped 0 times and core 0 took no fault on a clear bit. A run's instructions shift with its timing:
compare means (`--runs 3`). A worker that waits for its next job counts the instructions it waits in, so the factor depends on
the pass. Quake III left to where it starts is two games: a word more in the command line moves the player and the bot, which is
why `game_par.py` puts the player at one place. Where core 0's instructions go is `--profile` or `tools/prof.py`.

## A game's speed: passes a frame

A pass is the machine's step: every core runs in it at once, and what one core stores another
sees in the next. So a game's frames a second are the passes a second over its passes a frame. In the world the passes a second are
its rounds a frame times Unity's frames a second, whatever the guest does. What a change can move is the passes a frame: the
longest chain of work in a frame on any one core, and the passes spent waiting between cores (a job posted is begun in the next
pass, its answer seen in the pass after it ends). Core 0's instructions a frame are not the measure.

`python tools\game_par.py GAME --calibrated` gives each pass a card's time (`--pass-model`, `tools/card_model.json`:
the skill `calibrated-measurements`, good to about 7%). Calibrated, 31 October 2026, one run each:

| Game | Passes a frame | Frames a second |
|---|---|---|
| Doom, `-timedemo demo1` | 4.26 | 145.8 |
| Quake, e1m5 played, the client | 7.26 | 93.9 (the server 2,329 frames to the client's 4,698) |
| Quake, `+timedemo demo1` | 5.79 | 109.3 |
| Quake III, frames 300 to 700 | 8.36 | 79.1 (the server 26.1 frames of the game a second, 25.3 passes each) |
| ClassiCube, walking | 3.52 | 175.7 |
| Tiberian Dawn, mission 3 | 10.68 | 60.8 |
| Red Alert, Soviet | 7.39 | 86.7 |
| Red Alert, Allied | 10.53 | 61.6 |
| NES, Thwaite | 2.76 | 226.9 |
| nxphys, the wall | 3.59 | 187.2 substeps (leans on the many workers' term: not to be trusted as an absolute) |

`python tools\game_par.py GAME --image A --image B --card` runs the workload on the card in real time through `gpu_queue.ps1`
and the harness lock, for each image in turn from a folder of junctions (`build/game_par/card_NAME`; the guest's `md5sum` of
the game says which image ran), and prints the game's own frames a second, passes a second and a frame, and core 0's, the
workers' and all cores' millions a second. It will not run while VRChat or SteamVR does. A change is judged on `rvc_cpu` by its passes
a frame and calibrated frames, the card being the check of a round's end.

The total instructions a second rise only as far as workers run beside a core 0 that stays busy. `game_par.py` also prints how often
the workers stopped from the mark, on a written bit and core 0's faults on one: on this machine, which has no page walk, both are 0.

## In Unity

The VRChat machine is the one without an MMU (`NO_PAGING`, `docs/system/nommu.md`) with the harness's
256 cores, the guest's geometry, the tick in eight targets, the commit as points, the shader's
own write cache (336 texels: 64 + 32 + 16 buckets of three) and passes of 8,192 (the control panel's `tickSlider` sets 1,024 to 32,768)
(`CORES`, `L1_TABLE_BITS` and the sizes in `MachineBlit.cginc`). Its tick is the lean one (`MachineTickLean.shader`,
`lean/tick.hlsl`, a verbatim copy of `experiments/rvc_lean/tick.hlsl`), `MachineTick.shader` being the one it replaced.

- **The tick is a camera's pass.** The tick's shader is on a mesh of seventeen triangles (one for every sixteen cores and one
  for the lanes' field) that only the tick's camera sees (a layer of its own); `EmuMachine` gives the
  camera eight targets of 1040 x 32 once (`Camera.SetTargetBuffers`, which Udon has) and renders it once a round.
  `tick_geom` draws core 0's pixels and a block of columns for each worker with something to
  run, as the harness's does, and draws nothing for any other camera (it looks at the target's size).
- **The unpack** (`Machine.shader`'s third pass, a Blit into the 2048 x 64 texture the commit
  reads) puts the targets' texels where the state has its rows, and takes the state as it
  was for whatever the tick did not draw: a worker with nothing to run, the band at start.
- **The commit** writes RAM in place (`docs/machine/gpu.md`, "RAM in one texture"), in three
  steps: the gather's 257 points into the stage (2048 x 21); a Blit of the state rows from the tick's texture and the stage; and
  the commit's points (1 + 258 + 255 x 130 + 7,936 + 1) into the state (StateA, 2048 x 9280), each the rectangle it stands for and each
  texel the commit's own (`MachineCommit.cginc`, which all share). The points are blits (`MachineCommitBlit.shader`:
  `docs/machine/gpu.md`, "Blits in place of cameras"); the old meshes and points camera stay behind a switch below. A copy out of the ROM
  is a point too, and RAM at start is point 0, from the image's textures.
- **The control pass** draws its 11 rows of RAM into a window of their own (2048 x 11:
  `MachineWindow.cginc`), which the `WindowBack` pass copies into the state.
- **The readback** has state row 0, the first 64 control texels and 130 texels of the
  network's row, its window, what the cores ran and which sleep, then the console's words and
  ring (`Readback.shader`, 1032 words; `docs/machine/console.md`, "Words in memory"), where the counts are: the panel's statistics
  have a line for the workers and for all cores together.
- **The host's clock moves every round** (`HostClock` in `EmuMachine.cs`): a frame's length
  spread over its rounds, as the harness's moves every pass. A program that waits for it would otherwise run one frame of
  its own a Unity frame and end the other rounds at once.
- **The workers' counts are read ten times a second**, from a frame's last row: read every round they are milliseconds of Udon a frame.
- **The cameras are preview cameras** (`Camera.cameraType`, which Udon may set: `SetPaths` in `EmuMachine.cs`). A game camera's
  `Render` prepares every canvas of the room first.
- **The GPU's lists are drawn every round only while lists come.** When none has come for a second they are drawn in a frame's last
  round, and the control pass takes a list in no other (`_GpuDrawn` in `Machine.shader`): the first list after a pause waits a frame at
  most.
- `oldPaths` of the machine's behaviour is the switch for those paths: bit 0 game cameras, bit 1 the GPU's lists every round, bit 4 a
  test (the GPU's lists in a frame's last round only, whatever comes, so that every list is drawn late), bit 5 the commit by the points
  camera, bit 6 the GPU's lists by its camera. Set from editor code in play mode it gives before and after from one session.

**Measuring it.** The world's speed is measured in the editor with the Game view in front (play mode started by a script leaves the Scene
view there, and the room is then drawn small), by `totalInstructions` over several seconds, a change and the tree in turn. A session's level
differs from another's: only the two sides of one session compare. With the processor busy elsewhere the main thread is the limit at any
round length. The tick's bytecode is the harness's under `--rvc experiments\rvc_lean`: the editor's compiler is the same file as the harness's
fxc2 (`Data\Tools\fxc2_d3dcompiler.dll`), so a round costs what a pass costs in the harness; what is left between them is what a frame has
beyond its rounds (the room's cameras, the editor's own drawing, the frame's end), so more rounds a frame come nearer the harness's speed.
A guest's wait for something the host gives once a Unity frame shows as rounds that end at once to the frame's end.

"ShaderEmu/Add the tick's and the commit's cameras to the open scene" puts the cameras, their meshes and the targets into a scene without
building the world again.

## Switches

    RVC_RACES=FILE        rvc_cpu: memory as the GPU has it, and where that mattered ("Races")
    RVC_RACES_NEAR=N      rvc_cpu: a texel changed by two cores within N passes ("Passes apart")
    RVC_RACES_AT, _UNTIL  rvc_cpu: the check from one console text to another ("A part of a long program")
    RVC_BESIDE=1          rvc_cpu: core 0 beside the workers, as on the card ("Races")
    RVC_CALLS=FILE        rvc_cpu: core 0's calls by their instructions ("Finding the work")
    RVC_COUNT_MERGE=0     rvc_cpu: no merge of the counters, as the world's machine
    RVC_SOUND=1           rvc_cpu: the sound card's clock and voices move, so games' sound code runs ("Every core's share")
    RVC_CORES=N           rvc_harness: a machine of fewer cores (a measurement)
    --count-from T        rvc_cpu: every core's instructions from when the console said T
    --until-times N       rvc_cpu: stop at the Nth --until after that ("Every core's share")
    --pass-model A,B,C,D  rvc_cpu: a pass as long as a card's ("A game's speed")
    --workers auto|2|4|8  rvc_harness: how geotune lays out the workers ("Packing the small workers")
