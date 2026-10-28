// ram.h: RAM in one texture, written in place (RAM_INPLACE; docs/machine/gpu.md, "RAM in one
// texture"): the commit's gather, state and RAM passes, none of which reads what it writes.
// The includer declares _TickState, _Stage, _GpuTarget and the payload and ROM textures, and
// maps STATE_TEX to _TickState and RAM_TEX to _Ram (the gather) or ram_stage_read.
#ifndef RAM_H
#define RAM_H

#define RAM_TEXELS (2048u * (STATE_TEX_HEIGHT - 64u))
// The stage: rows 0-7 a copy's chunk (256 KB: a longer copy takes a pass a chunk), row 8 three
// texels a core (its lone store's texel; its fill's or ROM copy's first and last texels) and
// the head, row 9 the control words before the commit, rows 10-11 the picture's part texels
// (two a row of the picture), rows 12-20 the zap's square (up to 129 x 129).
#define STAGE_COPY_ROWS 8
#define STAGE_CHUNK (STAGE_COPY_ROWS * 2048u)
#define STAGE_SLOT_ROW 8
#define STAGE_HEAD_X 1024
#define STAGE_CONTROL_ROW 9
#define STAGE_PICTURE_ROW 10
#define STAGE_ZAP_ROW 12
#define STAGE_ROWS 21
#define STAGE_ZAP_REACH 64
#define CONTROL_ROW (64 + 0x700000 / 2048)

#ifndef RAM_GEOM_GUARD
#define RAM_GEOM_GUARD
#endif
#ifdef LANE_FIELD
#define LANE_FIELD_ROW 48   // the lanes' field's texels: state rows 48 to 63 (FIELD_STATE_ROW, rvc_lean's tick)
#endif

// RAM as the commit's passes other than the gather have it: the control words alone.
uint4 ram_stage_read(uint2 pos) {
    return pos.y == CONTROL_ROW ? _Stage[uint2(pos.x, STAGE_CONTROL_ROW)] : (uint4)0;
}

// Texel w of a core's state as the tick left it, in the layout of its own rows.
uint4 tick_state(uint core, uint w) {
#if CORES > 1
    if (core != 0) return _TickState[mc_tile_at(mc_tile_of(core) + (w >> 6)) + uint2(w & 7, (w & 63) >> 3)];
#endif
    return _TickState[uint2(w & 63, w >> 6)];
}

