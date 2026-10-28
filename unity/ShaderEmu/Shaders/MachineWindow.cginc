// The rows of RAM the control pass writes (Machine.shader's GPUControl), drawn into a window
// of their own WINDOW_ROWS high and copied into the state by the WindowBack pass, as no pass
// reads what it draws: the control words, the counters' totals, then the row of the console,
// the network and the core statistics, and a fetched page's eight (src/gpu.cginc).
#define WINDOW_ROWS 11
#define WINDOW_CONTROL (64 + 0x100000 / 2048)
#define WINDOW_COUNTS (64 + 0x0c1000 / 2048)
#define WINDOW_NET (64 + 0x16b800 / 2048)
uint window_state_row(uint r) {
    return r == 0 ? WINDOW_CONTROL : r == 1 ? WINDOW_COUNTS : WINDOW_NET + (r - 2);
}

// A quad of rows top to top + rows of a target `height` rows high (row 0 is clip y = 1).
void machine_rows(inout TriangleStream<blit_v2f> stream, float top, float rows, float height) {
    float y0 = 1.0 - 2.0 * top / height, y1 = 1.0 - 2.0 * (top + rows) / height;
    blit_v2f o;
    o.vertex = float4(-1.0, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(1.0, y0, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(-1.0, y1, 0.5, 1.0); stream.Append(o);
    o.vertex = float4(1.0, y1, 0.5, 1.0); stream.Append(o);
    stream.RestartStrip();
}
