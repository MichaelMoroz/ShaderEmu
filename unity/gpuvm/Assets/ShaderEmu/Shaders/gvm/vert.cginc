// The tick's vertex shader, the same text and bytes in GvmTick.shader and GvmTick0.shader (the harness draws both with
// one: a draw that changes more than the pixel shader did not overlap core 0's with the workers'). It reads no uniform.
// Vertices 0-5: a triangle a band of 128 cores, as the harness's viewport over each band: its long edge leaves the
// band's GEO_BH rows at the target's right edge, and the pixels it covers under them (ry >= GEO_BH) end at once (a
// quad a core was two triangles: the 2 x 2 quads its diagonal cut ran twice, helper lanes and all). With c0nb the
// first band starts at x 8. Vertices 6-11 (GvmTick0's mesh): core 0's block, 8 wide and as many rows as its pixels take.
v2f tick_vert(uint id : SV_VertexID) {
    float x, y;
    if (id < 6) {
        uint b = id / 3, v = id % 3;
        float x0 = (b == 0 && GVM_HAS_CORE0) ? 8 : 0, w = 1024 - x0;
        x = x0 + (v == 1 ? 2 * w : 0);
        y = b * GEO_BAND + (v == 2 ? 2 * GEO_BH : 0);
    } else {
#if GVM_HAS_CORE0
        uint spix0 = (NTEX0 + 7) / 8, rows = (spix0 + 8 * GEO_BH - 1) / (8 * GEO_BH) * GEO_BH;
#else
        uint rows = GEO_BH;
#endif
        uint v = (id - 6) % 6;
        x = (v == 1 || v == 3 || v == 4) ? 8 : 0;
        y = (v == 2 || v == 4 || v == 5) ? rows : 0;
    }
    v2f o;
    o.vertex = id < 12 ? float4(x / 1024.0 * 2 - 1, 1 - y / 24.0 * 2, 0, 1) : float4(-2, -2, 0, 1);
    return o;
}
