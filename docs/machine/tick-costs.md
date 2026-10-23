# What an instruction costs (October 2026)

Measurements for whoever next tries to make the tick faster, taken on the D3D11 harness's
machine as it is (eight targets, 256 cores, 384 texels of write cache, 8,192 instructions a
pass) on an RTX 5070 laptop card. Most of what was tried on the way gained nothing and was not
kept; what did gain is the first section.

They are of the machine with the MMU. The one the world runs now has no paging
(`docs/system/nommu.md`): no TLBs and no page walks, its CSRs fields of core 0's first texels, and a
write cache of 336 texels (64 + 32 + 16 buckets of three). On that machine, on the same card, on
19 October 2026 (`docs/machine/speed.md`): core 0 runs `factor 999999999989` at 7.05 million
instructions a second (142 ns an instruction), `seq | gzip` at 5.35; the compute machine at 12.03 and
8.75, the plain compute shader 4.48 and 3.88.

## Where the time was

Against the machine as it was before all of this, both builds on one snapshot, in turn, the
card's clock high, the same instructions a pass: a shell loop 3.51M to 3.90M instructions a
second (11.2%), Doom's demo 3.15M to 3.52M (11.8%). None of it is from fewer operations in an
instruction's own arithmetic, which is where it was looked for first (below: 12% fewer bytecode
instructions were worth under 1%). It is from three things a pass did that it need not have:

- **The TLBs were empty at the start of every pass.** They are kept in the state's texels
  from pass to pass (`mmu.h`), and the tick's last loop, since it went to eight targets, wrote
  zeros there: `if (!have && !_Init && fp_state_texel(at, texel))` asks for the float
  registers' texel whether or not the TLB's was had already (`&&` runs both its sides), and
  what that function puts in its `out` argument for a texel that is not its own is zeros. So
  each pass walked the page table for every page it touched: 52 walks a pass of a shell loop,
  112 of Doom's demo, each an instruction on the general path (some 2.5 microseconds). Fixed:
  2.4 walks a pass, and 4.4% (a shell loop), 4.6% (Doom), 5.8% with passes of 2,048. Nothing
  showed it: a walk is not an emulated instruction, so every count and checksum was the same.
  (`--profile` now says why a page was walked: `w_empty` was every one of them.)
- **The write cache was searched for the texel it had just been searched for.** A search is
  a bucket's three tags read and compared, and the next table's when the bucket is full; a
  register save, a structure, a copy go to the same texel four times running. The two texels
  found or made last are remembered with their place, and so is the texel looked for last
  and not found (`types.h`, `L1_KNOWN`): of a shell loop's loads that pass the filter of
  stored addresses a third are answered by that, and 57% of its stores. 5% (a shell loop),
  3% (Doom).
- **Half the loads that searched found nothing.** The filter of stored addresses or-ed
  together lets nine loads in ten through. A bit for each texel the cache was given, by a
  hash of its number (1,024 bits, `L1_HAS`), stops nearly all of those: 1% on Doom.

With these a shell loop's loads search the cache 0.043 times an instruction where it was
0.226, its stores 0.062 where it was 0.144.

## A read asked for early, the window turned, the common floats apart (13 October 2026)

Looked for: a fifth more instructions a second from the tick. Found: 0.7% on a shell loop,
4.6% on Doom's demo, 7.3% on Quake's first level. On an RTX 5090, D3D11 + fxc2, passes of
8,192, one snapshot for both builds, in turn, twice, the same state hash at the end:

| | Before | After | |
|---|---|---|---|
| a shell loop | 4.434M instructions a second | 4.466M | 0.7% |
| Doom's timedemo (`-nodraw`) | 4.103M | 4.292M | 4.6% |
| Quake, e1m1 where it starts | 3.433M | 3.683M | 7.3% |
| the same three, passes of 16,384 | 4.598M, 4.178M, 3.304M | 4.621M, 4.381M, 3.539M | 0.5%, 4.8%, 7.1% |

- **A data texel is asked of the texture before the write cache is searched for it**
  (`mem_get_cached_or_tex`). A texture read that is used at once costs 90 ns on this card
  (`X_P_TEX=1`: 0.74 ms a pass), and one that is not used yet costs next to nothing: the
  instruction window's read-ahead taken out is 3 to 5% slower, though it is fewer reads. So
  the read goes out first and the search runs while it is under way; a load the cache
  answers has asked for nothing it uses. Doom reads 0.17 data texels an instruction, Quake
  0.11, a shell loop 0.06: 3.3% on Doom alone, level on the others. Asked for later, after
  the two remembered texels are looked at, it is worth a third less; asked for sooner, where
  the load has its address and before it is known to be a load of RAM, less again.
- **The instruction window is kept turned** (`fw_cur`: its first word is the instruction to
  run, and it turns by a word an instruction), where the word was chosen among the texel's
  four every time; a jump within the texel is fetched anew. 1% on all three.
- **A float sum, difference or product, and the moves, signs and comparisons, each by
  itself** (`fast_exec`, `fp_light`), where `fp_exec` worked out every result there is (a
  quotient, a root, four conversions, a class) and chose one. They are 8 in 9 of the float
  instructions Quake runs (15% of all it runs; `flw` and `fsw` 6% more). 3.7% on Quake.

