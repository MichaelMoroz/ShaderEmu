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

## Against DXC on D3D12

D3D12 with DXC was the fast backend: 16% more instructions a second on the Linux bench and a
cold boot 1.5 s shorter. D3D11 with fxc2 is level with it or ahead now (8 October 2026, same
session for each row, `--fixed-dt 0.004`; the state hash and the instruction count are the same
on both backends in every row that prints them):

| | D3D11 + fxc2 before | D3D11 + fxc2 | D3D12 + DXC |
|---|---|---|---|
| fixed cost of a frame (`--ticks 2`) | 0.37 ms | 0.083 ms | 0.091 ms |
| Linux bench, 2,048 instructions a frame | 2,759k IPS | 3,280k IPS | 3,256k IPS |
| the same at 16,384 | 3,187k IPS | 3,660k IPS | 3,651k IPS |
| the same at 65,536 | | 3,440k IPS | 3,432k IPS |
| Linux cold boot, 21,000 frames | 16.1 s | 13.74 s | 14.24 s |
| raytracer, 40,000 frames (machine mode) | | 4,198k IPS | 4,046k IPS |
| gears, 12,000 frames (the GPU device) | | 3,786k IPS | 3,566k IPS |
| raycast's demo walk, 16,384 a frame | | 2,143k IPS | 2,099k IPS |

Four things did it:

- The D3D11 harness commits in bands, as D3D12 did (`COMMIT_BANDS`, one quad per 4 MiB band
  something wrote to). Rewriting all of RAM was 0.28 ms of every frame. `--no-bands` turns it
  off on either backend; hashes taken without bands need it.
- The fast loop leaves from where a thing is found out (item 25 in
  `experiments/rvc_opt/README.md`): 0.444 ms to 0.365 ms for the raytracer's tick pass.
- fxc2 puts the code behind a test of a flag where the flag was set, when both sides of the
  branch before it leave a constant there: what is left of `if (!step()) break;` is one test.
- The D3D11 harness flushes at the end of a frame. Before, the GPU was handed the frame when
  the readback's `Map` asked for it, after the host's own work: 0.02 ms a frame of a GPU with
  nothing to do, 7% at 2,048 instructions a frame. `RVC11_NO_FLUSH=1` brings that back.

The tick pass itself is still slower through D3D11. The harness can hand fxc2's bytecode to
D3D12 (`set RVC12_DXBC=1` with `--dxc`: every shader is then compiled by
`d3dcompiler_47.dll`): there the raytracer's tick takes 0.331 ms, DXC's DXIL 0.348, and the
same bytecode on D3D11 0.355 to 0.365. D3D11 makes that up in the commit (0.038 ms against
0.077).

At 65,536 the two backends end in different states: the D3D11 build computes the timer value
in the shader with doubles, the D3D12 build takes it from the host. With `--no-doubles` on
D3D11 the hashes are equal there too.

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
