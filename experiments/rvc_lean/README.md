# The machine's processor, a shader a draw (an experiment)

`experiments/rvc_opt`'s processor written again, for Linux without an MMU only, in the shape of
a compute shader of before: the tick one file with no includes.
Since 20 October 2026 it runs on `rvc_opt`'s commit, RAM in one texture written in place, and is
faster than `rvc_opt` ("Faster than rvc_opt", below).

- `tick.hlsl`: the tick, every core of the machine. Its vertex, geometry and fragment shaders, and
  with `TICK_UNPACK` the pass that copies its eight targets into the state.
- `main.shader`: the ShaderLab wrapper: `rvc_opt`'s, its tick pass this folder's `tick.hlsl`
  (`HLSLPROGRAM`, not `CGPROGRAM`: the harness, as Unity does, puts `HLSLSupport.cginc` and
  `UnityShaderVariables.cginc` in front of a `CGPROGRAM`, and those declare what the tick declares
  itself, `_Time`), its commit `rvc_opt`'s (`../rvc_opt/src/ram.h` and the rest). The harness gives
  a folder whose `main.shader` brings in `ram.h` RAM in one texture.
- `commit.hlsl` (367 lines): the commit of before, for two state textures (the state rows, the bands
  of RAM something other than a core changed, the cores' stores as points and their copy into the
  other buffer; at `_Init` RAM from the image). Nothing uses it now; a `main.shader` whose commit pass
  is `#include "commit.hlsl"` (git: before 20 October 2026) runs it, and the harness then keeps two
  textures for the folder.
- `attempts.csv`: every variant of the tick tried on the way, with its scores ("Faster than rvc_opt").

Lanes (20 October 2026, `docs/machine/lanes.md`): a worker's pixels each run a function on an index of their
own (`lanes_begin`, `lane_next`, `lane_output`), and with `LANE_FIELD` (this folder's `main.shader`) a field of
3,968 lanes of no core's in rows the cores leave (`field_begin`, `field_output`; the commit's points for it in
`../rvc_opt/src/ram.h`). What they cost a pass of the three loads, from one snapshot each: the medians level.

The GPU device and the sound card are `experiments/rvc_opt`'s (`gpu.shader`, `sound.shader`): the
harness takes them from there when the processor's folder has none.

    bin\rvc_harness.exe --rvc experiments\rvc_lean --image linux-net

The harness treats a folder whose name begins with `rvc_lean` as it does `rvc_opt`
(`isOpt`): the no-MMU machine's defines (`NO_PAGING`, `SBI_HLE`, `FPU`, `CORES=256`, `TICK_MRT`,
`COMMIT_BANDS`, `GPU_DEVICE`), so a copy of the folder elsewhere (`rvc_lean_try`) runs as well.

## What it is

The state texture is the machine's (docs/machine/multicore.md): RAM from row 64, core 0's block
and the workers' strip in rows 0-63, the worker geometry geotune lays out, the write cache of
three tables of buckets of three texels, the mailboxes of `src/mc.h`. A core's texels are where
`rvc_opt` has them (`tick.hlsl`, the comment at `L1_AT`), so the control pass, the host's reads
and the GPU device's passes are as they were.

What is not there: the MMU and its TLBs, the UART, the CSR cache and table (the CSRs Linux uses
are fields), `LAZY_MEMOP`, profiling and the experiments' switches (`X_P_*`, `TICK_LOAD`, `ZB`), the
tick drawn without eight targets, and upstream's switches of instructions: the general step is
one function of tests in the order of how common a kind is (`step`).

The tick:

- A fast loop (`fast`) for what cannot trap or change what interrupts see: the kinds told apart by
  `rvc_opt`'s tree of tests (arithmetic, loads, stores and floats one group, in it arithmetic and loads
  one; then branches, the jumps and `lui`/`auipc`, M), the arithmetic every result made and one chosen, one indexed register write
  (`xr[65]`: x0-x31, f0-f31 as bits, a slot for writes that must not land). Every float of opcode
  0x53 is the loop's (a sum, a difference, a product by themselves, `fp_light`'s signs, the
  lesser and greater, comparisons and moves, `fp_all` for the rest).
- The fetch from the texture (never the write cache), two texels at a time, and after an
  instruction that stayed in its texel the next as the texel turned (no texel compared, no word
  chosen).
