#ifndef GPU_DRAW_H
#define GPU_DRAW_H
// The GPU's drawing (docs/machine/gpu.md): the VRAM, a command's vertices, the draw IDs, the front end, the uploads, the
// copies, a fragment, and the holodeck's view of a frame. Included by the GPU's drawing passes only; src/gpu.h has the
// device's words and its control.
#include "gpu.h"

uint vram_pad(uint slot) { return 1u << VRAM_LOD(slot); }
// The block CMD_UPLOAD covers for a w x h texture (programs/common/gpu.h has the same): from 2^L before its texel (0, 0),
// rounded up to whole 2^L.
uint2 vram_block(uint2 size, uint slot) {
    uint pad = vram_pad(slot), mask = (1u << VRAM_LOD(slot)) - 1;
    return (size + 2 * pad + mask) & ~mask;
}

Texture2D<float4> _GpuVram;           // the VRAM, with its mip levels (VRAM_SIDE square)
SamplerState sampler_linear_clamp, sampler_point_clamp;   // (their names are their kinds, in Unity and the harness)
// Guest numbers are 16.16 fixed point.
float4 from_fixed(uint4 t) {
    return float4(asint(t)) / 65536.0;
}
// A colour word is 0xTTRRGGBB: T is transparency, so that the usual 0x00RRGGBB is opaque.
// A texel of numbers as a draw has them: floats as they are, or 16.16 fixed point.
float4 gpu_number(bool floats, uint4 t) {
    return floats ? asfloat(t) : from_fixed(t);
}

