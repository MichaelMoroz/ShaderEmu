// The pass that copies the tick's eight targets into the state texture (TICK_UNPACK, the harness's; the world has its own,
// Machine.shader): after tick.hlsl.
// A quad for core 0's rectangle, and one for each worker with something to run (16 a primitive).
void quad(inout TriangleStream<v2f> s, float x, float y, float w, float h, float2 size) {
    for (uint k = 0; k < 4; k++) {
        float2 p = float2(x + (k & 1) * w, y + (k >> 1) * h) / size;
        v2f o; o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
        s.Append(o);
    }
    s.RestartStrip();
}
[maxvertexcount(128)]
void tick_geom(triangle v2f IN[3], uint prim : SV_PrimitiveID, inout TriangleStream<v2f> s) {
    if (prim > (CORES + 15) / 16) return;
    float2 size = float2(TICK_W, STATE_TEX_HEIGHT);   // (the state texture)
    [branch] if (prim == (CORES + 15) / 16) {
        if (_Init || _LaneField == 0) return;
        uint4 job = TEX(RAM_ADDR(FIELD_JOB_T));
        if (job.x == 0) return;
        for (uint b = 0; b < 32; b++) quad(s, b * 64 + 40, 0, 16, 64, size);   // (tiles 5 and 6 of every slot: the first eight's are the cores' again)
        return;
    }
    geo_read();
    select_core(0);
    if (prim == 0) quad(s, 0, 0, 64, rows_now, size);
    if (_Init) return;
    if ((g.r & 1) != 0) { if (prim == 0) { quad(s, 64, 0, 1984, 8, size); quad(s, 0, 8, 2048, 56, size); } return; }
    for (uint n = 0; n < 16; n++) {
        uint k = prim * 16 + n;
        if (k == 0 || k > g_count) continue;
        select_core(k);
        if (worker_idle()) continue;   // (its state stays as the commit left it)
        quad(s, (k & 31) << 6, (k >> 5) << 3, rows_now * 8, 8, size);
    }
}

Texture2D<uint4> _TickOut0, _TickOut1, _TickOut2, _TickOut3, _TickOut4, _TickOut5, _TickOut6, _TickOut7;
uint4 frag(v2f i) : SV_Target {
    uint2 p = uint2(i.vertex.xy);
    uint k = (p.x >> 6) | ((p.y >> 3) << 5), r = p.y & 7, tile = (p.x & 63) >> 3;
    uint3 at = uint3((k << 3) + tile, r, 0);
    [branch] if (k >= 8 && tile >= 5 && tile < 7) {
        // the field's: lane F's texel, from its pixel in the targets (one of a row the tick did not draw: as it is)
        uint4 job = TEX(RAM_ADDR(FIELD_JOB_T));
        uint F = ((k - 8) << 4) | ((tile - 5) << 3) | r;
        if ((F >> 11) >= (min(job.z, (uint)FIELD_MAX) + 2047) >> 11) return TEX(p);
        at = uint3(F & 2047, 8 + (F >> 11), 0);
    } else if (k != 0) {
        geo_read();
        if (k > g_count) return 0;
    }
    k = p.x & 7;
    [branch] if (k < 4) return k == 0 ? _TickOut0.Load(at) : k == 1 ? _TickOut1.Load(at) : k == 2 ? _TickOut2.Load(at) : _TickOut3.Load(at);
    return k == 4 ? _TickOut4.Load(at) : k == 5 ? _TickOut5.Load(at) : k == 6 ? _TickOut6.Load(at) : _TickOut7.Load(at);
}
