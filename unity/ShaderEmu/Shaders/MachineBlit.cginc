// What Machine.shader's passes share: they are drawn with Blit, one pixel per state texel.
Texture2D<uint4> _SelfTexture2D;   // the machine's state before the pass
Texture2D<uint4> _TickState;       // what the tick keeps of the CPU's state area, after the tick
// The write cache is the shader's default (384 texels), which makes the state area's rows that
// the tick keeps 16 (STATE_ROWS of src/types.cginc).
#define L1_TABLE_BITS 6
#define TICK_STATE_ROWS 16
// Core 0 and up to 255 workers (docs/machine/multicore.md). The tick's texture is the state rows'
// own places: core 0's 64 x 16, and beside it the workers' bands, 248 tiles of 8 x 8 each, of
// which each worker has a strip as the guest's geometry says (src/types.cginc).
#define CORES 256
#define TICK_STATE_WIDTH 2048
#define WORKER_BAND_ROWS 48   // 8 * MC_STRIP_BANDS: six bands, 49 workers of five tiles to a band
// The tick itself is drawn into eight targets, eight state texels a pixel (MachineTick.shader):
// core 0's eight columns, TICK_GAP empty ones, then a block of columns for each worker, 128
// blocks to a band of TICK_STATE_ROWS rows, TICK_MRT_BANDS bands.
#define TICK_MRT_W 1040
#define TICK_MRT_BANDS 2
#define TICK_GAP 8

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
// The bands of RAM (4 MB, 32 rows of 8,192 texels from row 64 + 32 b) the pass has to draw: what it changes,
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
    uint bands = blit_bands(), count = (height - 64) / 32, from = 0;
    bool run = false;
    blit_rows(stream, 0.0, 64.0, height);
    [loop]
    for (uint b = 0; b <= count; b++) {
        bool on = b < count && (((bands >> (b & 31)) & 1) != 0 || (b < 32 && ((CONTROL_BANDS >> b) & 1) != 0));
        if (on && !run) from = b;
        if (!on && run) blit_rows(stream, 64.0 + from * 32.0, (b - from) * 32.0, height);
        run = on;
    }
}
#endif

// A texel of the state rows as the tick left it: from the tick's texture, which holds core 0's
// rectangle and the workers' band (what the tick did not draw of them was copied in before it).
uint4 state_after_tick(uint2 p) {
    bool kept = p.x < 64 ? p.y < TICK_STATE_ROWS : p.x < TICK_STATE_WIDTH && p.y < WORKER_BAND_ROWS;
    [branch]
    if (kept) return _TickState[p];
    return _SelfTexture2D[p];
}
