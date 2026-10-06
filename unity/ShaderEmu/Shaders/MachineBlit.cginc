// What Machine.shader's passes share: they are drawn with Blit, one pixel per state texel.
Texture2D<uint4> _SelfTexture2D;   // the machine's state before the pass
Texture2D<uint4> _TickState;       // the CPU's 64x64 state area after the tick

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
    if (p.x < 64 && p.y < 64) return _TickState[p];
    return _SelfTexture2D[p];
}