// A core's fill or copy (CSRs 0x0b0-0x0b3) at the end of its pass. A copy from RAM is staged:
// the gather copies a chunk of its source into the stage and the RAM commit from there.
#define RAM_OP_NONE 0
#define RAM_OP_STAGED 1
#define RAM_OP_ROM 2
#define RAM_OP_FILL 3
struct ram_op {
    uint kind, dst, src, n;   // dst a RAM offset; src an address, or a fill's word
};
ram_op ram_op_of(uint core) {
    ram_op op;
    op.kind = RAM_OP_NONE;
    uint stalled = tick_state(core, 28).r;
    uint4 t37 = tick_state(core, 37), t38 = tick_state(core, 38);
#ifdef CSR_FIELDS
    op.src = t37.b;
    op.dst = t37.a & 0x7fffffff;
#else
    op.src = stalled == STALL_MEMOP_FILL ? t37.b : t38.r;
    op.dst = t38.g & 0x7fffffff;
#endif
    op.n = t38.b;
    if ((stalled == STALL_MEMOP_COPY || stalled == STALL_MEMOP_FILL) && op.n != 0)
        op.kind = stalled == STALL_MEMOP_FILL ? RAM_OP_FILL : (op.src & 0x80000000) != 0 ? RAM_OP_STAGED : RAM_OP_ROM;
    return op;
}
uint4 ram_op_pack(ram_op op) { return uint4(op.kind, op.dst, op.src, op.n); }
ram_op ram_op_unpack(uint4 v) {
    ram_op op;
    op.kind = v.x; op.dst = v.y; op.src = v.z; op.n = v.w;
    return op;
}
// the RAM texels it writes
bool ram_op_texels(ram_op op, out uint t0, out uint t1) {
    t0 = op.dst >> 4;
    t1 = min((op.dst + op.n - 1) >> 4, RAM_TEXELS - 1);
    return op.kind != RAM_OP_NONE && t0 <= t1;
}
// A staged copy's chunk for this commit, texels c0 to c1 of RAM, and the copy left after it.
// A destination inside the source ahead of it is copied from its end, so that no chunk reads
// what an earlier one wrote.
bool ram_chunk(ram_op op, out uint c0, out uint c1, out ram_op rest) {
    rest = op;
    uint t0, t1;
    c0 = c1 = 0;
    if (!ram_op_texels(op, t0, t1)) { rest.n = 0; return false; }
    uint s = op.src & 0x7fffffff;
    if (t1 - t0 < STAGE_CHUNK) {
        c0 = t0;
        c1 = t1;
        rest.n = 0;
    } else if (s < op.dst && op.dst < s + op.n) {
        c0 = t1 - STAGE_CHUNK + 1;
        c1 = t1;
        rest.n = c0 * 16 - op.dst;
    } else {
        c0 = t0;
        c1 = t0 + STAGE_CHUNK - 1;
        uint used = (c1 + 1) * 16 - op.dst;
        rest.dst += used;
        rest.src += used;
        rest.n -= used;
    }
    return true;
}

// RAM texel t as `core` leaves it: its write cache's copy, or RAM as it was before the commit.
// A worker that stored nothing in its pass may have an older pass's cache in its state.
uint4 ram_seen_texel(uint core, uint t) {
    uint4 texel;
#if CORES > 1
    mc_select(core);
#endif
    uint bloom = STATE_TEX(uint2(41, 0)).r;
    if ((core == 0 || bloom != 0) && ((t << 4) & bloom) == (t << 4) && l1_state_find(t, texel)) return texel;
    return RAM_TEX(RAM_ADDR(t));
}
// A word of a fill's or copy's source, for the destination's byte `a`.
uint ram_op_word(uint core, ram_op op, uint a) {
    if (op.kind == RAM_OP_FILL) return op.src;
    uint s = op.src + (a - op.dst);
    if (op.kind == RAM_OP_STAGED) return idx_uint4(ram_seen_texel(core, (s & 0x7fffffff) >> 4), (s >> 2) & 3);
    return mem_get_cached_or_tex_from_state_cache_or_mtd(s);
}
// Destination texel t after the fill or copy: its words in the range from the source, the
// others as the core leaves them.
uint4 ram_op_texel(uint core, ram_op op, uint t) {
    uint4 res = ram_seen_texel(core, t);
    [loop]
    for (uint k = 0; k < 4; k++) {
        uint a = t * 16 + k * 4;
        if (a >= op.dst && a - op.dst < op.n) set_idx_uint4(res, ram_op_word(core, op, a), k);
    }
    return res;
}
bool ram_op_whole(ram_op op, uint t) {
    return t * 16 >= op.dst && t * 16 + 16 - op.dst <= op.n;
}

