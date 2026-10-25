# One core a wave (25 October 2026)

**The rule.** Every wave (an NVIDIA warp, an AMD wavefront) of the tick's draw holds the lanes of one core only. A tick
is valid only together with a geometry (the block, the draw, the harness's and Unity's) under which this holds on the
card. A tick that has not been measured against it is not valid yet.

**Why.** A core's lanes run the same program on the same state, so they never diverge: the block's pixels differ only
in which eight state texels they write at the end. Lanes of two cores run two programs. Wherever the cores' paths
differ, the wave runs both: its fast loop runs as long as its slowest core, and a general step that one core takes is
paid by every lane. No compiler or ISA change can make two programs agree, so the geometry must keep them apart.

## Measured (25 October 2026, `gpu_vm/tick/probes/waves.cpp`)

`bin\waves.exe` (`build_waves.bat`; through `tools\gpu_queue.ps1`, `--warp` first) draws 256 cores each way. A map
shader appends a record for every invocation (its wave from an atomic counter, pixel, core, lane), and a timing shader
runs 20,000 steps of a hash in every core's pixels (the median of 7 draws). The RTX 5070 Laptop runs pixel waves of 32
lanes. The timing's alternating column (odd cores long, even short) is left out: at 256 cores every wave runs at once,
so a draw lasts as long as its longest wave either way.

| how the cores are drawn | waves | waves mixing cores, % | cores a wave | live lanes a wave | invocations a useful pixel | time, ms |
|---|---|---|---|---|---|---|
| c8 (8 x 6 in cells 8 x 16), a triangle a core (today) | 894 | 0 | 0.72 | 27.4 | 2.00 | 0.229 |
| c8, a triangle a core cut to the block by clip distances | 512 | 0 | 1.00 | 24.0 | 1.00 | 0.186 |
| c8, a quad a core | 512 | 0 | 1.00 | 24.0 | 1.00 | 0.187 |
| c8, one triangle over all, padding discarded | 1,024 | 0 | 0.50 | 32.0 | 2.67 | 0.405 |
| 4 x 8, one triangle over all | 256 | 0 | 1.00 | 32.0 | 1.00 | 0.203 |
| 4 x 16, one triangle over all | 512 | 0 | 1.00 | 32.0 | 1.00 | 0.222 |
| 8 x 8, one triangle over all | 512 | 0 | 1.00 | 32.0 | 1.00 | 0.207 |
| 8 x 16, one triangle over all | 1,024 | 0 | 1.00 | 32.0 | 1.00 | 0.445 |
| 8 x 4, one triangle over all | 256 | 100 | 2.00 | 32.0 | 1.00 | 0.112 |
| 16 x 2, one triangle over all | 256 | 100 | 4.00 | 32.0 | 1.00 | 0.129 |
| 8 x 6, one triangle over all | 384 | 66.7 | 1.67 | 32.0 | 1.00 | 0.158 |
| 4 x 8 side by side, a quad a core | 384 | 33.3 | 1.33 | 21.3 | 1.00 | 0.147 |
| 8 x 8 side by side, a quad a core | 640 | 20.0 | 1.20 | 25.6 | 1.00 | 0.261 |
| 4 x 8, one quad over each row of 256 cores | 288 | 66.7 | 1.89 | 28.4 | 1.00 | 0.174 |
| 8 x 8, one quad over each row of 128 cores | 576 | 22.2 | 1.22 | 28.4 | 1.00 | 0.217 |
| 4 x 8 alone in a 64 x 64 cell, a quad a core | 512 | 0 | 1.00 | 16.0 | 1.00 | 0.194 |
| 4 x 8 alone in a 64 x 64 cell, a clipped triangle a core | 512 | 0 | 1.00 | 16.0 | 1.00 | 0.202 |
| 8 x 4 alone in a 64 x 64 cell, a quad a core | 512 | 0 | 1.00 | 16.0 | 1.00 | 0.186 |
| 16 x 2 alone in a 64 x 16 cell, a quad a core | 512 | 0 | 1.00 | 16.0 | 1.00 | 0.192 |
| 32 x 1 alone in a 64 x 8 cell, a quad a core | 512 | 0 | 1.00 | 16.0 | 1.00 | 0.186 |

