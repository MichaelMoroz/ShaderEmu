// The commit: what the cores stored in a pass goes into RAM, and their state rows are made ready
// for the next (README.md). The harness draws it three ways (rvc_backend11.cpp):
//   (plain)        the state rows, and the 4 MB bands of RAM something other than a core changed:
//                  the GPU's picture copied back, its control words, the host's zap; at _Init all
//                  of RAM, from the image
//   COMMIT_POINTS  a point for each bucket of a core's write cache, the store a full cache had no
//                  room for, and a copy or fill's rows: commit_geom makes each the texels it covers
//   COMMIT_COPY    the same points into the other buffer, as copies of what the first draw made

Texture2D<uint4> _SelfTexture2D;
Texture2D<float4> _Data_RAM_R, _Data_RAM_G, _Data_RAM_B, _Data_RAM_A, _Data_RAM_RAW;
Texture2D<float4> _Data_ROM;
Texture2D<float4> _GpuTarget;   // the GPU device's picture (docs/machine/gpu.md)
Texture2D<uint4> _TickOut0, _TickOut1, _TickOut2, _TickOut3, _TickOut4, _TickOut5, _TickOut6, _TickOut7;
uniform float _Init, _InitRaw;
// the host's hand in RAM for one commit (docs/world/danger.md)
uniform uint _CommitAlways;   // bands 0 to 31 drawn every commit (the control words')
uniform uint _ZapKind, _ZapX, _ZapY, _ZapR, _ZapWord, _ZapMaskLo, _ZapMaskHi, _ZapSeed, _ZapZones;

#ifndef CORES
#define CORES 256
#endif
#define TEX(p) _SelfTexture2D[p]
#define RAM_W 8192u   // the state texture's texels a row (tick.hlsl)
#define RAM_ADDR(t) uint2((t) % RAM_W, 64 + (t) / RAM_W)
#ifndef STATE_TEX_HEIGHT
#define STATE_TEX_HEIGHT 2368u   // the state texture's rows: 64 of state, then RAM to 0x92000000
#endif
#define RAM_TEXELS (RAM_W * (STATE_TEX_HEIGHT - 64u))
#define CTRL_ROW (64u + 0x100000u / RAM_W)   // the control words' row (0x81000000, its first 2,048 texels), the control pass's
#define L1_AT 44
#define FP_AT 492
#ifndef CORE0_BITS
#define CORE0_BITS 6
#endif
#define STATE_ROWS 16
#define BUCKETS(bits) ((7u << (bits)) >> 2)
#define H1(t, n) ((((t) >> 3) ^ ((t) << ((n) - 3)) ^ ((t) >> (n))) & ((1u << (n)) - 1))
#define H2(t, n) ((((t) >> 2) ^ ((t) << ((n) - 2)) ^ ((t) >> ((n) + 3))) & ((1u << (n)) - 1))
#define STALL_COPY 8
#define STALL_FILL 10
#define GPU_DISPLAY 0x100000u   // display: mode, width, height
#define GPU_CLOCK 0x100003u     // word 2: copies made so far
#define GPU_COPY 0x100007u      // the copy still to make of the list drawn last: how, address, width | height << 16, row
#define GPU_PIXELS 0x100100u    // the display's framebuffer
#define SUBMIT_INTO 4

