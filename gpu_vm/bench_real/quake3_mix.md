# Quake III Arena's instruction mix, and what gpu_vm's float ops would take off it

22 October 2026. Asked: how much special-function and multiply-add work does Quake III do on the RISC-V machine,
what would gpu_vm's single-op `sqrt/rsqrt/rcp/sin/cos/exp2/log2/floor...` and `fmad` save, or is it logic-bound?

**Answer: logic and memory. Float ops are 10% of all instructions; making every software special function one
op and every fmul-into-fadd an `fmad` would remove about 5% of the instructions, all of it on worker cores. Core 0's
frame has 1.3% float ops.**

## The run

- `rvc_cpu` (main checkout, its `build/images/nommu`, Quake III built at 438fe4d), `RVC_PROF`:
  `nano-X -p & sleep 2; QUAKE3_STATS_FRAMES=100 quake3 +set timedemo 1 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3`,
  `--count-from "q3stat: frame 300:" --until "q3stat: frame 600:"`: 300 frames of play, the level's pictures long
  decoded. 50.47 million instructions, 168 thousand a frame: core 0 6.67 M (13%), workers 43.81 M (the server's
  frame, the scene, the client's game, its side helper: the default layout). 2.2% is the kernel.
- The drawing itself is the GPU's: what is counted is the CPU side (the scene's lists, `entity_draw`,
  `gpuDraw*`, models' matrices and lighting, the client's game, the server, the bot).
- Compiler: GCC 13.3 `-O3 -march=rv32imaf -mabi=ilp32 -ffp-contract=off -fno-math-errno -fsingle-precision-constant`.
  `-ffp-contract=off` (toolchain.sh): **no fused multiply-add is ever emitted**, so `fmadd` counts 0; the candidates are
  counted as fmul results feeding the next fadd/fsub. `sin/cos/tan/atan/atan2` are Quake's float `q_*`
  (`linux/quake/fmath.h`), `sqrt` is inline `fsqrt.s`, `floor/ceil/fabs` the float versions.
- No ELF of the image's build was kept: the ELF was rebuilt from 438fe4d's sources (libmedia left out) and its
  4,610 functions placed on the run's binary by masked instruction n-grams, the rest by their neighbours: 97.7% of the
  instructions named, 0.1% not.

## 1. Dynamic mix (% of instructions)

| kind | all | core 0 | workers |
|---|---:|---:|---:|
| int ALU (addi 8.7, add/sub 6.2, auipc 4.3, mv 3.6, slli 2.5, li 2.3, lui 1.8, andi 1.4, ...) | 32.9 | 43.7 | 31.2 |
| int loads (lw 18.2, lbu 1.2, ...) | 19.6 | 22.3 | 19.2 |
| int stores (sw 11.0) | 11.6 | 13.8 | 11.3 |
| branches | 10.5 | 11.2 | 10.4 |
| jumps, calls, ret | 4.1 | 5.8 | 3.9 |
| mul / div,rem | 1.01 / 0.06 | 0.75 / 0.08 | 1.05 / 0.06 |
| flw / fsw | 7.15 / 2.85 | 0.51 / 0.15 | 8.16 / 3.26 |
| fmul | 3.70 | 0.30 | 4.22 |
| fadd / fsub | 2.38 / 0.90 | 0.08 / 0.08 | 2.73 / 1.02 |
| fmadd, fmsub, fnmadd, fnmsub | 0 | 0 | 0 |
| fcmp (feq/flt/fle) | 1.41 | 0.19 | 1.60 |
| fmv.w.x / fmv.x.w (floats passed in int regs, ilp32) | 0.64 / 0.22 | | |
| fcvt.s.w / fcvt.w.s | 0.39 / 0.28 | | |
| fsgnj (fabs, fneg) | 0.18 | | |
| fdiv | 0.06 | | |
| fsqrt | 0.01 | | |
| fmin/fmax | 0 | | |
| **all float ops** (without flw/fsw) | **10.2** | 1.3 | 11.5 |

