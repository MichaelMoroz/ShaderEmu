# GPU-VM: a clean repository and the next architecture (plan, 25 October 2026)

## What is asked

- A new public repository, **GPU-VM**, written again from scratch, with nothing of the RISC-V machine or of gpu_vm's
  older experiments in it. It holds a clean VM, a clean harness, a clean Linux, and wl's example programs. The
  userland (musl, busybox, the apps and games) is copied over, since it does not depend on this work.
- A tick shader that is as simple as it can be.
- E2 and E1 implemented and compared with today's E4 on the test set.
- One core a wave (`gpu_vm/tick/WAVES.md`) as a condition every tick must meet.
- A write cache that performs better within the fragment shader's limits (`gpu_vm/tick/CACHE.md`).

## Why gpu_vm grew large

The shader itself did not grow to RISC-V's size. The project around it did.

| what | size, lines | note |
|---|---|---|
| RISC-V's tick, the processor (`experiments/rvc_opt/src`: emu, mem, ram, trap, csr, mmu, ins, mc, cpu) | about 4,200 | plus types.h's 1,874 and the devices (gpu.h 1,187, sound, fb, uart) |
| gpu_vm's tick (`gpu_vm/tick/readable/tick.hlsl`) | 947 | c8 as generated: 1,078 |
| of which the fast step, the ALU and float units, branches and the cache | about 240 | the part that makes speed |
| of which the general step (any op, traps, special registers, any address) | about 300 | a second implementation of most of the ISA |
| of which paths 1 and 2 (`whole_step`) | 44 | a third implementation of part of it |
| of which the operations (copy, fill, blit in rounds of three draws) | 145 | the DMA engine |
| of which the pass, state in and out, `make_room`, the worker budget | 160 | |
| gpu_vm in git (692 files) | 96,439 | tick/: 236 files (90 variants, 69 steps, 24 probes, the builder and its parts) |

So the cost of optimizing was not that each step got bigger. It was:

- the ISA implemented three times over, once for each speed (fast, whole, general);
- features for Linux and devices (traps, interrupts, the operations engine, the idle and worker budgets);
- a research tree that kept every variant, probe and generator as files.

The new architecture keeps one implementation of each op in the tick, moves rare ops to the compiler, and keeps the
research out of the repository.

## Principles

1. **One step body.** The tick runs one restricted ISA in one fast step. Any op the step lacks is either lowered by
   the compiler into the restricted ISA (division, remainder, `mulh`, the dropped float ops, as RV32I does without M),
   or handled by one small system step for what cannot be software (traps, `eret`, special registers, `wait`, `sync`).
   An op leaves the tick only if the model and then the games show no loss: performance still comes first.
2. **The compiler does the work.** Fused ops are chosen from profiles (zext, ret with an add, lpc with 18 bits). Rare
   ops are placed so that the system step is entered rarely; the compiler's mode-switching passes are the method (GCC's
   `mode-switching`, LLVM's `RISCVInsertVSETVLI`). The float conversions become flags on float ops (the follow-up page's
   choice).
3. **One core a wave.** A core is a whole number of waves (one or two on NVIDIA), padding is never rasterized, and the
   write cache is sized to fill those waves exactly (256 or 512 state texels).
4. **One reference.** A C interpreter defines the ISA, and every tick is checked against it on WARP before it reaches
   the card. `isa.h` is shared by C and HLSL, as today.
