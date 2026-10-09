// What Machine.shader's passes share: they are drawn with Blit, one pixel per state texel.
Texture2D<uint4> _SelfTexture2D;   // the machine's state before the pass
Texture2D<uint4> _TickState;       // what the tick keeps of the CPU's state area, after the tick
// The rows of the state area that the tick draws: STATE_ROWS of src/types.cginc (16 with the
// default write cache; the CSR area under them is the commit's).
#define TICK_STATE_ROWS 16

struct blit_v2f {
    float4 vertex : SV_Position;   // .xy is the texel being written
};

blit_v2f blit_vert(appdata_base v) {
    blit_v2f o;
    o.vertex = UnityObjectToClipPos(v.vertex);
    return o;
}

uint4 state_after_tick(uint2 p) {
    [branch]
    if (p.x < 64 && p.y < TICK_STATE_ROWS) return _TickState[p];
    return _SelfTexture2D[p];
}
