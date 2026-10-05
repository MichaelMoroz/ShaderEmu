# rvc_opt

A patched copy of the shader from [pimaker/rvc](https://github.com/pimaker/rvc) (`_Nix/rvc`,
MIT License, Copyright (c) 2021 PiMaker). The state layout and the Commit pass are untouched,
so it runs upstream snapshots and payloads, and its emulation is bit-identical to upstream.

    tools\perf_test.ps1 -Rvc experiments\rvc_opt
    bin\rvc_harness.exe --rvc experiments\rvc_opt --payload rvc\_Nix\rvc\data-net --load-state build\snapshots\rvc_shell.snap

## Changes (all in the CPUTick pass)

Each one removes work that upstream repeats on every emulated instruction. All added state is
pass-local (`static`), never stored in the texture.

1. **Timer hoisted** (`emu.h`, `main.shader`): `mtime` depends only on `_Time`, so the
   double-precision math runs once per pass instead of once per instruction.
2. **Hot CSR shadows** (`mmu.h`, `csr.h`, `trap.h`, `emu.h`): `mstatus`, `mip` and `mie` were
   each looked up per instruction (a 16-way compare chain, then a texture read). They are now
   read once and dropped on any CSR write.
3. **Small TLB per access mode** (`mmu.h`): four direct-mapped entries each for fetch, read
   and write reuse a successful translation instead of a two-level page walk (two dependent
   texture reads) per access. Flushed on `mstatus` and `satp` writes, privilege changes, traps
   and `sfence.vma`. Only successful translations are cached, as RISC-V permits.
4. **Aligned word stores in one step** (`mem.h`): upstream stores a word as four byte writes,
   each a read-modify-write of the same word. An aligned RAM `sw` is now one read and one store.
5. **CSR writes drop only what they affect** (`csr.h`): a `mip`/`mie` write updates its shadow
   in place; only `mstatus` flushes the TLB. A pending-but-masked interrupt rewrites `mip` on
   every instruction, which used to flush everything each time.

6. **One dispatch for the common instructions** (`emu.h`, `OPT_DISPATCH`): upstream probes nine
   switches in turn. Integer ALU ops, branches, jumps, loads and stores now go through a single
   `[forcecase]` switch on the major opcode; anything it does not fully recognise falls through
   to the original decoder.
7. **Same-page fetch fast path** (`emu.h`, `mmu.h`, `OPT_FETCH_FAST`): a fetch from the page of
   the last translated fetch skips the mode, privilege and TLB checks.

6 and 7 are on by default; define `OPT_BASELINE` to build without them. `OPT_ALU_SELECT`
replaces the inner funct3 switch of 6 with a branch-free select; it measured within noise of
the switch, so it is off.

## Measured (RTX 5090, `tools\perf_test.ps1`, 2048 ticks)

| Shader | IPS | State hash after 1,024,000 instr |
| --- | --- | --- |
| upstream | 517k | 8d6fc1106d9cd3a1 |
| 1 + 2 | 604k | 8d6fc1106d9cd3a1 |
| 1 + 2 + 3 (one-entry TLB) | 748k | 8d6fc1106d9cd3a1 |
| 1 - 5 | 781k | 8d6fc1106d9cd3a1 |
| 1 - 5 + fetch fast path | 814k - 831k | 8d6fc1106d9cd3a1 |
| 1 - 5 + dispatch | 975k - 993k | 8d6fc1106d9cd3a1 |
| 1 - 7 (this folder) | 1,050k | 8d6fc1106d9cd3a1 |

After 6 and 7 the tick draw takes 1.66 ms instead of 2.30 ms, and its warp time shifts from
branches (28% -> 20%) towards memory and texture waits (17% -> 23%); dependent arithmetic stays
near 30%. The tables in the GPU counters section below were taken before 6 and 7.

A scripted shell session (5.07 M instructions, with traps, syscalls and cache stalls) also ends
in the same state and UART output as upstream. Cold boot to the prompt: 78.5 s -> 52.9 s
(measured with changes 1-3; not re-run since).

