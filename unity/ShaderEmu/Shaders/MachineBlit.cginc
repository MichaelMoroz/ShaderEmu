// What Machine.shader's passes share: they are drawn with Blit, one pixel per state texel.
Texture2D<uint4> _SelfTexture2D;   // the machine's state before the pass
Texture2D<uint4> _TickState;       // what the tick keeps of the CPU's state area, after the tick
// The write cache is the shader's default.
#define L1_TABLE_BITS 6
// Core 0 and up to 255 workers (docs/machine/multicore.md). The tick's texture is the state rows' own
// places: a slot of 64 x 8 for each core (src/types.cginc). The tick itself is drawn into eight targets,
// eight state texels a pixel (MachineTickLean.shader): a block of 8 x 8 for each core, in one row.
#define CORES 256

struct blit_v2f {
    float4 vertex : SV_Position;   // .xy is the texel being written
};

blit_v2f blit_vert(appdata_base v) {
    blit_v2f o;
    o.vertex = UnityObjectToClipPos(v.vertex);
    return o;
}

#ifdef BLIT_BANDS
// The control pass writes the device's words and a page the host fetched: RAM bands 4 and 5,
// drawn by every pass that draws bands.
#define CONTROL_BANDS (3u << 4)
// The bands of RAM (4 MB, 128 rows from row 64 + 128 b) the pass has to draw: what it changes,
// and what the pass before it changed, which the texture drawn into lacks. The other texels
// there are right already. Bit b & 31 stands for bands b, b + 32 and b + 64 (ram_bands, src/types.h).
uint blit_bands();

// rows top to top + rows of the texture, as a quad
void blit_rows(inout TriangleStream<blit_v2f> stream, float top, float rows, float height) {
    // clip space as the card has it: row 0 of the texture is y = 1
    float y0 = 1.0 - 2.0 * top / height, y1 = 1.0 - 2.0 * (top + rows) / height;
    blit_v2f o;
    o.vertex = float4(-1.0, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(1.0, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(-1.0, y1, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(1.0, y1, 0.5, 1.0); stream.Append(o);
    stream.RestartStrip();
}

// In place of Blit's two triangles: the 64 state rows and a rectangle for each run of those
// bands (72 bands make 36 runs at most).
[maxvertexcount(148)]
void blit_bands_geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> stream) {
    if (which != 0) return;
    uint width, height;
    _SelfTexture2D.GetDimensions(width, height);
    uint bands = blit_bands(), count = (height - 64) / 128, from = 0;
    bool run = false;
    blit_rows(stream, 0.0, 64.0, height);
    [loop]
    for (uint b = 0; b <= count; b++) {
        bool on = b < count && (((bands >> (b & 31)) & 1) != 0 || (b < 32 && ((CONTROL_BANDS >> b) & 1) != 0));
        if (on && !run) from = b;
        if (!on && run) blit_rows(stream, 64.0 + from * 128.0, (b - from) * 128.0, height);
        run = on;
    }
}
#endif

// A texel of the state rows as the tick left it: what the tick did not draw of them was copied in before it.
uint4 state_after_tick(uint2 p) {
    return _TickState[p];
}
