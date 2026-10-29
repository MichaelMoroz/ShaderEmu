// The runs of RAM the control pass writes (Machine.shader's GPUControl), drawn into a window
// of their own, WINDOW_W texels by WINDOW_ROWS, and copied into the state by the WindowBack pass,
// as no pass reads what it draws. A row of the window is 2,048 texels (32 KB) of RAM, a quarter of
// a row of the state (rows of 8,192: docs/machine/gpu.md, "Rows of 8192"): the control words, the
// counters' totals, then the console's, the network's and the core statistics', and a fetched
// page's eight (src/gpu.cginc).
#define WINDOW_W 2048
#define WINDOW_ROWS 11
#define WINDOW_STATE_W 8192
#define WINDOW_STATE_H 2368   // STATE_TEX_HEIGHT (src/layout.cginc)
#define WINDOW_CONTROL 0x100000u   // the runs' first RAM texels
#define WINDOW_COUNTS 0x0c1000u
#define WINDOW_NET 0x16b800u
uint window_texel(uint r) {
    return r == 0 ? WINDOW_CONTROL : r == 1 ? WINDOW_COUNTS : WINDOW_NET + (r - 2) * WINDOW_W;
}
// the state's texel of the window's (x, r)
uint2 window_state_pos(uint2 p) {
    uint t = window_texel(p.y) + p.x;
    return uint2(t % WINDOW_STATE_W, 64 + t / WINDOW_STATE_W);
}
// and the window's of a state texel in one of those runs
uint2 window_of(uint2 pos) {
    uint t = (pos.y - 64) * WINDOW_STATE_W + pos.x;
    if (t - WINDOW_CONTROL < WINDOW_W) return uint2(t - WINDOW_CONTROL, 0);
    if (t - WINDOW_COUNTS < WINDOW_W) return uint2(t - WINDOW_COUNTS, 1);
    return uint2((t - WINDOW_NET) % WINDOW_W, 2 + (t - WINDOW_NET) / WINDOW_W);
}

// A quad of rows top to top + rows of a target `height` rows high (row 0 is clip y = 1), from
// column `left` for `wide` columns of `width`.
void machine_rect(inout TriangleStream<blit_v2f> stream, float left, float wide, float width, float top, float rows, float height) {
    float x0 = 2.0 * left / width - 1.0, x1 = 2.0 * (left + wide) / width - 1.0;
    float y0 = 1.0 - 2.0 * top / height, y1 = 1.0 - 2.0 * (top + rows) / height;
    blit_v2f o;
    o.vertex = float4(x0, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(x1, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(x0, y1, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(x1, y1, 0.5, 1.0); stream.Append(o);
    stream.RestartStrip();
}
// the window's row r, where the state has it
void window_run(inout TriangleStream<blit_v2f> stream, uint r) {
    uint t = window_texel(r);
    machine_rect(stream, (float)(t % WINDOW_STATE_W), WINDOW_W, WINDOW_STATE_W, 64.0 + t / WINDOW_STATE_W, 1.0, WINDOW_STATE_H);
}