## Profiling counters

`PROF(id)` marks an event; ids and names are in `src/prof.h`. A normal build compiles them
away. With `rvc_harness --profile` each pixel tallies events in a local array and one pixel
adds its tally to a UAV buffer at the end of the pass, so the redundant copies of the CPU do
not inflate the totals (`tick` equals the instruction count exactly). Do not write the UAV at
the event site: FXC then refuses to compile the `[loop]` loops (X3531).

    bin\rvc_harness.exe --rvc experiments\rvc_opt --payload rvc\_Nix\rvc\data-net --no-stdin --load-state build\snapshots\rvc_bench.snap --fixed-dt 0.004 --bench 30 --frames 530 --profile

Events per emulated instruction on the benchmark, before and after changes 4-5 and the larger TLB:

| Event | 1-3 | 1-5 |
| --- | --- | --- |
| RAM word reads (`ram_read`) | 1.19 | 0.63 |
| ...of which texture reads | 0.77 | 0.50 |
| store steps (`ram_write_byte`) | 0.63 | 0.17 |
| page-table entry loads | 0.28 | 0.19 |
| CSR lookups | 0.21 | 0.15 |
| shadow/TLB flushes | 0.023 | 0.001 |

Halving memory reads bought only 4.5%, so what remains is mostly the per-instruction decode
and dispatch, not memory. 2% of instructions run the interrupt path for a pending but masked
interrupt (about six CSR lookups each); skipping it would change the CSR cache contents, so
it would no longer be bit-identical.

## GPU hardware counters (Nsight GPU Trace)

    pwsh tools\gpu_trace.ps1 -Filter warps_issue_stalled,latency

Nsight does not attach to the D3D11 harness, so `bin\rvc_trace12.exe` runs the same FXC bytecode
on D3D12. Two build switches exist for it:

- `NO_DOUBLES` (`--no-doubles`): NVIDIA's D3D12 path miscomputes double math in this shader (a
  double divide converted to uint returns 0xffffffff), which breaks the timer and MULH. With
  this define the host supplies the timer value and MULH uses exact 16-bit partial products.
  D3D11 doubles, D3D11 no-doubles and D3D12 no-doubles all end in state 8d6fc1106d9cd3a1 on the
  benchmark; MULH results can differ from upstream once a product needs more than 53 bits.
- `XREG_ARRAY` (`--define XREG_ARRAY`): guest registers in an indexable array instead of 31
  conditional moves per instruction. Bit-identical, bytecode 358 KB -> 236 KB and FXC time
  down about 30%, but 2.6% slower (773k vs 793k IPS), so it is off by default.

What the trace says about one frame (RTX 5090, 2048 ticks, mean of 20 frames):

| | CPUTick | Commit |
| --- | --- | --- |
| GPU time per draw | 2.30 ms | 0.075 ms |
| SM throughput, % of peak | 1.9 | 28.7 |
| pixel-shader warps active per cycle | 0.22 | 15 |
| texture read latency, cycles | 247 | 332 |
| local/global memory read latency, cycles | 159 | 502 |

The tick is 97% of the frame and leaves the GPU almost idle: it is a serial dependency chain,
not a throughput problem. Where its active warps spend their cycles:

| Warp state during CPUTick | Share |
| --- | --- |
| waiting on a fixed-latency dependency (ALU chain) | 29% |
| issuing an instruction | 23% |
| no instruction ready (after a branch) | 20% |
| waiting on L1/texture data | 17% |
| resolving a branch | 8% |

So about 28% is branch cost and 29% is dependent arithmetic, against 17% for all memory and
texture waits, which matches the event counters: fewer reads no longer buys much, and the next
gains have to come from fewer branches and shorter dependency chains per instruction.

## Fast iteration with DXC

`rvc_trace12 --dxc` compiles the same source with DXC to DXIL: 1.1 s for the tick pass against
2-3 minutes for FXC. It needed one source change (braces around a `case` body with a
declaration) and two compile-time type mappings for DX9 sampler types DXC no longer has.

