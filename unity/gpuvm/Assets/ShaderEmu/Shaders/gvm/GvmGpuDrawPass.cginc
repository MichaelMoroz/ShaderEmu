// GpuDrawPass.cginc over gpu_vm's RAM and ROM textures (GVM_RAM).
// One pass of the GPU device's draw (docs/machine/gpu.md). The including pass defines _GpuPass (0-7)
// and sets that pass's blending and depth use.
// The mesh is 65,536 points, one per triangle of the device's mesh. The geometry shader finds
// the command that claims the point's three vertices and emits them; for a triangle no
// command claims, or only claims part of, it emits nothing.
#include "UnityCG.cginc"

Texture2D<uint4> _State;
// The ROM, for textures that are files in it (FRAGMENT_RGB24): gpu_vm's one texture (GVM_RAM).
Texture2D<uint4> _GvmRom;
#define GPU_ROM
#define GPU_STATE _State
#define GPU_PASS_UNIFORMS
#define _GpuPasses 0xffu
// the picture is 1280x720 at most here
#define GPU_TARGET_W 1280.0
#define GPU_TARGET_H 720.0
#define GVM_RAM
#include "../src/gpu.cginc"

struct gpu_point {
    uint triangle_id : TEXCOORD0;
};

gpu_point vert(uint id : SV_VertexID) {
    gpu_point o;
    o.triangle_id = id;
    return o;
}

// The device's triangle `t`'s three vertices, if a command claims them.
void gpu_triangle(uint t, inout TriangleStream<gpu_varyings> stream) {
    uint first = t * 3, base;
    uint4 head;
    if (!gpu_command(first, 3, base, head)) return;
    for (uint k = 0; k < 3; k++) {
        gpu_varyings o = gpu_vertex_of(first + k, base, head);
#if defined(UNITY_REVERSED_Z)
        o.position.z = o.position.w - o.position.z;
#endif
        stream.Append(o);
    }
    stream.RestartStrip();
}

#ifdef GPU_BLIT
// As blits (GvmGpuDrawBlit.shader, docs/REGRESSIONS.md of GPU-VM, step 2 item 4): the blit's 5,184 calls, 13 of the
// device's triangles each (39 vertices of 20 values, under 1,024), cover its 65,536. A blit has no camera to check:
// the script blits only into the GPU's target.
static const uint GPU_BLIT_TRIS = 13;
[maxvertexcount(39)]
[instance(32)]
void amp_geo(point amp_d I[1], inout TriangleStream<gpu_varyings> stream, uint inst : SV_GSInstanceID) {
    uint pid = I[0].patch;
#if _GpuPass != 0
    if (((ram(GPU_CTRL).r >> (8 + _GpuPass)) & 1) == 0) return;
#endif
    uint first = amp_call(I[0], pid, inst) * GPU_BLIT_TRIS;
    for (uint t = 0; t < GPU_BLIT_TRIS; t++)
        if (first + t < 65536) gpu_triangle(first + t, stream);
}
#else
[maxvertexcount(3)]
void geom(point gpu_point i[1], inout TriangleStream<gpu_varyings> stream) {
    // Only the GPU's own camera draws this mesh: orthographic, onto the GPU's target.
    if (unity_OrthoParams.w != 1.0 || _ScreenParams.x != GPU_TARGET_W || _ScreenParams.y != GPU_TARGET_H) return;
#if _GpuPass != 0
    // nothing to do unless the submitted list says it has commands for this pass
    if (((ram(GPU_CTRL).r >> (8 + _GpuPass)) & 1) == 0) return;
#endif
    gpu_triangle(i[0].triangle_id, stream);
}
#endif

float4 frag(gpu_varyings i) : SV_Target {
    return gpu_fragment(i);
}
