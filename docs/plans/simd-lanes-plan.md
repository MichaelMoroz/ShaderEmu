# Plan: lanes, a worker's pixels each on an index of their own

Built 20 October 2026, in the lean tick: `docs/machine/lanes.md` is what there is. Where it
differs from this plan: one shape of output (`T` whole texels an index at its place, made by
ordinary stores; the texel and word shapes are one texel of four words), a descriptor in memory
in place of CSRs `0x0b4` to `0x0bd` (CSR `0x0b8` alone, and `0x0b9`), a lane's output taken from its
cache by the indices that ended (no walk of it), a lane stuck only at an index that fails as the
first of a whole pass, the job over when every pixel of the worker sees all its lanes through,
the lanes' instructions in the worker's clock, `rvc_cpu` without lanes (the library's plain calls
there), and, beside the workers' lanes, a field of 3,968 lanes of no core's that any program's
first core gives a job to ("The field"), which is what `nxray` and the shader example use.

This was the plan for a mode of the worker cores in which every pixel of a worker runs the same
function on its own index, in the tick that exists, and puts what it made into RAM through the
commit that exists.

## Why

A worker is a block of the tick's pixels: at size 5, 35 pixels hold its 276 texels. Every one of
them runs the whole pass, the same instructions on the
same registers, only to write back its own eight texels of the worker's state
(`shader_vm/tick.hlsl`, the comment at the top and `frag`). So a warp of the card does
one instruction stream of work, and the other lanes of it repeat that stream.

If each of those pixels ran the same code on its own data, the same pass would do one stream a
pixel. What that costs where the pixels' paths differ was measured by `tools/warpbench`: an
interpreter loop with a seed a pixel, every pixel on its own path, costs 1.6 times the same loop
on one seed (`docs/machine/multicore.md`, "Why: how many pixels the GPU runs at once").

A pass of its own for such work, beside the tick, was considered and is not the plan: there is no
asynchronous compute in D3D11 or in Unity, so it would run after the tick, not beside the
workers, and it would need a camera of its own in the world. Inside the tick it costs no draw, no
pass and no change to the commit.

## What the tick already has

- **A private memory a pixel.** The write cache is a local array of every pixel, `c[C_N]`, 448
  texels (7 KB): `C_N` is the largest cache's, whatever the worker's size. A store goes there and
  a load looks there first. In lanes, that array is the lane's stack, and nobody else sees it.
