// One pass of the GPU device's draw (docs/gpu.md). The including pass defines _GpuPass (0-7)
// and sets that pass's blending and depth use.
// The mesh is 65,536 points, one per triangle of the device's mesh. The geometry shader finds
// the command that claims the point's three vertices and emits them; for a triangle no
// command claims, or only claims part of, it emits nothing.
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
// the picture is 1280x720 at most here
#define GPU_TARGET_W 1280.0
#define GPU_TARGET_H 720.0
#include "src/gpu.cginc"

struct gpu_point {
    uint triangle_id : TEXCOORD0;
};

gpu_point vert(uint id : SV_VertexID) {
    gpu_point o;
    o.triangle_id = id;
    return o;
}

[maxvertexcount(3)]
void geom(point gpu_point i[1], inout TriangleStream<gpu_varyings> stream) {
    // Only the GPU's own camera draws this mesh: orthographic, onto the GPU's target.
    if (unity_OrthoParams.w != 1.0 || _ScreenParams.x != GPU_TARGET_W || _ScreenParams.y != GPU_TARGET_H) return;
#if _GpuPass != 0
    // nothing to do unless the submitted list says it has commands for this pass
    if (((ram(GPU_CTRL).r >> (8 + _GpuPass)) & 1) == 0) return;
#endif
    uint first = i[0].triangle_id * 3, base;
    uint4 head;
    if (!gpu_command(first, 3, base, head)) return;
    for (uint k = 0; k < 3; k++) {
        gpu_varyings o = gpu_vertex_of(first + k, base, head);
#if defined(UNITY_REVERSED_Z)
        o.position.z = o.position.w - o.position.z;
#endif
        stream.Append(o);
    }
}

float4 frag(gpu_varyings i) : SV_Target {
    return gpu_fragment(i);
}