#ifdef GPU_DEVICE
// The picture a drawn list asked to be copied into RAM (GPU_COPY, docs/machine/gpu.md): its first
// word (a RAM word index), size and row length in words.
struct ram_pic {
    uint first, width, height, row;
};
bool ram_picture(out ram_pic p) {
    uint4 copy = RAM_TEX(RAM_ADDR(GPU_COPY));
    if (copy.r & SUBMIT_INTO) {
        p.first = (copy.g & 0x7fffffff) >> 2;
        p.width = copy.b & 0xffff;
        p.height = copy.b >> 16;
        p.row = max(copy.a, p.width);
    } else {
        uint4 disp = RAM_TEX(RAM_ADDR(GPU_DISPLAY));
        p.first = GPU_PIXELS * 4;
        p.width = p.row = disp.g;
        p.height = disp.b;
    }
    p.height = min(p.height, 2048u);
    return copy.r != 0 && p.width != 0 && p.height != 0;
}
// RAM texel t with the picture's words over `old`, and how many words of it the picture has.
uint4 ram_picture_texel(ram_pic p, uint t, uint4 old, out uint covered) {
    covered = 0;
    uint4 result = old;
    [unroll]
    for (uint k = 0; k < 4; k++) {
        uint w = t * 4 + k;
        if (w >= p.first) {
            uint q = w - p.first, y = q / p.row, x = q - y * p.row;
            if (x < p.width && y < p.height) {
                uint3 c = (uint3)(_GpuTarget.Load(int3(x, y, 0)).rgb * 255.0 + 0.5);
                result[k] = (c.r << 16) | (c.g << 8) | c.b;
                covered++;
            }
        }
    }
    return result;
}
// the texel of slot s: a picture row's first texel, then its last
uint ram_picture_slot_texel(ram_pic p, uint s) {
    uint w = p.first + (s >> 1) * p.row + ((s & 1) != 0 ? p.width - 1 : 0);
    return w >> 2;
}
#endif

uint ram_zap_reach() {
    return _ZapKind == 2 ? min(_ZapR, (uint)STAGE_ZAP_REACH) : 0;
}

// What a vertex of these passes carries: what the texels become, or what makes them.
#define RK_VALUE 0      // value
#define RK_SLOT 1       // the stage's texel (info.y, info.z)
#define RK_CONTROL 2    // gather: the control words
#define RK_PICSLOT 3    // gather: the picture's part texels
#define RK_ZAPSLOT 4    // gather: the zap's square
#define RK_LONE 5       // gather: core info.y's lone store
#define RK_EDGE 6       // gather: texel info.z of core info.y's fill or copy (value)
#define RK_CHUNK 7      // gather: the chunk of core info.y's copy (value), from texel info.z
#define RK_INIT 8       // RAM from the payload
#define RK_ZAP 9        // the zap's square, from (value.x, value.y) as ints
#define RK_MEMOP 10     // core info.y's fill or copy (value) from texel info.z to info.w
#define RK_CHUNKOUT 11  // the chunk, from texel info.y
#define RK_PICTURE 12   // the picture

struct ram_point {
    uint id : TEXCOORD0;
};
ram_point ram_point_vert(uint id : SV_VertexID) {
    ram_point o;
    o.id = id;
    return o;
}
struct ram_v2f {
    float4 vertex : SV_Position;
    nointerpolation uint4 value : TEXCOORD0;
    nointerpolation uint4 info : TEXCOORD1;
};

// A rectangle of a target `height` rows high (2048 wide), as two triangles.
void ram_quad(inout TriangleStream<ram_v2f> stream, uint x, uint y, uint w, uint h, uint height, uint4 value, uint4 info) {
    if (w == 0 || h == 0) return;
    for (uint corner = 0; corner < 4; corner++) {
        float2 p = float2(x + (corner & 1) * w, y + (corner >> 1) * h) / float2(2048.0, height);
        ram_v2f o;
        o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.5, 1.0);   // the target's row 0 at the top
        o.value = value;
        o.info = info;
        stream.Append(o);
    }
    stream.RestartStrip();
}
// `count` texels from the start of row `row`
void ram_span(inout TriangleStream<ram_v2f> stream, uint row, uint count, uint height, uint4 value, uint4 info) {
    ram_quad(stream, 0, row, 2048, count / 2048, height, value, info);
    ram_quad(stream, 0, row + count / 2048, count % 2048, 1, height, value, info);
}
// RAM texels t0 to t1, row by row
void ram_rows(inout TriangleStream<ram_v2f> stream, uint t0, uint t1, uint4 value, uint4 info) {
    if (t0 > t1) return;
    uint y0 = t0 / 2048, y1 = t1 / 2048, x0 = t0 % 2048, x1 = t1 % 2048;
    if (y0 == y1) {
        ram_quad(stream, x0, 64 + y0, x1 - x0 + 1, 1, STATE_TEX_HEIGHT, value, info);
    } else {
        ram_quad(stream, x0, 64 + y0, 2048 - x0, 1, STATE_TEX_HEIGHT, value, info);
        ram_quad(stream, 0, 64 + y0 + 1, 2048, y1 - y0 - 1, STATE_TEX_HEIGHT, value, info);
        ram_quad(stream, 0, 64 + y1, x1 + 1, 1, STATE_TEX_HEIGHT, value, info);
    }
}

