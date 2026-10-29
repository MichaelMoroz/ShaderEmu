# The RISC-V machine's instruction set: what to add, measured (28 October 2026)

Step five of `riscv-backport-plan.md` (section 6, "The ISA fork, last"). The machine runs RV32IMAF, ilp32, on the
D3D11 tick of `experiments/rvc_opt`. Every candidate here was measured in the same way:

- The programs were built again with it.
- `rvc_cpu` ran them with the new instructions.
- Counts were taken on the profile's windows (`riscv-profile.md`).

The tick was prototyped for the two that need it.

## The ranking

| rank | what | core 0's instructions (%) | the window's passes (%) | needs | verdict |
|---|---|---|---|---|---|
| 1 | Zba, Zbb and Zbs | -2.7 to -3.8 (Doom, Quake, Tiberian Dawn, qjs, cc); -1.4 (nxray) | -1.8 to -3.5 | `-march`, the tick's `ZB` on (it exists), Zbs's four lines | do first |
| 2 | `fmadd` fast in the tick, then `-ffp-contract=fast` | -1.2 (Quake), -1.7 (nxray core 0); nxray's workers -9.1 | -0.7 (Quake), -4.5 (nxray) | a case in the fast step's last switch; every float sum set again | do second, with ZB: it is worth nothing without the tick's case (nxray's workers 2.2x slower modelled) |
| 3 | Zicond | at most 0.1 to 0.2 on core 0 (counted, not built: GCC 13.3 makes no `czero`) | - | GCC 14 or clang | not now |
| - | Zfinx | -1.4 (Quake) to +0.6 (nxray); nxray's workers +11.4 and their stores 2.8x | +3.9 (nxray) | every library, the kernel's floats, a tcc backend; GNU ld will not link it with F objects | no |
| - | C or Zcb (compressed) | none; the fetch is not where the time goes | - | a decoder on every instruction's path | no |
| - | load and store pairs (Zilsd and the like) | stores: 10-34% are a pair; no texel fewer | none for VR | a compiler pass; GCC and LLVM make `ld`/`sd` only for 64-bit values | no (a bundled ISA tried it: 0.99x) |
| - | a 64/32 divide step, the copy as an instruction, a cheaper `mcw_rest` | the divides are up to 33.6% of a part (`video-mpg`); the copy and the rest under 0.1% | - | - | no: each is cheaper fixed in the source or the kernel (below) |

"Passes" are `rvc_cpu`'s passes for the same work: its model of the harness's time. In that model each instruction
costs the same, so a Zba or Zbb instruction counts as one fast instruction. That is what the tick's `ZB` makes it.

## How it was measured

- **Builds.** Each variant is a private copy of the sysroot and programs, built in WSL (`~/bpi/VAR`). The base was
  `riscv-backport` at 7624f55's sources, with this branch's `toolchain.sh` fix (a4d5122).
  - `toolchain.sh`'s `ARCH_FLAGS` was set to the variant's `-march`. musl was built again with it.
  - Every `-march=rv32imaf` and `-ffp-contract=off` in `linux/` and `programs/` was replaced.
  - These were built: the userland, Nano-X, libmedia, Doom, Quake, Quake III, the ray tracers, QuickJS, Tiberian Dawn
    and tcc.
- **Variants.**
  - `base`: `rv32imaf`, contraction off, as today.
  - `zb`: `rv32imaf_zba_zbb_zbs`.
  - `fma`: `rv32imaf` with `-ffp-contract=fast`.
  - `zfinx`: `rv32ima_zfinx`.
  - `lay`: `base` with `-falign-functions=64`, a control for where the code lies.
- **Images.** Each image is the no-MMU prebuilt root with the variant's programs laid over it, made flat
  (`build/images/isa-VAR`, through `linux/nommu/image.py`).
- **Runs.** `tools/rv_compare/isa_runs.py` runs each part through `prof_parts.py` on its variant's image, with
  `RVC_ISA` set to what the variant needs and `RVC_WCACHE=1`. With `--mode prof` it also sets `RVC_PROF`, which gives
  stores and new texels by class.
- **Models.**
  - `tools/rv_compare/isa_model.py` counts upper bounds for each pattern from the 51 profiles' `pcs.txt`.
  - `tools/rv_compare/isa_time.py` weighs a run's instructions by the tick's costs per kind (`tick-costs.md`): 115 ns
    an ALU instruction, 450 a load, 370 a store, 2,700 one on the general path.
