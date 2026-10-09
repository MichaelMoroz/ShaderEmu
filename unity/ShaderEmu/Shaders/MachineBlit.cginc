// What Machine.shader's passes share: they are drawn with Blit, one pixel per state texel.
Texture2D<uint4> _SelfTexture2D;   // the machine's state before the pass
Texture2D<uint4> _TickState;       // what the tick keeps of the CPU's state area, after the tick
// The rows of the state area that the tick draws: STATE_ROWS of src/types.cginc (16 with the
// default write cache; the CSR area under them is the commit's).
#define TICK_STATE_ROWS 16
// Core 0 and three workers (docs/multicore.md), a block of state each, CORE_PITCH texels apart.
// The tick's texture has the same places: it is CORE_PITCH * (CORES - 1) + 64 wide.
#define CORES 4
#define CORE_PITCH 256
#define TICK_STATE_WIDTH (CORE_PITCH * (CORES - 1) + 64)
// What a worker keeps (MC_ROWS in src/types.cginc): 64 x 8, and 16 x 4 under that.
#define WORKER_ROWS 8
#define WORKER_TAIL_WIDTH 16
#define WORKER_TAIL_ROWS 4

struct blit_v2f {
    float4 vertex : SV_Position;   // .xy is the texel being written
};

blit_v2f blit_vert(appdata_base v) {
    blit_v2f o;
    o.vertex = UnityObjectToClipPos(v.vertex);
    return o;
}

#ifdef BLIT_TICK
void tick_zone(inout TriangleStream<blit_v2f> stream, float x, float y, float width, float height) {
    // clip space as the card has it: the texture's top left is (-1, 1)
    float x0 = 2.0 * x / TICK_STATE_WIDTH - 1.0, x1 = 2.0 * (x + width) / TICK_STATE_WIDTH - 1.0;
    float y0 = 1.0 - 2.0 * y / TICK_STATE_ROWS, y1 = 1.0 - 2.0 * (y + height) / TICK_STATE_ROWS;
    blit_v2f o;
    o.vertex = float4(x0, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(x1, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(x0, y1, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(x1, y1, 0.5, 1.0); stream.Append(o);
    stream.RestartStrip();
}

// In place of Blit's two triangles: core 0's rectangle and two small ones a worker. Pixels
// between them are not drawn: a pixel that runs the tick for nothing costs as one that counts.
[maxvertexcount(124)]
void blit_tick_geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> stream) {
    if (which != 0) return;
    tick_zone(stream, 0, 0, 64, TICK_STATE_ROWS);
    for (uint core = 1; core < CORES; core++) {
        tick_zone(stream, core * CORE_PITCH, 0, 64, WORKER_ROWS);
        tick_zone(stream, core * CORE_PITCH, WORKER_ROWS, WORKER_TAIL_WIDTH, WORKER_TAIL_ROWS);
    }
}
#endif

#ifdef BLIT_BANDS
// The control pass writes the device's words and a page the host fetched: RAM bands 28 and 29.
#define CONTROL_BANDS (3u << 28)
// The bands of RAM (4 MB, 128 rows; bit b is the one from row 64 + 128 b) the pass has to
// draw: what it changes, and what the pass before it changed, which the texture drawn into
// lacks. The other texels there are right already, so they are not drawn at all.
uint blit_bands();

// In place of Blit's two triangles: the 64 state rows and a rectangle for each of those bands.
[maxvertexcount(132)]
void blit_bands_geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> stream) {
    if (which != 0) return;
    uint width, height;
    _SelfTexture2D.GetDimensions(width, height);
    uint bands = blit_bands();
    for (uint quad = 0; quad < 33; quad++) {
        if (quad != 0 && ((bands >> (quad - 1)) & 1) == 0) continue;
        float top = quad == 0 ? 0.0 : 64.0 + (quad - 1) * 128.0, rows = quad == 0 ? 64.0 : 128.0;
        // clip space as the card has it: row 0 of the texture is y = 1
        float y0 = 1.0 - 2.0 * top / height, y1 = 1.0 - 2.0 * (top + rows) / height;
        blit_v2f o;
        o.vertex = float4(-1.0, y0, 0.5, 1.0); stream.Append(o);
        o.vertex = float4(1.0, y0, 0.5, 1.0); stream.Append(o);
        o.vertex = float4(-1.0, y1, 0.5, 1.0); stream.Append(o);
        o.vertex = float4(1.0, y1, 0.5, 1.0); stream.Append(o);
        stream.RestartStrip();
    }
}
#endif

// A texel of the state rows as the tick left it: from the tick's texture where the tick drew.
uint4 state_after_tick(uint2 p) {
    uint core = p.x / CORE_PITCH, x = p.x % CORE_PITCH;
    bool drawn = core == 0 ? x < 64 && p.y < TICK_STATE_ROWS
                           : core < CORES && ((x < 64 && p.y < WORKER_ROWS) ||
                                              (x < WORKER_TAIL_WIDTH && p.y >= WORKER_ROWS && p.y < WORKER_ROWS + WORKER_TAIL_ROWS));
    [branch]
    if (drawn) return _TickState[p];
    return _SelfTexture2D[p];
}
