# fxc2: the D3D11 build without FXC

[fxc2](https://github.com/MichaelMoroz/FXC2) is a replacement for Microsoft's HLSL compiler that
produces the same thing, D3D11 bytecode (DXBC), from vkd3d's HLSL compiler with a set of patches.
For this project it changes two things:

- The `CPUTick` pass compiles in about 9 seconds instead of about 9 minutes, so the shader can be
  tuned on D3D11, the backend VRChat runs, and not only under DXC on D3D12.
- It compiles two things FXC cannot, and `experiments/rvc_opt` uses both when it sees that
  compiler (`__FXC2__` is predefined by it): the big arrays as locals of the pass, and MULH as one
  instruction. Under FXC the shader builds exactly as before.

## Using it with the harness

`rvc_harness.exe` loads `d3dcompiler_47.dll` the usual way, and the build copies fxc2's
(`tools\fxc2\d3dcompiler_47.dll`) next to it: the harness compiles with fxc2. Its banner says
so ("D3D11 + fxc2"), and the shader cache keys a blob by the compiler that made it. To compile
with Microsoft's FXC, delete `bin\d3dcompiler_47.dll` (the next build puts it back). The D3D12
path's preprocessing comes from the same DLL; the harness undefines `__FXC2__` for it.

Two things do not work with it yet: upstream rvc's own shader (a syntax error; our
`experiments\rvc_opt` is fine), and `float - uint`, which it computes wrongly (write the cast).

Unity (and so a VRChat world's build) can use it as well: FXC2's `scripts/unity-overlay.ps1` makes
a copy of the editor whose shader compiler is fxc2, without touching the installed one.

## What it measures

Same machine, same session, `--d3d11`, upstream's `linux` image (`--payload rvc\_Nix\rvc\data-net`),
fixed timestep. FXC builds the same source with static arrays and the partial-product MULH.

| | FXC | fxc2 |
|---|---|---|
| `CPUTick` compile time | 554 s | 9.3 s |
| 6,000-frame bench at 2,048 instructions a frame | 1,528k IPS | 1,970k IPS |
| fixed cost of a pass (a frame that emulates two instructions) | 0.714 ms | 0.382 ms |
| 1,500 frames at 16,384 instructions a frame | 2,604k IPS | 2,810k IPS |
| Linux cold boot, 21,000 frames at 2,048 | 30.2 s | 22.6 s |
| state hash of the bench; boot instruction count (41,545,138) and console output | | identical |

The difference is the fixed cost of a pass: zeroing the 1024-entry write cache and the two
768-entry TLB arrays took 0.4 ms of every pass (item 23 in `experiments/rvc_opt/README.md`). Per
emulated instruction the two builds are about equal, so the gain is largest where frames are
short: 28% at 2,048 instructions a frame, 8% at 16,384. The same source compiled by both (static
arrays under fxc2 too, `--define L1_STATIC`) runs within 1 to 2% of FXC's build.

The machine these were taken on drifts by several percent from one session to the next; compare
numbers from one session only.

## What the shader uses from it

| | in the shader | under FXC |
|---|---|---|
| `inout` arrays worked on in place | `L1_LOCAL`: `l1_cache`, `tlb2_tag`, `tlb2_pg` are locals of `frag` handed down as arguments (`src/l1_local.h`) | static arrays, zeroed every pass. FXC fails on the local form: "can't unroll loops marked with loop attribute" |
| `mulhi(a, b)` | `mulhu32()` and `mulhs32()` in `src/emu.h`: one `umul` or `imul` | four multiplications and a carry chain |

fxc2 also has `umulExtended(a, b, hi, lo)` and `imulExtended` (both halves of the product from
one instruction), and struct member functions; nothing here needs those yet.

## Known differences and what to watch

- Use a build of fxc2 from 8 October 2026 or later (FXC2 commit `c39dbd1`). Before that,
  `float - uint` was compiled wrongly (the unsigned value was negated before it was converted),
  which is what broke `Terminal.shader`'s `at - cell` in Unity.
- Upstream rvc's unmodified shader compiles with it since the same build (its
  `4294967296.0l` literal used to be a syntax error): 5 s instead of 405 s, and the same state
  hash after 6000 frames. It runs slower though, 351k against 583k instructions per second,
  and why is not known yet. `experiments/rvc_opt` does not have that gap.
- fxc2 always optimises; `#pragma skip_optimizations` does nothing.
- FXC2's `tools/shaderbench.py` times any set of captured shaders under both compilers
  without Unity; its README says how to capture them.

## Checking a change against it

`tools\perf_test.ps1` and the state hashes in this repository were taken with FXC or DXC. A build
by fxc2 must end in the same emulated state; what differs between a static and a local-array build
is nothing, and between this shader and one from before the per-page `sfence.vma` only the TLB's
bookkeeping texels. The quick check is the cold boot above: 41,545,138 instructions after 21,000
frames and the same console output.