- Loads and stores within one word of RAM in the loop, through one function each (`ram_word`,
  `ram_store`); the write cache searched only when the bloom word says it may hold the texel, the two
  texels found or made last remembered, and the one looked for last and not found. A bucket's
  written-this-pass bit is a vector in registers (`occv`), not a local array.
- The CSRs nothing else looks at (sscratch, sepc, scause, stval, stvec, scounteren, the float flags,
  time, the hart) read and written in the loop: the kernel's trap entry and exit are full of them.
- The general step (`step`): any instruction, the loads put together from two words, stores a
  byte at a time when not one whole word, the CSRs, SBI, `sret`, the traps and interrupts, and a
  worker's fault record.

## Faster than rvc_opt (20 October 2026)

On the RTX 5070 laptop, D3D11 and fxc2, Doom's and Quake's timedemos whole, `rvc_opt` and this in
turn (A-B-B-A), the same image:

| | `rvc_opt` | `rvc_lean` |
|---|---|---|
| Doom, ms a frame, round 1 (5,026 tics in realtics) | 16.15, 17.24 cold (2,841) | 15.85, 15.86 (2,788) |
| Quake, ms a frame, round 1 (969 frames) | 11.34, 11.27 | 11.14, 11.14 |
| Doom, round 2 | 15.76, 16.16 (2,772, 2,842) | 15.79, 15.84 (2,777, 2,786) |
| Quake, round 2 | 10.99, 10.63 | 11.09, 10.32 |

(Round 2 is this folder as committed; round 1 the same shaders from a copy. The card drifts by 3 to 7%
in a round; compare means of a round.)

Red Alert's 300 frames `ea72a5d1`, fptest 0 of 15 wrong, mctest PASS, as `rvc_opt`.

The other loads, the same way (20 October 2026, one round each, A-B-B-A):

| | `rvc_opt` | `rvc_lean` | |
|---|---|---|---|
| Red Alert, frames 500-900, ms a frame | 21.20, 21.13 | 20.49, 21.07 | 1.8% faster |
| Quake III, q3dm17 timedemo, ms a frame | 13.29, 14.89 | 14.60, 13.39 | 0.7% faster (runs 10% apart) |
| Tiberian Dawn, mission 3, game frames a second | 53.5, 51.9 | 52.9, 52.5 | level (core 0 4% faster), state `9c84d847` |
| The NES, % of its speed | 568 to 571, 377 | 588 to 595, 393 to 395 | 4% faster |
| nxray, ms a picture | 44.8, 43.0 | 43.7, 42.3 | 1.9% faster |
| nxphys, 120 substeps, ms | 1,442, 1,447 | 1,355, 1,351 | 6.3% faster, sum `44d0d906` |

On the RTX 5090 (the same commit): Doom 1.5 and 2.8% faster, nxray 5.7 and 8.2%, Red Alert level (-0.3
and +1.1%), Quake, factor and gzip with the pairs of opposite signs; nxray's 60 sums, Red Alert's
`f40dec60`, Doom's 5,026 tics and Quake's 969 frames as `rvc_opt`'s.

How it got there, from 2 to 6% behind:

- **The gap was two halves.** Live, from one snapshot of Doom's timedemo (`--load-state`,
  6,000 frames, wall clock): `rvc_opt` 1.617 ms a frame, `rvc_opt` with two state textures
  (`--ping-pong`, this folder's way until now) 1.717, this folder 1.700, and its tick in `rvc_opt`'s
  pipeline 1.656. Two textures cost about 3%; the tick another 2.5%.
- **Frozen timings misled.** One pass drawn again and again (`tick_trace11`, the harness's
  `RVC_FREEZE`, now also on the in-place path) had this tick 1.5 to 2% faster than `rvc_opt`'s; a
  pass's cost depends on the stretch of code it runs (the kernel's timer wheel 13% slower, the rest
  faster), and `[prof]`'s tick column on the two-texture path held time that was not the tick's.
- **A replay bench** (`rvc_harness --replay DIR;DIR...`): the harness loads a snapshot once, builds
  the tick of each folder beside its own, and runs each 1,000 frames from the snapshot's state in
  turn, every device as in a live run, four rounds; a variant costs seconds, not ten minutes. The score
  is time a guest instruction against `rvc_opt`'s tick in the same run (Doom ±0.3%, Quake ±1%: the
  frames' work differs with the timing, ms a frame alone does not compare).