The full table is in `build\waves_card.txt`. What it shows:

- **Over one big triangle, a warp is a 4 x 8 pixel tile** (2 x 4 quads). A block that is a whole number of those tiles,
  on their grid (4 x 8, 4 x 16, 8 x 8, 8 x 16), never shares a warp and fills every lane.
- **Any primitive edge inside the cores' area mixes them.** One quad over a row of 4 x 8 cores mixed 67% of its waves,
  because its diagonal crosses the tiles. Side-by-side quads, one a core, mixed 20-33%.
- **A core as a primitive of its own, far from the next, never mixes, but its waves hold 16 lanes**, whatever the
  shape or the way it is drawn: a 32-pixel core is two half waves. At 256 cores they cost about what full waves cost
  (0.19-0.20 ms against 0.20). A tick that fills the card with warps would pay for them.
- **Rows 1 pixel tall** run half their lanes as helpers (16 live lanes a wave for 32 x 1 and 64 x 1).
- **Today's tick keeps the rule on this card**, but its triangle a core launches twice the pixels it uses, the spill
  past the block that it then discards. Cutting the triangle to the block with two clip distances (`SV_ClipDistance`,
  which Unity has) launches only the block, 0.186 ms against 0.229 in the probe. That is a change to try on c8 now.

**Padding inside one big triangle, and wider quads** (the second round, same probe):

| how the cores are drawn | waves | mixing, % | live lanes a wave | invocations a useful pixel | time, ms |
|---|---|---|---|---|---|
| 4 x 8 side by side, one triangle (no padding) | 256 | 0 | 32.0 | 1.00 | 0.203 |
| 4 x 8 in 8 x 8 cells, one triangle, padding discarded | 512 | 0 | 32.0 | 2.00 | 0.223 |
| 4 x 8 in 8 x 8 cells, one triangle, padding returns | 512 | 0 | 32.0 | 2.00 | 0.221 |
| 4 x 8 in 8 x 16 cells, one triangle | 1,024 | 0 | 32.0 | 4.00 | 0.442 |
| 4 x 8 in 16 x 16 cells, one triangle | 2,048 | 0 | 32.0 | 8.00 | 0.384 |
| 4 x 8 in 32 x 32 cells, one triangle | 8,192 | 0 | 32.0 | 32.0 | 0.554 |
| 4 x 8 in 64 x 64 cells, one triangle | 32,768 | 0 | 32.0 | 128 | 1.451 |
| 64 x 2 in 128 x 16 cells, a quad a core | 1,536 | 0 | 21.3 | 1.00 | 0.617 |
| 32 x 4 in 64 x 16 cells, a quad a core | 1,536 | 0 | 21.3 | 1.00 | 0.615 |
| 16 x 16 in 64 x 64 cells, a quad a core | 2,304 | 0 | 28.4 | 1.00 | 0.845 |
| 32 x 8 in 64 x 64 cells, a quad a core | 2,560 | 0 | 25.6 | 1.00 | 0.968 |

- Padding between tile-aligned cores keeps every core's warp full and separate. But padding is not free: every wave of
  padding is launched and leaves at once, and a draw costs about as many waves as it launches. Twice the pixels cost
  10%, 128 times cost 7x.
