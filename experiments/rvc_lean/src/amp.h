// amp.h: a draw of many points from one blit (docs/machine/gpu.md, "Blits in place of cameras"). Unity's Blit draws
// two triangles; with a hull and a domain shader they are two patches of three points, a quad domain at factor 8
// (never above 8: a driver may clamp a larger factor) is 81 points a patch, and a geometry shader of AMP_INST
// instances is a call for each point and instance: 5,184 calls a blit at 32. The including pass's geometry shader
// takes `point amp_d I[1]` and SV_GSInstanceID, and amp_call() says which call it is.
//
// Calls are numbered in the order the card draws them, so that a call's points or triangles are drawn after a lower
// call's, as a mesh's are: the patches come in order and the instances of a point too, but the order of a patch's
// points is the tessellator's own (WARP's from a corner inwards ring by ring, NVIDIA's the inner rows first and the
// ring last). That order is measured on the card by the order probe below (amp_order_geom, then amp_rank_frag), and
// _AmpRank says each point's place in it.
//
// The hull shader returns no control point by its index (fxc2 before patch 64 wrote `I[id]` as an indexable temp
// that removed the device on the card); a shader of patches still includes fxc2_check.h first, which refuses an
// fxc2 older than 65.
#ifndef AMP_H
#define AMP_H

#ifndef AMP_INST
#define AMP_INST 32
#endif
#define AMP_F 8
#define AMP_N 9          // points a side
#define AMP_POINTS 81    // a patch's points

struct amp_v {
    float4 p : INTERNALTESSPOS;
};
struct amp_tf {
    float e[4] : SV_TessFactor;
    float i[2] : SV_InsideTessFactor;
};
struct amp_d {
    nointerpolation uint at : TEXCOORD0;      // its place in the domain, v * 9 + u (not its order)
    nointerpolation uint patch : TEXCOORD1;   // the domain shader's SV_PrimitiveID: 0 or 1
};

// (no vertex input: a blit's corners are not used)
amp_v amp_vert(uint id : SV_VertexID) {
    amp_v o;
    o.p = float4(0, 0, 0, 1);
    return o;
}
amp_tf amp_hc(InputPatch<amp_v, 3> I) {
    amp_tf o;
    o.e[0] = o.e[1] = o.e[2] = o.e[3] = AMP_F;
    o.i[0] = o.i[1] = AMP_F;
    return o;
}
[domain("quad")]
[partitioning("integer")]
[outputtopology("point")]
[patchconstantfunc("amp_hc")]
[outputcontrolpoints(3)]
amp_v amp_hull(InputPatch<amp_v, 3> I, uint id : SV_OutputControlPointID) {
    amp_v o;
    o.p = float4(0, 0, 0, 1);
    return o;
}
[domain("quad")]
amp_d amp_dom(amp_tf h, const OutputPatch<amp_v, 3> I, float2 uv : SV_DomainLocation, uint pid : SV_PrimitiveID) {
    amp_d o;
    o.at = (uint)round(uv.y * AMP_F) * AMP_N + (uint)round(uv.x * AMP_F);
    o.patch = pid;
    return o;
}

#ifndef AMP_PROBE
Texture2D<float4> _AmpRank;   // 81 x 1: a point's place in the order its patch's points are drawn
uniform uint _AmpFirst;       // the first call's number: a draw of more calls than a blit has is several blits
// This call's number, _AmpFirst to _AmpFirst + 162 * AMP_INST - 1, in the order the card draws the calls.
uint amp_call(amp_d d, uint inst) {
    uint rank = (uint)_AmpRank[uint2(d.at, 0)].r;
    return _AmpFirst + (d.patch * AMP_POINTS + rank) * AMP_INST + inst;
}
#endif

// A texel's centre in a target w x h as a point's position, row 0 at the top.
float4 amp_texel(uint x, uint y, float w, float h) {
    return float4(((float)x + 0.5) / w * 2.0 - 1.0, 1.0 - ((float)y + 0.5) / h * 2.0, 0.5, 1.0);
}

// The order probe: a blit into an 81 x 81 target (one float a texel). Point i of the first patch draws texels
// (i, j) and (j, i) for every j, i + 1 in each, so that texel (i, j) ends as the later of i and j plus one.
struct amp_order_v {
    float4 position : SV_Position;
    nointerpolation float value : TEXCOORD0;
};
[maxvertexcount(162)]   // 2 * AMP_POINTS (vkd3d takes a number there)
void amp_order_geom(point amp_d I[1], inout PointStream<amp_order_v> s) {
    if (I[0].patch != 0) return;
    uint i = I[0].at;
    amp_order_v o;
    o.value = (float)(i + 1);
    for (uint j = 0; j < AMP_POINTS; j++) {
        o.position = amp_texel(i, j, AMP_POINTS, AMP_POINTS);
        s.Append(o);
        o.position = amp_texel(j, i, AMP_POINTS, AMP_POINTS);
        s.Append(o);
    }
}
float4 amp_order_frag(amp_order_v i) : SV_Target {
    return i.value;
}
// Then a pixel of an 81 x 1 target each: point x's place, how many points were drawn before it.
#ifdef AMP_PROBE
Texture2D<float4> _AmpOrder;
float4 amp_rank_of(uint x) {
    uint n = 0;
    for (uint j = 0; j < AMP_POINTS; j++)
        if (j != x && (uint)_AmpOrder[uint2(x, j)].r == x + 1) n++;
    return (float)n;
}
#endif

#endif
