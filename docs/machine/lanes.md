# Lanes: pixels of the tick, each running a function on an index of its own

The lean tick has them (`experiments/rvc_lean/tick.hlsl`, which the world's `MachineTickLean.shader` runs from a
verbatim copy, `lean/tick.hlsl`; in the harness `--rvc experiments\rvc_lean`). `rvc_cpu`, `rvc_harness --cpu`,
`experiments/rvc_opt` and the compute machine have none: the library makes every index a plain call there, which
gives the same output.

A worker is a block of the tick's pixels, and every one of them runs the whole pass to write back
its own eight texels: one instruction stream a block. A lane is a pixel that runs a stream of its
own instead: the same function as the others, on another index. There are two kinds:

- **A worker's lanes**: the pixels of a worker whose eight texels are two whole buckets of its
  write cache, which the commit writes as it writes any core's. 6 for a worker of 2^3, 13 of 2^4,
  27 of 2^5, 55 of 2^6. A worker asks for them and its own code waits while they run.
- **The field**: up to 3,968 pixels of no core's, which the host draws only while a job is
  posted. Any program's first core posts a job and goes on; the field's size does not depend on
  how the workers are laid out.

## A job

A job is a function `fn(index, ctx)`, the first index and how many, and an output of `T` whole
texels an index (1 to 6) at `out + 16 T (index - first)`. Lane `L` of `n` makes indices
`first + L`, `first + L + n`, ... in that order: neighbouring lanes take neighbouring indices,
whose paths are alike.

An index may compute, read any memory as the job found it, and store to its own stack and to its
output, nothing else: no system call, no device, no atomics. A store elsewhere is lost. Indices run
in no set order and see none of each other's stores.