// Whether `core` is one the geometry has.
bool ram_core_on(uint core) {
    if (core >= CORES) return false;
#if CORES > 1
    return core == 0 || (core <= mc_count && mc_bits_of(core) != 0);
#else
    return core == 0;
#endif
}
// Whether a core's write cache and lone store are of this pass: core 0's always are; a
// worker's when it stored anything (the word of its stored addresses or-ed together).
bool ram_core_stored(uint core) {
    return core == 0 || tick_state(core, 41).r != 0;
}

#ifdef COMMIT_GATHER
// The gather's points: point 0 the head (no copy), the control words, the picture's and the
// zap's texels; then one a core: its texels of row 8, and a staged copy's head and chunk. Of
// the cores with a copy pending the highest draws last, and its copy is the one made.
[maxvertexcount(24)]
void ram_gather_geom(point ram_point IN[1], inout TriangleStream<ram_v2f> stream) {
    RAM_GEOM_GUARD
    uint id = IN[0].id;
#if CORES > 1
    MC_GEO_READ
#endif
    if (id == 0) {
        ram_quad(stream, STAGE_HEAD_X, STAGE_SLOT_ROW, 1, 1, STAGE_ROWS, uint4(0xffffffff, 0, 0, 0), uint4(RK_VALUE, 0, 0, 0));
        ram_quad(stream, 0, STAGE_CONTROL_ROW, 2048, 1, STAGE_ROWS, 0, uint4(RK_CONTROL, 0, 0, 0));
#ifdef GPU_DEVICE
        ram_pic p;
        if (ram_picture(p)) ram_span(stream, STAGE_PICTURE_ROW, 2 * p.height, STAGE_ROWS, 0, uint4(RK_PICSLOT, 0, 0, 0));
#endif
        uint side = 2 * ram_zap_reach() + 1;
        if (_ZapKind != 0) ram_span(stream, STAGE_ZAP_ROW, side * side, STAGE_ROWS, 0, uint4(RK_ZAPSLOT, side, 0, 0));
        return;
    }
    uint core = id - 1;
    if (!ram_core_on(core)) return;
    if (ram_core_stored(core) && (tick_state(core, 8).r >> 4) < RAM_TEXELS)
        ram_quad(stream, 3 * core, STAGE_SLOT_ROW, 1, 1, STAGE_ROWS, 0, uint4(RK_LONE, core, 0, 0));
    ram_op op = ram_op_of(core);
    uint t0, t1;
    if ((op.kind == RAM_OP_FILL || op.kind == RAM_OP_ROM) && ram_op_texels(op, t0, t1)) {
        ram_quad(stream, 3 * core + 1, STAGE_SLOT_ROW, 1, 1, STAGE_ROWS, ram_op_pack(op), uint4(RK_EDGE, core, t0, 0));
        ram_quad(stream, 3 * core + 2, STAGE_SLOT_ROW, 1, 1, STAGE_ROWS, ram_op_pack(op), uint4(RK_EDGE, core, t1, 0));
    } else if (op.kind == RAM_OP_STAGED) {
        uint c0, c1;
        ram_op rest;
        if (ram_chunk(op, c0, c1, rest)) {
            ram_quad(stream, STAGE_HEAD_X, STAGE_SLOT_ROW, 1, 1, STAGE_ROWS, uint4(core, c0, c1, rest.n == 0 ? 1 : 0), uint4(RK_VALUE, 0, 0, 0));
            ram_span(stream, 0, c1 - c0 + 1, STAGE_ROWS, ram_op_pack(op), uint4(RK_CHUNK, core, c0, 0));
        }
    }
}

