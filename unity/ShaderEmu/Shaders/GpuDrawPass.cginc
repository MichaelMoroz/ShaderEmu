// One pass of the GPU device's draw (docs/machine/gpu.md), a blit of GpuDrawBlit.shader. The including pass defines _GpuPass
// (0-9, 12, 13; 16 the uploads, 17 the copies) and sets that pass's blending and depth use. Its triangles are the front
// end's (GpuFront.shader), each drawn whole when the draw IDs give all three of its vertices to this pass.
#include "UnityCG.cginc"

Texture2D<uint4> _State;
// The ROM, for textures that are files in it (the uploads and the front end read it).
Texture2D<float4> _Data_ROM;   // the ROM: a 16-byte texel a pixel, read by its bits (asuint)
#define GPU_ROM
#define GPU_STATE _State
float _StateRow;
#define GPU_STATE_ROW ((uint)(int)_StateRow)
// (every pass drawn every frame: 0-9, 12 and 13; bit 16, GPU_UPLOADED, when EmuMachine made the VRAM's levels this frame)
uniform uint _GpuUploaded;
#define _GpuPasses (0x33ffu | _GpuUploaded)
// the picture is 1280x720 at most here
#define GPU_TARGET_W 1280.0
#define GPU_TARGET_H 720.0
#ifdef GPU_AMP
// GpuDrawBlit.shader: the mesh's triangles made by a blit's calls (gpu_amp_geom in src/gpu_draw.cginc, src/amp.cginc),
// their depth turned as Unity's reversed Z has it
#include "src/fxc2_check.cginc"
#if defined(UNITY_REVERSED_Z)
#define GPU_AMP_Z(o) o.position.z = o.position.w - o.position.z
#endif
#endif
#include "src/gpu_draw.cginc"

#ifdef GPU_DRAW_IDS_PASS
// GpuDrawBlit.shader's pass 14: each mesh vertex's command and passes, a texel of the 512 x 384 draw IDs each (a blit's
// quad is 0 to 1 each way, which way up does not matter: the texel is the fragment's pixel)
float4 vert(float4 v : POSITION) : SV_Position {
    return float4(v.xy * 2.0 - 1.0, 0.5, 1.0);
}

int2 frag(float4 p : SV_Position) : SV_Target {
    uint2 at = uint2(p.xy);
    return asint(gpu_draw_ids_fragment(at.y * GPU_DRAW_IDS_W + at.x));
}
#elif defined(GPU_CLEAR_NEAR)
// GpuDrawBlit.shader's pass 15, blitted before pass 8: the whole target's depth at the near plane, so that past the
// picture (which nothing reads) a depth-tested pass's fragment fails the card's depth test before it is shaded: 3D
// triangles past a program's view were drawn and shaded over the rest of the 1280 x 720 target. Not a clip: a pixel
// inside the picture is drawn as before. Depth by the vertices, not SV_Depth, which would turn the card's coarse depth
// culling off for the passes after it.
float4 vert(float4 v : POSITION) : SV_Position {
#if defined(UNITY_REVERSED_Z)
    float near = 1.0;
#else
    float near = 0.0;
#endif
    return float4(v.xy * 2.0 - 1.0, near, 1.0);
}

float4 frag() : SV_Target {
    return 0;
}
#elif defined(GPU_CLEAR)
// GpuDrawBlit.shader's pass 8: the depth cleared to far over the list's picture only, after pass 15 put the whole
// target's at the near plane: past the picture a depth-tested fragment is not drawn. A blit's quad is 0 to 1 each way.
float4 vert(float4 v : POSITION) : SV_Position {
    float2 part = gpu_made_picture() / float2(GPU_TARGET_W, GPU_TARGET_H);
#if defined(UNITY_REVERSED_Z)
    float far = 0.0;
#else
    float far = 1.0;
#endif
    return float4(v.x * 2.0 * part.x - 1.0, 1.0 - v.y * 2.0 * part.y, far, 1.0);
}

float4 frag() : SV_Target {
    return 0;
}
#else

float4 frag(gpu_varyings i) : SV_Target {
#if _GpuPass == 16
    return gpu_upload_fragment(i);   // (GpuDrawBlit.shader's GPUUpload, into the VRAM)
#elif _GpuPass == 17
    return gpu_copy_fragment(i);     // (its GPUCopy: rectangles of the picture into the VRAM, after every pass)
#else
    return gpu_fragment(i);
#endif
}

#endif
