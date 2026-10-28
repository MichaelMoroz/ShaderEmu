// The commit's own code with RAM in one texture (experiments/rvc_opt/main.shader's Commit pass
// under RAM_INPLACE; src/ram.cginc, docs/machine/gpu.md "RAM in one texture"), for Machine.shader's
// Commit (the state rows), MachineGather.shader and MachineCommitPoints.shader (RAM). Include
// after UnityCG.cginc and MachineBlit.cginc; MachineGather.shader defines COMMIT_GATHER first.
float _Init, _InitRaw;
// MachineTick.shader's machine: its CSRs are fields of the state, with no table to commit
#define SBI_HLE
#define NO_PAGING

Texture2D<float4> _Data_RAM_R;
Texture2D<float4> _Data_RAM_G;
Texture2D<float4> _Data_RAM_B;
Texture2D<float4> _Data_RAM_A;
Texture2D<float4> _Data_RAM_RAW;

Texture2D<float4> _Data_ROM;   // the ROM: a 16-byte texel a pixel, read by its bits (asuint)

// The state after the tick is the tick's texture (all 64 rows), and RAM is read by the gather
// alone; the other passes see the control words as the stage keeps them.
Texture2D<uint4> _Stage;
#ifdef COMMIT_GATHER
Texture2D<uint4> _Ram;
#define RAM_TEX(pos) (_Ram[uint2(pos)])
#else
uint4 ram_stage_read(uint2 pos);
#define RAM_TEX(pos) ram_stage_read(uint2(pos))
#endif

// A core's state as the tick left it (docs/machine/multicore.md): core 0's block, or a
// worker's strip of tiles in the band, as the geometry says.
static uint2 state_off = uint2(0, 0);
static uint mc_core = 0;
static uint mc_bits_now = 6;    // the write cache's size of the core looked at
static uint mc_size_now = 4096; // and how many texels of state it has
static uint mc_tile0 = 0;       // and, a worker's, the first of its tiles
static uint4 mc_geo;            // the geometry's texel
uint4 mc_state_read(uint2 pos) {
    if (mc_core == 0) return _TickState[pos + state_off];
    uint w = pos.y * 64 + pos.x;
    if (w >= mc_size_now) return (uint4)0;
    uint t = mc_tile0 + (w >> 6), i = w & 63;
    return _TickState[uint2(64 + ((t % 248) << 3) + (i & 7), ((t / 248) << 3) + (i >> 3))];
}
#define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
#define STATE_TEX(pos) mc_state_read(uint2(pos))

static uint2 s_dim;
static uint2 m_dim;

#include "helpers.cginc"
#include "src/types.cginc" // includes fb.h

// The GPU device's picture, for copying into RAM (docs/machine/gpu.md).
Texture2D<float4> _GpuTarget;
#define GPU_STATE _SelfTexture2D
#include "src/gpu.cginc"
#include "src/zap.cginc"   // the host's hand in RAM, for one commit (docs/world/danger.md)
// the lanes' field (lean/tick.hlsl, docs/machine/lanes.md): its texels in the state rows, its buckets' points
#define LANE_FIELD
#include "src/ram.cginc"