float4 colour_of(uint v) {
    return float4((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 255 - (v >> 24)) / 255.0;
}

// A compact vertex's texture coordinates and colour, from its fourth word and the command's word 11.
void gpu_compact(uint how, uint fourth, uint word, out float2 uv, out float4 colour) {
    if (how & VERTEX_TAGGED) {
        uv = float2(fourth & 0xfff, (fourth >> 12) & 0xfff) / 1024.0;
        if (how & VERTEX_TABLE) word = ram_word(word + 4 * (fourth >> 24));
    } else {
        uv = float2(asint(fourth << 16) >> 16, asint(fourth) >> 16) / 1024.0;
    }
    colour = colour_of(word);
}

// Vertex k of a compact draw: its position, texture coordinates and colour.
void gpu_compact_vertex(uint base, uint4 head, uint4 how, uint4 more, uint k, out float4 pos, out float2 uv, out float4 colour) {
    uint4 t;
    if (how.r & VERTEX_PACKED) {
        // (both buffers may be a file's bytes where it lies in the ROM)
        uint p = texture_word(head.g + 4 * k);
        t = uint4(p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff, (texture_word(ram(base + 3).r + 4 * k) & 0xffffff) | (p & 0xff000000));
        pos = float4(float3(t.xyz), 1.0);
    } else {
        t = ram(texel_of(head.g) + k);
        pos = float4(gpu_number((how.r & VERTEX_FLOAT) != 0, t).xyz, 1.0);
    }
    gpu_compact(how.r, t.a, more.a, uv, colour);
    [branch]
    if (how.r & VERTEX_ENV) {
        // RB_CalcEnvironmentTexCoords (Raven's): the way to the eye made one long by Q_rsqrt (its first step, as
        // VectorNormalizeFast), d its dot with the normal, u and v the normal's x and y times d less half the way's;
        // the operations the CPU's, in its order (precise), and in 1,024ths as a compact vertex has them
        uint4 w = ram(base + 3);
        float3 n = asfloat(ram(texel_of(w.r) + k)).xyz;
        precise float3 viewer = asfloat(w.gba) - pos.xyz;
        precise float l0 = viewer.x * viewer.x + viewer.y * viewer.y;
        precise float l = l0 + viewer.z * viewer.z;
        precise float half_l = l * 0.5f;
        float y = asfloat(0x5f3759dfu - (asuint(l) >> 1));
        precise float a0 = half_l * y;
        precise float a1 = a0 * y;
        precise float a2 = 1.5f - a1;
        precise float il = y * a2;
        precise float3 v = viewer * il;
        precise float d0 = n.x * v.x + n.y * v.y;
        precise float d = d0 + n.z * v.z;
        precise float2 st = float2(n.x * d, n.y * d) - 0.5f * v.xy;
        uv = floor(st * 1024.0) / 1024.0;
    }
    if (how.r & VERTEX_FACING) {
        // A vertex's tag is which of 256 ways it faces (a texel each of floats, in the model's
        // own space); the light is three texels: from where, its colour, and the colour of
        // the light from everywhere. The colour is that last and, by how it faces the light, the first.
        float3 facing = asfloat(ram(texel_of(more.a) + (t.a >> 24))).xyz;
        uint light = texel_of(ram(base + 3).g);
        colour = float4(saturate(asfloat(ram(light + 2)).rgb + max(dot(facing, asfloat(ram(light)).xyz), 0.0) * asfloat(ram(light + 1)).rgb), 1.0);
    }
}

// Vertex k of a draw of a renderer's arrays (VERTEX_ARRAYS; docs/machine/gpu.md, "A renderer's arrays"), as Jedi
// Outcast's shim made a compact vertex of them (linux/jk2/gl_jk2.c, fast_triangles), so that a picture is that draw's to
// the pixel: word 1 the indices, two of 16 bits a word, from the arrays' first vertex; word 12 the positions, a texel of
// floats each (x, y, z); word 13 the texture coordinates, two floats each; word 14 the colours, the GL's four bytes each;
// word 15 the flags in its low byte and the coordinates' least, two integers of 12 bits from bit 8 (subtracted, as the
// shim made a compact vertex's coordinates from them, then in 1,024ths: 16 bits by an int, or with ARRAYS_SMALL 12 bits by
// an unsigned, as a tagged vertex's); word 11 the colour of every vertex with ARRAYS_ONE, else each its own, made as the
// shim's word of it (0xTTRRGGBB, T 255 less alpha; alpha 255 with ARRAYS_OPAQUE)
#define ARRAYS_FAR 1      // z the far plane's, -0.9999: 2D before the frame's first 3D
#define ARRAYS_ONE 2      // every vertex word 11's colour
#define ARRAYS_SMALL 4    // the coordinates a tagged vertex's (12 bits, unsigned)
#define ARRAYS_OPAQUE 8   // the alpha 255 (GL_ONE, GL_ONE)
#define ARRAYS_WIDE 16    // the indices a word each (the renderer's own, copied as they are)
void gpu_arrays_vertex(uint4 head, uint4 more, uint4 w, uint k, out float4 pos, out float2 uv, out float4 colour) {
    uint index;
    [branch]
    if ((w.a & ARRAYS_WIDE) != 0) {
        index = ram_word(head.g + 4 * k);
    } else {
        uint pair = ram_word(head.g + 4 * (k >> 1));
        index = (k & 1) != 0 ? pair >> 16 : pair & 0xffff;
    }
    float4 p = asfloat(ram(texel_of(w.r) + index));
    pos = float4(p.x, p.y, (w.a & ARRAYS_FAR) != 0 ? -0.9999f : p.z, 1.0);
    float2 t = asfloat(uint2(ram_word(w.g + 8 * index), ram_word(w.g + 8 * index + 4)));
    float2 low = float2((float)((int)(w.a << 12) >> 20), (float)((int)w.a >> 20));
    precise float2 d = t - low;
    precise float2 s = d * 1024.0;
    uint2 q = (w.a & ARRAYS_SMALL) != 0 ? uint2(s) & 0xfff : uint2(int2(s)) & 0xffff;
    uv = float2(q) / 1024.0;
    uint word = more.a;
    if ((w.a & ARRAYS_ONE) == 0) {
        uint c = ram_word(w.b + 4 * index);
        word = (255 - (c >> 24)) << 24 | (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff);
        if ((w.a & ARRAYS_OPAQUE) != 0) word &= 0x00ffffff;
    }
    colour = colour_of(word);
}

// Vertex k of a skinned draw (VERTEX_BONES; docs/machine/gpu.md, "A skinned model"), as Jedi Outcast's Ghoul2 makes
// its vertices (rd-vanilla's RB_SurfaceGhoul and RB_CalcDiffuseColor): word 1 is the triangles' indices, a word each
// (the drawn vertex k is the model's vertex of index word k); word 12 its vertices of 32 bytes, on 4 bytes: the normal
// and x, then y, z, the bone references (5 bits each, the count less one in bits 30-31, the weights' top two bits in
// pairs from bit 12) and the weights' low bytes; word 13 the bones, three texels of a row of four floats each, by the
// vertex's references; word 14 the texture coordinates, two floats a vertex; word 15, when not 0, three texels of light
// (from where, its colour and the light from everywhere, 0 to 255; each texel's fourth a channel's multiplier): the
// colour is then the light by the normal (turned by the first bone), and word 11's alpha; else word 11. The weights are
// tenths of 1,023, the last one 1 less the others. The operations are RB_SurfaceGhoul's, in its order and not fused
// (precise): the positions are the bits the CPU's skinning makes, so a picture is the CPU's drawing's to the pixel.
float gpu_bone_row(float4 r, float4 at) {
    precise float d = r.x * at.x + r.y * at.y;
    precise float e = d + r.z * at.z;
    precise float f = e + r.w;
    return f;
}
float gpu_bone_turn(float4 r, float3 n) {   // (a normal: DotProduct's order, no translation)
    precise float d = r.x * n.x + r.y * n.y;
    precise float e = d + r.z * n.z;
    return e;
}
float3 gpu_bone(uint bone, float4 at) {
    return float3(gpu_bone_row(asfloat(ram(bone)), at), gpu_bone_row(asfloat(ram(bone + 1)), at), gpu_bone_row(asfloat(ram(bone + 2)), at));
}
float gpu_bone_weight(uint refs, uint low, uint j) {
    precise float w = (float)(((low >> (8 * j)) & 0xff) | ((refs >> (12 + 2 * j)) & 0x300)) * (1.0f / 1023.0f);
    return w;
}
void gpu_bones_vertex(uint4 head, uint4 more, uint4 words, uint k, out float4 pos, out float2 uv, out float4 colour) {
    uint index = ram_word(head.g + 4 * k);
    // (the vertices on 4 bytes: a model's as its file lies; 32 bytes a vertex, so the draw's every vertex is as far
    // into its texel, and the branches below are the draw's)
    uint v = texel_of(words.r) + 2 * index, shift = (words.r >> 2) & 3;
    uint4 t0 = ram(v), t1 = ram(v + 1), b;
    float4 a;
    [branch]
    if (shift == 0) {
        a = asfloat(t0), b = t1;
    } else {
        uint4 t2 = ram(v + 2);
        if (shift == 1) a = asfloat(uint4(t0.yzw, t1.x)), b = uint4(t1.yzw, t2.x);
        else if (shift == 2) a = asfloat(uint4(t0.zw, t1.xy)), b = uint4(t1.zw, t2.xy);
        else a = asfloat(uint4(t0.w, t1.xyz)), b = uint4(t1.w, t2.xyz);
    }
    float4 at = float4(a.w, asfloat(b.x), asfloat(b.y), 1.0);
    uint refs = b.z, low = b.w, count = (refs >> 30) + 1, bones = texel_of(words.g);
    uint first = bones + 3 * (refs & 31);
    precise float3 x = gpu_bone(first, at);
    [branch]
    if (count == 2) {
        // w (t1 - t2) + t2, RB_SurfaceGhoul's two weights
        precise float w = gpu_bone_weight(refs, low, 0);
        precise float3 second = gpu_bone(bones + 3 * ((refs >> 5) & 31), at);
        precise float3 apart = x - second;
        precise float3 by = w * apart;
        x = by + second;
    } else if (count > 2) {
        precise float total = gpu_bone_weight(refs, low, 0);
        x = total * x;
        [loop]
        for (uint j = 1; j + 1 < count; j++) {
            precise float w = gpu_bone_weight(refs, low, j);
            total = total + w;
            precise float3 by = w * gpu_bone(bones + 3 * ((refs >> (5 * j)) & 31), at);
            x = x + by;
        }
        precise float last = 1.0f - total;
        precise float3 by = last * gpu_bone(bones + 3 * ((refs >> (5 * (count - 1))) & 31), at);
        x = x + by;
    }
    pos = float4(x, 1.0);
    // (in 1,024ths, as a compact vertex has them: the picture is a compact draw's of the CPU's skinning)
    uv = floor(asfloat(uint2(ram_word(words.b + 8 * index), ram_word(words.b + 8 * index + 4))) * 1024.0) / 1024.0;
    colour = colour_of(more.a);
    [branch]
    if (words.a) {
        // RB_CalcDiffuseColor (and RB_CalcDiffuseEntityColor, by the multipliers): the normal by the first bone's rows
        float4 l0 = asfloat(ram(texel_of(words.a))), l1 = asfloat(ram(texel_of(words.a) + 1)), l2 = asfloat(ram(texel_of(words.a) + 2));
        precise float3 n = float3(gpu_bone_turn(asfloat(ram(first)), a.xyz), gpu_bone_turn(asfloat(ram(first + 1)), a.xyz),
                                  gpu_bone_turn(asfloat(ram(first + 2)), a.xyz));
        precise float in0 = n.x * l0.x + n.y * l0.y;
        precise float incoming = in0 + n.z * l0.z;
        precise float3 toward = incoming * l1.rgb;
        precise float3 j = incoming > 0.0 ? l2.rgb + toward : l2.rgb;
        precise float3 by = min(j, 255.0) * float3(l0.w, l1.w, l2.w);
        colour.rgb = floor(by) / 255.0;
    }
}

// How far corner 1 (up) or 2 (right) of a point's triangle is from the point, which is `depth`
// in front of the eye. `how` is the command's words 12-14: size, growth with depth, from what depth.
float2 gpu_corner(float4 how, uint corner, float depth) {
    float s = how.x * (depth < how.z ? 1.0 : 1.0 + how.y * depth);
    return corner == 1 ? float2(0.0, s) : corner == 2 ? float2(s, 0.0) : float2(0.0, 0.0);
}

struct gpu_varyings {
    float4 position : SV_Position;
    float4 colour : COLOR0;
    float2 uv : TEXCOORD0;
    nointerpolation uint4 texture_info : TEXCOORD1;   // mode and flags, address, width, height
    nointerpolation uint key : TEXCOORD2;
    float2 light_uv : TEXCOORD3;                      // FRAGMENT_LIT: where in the light map
    nointerpolation uint3 light_info : TEXCOORD4;     // and its address, width, height
    // The picture's right and bottom edges, where the target is larger than the picture: CMD_CLEAR's triangle, twice
    // the picture each way, is cut there (docs/machine/gpu.md, "The picture's rectangle"). Only it: a cut triangle's
    // new corners move the interpolation inside it, and a 3D triangle so cut changed 113 of Half-Life's pixels.
    float2 edge : SV_ClipDistance0;
};

// Pixels (x right, y down, depth 0..1) to the corner of the render target the display shows.
float4 place_pixels(float2 p, float depth) {
    return float4(p.x * 2.0 / GPU_TARGET_W - 1.0, 1.0 - p.y * 2.0 / GPU_TARGET_H, depth, 1.0);
}

// The draw IDs (docs/machine/gpu.md, "Draw IDs"): a texture of GPU_DRAW_IDS_W x GPU_DRAW_IDS_H texels, one a mesh vertex, drawn
// once a frame a list is drawn (GPUDrawIds), before the upload pass: the texel of the command that claims the vertex and
// the passes it is drawn in (bit p pass p, GPU_DRAW_ID_UPLOAD the upload pass). Every later pass reads its vertex's texel in
// place of the search for its command, which 13 passes made 13 times for every vertex, drawn or not.
#define GPU_DRAW_IDS_W 512u
#define GPU_DRAW_IDS_H 384u   // (65,536 triangles' vertices)
#define GPU_DRAW_ID_UPLOAD 0x10000u   // the upload pass's (CMD_UPLOAD), pass 16, before every other
#define GPU_DRAW_ID_COPY 0x20000u     // the copy pass's (CMD_COPY), pass 17, after every other
Texture2D<int2> _GpuDrawIds;   // (signed, as Unity's RGInt is: the bits are the same)
uint2 gpu_draw_id(uint id) { return asuint(_GpuDrawIds.Load(int3(id % GPU_DRAW_IDS_W, id / GPU_DRAW_IDS_W, 0))); }

// The command whose vertex range holds mesh vertex `id`, whatever its pass: its first texel and
// first word (op, a, b, first slot), and the vertices it claims.
// The vertices the command at `base` claims.
uint gpu_slots(uint base, uint4 head) {
    uint op = head.r;
    return op == CMD_CLEAR ? 3 : op == CMD_RECT ? 6 : op == CMD_DRAW ? head.b : op == CMD_TEXT ? 6 * ram(base + 1).b : op == CMD_SPRITES ? 6 * head.b
         : op == CMD_UPLOAD || op == CMD_COPY ? 6 : 0;
}
bool gpu_owner(uint id, out uint base, out uint4 head, out uint slots) {
    base = 0;
    head = 0;
    slots = 0;
    uint4 ctrl = ram(GPU_CTRL);
    uint list = texel_of(ctrl.g), count = min(ctrl.b, GPU_MAX_COMMANDS);
    if ((ctrl.r & SUBMIT_DRAW) == 0 || count == 0) return false;
    // Commands claim rising vertex ranges, so the owner is the last one starting at or before
    // this vertex: a binary search, twelve steps for the largest list.
    uint lo = 0, hi = count;
    for (uint step = 0; step < 12; step++) {
        uint mid = (lo + hi) >> 1;
        if (hi - lo > 1) {
            if (ram(list + 4 * mid).a <= id) lo = mid;
            else hi = mid;
        }
    }
    base = list + 4 * lo;
    head = ram(base);
    slots = gpu_slots(base, head);
    return id >= head.a && id < head.a + slots;
}

// The mesh vertices the submitted list claims, from 0: its last command's end (commands claim rising ranges, the uploads
// last); 0 while none is submitted. The draw-ID pass and the front end draw those rows alone (gpu_rows_vertex).
uint gpu_mesh_used() {
    uint4 ctrl = ram(GPU_CTRL);
    uint count = min(ctrl.b, GPU_MAX_COMMANDS);
    if ((ctrl.r & SUBMIT_DRAW) == 0 || count == 0) return 0;
    uint base = texel_of(ctrl.g) + 4 * (count - 1);
    uint4 head = ram(base);
    return min(head.a + gpu_slots(base, head), GPU_DRAW_IDS_W * GPU_DRAW_IDS_H);
}
// A pass over the first `rows` of a target `height` rows tall (vertices 0 to 5), and vertices 6 to 11 over the first eight
// texels of its row `height - 1` (the front end's frame row; for the draw IDs, drawn with 6, none).
float4 gpu_rows_vertex(uint id, uint rows, uint height) {
    uint k = id % 6, corner = k < 3 ? k : (k == 3 ? 2 : (k == 4 ? 1 : 3));
    float2 p = id < 6 ? float2((corner & 1) ? GPU_DRAW_IDS_W : 0, (corner & 2) ? rows : 0)
                      : float2((corner & 1) ? 8 : 0, (corner & 2) ? height : height - 1);
    return float4(p.x * 2.0 / GPU_DRAW_IDS_W - 1.0, 1.0 - p.y * 2.0 / height, 0.5, 1.0);
}
uint gpu_used_rows() { return (gpu_mesh_used() + GPU_DRAW_IDS_W - 1) / GPU_DRAW_IDS_W; }

// The draw-ID pass's texel for mesh vertex `id`: its command's texel and the passes it is drawn in (`drawn` the passes the
// host draws this frame, _GpuPasses). A list waiting for a
// pass or for its levels (SUBMIT_WAITS) has only its uploads drawn. A sprite's is the passes of its kind: a shade in
// pass 2, the rest in the command's others (gpu_vertex_sized collapses it elsewhere).
uint2 gpu_draw_ids_fragment(uint id, uint drawn) {
    uint base, slots;
    uint4 head;
    if (!gpu_owner(id, base, head, slots)) return 0;
    uint op = head.r, passes;
    if (op == CMD_UPLOAD) return uint2(base, GPU_DRAW_ID_UPLOAD);
    if (SUBMIT_WAITS(ram(GPU_CTRL).r, drawn)) return 0;
    if (op == CMD_COPY) return uint2(base, GPU_DRAW_ID_COPY);   // (with its list: not while that waits)
    if (op == CMD_SPRITES) {
        uint kind = (ram(texel_of(head.g) + (id - head.a) / 6).a >> 22) & 15;
        passes = ram(base + 1).r & (SPRITE_SHADES(kind) ? 4u : ~4u);
    } else {
        passes = 1u << (op == CMD_RECT || op == CMD_TEXT ? FRAGMENT_PASS(ram(base + 2).r) : op == CMD_DRAW ? FRAGMENT_PASS(ram(base + 1).g) : 0);
    }
    return uint2(base, passes & 0xffff);
}

// (text, FRAGMENT_MSDF) how much steeper than a box filter's the coverage rises across an edge:
// fitted to the hinted bitmaps at 11 to 17 pixels (docs/system/fonts.md, "Distance fields")
#define MSDF_SHARP 1.2

// Mesh vertex `id`, which the command at `base` draw IDs, placed and coloured for a picture of `size` pixels; and for the
// holodeck (docs/world/holodeck.md) `eye`, its place in its program's camera (x right, y up, the view along -z) and in w
// its kind (VOLUME_WORLD a draw in perspective with a modelview of its own, VOLUME_FLAT one over the picture, 0 none),
// and `proj` the projection's near and far planes and its x and y scales.
#define VOLUME_WORLD 1.0
#define VOLUME_FLAT 2.0
gpu_varyings gpu_vertex_sized(uint id, uint base, uint4 head, float2 size, out float4 eye, out float4 proj) {
    eye = 0, proj = 0;
    gpu_varyings o = (gpu_varyings)0;
    o.position = float4(2, 2, 2, 1);
    o.edge = 1.0;
    {
        uint op = head.r;
        uint k = id - head.a;
        [branch]
        if (op == CMD_CLEAR) {
            // one triangle over the whole picture, at the far plane
            o.position = place_pixels(float2(k == 1 ? 2.0 : 0.0, k == 2 ? 2.0 : 0.0) * size, 1.0);
            o.colour = colour_of(head.g);
            // cut at x = size.x and y = size.y: its colour and depth are the same at every corner, so no pixel inside changes
            o.edge = float2(1.0 - (k == 1 ? 2.0 : 0.0), 1.0 - (k == 2 ? 2.0 : 0.0));
        } else if (op == CMD_RECT) {
            // head.g colour; then x0, y0, x1, y1; mode, texture address, width, height;
            // u0, v0, u1, v1 (16.16). head.b is the key
            uint4 box = ram(base + 1), tex = ram(base + 2);
            float4 uv = from_fixed(ram(base + 3));
            uint corner = k < 3 ? k : (k == 3 ? 2 : (k == 4 ? 1 : 3));   // 0 1 2, 2 1 3
            bool right = (corner & 1) != 0, low = (corner & 2) != 0;
            o.position = place_pixels(float2(asint(right ? box.b : box.r), asint(low ? box.a : box.g)), 0.0);
            eye.w = VOLUME_FLAT;
            o.colour = colour_of(head.g);
            o.uv = float2(right ? uv.z : uv.x, low ? uv.w : uv.y);
            o.texture_info = tex;
            o.key = head.b;
        } else if (op == CMD_UPLOAD) {
            // head.g where the texture is (RAM or the ROM), head.b its kind (FRAGMENT_TEXTURE, _INDEXED or _RGB24, and
            // FRAGMENT_KEYED); then width, height, the slot, the key; the palette's address (0: the display's) and the
            // row's length in pixels (0: the width). A quad over the slot's block in the VRAM, drawn in the upload pass.
            uint4 size = ram(base + 1), more = ram(base + 2);
            uint slot = size.b, pad = vram_pad(slot);
            uint2 block = vram_block(size.rg, slot);
            uint corner = k < 3 ? k : (k == 3 ? 2 : (k == 4 ? 1 : 3));
            float2 p = float2(VRAM_X(slot) - pad + ((corner & 1) ? block.x : 0), VRAM_Y(slot) - pad + ((corner & 2) ? block.y : 0));
            o.position = float4(p.x * 2.0 / VRAM_SIDE - 1.0, 1.0 - p.y * 2.0 / VRAM_SIDE, 0.0, 1.0);
            o.texture_info = uint4(head.b, head.g, size.r, size.g);
            o.key = size.a;
            o.light_info = uint3(slot, more.g != 0 ? more.g : size.r, more.r);
        } else if (op == CMD_COPY) {
            // head.g the picture's rectangle's corner (x | y << 16, pixels from its top left), head.b its size (w | h << 16);
            // then the slot, where in it (x | y << 16), the flags (COPY_TURNED). A quad over that place in the VRAM, drawn in
            // the copy pass, which takes each texel from the target.
            uint4 to = ram(base + 1);
            uint w = head.b & 0xffff, h = head.b >> 16, slot = to.r;
            uint corner = k < 3 ? k : (k == 3 ? 2 : (k == 4 ? 1 : 3));
            float2 p = float2(VRAM_X(slot) + (to.g & 0xffff) + ((corner & 1) ? w : 0), VRAM_Y(slot) + (to.g >> 16) + ((corner & 2) ? h : 0));
            o.position = float4(p.x * 2.0 / VRAM_SIDE - 1.0, 1.0 - p.y * 2.0 / VRAM_SIDE, 0.0, 1.0);
            o.texture_info = uint4(to.b, head.g, w, h);
            o.light_info = uint3(slot, to.g, 0);
        } else if (op == CMD_SPRITES) {
            // head.g the records, head.b how many; then the passes (a bit each), the first record's place in the
            // order, the palette (0: the picture is of indices), then words 7 and 8 the two remaps green counts (SPRITE_WORDS). A record is a
            // texel: its bytes' address (or a colour), a remap table (or what is added to green), x | y << 11 |
            // stride << 22, w | h << 11 | kind << 22 | (stride >> 10) << 26. A later record lies nearer.
            uint4 cw = ram(base + 1);
            uint part = k % 6, corner = part < 3 ? part : (part == 3 ? 2 : (part == 4 ? 1 : 3));
            uint4 rec = ram(texel_of(head.g) + k / 6);
            uint kind = (rec.a >> 22) & 15;
            bool right = (corner & 1) != 0, low = (corner & 2) != 0;
            uint x = rec.b & 0x7ff, y = (rec.b >> 11) & 0x7ff, w = rec.a & 0x7ff, h = (rec.a >> 11) & 0x7ff;
            o.position = place_pixels(float2(x + (right ? w : 0), y + (low ? h : 0)), 1.0 - (float)(cw.g + k / 6 + 1) / 4194304.0);
            uint slots = ram(base + 3).a;   // (the records' slots, a word each, gpu_list_uploads)
            o.texture_info = uint4(FRAGMENT_SPRITE | kind << 24, slots != 0 && !(kind == SPRITE_FILL || kind == SPRITE_CHECKER) ? ram_word(slots + 4 * (k / 6)) : rec.r,
                                   (rec.b >> 22) | (rec.a >> 26) << 10, rec.g);
            o.key = x | y << 16;
            o.light_info = uint3(cw.b, cw.a, ram(base + 2).r);
            // (a fill's or a checker's colour: its index, or with a palette the palette's colour of it)
            uint index = rec.r & 0xff;
            o.colour = cw.b != 0 ? float4(colour_of(ram_word(cw.b + 4 * index) & 0xffffff).rgb, 1.0) : float4((float)index / 255.0, 0.0, 0.0, 1.0);
        } else if (op == CMD_TEXT) {
            // head.g colour, head.b the font's table; then x, y, count, the string; then the
            // fragment word. A character is a word: its glyph, and in the high half where it
            // starts along the line. The table has a texel a glyph: bitmap, width, height, and
            // where its corner is from the character's (a signed byte each way). The table and
            // the bitmaps may be a file's where it lies in the ROM (a font: docs/system/fonts.md).
            // With TEXT_MORE: a scale (16.16), a slant (256ths of a pixel a row, and from bit 16
            // the row that stays), and what the table's bitmap addresses are counted from.
            // FRAGMENT_MSDF (docs/system/fonts.md): the table is into an atlas, its quad in 64ths of a
            // pixel, and word 12 the distance range in pixels (16.16) before the scale; key is
            // that range on the screen, light_uv how far in the quad a screen pixel is.
            uint4 at = ram(base + 1), how = ram(base + 2);
            bool more = (how.r & TEXT_MORE) != 0, msdf = (how.r & 0xff) == FRAGMENT_MSDF;
            uint part = k % 6, corner = part < 3 ? part : (part == 3 ? 2 : (part == 4 ? 1 : 3));
            uint letter = ram_word(at.a + 4 * (k / 6));
            uint4 glyph = texture_texel(head.b + 16 * (letter & 0xffff));
            bool right = (corner & 1) != 0, low = (corner & 2) != 0;
            float2 in_glyph = float2((asint(glyph.a << 24) >> 24) + (right ? (int)glyph.g : 0),
                                     (asint(glyph.a << 16) >> 24) + (low ? (int)glyph.b : 0));
            if (msdf) in_glyph = float2((asint(glyph.a << 16) >> 16) + (right ? (int)(glyph.b & 0xffff) : 0),
                                        (asint(glyph.a) >> 16) + (low ? (int)(glyph.b >> 16) : 0)) / 64.0;
            float scale = more && how.g != 0 ? (float)how.g / 65536.0 : 1.0;
            if (more) {
                in_glyph.x += ((float)(how.b >> 16) - in_glyph.y) * (float)(how.b & 0xffff) / 256.0;
                in_glyph *= scale;
            }
            o.position = place_pixels(float2(asint(at.r) + (int)(letter >> 16), asint(at.g)) + in_glyph, 0.0);
            eye.w = VOLUME_FLAT;
            o.colour = colour_of(head.g);
            o.uv = float2(right ? 1.0 : 0.0, low ? 1.0 : 0.0);
            // (the glyph's slot: word 15 a word a character, gpu_list_uploads)
            uint slots = ram(base + 3).a;
            o.texture_info = uint4((msdf ? FRAGMENT_MSDF : (how.r & 0xff) == FRAGMENT_COVER ? FRAGMENT_COVER : FRAGMENT_MASK) | (how.r & 0x70000),
                                   slots != 0 ? ram_word(slots + 4 * (k / 6)) : glyph.r + (more ? how.a : 0),
                                   msdf ? glyph.g & 0xffff : glyph.g, msdf ? glyph.g >> 16 : glyph.b);
            if (msdf) {
                o.key = asuint(max((float)ram(base + 3).r / 65536.0 * scale, 1.0) * MSDF_SHARP);
                o.light_uv = 64.0 / (scale * max(float2(glyph.b & 0xffff, glyph.b >> 16), 1.0));
            }
        } else {
            // head.g vertex buffer, head.b count; then vertex mode, fragment mode, uniforms,
            // texture address; width, height, key
            uint4 how = ram(base + 1), more = ram(base + 2);
            uint u = texel_of(how.b);
            bool fl = (how.r & VERTEX_FLOAT) != 0;
            if (how.r & VERTEX_QUADS) {
                // triangles 0 1 2 and 0 2 3 of each quad's corners
                uint part = k % 6;
                k = (k / 6) * 4 + (part < 3 ? part : part == 3 ? 0 : part - 2);
            }
            float4 pos, normal = 0;
            uint corner = 0;
            if (how.r & VERTEX_POINTS) { corner = k % 3; k = k / 3; }
            [branch]
            if (how.r & VERTEX_BONES) {
                gpu_bones_vertex(head, more, ram(base + 3), k, pos, o.uv, o.colour);
            } else if (how.r & VERTEX_ARRAYS) {
                gpu_arrays_vertex(head, more, ram(base + 3), k, pos, o.uv, o.colour);
            } else if (how.r & VERTEX_COMPACT) {
                gpu_compact_vertex(base, head, how, more, k, pos, o.uv, o.colour);
            } else {
                uint v = texel_of(head.g) + 4 * k;
                pos = gpu_number(fl, ram(v));
                normal = gpu_number(fl, ram(v + 1));
                o.uv = gpu_number(fl, ram(v + 2)).xy;
                o.colour = gpu_number(fl, ram(v + 3));
            }
            o.texture_info = uint4(how.g, how.a, more.r, more.g);
            o.key = more.b;
            if (how.r & VERTEX_MOVED) {
                // a texture that slides, turns or grows: u' and v' are each a row's dot with (u, v, 1)
                uint rows = texel_of(ram(base + 3).a);
                float3 at = float3(o.uv, 1.0);
                o.uv = float2(dot(gpu_number(fl, ram(rows)).xyz, at), dot(gpu_number(fl, ram(rows + 1)).xyz, at));
            }
            if ((how.g & FRAGMENT_LIT) && (how.r & VERTEX_COMPACT)) {
                // words 12-14: a word a vertex of where in the light map it is (16 bits each way
                // of the whole map), the map's address, its width | height << 16
                uint4 lit = ram(base + 3);
                uint at = ram_word(lit.r + 4 * k);
                o.light_uv = float2(at & 0xffff, at >> 16) / 65535.0;
                o.light_info = uint3(lit.g, lit.b & 0xffff, lit.b >> 16);
            }
            if (how.g & FRAGMENT_LAID) {
                // words 12-15: the texture coordinates of the picture's corner, and how far
                // they go in 1,024 pixels. The colour carries the second pair to the fragments.
                float4 lay = gpu_number(fl, ram(base + 3));
                o.uv = lay.xy;
                o.colour = float4(lay.zw, 0.0, 1.0);
            }
            [branch]
            uint vertex_mode = how.r & 0xff;
            if (vertex_mode == VERTEX_SCREEN) {
                o.position = place_pixels(pos.xy, pos.z);
                eye.w = VOLUME_FLAT;
            } else {
                // with VERTEX_MODELVIEW the GPU does the matrix product the program would have done
                if (how.r & VERTEX_MODELVIEW)
                    pos = float4(dot(gpu_number(fl, ram(u + 4)), pos), dot(gpu_number(fl, ram(u + 5)), pos),
                                 dot(gpu_number(fl, ram(u + 6)), pos), pos.w);
                if (how.r & VERTEX_POINTS) pos.xy += gpu_corner(gpu_number(fl, ram(base + 3)), corner, -pos.z);
                float4 r0 = gpu_number(fl, ram(u)), r1 = gpu_number(fl, ram(u + 1)), r2 = gpu_number(fl, ram(u + 2)), r3 = gpu_number(fl, ram(u + 3));
                float4 clip = float4(dot(r0, pos), dot(r1, pos), dot(r2, pos), dot(r3, pos));
                // (a projection whose w does not come from z draws flat on the picture, a status bar; OpenGL's third row
                // is 0, 0, -(f+n)/(f-n), -2fn/(f-n))
                bool world = (how.r & VERTEX_MODELVIEW) && ram(u + 3).b != 0;
                eye = float4(pos.xyz, world ? VOLUME_WORLD : VOLUME_FLAT);
                if (world) proj = float4(r2.w / (r2.z - 1.0), r2.w / (r2.z + 1.0), r0.x, r1.y);
                // The picture is the top-left size.x by size.y pixels of the target; clip z
                // arrives as -w..w (the OpenGL convention) and leaves as 0..w.
                float2 part = size / float2(GPU_TARGET_W, GPU_TARGET_H);
                o.position = float4((clip.x + clip.w) * part.x - clip.w, clip.w - (clip.w - clip.y) * part.y,
                                    (clip.z + clip.w) * 0.5, clip.w);
                if (vertex_mode == VERTEX_LIT) {
                    float3 n = float3(dot(gpu_number(fl, ram(u + 4)).xyz, normal.xyz), dot(gpu_number(fl, ram(u + 5)).xyz, normal.xyz),
                                      dot(gpu_number(fl, ram(u + 6)).xyz, normal.xyz));
                    float facing = max(dot(n, gpu_number(fl, ram(u + 7)).xyz), 0.0);
                    o.colour = float4(facing * gpu_number(fl, ram(u + 8)).rgb + gpu_number(fl, ram(u + 9)).rgb, 1.0);
                }
            }
        }
    }
    return o;
}

// The submitted list's picture, in pixels at the target's top left: the display's, or the
// rectangle it is drawn into.
float2 gpu_picture_size() {
    uint4 ctrl = ram(GPU_CTRL);
    uint4 disp = ram(GPU_DISPLAY);
    float2 size = float2(disp.g, disp.b);
    if (ctrl.r & (SUBMIT_INTO | SUBMIT_SIZED)) {
        uint4 into = ram(GPU_INTO);
        size = float2(into.g, into.b);
    }
    return size;
}


// ---- the vertex front end (docs/machine/gpu.md, "The vertex front end") ----
// Every vertex a command claims is made once a frame, by the GPUVertices pass, from the list in RAM, into six textures of
// GPU_DRAW_IDS_W x GPU_DRAW_IDS_H, a texel each (floats: the position, the colour, the two pairs of coordinates, the edge;
// words: the texture's four, the key and the light map's three). The passes after it take a vertex from there by its draw ID
// and read no RAM for it: a draw pass's vertex stage reads only the VRAM's textures.
uint2 gpu_made_at(uint id) { return uint2(id % GPU_DRAW_IDS_W, id / GPU_DRAW_IDS_W); }

Texture2D<float4> _GpuMadePos, _GpuMadeColour, _GpuMadeUv, _GpuMadeEdge, _GpuMadeEye, _GpuMadeProj;
Texture2D<int4> _GpuMadeTex, _GpuMadeMore;
// The list's picture as the front end found it (its frame's texel 3, gpu_frame_texel): what a pass reads in place of RAM.
float2 gpu_made_picture() { return max(_GpuMadeEye.Load(int3(3, GPU_DRAW_IDS_H, 0)).yz, 1.0); }
// ...and the vertices the list submitted this frame claims (texel 4, 0 with none: nothing is drawn again), and the last
// list's (texel 5, the holodeck's): the draw IDs and the front end's texels past them are an earlier list's.
uint gpu_made_used() { return (uint)_GpuMadeEye.Load(int3(4, GPU_DRAW_IDS_H, 0)).x; }
uint gpu_made_last() { return (uint)_GpuMadeEye.Load(int3(5, GPU_DRAW_IDS_H, 0)).x; }
gpu_varyings gpu_vertex_made(uint id) {
    gpu_varyings o;
    int3 at = int3(gpu_made_at(id), 0);
    float4 uv = _GpuMadeUv.Load(at);
    uint4 more = asuint(_GpuMadeMore.Load(at));
    o.position = _GpuMadePos.Load(at);
    o.colour = _GpuMadeColour.Load(at);
    o.uv = uv.xy;
    o.light_uv = uv.zw;
    o.texture_info = asuint(_GpuMadeTex.Load(at));
    o.key = more.r;
    o.light_info = more.gba;
    o.edge = _GpuMadeEdge.Load(at).xy;
    return o;
}
// Whether pass `p` (0 to 13, or the uploads' 16, or the copies' 17) draws mesh vertices id to id + span - 1, all of
// one command's: the draw IDs alone.
bool gpu_drawn(uint id, uint span, uint p) {
    uint2 c = gpu_draw_id(id);
    return id + span <= gpu_made_used() && ((c.g >> p) & 1) != 0 && (span < 2 || gpu_draw_id(id + span - 1).r == c.r);
}
struct gpu_made {
    float4 pos : SV_Target0;
    float4 colour : SV_Target1;
    float4 uv : SV_Target2;
    float4 edge : SV_Target3;
    int4 tex : SV_Target4;
    int4 more : SV_Target5;
    float4 eye : SV_Target6;    // (the holodeck's: gpu_vertex_sized's `eye` and `proj`)
    float4 proj : SV_Target7;
};
// The list's frame for the holodeck, the front end's row GPU_DRAW_IDS_H of its targets (`eye`): texels 0 to 2 the rows of
// the turn of the list's first draw in perspective with a modelview (for a program that draws its world before what moves
// in it, the view alone), w 1 if there is one; texel 3 which of the program's axes is up (1 x, 2 y, 3 z; 0 not said: the
// volume words' size word, docs/world/volume.md), the picture's size, and the list's address; texels 4 and 5 the vertices
// it claims (in a frame with no list submitted texel 4 is 0 and the rest stay the last list's).
#define VOLUME_LIST DEV_T(0x100030u)   // the volume words: the last whole frame's list address, commands, frames, size
float4 gpu_frame_texel(uint k) {
    uint4 ctrl = ram(GPU_CTRL);
    if (k == 3) return float4(ram(VOLUME_LIST).a >> 28, gpu_picture_size(), asfloat(ctrl.g));
    if (k == 4 || k == 5) return float4(gpu_mesh_used(), 0, 0, 0);
    uint list = texel_of(ctrl.g), count = min(ctrl.b, 24u);
    [loop]
    for (uint i = 0; i < count; i++) {
        uint at = list + 4 * i;
        uint4 how = ram(at + 1);
        if (ram(at).r != CMD_DRAW || (how.r & VERTEX_MODELVIEW) == 0 || (how.r & 0xff) == VERTEX_SCREEN) continue;
        uint u = texel_of(how.b);
        if (ram(u + 3).b == 0) continue;
        return float4(gpu_number((how.r & VERTEX_FLOAT) != 0, ram(u + 4 + k)).xyz, 1.0);
    }
    return float4(k == 0, k == 1, k == 2, 0.0);
}

// Mesh vertex `id` as the draw passes made it, for all of them (a sprite's shade or not is in its draw ID's passes); on
// the row past the mesh's, the frame's texels (gpu_frame_texel).
gpu_made gpu_front_fragment(uint id) {
    gpu_made m;
    m.pos = float4(2, 2, 2, 1);
    m.colour = 0, m.uv = 0, m.edge = 0, m.tex = 0, m.more = 0, m.eye = 0, m.proj = 0;
    [branch]
    if (id >= GPU_DRAW_IDS_W * GPU_DRAW_IDS_H) {
        // (with no list submitted the last one's stay, for the holodeck; texel 4, what the passes draw, says none)
        uint k = id - GPU_DRAW_IDS_W * GPU_DRAW_IDS_H;
        if (k != 4 && gpu_mesh_used() == 0) discard;
        if (k < 6) m.eye = gpu_frame_texel(k);
        return m;
    }
    uint2 c = gpu_draw_id(id);
    [branch]
    if (c.g != 0) {
        uint4 head = ram(c.r);
        gpu_varyings o = gpu_vertex_sized(id, c.r, head, gpu_picture_size(), m.eye, m.proj);
        m.pos = o.position;
        m.colour = o.colour;
        m.uv = float4(o.uv, o.light_uv);
        m.edge = float4(o.edge, 0, 0);
        m.tex = asint(o.texture_info);
        m.more = asint(uint4(o.key, o.light_info));
    }
    return m;
}

// The mesh's vertex `id` as the front end made it, if the pass being drawn draws it (gpu_drawn); vertices no command
// claims, and everything while no list is submitted, collapse to nothing.
gpu_varyings gpu_vertex(uint id, uint p) {
    gpu_varyings o = (gpu_varyings)0;
    o.position = float4(2, 2, 2, 1);   // outside the clip volume
    [branch]
    if (gpu_drawn(id, 1, p)) o = gpu_vertex_made(id);
    return o;
}

#ifdef GPU_AMP
// The device's mesh drawn by a blit (src/amp.h, docs/machine/gpu.md "Blits in place of cameras"): a call draws
// GPU_AMP_EACH of its triangles in their order, each whole if a command claims all three of its vertices (as
// Unity's mesh of a point a triangle did). 13 triangles are 39 vertices of gpu_varyings' 22 values, under the
// 1,024 a geometry shader may write, and 5,184 calls are 67,392 triangles. The includer defines _GpuPass (a
// number or a uniform) and GPU_AMP_Z(o) if the target's depth runs the other way (Unity's reversed Z).
#include "amp.h"
#define GPU_TRIANGLES 65536u
#ifndef GPU_AMP_Z
#define GPU_AMP_Z(o)
#endif
#define GPU_AMP_EACH 13
[maxvertexcount(39)]   // 3 * GPU_AMP_EACH (vkd3d takes a number there)
[instance(AMP_INST)]
void gpu_amp_geom(point amp_d I[1], inout TriangleStream<gpu_varyings> stream, uint inst : SV_GSInstanceID) {
    uint first = amp_call(I[0], inst) * GPU_AMP_EACH;
    for (uint t = 0; t < GPU_AMP_EACH; t++) {
        if (first + t >= GPU_TRIANGLES) break;
        uint v = (first + t) * 3;
        if (gpu_drawn(v, 3, _GpuPass)) {
            for (uint k = 0; k < 3; k++) {
                gpu_varyings o = gpu_vertex_made(v + k);
                GPU_AMP_Z(o);
                stream.Append(o);
            }
            stream.RestartStrip();
        }
    }
}
#endif

// Texel n of a texture of three bytes a pixel as a colour word: it may start at any byte, and
// three bytes may run into the next word. In RAM or the ROM.
uint gpu_rgb24(uint address, uint n) {
    uint first = address + 3 * n, at = first & ~3u, shift = 8 * (first & 3);
    uint texel = texture_word(at) >> shift;
    if (shift > 8) texel |= texture_word(at + 4) << (32 - shift);
    return (texel & 0xff) << 16 | (texel & 0xff00) | ((texel >> 16) & 0xff);
}

// The upload pass (CMD_UPLOAD): a texel of the VRAM in the texture's block, the texel of the texture it repeats (or, clamped,
// the edge's nearest), as a colour; with VRAM_PREMULTIPLIED times its alpha, and the key's 0.
// A byte of RAM.
uint gpu_byte(uint address) {
    return (ram_word(address & ~3u) >> (8 * (address & 3))) & 0xff;
}

// A sprite record's pixel (CMD_SPRITES), as its upload makes it (gpu_list_uploads: key the record, light_info.g its
// command, light_info.b the palette): an index in red, or with a palette its colour; alpha 0 a hole. A shade's pixel is
// only there or not (the draw adds the record's amount to green).
float4 gpu_sprite_upload(gpu_varyings i, uint u, uint v) {
    uint4 rec = ram(texel_of(i.key)), cw = ram(texel_of(i.light_info.g) + 1);
    uint kind = (rec.a >> 22) & 15, stride = (rec.b >> 22) | (rec.a >> 26) << 10, at = i.texture_info.g, index;
    [branch]
    if (kind == SPRITE_WORDS) {
        uint word = ram_word(at + v * stride + 4 * u), later = (word >> 8) & 0xff;
        index = (word >> 16) & 0xff;
        // green: the first table (word 7) made `later & 15` times, then the second (word 8) `later >> 4` times
        uint second = ram(texel_of(i.light_info.g) + 2).r;
        [loop]
        for (uint n = 0; n < (later & 15); n++) index = gpu_byte(cw.a + index);
        [loop]
        for (uint m = 0; m < (later >> 4); m++) index = gpu_byte(second + index);
    } else {
        uint s = gpu_byte(at + v * stride + u);
        if (s == 0 && kind != SPRITE_RAW) return 0.0;
        if (kind == SPRITE_SHADE) return 1.0;
        index = kind == SPRITE_REMAP ? gpu_byte(rec.g + s) : kind == SPRITE_BLACK ? 0 : s;
        if (index == 0 && kind == SPRITE_REMAP) return 0.0;
    }
    if (i.light_info.b != 0) return float4(colour_of(ram_word(i.light_info.b + 4 * index) & 0xffffff).rgb, 1.0);
    return float4((float)index / 255.0, 0.0, 0.0, 1.0);
}

float4 gpu_upload_fragment(gpu_varyings i) {
    uint slot = i.light_info.r, row = i.light_info.g, width = i.texture_info.b, height = i.texture_info.a;
    uint mode = i.texture_info.r & 0xff;
    int2 at = int2(i.position.xy) - int2(VRAM_X(slot), VRAM_Y(slot));
    uint x, y;
    if (slot & VRAM_CLAMPED) {
        x = (uint)clamp(at.x, 0, (int)width - 1);
        y = (uint)clamp(at.y, 0, (int)height - 1);
    } else {
        // (the padding is under the texture's size or a few of its repeats: whole repeats added first)
        x = (uint)(at.x + 64 * (int)width) % width;
        y = (uint)(at.y + 64 * (int)height) % height;
    }
    uint n = y * row + x, texel;
    [branch]
    if (mode == FRAGMENT_SPRITE) return gpu_sprite_upload(i, x, y);
    [branch]
    if (mode == FRAGMENT_MASK) {
        // a bit a pixel, rows of whole bytes: white, its alpha the bit (a draw of the slot in FRAGMENT_MASK tests it)
        n = y * ((width + 7) >> 3) + (x >> 3);
        texel = (texture_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
        return ((texel << (x & 7)) & 0x80) ? 1.0 : float4(1, 1, 1, 0);
    } else if (mode == FRAGMENT_COVER) {
        // four bits of coverage a pixel, the left in the high half: white, its alpha the coverage
        n = y * ((width + 1) >> 1) + (x >> 1);
        texel = (texture_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
        return float4(1, 1, 1, (float)((x & 1) ? texel & 15 : texel >> 4) / 15.0);
    } else if (mode == FRAGMENT_TILES) {
        // a layer of tiles (as gpu_fragment reads one): the pixel's cell, its tile's byte, the bank's colour; the key 0
        uint4 layer = ram(texel_of(i.texture_info.g));
        uint tile_w = max(layer.a & 0xff, 1), tile_h = max((layer.a >> 8) & 0xff, 1);
        uint cell_at = layer.r + 2 * ((y / tile_h) * ((layer.a >> 16) & 0xfff) + x / tile_w);
        uint cell = (ram_word(cell_at & ~3u) >> (8 * (cell_at & 3))) & 0xffff;
        uint px = x % tile_w, py = y % tile_h;
        if (cell & 0x400) px = tile_w - 1 - px;
        if (cell & 0x800) py = tile_h - 1 - py;
        uint tile = cell & 0x3ff, piece = layer.a >> 28, tiles = layer.g;
        if (piece != 0) {
            tiles = ram_word(layer.g + 4 * (tile >> piece));
            tile &= (1u << piece) - 1;
        }
        uint tat = tiles + (tile * tile_h + py) * tile_w + px;
        texel = (ram_word(tat & ~3u) >> (8 * (tat & 3))) & 0xff;
        if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) return 0.0;
        texel = ram_word(layer.b + 4 * (texel + 16 * (cell >> 12)));
    } else if (mode == FRAGMENT_YUV) {
        // a film's frame: luma, and the chroma of its 2 x 2 (BT.601, 16 to 235)
        uint half_w = width >> 1, luma_at = i.texture_info.g + y * width + x;
        uint blue_at = i.texture_info.g + width * height + (y >> 1) * half_w + (x >> 1), red_at = blue_at + half_w * (height >> 1);
        float luma = 1.164 * ((float)((texture_word(luma_at & ~3u) >> (8 * (luma_at & 3))) & 0xff) - 16.0);
        float blue = (float)((texture_word(blue_at & ~3u) >> (8 * (blue_at & 3))) & 0xff) - 128.0;
        float red = (float)((texture_word(red_at & ~3u) >> (8 * (red_at & 3))) & 0xff) - 128.0;
        return saturate(float4((luma + 1.596 * red) / 255.0, (luma - 0.392 * blue - 0.813 * red) / 255.0, (luma + 2.017 * blue) / 255.0, 1.0));
    } else if (mode == FRAGMENT_INDEXED) {
        texel = (texture_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
        if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) return 0.0;
        texel = ram_word((i.light_info.b != 0 ? i.light_info.b : DEV_AT + 0x1000400u) + 4 * texel);
    } else {
        // (a glyph's three distance fields are three bytes a pixel as RGB24's: the median does not mind their order)
        texel = mode == FRAGMENT_RGB24 || mode == FRAGMENT_MSDF ? gpu_rgb24(i.texture_info.g, n) : texture_word(i.texture_info.g + 4 * n);
        if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) return 0.0;
    }
    float4 c = colour_of(texel);
    if (slot & VRAM_PREMULTIPLIED) c.rgb *= c.a;
    return c;
}

// The copy pass (CMD_COPY): a texel of the slot's rectangle, the picture's texel it stands for (the rows turned with COPY_TURNED).
Texture2D<float4> _GpuTarget;   // the GPU's colour target, where the list was drawn
float4 gpu_copy_fragment(gpu_varyings i) {
    uint slot = i.light_info.r, at = i.light_info.g, from = i.texture_info.g, h = i.texture_info.a;
    int2 local = int2(i.position.xy) - int2(VRAM_X(slot) + (at & 0xffff), VRAM_Y(slot) + (at >> 16));
    int row = (i.texture_info.r & COPY_TURNED) ? (int)h - 1 - local.y : local.y;
    return _GpuTarget.Load(int3((int)(from & 0xffff) + local.x, (int)(from >> 16) + row, 0));
}

// The light a lit draw's fragment is times: its light map's slot, sampled by the card,
// weighed, the map's edge going on beyond it.
float3 gpu_light(gpu_varyings i) {
    uint width = i.light_info.g, height = i.light_info.b;
    if (width == 0 || height == 0) return 1.0;
    // (clamped, at level 0: the map's edge texels a half texel in)
    float2 p = clamp(saturate(i.light_uv) * float2(width, height), 0.5, float2(width, height) - 0.5);
    return _GpuVram.SampleLevel(sampler_linear_clamp, (float2(VRAM_X(i.light_info.r), VRAM_Y(i.light_info.r)) + p) / VRAM_SIDE, 0).rgb;
}

// A pixel of a sprite record (CMD_SPRITES), from its slot (the upload made it: an index in red, or with a palette its
// colour; alpha 0 a hole). A fill's and a checker's colour is the vertex's (the front end looked it up); a shade adds to
// green, which a later SPRITE_WORDS upload takes as the remaps made since the index was drawn.
float4 gpu_sprite_fragment(gpu_varyings i) {
    uint kind = (i.texture_info.r >> 24) & 15;
    uint2 p = (uint2)i.position.xy;
    [branch]
    if (kind == SPRITE_FILL_SHADE) return float4(0.0, (float)i.texture_info.a / 255.0, 0.0, 1.0);
    [branch]
    if (kind == SPRITE_FILL || kind == SPRITE_CHECKER) {
        if (kind == SPRITE_CHECKER && ((p.x + p.y) & 1) != ((i.texture_info.g >> 8) & 1)) discard;
        return i.colour;
    }
    uint slot = i.texture_info.g;
    if (!VRAM_IS(slot)) return float4(1.0, 0.0, 1.0, 1.0);   // (a record not uploaded)
    float4 t = _GpuVram.Load(int3(VRAM_X(slot) + p.x - (i.key & 0xffff), VRAM_Y(slot) + p.y - (i.key >> 16), 0));
    if (t.a == 0.0) discard;
    if (kind == SPRITE_SHADE) return float4(0.0, (float)i.texture_info.a / 255.0, 0.0, 1.0);
    return t;
}

// A fragment of a draw: its colour, times a light map's, times a texture's; every texture a slot of the VRAM sampled by the
// card (smooth: trilinear to the slot's levels; else the nearest texel of the nearest level).
float4 gpu_fragment(gpu_varyings i) {
    uint flags = i.texture_info.r, mode = flags & 0xff, slot = i.texture_info.g;
    float2 size = float2(i.texture_info.b, i.texture_info.a);
    float4 c = i.colour;
    float2 uv = i.uv;
    if (flags & FRAGMENT_LAID) {
        uv += i.position.xy * c.xy / 1024.0;
        c = 1.0;
    }
    // (how far apart the texels of neighbouring pixels are, before any branch: the level the texture is sampled at)
    float2 across = ddx(uv) * size, down = ddy(uv) * size;
    float lod = min(max(0.5 * log2(max(dot(across, across), dot(down, down))), 0.0), (float)VRAM_LOD(slot));
    [branch]
    if (mode == FRAGMENT_SPRITE) return gpu_sprite_fragment(i);
    [branch]
    if (flags & FRAGMENT_LIT) c.rgb *= gpu_light(i);
    [branch]
    if (mode == FRAGMENT_COLOUR || size.x == 0 || size.y == 0) return saturate(c);
    if (!VRAM_IS(slot)) return float4(1.0, 0.0, 1.0, 1.0);   // (a texture not uploaded)
    float2 corner = float2(VRAM_X(slot), VRAM_Y(slot));
    [branch]
    if (mode == FRAGMENT_MSDF) {
        // a glyph's coverage as four quarters' (2 x 2 samples), each the median of the three fields the sampler weighs at
        // level 0 (the quad's corners are the centres of the box's corner pixels); i.key the range in screen pixels
        float range = 2.0 * asfloat(i.key), cover = 0.0;
        [unroll]
        for (uint n = 0; n < 4; n++) {
            float2 at = saturate(uv + float2((n & 1) ? 0.25 : -0.25, (n & 2) ? 0.25 : -0.25) * i.light_uv) * (size - 1.0);
            float3 f = _GpuVram.SampleLevel(sampler_linear_clamp, (corner + at + 0.5) / VRAM_SIDE, 0).rgb;
            cover += saturate(range * (max(min(f.r, f.g), min(max(f.r, f.g), f.b)) - 0.5) + 0.5);
        }
        if (cover <= 0.0) discard;
        c.a *= 0.25 * cover;
        return saturate(c);
    }
    float2 at = (corner + ((flags & FRAGMENT_CLAMPED) ? saturate(uv) * 0.99999 : frac(uv)) * size) / VRAM_SIDE;
    float4 t = (flags & FRAGMENT_SMOOTH) && mode != FRAGMENT_MASK && mode != FRAGMENT_COVER
             ? _GpuVram.SampleLevel(sampler_linear_clamp, at, lod) : _GpuVram.SampleLevel(sampler_point_clamp, at, floor(lod + 0.5));
    [branch]
    if (mode == FRAGMENT_MASK || mode == FRAGMENT_COVER) {
        // (a mask's and a coverage's alpha is the upload's: the bit, the coverage; their colour the draw's)
        if (t.a <= (mode == FRAGMENT_MASK ? 0.5 : 0.0)) discard;
        if (mode == FRAGMENT_COVER) c.a *= t.a;
        return saturate(c);
    }
    if (slot & VRAM_PREMULTIPLIED) {
        // a key's texels are 0 and count for nothing; where they are most of it, nothing is drawn
        if (t.a < 0.5) discard;
        t = float4(t.rgb / t.a, 1.0);
    } else if ((flags & FRAGMENT_KEYED) && t.a == 0.0) {
        discard;   // (a keyed draw of a slot uploaded plain: its key is the word of no alpha)
    }
    return saturate(c * t);
}

// The holodeck (docs/world/holodeck.md, docs/world/volume.md): the device's last list as the front end made it, seen from a
// visitor's eyes. It reads no RAM: a vertex's place in its program's camera, its kind and its projection are the front end's
// (gpu_vertex_sized), the frame's camera its row past the mesh's (gpu_frame_texel).
float4 volume_frame(uint k) { return _GpuMadeEye.Load(int3(k, GPU_DRAW_IDS_H, 0)); }
uint volume_up() { return (uint)volume_frame(3).x; }
float2 volume_picture() { return gpu_made_picture(); }
// The frame's camera: the rows of its turn (camera = rows * world + move). False: no draw in perspective.
bool volume_view(out float3 rx, out float3 ry, out float3 rz) {
    rx = volume_frame(0).xyz, ry = volume_frame(1).xyz, rz = volume_frame(2).xyz;
    return volume_frame(0).w != 0.0;
}
// Whether mesh vertices v to v + 2 are one command's of this kind (VOLUME_WORLD or VOLUME_FLAT) in a pass that blends as
// pass `p` of the holodeck's four (VOLUME_BLENDING: passes 4 to 7, of no depth test, as the pass that blends alike).
bool volume_triangle(uint v, float kind, uint p) {
    uint2 c = gpu_draw_id(v);
    if (v + 3 > gpu_made_last() || (c.g & 0x3fffu) == 0 || gpu_draw_id(v + 2).r != c.r) return false;
    if (_GpuMadeEye.Load(int3(gpu_made_at(v), 0)).w != kind) return false;
    return VOLUME_BLENDING(firstbitlow(c.g & 0x3fffu)) == p;
}
// Mesh vertex `id`: as the device draws it, and its place in its program's camera, the projection's near and far planes
// and its x and y scales (the picture's edges are at x = +-z / focal.x and y = +-z / focal.y).
gpu_varyings volume_vertex(uint id, out float3 eye, out float2 planes, out float2 focal) {
    int3 at = int3(gpu_made_at(id), 0);
    float4 p = _GpuMadeProj.Load(at);
    eye = _GpuMadeEye.Load(at).xyz, planes = p.xy, focal = p.zw;
    return gpu_vertex_made(id);
}

#endif
