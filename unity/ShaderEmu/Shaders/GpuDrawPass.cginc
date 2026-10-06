// One pass of the GPU device's draw (docs/gpu.md). The including pass defines _GpuPass (0-7)
// and sets that pass's blending and depth use.
#include "UnityCG.cginc"

Texture2D<uint4> _State;
// The ROM, for textures that are files in it (FRAGMENT_RGB24).
Texture2D<float4> _Data_MTD_R;
Texture2D<float4> _Data_MTD_G;
Texture2D<float4> _Data_MTD_B;
Texture2D<float4> _Data_MTD_A;
#define GPU_ROM
#define GPU_STATE _State
#define GPU_PASS_UNIFORMS
#define _GpuPasses 0xffu
#include "src/gpu.cginc"

gpu_varyings vert(uint id : SV_VertexID) {
    gpu_varyings o;
    o.position = float4(2, 2, 2, 1);
    o.colour = 0;
    o.uv = 0;
    o.texture_info = 0;
    o.key = 0;
    // Only the GPU's own camera draws this mesh: orthographic, onto the 2048x2048 target.
    if (unity_OrthoParams.w != 1.0 || _ScreenParams.x != GPU_TARGET || _ScreenParams.y != GPU_TARGET) return o;
#if _GpuPass != 0
    // nothing to do unless the submitted list says it has commands for this pass
    if (((ram(GPU_CTRL).r >> (8 + _GpuPass)) & 1) == 0) return o;
#endif
    o = gpu_vertex(id);
#if defined(UNITY_REVERSED_Z)
    o.position.z = o.position.w - o.position.z;
#endif
    return o;
}

float4 frag(gpu_varyings i) : SV_Target {
    return gpu_fragment(i);
}
