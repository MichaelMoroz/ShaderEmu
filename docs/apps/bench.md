# The benchmark: nxperf

`nxperf` (`linux/bench`; Start menu: Utilities, "Benchmark") measures the machine: the first core's
arithmetic, branches and floats, memory as one core loads and stores it, the worker cores from 1 to
255, the lanes and the field, numerical kernels, libmedia and the GPU. There are 61 tests in seven
groups. Each gives one number in its unit and checks what its work made: a result with no check
would say nothing of a machine that is wrong.

    wsl -- bash linux/nommu/build.sh bench/build.sh     (after Nano-X's and libmedia's builds)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1
    / # nano-X -p & nxperf                              the window
    / # nxperf --cli [NAME|GROUP ...]                    a line a test (all when none named), then a count
    / # nxperf --list                                    the tests, their groups and units

The window shows one group at a time as bars: a bar is its value against the largest value of the
same unit in the group, blue where the check held and red where it did not. "Run all" runs every
test, "Run group" (Enter) the group shown, a click on a row that test alone; Stop (Esc) ends a run
after the test in hand, and Up and Down choose the group. The GPU's tests draw into the black square
under the groups. Each result is kept at once in the registry as `nxperf.NAME`, its value and its
check (`1` held, `0` did not, `-` not run), and the window shows the last results when it opens.
The buttons are 34 pixels high, for a hand in a world. "Score" (S) shows the score page and "Results"
(R) every result on one page, three columns of name, value, unit and check; a click there on a test,
or on the score page on a part's bar, shows that test's group. A Run all that ends without Stop opens
the score page by itself (`NXPERF_RUN=all` starts one at once, `NXPERF_VIEW=score|results` opens on a
page: for captures).

`--cli` prints each result as `nxperf: NAME VALUE UNIT [I i/op] OK|FAIL|not run (what was checked)`,
and ends with `nxperf: N results: A OK, B FAIL, C not run`, then the score ("The score" below): `nxperf:
score S of N results (1,000: the card model), MACHINE`, a `nxperf: part NAME S (N results)` line for each of
the eight parts and a `nxperf: beside it: MACHINE S (...)` line for each reference machine. It exits 1 when a
check failed. It reads and writes no registry key unless `--save` follows `--cli`; with `--save` a run of
every test keeps its score too. It draws the GPU's tests in a window of its
own when a window system runs, and leaves them out otherwise. `NXPERF_WORKERS=N` opens N workers (255
by default); with 0 the tests that want workers are not run.

## How a result is made

A test makes its data first and times only its work, by the guest's clock (`clock_gettime`). The
first core's instructions over the same span come from `rdcycle` and are printed as instructions an
operation (`i/op`) for the tests that run on the first core alone: they count the loop round the
operation too (a divide and a remainder in a loop of ten instructions is 5 i/op), and the kernel's
interrupts in the span.

So what a result means depends on the clock it ran under:

- **On the card** it is the machine's speed.
- **Under `rvc_cpu --pass-model`** (`calibrated-measurements` skill) a pass takes as long as the card's
  would by `tools/card_model.json`: the table below.
- **Under plain `rvc_cpu`** a pass is a fixed number of instructions of the first core, and the
  workers' time is free: only the i/op column and the checks mean anything there.

The sizes are set so that each test takes about 0.1 to 0.5 s of the card's time; `nxperf --cli`
whole is 23 s of the guest's under the model, 31 s on plain `rvc_cpu`.

## The tests