uint4 ram_gather_texel(ram_v2f i) {
    uint2 pos = (uint2)i.vertex.xy;
    s_dim = uint2(2048, STATE_TEX_HEIGHT);
    _Data_ROM.GetDimensions(m_dim.x, m_dim.y);
#if CORES > 1
    MC_GEO_READ
#endif
    uint kind = i.info.x, core = i.info.y;
    [branch]
    if (kind == RK_CONTROL) return _Ram[uint2(pos.x, CONTROL_ROW)];
#ifdef GPU_DEVICE
    [branch]
    if (kind == RK_PICSLOT) {
        ram_pic p;
        uint covered;
        ram_picture(p);
        uint t = ram_picture_slot_texel(p, pos.x + (pos.y - STAGE_PICTURE_ROW) * 2048);
        return t < RAM_TEXELS ? ram_picture_texel(p, t, RAM_TEX(RAM_ADDR(t)), covered) : (uint4)0;
    }
#endif
    [branch]
    if (kind == RK_ZAPSLOT) {
        uint s = pos.x + (pos.y - STAGE_ZAP_ROW) * 2048, side = i.info.y, reach = ram_zap_reach();
        int x = (int)_ZapX - (int)reach + (int)(s % side), row = (int)_ZapY - (int)reach + (int)(s / side);
        if (x < 0 || x >= 2048 || row < 0 || row >= (int)(STATE_TEX_HEIGHT - 64)) return (uint4)0;
        uint2 at = uint2(x, row + 64);
        return zap(at, _Ram[at]);
    }
    [branch]
    if (kind == RK_LONE) {
        uint4 lone = tick_state(core, 8);
        uint4 texel = ram_seen_texel(core, lone.r >> 4);
        set_idx_uint4(texel, lone.g, (lone.r >> 2) & 3);
        return texel;
    }
    [branch]
    if (kind == RK_EDGE) return ram_op_texel(core, ram_op_unpack(i.value), i.info.z);
    [branch]
    if (kind == RK_CHUNK) return ram_op_texel(core, ram_op_unpack(i.value), i.info.z + pos.x + pos.y * 2048);
    return i.value;
}
#endif