- No shape of a primitive a core fills its warps on NVIDIA, however wide (21-28 live lanes of 32).
- Other cards, from their public material (not measured here):
  - AMD RDNA runs pixel shaders in waves of 64 or 32, chosen by the compiler, usually 64 ([RDNA architecture,
    GPUOpen](https://gpuopen.com/download/RDNA_Architecture_public.pdf)).
  - Intel packs pixels of several polygons into one thread on purpose ("multi-polygon" dispatch: 2 polygons a thread on
    Gen12, SIMD4x8 and SIMD2x16 on Xe2; [Mesa MR 26585](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/26585),
    [Phoronix](https://www.phoronix.com/news/Intel-Dual-SIMD8-Mesa)).
  - Patents describe packers that merge partly covered quads of different primitives into one
    ([US11798218](https://patents.google.com/patent/US11798218), [US11748933](https://patents.google.com/patent/US11748933)).
  - So a primitive a core is never safe across vendors: packing several primitives into one wave is what these GPUs are
    built to do. One big triangle has no primitive edges inside it, so what remains is the wave's tile. A block of 8 x 8,
    on an 8 x 8 grid, is two whole NVIDIA warps and as large as a wave of 64.

**The geometry for the new tick (decided 25 October 2026):** a core is an 8 x 8 block (64 pixels: two whole NVIDIA
warps, one AMD wave of 64), and the cores lie in one row of blocks: 256 cores make a target 2,048 x 8, drawn by one
triangle. With one row, no wave can take pixels of two rows of cores, whatever a card's tile is. A core with nothing to
do still launches its waves and leaves at the top (measured below: not culled).

**Idle cores in that geometry** (`bin\waves.exe --only idle`, 25 October 2026, RTX 5070 Laptop). A busy core's pixels run
20,000 steps of a hash, an idle core's return at the top. Each draw is its own submission, waited for: drawn back to back
the draws overlapped, and a small draw's timestamps did not wait for its work (1 busy core read 0.001 ms).

| busy cores of 256 | how drawn | waves | mixing, % | live lanes a wave | time, ms |
|---|---|---|---|---|---|
| all | one triangle | 512 | 0 | 32.0 | 0.342 |
| 128, a run | one triangle | 512 | 0 | 32.0 | 0.225 |
| 128, a run | one quad over the run | 288 | 22.2 | 28.4 | 0.234 |
| 128, a run | a quad a busy core | 320 | 20.0 | 25.6 | 0.234 |
| 128, every 2nd | one triangle | 512 | 0 | 32.0 | 0.332 |
| 128, every 2nd | a quad a busy core | 384 | 0 | 21.3 | 0.250 |
| 32, a run | one triangle | 512 | 0 | 32.0 | 0.173 |
| 32, a run | one quad over the run | 72 | 22.2 | 28.4 | 0.186 |
| 32, every 8th | one triangle | 512 | 0 | 32.0 | 0.229 |
| 32, every 8th | a quad a busy core | 96 | 0 | 21.3 | 0.172 |
| 5, a run | one triangle | 512 | 0 | 32.0 | 0.173 |
| 5, a run | one quad over the run | 13 | 15.4 | 24.6 | 0.187 |
| 1 | one triangle | 512 | 0 | 32.0 | 0.173 |
| 1 | one quad over it | 3 | 0 | 21.3 | 0.172 |

At 2,000 steps the same order: all 0.044, 1 busy 0.020 either way, 128 every 2nd 0.043 against 0.027.

- Culling by geometry breaks the rule. A quad's diagonal edge crosses the blocks it covers, so a quad over a run of
  cores mixes 15-22% of its waves, and quads of neighbouring cores mix 20%. Only quads of cores apart keep 0%, at 21 live
  lanes of 32.
- One triangle keeps 0% in every pattern. An idle core's waves launched and left cost nothing measurable while the busy
  cores lie together (1, 5, 32 and 128 in a run: 0.173 to 0.225 against 0.172 to 0.234 culled).
- Busy cores scattered among idle ones cost up to a third more than culled ones (every 8th 0.229 against 0.172, every 2nd
  0.332 against 0.250): a run of 128 costs 0.225, the same 128 spread over the row 0.332, as much as all 256.
- So the new tick draws one triangle and culls nothing, and the kernel gives out worker cores packed from core 1 up,
  not spread.
- An idle core that copies its state (a load a pixel, as from the state's other texture: "idle copy") costs what one that
  returns a constant does: 1 busy 0.172 against 0.173, 32 in a run 0.173 against 0.172, every 8th 0.235 against 0.235,
  128 in a run 0.236 against 0.236 (a second run on the card, in which all 256 read 0.408: the clock moves between runs,
  so compare within one). So the state can stay in two textures with no copy draw (c8's vert_copy and frag_copy).

## Where today's tick stood before the probe

c8_v0001 had not been measured, and it looked as if it broke the rule:

- A worker's block is 8 x 6 pixels, which is 48 pixels or 12 quads. Its 363 state texels at eight a pixel need 46
  pixels: the 258-entry write cache sets the block's size.
- An NVIDIA pixel warp is 32 lanes, which is 8 quads, and 12 quads is not a multiple of 8.
- c8 draws each core as a triangle of its own (`vert_cull`), 16 x 12 pixels over the block. Its pixels outside the
  block are discarded (`i.cc != core`), so they are launched for nothing. Whether the card packs quads of two
  triangles into one warp is not known.
- IDEAS 23 fits this but does not prove it: blocks of 8 x 4 (one warp a core) gave 1.6-1.7x the throughput of 8 x 8 at
  255 cores.

## How to see which pixel is in which wave

| method | what it gives | what it cannot give |
|---|---|---|
| A D3D12 probe with Shader Model 6 wave intrinsics: the tick's geometry, and a pixel shader that writes `WaveGetLaneIndex()`, `WaveReadLaneFirst(pixel id)`, `WaveActiveCountBits(true)` and `WaveActiveAllEqual(core)` (`dxc` is in the Windows SDK; the harness has a D3D12 backend) | the map of pixel to wave, and for every wave how many cores and how many live lanes it holds | D3D11's packing directly. The rasterizer and the packing of quads into warps are the hardware's, so the same geometry in D3D11 should pack the same way; method 3 checks that |
| Nsight Graphics (2026.3.1 is installed), Shader Profiler on the D3D11 tick | instructions executed per warp and per thread for each line; their ratio is the live lanes per warp instruction, which is the divergence actually paid | which pixels share a warp. Whether this version exposes the thread-level count for D3D11 pixel shaders is still to be checked |
| A D3D11 timing probe: a loop whose length depends on the core, long and short cores alternating | whether short cores pay for long ones (waves mixing cores), on the real API and on any card | the map |

## What the probe must answer

Each case gets one table: the share of waves that mix cores (%), lanes launched for each useful lane, and the pass's
time for a fixed loop (ms).

- **Draws:** one triangle over all cores; one triangle a core (today); one quad (two triangles) a core.
- **Blocks:** 8 x 4, 4 x 8, 16 x 2, 32 x 1, 8 x 8, 16 x 4, 32 x 2, 8 x 6 (today).
- **Padding:** culled by the geometry; rasterized and discarded; rasterized and returned at the top.
- **Thin textures:** cores in rows 1 or 2 pixels tall.

What the hardware should do, all of it to be confirmed:

- **1-pixel-tall rows.** A row 1 pixel tall launches whole 2 x 2 quads with their second row as helper lanes, so half
  of every wave is wasted. A row 2 pixels tall wastes nothing, and 16 x 2 is exactly one warp.
- **Padding inside a busy wave** costs a lane, not time: the wave runs for its busiest lane anyway.
- **A wave of nothing but padding** costs its launch. Commit 759f259 measured that launch as the cost of a parked
  core's pixels: discarding them (idlew 3) did not help, keeping them out of the geometry (idlew 4) did. Padding must
  not be rasterized.
- **Other vendors.** AMD can run pixel shaders in waves of 64 or 32, and Intel in 8, 16 or 32. We have only the NVIDIA
  card (RTX 5070 Laptop), so the rule is checked there; a geometry for VRChat's other cards is an open question.

## What the rule does to the write cache

A pixel writes eight texels (eight render targets), so a core of one wave has 256 state texels and a core of two
waves 512. The cache's size has to be chosen to fill whole waves, not the other way round. Today's 258 entries need
363 texels, which is 1.5 waves.
