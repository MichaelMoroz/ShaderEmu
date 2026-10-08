# rvc_opt

A patched copy of the shader from [pimaker/rvc](https://github.com/pimaker/rvc) (`_Nix/rvc`,
MIT License, Copyright (c) 2021 PiMaker). The state layout and the Commit pass are untouched,
so it runs upstream snapshots and payloads, and its emulation is bit-identical to upstream.

    tools\perf_test.ps1 -Rvc experiments\rvc_opt
    bin\rvc_harness.exe --rvc experiments\rvc_opt --payload rvc\_Nix\rvc\data-net --load-state build\snapshots\rvc_shell.snap

## Build switches

Only these remain in the source:

| Define | Effect |
|---|---|
| `NO_PAGING` | machine without an MMU (below) |
| `M_MODE_ONLY` | machine mode only, includes `NO_PAGING` (below) |
| `NO_DOUBLES` | timer value from the host instead of double math (needed on D3D12) |
| `PROFILE` | `PROF()` event counters through a UAV |
| `L1_TABLE_BITS`, `TLB2_N`, `RAM_TILE_BITS` | cache, TLB and layout sizes |
| `L1_LOCAL`, `L1_STATIC` | the big arrays as locals of the pass (23 below): on with fxc2, off with FXC |

Everything else named in this document (`OPT_BASELINE`, the `OPT_*` experiments, `XREG_*`,
`MULH_DOUBLES`, `L1_HASH_LOW`, `RAM_TILE_ZORDER`) was a switch that existed while it was being
measured and has since been removed: the faster side is now the only code. The names are kept
below as labels for what was measured. Removing them left the compiled shader byte-identical.

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

8. **Interrupt gate** (`emu.h`, `OPT_IRQ_GATE`): the per-instruction interrupt/UART step is
   skipped while repeating it would do nothing: no trap was taken last time and none of its
   inputs (CSRs, CLINT and other MMIO, paging mode) has been written since. That includes an
   interrupt that is pending but masked. One UART poll tick in 256 still runs it.
9. **Fast step** (`cpu.h`, `emu.h`, `OPT_FAST_STEP`): while the gate holds, integer ALU ops,
   branches, jumps and RAM loads/stores whose translation is known (TLB hit, machine mode, or
   paging off) run on a short path that skips the general instruction path entirely. About 90%
   of instructions take it. Everything else, and anything that would trap, uses the general
   path unchanged.
10. **Guest registers in an indexed array** (`types.h`, `XREG_ARRAY`): one store per register
    write instead of a conditional move over all 31.
11. **Write-cache occupancy bitmap** (`mem.h`): one bit per cache entry written this pass, so
    lookups skip entries that are still empty instead of reading the 16 KB array.
12. **Vector component select** (`helpers.cginc`): `data[idx]` on a `uint4` became selects; DXC
    compiled it as four stores and a load on every fetch.
13. **Commit skip** (`types.h`, `OPT_COMMIT_BLOOM`): the tick pass leaves its write bloom filter
    in spare state word (41,0).r; RAM texels it rules out skip their eight cache lookups, and
    the commit pass clears the word, so the state after a frame is unchanged. Worth about 1%.

14. **Inner fast loop** (`main.shader`, `cpu.h`): fast steps run in their own tight loop
    (`fast_tick()`), and the general path runs once when one fails. With both paths in one
    loop body, every value the general path can modify had to be merged where the paths
    rejoin, on every tick: about 680 phi nodes per iteration in the DXIL, against roughly 70
    instructions of real work. The inner loop carries only what a fast step can change (93).
15. **Second-level TLB** (`mmu.h`): 64 entries per access mode behind the small first-level one,
    tagged with the privilege context and a generation number, so entries survive traps and
    are dropped only on `satp` writes and `sfence.vma`. Fast-step coverage 89.5% -> 94.6%.
16. **Integer MULH** (`emu.h`): exact 16-bit partial products instead of upstream's doubles,
    which round once a product needs more than 53 bits. Results can therefore differ from
    upstream on very large products; none did in any test. `MULH_DOUBLES` restores upstream's.