| D3D12, busy loop, `--no-doubles` | FXC bytecode | DXC, shader model 6.6 |
| --- | --- | --- |
| this folder | 1,087k IPS | 1,101k IPS |
| `OPT_BASELINE` | 821k IPS | 940k IPS |
| ratio | 1.32x | 1.17x |

All end in state 8d6fc1106d9cd3a1, and DXIL does so with doubles enabled too: the double
miscompute on NVIDIA's D3D12 path only affects FXC bytecode.

What moves DXC's speed (`--dxc-opt`, `--dxc-sm`):

| Setting | IPS |
| --- | --- |
| shader model 6.6 (default) | 1,101k |
| shader model 6.0 | 994k |
| 6.0 with `-Gfp` (prefer flow control) | 911k - 929k |
| 6.6 with doubles (no `--no-doubles`) | 936k - 940k |
| `-O1`, `-O2`, `-O3`, `-ffinite-math-only`, `-all-resources-bound` | no difference |
| `-O0`, `-Od` | fail validation on the `[forcecase]` attribute |

So DXC is reliable for checking that an edit preserves emulation, and with 6.6 and
`--no-doubles` its speed is close to FXC's for this build, but the two can still disagree on
how much a change helps. Rank variants with it, then confirm with FXC.

## Write-cache geometry

`L1_SET_BITS` (sets per slice, default 9), `L1_SLICES` (default 2) and `L1_HASH_LOW` (pick the
set from low word bits only) are build defines, e.g. `--define L1_SLICES=1`. The defaults are
upstream's geometry. Other geometries stall at different times, so their state hash differs
from upstream; guest output was checked against upstream instead (identical for b9s1, b10s1
and b7s2 over a scripted shell session).

| Geometry | Texels | Busy loop IPS (instr/frame) | Shell session IPS (instr/frame) |
| --- | --- | --- | --- |
| 9 bits x 2 slices (default) | 1024 | 797k (2048) | 780k (2036) |
| 7 bits x 2 | 256 | 809k (1879) | 796k (1890) |
| 5 bits x 2 | 64 | 704k (502) | 669k (434) |
| 3 bits x 2 | 16 | 474k (135) | 500k (151) |
| 9 bits x 1 | 512 | 838k (2018) | 834k (1904) |
| 10 bits x 1 | 1024 | 831k (2048) | 825k (2001) |
| 10 bits x 1, low-bits hash | 1024 | 826k (2001) | 789k (2019) |
| 9 bits x 2, low-bits hash | 1024 | 802k (2048) | 789k (2039) |

- Size is not a speed lever: a quarter of the cache gains about 1.5%, and below that the pass
  ends early on stalls and the fixed per-frame cost takes over.
- Associativity is: one slice means one array read per lookup instead of two, worth about 5%,
  more than the extra stalls cost on these workloads. Not measured on a cold boot, which
  writes far more.
- The hash itself is a few shifts and masks; changing it moves stall counts, not per-lookup
  cost. Upstream's mixed hash stalls less than plain low bits once there is only one slice.

## ddx_fine / ddy_fine quad sharing: works, not worth it for rvc

- FXC accepts fine derivatives inside the `[loop]` tick loop and under `[branch]`, with warning
  X3595 only. `[fastopt]` on the loop must become `[loop]`, or FXC tries to unroll and fails.
- A probe that sends every RAM read result through two x-neighbour round trips (16-bit halves
  as floats) stayed bit-identical, so the exchange is exact on this GPU.
- It cost 9% (471k vs 517k IPS). What it could buy is write-cache capacity, and rvc is not
  short of it: the benchmark never stalls, and a whole boot loses under 2% of ticks to stalls.
- Layout catch: lanes of a quad must all run the CPU. Upstream's state region starts and ends
  mid-row, so some quads have lanes that return early; a real version needs a quad-aligned
  state area.