5. **Nothing split into several draws.** One logical write is one draw. The commit is one draw of whole texels: two
   cores must not store into one texel in the same pass, and the reference checks it. The write cache is one array
   probed in turn, stopping at the first empty way, behind a hashed bitmap (`gpu_vm/tick/CACHE.md`, "Decided for the
   new tick").
6. **A small repository.** One tick in the tree. Experiments live on branches or in the old tree, and their result is
   one row of `RESULTS.md` with the code deleted. No generated variants are checked in. Every document is short and
   current.

## The repository

| folder | what | comes from |
|---|---|---|
| `spec/` | `ISA.md` and `isa.h`: the encoding and the semantics | new; today's tables as reference |
| `ref/` | the C reference: one core, many cores, the devices; runs bare-metal programs and Linux (today's `gvm_cpu`) | new |
| `llvm/` | the GPUVM target and its patches | today's backend, with the MC layer and the encoder written for the new encoding |
| `tick/` | `tick.hlsl`, written by hand to be read, with the commit and operation draws | new |
| `harness/` | a D3D11 host: passes, uploads, readback, display, a WARP check mode, card timing | new; takes the old harness's run lock, since both share one card |
| `platform/` | the devices: console, timer, input, GPU drawing, sound | new, from `gpu.h` and `sound.h` as reference |
| `linux/` | the no-MMU arch port, its drivers and the image builder | new port; today's `gpu_vm/linux` as reference |
| `userland/` | musl, busybox, the apps and games | copied |
| `apps/wl/` | the 24 test kernels and the plot | `programs/wl` |
| `docs/` | how to build, run, measure | new |
| `unity/` | the world | last |

## Phases, each with a gate

**Phase 0: measurements that fix the design (in today's tree, no new architecture yet).**

- **0a, the wave probe** (WAVES.md): the pixel-to-wave map for every draw, block and padding case.
  - Gate: a geometry with 0% of waves mixing cores, and its padding cost in lanes and ms.
  - Done (25 October 2026): an 8 x 8 block a core, one row of blocks (2,048 x 8 for 256 cores), one triangle.
- **0b, what divergence costs today:** Nsight Graphics' Shader Profiler on c8 (live lanes per warp instruction) and
  the D3D11 timing probe. This bounds the gain from 0a.

**Phase 1: the ISA, the reference, the compiler and the tick, bare metal (new repository).**

- `spec/` and `ref/` for E4, E2 and E1: one semantics, three decoders.
- The LLVM MC layer emits each of the three. It packs bundles already, so E1 is no packing and E2 is pairs.
  - Branch distances are counted again in each encoding's units.
  - Relaxation handles E2's 32-bit constants and E1's prefix word.
- wl's kernels built bare metal: the first core alone, gvm_cpu's semihosting, no Linux.
- Three ticks (E4, E2, E1), each one step body, the same geometry, on the new harness.
- Gates:
  - E4 on the new harness is within 5% of c8 on today's harness for the same kernels. This shows the new harness
    measures the same thing.
  - E2 and E1 against E4 on all 24 kernels, in units a second, with the scatter and the geometric mean. The faster
    encoding is kept, and the other two are deleted.

**Phase 2: many cores and the write cache.**

- Workers on bare metal (mcw's calls without Linux), and wl's modes "one worker" and "every worker".
- The cache:
  - sized to the waves;
  - shared across a quad's lanes by `ddx_fine`/`ddy_fine` instead of copied (CACHE.md);
  - committed as whole texels where it can be.
- Gate: every worker faster than today's tick in wl's "every worker" mode (0.59x RISC-V today), and no kernel slower.

**Phase 3: Linux.**

- The no-MMU arch port written again for the kept encoding, with the drivers the userland needs: console, timer,
  input, `/dev/gpu` with `REST`, workers, the ROM.
- The userland copied, and the image made.
- Gate: it boots to a shell, wl runs in all three modes, and the test set passes against the reference.

**Phase 4: the programs and games.**

- wl first, then the games from the copied userland: Doom, Quake, Quake III, Half-Life, nxray.
- Gate: every game's fps at or above today's tree, measured as `gpu_vm/bench_games/README.md` measures today.

**Phase 5: the Unity world.**

## The test set

| test | what it checks | where |
|---|---|---|
| ref against tick, on WARP | every op, path, trap and a full cache, before any card run | phase 1 on |
| LLVM's MC and CodeGen tests, c-torture at -O0 and -O2 | the compiler | phase 1 on |
| wl's 24 kernels, units a second, three modes | speed against today and against RISC-V | phases 1 (first core), 2 (workers), 3 (Linux) |
| the games' benchmarks | fps | phase 4 |

## Open decisions (the user's)

1. **Where it goes.** I would make it `github.com/MichaelMoroz/GPU-VM` under the MIT license, as ShaderEmu has. It
   would be created private and made public when phase 1's gate is met, unless you want it public from the start.
2. **The builder.** I would leave the parameter builder and its search in the old tree, so the new repository has
   only the tick by hand.
3. **The kernel.** I would keep Linux 5.17.11 for the new port, since the drivers and the userland are known to work
   with it.
