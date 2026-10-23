# The game bench

Game-like work in gpu_vm assembly (`gamebench.h`) for gvmbench (`gvmbench.cpp`), so that a tick or an ISA change can be
judged in a minute rather than in a game run. The same kernels exist in C for the RISC-V machine
(`gamebench_rv/gamebench.c`), and in C for gpu_vm's clang (`gamebench_rv/gvm_lone.c`). The scenes are calibrated
against Quake III, Half-Life and nxray (`gpu_vm/bench_games/run.py`).

The kernels are written with `gvm_asm.h`'s op-level calls. The assembler bundles them, so a step with another
encoding runs the same work.

## The cases

| case | set | what it is |
|---|---|---|
| g_logic | game | core 0 alone, a frame of entity logic as Half-Life's core 0 does it: calls through a table, frames four deep with a trace's result struct in them, struct copies, a hash table and heap nodes, a BSP-like trace, some floats |
| g_verts | game | one core: a model's vertices transformed by bones (a worker's float loop over arrays) |
| g_ray1 | game | one core: rays against spheres (nxray's worker) |
| g_q3 | game | core 0, 3 busy workers (a server's logic, a scene's vertices, a client helper) and 2 awake idle |
| g_hl | game | core 0 (the heavy logic), 4 busy workers and 40 awake idle |
| g_ray | game | core 0 and 191 workers sharing a picture in chunks of 32 pixels |
| g_q3p | game2 | Quake III as its passes were first modelled: core 0 waits for its jobs within the frame, and a server worker runs its whole budget |
| g_hlw, g_q3w | game3 | g_hl and g_q3p with 251 idle workers, the harness's 256 cores |
| g_q3s, g_q3r, g_hlr | game4 | the scenes calibrated to the games' core 0 pass ends (below) |
| g_tune | game_tune | a scene from `GB_TUNE`, for calibration (for example `E=72,spills=3,snap=2,serial=0,jobs=1:88/1:32/1:8,spin=1,spinE=64,idle=2`) |
| `_r`, `_f`, `_c` | game_x | the same work with the stack in registers, in the core's file, or the snapshots copied by the commit's copy operation |
| `_v` | game_vec | g_logic, g_verts and g_ray1 with the 128-bit vector ops (needs a vec tick) |
| g_logic_cc, g_verts_cc, g_ray1_cc | game_cc | `gvm_lone.c` as clang compiles it (`gamebench_rv/build_gvm.sh` writes `build/gamebench/gvm_lone.elf`), scored in ns a bundle |

The score is frames a second: core 0's r60 counts entities (or chunks), and a frame is `unit_div` of them. A case is
checked against the reference machine like every gvmbench case.

    pwsh tools\gpu_queue.ps1 bin\gvmbench.exe --variants FILE --set game --rounds 3
    GVM_PASS_US=250 ... --case g_q3s,g_hlr,g_hl      (ranking ticks as the games do: see "A pass's cost")
    GVM_ENDS=1 ...                                   (why core 0's passes end: not a timing)

## Calibration

### Against the RISC-V machine

`gamebench_rv/run.py` runs the same kernels on the RISC-V machine (rvc_opt on the card). The table below compares x23
with it, frames a second, in the same batch.

| | g_logic | g_verts | g_ray1 |
|---|---|---|---|
| RISC-V | 40.26 | 18.72 | 8.29 |
| x23 | 59.94 (1.49x) | 34.96 (1.87x) | 10.37 (1.25x) |
| builder's s8 v0008 | 73.64 (1.83x) | 38.50 (2.06x) | 11.71 (1.41x) |

On the scenes, x23 over RISC-V was g_hl 1.18 (Half-Life 1.10), g_ray 1.04 (nxray 0.86) and g_q3 1.74 (Quake III 0.79).
Quake III is bound by passes, not by code, which is what the later scenes model.

### Against the games' pass ends

`run.py bench` keeps a frame log: a sample of core 0's pass end a host frame (pc, bundles, why). Core 0's ends on the
card (img-fx2, 23 October 2026) and the calibrated scenes on the same ticks:

| | x23 | 16 buckets (s11 v0004) |
|---|---|---|
| Quake III | sync 41-55% (median 567-2,238 bundles), full 45-59% (~2,600-3,400) | sync 44% (628), full 56% (1,595) |
| g_q3s | sync ~50% (~400), full ~50% (2,800-2,900) | sync 37% (243), full 63% (1,356) |
| Half-Life | full 83% (2,588), sync 14%, budget 3.5% | full 94% (1,285), sync 5% |
| g_hlr | full 87% (2,693), sync 13% | full 92% (1,346), sync 8% |

Quake III's share of sync ends moves with the card's speed: its frames run on fixed time and core 0 rests when it is
ahead.

The first scenes missed this. g_hl's passes all ended full, and g_q3p's sync passes were 79 bundles of polling. They
also ranked ticks the wrong way round.

What the games' ends are:
- Every sync end of Quake III's core 0 is the REST ioctl's pause (`shaderemu_gpu_rest`). Core 0 has nothing to do until
  its jobs are done.
- None of them is an access an operation in flight holds, or an operation asked for with the queue full. `gvm_cpu`
  with `GVM_SYNCLOG=FILE` writes these ends by kind, pc and ra.
- 42% of its full ends are memcpy of snapshot structs (`CL_ParsePacketEntities`, `CL_ParseSnapshot`). 23% are the
  kernel's scheduler on the REST path.
- 81% of Half-Life's sync ends are the same pause.

### A pass's cost

gvmbench queues passes back to back, so a pass costs only the tick's GPU time. The harness pays about 0.25 ms a pass
more: the rows it reads back and its own loop. Fitted from the games, that is 240 us for Half-Life and 258 us for
Quake III.

Without that cost every scene preferred the 16-bucket write cache to x23, by 1.10-1.35, while the games did not. The
smaller cache halves the bundles a pass and also halves the pass's GPU time. `GVM_PASS_US=250` adds the harness's cost:

| s11 v0004 over x23 | plain | GVM_PASS_US=250 | the game |
|---|---|---|---|
| g_hl | 1.10 | 0.91 | Half-Life 0.92 |
| g_hlr | 1.12 | 0.95 | |
| g_q3s | 1.35 | 1.06 | Quake III 1.05 |
| g_q3p | 1.13 | 0.93 | |

Rank ticks that change how many passes core 0 needs (the write cache, overflow entries, worker budgets) with
`GVM_PASS_US=250` and g_q3s, g_hlr and g_hl.

Do not take a timing with `GVM_ENDS=1`: its readback loop is inside the timed window.

## Findings

These are recorded in `gpu_vm/IDEAS.md`, rows 15-17 and 19-22.

- **Bundles.** Many of the games' one-op bundles are not a scheduling loss (`gamebench_rv/oneop.py`). Both ops would
  need the bundle's single long immediate (fixed by `-gpuvm-rebase` and `-gpuvm-split-sp`), or a control word ends the bundle.
- **Late branches.** On the lone kernels they remove 7-16% of ray1's bundles, and fx1 removes 9%. On the games they
  break even against the tick's cost.
- **fnrel.** Quake III +3.2% (static bundles -6%), and Half-Life runs with it.
- **Vectors.** The vector forms are slower than the scalar ones: a vector op takes a bundle of its own, and lanes cost
  vins/vext. At most 5-8% of Quake III's worker bundles could use one.
- **The commit's copy operation** is much slower for struct copies (`_c`), and the games do not stall on it.

## The card's clock

On 23 October 2026 the card ran for a while at 1,130 of 3,090 MHz: a hardware power brake, on AC. Absolute frames a
second from such a batch are about half the usual (g_hl 17.2 against 42.4).

Compare only within one batch, note `nvidia-smi --query-gpu=clocks.sm` at its start and end, and drop a batch where it
changed.