- **A way out to RAM.** The commit takes a worker's stores bucket by bucket: a texel of three
  tags (a RAM texel's number and one), then those three texels, written where the tags say
  (`ram_commit_geom` in `shader_vm/src/ram.h`). It does not care where in the tables a
  bucket was, and it asks only that texel 41's first word (`bloom`) is not 0.
- **Two whole buckets a pixel.** The cache begins at texel 44 (`L1_AT`) and a bucket is four
  texels, so a pixel whose eight texels lie within the cache holds exactly two buckets: the pixel
  of texels `w0` to `w0 + 7`, for `w0` from 48 to the cache's end less 8.

| Worker's cache (bits) | Buckets | Cache ends at texel | Lanes (pixels of two buckets) |
|---|---|---|---|
| 3 | 14 | 100 | 6 |
| 4 | 28 | 156 | 13 |
| 5 (the kernel's geometry) | 56 | 268 | 27 |
| 6 (`geotune`'s large ones) | 112 | 492 | 55 |

Lane `L` is the pixel with `w0 = 48 + 8L`; its buckets are `2L + 1` and `2L + 2`. The pixels of
texels 0 to 47 (the CPU's registers, the CSRs, texel 41) and from the cache's end on (the float
registers) are the worker's **control pixels**.

## The mode

### Asked for

A worker sets CSRs and starts a job, as the kernel's copy does with `0x0b0` to `0x0b3`. Free in
both `rvc_cpu` (`csr_read`/`csr_write` in `harness/apps/rvc_cpu.cpp`) and the lean tick
(`csr_read`/`csr_write`): `0x0b4` to `0x0bd` (`0x0be` is the player, `0x0bf` the random word).

| CSR | Meaning |
|---|---|
| `0x0b4` | the function's address |
| `0x0b5` | the first index |
| `0x0b6` | how many indices |
| `0x0b7` | the output's first texel; `0x0b9` its last |
| `0x0ba` | the stack's top for every lane (the stack is the 4 KB under it) |
| `0x0bb` | the shape: 0 a struct, 1 a texel, 2 a word ("Shapes", below) |
| `0x0b8` | write: start (`stall = STALL_SIMD`, the pass ends); read: 1 where lanes are built |

The job's words are kept in words of the worker's state that are zeros now (texel 38's `.x`,
`.y` and `.w`, texel 2's `.z` and `.w`, and more as needed): to be checked against the commit
(which changes 8, 28 and 41) and the control pass before they are taken.

A tick without lanes (D3D12's one core, an older world) ignores a write to a CSR it
does not keep and reads it as 0: `0x0b8` reads 0, nothing stalls, and the library runs every index
as a plain call (below).

### A pass of lanes

`frag`, after `load_state`, sees `stall == STALL_SIMD`:

- **A lane pixel** sets `a0` to the first index plus its lane number, `sp` to the stack's top, `ra`
  to the library's stub (one `ebreak`), `pc` to the function, `bits` to 6 for its own hash (the
  whole local array), and runs the loop it always runs, with the same budget (`most`).
- **A control pixel** runs nothing. It writes the worker's state with the job moved on by the
  number of lanes; when no index is left it clears the stall, and the worker goes on after its
  write of `0x0b8` in the next pass. It sets `bloom` to all ones and `dirty` to the bands of the
  output's range (bit b for bands b, b + 32 and b + 64), which is all the commit asks of it.

A job of more indices than lanes is a pass of lanes after another, the worker's own code not
running between them.

### A lane's end

| What happens | Means |
|---|---|
| `ebreak` (the stub `ra` points at) | done: `step()` parks a worker there today; in lanes it ends the lane |
| a trap (`worker_fault`), `ecall`, `fence.i`, `pause` | failed |
| a full cache (`STALL_MEM`) | failed |
| the budget gone | failed |

Not a misaligned `ra` for the end: a `jalr` the general step takes while quiet leaves `pc`
misaligned and the fast loop then runs whatever is at the texel, with no trap.

### What a lane puts out

A lane that ended well walks its local array: entries inside the stack's 4 KB are dropped, the
rest are its output. At most six texels, all of them within the output's range, or the lane
failed. The six go into the lane's two buckets as tags and texels, and the commit writes them.
A failed lane puts out two empty buckets: nothing.

So a lane's output is **96 bytes, six whole texels, each where the function stored it**. The
function stores its results as any C function does.

### Shapes

That is the struct shape. The job's shape (`0x0bb`) says what one index is:

| Shape | An index is | A lane does | Output |
|---|---|---|---|
| struct | a call that stores anywhere in the output's range | 1 index | up to 6 texels, walked out of its cache (above) |
| texel | a call that returns 16 bytes in `a0` to `a3` | 6 indices (8 with step 6's point) | `out + 16 x index`, fixed |
| word | a call that returns 32 bits in `a0` | 24 indices (32 with step 6's point) | `out + 4 x index`, fixed |

In the texel and word shapes the function is pure: it returns its value and stores nowhere but
its stack. The lane calls it once an index, `ra` the stub each time, and puts each result into its
buckets' texels where the index says, with their tags: no walk of the cache, no test of the range.
Its stack is the same for every call of the lane and is not cleared between them.

A lane out of budget after k of its indices keeps the first k. A word has no bit left to say it is
there, so a job of these shapes either gives one texel of the lane's six to a status word (how
many of its indices it made: 20 words a lane then, or 5 texels) or is sized so that every index
fits (the program knows its cost a call). The library's fallback reads the status where there is
one, and otherwise calls the indices of every lane that did not end well.

## Rules for a program

- A lane stores to its stack and to its output and nowhere else: no other memory, no device, no
  atomics. In the texel and word shapes, to its stack only.
- No two lanes of a job store into one texel (16 bytes): the cache reads, changes and writes whole
  texels, and the commit writes whole texels. It is the rule that two cores do not store to one
  texel in a pass, for lanes.
- A lane's indices are done in one pass: up to 8,192 instructions where the pass has them, fewer
  where the round is shorter (for the word shape, all 24 calls together). A lane's registers and
  stack cannot outlive the pass.
- The function's result says it is there (a word the function sets). The library clears those
  words before the job, and afterwards calls the function itself for every index that has none:
  the same code, on the worker, as an ordinary call. Where lanes are not built every index takes
  that way.

The library is `mcw_lanes(fn, shape, first, count, out, out_end)` in `programs/mc/mcw.c` and `mcwx.h`
("The second layer": programs use that layer, not code of their own), with the stub.

## What it may gain

At size 5 a worker's pass does 27 indices where it did one stream; with the paths of the lanes
apart, 1.6 times slower by warpbench, about 15 to 20 times the worker. Large workers (55 lanes)
more. Not measured yet and able to take some of that away: each lane reads RAM at addresses of its
own where all of a worker's pixels read the same texel now (`dr_v`, the instructions' texels are
still shared while the lanes are on one `pc`).

For the whole machine, the figures to beat (worked out, not measured; RTX 5090, the harness):

| | Lanes | Instructions a second |
|---|---|---|
| today: 255 workers, the same loop on every core (`mctest ... bench 4`, 1.9 ms a pass) | | 1.1 billion |
| today: 255 workers, each its own float work (`nxpath`) | | 0.62 billion |
| lanes, 255 workers of size 5, paths apart (1.6 times: a pass of about 3 ms) | 6,885 | about 19 billion |
| lanes, every worker of size 6 (186 fit the strip's 1,488 tiles) | about 10,200 | about 25 billion: 11,500 pixels drawn against 8,900, past what the card runs for nothing, so a longer pass |
| the same, lanes nearly in step (1.0 to 1.2 times) | about 10,200 | about 35 billion |

The 1.6 is warpbench's every pixel on a path of its own. Simple drawing of old games (spans,
tiles, a palette) keeps lanes nearer one `pc`, which is how it could come above 30. Against it:
each lane reads its own RAM, not measured yet. The RTX 5070 laptop and the world (its rounds a
frame, the room's draws) are lower, in proportion to their passes a second. Step 1 puts measured
step counts and divergence in place of the guesses.

Uses at these rates, all low resolution with simple code a pixel: Doom's view, 320 x 200, is
64,000 pixels, 2,667 lanes of the word shape (under half of a pass's lanes, about 340 instructions
a pixel), where four workers make it now; the NES's picture, 256 x 240, a PPU a pixel; Tiberian
Dawn's and Red Alert's scenes, rectangles through a palette. Not the holodeck: millions of pixels
at 90 a second with real shaders are the card's rasteriser's and the GPU device's.

The ray tracers fit an index a pass: from the 256-core figures `nxray` is about 900 instructions
a pixel and `nxpath` about 1,800 a sample (`multicore.md`, "Up to 256 cores"). An index of
`nxray` is a texel of four pixels.

## What it must not cost

Nothing changes in `fast()` or in `step()`'s common path: the lanes' start in `frag`, one test at
`ebreak` and in `worker_fault`, and the walk in the output. The tree's experience is still that
nearly every change to the tick's function costs 1 to 3% of every instruction, however far from the
fast loop (AGENTS.md). The change is measured whole, alternating with the tree, on a shell loop,
Doom's timedemo and Quake e1m1 from one snapshot, and kept only within what two runs differ by.

## Steps

1. **The question, on `rvc_cpu`, before any code of the mode.** Run `nxray`'s and `nxpath`'s
   functions a pixel as 27 lanes in step: instructions an index, stack texels an index, output
   texels an index, and how many steps the lanes are on more than one `pc`. A run of minutes at
   most. If an index needs more than a pass or more than the local array, the cut of an index is
   changed before anything else.
2. **`rvc_cpu`.** The CSRs, a job's lanes run one after another with each lane's own stack and
   the rules checked (a store outside, a texel two lanes store, the budget), counted under
   `RVC_RACES`. A test program (`programs/lanes`) with a sum, and its case in `tools/test_all.py`.
3. **`nxray` on lanes**, with a switch for the old way (`RAY_LANES=0`): the sums of
   `docs/apps/raytrace.md` (`RAY_FRAMES=4` on `rvc_cpu`) the same both ways.
4. **The lean tick** (`shader_vm/tick.hlsl`; Unity's `MachineTickLean.shader` is a
   verbatim copy). `rvc_harness --cpu --warp` through `tools\gpu_queue.ps1`: the same sums as
   `rvc_cpu`. Then on the card: the three loads against the tree (above), and `nxray`'s pictures a
   second with and without lanes, with a page of the frames.
5. **`nxpath`**: `NXPATH_CHECK=500` 0, its sum `02f213e9` with lanes and with `NXPATH_CORES=0`.
   Then the texel and word shapes, each with a case of `programs/lanes` on `rvc_cpu` and the card.
6. **Later, if 96 bytes are too few:** a point a lane in `ram_commit_geom` that writes the lane's
   eight texels whole to `out + 128 x lane`. It gives the fixed shapes 8 texels or 32 words a
   lane where the buckets give 6 and 24, and a JPEG block of RGB565 (128 bytes), which libmedia's
   decode would want, fits an index.

## A use later: a rasterizer of lanes

In the word shape an index is a pixel of a picture, and a lane owns 24 of them, a tile of 6 x 4
or 8 x 3. A job before it, of the struct shape, sorts a draw's triangles into the tiles they
touch. The lane then goes through its tile's triangles in their order, a pixel at a time: the
edges, the depth, the shader as a call, and a blend with the colour that was there.

What it gives that the GPU device does not: the device's depth test is fixed, it has no blending
and a fragment is one of a few modes (`docs/machine/gpu.md`). Here every pixel has one owner, so
the order of the draw holds exactly, any blend and any depth test can be had, and a shader can be
any code, C included, compiled in the machine by `tcc` (`docs/system/cc.md`; its code is slower
than GCC's).

What it costs, at 255 workers of size 5: about 6,900 lanes a pass of 8,192 instructions each. 320
x 240 is 3,200 tiles, a pass for the whole picture at up to about 340 instructions a pixel (every
triangle that touches it and the shader). 640 x 480 is two passes. A draw that is not done in one
pass must keep its depth from one pass to the next: the output is then a colour and a depth, 12
pixels a lane.

Not this, for OpenGL programs: a short vec4 bytecode run by the GPU device's own fragment shader,
the card rasterising, would run every pixel the same program with nothing apart, an operation for
four numbers where RISC-V takes several, and no memory to emulate: likely 10 to 100 times faster
a pixel than lanes. The lanes are for drawing the card does not do: a software renderer's spans
or columns (Quake, Doom), filters over a picture, a film's decode, a grid's physics, and drawing
that depends on its order or reads the picture it draws into.

## Not settled

- The instruction count: the worker's `clock` is written by a control pixel, which does not see
  the lanes' counts. `rvc_cpu` counts exactly; on the card the count of a pass of lanes is a guess
  until a lane's count has a way out (a word of its buckets, or the commit point of step 6).
- A worker the kernel asks back (`mcw_recalled`) in the middle of a job: the control pixels see
  the mailbox's stop between passes and end the job; the indices left are the library's to call.
- Whether the lanes' memory traffic slows the passes of the other workers beside them.