- **What changed between builds.** Doom's state sums are the same in every variant (tic 5000, `ac937893`).
  - nxray's picture sums are the same for `zb` and `zfinx`.
  - Under `fma` they differ (`af85a1c6` became `4df05335` at picture 5): products are fused and rounded once fewer.
  - Quake III's window is not the same work across builds: its server frames and bots' thinking differ (18,417,
    19,307 and 33,598 instructions a frame at frame 600 for base, zb and fma). Its deltas are left out of the tables.
  - Quake's level window (`quake`) is paced by time and is left out too. `quake-timedemo` is fixed work.
- **What the layout alone does.** `lay` against `base` moved core 0's instructions by under 0.1%. It moved the passes
  that ended full by -4.8 to +9.2% (Doom 774 to 845), so a change under 10% in passes ended full is noise.

## 1. Zba, Zbb and Zbs

`zb` against `base`. Instructions are in millions over the window; the deltas are in %.

| part | core 0 (M instructions) | core 0 (%) | all cores (%) | passes (%) | passes ended full, core 0 | stores, core 0 (%) | new texels, core 0 (%) |
|---|---|---|---|---|---|---|---|
| Doom timedemo | 147.8 | -3.07 | -3.84 | -1.75 | 774 to 1,063 | -0.01 | -0.73 |
| Quake timedemo | 94.7 | -2.68 | -2.96 | -1.84 | 2,808 to 2,821 | -0.67 | -0.30 |
| Tiberian Dawn | 140.2 | -3.81 | -4.89 | -3.53 | 470 to 473 | -1.76 | -1.36 |
| nxray, 8 pictures | 1.8 | -1.35 | -0.18 | -0.56 | 98 to 96 | -0.51 | -0.13 |
| qjs | 3.3 | -2.71 | -2.71 | -3.26 | 46 to 42 | -0.57 | -0.85 |
| cc (tcc) | 21.2 | -3.29 | -3.29 | -3.46 | 249 to 249 | -1.17 | -2.32 |

- **Where it comes from.** The count model puts Zba (`sh1add` to `sh3add`) at 0.3 to 3.9% of a class's instructions,
  and Zbb at 1.1% or less (`orc.b` included). Zbs stays under 0.1%.
  - The measured cut is larger than Zba and Zbb's own count. musl and the programs were built with them, and GCC
    then picks other code around them: `andn`, `min`/`max`, `rev8` and `zext.h`.
  - The text shrinks 1.0 to 1.6% (Doom 783,103 bytes to 773,023; Quake III 3,021,119 to 2,982,547).
- **Doom's passes ended full.** They rose from 774 to 1,063 while all its passes fell by 428. The same stores fit into
  fewer passes, so more of the passes reach the write cache's limit. That is a gain, not a loss: the window is 1.75%
  fewer passes. The layout control moved the same count by 9%.
- **On the tick.** `ZB` (`zb_exec`, Zba and Zbb) is already in `emu.h`, behind the `K_ALU` block's `[branch] if (!ok)`.
  - Zbs and Zicond were added for this study as `ZB_X` (`build/isa/alt/rvc_opt_zb`, about 20 lines).
  - Without `ZB` on the tick, every one of these instructions takes the general path. That makes the zb build
    36% slower modelled (Doom's core 0: 35,569 ms to 48,267).
  - With `ZB` on, the model gives core 0 -1.5% (Doom), -1.6% (Quake), -2.6% (Tiberian Dawn), -1.6% (qjs) and
    -2.4% (cc).
  - The card's earlier run (`docs/system/nommu.md`, "Speed on the card", 16 October) put Zba and Zbb (programs only,
    not musl) within its 1-3% noise: +2.0, +2.6 and +2.3 points on Doom, Quake and Quake III, and -2.3 and -1.9 on
    nxray and Red Alert. That run had one run a cell.
- **Needs.**
  - `toolchain.sh` `ARCH_FLAGS` and the programs' `-march`, with musl built again.
  - OpenTTD's and Half-Life's own toolchains take the same flag.
  - The tick defines `ZB` (and `ZB_X` for Zbs), as does Unity's copy.
  - `rvc_cpu` takes `RVC_ISA=zb,zbs` (7bdafb7). Its default fast table would need the Zb kinds as fast handlers for
    speed. Today they run on its general step, so `rvc_cpu` is slower on such a build than on today's, not faster.
  - The kernel needs nothing: user code only.
  - tcc's code generator emits RV32IMAF and is unaffected. The archives it links (libc, libtcc1) are GCC's, and GNU ld
    merges `zba`/`zbb` attributes with plain ones.

## 2. `fmadd` on the fast path, then contraction

`fma` against `base` (`-ffp-contract=fast`, everything else the same):

