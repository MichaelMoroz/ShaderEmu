# One core a wave (25 October 2026)

**The rule.** Every wave (an NVIDIA warp, an AMD wavefront) of the tick's draw holds the lanes of one core only. A tick
is valid only together with a geometry (the block, the draw, the harness's and Unity's) under which this holds on the
card. A tick that has not been measured against it is not valid yet.

**Why.** A core's lanes run the same program on the same state, so they never diverge: the block's pixels differ only
in which eight state texels they write at the end. Lanes of two cores run two programs. Wherever the cores' paths
differ, the wave runs both: its fast loop runs as long as its slowest core, and a general step that one core takes is
paid by every lane. No compiler or ISA change can make two programs agree, so the geometry must keep them apart.

## Where today's tick stands

c8_v0001 has not been measured, and it probably breaks the rule:

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