What the three loads run, counted on the interpreter (a private `rvc_cpu` with counters:
kinds, pairs, texels fetched and read, what a larger memo or TLB would have had):

| Of every 100 instructions | A shell loop | Doom's demo | Quake e1m1 |
|---|---|---|---|
| arithmetic; loads; stores | 36; 24; 14 | 37; 29; 8.5 | 30; 20; 12 |
| branches; jumps and `lui`, `auipc`; floats | 16; 8.6; 0 | 19; 4.1; 0 | 11; 9.1; 15 |
| in the texel of the one before; the next texel; another | 62; 25; 12 | 68; 24; 8 | 69; 24; 8 |
| instruction texels read (one ahead) | 50 | 41 | 39 |
| loads the write cache answers; the texel read last; the texture | 12; 5.9; 6.2 | 7.6; 4.5; 17 | 11; 2.8; 11 |
| first-level TLB misses (fetch, load, store) | 2.4; 2.0; 0.3 | 0.9; 5.2; 0.7 | 2.1; 5.1; 1.3 |

Why not a fifth. On this card an operation is 1.35 ns (`X_P_ALU=8`: 32 ns for 24) and an
array element 5 to 7, so an instruction's 225 to 290 ns are some 130 to 140 operations of
its own path, which the last rewrite left little in, and what the table's reads cost. All
the data texture reads for nothing would be 2.5% (a shell loop) to 6% (Doom); every
instruction texel of a jump for nothing 3 to 4%; every first-level TLB miss for nothing 2 to
3%. What was had of those is above; the rest was tried and is below.

## The kinds as a tree, a load with one way out (13 October 2026, the second round)

Looked for: a structure with fewer operations an emulated instruction (a predecoded form,
two instructions a turn, work begun before it is known to be wanted, a build for each use).
None of those is one: the figures are below. Found instead that the shape of the fast step's
tests is worth more than its operations. The same card and method, against the tree as the
section above left it, in turn, twice, the same state hash and instruction count:

| | Before | After | |
|---|---|---|---|
| a shell loop | 4.504M instructions a second | 4.687M | 4.1% |
| Doom's timedemo (`-nodraw`) | 4.332M | 4.507M | 4.1% |
| Quake, e1m1 where it starts | 3.715M | 4.042M | 8.8% |
| the same three, passes of 16,384 | 4.661M, 4.416M, 3.576M | 4.866M, 4.617M, 3.906M | 4.4%, 4.5%, 9.2% |

(With the section above that is 4.8%, 8.9% and 16.7% over the tree of that morning.)

- **The kinds are told apart by a tree of tests** (`fast_exec`, `K_ALU`, `K_FP`): arithmetic,
  loads, stores and the floats are a group, and in it arithmetic and loads are one. They were
  tests one after another (arithmetic, load, store, branch, jump, atomic, float), and every
  step down that row cost the kinds after it 16 ns: floats moved up one place are 1.4% of
  Quake, two places 2.3%, and each place costs the integer kinds they pass as much. In the
  tree a store passes two tests' else and not two blocks, a float three. 1.8%, 1.4% and 6.0%.
- **A load from RAM has one way out** (`mem_get_cached_or_tex`). It returned from three
  places (the cache's word, the word the cache had no room for, the texture's), which the
  compiler makes into blocks inside one another with the last lines in each. One texel
  chosen, one word of it taken: 1.8%, 2.3% and 2.2% more.
- **A float result goes to its register by a write with no branch** (the scratch slot when
  it is none): 0.4%, 0.4%, 0.6% more.

