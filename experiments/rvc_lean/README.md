# The machine's processor, a shader a draw

The only tick: the harness's and the world's. A processor for Linux without an MMU only: the tick one file with no
includes, on RAM in one texture written in place.

- `tick.hlsl`: the tick, every core of the machine. Its vertex, geometry and fragment shaders, and
  with `TICK_UNPACK` the pass that copies its eight targets into the state.
- `main.shader`: the ShaderLab wrapper: its tick pass this folder's `tick.hlsl`
  (`HLSLPROGRAM`, not `CGPROGRAM`: the harness, as Unity does, puts `HLSLSupport.cginc` and
  `UnityShaderVariables.cginc` in front of a `CGPROGRAM`, and those declare what the tick declares
  itself, `_Time`), its commit `src/ram.h` and the rest. The harness gives
  a folder whose `main.shader` brings in `ram.h` RAM in one texture.

Lanes (20 October 2026, `docs/machine/lanes.md`): a worker's pixels each run a function on an index of their
own (`lanes_begin`, `lane_next`, `lane_output`), and with `LANE_FIELD` (this folder's `main.shader`) a field of
3,968 lanes of no core's in rows the cores leave (`field_begin`, `field_output`; the commit's points for it in
`src/ram.h`). What they cost a pass of the three loads, from one snapshot each: the medians level.

The GPU device and the sound card are `gpu.shader` and `sound.shader` here: the
harness takes them from this folder when another processor's folder has none.

    bin\rvc_harness.exe --image linux-net

The harness treats a folder whose name begins with `rvc_lean` as this one
(`isLean`): the no-MMU machine's defines (`NO_PAGING`, `SBI_HLE`, `FPU`, `CORES=256`, `TICK_MRT`,
`COMMIT_BANDS`, `GPU_DEVICE`), so a copy of the whole folder elsewhere (`rvc_lean_try`) runs as well.

`crt.cginc`, `helpers.cginc` and `src/` are what the commit and the devices include, and what Unity's copies are
made from (`ShaderEmu/Sync shader sources from the repository`).

## What it is

The state texture is the machine's (docs/machine/multicore.md): RAM from row 64, core 0's block
and the workers' strip in rows 0-63, the worker geometry geotune lays out, the write cache of
three tables of buckets of three texels, the mailboxes of `src/mc.h`. A core's texels are where
`tick.hlsl`'s comment at `L1_AT` says, so the control pass, the host's reads
and the GPU device's passes are as they were.

What is not there: the MMU and its TLBs, the UART, the CSR cache and table (the CSRs Linux uses
are fields), `LAZY_MEMOP`, profiling and the experiments' switches (`X_P_*`, `TICK_LOAD`, `ZB`), the
tick drawn without eight targets, and upstream's switches of instructions: the general step is
one function of tests in the order of how common a kind is (`step`).

The tick:

- A fast loop (`fast`) for what cannot trap or change what interrupts see: the kinds told apart by
  a tree of tests (arithmetic, loads, stores and floats one group, in it arithmetic and loads
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

## Rounds a frame (`--tick-rounds`, `--tick-split`)

`--tick-rounds N` draws the tick and its commit N times a frame before the devices' passes, the
clock a tick on for each (`_TimePass`) and the host's zap in the first only. `--tick-split F`: the
first F always run; a later one gives core 0 what is left of its F x `--ticks` (texel 39.y, its
instructions in this frame) and draws a worker only when core 0 ended the round before early
(39.z), with
no readback, the tick's geometry shader leaving the workers out. Off by default: four rounds with
one always run made nxray 2.4 times as fast (47.3 to 19.5 ms a picture) and Quake, Quake III and
Red Alert 5 to 15% slower, a round costing its draws whether anything runs in it or not.
