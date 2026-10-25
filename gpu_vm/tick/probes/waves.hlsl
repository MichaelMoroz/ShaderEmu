// The wave probe (gpu_vm/tick/WAVES.md): which pixels of the tick's draw share a wave, and what padding costs.
// waves.cpp draws a grid of cores (a block bw x bh at the top left of a cell cw x ch, perRow cells a row) three ways:
// one triangle over everything (geo 0), one triangle a core as c8's vert_cull (geo 1: 2bw x 2bh, its pixels outside the
// block discarded), or one quad a core (geo 2). Shader model 6: the wave intrinsics.
cbuffer P : register(b0) {
    uint geo, bw, bh, cw, ch, perRow, cores, work, pad, alt, w, h;
};
RWByteAddressBuffer Out : register(u1);   // map: [0] wave counter, [1] records; then records of 4 words

struct v2f { float4 pos : SV_Position; nointerpolation uint core : CORE; float2 clip : SV_ClipDistance0; };

// geo 3: geo 1's triangle cut to the block by two clip distances (bw - x, bh - y), so nothing outside it is rasterized
v2f vs(uint id : SV_VertexID) {
    v2f o;
    if (geo == 4) {   // one quad over a run of cores side by side: perRow / pad of them (pad 0: the whole row)
        uint run = pad == 0 ? perRow : perRow / pad, r = id / 6, q = id % 6 < 3 ? id % 6 : id % 6 - 2;
        uint row = r / (perRow / run), first = (r % (perRow / run)) * run;
        float2 a = float2(first * cw, row * ch), b = a + float2(run * cw, bh);
        float2 p4 = float2((q == 1 || q == 3) ? b.x : a.x, q >= 2 ? b.y : a.y);
        o.pos = float4(p4.x / w * 2 - 1, 1 - p4.y / h * 2, 0, 1);
        o.core = 0xffffffffu;
        o.clip = float2(1, 1);
        return o;
    }
    uint g = geo == 3 ? 1 : geo;
    uint c = g == 0 ? 0xffffffffu : g == 1 ? id / 3 : id / 6, k = g == 1 ? id % 3 : id % 6;
    float2 p, at = 0;
    o.clip = float2(1, 1);
    if (g == 0) p = float2(id == 1 ? 2.0 * w : 0.0, id == 2 ? 2.0 * h : 0.0);   // a triangle over the target
    else {
        at = float2((c % perRow) * cw, (c / perRow) * ch);
        if (g == 1) {
            p = at + float2(k == 1 ? 2.0 * bw : 0.0, k == 2 ? 2.0 * bh : 0.0);
            if (geo == 3) o.clip = float2(bw, bh) - (p - at);
        }
        else { uint q = k < 3 ? k : k - 2; p = at + float2((q == 1 || q == 3) ? bw : 0.0, (q >= 2) ? bh : 0.0); }   // 0 1 2, 1 3 2
    }
    o.pos = float4(p.x / w * 2 - 1, 1 - p.y / h * 2, 0, 1);
    o.core = c;
    return o;
}

// the core a pixel belongs to, or 0xffffffff for padding (outside every block, or outside its own triangle's block)
uint owner(v2f i) {
    uint2 x = uint2(i.pos.xy);
    uint c = (x.y / ch) * perRow + x.x / cw;
    bool inside = x.x % cw < bw && x.y % ch < bh && x.x / cw < perRow && c < cores && (geo == 0 || geo == 4 || i.core == c);
    return inside ? c : 0xffffffffu;
}

// map: every invocation appends (wave, pixel, core, lane | lanes active << 8)
uint4 ps_map(v2f i) : SV_Target {
    uint c = owner(i);
    uint id = 0;
    if (WaveIsFirstLane()) Out.InterlockedAdd(0, 1, id);
    id = WaveReadLaneFirst(id);
    uint n = WaveActiveCountBits(true), lane = WaveGetLaneIndex(), slot;
    Out.InterlockedAdd(4, 1, slot);
    uint2 x = uint2(i.pos.xy);
    if (slot < (w * h * 3)) Out.Store4(16 + slot * 16, uint4(id, x.y * w + x.x, c, lane | n << 8));
    return uint4(id, c, lane, n);
}

// timing: a core's pixels run `work` steps of a hash (alt: odd cores work, even cores work / 16); padding per `pad`
// (0 discard at the top, 1 return at the top). The loop is capped by work's own bound.
uint4 ps_time(v2f i) : SV_Target {
    uint c = owner(i);
    if (c == 0xffffffffu) { if (pad == 0) discard; return 0; }
    uint n = alt != 0 && (c & 1) == 0 ? work / 16 : work, x = c * 0x9e3779b9u + uint(i.pos.x);
    [loop] for (uint k = 0; k < n && k < 1000000; k++) x = (x ^ (x >> 15)) * 0x2c1b3c6du + k;
    return uint4(x, c, 0, 0);
}