| Group | Test | Unit | Work | Check |
|---|---|---|---|---|
| cpu | `int-alu` | Mop/s | 800,000 operations of a chain of add, xor, shifts, or, sub | the result, the host's gcc's (`c77245e5`) |
| | `int-mul` | Mop/s | 200,000 steps of `x * 1664525 + 1013904223` | x is the map's power by squaring |
| | `int-div` | Mop/s | 40,000 divu and remu | the result, the host's (`fe944a24`) |
| | `branch` | Mop/s | 327,680 branches on random bits | how often each was taken, from the table's bits |
| | `fp-add`, `fp-mul`, `fp-div` | Mflop/s | four chains of 100,000, 50,000, 25,000 | the exact sum; within 2e-4 of `exp(n log1p(m - 1))` |
| | `fp-sqrt` | Mop/s | sqrt(1) + ... + sqrt(65,536) | within 1e-4 of Euler-Maclaurin's sum |
| memory | `load-4`, `load-16`, `load-64`, `load-4k` | MB/s | every word of 1 MB, at strides of 4, 16, 64 and 4,096 bytes | the sum of 0 .. 262,143 |
| | `lat-4k`, `lat-64k`, `lat-256k` | ns/load | 131,072 loads round a random cycle (Sattolo's) of 4, 64, 256 KB | back at the start after whole cycles |
| | `store-seq` | MB/s | every word of 256 KB, four times | every word read back |
| | `wcache-hot` | M/s | 262,144 stores within 1 KB (64 texels) | every texel read back |
| | `wcache-cold` | M/s | 262,144 stores, one a texel, over 1 MB | every texel read back |
| | `copy-loop` | MB/s | 1 MB copied by a loop of words | `memcmp` |
| | `memcpy`, `memset` | MB/s | 4 MB by the C library: the machine's copy and fill (`docs/machine/gpu.md`, "A program's copy") | `memcmp`; every word |
| | `gather` | M/s | 262,144 loads at a random permutation's indices | the sum of what was loaded |
| | `mc-copy`, `mc-fill` | MB/s | 4 MB copied, filled by the workers in 4 KB pieces (`mcw_for`) | `memcmp`; every word |
| cores | `mc-0` .. `mc-255` | Mop/s | 2,048 items a core of 64 rounds of a mix: the first core alone, then 1, 4, 16, 64 and 255 workers | 256 items made again on the first core |
| | `mc-for` | rounds/s | 40 rounds of `mcw_for` over 4,096 words | the last round's words |
| | `mc-call` | calls/s | 30 `mcw_call`s, each joined before the next | every answer |
| | `handoff` | trips/s | 60 round trips of a word to a worker and back | the worker's count and its last word |
| | `counters` | M/s | 2,000 `mcw_add`s from each worker | the settled total |
| | `hybrid-big`, `hybrid-small` | MB/s | 256 KB stored by one worker on a big core (1 to 16, `mcw_fast`), then on a small one | every texel |
| lanes | `lanes` | M/s | 256 indices a worker on its lanes (`mcw_lanes`), a texel each | 256 indices made again |
| | `field` | M/s | 15,872 indices on the field (`mcw_field_begin`) | 256 indices made again |
| | `shade` | Mpx/s | `mcw_shade` of 256 x 256 | 255 pixels made again |
| numbers | `dot`, `axpy`, `gemv`, `gemm` | Mflop/s | sdot and saxpy of 16,384 (8 times), sgemv of 128 (8 times), sgemm of 64 | exact: the operands are small whole numbers, compared with integers' |
| | `gemm-mc` | Mflop/s | sgemm of 256, a row a worker | 64 entries, exact |
| | `lu`, `cholesky` | Mflop/s | factor and solve, 64: LU with partial pivoting (twice), Cholesky of `M^T M + 64 I` (four times) | x's relative error under 1e-3, 1e-4 |
| | `trsv` | Mflop/s | a lower triangular solve of 256, four times | x's relative error under 1e-4 |
| | `qr` | Mflop/s | Householder QR least squares, 64 x 48, twice | x's relative error under 1e-3 |
| | `fft` | Mflop/s (5 N log2 N) | complex radix-2 FFT of 1,024 and its inverse, four times | the round trip within 1e-4, the tone's bin 512 |
| | `sort` | M/s | a quicksort of 8,192 words | in order, the same sum and hash |
| | `hash` | M/s | 8,192 puts and 16,384 gets of a hash table with open addressing | every key put found, none other |
| | `crc32` | MB/s | libmedia's CRC-32 of 128 KB | `123456789` is `cbf43926`; the halves joined are the whole |
| media | `inflate` | MB/s | the PNG's zlib stream (26,671 bytes to 147,648) on one core (`media_inflate_one`) | its Adler-32 |
| | `png-dec` | Mpx/s | a PNG of 256 x 192 on the workers | every row as encoded |
| | `jpeg-enc`, `jpeg-dec` | Mpx/s | a JPEG of 256 x 192 at quality 90 on the workers | a JPEG's markers; PSNR over 30 dB against the source |
| gpu | `gpu-list` | lists/s | 40 lists of a clear, each drawn and waited for (`seglSwap`) | no GL error |
| | `gpu-tris` | Mtri/s | 20 lists of 2,000 triangles of 10 pixels from a vertex array | no GL error |
| | `gpu-quads` | M/s | 20 lists of 400 textured quads (`seglQuad`) | no GL error |
| | `gpu-fill` | Mpx/s | 20 lists of 16 blended quads over the picture | no GL error |
| | `gpu-copy` | MB/s | 2 MB copied within GPU memory (`seglMemory`) | `memcmp` |

The picture the media tests use is made in the program (rings and gradients with a little noise),
and encoded once before the first of them.

## Results

`rvc_cpu --pass-model` with `tools/card_model.json` (1.4005 ms + 0.0111 ms a thousand of core 0's
instructions + 0.00079 ms a thousand of the workers' + 0.5705 ms a GPU list), `RVC_WCACHE=1
RVC_ADFAULT=1 RVC_SOUND=1`, `nano-X -p & sleep 2; nxperf --cli`, 255 workers, 3 November 2026: two
runs. Calibrated, not the card: the model is good to about 7% for programs on the first core, and its
workers' term rests on one program (`nxphys`).

| Test | Run 1 | Run 2 | Unit | Core 0's i/op |
|---|---|---|---|---|
| `int-alu` | 5.06 | 5.06 | Mop/s | 1.09 |
| `int-mul` | 5.59 | 5.35 | Mop/s | 1.02 |
| `int-div` | 1.07 | 1.07 | Mop/s | 5.12 |
| `branch` | 0.976 | 0.981 | Mop/s | 5.61 |
| `fp-add` | 7.25 | 7.25 | Mflop/s | 0.77 |
| `fp-mul` | 3.62 | 3.53 | Mflop/s | 1.54 |
| `fp-div` | 3.52 | 3.52 | Mflop/s | 1.53 |
| `fp-sqrt` | 1.04 | 1.10 | Mop/s | 5.16 |
| `load-4` | 6.17 | 6.17 | MB/s | 3.58 |
| `load-16` | 6.17 | 6.12 | MB/s | 3.56 |
| `load-64` | 6.17 | 6.17 | MB/s | 3.58 |
| `load-4k` | 6.11 | 6.12 | MB/s | 3.60 |
| `lat-4k` | 932.3 | 920.1 | ns/load | 5.12 |
| `lat-64k` | 932.3 | 932.3 | ns/load | 5.13 |
| `lat-256k` | 932.3 | 932.3 | ns/load | 5.09 |
| `store-seq` | 3.52 | 3.53 | MB/s | 2.62 |
| `wcache-hot` | 2.37 | 2.37 | M/s | 2.31 |
| `wcache-cold` | 0.224 | 0.224 | M/s | 4.48 |
| `copy-loop` | 3.52 | 3.52 | MB/s | 2.87 |
| `memcpy` | 187.2 | 187.2 | MB/s | |
| `memset` | 749.0 | 749.0 | MB/s | |
| `gather` | 0.598 | 0.598 | M/s | 9.18 |
| `mc-copy` | 127.1 | 126.3 | MB/s | |
| `mc-fill` | 114.6 | 115.2 | MB/s | |
| `mc-0` | 0.888 | 0.888 | Mop/s | 6.22 |
| `mc-1` | 0.946 | 0.947 | Mop/s | |
| `mc-4` | 3.73 | 3.73 | Mop/s | |
| `mc-16` | 14.15 | 14.15 | Mop/s | |
| `mc-64` | 46.04 | 45.69 | Mop/s | |
| `mc-255` | 104.7 | 105.2 | Mop/s | |
| `mc-for` | 180.2 | 180.2 | rounds/s | |
| `mc-call` | 338.6 | 338.6 | calls/s | |
| `handoff` | 712.6 | 710.9 | trips/s | |
| `counters` | 7.87 | 7.87 | M/s | |
| `hybrid-big` | 1.74 | 1.74 | MB/s | |
| `hybrid-small` | 1.74 | 1.74 | MB/s | |
| `lanes` | 1.76 | 1.76 | M/s | |
| `field` | 0.020 | 0.020 | M/s | |
| `shade` | 3.31 | 3.28 | Mpx/s | |
| `dot` | 2.28 | 2.28 | Mflop/s | 2.41 |
| `axpy` | 1.70 | 1.70 | Mflop/s | 2.94 |
| `gemv` | 2.20 | 2.17 | Mflop/s | 2.52 |
| `gemm` | 1.83 | 1.82 | Mflop/s | 3.01 |
| `gemm-mc` | 152.0 | 151.8 | Mflop/s | |
| `lu` | 1.14 | 1.13 | Mflop/s | 4.77 |
| `cholesky` | 1.30 | 1.30 | Mflop/s | 4.18 |
| `trsv` | 1.52 | 1.52 | Mflop/s | 3.61 |
| `qr` | 1.33 | 1.32 | Mflop/s | 4.12 |
| `fft` | 1.32 | 1.32 | Mflop/s | 3.83 |
| `sort` | 0.047 | 0.047 | M/s | 115.51 |
| `hash` | 0.261 | 0.261 | M/s | 15.99 |
| `crc32` | 0.977 | 0.977 | MB/s | 5.61 |
| `inflate` | 0.370 | 0.371 | MB/s | 14.56 |
| `png-dec` | 0.333 | 0.339 | Mpx/s | |
| `jpeg-enc` | 0.731 | 0.729 | Mpx/s | |
| `jpeg-dec` | 0.250 | 0.248 | Mpx/s | |
| `gpu-list` | 490.2 | 480.8 | lists/s | 1,208 |
| `gpu-tris` | 0.090 | 0.090 | Mtri/s | 55.75 |
| `gpu-quads` | 0.019 | 0.019 | M/s | 279.42 |
| `gpu-fill` | 399.2 | 397.2 | Mpx/s | |
| `gpu-copy` | 187.2 | 184.0 | MB/s | |

The two runs differ by up to 6% (`fp-sqrt`, `int-mul`): a short test's span ends in a pass that the
other run splits otherwise. Every check held in both, and in a run under `RVC_RACES` with
`RVC_RACES_NEAR=4`: 0 changes undone, 0 texels near.

## What it says

- **The first core runs about 5.5 million instructions a second** under the model: `int-alu`'s 5.06
  Mop/s at 1.09 i/op. An operation's cost is its instructions: a mul is one (`int-mul`), and so is a
  float add, mul or divide; `divu`, `fsqrt` and a branch are one each too, and their tests are slower
  only by the loop round them. `fp-add`'s 0.77 i/op is four adds to three instructions of loop.
- **A load costs its instruction, wherever it reads.** The four strides give the same 6.1 MB/s, and a
  pointer chase takes the same 932 ns a load in 4 KB as in 256 KB: the machine reads RAM from its
  texture with no cache between, so there is no locality to gain on loads. 932 ns is the chase's 5.1
  instructions.
- **Stores pay for new texels.** Stores within 64 texels run at 2.37 million a second; one store a texel
  over 1 MB at 0.224 million, 10.6 times fewer: the write cache keeps about 6 KB of new texels a pass
  (AGENTS.md), and a store that finds it full ends the pass. A structure stored to often is laid out by
  what is written together.
- **The machine's copy and fill are the way to move memory**: `memcpy` 187 MB/s (256 KB a pass) and
  `memset` 749 MB/s against 3.52 MB/s for a loop of words, 53 and 213 times. The workers' copy (`mc-copy`,
  127 MB/s on 255 workers, which may not ask for the machine's copy) does not reach one core's `memcpy`.
- **The workers scale, and pay for each other.** 2,048 items a core: 0.95 Mop/s on one worker, 3.73 on 4,
  14.15 on 16, 46.0 on 64 and 104.7 on 255, 110 times one worker. Up to 16 workers a pass is nearly as long
  as one's (15 times one worker's on 16); past that the model makes each worker's instructions lengthen every pass (0.00079 ms a thousand),
  and a full pass of 255 is 1.65 ms more. Each worker then stores less a second (`hybrid-big` alone
  1.74 MB/s; `mc-fill` 0.45 MB/s a worker of 255). The same work on the first core is 0.89 Mop/s, as one
  worker.
- **What a hand-over costs, in passes**: a round of `mcw_for` 5.5 ms, a call and its join 3.0 ms, a word
  there and back 1.4 ms. On `rvc_cpu` the cores run one after the other in a pass, so a worker sees the
  first core's word in the same pass and the trip is one pass; on the card a store is seen a pass later,
  so it is two.
- **`hybrid-big` and `hybrid-small` are the same**, 1.74 MB/s: the kernel lays every worker out with
  tables of 2^5 (2.6 KB of new stores a pass; `docs/machine/multicore.md`, "The geometry"), so the first 16
  cores that `mcw_fast` names have no larger cache than the rest. The test stays to show it if that changes.
- **The lanes and the field are not measured here.** `rvc_cpu` has no lanes: `mcw_lanes` and the field
  make their indices as plain calls (the field's on the first core: 0.020 M/s), which is the output the
  lanes would give but not their speed. On the card the lean tick has them (`docs/machine/lanes.md`).
- **The numerical kernels are 1 to 2.3 Mflop/s on the first core**, 2.4 to 4.8 instructions a flop: a
  flop is its instruction and the loads and loop round it. sgemm on 255 workers is 152 Mflop/s, 83 times
  the first core's.
- **libmedia's decoders on one core and on the workers**: inflating is 14.6 instructions a byte (0.37
  MB/s), a PNG decoded on the workers 0.33 Mpx/s, a JPEG 0.25 Mpx/s and encoded 0.73. The picture is
  small (49,152 pixels): its segments and restart intervals are fewer than the workers.
- **The GPU's tests measure the first core's making of a list and the model's 0.57 ms a list.** `rvc_cpu`
  draws nothing, so `gpu-fill`'s pixels cost nothing here: 399 Mpx/s is the model's lists, and the card's
  fill rate is the card's to say. A clear and a swap are 1,208 instructions, a small triangle from a
  vertex array 56, a textured quad by `seglQuad` 279 (with its clear and swap shared out).

## The host's clock

`rvc_cpu` gives a program the host's own clock and processor: CSRs 0x8c0 and 0x8c1 are the host's nanoseconds since
the run began (reading 0x8c0 latches them, 0x8c1 is the latched high word), 0x8c4 to 0x8cf the processor's CPUID brand
string, four bytes each. nxperf times its tests by that clock where it is not 0 (`perf_now_ns` in `perf_tests.c`), so
on `rvc_cpu` a result is the host's real speed, and says `the host's clock` and the processor on its first lines; the
score page names `rvc_cpu on` the processor. Under `--pass-model` 0x8c0 reads 0 (the card's time is the model's), and
with `RVC_HOST_CLOCK=0`; the tick reads them as 0 too, so on the card and under the model the guest's clock times them
as before. `NXPERF_CLOCK=guest` takes the guest's clock anyway. The card's name is not passed to the guest.

## The score

Each result is divided by the reference machine's (larger is better; a time an operation, `ns/load`, the
other way), a part's score is 1,000 times the geometric mean of its results' ratios, and the whole is the
geometric mean of the parts there are (`linux/bench/perf_score.c`). A test that failed its check or was not
run counts in none. The reference is the machine as the card model has it: the values of "Results" run 1
above, so there every part and the whole are 1,000 (a run of the same binary under the model, 3 November
2026: 1,007, its parts 989 to 1,063). The parts:

| Part | Tests |
|---|---|
| single core | `int-alu`, `int-mul`, `int-div`, `branch` |
| FPU | `fp-add`, `fp-mul`, `fp-div`, `fp-sqrt` |
| memory | the memory group (16) |
| multicore | the cores group (12) |
| lanes | `lanes`, `field`, `shade` |
| numerical | the numbers group (13) |
| media | `inflate`, `png-dec`, `jpeg-enc`, `jpeg-dec` |
| GPU | the GPU group (5) |

The score page has the whole in large type, a bar for each part (the reference's 1,000 a grey mark), the
date and the machine, whether every check held, the last run's score and the best (with their dates), and
the reference machines' scores beside this run's in a table. The machine is told by the field: it has
lanes on the card's lean tick only, so "the card, N lanes in the field" or "no lanes, rvc_cpu or a tick
without them", with the workers, each on a line of its own; on `rvc_cpu` by the host's clock, the processor
(`on AMD Ryzen 9 9950X3D`, CPUID's brand string without its "16-Core Processor").

The bars are logarithmic, from 10 to 10 million (`bar_width`): the card model's 1,000 is a third of the way,
`rvc_cpu`'s tens of thousands half, a native part's millions near the end. Bars by the largest part (before 4
November 2026) left every other part 0 pixels when one was large, as the world's lanes were. The table's
columns are as wide as their widest text, its names say the processor (`rvc_cpu, 5900X, 1 thread`), and a
machine with no values yet (the world on an RTX 5090, until a run with the repeats is written into its column) is
not shown. The window is 700 x 540.

### A test run until it is timed

A test runs again until its timed parts are 200 ms together (`perf_run_test`, `PERF_MIN_NS`), 64 runs at most
on the machine and 4,096 natively (`perf_most_runs`: each run makes its data again, which under `rvc_cpu` is
most of the time; natively 64 runs of a 40 us test were 9% slow, warming up), and its value is the runs' work over
their time. The tests are sized for a few hundred milliseconds of the card's time, so on the card and under the
model most run once, as their reference values were made. In the world the guest's clock moves once a Unity
frame: the field's 9 ms was inside one frame and timed 0 ns (1 ns, `perf_end`), 15,872 indices in a nanosecond,
and the lanes part was 296,317,824 (4 November 2026, the world on an RTX 5090). `--cli` says how many runs a
result took. A run of every test keeps its score in the registry as `nxperf.score`, and
as `nxperf.best` when it is the highest yet: `SCORE PARTS... TIME WORKERS LANES`.

### Reference machines

`perf_machines` and the values table in `perf_score.c`: each machine's results of the same tests, scored by
the same arithmetic, 3 and 4 November 2026.

| Machine | Score | single core | FPU | memory | multicore | lanes | numerical | media | GPU |
|---|---|---|---|---|---|---|---|---|---|
| the card model (reference) | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 |
| 486DX-33 (1990, estimated) | 761 | 1,571 | 327 | 1,585 | 517 | - | 780 | 588 | - |
| RTX 3090, harness (the card, 255 workers) | 1,175 | 1,064 | 762 | 649 | 1,324 | 8,904 | 858 | 990 | 691 |
| rvc_cpu, 5900X, 1 thread (the host's clock) | 23,109 | 55,021 | 28,896 | 42,979 | 19,093 | 3,928 | 28,315 | 8,501 | 65,921 |
| rvc_cpu, 5900X, 24 threads (the same) | 33,244 | 55,959 | 29,833 | 45,328 | 36,687 | 12,913 | 33,478 | 18,837 | 65,992 |
| rvc_cpu, 9950X3D, 1 thread (this PC, the host's clock) | 36,346 | 85,304 | 42,024 | 72,473 | 28,934 | 5,993 | 42,705 | 12,887 | 122,845 |
| rvc_cpu, 9950X3D, 16 threads (the same) | 52,678 | 85,529 | 42,248 | 76,256 | 59,346 | 19,293 | 51,151 | 29,911 | 122,838 |
| 9950X3D, native (this PC, no workers, lanes or GPU) | 2,155,644 | 1,717,048 | 1,240,884 | 2,782,002 | 1,828,829 | - | 4,409,307 | 2,099,189 | - |

- **rvc_cpu, 9950X3D**: this PC, an AMD Ryzen 9 9950X3D, `RVC_THREADS=1` and `=16` (half its 32 threads),
  `tools/test_all.py nxperf`, 4 November 2026, with the repeats ("A test run until it is timed"): 1.57 times
  the 5900X on one thread (`int-alu` 455 Mop/s), 1.58 times its 24 threads on 16.
- **9950X3D, native**: the tests compiled for the build computer, an AMD Ryzen 9 9950X3D, by WSL's gcc -O2 and
  timed by its own clock, one thread (`linux/bench/perf_host.c`, `$WORK/src/bench-build/nxperf-host --table`,
  4,096 runs at most a test). The workers' and lanes' tests and the GPU's are not run there: the machine's
  workers are not the host's threads (`perf_host_mcw.c` only links), so multicore is `mc-0` alone and media
  `inflate` alone, and the whole is of six parts. A second run was within 3%. Against the machine on the card
  the host is about 2,000 times as fast; against rvc_cpu on one thread of a 5900X, 31 times in a single core's
  integer tests.

- **The card**: `rvc_harness --d3d11` through `tools\gpu_queue.ps1` on the RTX 3090, `nano-X -p & sleep 2;
  nxperf --cli`, 255 workers, the field 3,968 lanes, every check held. Against the model: the first core 1 to
  25% faster in its own tests, the lanes real (`field` 1.80 M/s against the model's plain calls, 90 times;
  `shade` 16.4 Mpx/s, 5 times), and the machine's copy and fill far slower: `memcpy` 5.82 MB/s, `memset` 7.41
  and `gpu-copy` 5.76 against the model's 187, 749 and 187, about the speed of a loop of words. The model
  times a copy as a pass's work; the harness's copies are not that fast: not looked into here.
- **rvc_cpu**: plain `rvc_cpu` (no pass model) on an AMD Ryzen 9 5900X, `RVC_THREADS=1` and `=24`, timed by the
  host's clock ("The host's clock"). One thread: the first core 262 Mop/s in `int-alu` at 1.10 i/op, 288 million
  instructions a second, 52 times the model's; 24 threads: the same first core, the workers' `mc-255` 625 Mop/s
  against one thread's 61 (10.2 times), `gemm-mc` 601 Mflop/s. Before the host's clock nxperf timed rvc_cpu by its
  guest clock, a pass a fixed number of the first core's instructions and the workers free: 822, the same at any
  number of threads, which measured that model of time and not the host.
- **486DX-33**: an Intel 486DX at 33 MHz with an 8 KB cache inside, 256 KB outside and 70 ns memory, as most
  were sold in 1990-1991. One number is published: LINPACK at 0.8 Mflop/s, the best on such a machine (33 MHz
  486DX, 256 KB cache, 70 ns memory, the Salford compiler; NA Digest volume 92, issue 25, netlib.org
  `na-digest-html/92/v92n25.html`), which is `lu`, `cholesky`, `trsv` and `qr` here (double precision there,
  single here). **The rest are estimates**, from the i486's cycle counts (Intel's i486 Microprocessor
  Programmer's Reference Manual, 1990: a register add or logic operation 1 clock, `imul` 13 to 42, `div` 40,
  a taken jump 3, `fadd` 8 to 20, `fmul` 11 to 16, `fdiv` 73, `fsqrt` 83 to 87) with the loop round each
  operation, and the bus's 33 MHz, 32 bits and a cache line of 16 bytes for memory: `int-alu` 25 Mop/s,
  `int-mul` 1.6, `int-div` 0.75, `branch` 6.0, `fp-add` 3.3 Mflop/s, `fp-mul` 2.0, `fp-div` 0.45, `fp-sqrt`
  0.37; loads 18, 14, 9 and 5 MB/s at the four strides, a dependent load 90 ns in the first cache, 150 in the
  second, 250 from memory; stores 25 MB/s, `memcpy` 15, `memset` 25, `copy-loop` 12, `gather` 5 M/s; the
  numerical kernels 0.8 to 1.3 Mflop/s; `sort` 0.9 M/s, `hash` 1.5, `crc32` 4 MB/s, `inflate` 1 MB/s, the
  pictures 0.12 to 0.15 Mpx/s. It has one core: the multicore tests that are the same work on more cores
  (`mc-0` to `mc-255` 4.0 Mop/s, `mc-copy` and `mc-fill` its `memcpy` and `memset`, `gemm-mc` its `gemm`)
  are its one core's, and the hand-overs (`mc-for`, `mc-call`, `handoff`, `counters`, `hybrid-*`), the lanes
  and the GPU have no counterpart (-). Its whole is of six parts. A MIPS R3000 of 1990 is not in the table.

## Adding a test

A test is a function of `struct perf_run` in `perf_tests.c` (or `perf_gpu.c`) and a row of
`perf_tests[]`: its name, group, unit, flags (`PERF_WORKERS`, `PERF_GPU`) and a line for the window, and a
row of the values table in `perf_score.c` (the reference's value, and the other machines' or 0): a test with
no reference value counts in no part. It
makes its data, calls `perf_begin` and `perf_end` round its work alone, sets its value with `perf_rate`
or `perf_each_ns` (and `perf_cpo` on the first core), and ends with `perf_check`. A kernel function is
`noipa`, or the compiler may call it once for several repetitions. A result whose work goes to the
workers stores only to memory of its own piece (whole texels: `mcw_for`'s `out_bytes`), and the test set's
case (`tools/test_all.py nxperf`) counts the results: a new test changes its `61`. Run it once with
`RVC_RACES=FILE RVC_RACES_NEAR=4` on `rvc_cpu`: 0 undone.