17. **Redundant fast-step writes removed** (`emu.h`): the timer value and the single-step
    marker are already set by the first tick of each pass, which always takes the general path.

18. **Fast run** (`cpu.h`, `fast_run`): what is constant for a run of fast instructions is
    decided once, not per instruction: single-stepping, the interrupt gate, and the distance to
    the next UART poll tick. The clock is advanced once at the end of the run.
19. **Early register reads** (`emu.h`): rs1 and rs2 sit at the same bits in every format, so both
    are read before the opcode is decoded and the reads overlap the decode.
20. **Instruction window with read-ahead** (`cpu.h`): the fetched RAM texel (four instructions)
    is kept, and entering a texel also reads the next one, so in straight-line code the texture
    read has finished long before its instructions are needed.
21. **Second-level TLB grown to 256 entries per mode.**

22. **`sfence.vma` for one page** (`emu.h`, `mmu.h`): with an address in rs1 only the entries
    that could translate that address are dropped (all three access modes, any privilege
    context; the megapage entry covering it too) instead of every entry. Linux flushes single
    pages far more often than everything, and after a full flush each page touched next is a
    page walk on the general path. Over a cold boot: 351k page walks become 141k, 97.8% of
    instructions on the fast path become 98.4%, same instruction count and console output.
23. **Write cache and second-level TLB as local arrays** (`L1_LOCAL`; `l1_local.h`, `mmu.h`,
    `main.shader`): the three big arrays are locals of the tick's `frag`, handed down as `inout`
    arguments, and so are not zeroed at the start of every pass: 1024 `uint4` and twice 768
    `uint`, about 0.4 ms of the 0.8 ms a pass costs before it has emulated anything. The TLB
    got an occupancy bitmap for it, like the write cache had (and a second one for entries
    flushed by 22, which must also hide what the texture still holds). This needs a compiler
    that works on an `inout` array in place: fxc2 (it defines `__FXC2__`, which switches this
    on) and DXC do. FXC copies the array at every call and fails ("can't unroll loops marked
    with loop attribute"), so under FXC the arrays stay static. `L1_STATIC` keeps them static
    under fxc2 too.

24. **MULH in one instruction** (`emu.h`): the GPU's `umul` and `imul` return both halves of a
    32 x 32 bit product, which HLSL cannot ask for. fxc2 adds `mulhi()` (and `umulExtended()`,
    `imulExtended()`), so with `__FXC2__` `mulhu32()` is that one instruction instead of four
    multiplications and a carry chain, and `mulh` needs no sign corrections. Our raytracer
    (73,669,730 instructions, machine mode): 4.18M IPS against 3.96M to 4.13M without, same
    final state. Under FXC the partial products remain.

25. **The fast loop is left where a thing is found out** (`cpu.h`, `fast_run`): the step used
    to be a function returning success, and the loop tested that, the budget and the stall
    flag on its way round. In D3D11 bytecode a return value is a flag, so every instruction
    paid for writing it, copying it into the caller's, testing it twice, and keeping
    `pre_valid` and the stall count up to date: 6 branches and some 20 other operations a
    turn where DXC's arbitrary control flow has 4 branches. The fetch and the step are now
    written into the loop, and each way out is a `break` at the place that decides it;
    `pre_valid` is set on the one path that needs it and the stall count after the loop. Our
    raytracer's tick pass on D3D11 (fxc2): 0.444 ms to 0.365 ms for 2,048 instructions; DXC on
    D3D12 0.354 to 0.348. Same state hash on both.

With 22 and 23, D3D11, bytecode from fxc2, same machine and the same state hash in each column
(emulation is unchanged; only the TLB's bookkeeping texels differ from before 22):

| Build | 6000-frame bench, 2,048 ticks | fixed cost per pass | Cold boot (21,000 frames) |
| --- | --- | --- | --- |
| static arrays (`L1_STATIC`) | 1,502k IPS | 0.80 ms | 29.9 s |
| local arrays (default with fxc2) | 1,951k IPS | 0.39 ms | 22.6 s |

6 to 21 were once switchable (`OPT_BASELINE` built without them); they are now unconditional. `OPT_ALU_SELECT`
replaces the inner funct3 switch of 6 with a branch-free select; it measured within noise of
the switch, so it is off.

## `M_MODE_ONLY`: machine mode only

`--define M_MODE_ONLY` (harness: `--machine mmode`, automatic for our own programs) builds on
`NO_PAGING` and removes the other privilege levels: no supervisor or user mode, no trap
delegation, no `sret`, `MPP` hardwired to machine, every CSR accessible. OpenSBI and what it
starts (Linux, MicroPython, the Rust payload) and rvc's `bare` test need the supervisor and do
not run on it.

All of this is on the slow path (traps, CSR instructions, `mret`), so the gain is small. Our
raytracer, 75,275,360 instructions on a fixed timestep at 16,384 ticks, two runs each, same
state hash `35e2e5a58f60087b` on all three:

| Machine | IPS | CPUTick DXIL |
|---|---|---|
| full | 3.00M - 3.17M | 158 KB |
| `NO_PAGING` | 3.52M - 3.61M | 114 KB |
| `M_MODE_ONLY` | 3.67M - 3.71M | 101 KB |

## `NO_PAGING`: a machine without an MMU

`--define NO_PAGING` (harness: `--machine nopaging`, automatic for the bare-metal images) hardwires
`satp` to 0, which the privileged spec permits: writes are ignored, every address is physical,
and the TLBs, the fetch-page check and the translation flags are not compiled in at all. Linux
cannot run on it. Guests that never enable paging run identically: same instruction count and
state hash after 6,000 frames at 2,048 ticks on a fixed timestep (DXC, D3D12, with another
harness instance running at the same time, so absolute speeds are low):

| Image | Full machine | `NO_PAGING` | State hash (both) |
|---|---|---|---|
| rvc-raytrace | 2,613k IPS | 2,982k | `8d45fbbd0d818281` |
| bare | 4,764k | 5,575k | `2748940119e7f826` |
| rust | 3,867k | 4,484k | `0cad656e1252cdc8` |
| micropython | 2,436k | 2,855k | `a624cde3d3333ff3` |

## Measured (RTX 5090, `tools\perf_test.ps1`, 2048 ticks)

| Shader | IPS | State hash after 1,024,000 instr |
| --- | --- | --- |
| upstream | 517k | 8d6fc1106d9cd3a1 |
| 1 + 2 | 604k | 8d6fc1106d9cd3a1 |
| 1 + 2 + 3 (one-entry TLB) | 748k | 8d6fc1106d9cd3a1 |
| 1 - 5 | 781k | 8d6fc1106d9cd3a1 |
| 1 - 5 + fetch fast path | 814k - 831k | 8d6fc1106d9cd3a1 |
| 1 - 5 + dispatch | 975k - 993k | 8d6fc1106d9cd3a1 |
| 1 - 7 | 1,050k | 8d6fc1106d9cd3a1 |
| 1 - 13 | 1,575k | 8d6fc1106d9cd3a1 |

With 14 to 17, FXC on D3D11 measures 1,856k - 1,865k IPS on the perf test (five runs) with the
upstream state hash. The scripted shell session ends in upstream's state and output at
1,827k IPS, and the deterministic cold boot ends in upstream's state, instruction count
(42,245,866) and console output at 2,037k IPS, 21 s instead of 77 s.

14 to 17 were developed with DXC on D3D12 (`rvc_trace12 --dxc --no-doubles`). Under DXC, median
of five runs:

| Build | Busy loop | Syscall-heavy | Cold boot (21,000 frames) |
| --- | --- | --- | --- |
| 1 - 13 | about 1,750k | about 1,700k | |
| 1 - 17 (this folder) | 2,052k | 1,982k | 2.2M IPS, 19 s |
| `OPT_BASELINE` | about 930k | about 900k | 987k IPS, 43 s |

All three workloads end in the same state as the `OPT_BASELINE` build, cold boot included
(42,231,339 instructions).

With 18 to 21 (DXC, D3D12, median of 3 to 5 runs), by ticks per draw:

| Ticks per draw | Busy loop | Syscall-heavy | Draw time |
| --- | --- | --- | --- |
| 2,048 | 2,193k | 2,093k | 0.9 ms |
| 32,768 | 2,992k | 2,540k | 11 ms |
| 65,536 | 3,013k | 2,488k | 22 ms |
| 131,072 | 3,053k | 2,429k | 43 ms |

About 0.15 ms per draw is fixed, so more ticks per draw raise IPS until the write cache fills:
the write-heavy workload peaks near 16,000 to 32,000 ticks, the busy loop keeps gaining. A draw
of 22 ms is fine for a benchmark and far too long for a VR frame; pick ticks for the frame
budget, not for the headline number. The cold boot at 2,048 ticks runs at 2.38M IPS (18 s).

Measured and left off in this round (all exact, all slower or neutral): a branch-free datapath
for the non-memory instructions (`OPT_BRANCHLESS_ALU`, 5% slower than the switches), a data
texel window (`OPT_DATA_WINDOW`), `OPT_ADDI_FIRST`, folding the page and alignment checks into
one compare, an innermost per-texel instruction loop, a 2048-entry write cache, and a single
array TLB (`OPT_TLB_ARRAY`). What helped was hiding latency, not removing branches.

18 to 21 have not been built or measured with FXC.

With 1 - 13 the scripted shell session runs at 1,566k IPS with upstream's final state and
output, and a deterministic cold boot (21,000 frames, 42.2 M instructions) ends in the same
state and console output as upstream in 25 s instead of 77 s.

How the last third was found (all under DXC, a few seconds per try):

- Timing-only ablations: the bare fetch loop runs at 7M IPS, and the interrupt/UART step alone
  cost 25%.
- Unit-cost probes (extra work whose result lands in a debug field): a dependent texture read
  costs about 57 ns, a write-cache array read 27 ns, a switch about 9 ns per binary level, and
  independent loads, small-array stores and plain branches 3 to 5 ns.
- Counting fast versus general-path ticks by reason showed which cases were worth moving.
- Unrelated code changes move DXC results by up to 10%, so only structural changes counted.

Tried and left out: caching the fetched instruction texel, a 16-entry array TLB (more fast
ticks, but slower lookups), select-based ALU, a branch-free register write. A one-slice write
cache is 9 to 14% faster again but changes when passes stall, so its state no longer matches
upstream (see the geometry section).

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

This section is the history of the cache of words (upstream's, 512 sets of four, sized by
`L1_SET_BITS` and `L1_SLICES`, which no longer exist). The cache is of RAM texels now, see
"Texels instead of words" at its end; `L1_TABLE_BITS` (7 by default: two tables of 128
buckets of three texels) is its one size. Any geometry stalls at different times, so state
hashes differ between them and guest output is what is compared.

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

The table above is of upstream's hash (address bits 2-8 and 11-12). The set is now bits 2-10
xor bits 11-19. With upstream's, a frame ended on a full set with a median of 550 of the
2,048 places taken, and in the set that overflowed the words were 512 bytes apart nine
times in ten (`--l1-log` and `tools/l1_study.py`, which replays a run's writes through other
designs). Frames that ended on a full set, D3D12 + DXC, same runs before and after:

| Run | Upstream's hash | Xor | Instructions in such a frame |
| --- | --- | --- | --- |
| Tiberian Dawn, mission 3 to frame 300 | 11,040 | 5,408 | 5,624 to 8,416 |
| Tiberian Dawn, mission 1 to frame 300 | 11,353 | 6,038 | 5,842 to 8,265 |
| Doom, to its first figures | 5,089 | 2,488 | 3,954 to 6,309 |

The game's state sums are unchanged on both backends. What the replay says is left: 16 words
of overflow would take most of the remaining stalls (not built: the commit pass would have
to look there too), and one texel a lookup (1,024 sets of two) doubles them.

### Texels instead of words

Both of those hashes are of a cache of words, and both are replaced: the cache now holds RAM
texels, four words to an entry. Stores come in runs (a register save, a structure, a copy),
and a run that took four entries, in four sets, takes one. The same 1,024 state texels hold
256 buckets of a tag texel and three RAM texels: 768 texels, 3,072 words. The buckets are two
tables of 128 with different hashes (the texel number's low bits; those bits shifted and
xor-ed with higher ones), and a texel goes to the second table only when its bucket in the
first is full, so most lookups read one bucket. An entry is made from the texel as RAM has
it: a store that changes nothing costs no entry, a partial store needs no second read, and
the commit copies whole texels (one lookup a RAM texel where the word cache did eight).

Replaying recorded stores (`tools/l1_sim.py`; a store log taken 32 instructions a record so
that its order is known) through the layouts, instructions a frame and the share of frames a
full cache ended:

| Layout | Linux boot | desktop, glxgears | shell (`ls -lR`, `find`, md5sum) |
| --- | --- | --- | --- |
| words, upstream's set | 8,196, 60% | 5,395, 72% | 7,165, 49% |
| words, set from xor-ed bits | 10,045, 47% | 6,975, 68% | 8,305, 35% |
| words, two tables of 512 x 2, second when the first is full | | 8,356, 50% | |
| words, any 2,048 (no hash can do better) | 12,820, 13% | 10,738, 8% | |
| texels, two tables of 128 x 3 (this) | 12,257, 18% | 10,573, 18% | 9,677, 8% |
| texels, two tables of 64 x 3 | 9,486, 50% | 7,184, 62% | 8,028, 42% |
| texels, any 819 | 12,967, 7% | 11,035, 5% | 9,913, 0% |

Measured, D3D11 + fxc2, 16,384 instructions a frame at most:

| | words, upstream's set | words, xor | texels |
| --- | --- | --- | --- |
| frames to the login shell (upstream's image) | 4,517 | 3,538 | 2,800 |
| desktop with glxgears, 12,000 frames: frames a full cache ended | 8,237 | 8,065 | 39 |
| the same: instructions a frame | 5,869 | 6,579 | 11,679 |
| boot, checksums of three programs, `ls -lR`, 300 KB of `dd`: frames | 50,014 | 42,798 | 39,084 |
| the same: seconds | 186.1 | 184.6 | 186.6 |
| Linux bench, 16,384 a frame, fxc2's bytecode on D3D12 | 3,456k IPS | | 3,893k IPS |

The console output and every checksum of that third run are the same with all three, and on
D3D12 + DXC; our raytracer ends with the same RAM but for its first word, which the word
cache's commit zeroed (it took a free entry, address 0, for a store to address 0).

A store costs more than it did (a texture read when a texel enters the cache), which is why
fewer frames are not less time in the third run; where a pass costs more than in the harness
or their number is capped, the frames are what counts. `L1_TABLE_BITS=6` halves the cache:
every instruction is then about 7% cheaper in the tick pass (0.455 ms against 0.488 for
2,048), stalls return (4,101 frames of that desktop run) and the harness's wall time is the
same.

## RAM texel layout

`RAM_TILE_BITS=b` places RAM in square tiles of 2^b x 2^b texels instead of upstream's
row-major order (32 KB per texture row), so nearby addresses are close in both directions;
`RAM_TILE_ZORDER` walks each tile in Z-order (Morton order). Only the address-to-texel mapping
changes. Snapshots are tied to the layout: `tools\relayout_snapshot.py in.snap out.snap BITS [z]`
converts a row-major one.

All layouts end in the reference state on both workloads. Speed (DXC, D3D12, median of 4 and 3
runs):

| Layout | Busy loop | Syscall-heavy |
| --- | --- | --- |
| row-major (default) | 1,731k | 1,744k |
| 8x8 tiles (1 KB) | 1,788k | 1,609k |
| 16x16 tiles (one 4 KB page) | 1,743k | 1,790k |
| 32x32 tiles (16 KB) | 1,843k | 1,680k |
| 64x64 tiles (64 KB) | 1,774k | 1,770k |
| 16x16 tiles, Z-order | 1,825k | 1,696k |

No layout wins on both workloads and the spread is within what unrelated code changes cause,
so texture locality is not a limit here and the default stays row-major. The guest touches
only a few MB, which the GPU's caches hold in any order.

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
