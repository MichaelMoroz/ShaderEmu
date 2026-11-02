static bool is_ortho = unity_OrthoParams.w;

#define OFFSET_UINT2_POS(x, y, w, h, n) uint2((x + n) % w, y + ((n + x) / h))



uint idx_uint4(uint4 data, uint idx) {
    // Selects, not data[idx]: DXC compiles a dynamic vector index as four stores to a local
    // array plus a load, on every instruction fetch. idx is always 0..3.
    return (idx & 2) ? ((idx & 1) ? data.w : data.z) : ((idx & 1) ? data.y : data.x);
    /* [flatten] */
    /* switch (idx) { */
    /*     case 0: */
    /*         return data.x; */
    /*     case 1: */
    /*         return data.y; */
    /*     case 2: */
    /*         return data.z; */
    /*     default: // 3 */
    /*         return data.w; */
    /* } */
}

void set_idx_uint4(inout uint4 data, uint value, uint idx) {
    [flatten]
    switch (idx) {
        case 0:
            data.x = value;
            break;
        case 1:
            data.y = value;
            break;
        case 2:
            data.z = value;
            break;
        default: // 3
            data.w = value;
            break;
    }
}

struct v2f {
    float4 vertex : SV_Position;
    centroid float2 uv : TEXCOORD0;
};




#define PACK_MASK 0xFF
#define PACK_SHIFT 8
uint4 unpack_uint4(in float3 data[6]) {
    uint4 result;
    result.x = uint(data[0].r * 255.0f + 0.5f) |
               (uint(data[1].r * 255.0f + 0.5f) << 8) |
               (uint(data[2].r * 255.0f + 0.5f) << 16) |
               (uint(data[3].r * 255.0f + 0.5f) << 24);
    result.y = uint(data[0].g * 255.0f + 0.5f) |
               (uint(data[1].g * 255.0f + 0.5f) << 8) |
               (uint(data[2].g * 255.0f + 0.5f) << 16) |
               (uint(data[3].g * 255.0f + 0.5f) << 24);
    result.z = uint(data[0].b * 255.0f + 0.5f) |
               (uint(data[1].b * 255.0f + 0.5f) << 8) |
               (uint(data[2].b * 255.0f + 0.5f) << 16) |
               (uint(data[3].b * 255.0f + 0.5f) << 24);
    result.w = uint(data[4].r * 255.0f + 0.5f) |
               (uint(data[4].g * 255.0f + 0.5f) << 8) |
               (uint(data[4].b * 255.0f + 0.5f) << 16) |
               (uint(data[5].r * 255.0f + 0.5f) << 24);
    return result;
}
#undef PACK_SHIFT
#undef PACK_MASK

uint unpack_raw_float4(in float4 data) {
    uint res = 0;
    res |= uint(data.a*255.0f+0.5f) << 24;
    res |= uint(data.b*255.0f+0.5f) << 16;
    res |= uint(data.g*255.0f+0.5f) << 8;
    res |= uint(data.r*255.0f+0.5f);
    return res;
}

