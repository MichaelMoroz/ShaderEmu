// One pass of the holodeck's HUD layer (docs/world/holodeck.md, "The HUD layer"): what the program draws flat over its
// picture (a status bar, a cross-hair, messages, the console), which the world round the seat leaves out, drawn on this
// renderer's unit quad (x and y from -0.5 to 0.5, scaled by the object) before the sitter. The including pass defines
// _VolumePass (0-3) and sets that pass's blending; the GPU's passes 4-7 are drawn with the pass that blends alike.
#include "UnityCG.cginc"

Texture2D<uint4> _State;
float _HudFlip;                // 1: the picture's top is the quad's top (-1 turns it over, if a platform's clip space differs)
// (the device's last list as its front end made it, the VRAM, the draw IDs: no RAM is read here)
#define GPU_STATE _State
#define GPU_TARGET_W 1280.0
#define GPU_TARGET_H 720.0
#define GPU_KEPT_SCRATCH   // (a kept frame: the scratch band as its list left it, EmuMachine.holoScratch)
#include "src/gpu_draw.cginc"

struct mesh_point {
    uint id : SV_VertexID;
    UNITY_VERTEX_INPUT_INSTANCE_ID
};
struct hud_point {
    uint triangle_id : TEXCOORD0;
    UNITY_VERTEX_OUTPUT_STEREO
};
struct hud_out {
    gpu_varyings v;
    UNITY_VERTEX_OUTPUT_STEREO
};

hud_point vert(mesh_point v) {
    hud_point o;
    UNITY_SETUP_INSTANCE_ID(v);
    UNITY_INITIALIZE_OUTPUT(hud_point, o);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
    o.triangle_id = v.id;
    return o;
}

[maxvertexcount(3)]
void geom(point hud_point i[1], inout TriangleStream<hud_out> stream) {
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i[0]);
    uint first = i[0].triangle_id * 3;
    if (!volume_triangle(first, VOLUME_FLAT, _VolumePass)) return;
    float2 size = volume_picture();
    for (uint k = 0; k < 3; k++) {
        hud_out o;
        UNITY_INITIALIZE_OUTPUT(hud_out, o);
        o.v = gpu_vertex_made(first + k);
        // the GPU's render target's pixels (the picture in its corner) to a share of the picture, then to the quad
        float w = abs(o.v.position.w) > 1e-6 ? o.v.position.w : 1.0;
        float2 target = float2((o.v.position.x / w + 1.0) * 0.5 * GPU_TARGET_W, (1.0 - o.v.position.y / w) * 0.5 * GPU_TARGET_H);
        float2 share = target / size;
        float3 local = float3(share.x - 0.5, (0.5 - share.y) * _HudFlip, 0.0);
        o.v.position = UnityObjectToClipPos(float4(local, 1.0));
        UNITY_TRANSFER_VERTEX_OUTPUT_STEREO(i[0], o);
        stream.Append(o);
    }
}

float4 frag(hud_out i) : SV_Target {
    float4 c = gpu_fragment(i.v);
#ifndef UNITY_COLORSPACE_GAMMA
    c.rgb = GammaToLinearSpace(c.rgb);
#endif
    return c;
}