It is 10 bytecode instructions more an emulated one (a shell loop 120.8 to 130.0, Doom
117.2 to 126.1, Quake 136.6 to 141.8, `tools/dxbc_run.py`, the same texels out), and
`tools/tick_path.py` has the new tree's turns. Checked with it: `fptest` 0 of 15 wrong,
`mctest 3 60000` and `mctest 63 20000`, `programs\lazy` off and on (`1a4a1226`), a cold boot
to the desktop (25,733,311 instructions in 8,999 passes, the tree's state hash), and the
raytracer's machine, which has no floats and no paging (the same state, 0.3% faster).
Windows' FXC compiles the fragment stage (494 s); nothing was run under it, the tick's
geometry stage not compiling there.

### Where an instruction's operations are

`python tools/dxbc_run.py TICK.asm SNAPSHOT CONSTANTS --lines FILE` writes how often each
line of the bytecode ran. One pass of 8,192 of each load, the tree before this section,
bytecode instructions an emulated one by what they are for:

| | A shell loop | Doom's demo | Quake e1m1 |
|---|---|---|---|
| fetch (the texel's test, the window, the pc, the turn) | 20.7 | 18.0 | 18.4 |
| decode (fields, the kinds' tests, whether the encoding exists) | 28.2 | 28.0 | 30.6 |
| the two register reads | 2.0 | 2.0 | 2.0 |
| execute, integer | 31.0 | 30.5 | 25.6 |
| execute, float | 0 | 0.1 | 14.8 |
| the data TLB (first level, and the second on a miss) | 10.3 | 10.8 | 10.0 |
| the write cache (a load's search, a store) | 14.6 | 15.0 | 19.4 |
| a data texel read | 2.0 | 2.5 | 1.3 |
| the result's write | 4.0 | 4.0 | 4.0 |
| the loop (was it this path's, the budget) | 5.0 | 5.0 | 5.0 |
| the fetch's TLB | 2.1 | 0.5 | 2.2 |
| all | 119.9 | 116.6 | 133.2 |
| of them tests that branch | 9.7 | 9.7 | 11.8 |
| texels read: instruction, data | 0.52, 0.16 | 0.40, 0.22 | 0.41, 0.20 |

Fetch and decode are two fifths of the count, and what a predecoded form could take of them
is the existence tests and the immediates, some ten operations. That bound was measured:
the existence tests of arithmetic and of the jumps taken out (four operations an
instruction fewer, the same state) is 1.0 to 1.4% slower.

### What costs, measured on this path

Two runs of one build differ by 0.1% here (2,000 passes, a warm card), and the same work
written in another order is the same speed to that 0.1% (three such, alone and together).
So a difference of half a percent between two builds is the builds'.

- An operation more on the last one's result: 1.1 ns (24 of them 26.6 ns, 48 of them 50).
- An operation more that nothing waits for: 0.55 ns. Arithmetic, branches and jumps as one
  block with no branch in it (55 operations more an instruction) is 13% slower on the
  integer loads; a load's and a store's address and first-level translation worked out
  before the kind is known (44 more) 9%; both 20%. Nothing overlaps for nothing.
- A test and its block that an instruction has to pass: 16 to 19 ns. The kinds as eight ifs
  one after another, no else, is 39% slower (134 ns an instruction, seven blocks more to
  pass); a place down the row of tests is 16 ns (above).
- But a count of tests does not say it either: in the tree arithmetic passes three tests
  where it passed one, and is faster. Only the two builds in turn say.
- Values changed in a rare branch and alive after the loop: nothing (8, 16 and 32 more).
- The tests' results as values worked out first (`bool k = ...`) and the same tree on
  those: -0.5%, -0.1%, +4.6%, where the tests written in the ifs are +4.1%, +4.3%, +8.9%.

So the fifth that was looked for is not in the operations (all of fetch and decode for
nothing would be some 25 ns of 220 to 270), and what is left of it is in which blocks an
instruction passes on its way, which nothing predicts: each shape has to be run.

## Measure with the card's clock high

The card runs at 1,455 MHz (state P4) for the first two to ten seconds of a load and at
2,835 MHz (P2) after, and the emulator's speed is the clock's: the same snapshot ran at
1.70M instructions a second and at 3.41M. A short benchmark after a pause measures the
governor. Warm the card first and watch `nvidia-smi --query-gpu=clocks.gr` while it runs;
compare two builds by running them in turn (A B A B), long runs, from one and the same
snapshot (two snapshots of "the same" work differ by more than most changes are worth), and
expect two runs of one build to differ by half a percent. A cold boot is inside those first seconds: it takes twice as long on a
card that was idle as on one that was busy.

## Kinds of instruction

`programs/classes` (`--image classes`): loops of one kind each, a mark between them, timed
with `--stats-from P2 --until P3`. Bare metal, no paging. Nanoseconds an instruction, a pass's
own cost (below) taken off:

| | 8,192 a pass |
|---|---|
| register arithmetic (15 and a branch back) | 135 |
| a two-instruction loop within one instruction texel | 120 |
| loads from one page | 215 |
| stores to texels already in the write cache | 290 |
| a jump or a branch taken to another texel | about 235 |

Under Linux a load costs more than that: nine in ten pass the stored-addresses filter and
search the cache's tags (`--profile`: `ram_read_bloom_pass`; `L1_HAS` now stops most of
those, "Where the time was").

## A pass besides its instructions

Two instructions a pass (`--ticks 2`), milliseconds a pass:

| | |
|---|---|
| D3D11 as it is | 0.134 |
| without the GPU device | 0.102 |
| one target, one core (`--no-mrt`) | 0.087 |
| one target, one core, no GPU device | 0.067 |
| D3D12 | 0.088 |

A shell loop is 2.37 ms a pass of 8,192, so this is 5.6% of it, and a seventh of a pass of
2,000 (a desktop's, a file being read). Instructions a second by pass length, the same loop:
2.81M at 2,048, 3.21M at 4,096, 3.45M at 8,192, 3.72M at 16,384, 3.93M at 32,768.

## What things cost

A way to time one thing: `RVC_FREEZE=1` in the environment and the D3D11 harness draws the
tick and nothing after it, so the state is never written back and every frame runs the same
pass (give it `--fixed-dt 0.000000001` for guest time to stand still too, or every frame
takes the timer's interrupt). `--bench` then prints `best_ms`, the fastest 1,024 frames'
time a frame, which two runs give within half a percent. Work added to every fast instruction
(`--define X_P_ALU=8`, `X_P_READ=7`, `X_P_TEX=1`: `emu.h`) is then so many nanoseconds an
instruction (`RVC_TICK_SCISSOR="x0 y0 x1 y1"` draws only those pixels of the tick):

| Added to every instruction | Nanoseconds |
|---|---|
| 24 operations, each on the last one's result | 31 (1.3 each) |
| 48 of them | 51 (1.1 each) |
| four reads of an array, among 32 of its elements | 8 |
| ... among 64, 128 | 12, 18 |
| ... among 256, 512 | 50, 100 |
| all four words of a `uint4` element chosen at random among 256 | 49 |
| one word of it | 10 |
| a write of an array's element; of a `uint4` one | 2; 4 |
| a texel of RAM read and used, the same one every time | 36 |
| ... a different one every time; two | 86; 211 |

So: an operation is a nanosecond and a bit, whether or not anything waits for it. An array
element that was read lately costs two or three; about a hundred elements stay that way, and
past that every read is 12 to 25 ns, one after another (a `uint4` element is four of them,
and so is one of its words chosen by a number: the four are read and one kept). A write
costs next to nothing. A texel of RAM is 36 ns when the texture's cache has it and a hundred
when it does not.

And what does not matter: **how many pixels run the tick.** The same pass drawn for one
pixel takes 2.07 ms, for one block of 32 pixels 2.03, for all 128 of core 0's 2.02. So there
is nothing to be had from fewer pixels, from one invocation in a vertex or geometry shader
with the state sent on as points, or from where the pixels are. Nor does the harness's wait
for its readback matter (spinning on `Map`, a longer ring, sixteen frames a `Map`: nothing),
and the card's clock under this load is the highest it reaches (2,835 MHz, P0).

## What a pass is made of

`tools\pass_costs.ps1` times one pass of each of seven kinds of work under Linux at three
places (a shell loop, Doom's demo, gzip, sha256sum, find, dd, grep) on a frozen state and
counts the same pass with `--profile`; `python tools\pass_regress.py logs\pass_costs.csv` fits
a cost to each kind of event. Twenty-one passes cannot tell apart counts that rise and fall
together, so the kinds are few and the figures rough (the passes are fitted within 5%):

| | Nanoseconds each | Of the time |
|---|---|---|
| an instruction that is no load or store | 115 | 33% |
| a load | 450 | 35% |
| a store (and 180 more when it searches the cache) | 370 | 29% |
| an instruction on the general path | 2,700 | 2.5% |

Loads and stores are 39% of a shell loop's instructions and two thirds of its time; a pass of
sha256sum, which is 85% arithmetic, takes 1.2 ms where the others take 1.9 to 2.7. What is
left in a load is mostly the texture: 0.09 (a shell loop) to 0.17 (Doom) data texels read an
instruction. Sixteen texels remembered would have had a third to a half of those
(`--profile` counted it), at two array reads for every load that gets that far: not tried.

The timer's interrupt is 2.5% of a shell loop at `--fixed-dt 0.004` (which has it every 2.5
passes; real time every five): 4.07M against 3.97M with 0.04 ms a pass.

## One workload, the builds there are

A shell loop, passes of 8,192, long runs in turn:

| | Instructions a second |
|---|---|
| D3D11, eight targets, 256 cores (the default) | 3.43 to 3.48M |
| D3D11, eight targets, 16 cores | 3.52M |
| D3D11, one target, one core | 3.54M |
| D3D12, DXC's bytecode, one target, one core | 3.43M |
| D3D12, fxc2's bytecode (`RVC12_DXBC=1`), one target, one core | 3.67M |

The eight targets no longer make core 0 faster by themselves (they did while its state was
1,485 texels): what they cost is the pass's own 0.05 ms.

## Bytecode instructions an emulated one costs

Measured, by running the bytecode: `tools/dxbc_run.py TICK.asm SNAPSHOT CONSTANTS` is an
interpreter of the tick's pixel shader as `dxbc_disasm` prints it (`docs/world/linux.md`). It runs
one pixel of one pass (the pixel whose texels have the instruction clock) from a snapshot
taken before the pass, with the pass's constant buffer (`RVC_DUMP_CB=FILE` while the harness
runs it), and counts every instruction executed. Its eight output texels are the harness's
after the same pass, to the word (the commit's count, which the commit adds, aside): that is
the check that what it counts is what ran.

Before and after the fast step was rewritten for it (October 2026), the same four passes of
8,192 emulated instructions each, both builds giving the same texels:

| The pass | Bytecode instructions, before | After | For an emulated one | Emulated for a bytecode one |
|---|---|---|---|---|
| a shell loop, pass 40 | 1,318,187 | 1,183,065 | 160.9 to 144.4 | 11.4% more |
| a shell loop, pass 200 | 1,298,979 | 1,163,140 | 158.6 to 142.0 | 11.7% |
| Doom's demo, pass 40 | 1,236,392 | 1,106,049 | 150.9 to 135.0 | 11.8% |
| Doom's demo, pass 200 | 1,215,510 | 1,077,099 | 148.4 to 131.5 | 12.9% |
| all four | | | 154.7 to 138.2 | 11.9% |

(About nine of those are the pass's own beginning and end, not the fast loop's.)

`tools/tick_path.py TICK.asm [TICK2.asm]` is the same count without running anything: the
fast loop walked along the path each kind of instruction takes, weighed as a shell loop runs
them (`--profile` has the shares: `x_alu`, `x_load`, `x_store`, `x_branch`, `x_jump`,
`x_refetch`). It says where a change takes instructions out, by kind: arithmetic 112 to 89,
a load from the write cache 160 to 146, a store to a texel in the cache 173 to 162, a branch
92 to 82, a jump 106 to 97 (and 27 more for any of them when the next instruction texel
has to be taken up).

In time the gain is far less, the driver having done most of it already. Both builds on one
and the same snapshot, in turn, the card's clock high, the same state hash at the end: a
shell loop 3.908M to 3.983M instructions a second (1.9%), Doom's demo 3.676M to 3.740M
(1.7%), and of that 1.3% is the harness no longer drawing or copying anything of the
workers' while the machine has none. On WARP (the shader as x86 code): 1.6%.

(Figures of 4 to 7% in this change's first commit messages were taken with a snapshot made
by each build for itself: the two were then at different places in the guest's work, which
is worth more than the change. One snapshot for both builds, always.)

What was taken out, all of it on the path of every instruction or of every arithmetic one:

- whether an arithmetic encoding exists was 17 instructions of comparisons; it is a bit of a
  small table chosen by funct7, shifted by funct3 (8);
- a sign is carried by an arithmetic shift (`asint(w) >> 20`, one instruction) where a test,
  a select and a mask were four: the immediates, `sra`, a load's byte or half;
- a signed comparison is the unsigned one with both signs turned over;
- which funct3 is a load is a bit of a constant, and "in RAM" one comparison after the
  address's top bit is turned over;
- the two low bits of an instruction are in each kind's own test, not tested for all;
- the loop's end is one comparison with a budget that a store which filled the write cache
  sets to nothing, where it was the budget or the stall;
- the fetched word is kept for the general path only when the step declined it.

And what the driver does not forgive, each tried by itself on the same path: the "next
instruction is in this texel" flag worked out by each kind in place of once at the end
(3.6% slower, though three instructions fewer), register 0 turned into the scratch register
at decode (2% slower), the old four-instruction immediate (3% slower than the shift). A
count is a guide; only the two builds run in turn say what a change is worth.

## Tried, and not kept

- The run's end looked for only after a store or a jump taken, in place of after every
  instruction: one branch fewer for two instructions in three, and slower (arithmetic 153 ns
  against 150; as a value returned from the step, much slower).
- The step without its debug word; the write cache's tags zeroed at a pass's start in place
  of an occupancy bit looked up at every load and store (stores 292 ns against 305 alone,
  nothing on Linux); a second-level TLB of 512 entries (its two arrays as one, so that the
  shader's arrays stay under the 4,096 elements the driver takes): nothing, each.
- Core 0's pixels drawn by a build of the pass without `CORES`, the workers' by the build
  that has them: the same state, and nothing.
- One buffer kept as the one the tick reads, each commit copied back into it (in case a
  texel read after the other buffer's turn were a slow one): nothing.
- The GPU device's first pass drawn only when a list is submitted: 0.008 ms a pass.
- `add` and `addi` (most of the arithmetic) taken by a branch of their own in place of the
  eight results computed and one selected: nothing (the driver makes a select of a small
  branch anyway). The instruction's two ends, "not taken by this path" and "the budget is
  spent", as one test: 2% slower.
- The write cache's texels as an array of words in place of one of `uint4`, so that a load
  reads one element and not four: 0.5% by itself, 3% slower with the remembered texels.
- Four texels of RAM remembered in place of one for data reads: the counts say a third fewer
  texture reads, which choosing among four costs back.

Of 13 October 2026 (an RTX 5090; a shell loop, Doom's demo, Quake e1m1, each against the
tree in turn):

- The first-level TLB as arrays of 8 or 16 pages a mode, an element read in place of a
  choice among a vector's four: its misses fall by half (Quake's loads 21% to 10%) and it is
  3%, 3% and 2.4% slower. Searched whole (a vector compared at once, the newest page first),
  4 or 8 pages: 0.4% slower.
- No read-ahead of the next instruction texel (a third fewer texture reads): 3 to 5% slower.
- A jump's target texel asked for where the target is known, into the window's second place:
  2% slower and worse.
- Three texels of the write cache remembered in place of two (a fifth fewer searches): level.
- A float result sent to its register by the instruction's one write at the end (`rd` made
  the float register's number) in place of a write and a branch of its own: 4.5% slower on
  the two loads that run no float at all. Nothing an instruction of another kind passes
  through is touched for one kind's sake.
- Not built, by their counts: sixteen data texels remembered would have 24% (Doom) to 62% (a
  shell loop) of the data texture reads, 32 instruction texels 34% (Quake) to 56% (Doom) of
  the jumps' (the code a pass runs is wide), each for an array's reads at every miss.

Of the second round of 13 October 2026 (the same three loads, each against the tree in turn;
a shell loop, Doom's demo, Quake):

- A straight run into the next texel of the page taken by a short way of its own (no page,
  alignment or window test; 3.7 operations fewer): -4.2%, -2.3%, -3.1%.
- The float block out of the fast loop, for a build of integer programs (a twentieth of the
  bytecode less): -2.9% and -3.0% on the two integer loads. A smaller pass is not a faster one.
- The page of the last load, and of the last store, tried before the first level's four:
  +0.1%, -0.1%, -0.4%. A load's funct3 and "within the word" as one bit of a table (five
  operations fewer a load): 0.0%. The M extension's test inside arithmetic's block: -0.9%,
  -0.2%, -0.4%.
- A store's two buckets worked out only when it has to search (15 operations fewer for
  four stores in seven): -2.1%, -2.5%, -2.1%.
- The run's end looked for where a texel is entered (at most four instructions run from
  there; the last three of a pass on the general path), no test of it an instruction:
  0.0%, -0.3%, -0.8%.
- "Was it this path's" as a test that leaves the loop, the result written after it in the
  loop: -2.4%, -2.5%, -2.3%. With that, an instruction within its texel going round by
  `continue` and the fetch below it in the loop: -7.9%, -6.7%, -6.6%.
- The step with one way out (its end inside `if (ok)`): -3.6%, -3.3%, -3.2% on the tree.
  The fetch's "general path takes it" as a test after its block: -1.4%, -1.1%, -1.0%.
- Other trees of the kinds, of thirty: arithmetic and loads a group and the rest a row,
  +1.6%, +1.4%, +3.4%; branches and jumps a group as well, +0.9%, +0.6%, +5.7%; arithmetic,
  loads, stores and the M extension by one test of the opcode (`(w & 0x4f) == 0x03`),
  -0.1%, +0.5%, +3.5%, and with the floats or the branches in that group -6%. `flw`, `fsw`
  and opcode 0x53 as three kinds of the tree: -3% to -4.5% under the one kept.
- A whole word into a texel of the cache as the texel read, changed and written (no test of
  which word): -2.0%, -1.0%, -1.6%; as four small ifs: -0.8%, -0.4%, -0.4%.

A pass at 8,192 and at 32,768 differs in its events too when timed with `--fixed-dt 0.004`:
that is 4 ms of guest time a pass whatever its length, so the kernel's timer interrupt (some
60 page walks and 65 general-path instructions each) comes four times as often an instruction
in the shorter pass. Real time has it a hundred times a second either way.

## The window's own texel not asked after, and what a block costs (13 October 2026, the third round)

Looked for: the same fifth, in the functions the second round left (`mem_set_ram`, the fetch,
the data TLB, the loop's end), by the bytecode's lowering. Found: 1.0%. The same card and
method (D3D11 + fxc2, passes of 8,192, one snapshot, in turn, twice, 2,000 passes, the same
state hash and instruction count), against the tree as the section above left it:

| | Before | After | |
|---|---|---|---|
| a shell loop | 4.699M instructions a second | 4.763M | 1.3% |
| Doom's timedemo (`-nodraw`) | 4.523M | 4.565M | 0.9% |
| Quake, e1m1 where it starts | 4.053M | 4.087M | 0.9% |

(Three batches gave 1.3, 1.4, 1.3; 1.0, 0.9, 0.9; 0.8, 0.8, 0.9. Over the tree of that
morning: 7.4%, 11.3% and 19.0%.)

- **A texel entered is not asked whether it is the window's own** (`fast_run_l1`,
  `f_t != fw_addr0`). It was the window's own 3 times in 1,000 instructions (a jump within
  the texel), and those read it again now; the other 390 pass a block less.

Checked with it: `fptest` 0 of 15 wrong, `mctest 3 60000` and `mctest 63 20000`,
`programs\lazy` off and on (`1a4a1226`), a cold boot to the desktop (25,733,311 instructions
in 8,999 passes, state `7a86a436e5ce44b2`), the `--profile` build compiles. Not run: Windows'
FXC, the raytracer's machine, passes of 16,384.

### What a block costs, added by itself

Work added after the kinds and before the result's write, each build against the tree in
turn (one clean batch each; nanoseconds an instruction, a shell loop, Doom, Quake alike):

| Added to every instruction | Nanoseconds |
|---|---|
| a write of an array's element | 1 |
| ... inside a test that never holds, or that always holds | 1 (the driver makes it a write with no branch) |
| a test that never holds, with 3, 6, 12, 18, 30, 300, 1,500 operations in it | 10, 8, 14, 13, 15.5, 17.5, 17.5 |
| the same with a loop of 100 turns in it | 17.5 |
| 30 operations, no test; in a test that always holds | 32; 44 |
| a test that never holds, with a texture read in it | 46 |
| two blocks (`if`, `else`), a write in each | 7 |
| a value chosen into the result (`if (..) val = pc`) | 4 |

So a block that is not made into a choice costs 8 to 17 ns to pass over and 12 to enter,
whatever its size, and a small block with a texture read in it has its read made whether or
not the test holds. How deep in blocks the loop lies is nothing (four more round it: -0.2%).
None of it says what a given shape will cost: of what follows, most takes blocks out of a path.

### Tried, and not kept (a shell loop, Doom, Quake; against the tree in turn)

- The window's read of a jump's texel in other shapes. Read into the second place and moved
  (`if (f_t != fw_addr1) fw_tex1 = read; fw_tex0 = fw_tex1`): -7.7%, -5.9%, -6.2%, and with
  no outer test -4.2%, -2.0%, -3.8%. The read ahead written in both ways of the test:
  -10.8%, -8.4%, -8.5%. Read always and chosen: -3.2%, -2.2%, -2.1%. The read ahead asked for
  first: 0.0%. The two ways changed over: +0.3%, +0.2%, +0.2%. The page of the last fetch
  and "the texel read ahead" as one test, the rest in its else: +0.5%, +0.7%, +0.4% (under
  the kept one).
- The texel a jump left and the one it came to kept (16 to 21% of the jumps' reads are one
  of them): -2.6%, -0.9%, -1.7%; of the last two jumps (20 to 38%): -3.1%, -1.7%, -1.9%.
- The data texel after the one read, read ahead as the window's is: not timed. It is the
  next one wanted for 6 to 10% of a load's reads (a store's: 17%, 0%, 49%).
- The loop's body twice a turn, every test kept (the same bytecode instructions run): -2.9%,
  -4.6%, -4.4%; three times: -5.4%, -5.8%, -9.6%.
- The step's end and the loop's as one test (nothing written for an instruction not taken,
  by choices): -1.2%, -0.7%, -1.0%. With that, a loop of one texel's instructions inside the
  loop of texels, so that no instruction asks "the same texel": -3.0%, -0.7%, -1.7%.
- A store that looks for its texel as a load does (not one the cache was never given, nor
  the one looked for last and not found; the bucket's tags read again when an entry is
  made): Quake's stores search 123 times in 1,000 instructions and find 15, and it is +0.2%,
  -0.1%, +0.2% (2.7, 1.3 and 5.8 bytecode instructions fewer). With a texel the cache has
  and one it has not each by itself: 0.0%. `mem_set_ram` with one way out (its callers say
  the word is in RAM): -0.6%, -0.6%, -0.4%.
- A load's search under one test in place of three: -0.7%, +0.4%, +0.2%. Its last lines as
  two ways (the cache's word, the texture's): -2.3%, -1.2%, -1.5%. A word load passing over
  the shifts of a byte and a half: -1.4%, -1.7%, -2.1%.
- A first-level TLB hit and "it is a load of RAM" as one test, the miss's work and the load
  again in its else: -0.4%, -0.4%, -1.4%; a store's too: +0.6%, -0.3%, -1.3%.
- A branch's target worked out only when it is taken: -1.2%, -0.5%, -1.5%. Its comparison
  as arithmetic's (signs turned over): +0.1%, +0.7%, -0.7%.
- A float's first operand by one read with no branch: -0.3%, -0.2%, -0.4%.
- A block's texture read kept from being made early (a write of an array put beside it):
  -0.6% to +0.1%; it is not made early in the tree's shapes.
- Together: the kept one with the store's search +0.6%, 0.0%, +0.5%; with the load's one
  test +0.2%, +0.9%, +0.6%. Each under the kept one alone.

### Not built: the general path's share

`--profile` on the three snapshots, 300 passes with `--fixed-dt 0.004`: 2.1, 4.0 and 6.4
instructions in 1,000 take the general path (5,173, 9,816 and 15,702), which at 2,700 ns is
some 3%, 5% and 7% of the time. Of them 55%, 43% and 40% are the kernel's CSR instructions
(a timer interrupt every 2.5 passes at that step: twice real time's), and 36%, 51% and 46%
are loads, stores and fetches whose page neither TLB level had (`w_other_page`: 5, 10 and 20
walks a pass). `mmu_translate` called where the fast step declines such an instruction, and
the instruction run again on the fast path, was written down and not built; so was a second
level of more than 256 pages (it moves the state's texels).

## gpu_vm's floor: what the pieces of a step cost (23 October 2026)

Before a new gpu_vm tick was designed (x30), its pieces were timed alone: `gpu_vm/tick/probes/floor.hlsl` is a loop
whose turn is one bundle of a synthetic program, its parts chosen by defines, and `floor.cpp` (`bin\floor.exe`,
`probes\build_floor.bat`) times it on the card by the GPU's clock (two lengths, the difference; the best of five or
seven rounds), one warp a core (8 x 4 pixels), and checks its output against WARP's. The probe lists are
`probes/floor1.txt` to `floor6.txt`. RTX 5070 Laptop at 2.8 GHz, one core (as gvmbench's ns cases; two runs agree to
0.1 ns), ns a bundle and the timed loop's DXBC instructions; v0009's integer bundle is 103 ns:

| Probe | ns | DXBC |
|---|---|---|
| straight code: the next bundle fetched, no work (its address does not wait for it) | 16.9 | 13 |
| both next bundles fetched, the taken one chosen by a bit of the bundle (the tick's shape) | 48.5 | 19 |
| ... the fall-through fetched a bundle ahead, chosen | 53.6 | 28 |
| ... the target fetched by a branch only when taken | 55.0 | 36 |
| one add (two register reads, a write), straight code | 27.7 | 22 |
| two adds, straight code | 42.9 | 31 |
| one add, both bundles fetched | 52.7 | 28 |
| two adds | 63.9 | 37 |
| ... no register writes | 52.8 | 39 |
| two ops of a unit of 2, 4, 8, 16 functions chosen by selects | 65.7, 69.4, 70.5, 67.5 | 45-98 |
| two ops of x23's mid unit; with the I/R choice | 75.3; 74.6 | 110; 112 |
| two ops of 8 float functions; an add and the float unit behind a branch | 68.9; 70.5 | 86; 65 |
| the 8-function unit and a branch on a register != 0, read with the ops' operands | 77.6 | 81 |
| ... on lt/ltu/eq of two registers, a sense bit; x23's 16 kinds | 88.4; 93.9 | 94; 104 |
| ... the same three with the registers read after the ops' writes | 116.1, 125.0, 126.7 | 82-105 |
| ... on op 1's result != 0 (forwarded, no read) | 102.0 | 79 |
| x23's mid unit, I/R, 16 kinds read first (the tick's integer path without its bookkeeping) | 112.2 | 142 |
| two adds with 8 or 16 registers in the GPU's own registers (a tree of selects); with a branch on one | 52.1, 67.2; 74.9, 93.0 | 96-239 |
| two chains of two adds a turn (two cores' bundles in one warp) | 97.1 (48.6 each) | 79 |
| a load (the data texel, no write cache) and an add | 83.4 | 48 |
| ... through a direct-mapped write cache of one tag; two buckets of four (x23's eight tags); behind the bloom word | 170.3; 110.7; 113.2 | 56; 103; 113 |
| a store into the direct-mapped cache (tag and word written) and an add | 422.2 | 61 |

A dependent chase through a local array (`CHASE`): 17 ns a read in the L1 cache, 106-117 in L2. One warp an SM keeps
256 words a lane (32 KB a warp) in L1 and not 1,024; at 255 cores (7 warps an SM) 64 words a lane are already 33 ns and
256 are 116. With all 255 cores busy every probe above is 1.2-4x dearer (two adds 77, the 8-unit 103, a branch on a
register 115, x23's buckets 393): the 32 lanes of a core each keep the same registers and write cache, so a warp's
local memory is 32 times its state.

So, for one core, a bundle's floor is the fetch: its next bundle's address waits for the bundle (48.5 ns, 47% of
v0009's 103), and a bundle that does anything useful costs more: two ops 64, two ops and a branch on a register 78 (the
cheapest test, read with the operands), the same with x23's branch kinds 94. Half of today's bundle (52 ns) is the
fetch and one op; a step of two ops and a branch cannot be 2x v0009's on this card in a fragment shader with the
registers in local memory, and the GPU's registers (a tree of selects) are no cheaper past 8. What a unit holds is
nearly free (2 to 16 functions: 66-71 ns); what costs is every dependence on a fetched or loaded value: the branch's
register reads after a store (+38-50 ns), the write cache's tag reads (+27-87 ns over the texel alone), a store into
local memory that the next bundle reads (+250 ns). Not measured: SASS counts (the GPU trace exports no shader's code;
its SASS view is the Nsight UI's only), so the DXBC counts are the proxy.

### Blocks: the fetch paid once a block (phase 1b)

The fetch's chain is paid once a block rather than once a bundle when K bundles are fetched together from addresses that
do not wait for them and run straight, with one decision at the block's end (`BLOCK=K`, `floor7.txt` to `floor9.txt`;
two ops of the 8-function unit a bundle, the decision a register != 0). One core, ns a bundle:

| K | 1 | 2 | 3 | 4 | 6 | 8 |
|---|---|---|---|---|---|---|
| the decision on the last bundle's register, after the ops | 113.2 | 81.8 | 69.4 | 64.2 | 59.3 | 57.6 |
| the decision from the first bundle, before the ops (the next block in flight while they run) | 79.4 | 62.3 | 58.9 | 57.0 | 55.2 | 52.8 |

Today's shape with the same unit and test is 77.7. Loads in blocks (op 0 a load in every bundle, or in every other; K 4):
the data texel alone 86.4 and 68.3 (today's shape 83.5 every bundle), x23's two buckets of four 125.3 and 93.8 (110.4).
In a block a bundle's register write of a loaded value holds every later bundle (a store of a value still in flight
stops the warp's issue), so the loads that overlapped the fetch are on the path now. Written at the block's end instead
(`DEFER`, the compiler keeping a load's uses in later blocks): every other bundle a load, K 2, 4, 8: the texel 68.9,
61.8, 64.4; two buckets 78.4, 83.2, 77.7.

Guest blocks (`gpu_vm/tick/x30/blocks.py` on gvm_cpu's profiles of the po builds, `build/bench_games/prof_lay/po`):
bundles a run between decisions (a branch, addbnz, a j, a call, a return) are 3.09 for Quake III's core 0, 3.95 its
workers, 3.33 Half-Life's core 0, 4.00 its workers; of the bundles 10-12% are in runs of 1, 11-16% of 2, 14-26% of 3-4,
16-34% of 5-8, 18-42% longer; 48-59% hold a load or store. Weighed so, a block step costs about 59.5 ns an integer bundle
against 77.7 (1.31x) and about 81 ns a bundle of the games' mix with the write cache and deferred loads against 94
(1.16x; 1.06x without deferral, 1.2x with no write cache at all). The fetch is not where the rest of the time is: a
block of straight bundles is bound by its registers' and loads' chains through local memory, which the in-order warp
cannot overlap. Short of 1.6x: x30 not built on it.