- **The tick's local arrays were the cost.** `rvc_opt`'s fast path, its pixel shader's shape and its
  general step's switches brought into this tick changed nothing or made it slower; taking out the
  32-word bit-a-texel filter (`has`) made it 2.5% faster on Doom and 1.4% on Quake: the card keeps a
  pixel's local arrays in local memory, and the filter was read on every load the bloom word let
  through. The bucket bits as a register vector a little more, and the plain CSRs in the fast loop
  a little more. Then in `rvc_opt`'s in-place pipeline: Doom 1 to 1.8% and Quake 1 to 3.5% faster a
  guest instruction than `rvc_opt`.
- What did not help (`attempts.csv`, 79 rows): the registers four to an entry (+10%), a smaller core
  0 cache (`CORE0_BITS` 5: +7.7% on Doom, the cache full sooner), no fetch ahead (+5%), arithmetic by
  a switch (+8%), the filter as 128 bits in registers, the interrupt CSRs in the fast loop too,
  compiler hints (`[fastopt]`), the unpack drawn in place (no gain), the arrays laid out as
  `rvc_opt`'s.

## Measured

On the RTX 5070 laptop, 19 October 2026, D3D11 and fxc2, four loads: fps or ms, and millions of instructions a second
over the window each measures (all cores, core 0, the workers).

Bytecode (fxc2):

| | `rvc_opt` | `rvc_lean` |
|---|---|---|
| the tick's fragment shader | 301,848 bytes | 116,344 |
| the commit's (the bands; the points; the copy) | 238,120; 81,528; 29,452 | 22,596; 33,052; 640 |

