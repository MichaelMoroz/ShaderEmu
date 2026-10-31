# rvc_opt

The machine's shader: a patched copy of the one from [pimaker/rvc](https://github.com/pimaker/rvc) (`_Nix/rvc`, MIT
License, Copyright (c) 2021 PiMaker). Its state layout, commit and fast path are its own now
(`docs/machine/multicore.md`, `docs/machine/gpu.md`), and it runs Linux without the MMU (`docs/system/nommu.md`). What the tick
costs and how to measure it is `docs/machine/tick-costs.md`; here is how it is built and why.

    bin\rvc_harness.exe --image linux-net

`tools\gpu_queue.ps1` runs it (one at a time for every session). `python tools/test_all.py lazy mctest-3` and the rest check the
machine on `rvc_cpu`; `tools/test_card.py` on the card.

## Build switches

The CPU's own; the machine around it has more (`CORES`, `MC_UNIFORM`, `TICK_MRT` and the commit's in
`docs/machine/multicore.md`; `SBI_HLE` and `LAZY_MEMOP` in `docs/machine/boot.md`). `--define NAME` gives one to the harness.

| Define | Effect |
|---|---|
| `NO_PAGING` | no MMU: `satp` hardwired to 0, every address physical, no TLBs. The harness's for our Linux and the bare-metal images (`--machine nopaging`) |
| `CSR_FIELDS` | follows from `NO_PAGING` with `SBI_HLE` (`types.h`): CSRs as fields of core 0's first texels, the console as memory, no UART, no CLINT |
| `M_MODE_ONLY` | machine mode only, includes `NO_PAGING`: for our own bare-metal programs (`--machine mmode`) |
| `NO_DOUBLES` | timer value from the host instead of double math (D3D12's bytecode path miscomputes doubles) |
| `FPU` | single-precision float instructions, the F extension (`docs/machine/fpu.md`): the harness sets it for Linux |
| `PROFILE` | `PROF()` event counters through a UAV (`--profile`) |
| `L1_TABLE_BITS`, `L1_WAYS`, `RAM_TILE_BITS` | the write cache's size, RAM's layout in the texture |
| `L1_LOCAL`, `L1_STATIC` | the write cache as a local of the pass: on with fxc2, off with FXC |

The tick's paged paths (`mmu.h`, `TLB2_N`, `--machine full`) are still in the source and are not built or tested for the
machine that runs: Linux with the MMU is not supported.

## How the tick is shaped

The tick is the `CPUTick` pass of `main.shader`. Its rules are in `docs/machine/tick-costs.md`, "The shape the code must keep".

- **Two paths.** The fast path (`fast_run_l1` in `cpu.h`, `fast_exec_l1` in `emu.h`) runs instructions that cannot trap or
  change what interrupts see, back to back in a loop of its own: integer arithmetic, branches, jumps, RAM loads and stores,
  common floats. Everything else takes the general path once. With both in one loop body every value the general path can
  change would be merged where they rejoin, on every instruction.
- **The interrupt gate** (`irq_quiet`): while no trap was taken and nothing an interrupt sees was written, a run needs no
  interrupt step. What is the same for a whole run is decided once, and the clock is advanced at its end.
- **Registers** in an indexed array, read before the opcode is known: rs1 and rs2 sit at the same bits in every format.
- **The instruction window**: the texel fetched and the one after it, read ahead, the words turned so that the next
  instruction's is the first. Fetches read the texture, never the write cache: a texel stays good for the pass.
- **Aligned word stores** in one step, into a cache of whole RAM texels (below).
- **The commit's skip**: a mask of the stored addresses (`mem_cache_bloom`) and the bands written (`multicore.md`).
- **The big arrays are locals of the tick's `frag`**, handed down as `inout` (`L1_LOCAL`, `l1_local.h`), so that they are not zeroed
  at every pass. This needs a compiler that works on an `inout` array in place: fxc2 (it defines `__FXC2__`) and DXC do. FXC
  copies the array at every call and fails ("can't unroll loops marked with loop attribute"), so under FXC it stays static.
- **Floats out of the integers' way**: one branch of the tree with its own operand reads, load, store and register write. The
  common ones each by itself (`fp_light`), the rest in `fp_exec`.
- **MULH by 16-bit partial products**, not doubles and not fxc2's `mulhi()`: the layer under Direct3D on Linux makes an invalid
  SPIR-V module of that instruction (`docs/world/linux.md`). So a very large product is exact where upstream's rounded.

## The write cache

Stores of a pass wait in a cache in the state texture and the commit writes them. It holds RAM texels, four words to an
entry: stores come in runs (a register save, a structure, a copy), and a run takes one entry. A bucket is a texel of tags and
the texels they name. There are three tables with a hash each (`L1_H1`, `L1_H2`, `types.h`): core 0's are 64 + 32 + 16 buckets of
three, 336 texels in 448 of state; a worker's follow its `geotune` layout (`docs/machine/multicore.md`). A texel goes to the next
table only when its bucket is full, so most lookups read one bucket. An entry is made from the texel as RAM has it: a store that
changes nothing costs no entry, a partial store needs no second read, and the commit copies whole texels.

Why this size:

- A full cache ends the pass. Stalls come from collisions, well before the cache is full: another hash or one that changes with a
  seed does little, another bucket to try does a lot. A fourth table is slower on every instruction than it gains.
- Four entries a bucket cost every instruction more than the stalls they save.
- Every state texel is a pixel that runs the tick. Core 0's state is 500 texels (8 rows).
- The array's declared size costs nothing, nor does one more array beside it.

`RVC_WCACHE=1` on `rvc_cpu` models it (its summary says how many passes ended full; `RVC_WC_LOG`). `tools/l1_sim.py` replays
logged stores through other layouts, `tools/l1_study.py` a run's writes (`--l1-log` on the harness). Cold boot on `rvc_cpu`,
31 October 2026: 28 of 933 passes ended full.

## RAM's layout

`RAM_TILE_BITS=b` places RAM in square tiles of 2^b x 2^b texels instead of rows of 2,048. Only the address-to-texel mapping
changes. No layout won on every load, so texture locality is not a limit here and the default stays rows.

## The same loop, compiled slower

The tick's speed is not only what the fast loop does. Changes that leave the loop's bytecode as it is, instruction for instruction
(`python tools\cso_loops.py` on the blobs in `build\shadercache` says so; the tick's is the one with eight outputs), move every
instruction by 1 to 3%, and nearly always down: the card's driver compiles the same loop differently inside a different function.
The difference is the same at any pass length, not in the commit and not a warm-up. Such a change may be anywhere: a state word
written in a rare branch of the general path, one more call in `sbi_call`, an array made of a switch. Code after the loop in the
source and one more local before it are the safest places.

So a variant has to be measured whole, in alternating runs against the tree and on more than one workload (the shell loop and Doom's
timedemo disagree), whatever the loop's bytecode says. A change to the general path that measures 1% slower may be this and not its
own work. With another program on the card even a byte-identical blob measures under. This is why `LAZY_MEMOP` is off by default
(`docs/machine/boot.md`).

## Profiling counters

`PROF(id)` marks an event; ids and names are in `src/prof.h`. A normal build compiles them away. With `rvc_harness --profile` each
pixel tallies events in a local array and one pixel adds its tally to a UAV buffer at the end of the pass, so that the core's redundant
pixels do not inflate the totals (`tick` equals the instruction count). Do not write the UAV at the event site: FXC then refuses to
compile the `[loop]` loops (X3531).

Where a program's own instructions go is `rvc_cpu`'s: `tools/prof.py` (`docs/machine/profiling.md`).

## GPU hardware counters

The skill `nsight-gpu-trace` takes a trace of a shader on the card. Nsight does not attach to the D3D11 harness: `bin\rvc_trace12.exe` runs
the same bytecode on D3D12 (`pwsh tools\gpu_trace.ps1`). `NO_DOUBLES` (`--no-doubles`) is needed there, NVIDIA's D3D12 path miscomputing double
math in this shader.

## Fast iteration with DXC

`rvc_trace12 --dxc` and `rvc_harness_dxc --dxc` compile the same source with DXC to DXIL in about a second, where fxc2 and above all FXC take
longer. It is reliable for checking that an edit keeps emulation as it is. The two compilers can disagree on how much a change helps, and
the world runs fxc2's bytecode: rank variants with it if you like, then decide with fxc2 on D3D11.

## Lanes exchanging words

`ddx_fine` and `ddy_fine` pass a 32-bit word between a quad's lanes exactly, as two 16-bit halves in floats, and FXC takes them in a `[loop]`
with warning X3595. They are slower than what they could buy for a core, and every lane of the quad must be running the core:
state texels must be quad-aligned. Lanes for programs are `docs/machine/lanes.md`.
