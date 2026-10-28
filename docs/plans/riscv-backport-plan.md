# What gpu_vm learned that the RISC-V machine can take (28 October 2026)

gpu_vm (the GPU-VM repository) beats the RISC-V machine in the harness, but in a real VRChat instance in VR the two are
level: Dear ImGui 35 fps in the harness, at most 25 in VR on both. gpu_vm only stays ahead on instruction-heavy work
such as loading. This plan takes what gpu_vm found and applies it to the RISC-V machine, ordered by what decides
frames in VR.

## The measure

In VR a frame fits few Unity rounds, and each round costs a fixed amount of the main thread (cameras, blits) whatever
the guest does in it. What a game gets is **useful guest work a round** (and so a frame), not instructions a second
of card time. A round's work ends early when core 0's write cache fills:

| per pass, store-heavy game code | instructions it runs (RISC-V instructions) |
|---|---|
| RISC-V (336 texels of write cache, 8,192 a pass) | about 8,100 (98-99% utilization in Quake, ClassiCube, the NES) |
| gpu_vm (170 entries) | about 2,000-2,700 bundles, at 1.8-2.4 RISC-V instructions a bundle: about 3,500-5,300 |

gpu_vm's bundles do about twice the work of an instruction, and its cache is half the size: per round they cancel.
So **making instructions cheaper does not help in VR**. Only three things do:

1. more stores fit in a round (the write cache);
2. fewer stores for the same work (programs, the kernel, copies);
3. more rounds in a frame for the same main thread (the cost of a round, or several passes a round).

Every step below is judged by guest work a VR frame under a VR-shaped model, against the tree, in turn.

## What not to do (decided by gpu_vm's measurements)

- **No instruction fusion, no bundles, no new encoding.** That is what gpu_vm did. It made instructions cheaper on
  the card, which a pass that ends on its write cache never sees.
