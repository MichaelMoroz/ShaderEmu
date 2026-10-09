// One pass of the volume display (docs/volume.md). The including pass defines _VolumePass (0-3)
// and sets that pass's blending: the GPU's own passes 0-3, and 4-7 with them (with depth here).
// The same point mesh as the GPU device's, one point a triangle, seen by the visitor's camera.
#include "UnityCG.cginc"

Texture2D<uint4> _State;
float4 _ScreenSize;
float _Plane;
#ifdef HOLODECK
float _Anchor, _UpAxis;
float4 _RoomMin, _RoomMax;   // the holodeck itself, in the world
Texture2D<float4> _Origin;   // where the world's middle is, in the program's own world (HolodeckOrigin.shader)
#endif
#define GPU_STATE _State
#define GPU_VOLUME
#define GPU_VOLUME_PASS _VolumePass
#include "src/gpu.cginc"

struct mesh_point {
    uint id : SV_VertexID;
    UNITY_VERTEX_INPUT_INSTANCE_ID
};
struct volume_point {
    uint triangle_id : TEXCOORD0;
    UNITY_VERTEX_OUTPUT_STEREO
};
struct volume_out {
    gpu_varyings v;
    float behind : SV_ClipDistance0;   // how far behind the screen: what is before it is cut away
#ifdef HOLODECK
    float3 world : TEXCOORD5;
#endif
    UNITY_VERTEX_OUTPUT_STEREO
};

volume_point vert(mesh_point v) {
    volume_point o;
    UNITY_SETUP_INSTANCE_ID(v);
    UNITY_INITIALIZE_OUTPUT(volume_point, o);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
    o.triangle_id = v.id;
    return o;
}

[maxvertexcount(3)]
void geom(point volume_point i[1], inout TriangleStream<volume_out> stream) {
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i[0]);
    uint first = i[0].triangle_id * 3, base;
    uint4 head;
    if (!volume_command(first, base, head)) return;
#ifdef HOLODECK
    float3 rx, ry, rz, move;
    bool placed = _Anchor < 1.5 && volume_view(rx, ry, rz, move);
    // which of the program's axes is up (0: x, 1: y, 2: z): as set, or the one the camera's own up
    // is nearest to and its right is square to (a camera does not roll)
    float3 upright = ry - 2.0 * abs(rx);
    float3 middle = _Origin.Load(int3(0, 0, 0)).xyz;
    int up = _UpAxis > 1.5 ? 2 : _UpAxis > 0.5 ? 1 : upright.z > max(upright.x, upright.y) ? 2 : upright.y >= upright.x ? 1 : 0;
#endif
    for (uint k = 0; k < 3; k++) {
        float3 eye;
        float2 planes, focal;
        volume_out o;
        UNITY_INITIALIZE_OUTPUT(volume_out, o);
        o.v = volume_vertex(first + k, base, head, eye, planes, focal);
#ifdef HOLODECK
        // out of the camera's turn, back into the world's: the room does not swing when the
        // player looks about. With the world's own place too, or (anchor 0) the player's.
        float3 from = _Anchor > 0.5 ? eye - move : eye;
        float3 p = placed ? rx * from.x + ry * from.y + rz * from.z : eye;
        if (placed && _Anchor > 0.5) p -= middle;
        // metres a unit, as the volume display's: at its largest the picture's rectangle on the
        // near plane is _ScreenSize, and _Plane moves that plane out towards the far one
        float near = max(planes.x, 1e-4), at = near * pow(max(planes.y, near) / near, _Plane);
        float scale = min(_ScreenSize.x * focal.x, _ScreenSize.y * focal.y) / (2.0 * at);
        // right-handed to Unity's left-handed, the program's up to this room's
        float3 local = (!placed || up == 1 ? float3(p.x, p.y, -p.z) : up == 2 ? float3(p.x, p.z, p.y) : float3(p.y, p.x, p.z)) * scale;
        // a sky is laid on the program's picture: here it is a panorama round the visitor, and
        // its colour carries the way from the eye to this place
        if (o.v.texture_info.r & FRAGMENT_LAID)
            o.v.colour = float4(local - mul(unity_WorldToObject, float4(_WorldSpaceCameraPos, 1.0)).xyz, 1.0);
        o.v.position = UnityObjectToClipPos(float4(local, 1.0));
        o.world = mul(unity_ObjectToWorld, float4(local, 1.0)).xyz;
        o.behind = 1.0;
#else
        // the screen's distance from the guest's camera: from near to far in equal ratios
        float near = max(planes.x, 1e-4), at = near * pow(max(planes.y, near) / near, _Plane);
        // metres a guest unit: the picture's rectangle at that distance, 2 at / focal across,
        // fits the screen
        float scale = min(_ScreenSize.x * focal.x, _ScreenSize.y * focal.y) / (2.0 * at);
        // the camera stands that far before the screen, on the visitor's side; what is nearer
        // to it than the screen is cut at the screen: nothing is before a window
        float3 local = float3(eye.x, eye.y, -eye.z - at) * scale;
        o.v.position = UnityObjectToClipPos(float4(local, 1.0));
        o.behind = local.z;
#endif
        UNITY_TRANSFER_VERTEX_OUTPUT_STEREO(i[0], o);
        stream.Append(o);
    }
}

float4 frag(volume_out i) : SV_Target {
#ifdef HOLODECK
    // Seen from outside the room, what is nearer than the room is not in it: it would hang in
    // the doorway. (From inside, the way from the eye is in the room from its start.)
    float3 way = i.world - _WorldSpaceCameraPos, each = 1.0 / (abs(way) < 1e-6 ? 1e-6 : way);
    float3 one = (_RoomMin.xyz - _WorldSpaceCameraPos) * each, two = (_RoomMax.xyz - _WorldSpaceCameraPos) * each;
    float3 near3 = min(one, two), far3 = max(one, two);
    float enters = max(near3.x, max(near3.y, near3.z)), leaves = min(far3.x, min(far3.y, far3.z));
    if (enters > 0.0 && (enters > leaves || enters > 1.0)) discard;
#ifdef HOLODECK_INSIDE
    // where the room's faces are not what is seen, the program's world ends at the room's walls
    if (any(i.world < _RoomMin.xyz - 0.001) || any(i.world > _RoomMax.xyz + 0.001)) discard;
#endif
    if (i.v.texture_info.r & FRAGMENT_LAID) {
        // four of the picture to a turn, its horizon three quarters down (as Doom's sky is hung)
        float3 sky = i.v.colour.xyz;
        i.v.uv = float2(atan2(sky.x, sky.z) * 0.6366198, clamp(0.78 - 1.04 * sky.y / max(length(sky.xz), 1e-4), 0.01, 0.99));
        i.v.colour = 1.0;
        i.v.texture_info.r &= ~FRAGMENT_LAID;
    }
#endif
    float4 c = gpu_fragment(i.v);
#ifndef UNITY_COLORSPACE_GAMMA
    c.rgb = GammaToLinearSpace(c.rgb);
#endif
#if _VolumePass == 0
    c.a = 1;
#endif
    return c;
}