The job's descriptor is three texels of the asking core's memory: `fn, first, count, out`;
`T, the stack's top, ctx, the stub`; and `the stuck texels' array, gp, tp, K` (gp and tp the field's).
The stub is an `ebreak` in the library (`mcw_lane_stub`): a lane's call returns there, which ends the
index.

| CSR | |
|---|---|
| `0x0b8` write | a worker: start a job, the value its descriptor's address. Its pass ends; its passes are its lanes' until the job is through, and it goes on after the write |
| `0x0b8` read | bit 31 where the tick has lanes, this core's lanes in bits 8-15 (0 on the first core), how its last job ended in bits 0-7 (1 whole, 2 a lane stuck, 4 an index not through: "Steps") |
| `0x0b9` read | the field's lanes where the host draws it (3,968), else 0 |
| `0x0ba` read | in a lane, the instructions its pass has left; elsewhere `0xffffffff` (no end) |

## A lane's pass

A lane starts every index with `a0` the index, `a1` the context, `sp` the stack's top, `ra` the stub
and `pc` the function, and runs the tick's own loop (`fast()`, `step()`) with the pass's budget
(8,192 instructions), index after index while it has room: six texels for what it makes, and
instructions for another by those it made so far. Its cache is the pixel's whole local array
(the hash of 2^6, whatever the worker's size), its stack in it; nothing of the stack is stored.

An index ends well at the stub. A trap, a system call, `fence.i`, `pause`, `wfi`, a full cache or
the budget gone end it badly: the lane keeps what it made before it, and tries the index again in
the next pass. An index that fails as the first of a whole pass would fail again: the lane
**skips** it, sets its bit in its stuck texels, and goes on with its order. A lane has K + 1 of
them in an array the library gives (K is 1 for a worker's lanes, 3 for the field's): K of a bit for
each of the first 96 K of its order, and one that says where it stopped, for an index after those,
which ends its order there. Each texel's first word is the job's (`0x80000000 |` the job's number),
so one left from another job reads as empty and nothing is cleared between jobs. The lane's place in
its order keeps bit 30 once it has skipped one, bit 31 when it stopped. The library makes what the
lanes left as plain calls on the core that asked, or (the field) gives it to the program's workers.

What it made goes into its two buckets: the output texels of the indices that ended, as its cache
has them, each with its tag. The last word of each bucket's tags is the lane's: where it is in its
order, and its instructions in the pass. Every pixel of the worker reads those of all its lanes
at the start of the next pass: the worker's clock goes on by their instructions (so the harness and
the kernel count them), and when every lane is through or stuck the job is over and the worker's
own code runs on in that same pass.

A worker in lanes passes keeps its registers, its stall (`STALL_LANES` 12, `STALL_LANES_START` 13)
and the descriptor's address in texel 28's last word; the commit clears neither. Its other pixels
run nothing.

## The field

The field's pixels are rows 8-15 of the tick's targets' two bands (no core's block is there),
248 across: 3,968 lanes. Their texels are rows 48 to 63 of the state (no core's tiles reach them),
eight a lane. The tick's primitive after the 256 cores' draws it, as many rows as the posted job
wants, and the commit has a point for each of its 7,936 buckets, before the picture's; its state
pass keeps of a lane's texels only the last words of its two tags (the commit's other rows are as
they were). All under `LANE_FIELD`, which `experiments/rvc_lean/main.shader` defines; the harness
draws the field when the folder's `main.shader` has it (`BackendOptions::laneField`), and sets
`_LaneField` and `_FieldPoints`.

The job is posted in the mailboxes' page: the texel at `0x80C01900` is the descriptor's address,
the job's number, how many lanes. Each lane keeps the job's number's last byte in its state: a lane
that sees another number starts the new job's order from its beginning. The first pixel of every
64 is its group's **leader**: it runs no index, and each pass writes how its 63 lanes left the pass
before into the group's texel (`0x80C01A00 + 16 g`, 62 of them): the number of the job; bit 0 busy,
and bits 1 to 31 which of lanes 0 to 30 skipped or stopped; its lanes' instructions for the job in
sixteens; and which of lanes 31 to 62 did. The first core asks the 62 texels whether the job is
through, and looks at the stuck texels of those lanes alone: a scan of every lane's at every job
would take most of its time. The harness reads them back each frame with the job's texel, counts their
instructions with the workers', and draws the field while it has seen a job posted in the last eight
frames (the readback is frames behind; with no job posted the primitive draws no pixel).

One job is out at a time, of one program at a time: a program whose job is out has its texel, and
another waits for it to clear (`mcw_field_begin`). The library posts every job on all 3,968 lanes,
so that every lane's number is the last job's.

## The library

`mcw.h` (`programs/mc/mcw.c`):

- `mcw_lanes(fn, ctx, first, count, out, texels)`: on a worker, the job on its lanes; returns how
  many it had (0: plain calls, on the first core, on a machine without lanes, with texels out of
  range or an output not on 16 bytes). `mcw_lanes_here()`: how many lanes this core has.
- `mcw_field_lanes()`, `mcw_field_begin(...)` (the same arguments), `mcw_field_ready()`,
  `mcw_field_wait()`: the field's job from the first core, not waited for. Where there is no field,
  `mcw_field_begin` makes the calls before it returns. `mcw_field_stuck` counts the indices the
  field's lanes left to the first core.
- `mcw_field_apart(1)`: what the lanes skipped is not made by the first core in `mcw_field_ready()`
  but kept, the groups' words copied: `mcw_field_skips()` says what the last job left (0: nothing), and
  each of `parts` workers calls `mcw_field_skips_make(skips, part, parts)`, which deals the lanes that
  left some out in turn (they lie together, where the work is long). The stuck texels are two arrays,
  one for every other job, so a job's skips are kept until the job after the next is begun: the
  workers make one job's while the field runs the next.

`mcw_for`'s wait (`round_through` in `mcwx.c`) asks one worker a pass, the first not through, and every
one at every eighth: asking all 255 every pass would be most of the first core's time in a round.

## Shaders

`mcwx.h` (`programs/mc/mcwx.c`): `mcw_shade(pixels, width, height, fn, ctx)` and
`mcw_shade_begin` / `mcw_shade_wait`: `fn(x, y, ctx)` is a pixel's colour, called for every pixel of
a picture of words (16-byte aligned, the width a multiple of four), four pixels side by side an
index: one job of the field where there is one, else a piece a worker (`mcw_for`, the first core
taking none), each on its lanes. The example `linux/tcc/examples/shader.c` is a shader written so,
compiled in the machine by `tcc` and linked with its `libmcw.a`. HLSL compiled in the machine runs on
the lanes in steps (`docs/system/hlsl.md`).

## Steps

An index that may need more than a pass is written as **steps**: the function
does what fits, stores in its output what it needs to go on, and says it is not through; the job is
posted again, a **round**, until every index is. Nothing of a lane outlives its pass, so the
program decides what its state is: no registers are saved for it.

- `mcw_step_fn(index, ctx, round)` returns 0 when its index is through. Round 0 is its first call;
  in a later one it reads its output back, and one through in an earlier round returns at once.
- `mcw_steps_left()` is CSR `0x0ba`: how many of the pass's 8,192 instructions the lane has left.
  A step looks at it where it can stop, and stops while what is left still covers the longest
  stretch to its next such place. Off the lanes it is `0xffffffff` and a step never stops.
- `mcw_field_steps(fn, ctx, first, count, out, T)` posts the field's job in steps; `mcw_field_ready()`
  posts the next round itself until no index is left. `mcw_lanes_steps(..., &report)` is the same on
  a worker's lanes, returned when through. A plain call (the first core, `rvc_cpu`) calls the step
  until it is through. `mcw_field_steps_report()` says how many rounds a job took and how many of
  its steps no lane could make (`too_long`), which a core made: not 0, the steps are too long for
  the lanes, and the program should say so.
- A round waits for its slowest lane, so a job's last few indices cost a round each.
  `mcw_field_steps_rounds(n)` ends the field's jobs after n rounds (0: when all are through; it may be
  changed while a job is out, and counts at the next round's end); the report's `left` then says some
  indices are not through, and the program calls the step itself on those (any round but 0), on its
  workers for instance, while the field makes the next job.
- An index takes as many of a lane's six output texels as it **wrote** (the tick counts the texels
  of its output in the lane's cache): one through that writes nothing takes none, so a lane goes on
  past the done indices of later rounds. A step that writes six ends its lane's pass, and one that
  does not fit in what the lane has left of the six is lost and made again as the next pass's first.
- `mcw_steps_probe(fn, ctx, first, count, budget)` is the self-test: on a worker (the first core's
  counts take interrupts in), every index made in steps, each step given the budget as a lane is,
  timed by the instruction clock: steps, the most rounds, a step's mean and longest, and how many
  went over. Give it the worst cases the program has.

In the tick: a step's job is marked by bit 0 of its stuck texels' address; a step that returns not
through sets bit 29 of its lane's place (a lane's order goes on, so the bit is only "some index of
mine is not through"); the group's leader gathers it into bit 31 of its third word, and a worker's
lanes into bit 2 of the job's end. The lane's clock starts at 0 for `0x0ba`. `steptest` (index i is
up to a few hundred thousand instructions of hashing, one in 997 with a first stretch longer than a
pass) checks the field, the workers' lanes and the self-test: every index right, and the long ones
made by a core and said so.

## Rules for an index

- Its stores: its stack and its output; no two lanes into one texel (the output's place is the
  index's, so that holds by itself).
- One pass: an index must end within 8,192 instructions (all of a lane's indices of a pass share
  them): about 2,000 instructions a pixel is what four pixels of a texel can have. Code that `tcc`
  compiles is much longer than GCC's. An index that takes longer costs its lane a pass and is made by
  the core that asked, or by the workers it gives them to: work that can be longer is written in steps
  ("Steps"; `nxpath`'s long paths are, `docs/apps/raytrace.md`).
- HLSL makes both sides of an `||` and an `&&`: in the tick, the lanes' loop asks whether the pixel
  is a lane before it calls `lane_next`, in two tests. With one, every lane through with its order
  would call it and take itself for stuck.
- A value a lane keeps from before its run to after it belongs in a word that is carried already
  (the skipped flag is bit 30 of `lane_j`, K is read again from the descriptor where a lane is
  stuck), not in a new static: a static the tick sets before a pass's loop and reads after it is
  carried through every core's loop, and costs every program whether or not a lane runs.

## Checks

`nxray`'s sums are the same with lanes, with the field, with `RAY_LANES=0` and with no worker, and `nxpath`'s
are the same on the field as on its workers (`NXPATH_FIELD=0`). `tools/test_all.py` has
`lanetest 255 50 1 40; lanetest 31 40 3 20` and `steptest 31 3000`, which on `rvc_cpu` compare plain calls with
themselves and check the library's paths. What only the card can show is run under
`--rvc experiments\rvc_lean`: `lanetest` with 1, 3 and 6 texels an index, with 3 workers and with 255, on the
workers' lanes and on the field, and `steptest`. `rvc_cpu` has no lanes: a program's output is checked there, its
speed only on the card.

How many lanes the workers have together goes by geotune's layout ("Packing the small workers" in
[multicore.md](multicore.md)): its dense one, 16 workers of 2^5 and 239 of 2^3, has 1,866, and every worker
at 2^5 (`--workers 8`) 6,885. Lanes gain from large caches, the field from neither.

## What is not done

- The field in the world is wired and has not been run there: `MachineTickLean.shader` defines `LANE_FIELD`, the
  tick's mesh has the field's primitive (`ShaderEmuBuilder`: 17 triangles), `Machine.shader`'s Unpack fills rows 48 to
  63 from the targets while a job is posted, `MachineCommit.cginc` defines `LANE_FIELD` for `src/ram.cginc` (synced from
  `experiments/rvc_opt/src/ram.h`), and the commit's points and `EmuMachine` have the field's 7,936 points
  (`_FieldPoints`). Unlike the harness, the world draws the primitive and the points every round, a job posted or
  not (with none posted they draw nothing), and does not count the field's instructions in its figures. To run it:
  "Sync shader sources from the repository" and the builder, then `example shader` in the world.
- A queue of the field's jobs: a job's end is seen two passes after its last lane, and the next
  one waits for that.
- An index longer than a pass that is not written in steps: a lane's registers and stack do not
  outlive its pass. It is skipped and made elsewhere, which is right but costs.
- A round is the whole job posted again: a lane walks past the done indices of its order to find the
  ones not through. A job with few indices left could be posted as those alone.