- **No more registers.** RISC-V's encoding caps them at 32, and gpu_vm compiled Quake's hot files with 64 and 32
  registers: 32 cost +0.5% bundles, +1.8% loads and -0.5% stores (GPU-VM RESULTS.md, 25 October, "the register
  count"). The cost appears only under 24.
- **No custom special-function float opcodes for now.** Quake III's instruction mix says one-op sin/cos/exp2/rsqrt
  plus fused multiply-add would remove about 5% of its instructions, nearly all on workers; core 0's frame is 1.3%
  floats (`gpu_vm/bench_real/quake3_mix.md`).

## Already on the RISC-V machine (do not redo)

- The commit writes whole texels, drawn as points; one writer a texel (`docs/machine/multicore.md`, "The commit as
  points", "Sixteen bytes, one writer").
- Statics a worker writes on 16 bytes (`AGENTS.md`). gpu_vm's llvm-objcopy wildcard bug does not apply: GNU objcopy
  takes the wildcard.
- The third write-cache table: the stalls were collisions at 50-80% full, and another bucket to try took Doom's
  full-cache passes from 1,840 to 93 (`AGENTS.md`). A fourth table measured 0.6% slower.
- Every GPU pass drawn every frame (`RVC_PASS_LIFE=1` is the old way).
- Hardware single-precision floats (RV32IMAF); Zba and Zbb built and measured (1-2.5% fewer instructions, within the
  noise: `docs/system/nommu.md`).

## Step 0: measure VR, and a harness model that predicts it

- **In the VRChat instance**, a probe sample of several seconds for Dear ImGui, Doom, Quake and one desktop program:
  rounds a frame, the main thread's milliseconds a round, the card's milliseconds a frame, and the share of core 0's
  passes that ended on a full write cache.
- **The harness model:** set `rvc_harness --unity F,U,R` (and `vm.exe`'s) from those numbers. Check that ImGui's
  harness fps under it is near its VR fps (25), on both machines.
- **Baseline table:** the same parts on RISC-V and gpu_vm under that model. Every later step is a row against it.

Done when the model's fps for the probed parts are within 10% of VR's.

## Step 1: what a round costs Unity (more rounds a frame)

RISC-V's round in the world (`docs/machine/multicore.md`, "In Unity") is:

1. the tick's camera (eight targets);
2. the unpack (a blit);
3. the gather (the points camera);
4. a blit of the state rows;
5. the commit's points (the same camera);
6. the control pass and its window copy (`WindowBack`).

gpu_vm measured (GPU-VM `docs/REGRESSIONS.md`, steps 1 and 2): a camera's render costs 0.15 ms of the main thread, a
blit next to nothing, binding a state set's textures 10 us; an idle round was 0.83 ms of a frame. It then took four
steps:

| gpu_vm change | effect |
|---|---|
| the commit and the operations' write in one camera (`gvmMergeWrite`) | idle frame 13.4 to 11.3 ms |
| the commit as blits (`GvmCommitBlit`) | Doom's main thread 3.4 ms a frame less; Half-Life's world 6 ms less |
| the GPU's lists as blits (`GvmGpuDrawBlit`) | a camera of main thread a frame, for 1.3% of the card |
| the tick as one blit below 8 cores (`GvmTickOne`) | main thread 0.33 to 0.19 ms a pass |

The blits draw geometry as patches: tessellation factor 8 only (never above 8: AMD's driver can clamp it), the
amplification in the geometry shader (`[instance(32)]`).

For RISC-V:

- the gather and the commit's points as blits (the same patch-and-geometry-shader form as `GvmCommitBlit`);
- the unpack, the state rows and `WindowBack` folded into those blits where their targets allow;
- the tick: eight targets need a camera (MRT); one target below 8 cores as gpu_vm does.

Each change is checked for the same RAM after N passes (the md5 check gpu_vm used), then measured as main-thread ms a
round and guest work a frame under step 0's model.

## Step 2: several passes in one round (the largest lever)

If a round is blits, its main-thread cost is a few calls, and the tick plus commit can run K times in a round: K
passes for one round's fixed cost.

- **Needs:** the state alternating between two textures inside the round (a pass may not read what it draws), the
  host's clock spread over the K passes (`HostClock`), and the devices (GPU lists, readback, input) still once a frame.
- **First, a probe:** K = 1, 2, 4 with nothing else changed, in the harness under the model and in the world, for
  ImGui and Doom.
- **The cost to watch:** card time grows with K. A round's work rises only while the card has room. The model's card
  budget says where K stops paying.

## Step 3: more stores in a round (the write cache)

RISC-V can do what gpu_vm could not. gpu_vm draws every core in one compile, so every core pays for the largest
array. RISC-V already gives its workers sizes of their own (`.b`, 3 to 6), and core 0 its own tables.

- **Core 0's cache:** 336 texels today; it was 768 on the machine with the MMU (`multicore.md`). Try 336, 512 and 768
  for core 0 alone, workers unchanged.
- **Measure:**
  - core 0's passes that end full and its instructions a pass, on `rvc_cpu`'s model of the cache (`RVC_WCACHE`);
  - then the card's time a pass and guest work a frame under step 0's model.
- **Done when** a size is found that is faster in VR on every probed part and slower on none in the harness, or the
  size is shown not to matter.

GPU-VM is measuring the same question for gpu_vm now: compare its answer with this one.

## Step 4: fewer stores for the same work

- **Copies that do not end the pass.**
  - RISC-V's `LAZY_MEMOP` only works with paging (it hides a destination by its TLB entries), and the world's machine
    has none (`NO_PAGING`), so it is off.
  - gpu_vm's way: libc's memcpy asks the commit for a copy from 2 KB up and goes on without a sync, up to 192 KB a
    pass; a core's access to a range still in flight stops it there (`musl/src/string/gpuvm/memcpy.c`).
  - Design the same for `NO_PAGING`, keeping the range check out of the fast loop. gpu_vm put it there once and took
    it out (GPU-VM RESULTS.md, 25 October, "copies that do not wait").
  - Measure the passes that end on a copy (`tools\tick_util.ps1`) and Quake's timedemo, where gpu_vm found copies of 2
    to 64 KB into GPU memory made most of memcpy's full ends.
- **The programs this session fixed on gpu_vm**, in sources both machines share (`linux/nanox`):
  - **nano-X's horizontal lines** (`scr_shaderemu.c`, GPU-VM 002bcdf): a filled shape's rows as one rectangle or
    one 1-bit mask, not a command a row. Pinball's frames went from 4.21 to 11.02 a second on gpu_vm. The fix to
    `string_words` in the same commit is gpu_vm's only (its GPU memory is at address 0), but harmless here.
  - **Doom's level load** (`doom_gl.c`, `doom_fast.c`, GPU-VM a06f225): the wall pictures made 64 at a time on the
    workers, no 64-bit divide in `clip`, the texture names' answers kept. Core 0's load went from 3.68M to 2.86M
    instructions.

  Each is checked as there: `DOOM_SUM` the same, Pinball's state sum at a held frame the same, and the picture compared
  pixel by pixel.
- **The kernel**, each only if a profile shows its cost on RISC-V:
  - `flatreloc_hook.py`: a flat program's relocations a word at a time where they lie. On gpu_vm they were 80% of
    Red Alert's start.
  - `zeroread_hook.py`: a read of `/dev/zero` cleared a megabyte at a time.
  - The boot-time workers' layout (GPU-VM c519897), if RISC-V's first program that asks for workers also waits for
    it.

## Step 5: a small ISA fork, judged by stores

- **Zfinx** (standard: floats in the integer registers, `-march=rv32ima_zfinx`, GCC 12 and later, LLVM 17 and
  later):
  - the gain: the float registers' 8 state texels a core go, the float ops decode as integer ops, and calls stop
    moving floats through integer registers;
  - the risk: float-heavy code has 32 registers for everything, and a spill is a store;
  - try it on two or three games and one physics or path-tracing program;
  - kept only if stores a frame do not rise and guest work a VR frame does.
- **A fused multiply-add on the fast path** (it reads three registers, so it takes the general path today). Then
  build without `-ffp-contract=off`. Small, and on workers mostly: after the steps above.

## Rules for every step

- Every run under 60 s (`timeout 55`), never two one-minute runs in a row; long builds in the background with a
  heartbeat.
- Card runs through `tools\gpu_queue.ps1`; a tick variant on WARP before the card.
- A change is measured against the tree in turn (alternating, several runs), under step 0's model and in the harness,
  on a shell loop and on Doom's timedemo at least.
- `python tools/test_all.py` passes, and `--races` for anything that touches workers. A program's own checks hold
  (`DOOM_SUM`, `NXWEB_CHECK`, the state sums).
- A step's result is written into this plan's step and the docs it changed, with units in every table.

## Order

Step 0 decides everything else, so it comes first. Then 1 and 2 (rounds a frame), 3 (stores a round), 4 (stores for
the work) and 5. Steps 1 and 3 are independent and can run side by side. Step 2 needs step 1's blits.