// ---- The geometry, as the tick has it (tick.hlsl)
#define GEO_TEXEL 0x10003du
#define STRIP_TILES 248
#define STRIP_BANDS ((CORES + 47) / 49)
#define TICK_W 1040
#define TICK_GAP 8
#define TICK_COLS (TICK_W - 8 - TICK_GAP)
#define TICK_BANDS ((CORES + 126) / 128)
static uint4 g;
static uint g_count, g_rows, g_cols, g_nb, g_bb, g_rows_b, g_cols_b, g_per0;
static uint core, bits, tile0;
uint rows_of(uint b) { return b == 0 ? 0 : (L1_AT + 8 + BUCKETS(b) * 4 + 63) / 64; }
uint cols_wide(uint rows, uint block, uint pad) { return (rows + pad + block - 1) / block * block; }
void geo_read() {
    g = TEX(RAM_ADDR(GEO_TEXEL));
    uint ws = (g.a >> 16) & 0xff, wb = g.a >> 24;
    g_rows = g.b < 3 || g.b > 6 ? 0 : rows_of(g.b);
    g_cols = ws != 0 && ws <= 64 ? cols_wide(g_rows, ws, 0) : cols_wide(g_rows, 4, 2);
    g_count = 0; g_nb = 0; g_per0 = 0;
    g_bb = (g.a >> 8) & 0xff;
    if ((g.a & 0xff) != 0 && g_bb >= 3 && g_bb <= 6 && g_rows != 0) {
        g_rows_b = rows_of(g_bb);
        g_cols_b = wb != 0 && wb <= 64 ? cols_wide(g_rows_b, wb, 0) : cols_wide(g_rows_b, 8, 0);
        g_nb = min(min(min(g.a & 0xff, STRIP_TILES / g_rows_b), TICK_COLS / g_cols_b), min(g.g, (uint)CORES - 1));
        if (g_nb != 0) g_per0 = (TICK_COLS - g_nb * g_cols_b) / g_cols;
    }
    uint sb0 = g_nb != 0 ? 1 : 0;
    if (g_rows != 0 && (g.r & 1) == 0)
        g_count = g_nb + min(min(g.g, (uint)CORES - 1) - g_nb,
                             min((STRIP_BANDS - sb0) * (STRIP_TILES / g_rows), (TICK_BANDS - sb0) * (TICK_COLS / g_cols) + g_per0));
}
void select_core(uint k) {
    core = k;
    bool big = k != 0 && k <= g_nb;
    bits = k == 0 ? CORE0_BITS : big ? g_bb : k <= g_count ? g.b : 0;
    uint j = k - 1 - g_nb, per = STRIP_TILES / max(g_rows, 1);
    tile0 = k == 0 ? 0 : big ? (k - 1) * g_rows_b : ((g_nb != 0 ? 1 : 0) + j / per) * STRIP_TILES + (j % per) * g_rows;
}
uint2 tile_at(uint t) { return uint2(64 + 8 * (t % STRIP_TILES), 8 * (t / STRIP_TILES)); }
uint2 block_of(uint k) {
    if (k <= g_nb) return uint2(8 + TICK_GAP + (k - 1) * g_cols_b, 0);
    uint j = k - 1 - g_nb, per = TICK_COLS / max(g_cols, 1), sb0 = g_nb != 0 ? 1 : 0, below = (TICK_BANDS - sb0) * per;
    if (j >= below) return uint2(8 + TICK_GAP + g_nb * g_cols_b + (j - below) * g_cols, 0);
    return uint2(8 + TICK_GAP + (j % per) * g_cols, (sb0 + j / per) * STATE_ROWS);
}
uint core_at(uint t) {
    uint band = t / STRIP_TILES, i, k;
    if (g_nb != 0 && band == 0) {
        i = (t % STRIP_TILES) / max(g_rows_b, 1);
        if (i >= g_nb) return 0;
        k = i + 1;
    } else {
        uint per = STRIP_TILES / max(g_rows, 1);
        i = (t % STRIP_TILES) / max(g_rows, 1);
        if (i >= per) return 0;
        k = g_nb + (band - (g_nb != 0 ? 1 : 0)) * per + i + 1;
    }
    if (k > g_count) return 0;
    select_core(k);
    return k;
}
// texel lin (core 0's numbering) of the selected core's state
uint4 st(uint lin) {
    uint end = L1_AT + BUCKETS(bits) * 4, w = lin >= FP_AT ? end + lin - FP_AT : lin;
    if (lin >= end && lin < FP_AT) return 0;
    if (core == 0) return TEX(uint2(w & 63, w >> 6));
    return TEX(tile_at(tile0 + (w >> 6)) + uint2(w & 7, (w & 63) >> 3));
}
uint idx(uint4 v, uint i) { return (i & 2) ? ((i & 1) ? v.w : v.z) : ((i & 1) ? v.y : v.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
uint unpack(float4 f) { return uint(f.a * 255.0 + 0.5) << 24 | uint(f.b * 255.0 + 0.5) << 16 | uint(f.g * 255.0 + 0.5) << 8 | uint(f.r * 255.0 + 0.5); }

// The selected core's write cache, as the tick left it in its state: RAM texel t, if it holds it.
bool cached(uint t, out uint4 texel) {
    texel = 0;
    [loop] for (uint k = 0; k < 3; k++) {
        uint n = 1u << bits, b = k == 0 ? t & (n - 1) : k == 1 ? n + H1(t, bits - 1) : n + (n >> 1) + (H2(t, bits - 1) & ((n >> 2) - 1));
        uint4 tags = st(L1_AT + 4 * b);
        uint e = tags.x == t + 1 ? 1 : tags.y == t + 1 ? 2 : tags.z == t + 1 ? 3 : 0;
        if (e != 0) { texel = st(L1_AT + 4 * b + e); return true; }
        if (tags.z == 0) return false;
    }
    return false;
}
// a copy's source word: RAM as the core's cache or the texture has it, or the ROM
uint source_word(uint a) {
    [branch] if (a & 0x80000000) {
        uint4 c;
        uint t = (a & 0x7fffffff) >> 4;
        return idx(cached(t, c) ? c : TEX(RAM_ADDR(t)), (a >> 2) & 3);
    }
    [branch] if ((a & 0x40000000) == 0) return 0;
    uint t = (a - 0x40000000) >> 4, i = (a >> 2) & 3;
    uint2 p = uint2(t % RAM_W, t / RAM_W);
    return asuint(_Data_ROM[p])[i];   // (the ROM: a texel a pixel)
}
// RAM texel at pos as the selected core's pass leaves it: its copy or fill, or its cache's texel
// and the one store the cache had no room for. False when the core stored nothing there.
bool core_texel(uint2 pos, inout uint4 r) {
    uint o = ((pos.y - 64) * RAM_W + pos.x) << 4, bloom = st(41).x, stall = st(28).x;
    uint4 t37 = st(37), t8 = st(8);
    uint dst = t37.w & 0x7fffffff, n = st(38).z;
    bool op = stall == STALL_COPY || stall == STALL_FILL;
    if (core != 0 && bloom == 0 && !op) return false;
    [branch] if (op && o + 12 >= dst && o < dst + n) {
        // (a copy or fill is the last thing of a pass: it lies over the cache)
        [loop] for (uint k = 0; k < 4; k++)
            if (o + 4 * k >= dst && o + 4 * k < dst + n) r = put(r, k, stall == STALL_COPY ? source_word(t37.z + o + 4 * k - dst) : t37.z);
        return true;
    }
    if (core != 0 && ((o & ~bloom) != 0)) return false;
    uint4 c;
    [branch] if ((o & bloom) == o && cached(o >> 4, c)) r = c;
    [branch] if (t8.x - o < 16 && (t8.x & 3) == 0) r = put(r, (t8.x >> 2) & 3, t8.y);
    return true;
}

// ---- The GPU device's copy of a drawn list into RAM (docs/machine/gpu.md), and the host's zap
uint4 ram(uint t) { return TEX(RAM_ADDR(t)); }
// 4 MB bands lo to hi, a bit each, band k at bit k mod 32
uint ram_bands(uint lo, uint hi) {
    if (hi < lo) return 0;
    if (hi - lo >= 31) return 0xffffffff;
    uint m = (2u << (hi - lo)) - 1;
    return (m << (lo & 31)) | (m >> ((32 - (lo & 31)) & 31));
}
uint copy_bands() {
    uint4 c = ram(GPU_COPY), d = ram(GPU_DISPLAY);
    if (c.r == 0) return 0;
    uint first = (c.r & SUBMIT_INTO) ? c.g & 0x7ffffffc : GPU_PIXELS * 16;
    uint bytes = (c.r & SUBMIT_INTO) ? max(c.a, c.b & 0xffff) * (c.b >> 16) * 4 : d.g * d.b * 4;
    return ram_bands(first >> 22, min((first + bytes) >> 22, (RAM_TEXELS * 16 - 1) >> 22));
}
bool writeback(uint2 pos, out uint4 r) {
    r = 0;
    uint t = (pos.y - 64) * RAM_W + pos.x;
    uint4 c = ram(GPU_COPY);
    if (pos.y < 64 || t >= 0x1e0000u || c.r == 0) return false;   // (what a GPU program may own: 0x80000000 to 0x81e00000)
    if (t == GPU_COPY) return true;                          // made: cleared
    if (t == GPU_CLOCK) { r = TEX(pos); r.b += 1; return true; }
    uint4 d = ram(GPU_DISPLAY);
    bool into = (c.r & SUBMIT_INTO) != 0;
    uint first = into ? (c.g & 0x7fffffff) >> 2 : GPU_PIXELS * 4, width = into ? c.b & 0xffff : d.g, height = into ? c.b >> 16 : d.b;
    uint row = into ? max(c.a, width) : width, word = t * 4;
    if (width == 0 || word + 3 < first || word >= first + row * height) return false;
    r = TEX(pos);
    bool any = false;
    for (uint k = 0; k < 4; k++) {
        uint p = word + k - first, y = p / row, x = p - y * row;
        if (word + k >= first && x < width && y < height) {
            uint3 v = (uint3)(_GpuTarget.Load(int3(x, y, 0)).rgb * 255.0 + 0.5);
            r = put(r, k, (v.r << 16) | (v.g << 8) | v.b);
            any = true;
        }
    }
    return any;
}
uint zap_hash(uint n) { n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; n ^= n >> 16; return n; }
uint zap_bands() {
    uint reach = _ZapKind == 2 ? _ZapR : 0;
    return _ZapKind == 0 ? 0 : ram_bands((_ZapY - min(_ZapY, reach)) >> 5, min((_ZapY + reach) >> 5, (STATE_TEX_HEIGHT - 65) >> 5));   // (32 rows a band)
}
uint4 zap(uint2 pos, uint4 r) {
    uint row = pos.y - 64, reach = _ZapKind == 2 ? _ZapR : 0, mb = row >> 3, mask = _ZapMaskLo | _ZapMaskHi << 16;
    uint zone = mb < 9 ? 0 : mb < 96 ? 1 : mb < 112 ? 2 : mb < 119 ? 3 : mb < 123 ? 4 : 5;   // whose a row is
    if (_ZapKind == 0 || pos.x + reach < _ZapX || pos.x > _ZapX + reach || row + reach < _ZapY || row > _ZapY + reach) return r;
    if (((_ZapZones >> zone) & 1) == 0) return r;
    if (_ZapKind == 1) return r ^ uint4(_ZapWord == 0 ? mask : 0, _ZapWord == 1 ? mask : 0, _ZapWord == 2 ? mask : 0, _ZapWord == 3 ? mask : 0);
    uint seed = zap_hash(pos.x + pos.y * RAM_W + _ZapSeed * 0x9e3779b9u);
    return r ^ (uint4(zap_hash(seed), zap_hash(seed + 1), zap_hash(seed + 2), zap_hash(seed + 3)) & mask);
}
// the bands the plain draw makes whole: all at _Init, else what the host changes
uint bands() { return _Init ? 0xffffffff : zap_bands(); }
// the rows of the texture the GPU's picture copied back covers, first | last << 16 (0: none); the
// plain draw makes those rows of this commit's and the last one's (core 0's texel 39.x), not their bands
uint copy_rows() {
    uint4 c = ram(GPU_COPY), d = ram(GPU_DISPLAY);
    if (c.r == 0 || _Init) return 0;
    uint first = (c.r & SUBMIT_INTO) ? c.g & 0x7ffffffc : GPU_PIXELS * 16;
    uint bytes = (c.r & SUBMIT_INTO) ? max(c.a, c.b & 0xffff) * (c.b >> 16) * 4 : d.g * d.b * 4;
    if (bytes == 0 || first >= RAM_TEXELS * 16) return 0;
    uint last = min(first + bytes - 1, RAM_TEXELS * 16 - 1);
    return (64 + (first >> 17)) | ((64 + (last >> 17)) << 16);   // (128 KB a row)
}

struct v2f { float4 vertex : SV_POSITION; nointerpolation uint core : TEXCOORD0; };
v2f commit_vert(uint id : SV_VertexID) {
    v2f o;
    o.core = id;
    o.vertex = float4(0, 0, 0, 1);
#ifndef COMMIT_POINTS
    // the state rows (as wide as the cores' block), and each band of RAM that is drawn (quad 0, then bands of 32 rows)
    static const float2 corners[6] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}};
    uint q = id / 6;
    float2 c = corners[id % 6], size;
    _SelfTexture2D.GetDimensions(size.x, size.y);
    float top = q == 0 ? 0.0 : 64.0 + (q - 1) * 32.0, rows = q == 0 ? 64.0 : 32.0;
    core = 0;
    bool on = q == 0 || (((bands() | TEX(uint2(41, 0)).z) >> ((q - 1) & 31)) & 1) != 0;
    // a band otherwise only of the picture copy (this commit's and the last's) and the control words
    // (_CommitAlways: the control pass's row, CTRL_ROW): those rows of it, not all 32
    uint now = copy_rows(), was = TEX(uint2(39, 0)).x, ctrl = q - 1 < 32 && ((_CommitAlways >> (q - 1)) & 1) != 0 ? CTRL_ROW | CTRL_ROW << 16 : 0;
    [branch] if (!on && (now | was | ctrl) != 0) {
        uint lo = min(min(now != 0 ? now & 0xffff : 0xffff, was != 0 ? was & 0xffff : 0xffff), ctrl != 0 ? CTRL_ROW : 0xffff);
        uint hi = max(max(now >> 16, was >> 16), ctrl >> 16);
        float a = max(top, (float)lo), b = min(top + rows, (float)hi + 1.0);
        if (a < b) { on = true; top = a; rows = b - a; }
    }
    float2 uv = float2(c.x * (q == 0 ? 2048.0 / size.x : 1.0), (top + c.y * rows) / size.y);
    o.vertex = on ? float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0) : float4(2.0, 2.0, 0.0, 1.0);
