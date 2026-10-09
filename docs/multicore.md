# More than one core

An experiment: the machine with worker cores beside it, all run by the one tick pass. It works
(Linux on core 0 starts three workers and gives them work, and every result is right). On the
GPU it was measured on it buys little: a pass costs about as much as the cores that are busy
in it put together, so four cores do 1.3 to 1.4 times the work of one, not four times.

    bin\rvc_harness.exe --d3d11 --image linux-net --cores 4
    / # mctest            (3 workers; "mctest N LIMIT" for other numbers, "mctest N LIMIT bench" for the cost of a pass)

`--cores N` compiles the shader with `CORES=N` (D3D11 backend). Without it nothing changes: the
default build is the one-core machine (the code is under `#if CORES > 1`; its tick time and
the Linux checksums are as before).

## How it is made

- **A block of state a core.** Core 0's state is the 64 x 64 texels it always was. Core k's is
  the same block `CORE_PITCH` (256) texels further right in the state rows, which were empty
  there. `STATE_TEX` adds the block's place (`state_off`); RAM is read with `RAM_TEX`, which
  nothing moves. The tick pass draws a zone a core.
- **The same RAM.** Every core reads RAM as the last commit left it, plus its own writes (its
  write cache). The commit pass looks a RAM texel up in every core's cache, the highest core
  last. So a core sees another's stores one pass later.
- **Sixteen bytes, one writer.** The write cache holds whole texels: a core that stores one word
  carries the texel's other three as it read them, and would undo another core's store to them
  in the same pass. What changes hands is therefore 16 bytes with one writer a pass. There are
  no atomic instructions between cores and no locks; `mc.h` (programs/mc) is the convention.
- **Workers** have no devices and take no traps. A worker is parked until core 0 writes its
  mailbox, the texel at `0x86C00000 + 16 k`: `{ "MCST", pc, sp, a0 }`. It starts there in machine
  mode with paging off and its number in `a1`. `wfi` ends its pass; `ebreak` parks it again, and
  so does any trap, with the cause in its state word, texel (41,0).a. `mhartid` is the core.
- **The arena** is 4 MiB of the GPU device's memory at `0x86C00000`, which `/dev/gpu` maps: a
  Linux program and a worker with paging off see the same bytes there, and no kernel change
  was needed. (Nothing keeps a GPU client from using that range too; this is a prototype.)

At exit the harness prints a line a core: its pc, instructions and whether it runs or what
parked it.

## The test (`programs/mc`)

`worker.c` is what a worker runs, linked at the arena; `mctest.c` is a Linux program that
carries it, starts the workers and checks them (`programs\mc\build.bat`, then
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
- **Pixels that do the same thing are nearly free.** Making every pixel of core 0's block run
  the whole tick (4,096 instead of 973) cost 4 to 8%, and three workers on the same work cost
  two passes, not three. What the GPU cannot do at no cost is run different work.
- **One wide zone over every block is worse**: a lone worker then costs 6.85 ms, twice core 0
  alone, and blocks 256 apart 5.1 ms with only core 0 busy (`RVC_MC_ONE_ZONE=1`).
- The commit pass grows from 0.12 to 0.23 ms with four busy cores.

So in wall time the primes test is 12.3 s on core 0 alone and 9.4 s with three workers, where
the count of passes says 3.2 times fewer. Why different work adds up on this GPU is not known.

## Switches

    --cores N          N cores (1 to 16)
    --core-pitch N     the blocks are N texels apart (256)
    --core-y N         the workers' blocks are N rows down, in rows of RAM nothing uses (an experiment; with RVC_MC_QUADS)
    RVC_MC_QUADS=1     one draw, a quad a core
    RVC_MC_ONE_ZONE=1  one zone over every block
