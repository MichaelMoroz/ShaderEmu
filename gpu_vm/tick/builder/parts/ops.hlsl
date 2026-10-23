// ---- The commit: a point a write cache entry, drawn into its texel of RAM in the draw of its word's component
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _cpad; };
uint4 state_any(uint t, uint c) {
    int3 at = sat(t >> 3, c);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    return (t & 4) ? ((t & 2) ? v3 : v2) : ((t & 2) ? v1 : v0);
}
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint tg = pick(state_any(T_WT + (j >> 2), c), j & 3), tex = tg & 0x0fffffffu;
    float2 at = (float2(tex % RAM_W, tex / RAM_W) + 0.5) / float2(RAM_W, _RamRows);
    c2f o;
    o.vertex = tg != NONE && ((tg >> (28 + _Comp)) & 1) != 0 ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = pick(state_any(T_WC + j, c), _Comp);
    return o;
}
uint4 commit_frag(c2f i) : SV_Target { return i.value; }

// ---- The operations, in rounds of three draws: stage (each piece's texels as they will be), write (into RAM), progress
// (_OpState: the queue's generation, operations through, pieces made of the next, refused). A piece is up to 4 KB within
// one 4 KB block of its destination; a round makes up to OP_SLOTS of them a core, in the queue's order, each operation
// whole before the next starts, and the next only if it is independent of those before it in the round.
bool op_ok(opd o) {   // the reference's checks (gvm__range)
    uint de = o.dst + o.dn - 1, se = o.src + o.sn - 1;
    bool dok = o.dn == 0 || (de >= o.dst && o.dst >= GVM_DEV && de < _RamEnd);
    bool sok = o.sn == 0 || (se >= o.src && (se < _RamEnd || (o.src >= GVM_ROM && se < GVM_ROM_END)));
    return (o.kind == GVM_OP_COPY || o.kind == GVM_OP_FILL || o.kind == GVM_OP_BLIT) && dok && sok;
}
uint op_per_row(opd o) { return o.kind == GVM_OP_BLIT ? ((o.row + 4095) >> 12) + 1 : 0; }
uint op_segs(opd o) {
    return o.kind == GVM_OP_BLIT ? (o.dn != 0 ? o.rows * op_per_row(o) : 0) : o.n == 0 ? 0 : ((o.dst + o.n - 1) >> 12) - (o.dst >> 12) + 1;
}
bool overlap(uint a, uint an, uint b, uint bn) { return an != 0 && bn != 0 && a < b + bn && b < a + an; }
bool op_seq(opd o) {   // a blit whose rows share texels or read what it writes: a piece a round, in order
    return o.kind == GVM_OP_BLIT && o.dn != 0 && ((o.rows > 1 && o.ds < o.row + 15) || overlap(o.dst, o.dn, o.src, o.sn));
}
bool conflict(opd o, opd e) {   // o reads what e writes, or o and e write one texel
    uint ot = o.dst >> 4, et = e.dst >> 4;
    return overlap(o.src, o.sn, e.dst, e.dn) || overlap(ot, o.dn != 0 ? ((o.dst + o.dn - 1) >> 4) - ot + 1 : 0, et, e.dn != 0 ? ((e.dst + e.dn - 1) >> 4) - et + 1 : 0);
}
void op_piece(opd o, uint g, out uint d0, out uint d1, out uint s0) {   // piece g: bytes [d0, d1) from s0
    bool blit = o.kind == GVM_OP_BLIT, back = o.kind == GVM_OP_COPY && o.dst > o.src && o.dst - o.src < o.n;
    uint P = blit ? op_per_row(o) : op_segs(o), rw = blit ? g / P : 0, j = blit ? g % P : back ? P - 1 - g : g;
    uint dr = o.dst + rw * o.ds, sr = o.src + rw * o.ss, L = blit ? o.row : o.n, A = dr & ~4095u;
    d0 = max(dr, A + j * 4096); d1 = max(d0, min(dr + L, A + (j + 1) * 4096)); s0 = sr + (d0 - dr);
}
opd op_at(uint c, uint k) { return op_of(state_any(T_Q + 2 * k, c), state_any(T_Q + 2 * k + 1, c)); }
struct plan_t { uint d, p, rf, k, g; bool hit; };
plan_t plan(uint c, uint want) {   // core c's round: slot want's operation and piece, and the progress after the round
    uint4 qs = state_any(22, c), os = _OpState.Load(int3(c, 0, 0));
    bool fresh = os.x != qs.z;
    plan_t P;
    P.d = fresh ? 0 : os.y; P.p = fresh ? qs.y : os.z; P.rf = fresh ? 0 : os.w; P.k = 0; P.g = 0; P.hit = false;
    uint used = 0, first = P.d;
    bool stop = c >= _NCores;
    [loop] for (uint k = P.d; k < min(qs.x, QMAX) && !stop; k++) {
        opd o = op_at(c, k);
        bool ok = op_ok(o), indep = true;
        [loop] for (uint i = first; i < min(k, QMAX) && ok; i++) { opd e = op_at(c, i); indep = indep && !(op_ok(e) && conflict(o, e)); }
        [branch] if (!ok) { P.rf++; P.d = k + 1; P.p = 0; }
        else [branch] if (!indep) stop = true;
        else {
            uint S = op_segs(o), rem = S - P.p, take = min(op_seq(o) ? min(rem, 1u) : rem, OP_SLOTS - used);
            [flatten] if (want - used < take) { P.hit = true; P.k = k; P.g = P.p + want - used; }
            used += take; P.p += take;
            stop = P.p < S || used == OP_SLOTS;
            [flatten] if (P.p >= S) { P.d = k + 1; P.p = 0; }
        }
    }
    return P;
}
struct o2f { float4 vertex : SV_POSITION; nointerpolation uint4 j0 : J0; nointerpolation uint4 j1 : J1; };
float4 corner(uint vid, float x0, float x1, float y0, float y1, float w, float h, bool on) {   // a quad, six vertices
    uint v = vid % 6;
    float x = (v == 1 || v == 3 || v == 4) ? x1 : x0, y = (v == 2 || v == 4 || v == 5) ? y1 : y0;
    return on ? float4(x / w * 2 - 1, 1 - y / h * 2, 0, 1) : float4(-2, -2, 0, 1);
}
void slot_piece(uint inst, out opd o, out uint d0, out uint d1, out uint s0, out bool on) {
    plan_t P = plan(inst / OP_SLOTS, inst % OP_SLOTS);
    o = op_at(inst / OP_SLOTS, P.k);
    op_piece(o, P.g, d0, d1, s0);
    on = P.hit && d1 > d0;
}
o2f op_stage_vert(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    opd o; uint d0, d1, s0; bool on;
    slot_piece(inst, o, d0, d1, s0, on);
    uint cnt = ((d1 - 1) >> 4) - (d0 >> 4) + 1, sh = (o.dst & 3) * 8, dl = d0 - s0;
    o2f r;
    r.vertex = corner(vid, 0, (float)cnt, (float)inst, (float)(inst + 1), (float)OP_SPAN, (float)(_NCores * OP_SLOTS), on);
    r.j0 = uint4(d0, d1, s0, o.kind);
    r.j1 = uint4(sh != 0 ? (o.src << sh) | (o.src >> (32 - sh)) : o.src, o.kind == GVM_OP_BLIT && dl != 0 && dl < d1 - d0 ? dl : 0, 0, 0);
    return r;
}
uint src_byte(uint a) { return (pick_ad(data_texel(a), a) >> ((a & 3) * 8)) & 0xffu; }
uint4 op_stage_frag(o2f i) : SV_Target {
    uint d0 = i.j0.x, d1 = i.j0.y, s0 = i.j0.z, t = (d0 >> 4) + (uint)i.vertex.x, k;
    uint lo = max(d0, t * 16) - t * 16, hi = min(d1, t * 16 + 16) - t * 16, sa = s0 + (t * 16 - d0);
    uint4 old = ram(t), nv = i.j1.x;   // a fill: its word turned to the destination's bytes
    [branch] if (i.j0.w != GVM_OP_FILL) {
        [branch] if (i.j1.y != 0) {   // a blit's row that reads what it writes: a byte at a time, as made in order
            [loop] for (k = 0; k < 16; k++) {
                uint b = t * 16 + k - d0, v = src_byte(s0 + b % i.j1.y);
                nv = put(nv, k >> 2, (pick(nv, k >> 2) & ~(0xffu << (8 * (k & 3)))) | (v << (8 * (k & 3))));
            }
        } else {   // two source texels, their bytes moved into place
            uint4 a = data_texel(sa), b = data_texel(sa + 16);
            uint w[8] = { a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w };
            uint o = sa & 15, sh = (o & 3) * 8;
            [unroll] for (k = 0; k < 4; k++) {
                uint x = w[(o >> 2) + k], y = w[(o >> 2) + k + 1];
                nv = put(nv, k, sh != 0 ? (x >> sh) | (y << (32 - sh)) : x);
            }
        }
    }
    [unroll] for (k = 0; k < 4; k++) {   // the bytes [lo, hi) of the texel are the piece's
        uint l = clamp((int)lo - 4 * (int)k, 0, 4), h = clamp((int)hi - 4 * (int)k, 0, 4);
        uint m = (h >= 4 ? 0xffffffffu : (1u << (8 * h)) - 1) & ~(l >= 4 ? 0xffffffffu : (1u << (8 * l)) - 1);
        old = put(old, k, pick(old, k) ^ ((pick(old, k) ^ pick(nv, k)) & m));
    }
    return old;
}
o2f op_write_vert(uint vid : SV_VertexID, uint inst : SV_InstanceID) {   // two quads: the piece's texels on two rows
    opd o; uint d0, d1, s0; bool on;
    slot_piece(inst, o, d0, d1, s0, on);
    uint t0 = d0 >> 4, t1 = (d1 - 1) >> 4, x0 = t0 % RAM_W, y0 = t0 / RAM_W, xe = x0 + t1 - t0 + 1;
    bool second = vid >= 6;
    o2f r;
    r.vertex = corner(vid, second ? 0 : (float)x0, (float)(second ? xe - RAM_W : min(xe, RAM_W)), (float)(y0 + (second ? 1 : 0)),
                      (float)(y0 + (second ? 2 : 1)), (float)RAM_W, (float)_RamRows, on && (!second || xe > RAM_W));
    r.j0 = uint4(t0, t1, inst, 0);
    r.j1 = 0;
    return r;
}
uint4 op_write_frag(o2f i) : SV_Target {
    uint2 px = uint2(i.vertex.xy);
    uint t = px.y * RAM_W + px.x;
    if (t < i.j0.x || t > i.j0.y) discard;
    return _Stage.Load(int3(t - i.j0.x, i.j0.z, 0));
}
struct p2f { float4 vertex : SV_POSITION; nointerpolation uint4 v : V; };
p2f op_prog_vert(uint c : SV_VertexID) {
    plan_t P = plan(c, NONE);
    p2f o;
    o.vertex = float4((float)(2 * c + 1) / (float)_NCores - 1, 0, 0, 1);   // (fxc2 took (float)c's bits for c in the loads)
    o.v = uint4(state_any(22, c).z, P.d, P.p, P.rf);
    return o;
}
uint4 op_prog_frag(p2f i) : SV_Target { return i.v; }