// The 64 state rows after the commit.
uint4 ram_init_raw(uint2 pos) {
    float3 raw[6];
    for (uint off = 0; off < 6; off++)
        raw[off] = _Data_RAM_RAW[pos + uint2((off * 2048u) % (2048u * 3), (off / 3) * STATE_TEX_HEIGHT)].rgb;
    return unpack_uint4(raw);
}
// A staged copy's place in what its core keeps: the stall stays until its last chunk is made,
// and its addresses and length move on by a chunk when one is.
uint4 ram_progress(uint core, uint x, uint4 result) {
    if (x != 28 && x != 37 && x != 38) return result;
    ram_op op = ram_op_of(core);
    if (op.kind != RAM_OP_STAGED) return result;
    uint c0, c1;
    ram_op rest;
    bool any = ram_chunk(op, c0, c1, rest);
    bool served = any && _Stage[uint2(STAGE_HEAD_X, STAGE_SLOT_ROW)].r == core;
    bool done = !any || (served && rest.n == 0);
    if (x == 28) {
        if (!done) result.r = STALL_MEMOP_COPY;
        return result;
    }
    if (!served || done) return result;
#ifdef CSR_FIELDS
    if (x == 37) result = uint4(result.r, result.g, rest.src, (result.a & 0x80000000) | rest.dst);
    if (x == 38) result.b = rest.n;
#else
    if (x == 38) result = uint4(rest.src, (result.g & 0x80000000) | rest.dst, rest.n, result.a);
#endif
    return result;
}
uint4 ram_state_texel(uint2 pos) {
    s_dim = uint2(2048, STATE_TEX_HEIGHT);
    _Data_ROM.GetDimensions(m_dim.x, m_dim.y);
    if (_Init && _InitRaw) return ram_init_raw(pos);
    uint4 own = _TickState[pos], result;
#ifdef LANE_FIELD
    // the lanes' field (experiments/rvc_lean/tick.hlsl): of a lane's texels its place and its count,
    // the last words of its buckets' tags; the buckets themselves emptied, their points made
    if (pos.y >= LANE_FIELD_ROW) return _Init || ((pos.x & 7) != 0 && (pos.x & 7) != 4) ? (uint4)0 : uint4(0, 0, 0, own.w);
#endif
#if CORES > 1
    MC_GEO_READ
    uint hb = 0;
    mc_select(0);
    if (pos.x >= MC_STRIP_X) {
        uint tile = ((pos.x - MC_STRIP_X) >> 3) + MC_STRIP_TILES * (pos.y >> 3);
        hb = mc_core_at(tile);
        if (hb == 0) return (uint4)0;
        uint w = ((tile - mc_tile0) << 6) + ((pos.y & 7) << 3) + (pos.x & 7);
        pos = uint2(w & 63, w >> 6);
    }
    pos = mc_texel_of(pos);
    // (of a worker's state the commit changes five texels: the rest is as it is)
    if (hb != 0 && (pos.y != 0 || (pos.x != 8 && pos.x != 28 && pos.x != 37 && pos.x != 38 && pos.x != 41))) return own;
    decode_for_commit();
    result = commit(pos, own);
    // A worker's one store that found its cache full is in its state until its next pass
    // rewrites it: gone now, or the commits of a worker asleep would store it again.
    if (hb != 0 && pos.x == 8 && pos.y == 0) result.r = 0xffffffff;
#else
    uint hb = 0;
    decode_for_commit();
    result = commit(pos, own);
#endif
    if (pos.y == 0) result = ram_progress(hb, pos.x, result);
    return result;
}

