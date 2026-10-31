# fxc2: the D3D11 build without FXC

[fxc2](https://github.com/MichaelMoroz/FXC2) is a replacement for Microsoft's HLSL compiler that
produces the same thing, D3D11 bytecode (DXBC), from vkd3d's HLSL compiler with a set of patches.
For this project it changes two things:

- The `CPUTick` pass compiles in seconds, where FXC takes minutes, so the shader can be tuned on
  D3D11, the backend VRChat runs.
- It compiles what FXC cannot: the big arrays as locals of the pass, which `experiments/rvc_opt`
  uses when it sees that compiler (`__FXC2__` is predefined by it). Under FXC the shader builds
  with static arrays, zeroed every pass.

## Using it with the harness

`rvc_harness.exe` loads `d3dcompiler_47.dll` the usual way, and the build copies fxc2's
(`tools\fxc2\d3dcompiler_47.dll`) next to it: the harness compiles with fxc2 (`shader_compiler.cpp`
tells which one it loaded by where it lies), and the shader cache keys a blob by the compiler that
made it. To compile with Microsoft's FXC, delete `bin\d3dcompiler_47.dll` (the next build puts it
back). The D3D12 path's preprocessing comes from the same DLL; the harness undefines `__FXC2__` for
it.

Unity (and so a VRChat world's build) can use it as well: FXC2's `scripts/unity-overlay.ps1` makes
a copy of the editor whose shader compiler is fxc2, without touching the installed one.

`src/fxc2_check.h` refuses a build of fxc2 older than patch 65 (`__FXC2_VERSION__`): a hull shader's
`return I[id]` before it was an indexable temp the card did not draw. To update the DLL, copy it from
a newer FXC2 checkout and run the check at the end of this page.

## What keeps a D3D11 frame cheap

- The harness commits in bands (`COMMIT_BANDS`, a quad per 4 MB band something wrote to), and
  what the cores stored is drawn as points, a texel each (`multicore.md`, "The commit as points").
  `--no-bands` has the commit rewrite all of RAM; hashes taken without bands need it.
- The fast loop leaves from where a thing is found out (item 25 in
  `experiments/rvc_opt/README.md`), and fxc2 puts the code behind a test of a flag where the flag
  was set, when both sides of the branch before it leave a constant there: what is left of
  `if (!step()) break;` is one test.
- The harness flushes at the end of a frame, so the GPU is not handed the frame only when the
  readback's `Map` asks for it. `RVC11_NO_FLUSH=1` leaves the flush out.

The harness can hand fxc2's bytecode to D3D12 (`set RVC12_DXBC=1` with `--dxc`: every shader is then
compiled by `d3dcompiler_47.dll`). The D3D11 build computes the timer value in the shader with
doubles, the D3D12 build takes it from the host, so at long frames the two backends end in different
states; with `--no-doubles` on D3D11 the hashes are equal.

## What the shader uses from it

| | in the shader | under FXC |
|---|---|---|
| `inout` arrays worked on in place | `L1_LOCAL`: `l1_cache`, `tlb2_tag`, `tlb2_pg` are locals of `frag` handed down as arguments (`src/l1_local.h`); `--define L1_STATIC` keeps them static under fxc2 too | static arrays, zeroed every pass. FXC fails on the local form: "can't unroll loops marked with loop attribute" |

fxc2 also has `mulhi(a, b)`, `umulExtended(a, b, hi, lo)` and `imulExtended` (both halves of the
product from one instruction), and struct member functions. The shader uses none of them:
`mulhu32()` and `mulhs32()` in `src/emu.h` are four multiplications and a carry chain under both
compilers, as a `umul` or `imul` is turned into an invalid SPIR-V module by the layers that run
Direct3D on Linux (`docs/world/linux.md`).

## Known differences and what to watch

- fxc2 always optimises; `#pragma skip_optimizations` does nothing.
- FXC2's `tools/shaderbench.py` times any set of captured shaders under both compilers
  without Unity; its README says how to capture them.
- `ShareUnits.shader` must compile under both: a `continue` in a loop, or a vector's component
  written by index in one, compiled under one and not the other.

## Checking a change against it

A build by fxc2 must end in the same emulated state as one by FXC or DXC, and a static and a
local-array build in the same state. The quick check is a cold boot of the same number of frames
with `--fixed-dt` under each: the same instruction count and the same console output. On the card,
`python tools/test_card.py mctest-3 fptest` holds the tick to the test set's values, and the
FXC2 README measures the ShaderEmu and Poiyomi shaders it must still compile.
