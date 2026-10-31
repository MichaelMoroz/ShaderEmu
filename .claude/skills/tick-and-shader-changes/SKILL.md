---
name: tick-and-shader-changes
description: Change and measure the ShaderX86 tick and its shaders (experiments/rvc_opt, experiments/rvc_lean, Unity's copy, fxc2) - measuring a change whole and alternating, the three loads, RVC_TICK_ALT in one run, the shape the fast loop must keep, HLSL traps, the WARP stamp (warp_ok, --stamp, --mark-ok), rvc_cpu in step with the tick, bringing it to Unity, VRChat's constraints, and changing fxc2 itself. Use before editing tick code, main.shader, src/*.h, tick.hlsl, gpu.shader, a commit pass, or fxc2, or when asked what a tick change is worth. Triggers on: tick, fast loop, fast_exec_l1, fxc2, DXBC, warp_ok, stamp, RVC_TICK_ALT, shell loop and Doom, rvc_lean, MachineTickLean, Sync shader sources, cso_loops, tick_path.
---

# Changing the tick

Depth: `docs/machine/tick-costs.md` (costs and shape), `docs/machine/speed.md`, `docs/machine/fxc2.md`, `experiments/rvc_opt/README.md`, `experiments/rvc_lean/README.md`.

Limits: VRChat, D3D11 DXBC, no UAVs in the fragment shader, no texture arrays. fxc2 may change, as long as it still compiles the harness, Unity, and the ShaderEmu and Poiyomi shaders its README measures. The whole stack is ours, so fix a cost at the cheapest layer: often a guest's source, not the tick.

## 1. Which tick

- `experiments/rvc_opt`: the harness's and the test set's (`--rvc experiments\rvc_opt`).
- `experiments/rvc_lean`: the world's. `unity/ShaderEmu/Shaders/lean/tick.hlsl` is a verbatim copy of `tick.hlsl` (`diff -q` them). A folder whose name begins `rvc_lean` runs with the right defines, so `rvc_lean_try` works. It has the lanes; `rvc_opt` and `rvc_cpu` do not.
- A change of behaviour also goes into `harness/apps/rvc_cpu.cpp`, or the tests no longer test the machine. `rvc_layout.h` and `layout.h` hold the layout, one each for host and tick.

## 2. Before writing

- Think whether it can be anything else. Nearly every change to the tick's function, however far from the fast loop, has cost 1 to 3% of every instruction, and the tree sits at a level most changes leave. Taking code out can cost the same.
- A count is a guide, never a verdict: `python tools\cso_loops.py` (the eight-output blob in `build\shadercache`), `tools/tick_path.py`, `tools/dxbc_run.py`.
- Local arrays move each other: try such changes together. Layouts, caches and ISA are weighed together too (`AGENTS.md`, rules of the work).

## 3. The shape (from `tick-costs.md`)

- Kinds are a tree of tests in `if`s, no `switch`, no bools computed first (4% slower). A load has one way out. The loop is left by a `break` where a thing is found out, since a return value is a flag written, copied and tested.
- **HLSL evaluates both sides of `&&`, `||` and `?:`.** A call with an `out` or a side effect behind one runs always: every finished lane took itself for stuck; a loop wrote zeros over state nothing counted.
- A static set before a pass's loop and read after it is carried through every core's loop (`lane_skip`, 0.7% of Doom with no lane running): keep it in a word already carried, or read it again.
- `l1_occ` is a `uint4` by selects, not an array (3 to 5% of every load).
- A texture read used at once is the dearest thing on the path; one not used yet costs almost nothing. Fewer reads can be slower.
- "One core a wave": a primitive edge inside the cores' area mixes programs in a wave. Padding lanes are kept out of the geometry, not discarded.
- Both compilers: `ShareUnits.shader` must compile under fxc2 and Windows' FXC; a `continue` in a loop or a vector component written by index compiled under one only.

## 4. Check it runs, then stamp it

The card refuses a tick it has not seen in software: `rvc_harness` says so and exits 3 unless `warp_ok` in the processor's folder holds its sources' hash.

    bin\rvc_harness.exe --rvc experiments\rvc_opt --stamp
    python tools\test_card.py mctest-3 --warp --parts --max-wall 240 --mark-ok

WARP is about 300 thousand instructions a second, so `--parts` (45 s runs, resumed). The stamp is of the sources: a `--define` or `--lazy` is the same tick checked. Commit `warp_ok`'s change with its own message (`WARP stamp for ...: mctest-3 passed in 25 s parts`).

Then on the card: `python tools\test_card.py mctest-3 fptest` and a same-state check: a cold boot of the same frames under `--fixed-dt` under each build, same instructions and console.

## 5. Measure it whole

A change is judged on the **whole run**, on all three loads, alternating with the tree. They disagree: two SBI calls in the general path were level on one and 1.9% under on another.

| Load | Command |
|---|---|
| A shell loop | `sh -c 'i=0; while [ $i -lt 20000 ]; do i=$((i+1)); done'` |
| Doom's timedemo | `DOOM_VIEW=told doom -1 -timedemo demo1`, with `nano-X -p & sleep 2;` first |
| Quake at e1m1 | `quake +map e1m1`, same prefix |

Best: one run, both ticks, one clock.

    $env:RVC_TICK_ALT='D:\path\rvc_opt_try'
    pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --image linux-net --stats-after 5 --load-state SNAP

Each frame's tick is picked by a hash of the frame (`RVC_TICK_ALT_SHIFT=N`), and `STATS tick N DIR: X ms` says each. The same layout is required. Two copies of one tick gave 0.9548 and 0.9545 ms: that is the noise to beat. `--replay` runs each tick from one snapshot.

Otherwise A B A B, long runs from **one and the same snapshot**: two snapshots of "the same" work differ more than most changes, and a snapshot made by each build measures nothing. Reach it quickly with `--cpu ... --save-state`, then `--load-state`.

Rules of the bench:
- Calibrated figures say nothing about a tick change: the model is fitted to one tick.
- Warm the card; watch `nvidia-smi --query-gpu=clocks.gr`. The governor runs low for the first seconds.
- Look for `rvc_harness.exe` with no arguments, other agents' runs, and a kernel build on all cores.
- `RVC_FREEZE=1` with `--bench`, `--define X_P_ALU=8`, `RVC_TICK_SCISSOR`, `RVC_PASS_PROF=1`, `RVC_STATS_EVERY=1` and `RVC_MARK=TEXT` time parts. `tools\pass_costs.ps1` then `tools\pass_regress.py` fit costs to kinds.
- `--unity F,U[,R]` gives rounds as a world's frame, with `RVC_UNITY_PACE=world`.

Keep a change only if the three loads say so. Write "level" when it is level. `docs/machine/tick-costs.md` has the shape; add a line there for a rule, and `AGENTS.md` for one a later change could break.

## 6. Bring it to Unity

- Verbatim copy of what changed: `lean/tick.hlsl`; `MachineCommit.cginc` and `MachineBlit.cginc` for the commit and sizes, `CONTROL_BANDS`, `_CommitAlways`' counterpart. A layout change: `STATE_TEX_HEIGHT`, `kStateHeight`, Unity's StateA.
- In the editor: "ShaderEmu/Sync shader sources from the repository" and, with a new image, "ShaderEmu/Import boot images from the repository".
- Measure with the Game view in front (play mode started by a script leaves the Scene view, and the room is then drawn small), by `totalInstructions` over several seconds, a change and the tree in turn. Only two sides of one session compare. `docs/machine/multicore.md`, "In Unity".

## 7. Changing fxc2

The source is `C:\Users\micha\Documents\FXC2`; the DLL lives in `tools\fxc2\d3dcompiler_47.dll`, and the build copies it next to the harness. `src/fxc2_check.h` refuses a build older than patch 65; raise it when the tick comes to need a patch. After a change: compile the tick (`__FXC2__`), `python tools\test_card.py mctest-3 fptest`, and FXC2's `tools/shaderbench.py` with the shaders its README measures. Then fit the calibrated model again if the cost moved (`calibrated-measurements`).

To see what the driver made of a shader: the user-level skill `nsight-gpu-trace` (D3D12 only, DXBC by `dxilconv`).

## 8. Finish

`python tools/test_all.py` if `rvc_cpu` changed, `tools/test_card.py machine doom-3 nxpath-balls nxpaint`, `--parts` for `mctest-255`. Snapshots of a layout before are refused: make them again (`--snapshot`). A `RVC_ZB`/`RVC_ISA` idea goes through `tools/rv_compare` and `docs/plans/riscv-isa.md` first.