#ifdef COMMIT_RAM
uniform uint _CommitPoints;   // how many points the RAM commit draws: the last is the picture
#ifdef LANE_FIELD
uniform uint _FieldPoints;    // of them, before the picture's, the lanes' field's buckets (0: it was not drawn)
#endif
// The RAM commit's points: point 0 the zap's square (or, with _Init, all of RAM), then as many as
// the cores' places (core 0's 258: its buckets, its lone store, its fill or copy; a worker's
// 130), then the picture. Drawn in that order, which is which wins where two write a texel.
[maxvertexcount(20)]
void ram_commit_geom(point ram_point IN[1], inout TriangleStream<ram_v2f> stream) {
    RAM_GEOM_GUARD
    uint id = IN[0].id;
#if CORES > 1
    MC_GEO_READ
#endif
    if (id == 0) {
        if (_Init) {
            ram_quad(stream, 0, 64, 2048, STATE_TEX_HEIGHT - 64, STATE_TEX_HEIGHT, 0, uint4(RK_INIT, 0, 0, 0));
        } else if (_ZapKind != 0) {
            int reach = (int)ram_zap_reach(), x0 = (int)_ZapX - reach, y0 = (int)_ZapY - reach;
            int x1 = min(x0 + 2 * reach, 2047), y1 = min(y0 + 2 * reach, (int)(STATE_TEX_HEIGHT - 65));
            int xa = max(x0, 0), ya = max(y0, 0);
            if (xa <= x1 && ya <= y1)
                ram_quad(stream, xa, 64 + ya, x1 - xa + 1, y1 - ya + 1, STATE_TEX_HEIGHT, uint4(asuint(x0), asuint(y0), 0, 0), uint4(RK_ZAP, 2 * reach + 1, 0, 0));
        }
        return;
    }
    if (_Init) return;
#ifdef LANE_FIELD
    // the field's buckets, two a lane: the points before the picture's (as many as _FieldPoints)
    if (_FieldPoints != 0 && id + 1 < _CommitPoints && id + 1 + _FieldPoints >= _CommitPoints) {
        uint f = id + 1 + _FieldPoints - _CommitPoints, w = (f >> 1) * 8 + (f & 1) * 4;
        uint4 tags = _TickState[uint2(w & 2047, LANE_FIELD_ROW + (w >> 11))];
        for (uint n = 0; n < 3; n++) {
            uint tag = idx_uint4(tags, n), v = w + 1 + n;
            if (tag != 0 && tag - 1 < RAM_TEXELS) ram_rows(stream, tag - 1, tag - 1, _TickState[uint2(v & 2047, LANE_FIELD_ROW + (v >> 11))], uint4(RK_VALUE, 0, 0, 0));
        }
        return;
    }
#endif
#ifdef GPU_DEVICE
    if (id == _CommitPoints - 1) {
        ram_pic p;
        if (!ram_picture(p)) return;
        uint last = p.first + p.row * (p.height - 1) + p.width - 1;
        ram_rows(stream, p.first >> 2, min(last >> 2, RAM_TEXELS - 1), 0, uint4(RK_PICTURE, 0, 0, 0));
        // the copy is made: GPU_COPY cleared, one more counted in GPU_CLOCK
        uint4 clock = RAM_TEX(RAM_ADDR(GPU_CLOCK));
        ram_rows(stream, GPU_COPY, GPU_COPY, 0, uint4(RK_VALUE, 0, 0, 0));
        ram_rows(stream, GPU_CLOCK, GPU_CLOCK, uint4(clock.r, clock.g, clock.b + 1, clock.a), uint4(RK_VALUE, 0, 0, 0));
        return;
    }
#endif
    uint place = id - 1, core = 0, e = place, bits = L1_TABLE_BITS;
    if (place >= 258) {
        core = 1 + (place - 258) / 130;
        e = (place - 258) % 130;
    }
    if (!ram_core_on(core)) return;
#if CORES > 1
    bits = mc_bits_of(core);
#endif
    uint last = core == 0 ? 256 : 128;
    bool stored = ram_core_stored(core);
    if (e < last) {
        // a bucket of the write cache: an entry's tag is its texel's number and one
        if (!stored || e >= L1_BUCKETS_OF(bits)) return;
        uint at = L1_STATE_AT + e * L1_STRIDE;
        uint4 tags = tick_state(core, at);
        for (uint n = 0; n < 3; n++) {
            uint tag = idx_uint4(tags, n);
            if (tag != 0 && tag - 1 < RAM_TEXELS) ram_rows(stream, tag - 1, tag - 1, tick_state(core, at + 1 + n), uint4(RK_VALUE, 0, 0, 0));
        }
    } else if (e == last) {
        // the store a full cache had no room for, merged into its texel by the gather
        uint t = tick_state(core, 8).r >> 4;
        if (stored && t < RAM_TEXELS) ram_rows(stream, t, t, 0, uint4(RK_SLOT, 3 * core, STAGE_SLOT_ROW, 0));
    } else {
        ram_op op = ram_op_of(core);
        uint t0, t1;
        if ((op.kind == RAM_OP_FILL || op.kind == RAM_OP_ROM) && ram_op_texels(op, t0, t1)) {
            ram_rows(stream, t0, t1, ram_op_pack(op), uint4(RK_MEMOP, core, t0, t1));
        } else if (op.kind == RAM_OP_STAGED) {
            uint4 head = _Stage[uint2(STAGE_HEAD_X, STAGE_SLOT_ROW)];
            if (head.r == core) ram_rows(stream, head.g, head.b, 0, uint4(RK_CHUNKOUT, head.g, 0, 0));
        }
    }
}