| part | core 0 (%) | all cores (%) | passes (%) | stores, all (%) | new texels, workers (%) |
|---|---|---|---|---|---|
| Quake timedemo | -1.16 | -1.23 | -0.65 | +0.05 | -0.13 |
| nxray, 8 pictures | -1.67 | -9.12 | -4.48 | +2.1 | -5.07 |
| Doom, Tiberian Dawn, qjs, cc | 0.00 to -0.07 | 0.00 to +0.01 | 0 | 0 | 0 |

- **On the tick today.** The four `fmadd` opcodes take the general path, which makes a product and a sum, each
  rounded. Built with contraction and run on today's tick, nxray's workers would be 2.2x slower in the model (45,583
  ms to 98,665) and Quake's core 0 12% slower. Contraction comes only with the fast case.
- **The prototype.** `build/isa/alt/rvc_opt_fma` adds one `case 0x43: case 0x47: case 0x4b: case 0x4f:` to
  `fast_exec_l1`'s last `switch (opc)`. It makes the general path's product and sum, single precision only.
  - With it the model gives nxray's workers -4.0% and Quake's core 0 -0.6% against base.
  - The card's cost of the case is not measured yet (below).
- **The floats change.** nxray's sums differ, as above. Every float program's sum in `test_all.py` is set again. A port
  that needs an exact sum keeps `-ffp-contract=off` on that function (Half-Life's `se_sum_exact`,
  `Mod_PointVecsProduct`).
  - SM5 has no single-precision fused multiply-add, so the card's `fmadd` is the two roundings `rvc_cpu`'s `fmaf` is
    not. That is already so on the general path; the rule for floats is "close enough".
- **Needs.** The tick's case (and Unity's copy), the flag out of the builds, and the sums. Nothing in the kernel.

## 3. Zicond