#endif
    return o;
}

#ifdef COMMIT_POINTS
// texel w of a core's state as the tick left it, in its targets
uint4 tick_state(uint k, uint w) {
    uint3 p = uint3(k == 0 ? uint2((w & 63) >> 3, w >> 6) : block_of(k) + uint2(w >> 6, (w & 63) >> 3), 0);
    uint i = w & 7;
    [branch] if (i < 4) return i == 0 ? _TickOut0.Load(p) : i == 1 ? _TickOut1.Load(p) : i == 2 ? _TickOut2.Load(p) : _TickOut3.Load(p);
    return i == 4 ? _TickOut4.Load(p) : i == 5 ? _TickOut5.Load(p) : i == 6 ? _TickOut6.Load(p) : _TickOut7.Load(p);
}
void rect(inout TriangleStream<v2f> s, uint k, uint x, uint y, uint w, uint h) {
    float2 size; _SelfTexture2D.GetDimensions(size.x, size.y);
    for (uint n = 0; n < 4; n++) {
        float2 p = float2(x + (n & 1) * w, y + (n >> 1) * h) / size;
        v2f o; o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); o.core = k;
        s.Append(o);
    }
    s.RestartStrip();
}
// RAM texels t0 to t1, row by row
void rows(inout TriangleStream<v2f> s, uint k, uint t0, uint t1) {
    uint y0 = t0 / RAM_W, y1 = t1 / RAM_W, x0 = t0 % RAM_W, x1 = t1 % RAM_W;
    if (t0 > t1) return;
    if (y0 == y1) { rect(s, k, x0, 64 + y0, x1 - x0 + 1, 1); return; }
    rect(s, k, x0, 64 + y0, RAM_W - x0, 1);
    rect(s, k, 0, 64 + y1, x1 + 1, 1);
    if (y1 > y0 + 1) rect(s, k, 0, 64 + y0 + 1, RAM_W, y1 - y0 - 1);
}
// point n: core 0's bucket n (256 places), its store with no room, its copy; from 258, 130 a worker
[maxvertexcount(12)]
void commit_geom(point v2f IN[1], inout TriangleStream<v2f> s) {
    uint id = IN[0].core, k = 0, e = id, last;
    geo_read();
    if (id >= 258) {
        k = 1 + (id - 258) / 130;
        e = (id - 258) % 130;
        if (k >= CORES) return;
    }
    select_core(k);
    if (bits == 0) return;
    last = k == 0 ? 256 : 128;
    if (e < last) {
        if (e >= BUCKETS(bits)) return;
        uint4 tags = tick_state(k, L1_AT + 4 * e);
        for (uint n = 0; n < 3; n++) {
            uint tag = idx(tags, n);
            if (tag != 0 && tag - 1 < RAM_TEXELS) rect(s, k, (tag - 1) % RAM_W, 64 + (tag - 1) / RAM_W, 1, 1);
        }
    } else if (e == last) {
        uint t = tick_state(k, 8).x >> 4;
        if (t < RAM_TEXELS) rect(s, k, t % RAM_W, 64 + t / RAM_W, 1, 1);
    } else {
        uint stall = tick_state(k, 28).x, dst = tick_state(k, 37).w & 0x7fffffff, n = tick_state(k, 38).z;
        if ((stall == STALL_COPY || stall == STALL_FILL) && n != 0) rows(s, k, dst >> 4, min((dst + n - 1) >> 4, RAM_TEXELS - 1));
    }
}
#endif