uint4 ram_init_texel(uint2 pos) {
    if (_InitRaw) return ram_init_raw(pos);
    uint lin = RAM_LIN(pos.x, pos.y - 64);
    uint2 ram_dim;
    _Data_RAM_A.GetDimensions(ram_dim.x, ram_dim.y);
    uint2 pos2 = uint2(lin % 2048, lin / 2048);   // the payload image is row-major, rows bottom up
    pos2.y = ram_dim.y - pos2.y - 1;
    if (pos2.y >= ram_dim.y) return (uint4)0;
    return uint4(unpack_raw_float4(_Data_RAM_R[pos2]), unpack_raw_float4(_Data_RAM_G[pos2]),
                 unpack_raw_float4(_Data_RAM_B[pos2]), unpack_raw_float4(_Data_RAM_A[pos2]));
}

uint4 ram_commit_texel(ram_v2f i) {
    uint2 pos = (uint2)i.vertex.xy;
    s_dim = uint2(2048, STATE_TEX_HEIGHT);
    _Data_ROM.GetDimensions(m_dim.x, m_dim.y);
    uint kind = i.info.x, t = (pos.y - 64) * 2048 + pos.x;
    [branch]
    if (kind == RK_SLOT) return _Stage[i.info.yz];
    [branch]
    if (kind == RK_INIT) return ram_init_texel(pos);
    [branch]
    if (kind == RK_ZAP) {
        uint s = (uint)((int)pos.x - asint(i.value.x)) + (uint)((int)(pos.y - 64) - asint(i.value.y)) * i.info.y;
        return _Stage[uint2(s % 2048, STAGE_ZAP_ROW + s / 2048)];
    }
    [branch]
    if (kind == RK_CHUNKOUT) {
        uint s = t - i.info.y;
        return _Stage[uint2(s % 2048, s / 2048)];
    }
    [branch]
    if (kind == RK_MEMOP) {
        ram_op op = ram_op_unpack(i.value);
        uint core = i.info.y;
        if (!ram_op_whole(op, t)) return _Stage[uint2(3 * core + (t == i.info.z ? 1 : 2), STAGE_SLOT_ROW)];
        if (op.kind == RAM_OP_FILL) return uint4(op.src, op.src, op.src, op.src);
        uint s = op.src + (t * 16 - op.dst);
        return uint4(mem_get_cached_or_tex_from_state_cache_or_mtd(s), mem_get_cached_or_tex_from_state_cache_or_mtd(s + 4),
                     mem_get_cached_or_tex_from_state_cache_or_mtd(s + 8), mem_get_cached_or_tex_from_state_cache_or_mtd(s + 12));
    }
#ifdef GPU_DEVICE
    [branch]
    if (kind == RK_PICTURE) {
        // (only what a GPU program may own, 0x86000000 to 0x87e00000)
        if (t - 0x600000u >= 0x1e0000u) discard;
        ram_pic p;
        uint covered;
        ram_picture(p);
        uint4 whole = ram_picture_texel(p, t, (uint4)0, covered);
        if (covered == 0) discard;
        if (covered == 4) return whole;
        // a part texel: from the slot of the row of its first word that the picture has
        uint k = 0;
        while (k < 3 && (t * 4 + k < p.first || (t * 4 + k - p.first) % p.row >= p.width)) k++;
        uint y = (t * 4 + k - p.first) / p.row;
        uint s = 2 * y + ((p.first + y * p.row) >> 2 == t ? 0 : 1);
        return _Stage[uint2(s % 2048, STAGE_PICTURE_ROW + s / 2048)];
    }
#endif
    return i.value;
}
#endif

#endif