Memory is 41% of instructions (int and float loads and stores), compares-and-branches 10.5%, address forming
(auipc/lui/addi) a large part of the ALU third.

## 2. Special functions in software (300 frames)

| function | instr. inclusive | % of all | calls | a call | called from |
|---|---:|---:|---:|---:|---|
| `q_cos` | 548 k | 1.09 | 12,272 | 44 | `AnglesToAxis`, `CG_AddCEntity`, `AngleVectors` |
| `q_sin` | 311 k | 0.62 | 7,102 | 43 | same |
| `logf` (musl, in soft doubles: `__adddf3`, `__muldf3`) | 539 k | 1.07 | 300 | 1,795 | `pow` in `tr_shaderemu.c` line 375, once a frame, **core 0: 8% of its instructions** |
| `q_tan` | 59 k | 0.12 | 900 | 65 | `CG_CalcFov`, the renderer |
| `q_atan2` | 59 k | 0.12 | 1,346 | 43 | `vectoangles`, `CG_CalcFov` |
| `roundf` | 43 k | 0.09 | 2,061 | 20 | the game's syscalls |
| `VectorNormalize` (fsqrt + fdiv inside) | 96 k | 0.19 | 4,594 | 20 | |
| `floorf`, `ceilf`, `sqrtf`, `powf`, `expf`, `Q_rsqrt` | ~0 | | | | inline or not called |
| **sum of the one-op candidates** (sin, cos, tan, atan2, log/pow, round) | **1.56 M** | **3.1** | 24 k | | |

(`AnglesToAxis` 181 instructions a call, `MatrixMultiply` 109, `AngleVectors` 215: float code, but straight
multiply-add math, not special functions.)

## 3. What gpu_vm's ops would take off

| | instructions removed | % of all |
|---|---:|---:|
| each software special function above one op (call, range reduction, polynomial gone; ~24 k ops left) | ~1.53 M | ~3.0 |
| each fadd/fsub fed by the fmul just before it as one `fmad`/`msb`/`nmsb` (rounds the product first: **the same results as now**, so legal under `-ffp-contract=off`) | 1.04 M at most | 2.1 |
| fdiv -> rcp, fsqrt -> rsqrt, `VectorNormalize` shorter | | < 0.1 |
| **together** | | **~5** |

- Of that ~5%, the `pow` once a frame (1.1%) is a plain source fix too: `powf`, or `exp2f(x * log2f(c))` with the
  constant folded, in floats, would take most of it off on RISC-V too (not measured).
- In bundles the win is somewhat larger than in instructions for the polynomial chains (`q_sin` 43 instructions are
  a dependent fmul/fadd chain, a single op on gpu_vm), but they are 1.7% of instructions to begin with.
- Where the time is: functions with more than 20% float ops are 20% of the instructions (`entity_matrix` 56% float,
  `MatrixMultiply` 41%, `AnglesToAxis` 38%, `light_at` 24%, `stage_moved` 27%, the bot's `AAS_*` traces 26%); even
  there loads and stores are 33-58%. The rest is list building and copying (`gpuDrawAt`, `gpuDrawRoom`,
  `SE_AddRefEntity`, `picture_draw`, `memset`, `memcpy`: 50-60% memory, 0% float), network messages and
  delta coding (`MSG_*`, `Huff_*`, `CL_ParsePacketEntities`, `SV_SendClientSnapshot`), interpolation and prediction
  (`CG_CalcEntityLerpPositions`, `CG_PredictPlayerState`), collision (`CM_Trace*`), the worker plumbing
  (`mcw_done`, `mcw_ready`, `serve`: 19% of core 0).

## Conclusion

Quake III on this machine is bound by integer, load/store and branch work: 41% memory instructions, 33% integer ALU,
15% control flow, 10% float ops. gpu_vm's special-function and `fmad` ops would save about 5% of its instructions
(3% special functions, 2% multiply-adds), on the worker cores; core 0, the frame's critical core, gains only the one
`pow` a frame. The levers for this program are memory and logic: load/store density, address forming
(auipc/lui/addi), compare-and-branch, call overhead.