GCC 13.3 (Buildroot's) accepts `_zicond` but makes no `czero`. Clang 21 does make it. The count model
bounds what it could replace (a branch over one move, or `and` with a mask from `slt`/`seqz`):

- 0.03 to 0.16% of core 0's instructions (Doom 0.09, Quake 0.06, Tiberian Dawn 0.03, cc 0.16);
- 0.4 to 0.8% on Quake's and OpenTTD's workers.

`rvc_cpu` runs it (`RVC_ISA=zicond`), and the tick prototype has it under `ZB_X`. It is not worth a compiler change
now. It comes for free when the toolchain is GCC 14 or clang.

## 4. Zfinx

`zfinx` against `base` (`-march=rv32ima_zfinx`; `rvc_cpu` with `RVC_ISA=zfinx`, user mode only, the kernel keeping its
float registers):

| part | core 0 (%) | all cores (%) | passes (%) | stores, workers (%) | new texels, workers (%) |
|---|---|---|---|---|---|
| Quake timedemo | -1.38 | -0.37 | -0.64 | +1.96 | -0.72 |
| nxray, 8 pictures | +0.57 | +11.42 | +3.92 | +177 | +61 |
| Doom, Tiberian Dawn, qjs | -0.03 to +0.03 | -0.15 to +0.01 | -0.26 to +0.01 | -1.6 to 0 | 0 |

- **It saves little.** The `fmv` it saves are 0.4 to 4.5% of the float programs' instructions in the model. Measured,
  Quake's core 0 is -1.4%.
- **It costs the programs that use floats most.** nxray's workers spill: 31 registers for integers and floats
  together. Their stores are 2.8x and their new texels 1.6x, the very thing that ends passes early.
- **The tick's side would be small.**
  - The per-pass fixed cost of the 8 float state texels (`fp_state_load`) is about 0.03%.
  - `xr` would shrink from 65 to 33 words. A change to the local arrays moves the tick's speed level unpredictably by a
    few %.
  - The float ops could take the integer reads already made.
  - None of this can be timed with `RVC_TICK_ALT`: the base programs mean other things under it.
- **What it needs.**
  - Every library and program built again. GNU ld refuses to link a zfinx object with an F one ("`zfinx' is conflict
    with the `f/d/q/zfh/zfhmin' extension"), so nothing prebuilt can be mixed in.
  - musl's riscv32 float asm (`src/math/riscv32/*f.c`) and `linux/userland/libm/flm_libm.h` take `"r"` for `"f"`.
  - tcc needs a zfinx backend. Its own float code is F, and its runtime links F objects. That is why cc did not run
    in this study: the image's `cc` stopped on an illegal instruction.
  - The kernel would lose its float context switch (no `CONFIG_FPU`).
  - The gain the plan expected (on a bundled ISA, 64 to 32 registers cost 0.5%) does not hold here. That ISA went
    from 64 to 32; Zfinx goes from 64 (32 + 32) to 32.

## 5. Compressed instructions (C, Zcb)

Fetch is not the limit:

- The tick reads 0.40 to 0.52 instruction texels per instruction (`tick-costs.md`, the shell loop, Doom and Quake). The
  read for the next texel goes out ahead and mostly overlaps.
- Fetch is 18-21% of the bytecode operations per instruction, and "all of fetch and decode for nothing would be some 25
  ns of 220 to 270".
- C would make the code about a quarter smaller and take perhaps 0.1 of those texel reads away. But every instruction
  would pass one more test (16 to 19 ns a test passed, `tick-costs.md`) and an expansion.
- Four-byte instructions would lie on two-byte boundaries, some across two texels. The window would turn by
  halfwords.
- A bundled ISA on the same card measured the same trade, smaller code against dearer steps. Its E2 and E1 encodings
  (code 53-56% and 44-48% of E4's) ran half and a third of E4's work a second.

Not recommended. The ROM's size is not a limit worth that.

## 6. Load and store pairs

In the model, two word stores to adjacent addresses from one base, one after the other, are:

- 15.7% of Doom's core 0 stores;
- 22.5% of Quake's;
- 34% of nxray's core 0 stores.

Loads pair less often. As instructions, store pairs are 1.9% of Doom's core 0 (2.97 M of 156.6 M) and 3.7% of Quake's.

- **No gain for VR.** A pair writes one texel either way, so the write cache's entries, and the passes that end on
  them, do not change.
- **A bundled ISA already tried it.** It built `ld2`/`sd2` with a post-RA pass in LLVM. Core 0's bundles fell 0.7%, the
  image ran at 0.99x, and its model had overrated the change 3 to 4 times. It was rejected.
- **No compiler makes them.** GCC and LLVM make Zilsd's `ld`/`sd` only for 64-bit values, never for two `int` stores,
  so a compiler pass would be needed.

Not recommended.

## 7. Custom instructions

- **A 64/32 divide step.** 64-bit divides are large in a few parts of the profile (`riscv-profile.md`):
  - `__udivdi3` is 33.6% of `video-mpg`, called from `media_video_time` 43,604 times;
  - `__divdi3` and `__udivmoddi4` are 17% of nxview's `fit_rows`;
  - `__udivdi3` and `__umoddi3` are 9.8% of qjs's bigints;
  - `__divdi3` is 3.2% of Doom's level loads.

  Each is a divide made again and again by a value that does not change, or one that could be made in 32 bits. The
  profile's fixes remove them outright: the hoisted `media_video_time`, steps made once in `fit_rows`, qjs dividing by
  10^9 in 32 bits, and bp-progs' `mp_divlu`. A divide-step instruction would only make them some 2 to 4 times cheaper,
  through a new kind on the tick, so the sources come first.
- **The copy or fill as an instruction.** Today a copy is a write to the `0x8b0` CSR alias, which already leaves the
  fast loop for the general path and ends the pass. An instruction would save about 3 general steps, some 8 µs, on a
  copy of 2 KB or more. That is far under 0.1% of any part.
- **A cheaper `mcw_rest`.** Its 322 instructions a call are the ioctl's path through the kernel and the idle loop's
  work, not an instruction's cost (`docs/machine/multicore.md`, "A core that rests"). It belongs to the kernel and
  userland: a direct entry like a vDSO, or a lighter ioctl.
- **A bundled ISA's E4 wins.** Per instruction, E4 runs arithmetic 2.40x, loads 1.44x and stores 1.31x as fast as lean
  RISC-V, but games carried by core 0 only 1.1-1.2x.
  - Its measurements say why: a step's cost is its fetch, decode and control chain, nearly the same for two ops as
    for one. The win is the bundle: two ops a step.
  - RISC-V can have a little of it with fused custom opcodes that are one fast kind each. The model's bounds on what
    they would take away, as % of a class's instructions:
    - `add` and a load or store (an indexed access, like XTheadMemIdx): 1 to 8% (Doom core 0 1.9, Quake 2.1,
      Tiberian Dawn 2.1; workers up to 5.6);
    - `li` and a branch (compare with an immediate and branch): 0.5 to 2%;
    - `addi` and a branch (a counted loop): up to 4%;
    - `auipc` and its use: 1 to 6.6%. These are PIE's GOT loads, which belong to the linker (non-PIE or relaxed to
      `gp`), not the ISA.
  - Each one is a new kind in `fast_exec_l1`'s tree, and changes to that tree have cost 1-3% of every instruction.
    Each also needs a compiler pass and an assembler. The indexed access is the only one whose bound clearly exceeds
    that risk. It is left as a later study, after Zb and `fmadd`, with the card measuring the tree before and after.

## The tick, prototyped

Three variants, copies of `experiments/rvc_opt` made by the study's script (scratchpad `tickvar.py`):

- `build/isa/alt/rvc_opt_zb` (`ZB`, `ZB_X`);
- `build/isa/alt/rvc_opt_fma` (`FAST_FMA`);
- `build/isa/alt/rvc_opt_zbfma` (both).

None of them has been compiled or run yet. The WARP check and the card's timing both wait until the before/after
comparison frees the harness lock. Owed, in order:

1. `python tools/test_card.py fptest mctest-3 lanetest --warp --mark-ok --rvc build/isa/alt/rvc_opt_zbfma --image
   build/images/isa-zb`, then the same with `--image build/images/isa-fma`. Use `--parts`, not `--max-wall 0`.
2. `tools/cso_loops.py` on the compiled blobs against the tree's: the fast loop's length and registers.
3. `RVC_TICK_ALT` with the tree's tick and each variant in turn, on the shell loop and Doom's timedemo on the base
   image: the cost to programs that use none of it. Then the same with the zb and fma images on Doom, Quake and nxray:
   the gain.

## The net estimate

- **The harness, flat out.** This is fps under `rvc_cpu`'s pass model, with the tick's per-instruction cost taken as
  unchanged.
  - Zb: +1.8 to +3.7% (Doom +1.8, Quake +1.9, Tiberian Dawn +3.7, qjs +3.4, cc +3.6).
  - `fmadd`: Quake +0.7, nxray +4.7, the integer programs 0.
  - Together: Quake about +2.5%, nxray +5%.
  - These are the gains if `ZB` and the `fmadd` case cost the tick nothing. The card's earlier run of `ZB` saw a level
    within its noise, which is about the gain's own size.
- **VR.** Core 0 per round is what counts; a round ends at the write cache's 336 texels or the budget.
  - New texels fall a little (Doom -0.7%, Tiberian Dawn -1.4%, cc -2.3%), never rise.
  - Core 0's passes ended full are 3% (Doom) to 18% (Quake) of its passes.
  - So most rounds end on the budget, and the instructions saved carry over: about +2 to +3.5% of core 0's work a
    round with Zb, and a further 0.5 to 1% with `fmadd` in float programs.
  - Zfinx would lose here as well: nxray's workers' new texels were +61%.
- **Neither is a 2x lever.** Together they are worth a few percent, and the card has to show that `ZB` and the
  `fmadd` case cost the fast loop less than that.

## The card's timing (28 October 2026): not taken

The three variants' own changes (`ZB`/`ZB_X`, `FAST_FMA`; 31 to 33 lines of `main.shader` and `src/emu.h`) were put on
the backport's tick (the after machine's, RAM in one piece) in `build/isa2/rvc_opt_{zb,fma,zbfma}`. Each passed
`mctest-3` on WARP (`tools/test_card.py mctest-3 --warp --parts --max-wall 0`, 174 to 182 s), the case the tree's own
stamp holds. `lanetest` and `fptest` do not finish on WARP in parts with the tree's tick either (no line in 280 s), so
they say nothing of the variants.

`RVC_TICK_ALT` with the tree's tick and the three, a tick picked every 4 frames, every frame timed, on the after image
(whose programs use neither extension), 40 s each:

| workload | tree (ms a frame's tick) | zb | fma | zbfma |
|---|---|---|---|---|
| a shell loop (8,193 instructions a frame) | 1.4266 | 1.4426 (+1.1%) | 1.4317 (+0.4%) | 1.4505 (+1.7%) |
| Doom's timedemo (2,680 a frame) | 0.5823 | 0.5987 (+2.8%) | 0.6029 (+3.5%) | 0.5976 (+2.6%) |

Every program not built for them pays 1 to 3.5% for the decode, as the tick's other changes to the fast loop have
(AGENTS.md); Doom pays more (2.8%) than Zb was estimated to give it (+1.8%, at no cost). By the rule for a default
switch (no game slower) none is taken. Zb would need a decode that costs the fast loop nothing: the instructions
taken where the tree already falls out of the fast path (the general step), so that only programs that use them pay.
That is the next thing to try; `rvc_cpu`'s `RVC_ISA` counts stay as the measure of what they would save.

## What is left

- Zb on the general step only (above), measured as here.
- These builds were based on 7624f55. `riscv-backport` is now at c135015 (RAM in one piece). The deltas are build
  against build, so they should hold, but the absolute counts are 7624f55's.