uint4 frag(v2f i) : SV_Target {
    uint2 pos = uint2(i.vertex.xy);
#ifdef COMMIT_COPY
    return TEX(pos);   // (into the buffer the first draw did not draw into)
#endif
    [branch] if (_Init && _InitRaw) {
        float2 size; _SelfTexture2D.GetDimensions(size.x, size.y);
        float3 w[6];
        for (uint k = 0; k < 6; k++) w[k] = _Data_RAM_RAW[pos + uint2((k * (uint)size.x) % ((uint)size.x * 3), (k / 3) * (uint)size.y)].rgb;
        #define B(v) uint((v) * 255.0 + 0.5)
        return uint4(B(w[0].r) | B(w[1].r) << 8 | B(w[2].r) << 16 | B(w[3].r) << 24, B(w[0].g) | B(w[1].g) << 8 | B(w[2].g) << 16 | B(w[3].g) << 24,
                     B(w[0].b) | B(w[1].b) << 8 | B(w[2].b) << 16 | B(w[3].b) << 24, B(w[4].r) | B(w[4].g) << 8 | B(w[4].b) << 16 | B(w[5].r) << 24);
    }
    uint4 r;
    if (writeback(pos, r)) return r;
    geo_read();
    select_core(0);
    [branch] if (pos.y < 64) {
        // a core's state: its pass counted, its stall over, its stores' addresses cleared
        uint lin = pos.x + 64 * pos.y;
        r = TEX(pos);
        [branch] if (pos.x >= 64) {
            uint tile = ((pos.x - 64) >> 3) + STRIP_TILES * (pos.y >> 3);
            if (core_at(tile) == 0) return 0;
            uint w = ((tile - tile0) << 6) + ((pos.y & 7) << 3) + (pos.x & 7), end = L1_AT + BUCKETS(bits) * 4;
            lin = w < end ? w : w < end + 8 ? FP_AT + w - end : 4095;
            if (lin == 8) r.x = 0xffffffff;   // (a worker's store with no room is made now, and not again)
        }
        if (lin == 28) { r.z += _Init ? 0 : 1; r.x = r.x == 3 || r.x == 7 || r.x == 8 || r.x == 9 || r.x == 10 ? 0 : r.x; }
        if (lin == 41) { r.x = 0; if (core == 0) r.z = bands(); }
        if (lin == 39 && core == 0) r.x = copy_rows();   // (core 0's .b: the bands of this commit, for the next)
        return r;
    }
    r = TEX(pos);
    [branch] if (_Init) {
        // RAM from the image, rows bottom up
        uint2 d; _Data_RAM_A.GetDimensions(d.x, d.y);
        uint2 p = uint2(pos.x, d.y - 1 - (pos.y - 64));
        return pos.y - 64 >= d.y ? (uint4)0 : uint4(unpack(_Data_RAM_R[p]), unpack(_Data_RAM_G[p]), unpack(_Data_RAM_B[p]), unpack(_Data_RAM_A[p]));
    }
#ifdef COMMIT_POINTS
    // a point: one core's store (two cores store to one texel in no pass); a core whose pass
    // stored nothing here leaves the texel to whoever did
    select_core(min(i.core, g_count));
    if (!core_texel(pos, r)) discard;
    return r;
#else
    return zap(pos, r);
#endif
}