Checked: Linux to its shell, geotune's layout of 255 workers, `fptest` 0 of 15 wrong, `mctest
255 30000` PASS with 0 pages brought, Doom's demo1 told and checked (0 of 5,000 views not as told,
digest `0ab4df93`), nxray's pictures 1-7 the tick's sums.

How it was made faster, one change at a time, two rounds each where they were close (Quake's
timedemo; nxray, 60 pictures, ms a picture):

| | Quake | nxray |
|---|---|---|
| as first written | 15.03 ms | 64.2 |
| the cache's three tables unrolled | 15.20 | 64.2 |
| the fetch's next word as the texel turned | 13.90 | 61.2 |
| every float the loop's | 13.50 | 49.2 |
| the kinds as `rvc_opt`'s tree | 13.10 | 46.8 |
| `rvc_opt` | 12.29 | 45.5 |

Tried and not kept, each on the same two loads: two instructions a turn of the loop (a function
called twice: nxray 47.5 to 49.0 ms), a whole word into a texel the cache has made without
reading it and a texel looked for and not there remembered (the same to the noise), M and the
floats that are not the loop's own in an outer loop as the compute shader has them (nxray 47.3 to
51.5: a division or a root then leaves the inner loop and comes back), and a smaller cache for
core 0 (`CORE0_BITS`, 5: the same; 4: Quake 7% slower, the cache full and the pass over sooner).
Those measured on one card: how large a loop, an array or a cache may be before it costs is the
card's, so where two shapes are equal here the smaller is kept.

And after those, each against the tick as it is, in turn (`factor 999999999989` and `gzip -c
/bin/busybox` twice each by the guest's `time`; Quake's timedemo; nxray's 60 pictures):

| | factor | gzip | Quake | nxray |
|---|---|---|---|---|
| the tick | 21.1 to 21.4 s | 107.7 to 108.7 s | 12.8 to 13.3 ms | 45.7 to 47.2 ms |
| arithmetic tested first by itself, then a group of the loads, stores and floats | 19.02 | 111.57 | 13.00 | 46.5 |
| arithmetic, loads and branches a test each, then stores or floats | 19.58 | 108.25 | 13.14 | 47.2 |
| a branch's or `jal`'s target texel read with the instruction, as the compute shader reads its entries | 25.23 | 122.07 | 15.11 | 52.6 |
| the second register read only when the instruction has one | 22.08 | 108.45 | 13.50 | 46.8 |

A loop of arithmetic gains from its own test, and nothing else does; one more texture read an
instruction costs more than the wait it saves. Nor does the frame's length: 16,384 instructions a
frame (`--ticks`), two rounds both always run and three with two (`--tick-rounds 3 --tick-split 2`)
left the machine at the same 8.0 to 8.4 million instructions a second in Quake (13.85, 12.95 and
13.37 ms); only nxray, whose first core waits for its workers, was faster (39.5, 39.2 and 28.4 ms).

### To `rvc_opt`'s speed (19 October 2026)

After the merge `rvc_opt` was 11 to 12% ahead on what stores (`gzip`, a shell loop, the games) and
level on `factor`. Not its RAM in one texture (`rvc_opt --ping-pong` was as far ahead on `gzip`), nor
its commit (`rvc_opt`'s under this tick was no faster): the tick. Each change below against the one
before and `rvc_opt` in the same round, twice, kept when it was ahead on every load:

- **A store as `rvc_opt`'s** (`ram_store`): the tables one after another, a table's bucket worked out
  only when the one before is full, and a whole word into a texel the cache has written without
  reading it. 2 to 3%.
- **No `?:` round a texture read**: the fetch's `t == fw_t + 1 ? fw1 : TEX(...)` read the texel
  whichever side was taken (HLSL evaluates both). An `if`: 2% on stores, the same on Quake.
- **`flw` and `fsw` the floats'**, not the integer load's and store's: an integer store read a float
  register for nothing. 2%.
- **The picture copied back drawn by rows** (`commit.hlsl`): the plain draw made whole 4 MB bands for
  the GPU's copy, the last commit's copy and their bands 32 and 64 on, and the control words' band;
  now the copy's rows, this commit's and the last's (core 0's texel 39.x), and the control pass's row.
  Quake 11.42 to 10.65 ms.
- **No test of the fetch window**: a jump within the texel reads it again, as `rvc_opt` does. 0.5 to
  1%. And the general step takes the word the fast loop fetched (`pre_w`), 0.6% on Quake.

Not kept: the loads as `rvc_opt`'s (the tables one after another, the texel last found absent
remembered: `gzip` 4.6% slower), M by its kind (the same), `fmadd` and its kin in the fast loop
(Quake 1.7% slower), and the commit's points out early for a bucket past the largest cache (the
same). Where it is now, in the last round (ms, s; `rvc_opt` first):

| | `rvc_opt` | `rvc_lean` |
|---|---|---|
| Quake, timedemo demo1 | 10.28, 10.27 | 10.56, 10.53 |
| a shell loop of 30,000 turns, twice | 114.20, 114.46 | 116.33, 116.30 |
| `seq 1 20000 \| gzip`, twice | 20.91, 20.94 | 21.05, 21.14 |
| nxray, 60 pictures (the round before) | 40.9, 40.7 | 40.8, 40.9 |
| Red Alert's battle (two rounds before) | 20.46, 19.55 | 19.63, 19.61 |
| Doom's timedemo (two rounds before) | 14.79, 14.79 | 14.97, 14.98 |

Red Alert's 300 frames end in the same state (`ea72a5d1`), `fptest` 0 of 15 wrong, `mctest 255
30000` PASS with 0 pages brought, and `nxweb`'s pixel check of `web-long.html` the same as
`rvc_opt`'s.

### Where the rest was (19 October 2026, later)

`RVC_PASS_PROF=1` times every pass of a frame on the card (`rvc_harness`, printed as `[prof]`). On the
shell loop the tick's draw is 90% of a frame on both machines, and `rvc_lean`'s other passes are
cheaper (commit 0.041 ms against 0.051, devices 0.053 against 0.060); its draw was 2.8% dearer for
the same 8,192 instructions (1.431 ms against 1.392). At 2,048 a pass `rvc_lean` was the faster (the
loop 54.65 s against 56.04): a pass costs it less, an instruction more.

`tools/dxbc_run.py` (fxc2's disassembly with its lines numbered, `l(...)` and `d(...)` as `(...)`,
`breakp_` as `breakc_`, and `firstbit_lo` added) ran both ticks' bytecode for core 0's pixel from one
snapshot of the shell loop: 111.7 bytecode instructions an emulated one in the fast loop in both, but
2,000 more reads of the pixel's arrays a pass in this one. A load read the cache's entry whether it had
the texel or not (`idx(at != 0 ? c[at] : dr_v, ...)`: HLSL makes both sides of a `?:`), and read the
bit of `has[]` even for the texel it had last looked for and not found (`t != absent_t && has[...]`:
HLSL makes both sides of an `&&` too). Both as `if`s, nested, and that texel remembered, as `rvc_opt`
has them: the draw 1.431 to 1.412 ms, the loop 38.69 to 38.21 s (`rvc_opt` 38.10). Quake stays 1.5 to
3% behind, its draw more so in the card's slower states. (The two constant buffers for that tool are
best dumped in one run: with the guest's clock from them, a pass of one can take a timer interrupt
and the other not, and their instructions differ.)

Then, with DXC's code and its debug information under Nsight Graphics' GPU Trace on D3D12 (a frozen
tick drawn again and again from a snapshot, both ticks given one guest time), and fxc2's bytecode
timed the same way on D3D11: M by its kind, a product tested first (one result made of eight cost
every `mul` all of them); a CSR write ending the fast loop only when it changes what interrupts see
or starts a copy; a worker's fault record stored word by word as it is worked out, with no array; the
cache's search unrolled, a table at a time; and opcode 0x53 tested before `flw` and `fsw`. Against
`rvc_opt` in the harness: a loop of 10,000 turns 41.32 s against 41.11, gzip 22.52 s against 22.95,
Quake 10.89 ms against 10.75. Red Alert's 300 frames `ea72a5d1`, fptest 0 of 15 wrong, mctest PASS.

## Rounds a frame (`--tick-rounds`, `--tick-split`)

`--tick-rounds N` draws the tick and its commit N times a frame before the devices' passes, the
clock a tick on for each (`_TimePass`) and the host's zap in the first only. `--tick-split F`: the
first F always run; a later one gives core 0 what is left of its F x `--ticks` (texel 39.y, its
instructions in this frame) and draws a worker only when core 0 ended the round before early
(39.z), with
no readback, the tick's geometry shader leaving the workers out. Off by default: four rounds with
one always run made nxray 2.4 times as fast (47.3 to 19.5 ms a picture) and Quake, Quake III and
Red Alert 5 to 15% slower, a round costing its draws whether anything runs in it or not.


All five in one session (19 October 2026), each load once, ms (a game's frame, nxray's picture) and
millions of instructions a second, all cores / core 0 / the workers, over the window measured:

| | `rvc_opt` | `rvc_lean` | `rvc_lean`, 4 rounds, 1 always | compute before | compute now |
|---|---|---|---|---|---|
| Quake, timedemo demo1 | 13.35 (8.3 / 3.2 / 5.0) | 14.20 (7.8 / 3.0 / 4.8) | 15.75 (6.9 / 2.8 / 4.2) | 9.20 (13.4 / 4.9 / 8.5) | 7.33 (16.0 / 6.2 / 9.8) |
| Quake III, timedemo q3dm17 | 14.66 (12.0 / 1.7 / 10.2) | 15.38 (10.9 / 1.6 / 9.3) | 15.28 (11.1 / 1.6 / 9.5) | 9.81 (17.2 / 2.5 / 14.7) | 8.12 (20.7 / 2.9 / 17.8) |
| Red Alert, a battle, frames 500-900 | 24.55 (6.5 / 4.3 / 2.2) | 25.15 (6.4 / 4.2 / 2.2) | 27.62 (5.7 / 3.8 / 1.9) | 13.74 (12.9 / 7.5 / 5.4) | 12.69 (13.6 / 8.1 / 5.6) |
| nxray, 60 pictures | 45.3 (183 / 4.5 / 178) | 47.0 (177 / 4.5 / 172) | 20.2 (150 / 3.9 / 146) | 29.9 (296 / 7.2 / 288) | 11.5 (354 / 8.7 / 345) |

(compute now's nxray from a run of the day before that did not meet the compute machine's start
race.) `rvc_lean` is `rvc_opt`'s speed to 2 to 6% in this
round (in the A/B rounds before it, Quake 12.6 to 13.1 against 12.3, Red Alert's 300 frames 19.9
against 20.5), with two fifths of the tick's bytecode and a sixth of the commit's. The compute
machine is 1.8 to 2 times either on what core 0 does (measured again with every machine the same
day, `docs/machine/speed.md`: `rvc_lean` Quake 13.07 ms, Quake III 14.16, Red Alert 24.39, nxray 46.1,
Doom 18.69 against `rvc_opt`'s 11.71, 13.06, 21.68, 44.2, 16.67; the tick's bytecode 116,344 bytes now,
the commit's 22,596 and the points' 33,052): what it has that a fragment shader cannot
is RAM read and written in place (no write cache, no commit), its registers in group shared
memory, and decoded instructions kept from pass to pass.