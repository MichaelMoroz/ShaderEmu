#ifndef GPU_H
#define GPU_H

// The GPU device. See docs/machine/gpu.md for the guest's view; programs/common/gpu.h has the same
// numbers for C. GPU_STATE is the machine's state texture (RAM from row 64 on).
//
// Addresses below are RAM texel indices: (physical address - 0x80000000) / 16.
#define GPU_DISPLAY 0x700000u   // display control: mode, width, height
#define GPU_CTRL    0x700001u   // submit, command list address, command count, frames done
#define GPU_PIXELS  0x700100u   // the display's framebuffer in RAM, for writing the picture back
#define GPU_INTO    0x700005u   // where SUBMIT_INTO writes the picture: address, width, height, row length
#define GPU_COPY    0x700007u   // the copy still to do for the list just drawn: how (0: none), address, width | height << 16, row length
#define GPU_CLOCK   0x700003u   // word 1: the host's clock in milliseconds; word 2: copies made so far; word 3: host flags

// Submit word: what to do with the list.
#define SUBMIT_DRAW 1        // draw it
#define SUBMIT_WRITEBACK 2   // afterwards copy the picture into the RAM framebuffer
#define SUBMIT_INTO 4        // afterwards copy it into the rectangle of RAM at GPU_INTO, whose size the picture has
#define SUBMIT_COPIES (SUBMIT_WRITEBACK | SUBMIT_INTO)
// Bits 9-15: the list has commands for passes 1-7. It is drawn, and the submit word taken back,
// once the host draws all of those (it learns of them by reading these words back).
#define SUBMIT_PASSES(submit) (((submit) >> 8) & 0xfe)

#define GPU_TARGET 2048.0       // the GPU's render target is GPU_TARGET pixels square,
// unless the host draws into a smaller one and says so (the picture cannot be larger than it)
#ifndef GPU_TARGET_W
#define GPU_TARGET_W GPU_TARGET
#define GPU_TARGET_H GPU_TARGET
#endif
#define GPU_MAX_COMMANDS 4096

#define CMD_END 0
#define CMD_CLEAR 1     // 3 vertices
#define CMD_RECT 2      // 6 vertices
#define CMD_DRAW 3      // `count` vertices
#define CMD_TEXT 4      // 6 vertices a character

// How a draw's vertices are projected.
#define VERTEX_SCREEN 0   // position is already in pixels (x, y) and depth (z, 0..1)
#define VERTEX_CLIP 1     // uniforms c0-c3 are a clip matrix; colour is the vertex colour
#define VERTEX_LIT 2      // also c4-c6 normal matrix, c7 light direction, c8 diffuse, c9 ambient
#define VERTEX_MODELVIEW 0x100   // flag: c0-c3 is the projection alone and c4-c6 the modelview's rows
#define VERTEX_QUADS 0x200       // flag: the buffer holds four corners per quad, in order round it, for every six vertices drawn
#define VERTEX_COMPACT 0x400     // flag: a vertex is one texel: x, y, z, then u | v << 16 in 1/1024ths; the colour is the command's word 11
#define VERTEX_FLOAT 0x800       // flag: the draw's numbers (vertices, uniforms, a laid texture's four) are floats, not 16.16
#define VERTEX_TAGGED 0x1000     // flag (compact): the fourth word is u | v << 12 | tag << 24, u and v 12 bits of 1/1024ths
#define VERTEX_TABLE 0x2000      // flag (tagged): word 11 is the address of a table of colours, and a vertex has its tag's
#define VERTEX_PACKED 0x4000     // flag (tagged): a vertex is a word, x | y << 8 | z << 16 | tag << 24; u and v are a word each at word 12's address
#define VERTEX_POINTS 0x8000     // flag (compact, modelview): a vertex a triangle, which faces the eye; words 12-14 its size, growth and from where
#define VERTEX_FACING 0x20000    // flag (tagged): word 11 is the address of a table of the ways a vertex faces, and it is lit from word 13's three texels
#define VERTEX_MOVED 0x10000     // flag: word 15 is the address of two rows of three numbers the texture coordinates are turned and moved by

// How fragments are coloured (low byte), plus flags.
#define FRAGMENT_COLOUR 0     // interpolated colour
#define FRAGMENT_TEXTURE 1    // 0x00RRGGBB texture times colour
#define FRAGMENT_INDEXED 2    // 8-bit texture through the display palette, times colour
#define FRAGMENT_MASK 3       // 1-bit texture, rows of whole bytes, leftmost bit highest: set bits take the colour
#define FRAGMENT_RGB24 4      // texture of three bytes a pixel (red, green, blue), rows with no padding, at any byte address, in RAM or ROM; times colour
#define FRAGMENT_TILES 5      // a layer of tiles: the address is a texel of four words (cells, tiles, palette, sizes; docs/machine/gpu.md)
#define FRAGMENT_YUV 6        // a picture as three planes of bytes one after another: luma, then the two of chroma at half the size each way (a video's frame)
#define FRAGMENT_COVER 7      // (text) four bits a pixel of how much of it the glyph covers, rows of whole bytes, the left pixel in the high half
#define FRAGMENT_MSDF 8       // (text) a glyph's three distance fields, three bytes a pixel, sampled bilinearly; their median is the edge
#define TEXT_MORE 0x100       // flag (text): words 9-11 are a scale, a slant and what a glyph's bitmap address is from
#define FRAGMENT_KEYED 0x100  // flag: texels equal to the key colour (or index) are not drawn
#define FRAGMENT_LAID 0x200   // flag (draws): the texture lies on the picture, not on the surface (a sky)
#define FRAGMENT_SMOOTH 0x400 // flag (word, indexed and three-byte textures): the four texels round the point, weighed (bilinear)
#define FRAGMENT_CLAMPED 0x1000 // flag: the texture does not repeat: beyond its edge the edge goes on
#define FRAGMENT_LIT 0x800    // flag (compact draws): times a light map, a three-byte texture, at a second pair of coordinates a vertex (words 12-14)
// Bits 16-18 of the same word: the pass the command is drawn in. Passes are drawn in order, each
// with fixed blending and depth use; within a pass commands keep the list's order.
//   0 opaque   1 alpha blend   2 additive   3 multiply     depth tested; only pass 0 writes it
//   4 opaque   5 alpha blend   6 additive   7 multiply     no depth
#define FRAGMENT_PASS(mode) (((mode) >> 16) & 7)

#ifdef GVM_RAM
// gpu_vm (gpu_vm/platform.md): RAM is a texture of its own, 8192 texels wide from address 0, and the device window is
// the RISC-V machine's moved down by 0x86000000. Indices here stay the RISC-V machine's ((address - 0x80000000) / 16):
// gpu_vm's texel is the index less 0x600000, and a guest's address below the ROM is a gpu_vm one (a constant of this
// file, 0x87000400 say, is the RISC-V machine's).
// GVM_MAP 1 (the tick's RAM_MAP): texel t at the strips' place (gpu_vm/isa.h GVM_STRIP_X), the ROM's too
#if defined(GVM_MAP) && GVM_MAP == 1
uint2 gvm_at(uint t) { return uint2((t & 0x1ffu) | ((t >> 12) & ~0x1ffu), (t >> 9) & 0xfffu); }
uint gvm_texel(uint2 p) { return (p.x & 0x1ffu) | ((p.y & 0xfffu) << 9) | ((p.x >> 9) << 21); }
#else
uint2 gvm_at(uint t) { return uint2(t % 8192u, t / 8192u); }
uint gvm_texel(uint2 p) { return p.y * 8192u + p.x; }
#endif
uint4 ram(uint index) {
    uint t = index - 0x600000u;
    return GPU_STATE[gvm_at(t)];
}
uint texel_of(uint address) {
    return ((address < 0x40000000u ? address + 0x86000000u : address) & 0x7fffffff) >> 4;
}
#else
#ifndef GPU_STATE_ROW
#define GPU_STATE_ROW 64u   // the state texture's row RAM starts in
#endif
uint4 ram(uint index) {
    return GPU_STATE[uint2(index % 2048, GPU_STATE_ROW + index / 2048)];
}
uint texel_of(uint address) {
    return (address & 0x7fffffff) >> 4;
}
#endif
uint ram_word(uint address) {
    uint4 t = ram(texel_of(address));
    uint k = (address >> 2) & 3;
    return k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a;
}
// A word of a texture: RAM, or with GPU_ROM the ROM (addresses from 0x40000000), read as the
// CPU reads it: four textures, one for each word of a 16-byte texel, rows bottom up.
#if defined(GPU_ROM) && defined(GVM_RAM)
// (gpu_vm: the ROM is one texture as wide as RAM's, offset o at texel o >> 4, rows top down)
uint texture_word(uint address) {
    [branch]
    if ((address >> 30) != 1) return ram_word(address);
    uint o = address - 0x40000000u, t = o >> 4, k = (o >> 2) & 3;
    uint4 v = _GvmRom.Load(int3(gvm_at(t), 0));
    return k == 0 ? v.r : k == 1 ? v.g : k == 2 ? v.b : v.a;
}
uint4 texture_texel(uint address) {
    [branch]
    if ((address >> 30) != 1) return ram(texel_of(address));
    return uint4(texture_word(address), texture_word(address + 4), texture_word(address + 8), texture_word(address + 12));
}
#elif defined(GPU_ROM)
uint texture_word(uint address) {
    [branch]
    if ((address >> 30) != 1) return ram_word(address);
    uint2 dim;
    _Data_MTD_R.GetDimensions(dim.x, dim.y);
    uint texel = (address - 0x40000000) >> 4, k = (address >> 2) & 3;
    uint2 at = uint2(texel % dim.x, dim.y - 1 - texel / dim.x);
    float4 t;
    if (k == 0) t = _Data_MTD_R[at];
    else if (k == 1) t = _Data_MTD_G[at];
    else if (k == 2) t = _Data_MTD_B[at];
    else t = _Data_MTD_A[at];
    uint4 b = (uint4)(t * 255.0 + 0.5);
    return b.r | b.g << 8 | b.b << 16 | b.a << 24;
}
// A 16-byte texel there.
uint4 texture_texel(uint address) {
    [branch]
    if ((address >> 30) != 1) return ram(texel_of(address));
    return uint4(texture_word(address), texture_word(address + 4), texture_word(address + 8), texture_word(address + 12));
}
#else
#define texture_word ram_word
uint4 texture_texel(uint address) { return ram(texel_of(address)); }
#endif
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
    if (how.r & VERTEX_FACING) {
        // A vertex's tag is which of 256 ways it faces (a texel each of floats, in the model's
        // own space); the light is three texels: from where, its colour, and the colour of
        // the light from everywhere. The colour is that last and, by how it faces the light, the first.
        float3 facing = asfloat(ram(texel_of(more.a) + (t.a >> 24))).xyz;
        uint light = texel_of(ram(base + 3).g);
        colour = float4(saturate(asfloat(ram(light + 2)).rgb + max(dot(facing, asfloat(ram(light)).xyz), 0.0) * asfloat(ram(light + 1)).rgb), 1.0);
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
};

// Pixels (x right, y down, depth 0..1) to the corner of the render target the display shows.
float4 place_pixels(float2 p, float depth) {
    return float4(p.x * 2.0 / GPU_TARGET_W - 1.0, 1.0 - p.y * 2.0 / GPU_TARGET_H, depth, 1.0);
}

// The command that claims mesh vertices `id` to `id + span - 1`: its first texel and first
// word (op, a, b, first slot). False while no list is submitted, when no command claims all of
// them, or when the command belongs to another pass than the one being drawn.
bool gpu_command(uint id, uint span, out uint base, out uint4 head) {
    base = 0;
    head = 0;
    uint4 ctrl = ram(GPU_CTRL);
    uint list = texel_of(ctrl.g), count = min(ctrl.b, GPU_MAX_COMMANDS);
    if ((ctrl.r & SUBMIT_DRAW) == 0 || count == 0) return false;
#ifdef GPU_PASS_UNIFORMS
    if ((SUBMIT_PASSES(ctrl.r) & ~_GpuPasses) != 0) return false;   // not until every pass it needs is drawn
#endif
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
    uint op = head.r;
    uint slots = op == CMD_CLEAR ? 3 : op == CMD_RECT ? 6 : op == CMD_DRAW ? head.b : op == CMD_TEXT ? 6 * ram(base + 1).b : 0;
    if (id < head.a || id + span > head.a + slots) return false;
#ifdef GPU_PASS_UNIFORMS
    uint drawn_in = op == CMD_RECT || op == CMD_TEXT ? FRAGMENT_PASS(ram(base + 2).r) : op == CMD_DRAW ? FRAGMENT_PASS(ram(base + 1).g) : 0;
    if (drawn_in != _GpuPass) return false;
#endif
    return true;
}

// (text, FRAGMENT_MSDF) how much steeper than a box filter's the coverage rises across an edge:
// fitted to the hinted bitmaps at 11 to 17 pixels (docs/system/fonts.md, "Distance fields")
#define MSDF_SHARP 1.2

// Mesh vertex `id`, which the command at `base` claims, placed and coloured for a picture of
// `size` pixels.
gpu_varyings gpu_vertex_sized(uint id, uint base, uint4 head, float2 size) {
    gpu_varyings o;
    o.position = float4(2, 2, 2, 1);
    o.colour = 0;
    o.uv = 0;
    o.texture_info = 0;
    o.key = 0;
    o.light_uv = 0;
    o.light_info = 0;
    {
        uint op = head.r;
        uint k = id - head.a;
        [branch]
        if (op == CMD_CLEAR) {
            // one triangle over the whole picture, at the far plane
            o.position = place_pixels(float2(k == 1 ? 2.0 : 0.0, k == 2 ? 2.0 : 0.0) * size, 1.0);
            o.colour = colour_of(head.g);
        } else if (op == CMD_RECT) {
            // head.g colour; then x0, y0, x1, y1; mode, texture address, width, height;
            // u0, v0, u1, v1 (16.16). head.b is the key
            uint4 box = ram(base + 1), tex = ram(base + 2);
            float4 uv = from_fixed(ram(base + 3));
            uint corner = k < 3 ? k : (k == 3 ? 2 : (k == 4 ? 1 : 3));   // 0 1 2, 2 1 3
            bool right = (corner & 1) != 0, low = (corner & 2) != 0;
            o.position = place_pixels(float2(asint(right ? box.b : box.r), asint(low ? box.a : box.g)), 0.0);
            o.colour = colour_of(head.g);
            o.uv = float2(right ? uv.z : uv.x, low ? uv.w : uv.y);
            o.texture_info = tex;
            o.key = head.b;
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
            o.colour = colour_of(head.g);
            o.uv = float2(right ? 1.0 : 0.0, low ? 1.0 : 0.0);
            o.texture_info = uint4((msdf ? FRAGMENT_MSDF : (how.r & 0xff) == FRAGMENT_COVER ? FRAGMENT_COVER : FRAGMENT_MASK) | (how.r & 0x70000),
                                   glyph.r + (more ? how.a : 0), msdf ? glyph.g & 0xffff : glyph.g, msdf ? glyph.g >> 16 : glyph.b);
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
            if (how.r & VERTEX_COMPACT) {
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
            } else {
                // with VERTEX_MODELVIEW the GPU does the matrix product the program would have done
                if (how.r & VERTEX_MODELVIEW)
                    pos = float4(dot(gpu_number(fl, ram(u + 4)), pos), dot(gpu_number(fl, ram(u + 5)), pos),
                                 dot(gpu_number(fl, ram(u + 6)), pos), pos.w);
                if (how.r & VERTEX_POINTS) pos.xy += gpu_corner(gpu_number(fl, ram(base + 3)), corner, -pos.z);
                float4 clip = float4(dot(gpu_number(fl, ram(u)), pos), dot(gpu_number(fl, ram(u + 1)), pos),
                                     dot(gpu_number(fl, ram(u + 2)), pos), dot(gpu_number(fl, ram(u + 3)), pos));
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

// The same for the list that is submitted (gpu_command): its picture is the display's, or the
// rectangle it is drawn into.
gpu_varyings gpu_vertex_of(uint id, uint base, uint4 head) {
    uint4 ctrl = ram(GPU_CTRL);
    uint4 disp = ram(GPU_DISPLAY);
    float2 size = float2(disp.g, disp.b);
    if (ctrl.r & SUBMIT_INTO) {
        uint4 into = ram(GPU_INTO);
        size = float2(into.g, into.b);
    }
    return gpu_vertex_sized(id, base, head, size);
}

// The mesh's vertex `id` belongs to whichever command claims that slot. Vertices no command
// claims, and everything while no list is submitted, collapse to nothing.
gpu_varyings gpu_vertex(uint id) {
    gpu_varyings o;
    o.position = float4(2, 2, 2, 1);   // outside the clip volume
    o.colour = 0;
    o.uv = 0;
    o.texture_info = 0;
    o.key = 0;
    o.light_uv = 0;
    o.light_info = 0;
    uint base;
    uint4 head;
    [branch]
    if (gpu_command(id, 1, base, head)) o = gpu_vertex_of(id, base, head);
    return o;
}

// Texel n of a texture of three bytes a pixel as a colour word: it may start at any byte, and
// three bytes may run into the next word. In RAM or the ROM.
uint gpu_rgb24(uint address, uint n) {
    uint first = address + 3 * n, at = first & ~3u, shift = 8 * (first & 3);
    uint texel = texture_word(at) >> shift;
    if (shift > 8) texel |= texture_word(at + 4) << (32 - shift);
    return (texel & 0xff) << 16 | (texel & 0xff00) | ((texel >> 16) & 0xff);
}

// A texel of an indexed, a word or a three-byte texture as a colour, and whether it is drawn
// at all (the key colour is not, when the draw is keyed). Words and bytes may be in the ROM.
float4 gpu_texel(gpu_varyings i, uint mode, uint x, uint y, uint width, out float there) {
    uint n = y * width + x, texel;
    if (mode == FRAGMENT_INDEXED) {
        texel = (ram_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
        there = (i.texture_info.r & FRAGMENT_KEYED) && texel == i.key ? 0.0 : 1.0;
        texel = ram_word(0x87000400 + 4 * texel);
    } else {
        if (mode == FRAGMENT_RGB24) texel = gpu_rgb24(i.texture_info.g, n);
        else texel = texture_word(i.texture_info.g + 4 * n);
        there = (i.texture_info.r & FRAGMENT_KEYED) && texel == i.key ? 0.0 : 1.0;
    }
    return colour_of(texel);
}

// The light a lit draw's fragment is times: its light map's four texels round the point,
// weighed, the map's edge going on beyond it.
float3 gpu_light(gpu_varyings i) {
    uint width = i.light_info.g, height = i.light_info.b;
    if (width == 0 || height == 0) return 1.0;
    float2 at = saturate(i.light_uv) * float2(width, height) - 0.5;
    float2 low = floor(at), part = at - low;
    uint x0 = (uint)clamp((int)low.x, 0, (int)width - 1), y0 = (uint)clamp((int)low.y, 0, (int)height - 1);
    uint x1 = min(x0 + 1, width - 1), y1 = min(y0 + 1, height - 1);
    if (low.x < 0.0) x1 = x0;
    if (low.y < 0.0) y1 = y0;
    float3 c00 = colour_of(gpu_rgb24(i.light_info.r, y0 * width + x0)).rgb, c10 = colour_of(gpu_rgb24(i.light_info.r, y0 * width + x1)).rgb;
    float3 c01 = colour_of(gpu_rgb24(i.light_info.r, y1 * width + x0)).rgb, c11 = colour_of(gpu_rgb24(i.light_info.r, y1 * width + x1)).rgb;
    return lerp(lerp(c00, c10, part.x), lerp(c01, c11, part.x), part.y);
}

// Pixels n and n + 1 of a glyph's distance fields, three bytes a pixel, in RAM or the ROM.
void gpu_msdf_pair(uint address, uint n, out float3 a, out float3 b) {
    uint first = address + 3 * n, at = first & ~3u, shift = 8 * (first & 3);
    uint w0 = texture_word(at), w1 = texture_word(at + 4), w2 = 0;
    [branch]
    if (shift == 24) w2 = texture_word(at + 8);
    uint lo = shift != 0 ? (w0 >> shift) | (w1 << (32 - shift)) : w0;
    uint hi = shift != 0 ? (w1 >> shift) | (w2 << (32 - shift)) : w1;
    a = float3(lo & 0xff, (lo >> 8) & 0xff, (lo >> 16) & 0xff) / 255.0;
    b = float3(lo >> 24, hi & 0xff, (hi >> 8) & 0xff) / 255.0;
}

float4 gpu_fragment(gpu_varyings i) {
    uint mode = i.texture_info.r & 0xff, width = i.texture_info.b, height = i.texture_info.a;
    float4 c = i.colour;
    float2 uv = i.uv;
    if (i.texture_info.r & FRAGMENT_LAID) {
        uv += i.position.xy * c.xy / 1024.0;
        c = 1.0;
    }
    [branch]
    if (i.texture_info.r & FRAGMENT_LIT) c.rgb *= gpu_light(i);
    [branch]
    if (mode == FRAGMENT_MSDF) {
        // the pixel's coverage as four quarters' (2 x 2 samples): at each the four atlas pixels
        // round it, weighed, their channels' median the distance (the quad's corners are the
        // centres of the box's corner pixels). i.key: the range in screen pixels
        if (width < 2 || height < 2) discard;
        float range = 2.0 * asfloat(i.key), cover = 0.0;
        [loop]
        for (uint n = 0; n < 4; n++) {
            float2 at = saturate(uv + float2((n & 1) ? 0.25 : -0.25, (n & 2) ? 0.25 : -0.25) * i.light_uv) * float2(width - 1, height - 1);
            uint x0 = min((uint)at.x, width - 2), y0 = min((uint)at.y, height - 2);
            float2 part = at - float2(x0, y0);
            float3 s00, s10, s01, s11;
            gpu_msdf_pair(i.texture_info.g, y0 * width + x0, s00, s10);
            gpu_msdf_pair(i.texture_info.g, (y0 + 1) * width + x0, s01, s11);
            float3 s = lerp(lerp(s00, s10, part.x), lerp(s01, s11, part.x), part.y);
            cover += saturate(range * (max(min(s.r, s.g), min(max(s.r, s.g), s.b)) - 0.5) + 0.5);
        }
        if (cover <= 0.0) discard;
        c.a *= 0.25 * cover;
        return saturate(c);
    }
    [branch]
    if ((i.texture_info.r & FRAGMENT_SMOOTH) && (mode == FRAGMENT_INDEXED || mode == FRAGMENT_TEXTURE || mode == FRAGMENT_RGB24) && width != 0 && height != 0) {
        // the four texels round the point, repeating, each by how near its centre is. A texel
        // of the key colour counts for nothing; where those are most of the four, nothing is drawn.
        bool clamped = (i.texture_info.r & FRAGMENT_CLAMPED) != 0;
        float2 at = (clamped ? saturate(uv) : frac(uv)) * float2(width, height) - 0.5;
        float2 low = floor(at), part = at - low;
        uint x0 = (uint)((int)low.x + (int)width) % width, y0 = (uint)((int)low.y + (int)height) % height;
        uint x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
        if (clamped) {
            x0 = (uint)clamp((int)low.x, 0, (int)width - 1), y0 = (uint)clamp((int)low.y, 0, (int)height - 1);
            x1 = (uint)clamp((int)low.x + 1, 0, (int)width - 1), y1 = (uint)clamp((int)low.y + 1, 0, (int)height - 1);
        }
        float t00, t10, t01, t11;
        float4 c00 = gpu_texel(i, mode, x0, y0, width, t00), c10 = gpu_texel(i, mode, x1, y0, width, t10);
        float4 c01 = gpu_texel(i, mode, x0, y1, width, t01), c11 = gpu_texel(i, mode, x1, y1, width, t11);
        float w00 = (1 - part.x) * (1 - part.y) * t00, w10 = part.x * (1 - part.y) * t10;
        float w01 = (1 - part.x) * part.y * t01, w11 = part.x * part.y * t11;
        float all = w00 + w10 + w01 + w11;
        if (all < 0.5) discard;
        return saturate(c * (c00 * w00 + c10 * w10 + c01 * w01 + c11 * w11) / all);
    }
    [branch]
    if (mode != FRAGMENT_COLOUR && width != 0 && height != 0) {
        // nearest texel, repeating
        if (i.texture_info.r & FRAGMENT_CLAMPED) uv = saturate(uv) * 0.99999;
        uint x = min((uint)(frac(uv.x) * width), width - 1), y = min((uint)(frac(uv.y) * height), height - 1);
        uint n = y * width + x, texel;
        if (mode == FRAGMENT_MASK) {
            n = y * ((width + 7) >> 3) + (x >> 3);
            texel = (texture_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
            if (((texel << (x & 7)) & 0x80) == 0) discard;
            return saturate(c);
        }
        if (mode == FRAGMENT_COVER) {
            // a glyph's coverage is the colour's alpha: drawn in a pass that blends
            n = y * ((width + 1) >> 1) + (x >> 1);
            texel = (texture_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
            texel = (x & 1) ? texel & 15 : texel >> 4;
            if (texel == 0) discard;
            c.a *= (float)texel / 15.0;
            return saturate(c);
        }
        if (mode == FRAGMENT_TILES) {
            // the pixel's cell (16 bits: a tile, mirrored across at bit 10 and down at bit 11, a
            // palette bank), that tile's byte for the pixel, and the bank's colour of that number
            uint4 layer = ram(texel_of(i.texture_info.g));
            uint tile_w = max(layer.a & 0xff, 1), tile_h = max((layer.a >> 8) & 0xff, 1);
            uint cell_at = layer.r + 2 * ((y / tile_h) * ((layer.a >> 16) & 0xfff) + x / tile_w);
            uint cell = (ram_word(cell_at & ~3u) >> (8 * (cell_at & 3))) & 0xffff;
            uint px = x % tile_w, py = y % tile_h;
            if (cell & 0x400) px = tile_w - 1 - px;
            if (cell & 0x800) py = tile_h - 1 - py;
            // (tiles in pieces of 2^n: then the tiles' address is a table of the pieces')
            uint tile = cell & 0x3ff, piece = layer.a >> 28, tiles = layer.g;
            if (piece != 0) {
                tiles = ram_word(layer.g + 4 * (tile >> piece));
                tile &= (1u << piece) - 1;
            }
            uint at = tiles + (tile * tile_h + py) * tile_w + px;
            texel = (ram_word(at & ~3u) >> (8 * (at & 3))) & 0xff;
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
            texel = ram_word(layer.b + 4 * (texel + 16 * (cell >> 12)));
        } else if (mode == FRAGMENT_YUV) {
            // the pixel's luma, and the chroma of the 2 x 2 pixels it is one of (BT.601, 16 to 235)
            uint half_w = width >> 1, luma_at = i.texture_info.g + n;
            uint blue_at = i.texture_info.g + width * height + (y >> 1) * half_w + (x >> 1), red_at = blue_at + half_w * (height >> 1);
            float luma = 1.164 * ((float)((ram_word(luma_at & ~3u) >> (8 * (luma_at & 3))) & 0xff) - 16.0);
            float blue = (float)((ram_word(blue_at & ~3u) >> (8 * (blue_at & 3))) & 0xff) - 128.0;
            float red = (float)((ram_word(red_at & ~3u) >> (8 * (red_at & 3))) & 0xff) - 128.0;
            return saturate(c * float4((luma + 1.596 * red) / 255.0, (luma - 0.392 * blue - 0.813 * red) / 255.0, (luma + 2.017 * blue) / 255.0, 1.0));
        } else if (mode == FRAGMENT_INDEXED) {
            texel = (ram_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
            texel = ram_word(0x87000400 + 4 * texel);
        } else if (mode == FRAGMENT_RGB24) {
            texel = gpu_rgb24(i.texture_info.g, n);
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
        } else {
            texel = texture_word(i.texture_info.g + 4 * n);
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
        }
        c *= colour_of(texel);
    }
    return saturate(c);
}

#ifdef GPU_VOLUME
// The volume display (docs/world/volume.md): a host that shows a 3D program's last whole frame from
// a point of view of its own, instead of the picture the GPU drew of it.
#define VOLUME_LIST 0x700030u   // that frame's list address, command count, frames so far, picture size
// Which of the program's axes is up, where the program says (the top four bits of the size
// word: 1 x, 2 y, 3 z); 0: it does not, and a host works it out from the camera.
uint volume_up() { return ram(VOLUME_LIST).a >> 28; }

// The frame's command that claims mesh vertices id to id + 2, if it is a draw in perspective
// with its own modelview: the only kind that has a place in space.
bool volume_command(uint id, out uint base, out uint4 head) {
    base = 0;
    head = 0;
    uint4 frame = ram(VOLUME_LIST);
    uint list = texel_of(frame.r), count = min(frame.g, GPU_MAX_COMMANDS);
    if (frame.r == 0 || count == 0) return false;
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
    if (head.r != CMD_DRAW || id < head.a || id + 3 > head.a + head.b) return false;
    uint4 how = ram(base + 1);
    if ((how.r & VERTEX_MODELVIEW) == 0 || (how.r & 0xff) == VERTEX_SCREEN) return false;
#ifdef GPU_VOLUME_PASS
    // the pass being drawn: the command's own, or for passes 4-7 (no depth test) the one that blends alike
    if ((FRAGMENT_PASS(how.g) & 3) != GPU_VOLUME_PASS) return false;
#endif
    // a projection whose w does not come from z draws flat on the picture (a status bar)
    return ram(texel_of(how.b) + 3).b != 0;
}

// The frame's camera: the modelview of its first draw with a place in space, which for a
// program that draws its world before what moves in it is the view alone. Rows of its
// rotation and its translation: camera = rows * world + move. False: no such draw.
bool volume_view(out float3 rx, out float3 ry, out float3 rz, out float3 move) {
    rx = float3(1, 0, 0); ry = float3(0, 1, 0); rz = float3(0, 0, 1); move = 0;
    uint4 frame = ram(VOLUME_LIST);
    uint list = texel_of(frame.r), count = min(frame.g, 24u);
    if (frame.r == 0) return false;
    for (uint i = 0; i < count; i++) {
        uint at = list + 4 * i;
        uint4 how = ram(at + 1);
        if (ram(at).r != CMD_DRAW || (how.r & VERTEX_MODELVIEW) == 0 || (how.r & 0xff) == VERTEX_SCREEN) continue;
        uint u = texel_of(how.b);
        if (ram(u + 3).b == 0) continue;
        bool fl = (how.r & VERTEX_FLOAT) != 0;
        float4 mx = gpu_number(fl, ram(u + 4)), my = gpu_number(fl, ram(u + 5)), mz = gpu_number(fl, ram(u + 6));
        rx = mx.xyz; ry = my.xyz; rz = mz.xyz; move = float3(mx.w, my.w, mz.w);
        return true;
    }
    return false;
}

// Mesh vertex `id` of such a command as the program's camera sees it, in the camera's own
// units (x right, y up, the view along -z), coloured as the GPU colours it. `planes` is how
// far the camera's near and far planes are and `focal` the projection's x and y scales: the
// picture's edges are at x = +-z / focal.x and y = +-z / focal.y.
gpu_varyings volume_vertex(uint id, uint base, uint4 head, out float3 eye, out float2 planes, out float2 focal) {
    gpu_varyings o;
    o.position = 0;
    o.texture_info = 0;
    o.light_uv = 0;
    o.light_info = 0;
    uint4 how = ram(base + 1), more = ram(base + 2);
    uint u = texel_of(how.b), k = id - head.a;
    bool fl = (how.r & VERTEX_FLOAT) != 0;
    if (how.r & VERTEX_QUADS) {
        uint part = k % 6;
        k = (k / 6) * 4 + (part < 3 ? part : part == 3 ? 0 : part - 2);
    }
    float4 pos, normal = 0;
    uint corner = 0;
    if (how.r & VERTEX_POINTS) { corner = k % 3; k = k / 3; }
    [branch]
    if (how.r & VERTEX_COMPACT) {
        gpu_compact_vertex(base, head, how, more, k, pos, o.uv, o.colour);
    } else {
        uint v = texel_of(head.g) + 4 * k;
        pos = gpu_number(fl, ram(v));
        normal = gpu_number(fl, ram(v + 1));
        o.uv = gpu_number(fl, ram(v + 2)).xy;
        o.colour = gpu_number(fl, ram(v + 3));
    }
    // every pass is drawn as pass 0: what blends there is solid here
    o.texture_info = uint4(how.g, how.a, more.r, more.g);
    o.key = more.b;
    if (how.r & VERTEX_MOVED) {
        uint rows = texel_of(ram(base + 3).a);
        float3 at = float3(o.uv, 1.0);
        o.uv = float2(dot(gpu_number(fl, ram(rows)).xyz, at), dot(gpu_number(fl, ram(rows + 1)).xyz, at));
    }
    if ((how.g & FRAGMENT_LIT) && (how.r & VERTEX_COMPACT)) {
        uint4 lit = ram(base + 3);
        uint at = ram_word(lit.r + 4 * k);
        o.light_uv = float2(at & 0xffff, at >> 16) / 65535.0;
        o.light_info = uint3(lit.g, lit.b & 0xffff, lit.b >> 16);
    }
    float4 mx = gpu_number(fl, ram(u + 4)), my = gpu_number(fl, ram(u + 5)), mz = gpu_number(fl, ram(u + 6));
    eye = float3(dot(mx, pos), dot(my, pos), dot(mz, pos));
    if (how.r & VERTEX_POINTS) eye.xy += gpu_corner(gpu_number(fl, ram(base + 3)), corner, -eye.z);
    if ((how.r & 0xff) == VERTEX_LIT) {
        float3 n = float3(dot(mx.xyz, normal.xyz), dot(my.xyz, normal.xyz), dot(mz.xyz, normal.xyz));
        float facing = max(dot(n, gpu_number(fl, ram(u + 7)).xyz), 0.0);
        o.colour = float4(facing * gpu_number(fl, ram(u + 8)).rgb + gpu_number(fl, ram(u + 9)).rgb, 1.0);
    }
    float4 depth = gpu_number(fl, ram(u + 2));   // OpenGL's third row: 0, 0, -(f+n)/(f-n), -2fn/(f-n)
    planes = float2(depth.w / (depth.z - 1.0), depth.w / (depth.z + 1.0));
    focal = float2(gpu_number(fl, ram(u)).x, gpu_number(fl, ram(u + 1)).y);
    return o;
}
#endif

#ifdef GPU_WRITEBACK
// The 4 MB bands of RAM the pending copy writes (ram_bands).
uint gpu_copy_bands() {
    uint4 copy = ram(GPU_COPY);
    if (copy.r == 0) return 0;
    uint first = GPU_PIXELS * 16, bytes;
    if (copy.r & SUBMIT_INTO) {
        first = copy.g & 0x7ffffffc;
        bytes = max(copy.a, copy.b & 0xffff) * (copy.b >> 16) * 4;
    } else {
        uint4 disp = ram(GPU_DISPLAY);
        bytes = disp.g * disp.b * 4;
    }
    return ram_bands(first >> 22, min((first + bytes) >> 22, (uint)(RAM_MAX - 1) >> 22));
}

// For the Commit pass. The control pass leaves the copy a drawn list asked for in GPU_COPY; the
// next commit makes it: the RAM texels it covers take the picture (one 0x00RRGGBB word per
// pixel) and GPU_COPY is cleared. False for every other texel.
bool gpu_writeback(uint2 pos, out uint4 result) {
    result = 0;
    if (pos.y < 64) return false;
    uint index = (pos.y - 64) * 2048 + pos.x;
    if (index - 0x600000u >= 0x1e0000u) return false;   // outside what a GPU program may own (0x86000000 to 0x87e00000)
    uint4 copy = ram(GPU_COPY);
    if (copy.r == 0) return false;
    if (index == GPU_COPY) return true;    // done: result is all zeros
    if (index == GPU_CLOCK) {
        result = GPU_STATE[pos];
        result.b += 1;
        return true;
    }
    uint first, width, height, row;
    if (copy.r & SUBMIT_INTO) {
        first = (copy.g & 0x7fffffff) >> 2;
        width = copy.b & 0xffff;
        height = copy.b >> 16;
        row = max(copy.a, width);
    } else {
        uint4 disp = ram(GPU_DISPLAY);
        first = GPU_PIXELS * 4;
        width = row = disp.g;
        height = disp.b;
    }
    uint word = index * 4;
    if (width == 0 || word + 3 < first || word >= first + row * height) return false;
    result = GPU_STATE[pos];
    bool any = false;
    for (uint k = 0; k < 4; k++) {
        if (word + k >= first) {
            uint p = word + k - first, y = p / row, x = p - y * row;
            if (x < width && y < height) {
                uint3 c = (uint3)(_GpuTarget.Load(int3(x, y, 0)).rgb * 255.0 + 0.5);
                result[k] = (c.r << 16) | (c.g << 8) | c.b;
                any = true;
            }
        }
    }
    return any;
}
#endif

#define INPUT_STATE 0x700002u   // pointer x, y (display pixels), buttons, key events so far
#define INPUT_KEYS  0x700008u   // ring of the last 32 key events, one word each
#define FETCH_REPLY 0x700011u   // requests the host has answered, bytes, status (docs/system/fetch.md)
#define CON_HOST    0x76b828u   // the console (docs/machine/console.md): characters delivered, the last of them
#ifndef CON_IN_TAG
#define CON_IN_TAG _UdonUARTInTag
#define CON_IN_CHARS _UdonUARTInChar
#endif
#define FETCH_DATA  0x76c000u   // what the host fetched: 0x4000 texels
// "Open this" from the host (docs/system/open.md): requests so far and the address's length, then the
// address, 176 bytes of it in the eleven texels after and 80 more from HOST_OPEN_MORE.
#define HOST_OPEN      0x700024u
#define HOST_OPEN_MORE 0x700031u
// What the cores did, for a guest to show (docs/machine/multicore.md): 16 words, core k's count of
// instructions in word k; then a texel of how many cores, which run and which are asleep (a bit each).
// With more than 16 cores: every core's count again in 64 texels at MC_STATS_ALL (the
// network's row, after its ring: 0x876ba000), then eight texels, one for every 32 cores: a
// word of which of them run, a bit each, and a word of which are asleep. (A pixel that loops
// over every core takes as long as hundreds of instructions of the tick: none does.)
#define MC_STATS    0x700038u
#define MC_STATS_ALL 0x76ba00u
// What the host says of where the machine is shown (docs/world/holodeck.md): two texels.
#define HOST_STATE  0x700036u

#if defined(GVM_RAM) && defined(CORES) && CORES > 1
// (gpu_vm: a core is a row of the tick's state; the host starts workers 1 to _GvmWorkers at the firmware, which
// parks a core in its loop at _GvmParkLo to _GvmParkHi: gpu_vm/platform.md)
int3 gvm_s16(uint core) {   // the pixel of texels 16 to 23: a row a core, or a block (_GvmGeo: 1 8 x 8, 128 a band of 16 rows;
    // 2 8 x 8 in one row; 3 4 x 8 in one row, gpu_vm/tick/WAVES.md)
    // 4: GvmTickOne's one target, core 0 72 columns and a worker 48, texel 16 + r at row r: _GvmS0 and _GvmS3 are the
    // same texture there, and texel 19 is gvm_s19's)
    return _GvmGeo == 4 ? int3(core == 0 ? 2 : 72 + 48 * (core - 1) + 2, 0, 0) : _GvmGeo == 3 ? int3(core * 4, 2, 0) : _GvmGeo == 2 ? int3(core * 8, 2, 0) : _GvmGeo != 0 ? int3((core & 127) * 8, (core >> 7) * 16 + 2, 0) : int3(2, core, 0);
}
int3 gvm_s19(uint core) { return gvm_s16(core) + (_GvmGeo == 4 ? int3(0, 3, 0) : int3(0, 0, 0)); }   // (texel 19's pixel in _GvmS3)
// (every load made, whether or not the core is there, and the loop unrolled: the 64 loads of a texel of bits go out
// together, not one core's after the last one's came back, 5 us a pass less: gpu_vm/IDEAS.md 38. A core past
// _GvmWorkers reads a texel of the state that is there and is masked off.)
uint mc_stat_count(uint core) {
    uint count = _GvmS3.Load(gvm_s19(min(core, 255u))).g;
    return core <= _GvmWorkers ? count : 0;
}
uint2 mc_stat_bits(uint from) {
    uint running = 0, asleep = 0;
    [unroll] for (uint n = 0; n < 32; n++) {
        uint core = from + n;
        uint4 s16 = _GvmS0.Load(gvm_s16(min(core, 255u))), s19 = _GvmS3.Load(gvm_s19(min(core, 255u)));
        bool there = core <= _GvmWorkers;
        bool parked = core != 0 && s16.x >= _GvmParkLo && s16.x < _GvmParkHi;
        running |= (there && !parked ? 1u : 0u) << n;
        asleep |= (there && !parked && core != 0 && s19.w == 2 && (s16.y & 8) == 0 ? 1u : 0u) << n;   // a user's wait ended its pass
    }
    return uint2(running, asleep);
}
uint mc_stat_cores() {
    return 1 + _GvmWorkers;
}
#elif defined(CORES) && CORES > 1
// Where worker `core`'s tiles begin in the strip (the geometry: src/types.h has the same), or
// 0xffff when there is no such worker; core 0's state is at the left.
uint mc_rows_of_bits(uint bits) {
    return bits == 0 ? 0 : (44 + 8 + ((7u << bits) >> 2) * 4 + 63) / 64;
}
// The hybrid geometry is the machine's (geotune lays it out); MC_UNIFORM compiles the old one.
#if !defined(MC_HYBRID) && !defined(MC_UNIFORM)
#define MC_HYBRID
#endif
#ifdef MC_HYBRID
// The hybrid geometry as src/types.h reads it (mc_geo_read: big workers first, the blocks'
// widths in .a), here again since not every pass that has this file has types.h: how many
// workers it has, and how many of them are big and how many tiles a big and a small one has.
#ifndef TICK_BLOCK
#define TICK_BLOCK 4
#endif
#ifndef TICK_PAD
#define TICK_PAD 2
#endif
#ifndef TICK_BLOCK_BIG
#define TICK_BLOCK_BIG 8
#endif
#ifndef TICK_PAD_BIG
#define TICK_PAD_BIG 0
#endif
#ifndef TICK_GAP
#define TICK_GAP 8
#endif
#ifndef TICK_MRT_W
#define TICK_MRT_W 1040
#endif
uint mc_geo_cols(uint rows, uint width, uint block, uint pad) {
    if (width != 0 && width <= 64) { block = width; pad = 0; }
    return (rows + pad + block - 1) / block * block;
}
uint mc_geo_count(uint4 geo, out uint nb, out uint rb, out uint rows) {
#ifdef MC_FORCE_BITS
    if (geo.b != 0) geo.b = MC_FORCE_BITS;
#endif
    uint avail = TICK_MRT_W - 8 - TICK_GAP, bb = (geo.a >> 8) & 0xff, per0 = 0, sb0;
    rows = geo.b < 3 || geo.b > 6 ? 0 : mc_rows_of_bits(geo.b);
    nb = 0;
    rb = 0;
    if (rows == 0 || (geo.r & 1) != 0) return 0;
    uint cols = mc_geo_cols(rows, (geo.a >> 16) & 0xff, TICK_BLOCK, TICK_PAD);
    if ((geo.a & 0xff) != 0 && bb >= 3 && bb <= 6) {
        rb = mc_rows_of_bits(bb);
        uint cb = mc_geo_cols(rb, geo.a >> 24, TICK_BLOCK_BIG, TICK_PAD_BIG);
        nb = min(min(min(geo.a & 0xff, 248 / rb), avail / cb), min(geo.g, (uint)CORES - 1));
        if (nb != 0) per0 = (avail - nb * cb) / cols;
    }
    sb0 = nb != 0 ? 1 : 0;
    return nb + min(min(geo.g, (uint)CORES - 1) - nb, min(((CORES + 47) / 49 - sb0) * (248 / rows), ((CORES + 126) / 128 - sb0) * (avail / cols) + per0));
}
uint2 mc_place(uint core) {
    if (core == 0) return uint2(0, 0);
    uint nb, rb, rows, count = mc_geo_count(ram(0x70003du), nb, rb, rows);   // MC_GEOMETRY
    if (core > count || core >= CORES) return uint2(64, 0xffff);
    if (core <= nb) return uint2(64, (core - 1) * rb);
    uint j = core - 1 - nb, per = 248 / rows, sb0 = nb != 0 ? 1 : 0;
    return uint2(64, (sb0 + j / per) * 248 + (j % per) * rows);
}
#else
uint2 mc_place(uint core) {
    if (core == 0) return uint2(0, 0);
    uint4 geo = ram(0x70003du);   // MC_GEOMETRY: how many workers, and their size
#ifdef MC_FORCE_BITS
    if (geo.b != 0) geo.b = MC_FORCE_BITS;
#endif
    uint rows = geo.b < 3 || geo.b > 6 ? 0 : mc_rows_of_bits(geo.b);
    uint j = core - 1, per = 248 / max(rows, 1);
    if (rows == 0 || (geo.r & 1) != 0 || core > geo.g || core >= CORES || j / per >= (CORES + 47) / 49) return uint2(64, 0xffff);
    return uint2(64, (j / per) * 248 + (j % per) * rows);
}
#endif
// Texel w (below 64) of a core's state: a worker's is in the first of its tiles, eight to a row.
uint4 mc_state_texel(uint2 at, uint w) {
    if (at.x == 0) return GPU_STATE[uint2(w, 0)];
    return GPU_STATE[uint2(64 + ((at.y % 248) << 3) + (w & 7), ((at.y / 248) << 3) + (w >> 3))];
}
uint mc_stat_count(uint core) {
    uint2 at = mc_place(core);
    return core < CORES && at.y != 0xffff ? mc_state_texel(at, 28).g : 0;
}
// Which of the 32 cores from `from` run, and which are asleep: a worker's state word
// (41,0).a has bit 0 while it runs and bit 1 while it sleeps on its job word.
uint2 mc_stat_bits(uint from) {
    uint running = 0, asleep = 0;
    for (uint n = 0; n < 32; n++) {
        uint core = from + n;
        uint2 at = mc_place(core);
        if (core >= CORES || at.y == 0xffff) continue;
        uint word = core == 0 ? 1 : mc_state_texel(at, 41).a;
        running |= (word & 1) << n;
        asleep |= ((word >> 1) & 1) << n;
    }
    return uint2(running, asleep);
}
// How many cores there are as the geometry has them now.
uint mc_stat_cores() {
#ifdef MC_HYBRID
    uint nb, rb, rows;
    return 1 + mc_geo_count(ram(0x70003du), nb, rb, rows);
#else
    uint4 geo = ram(0x70003du);
    uint rows = geo.b < 3 || geo.b > 6 ? 0 : mc_rows_of_bits(geo.b);
    if (rows == 0 || (geo.r & 1) != 0) return 1;
    return 1 + min(min(geo.g, (uint)CORES - 1), (248 / rows) * ((CORES + 47) / 49));
#endif
}
#endif

#ifdef GPU_INPUT
// A word of what the host fetched: its texture holds one byte a channel, one word a texel.
// With _FetchDeliver 2 it is a picture the host still has as a texture of its own (_HostImage,
// any size, sRGB, rows bottom up): _FetchW by _FetchH pixels of it, a word each, rows from the
// top, of which this part begins at byte _FetchOffset (a host that hands over the picture's
// own size hands over its pixels as they are).
// A file carried by a picture (docs/system/fetch.md): its bytes are the pixels' red, green and blue,
// rows from the top (the texture's are bottom up). The first 256 are a header: a mark, the
// file's length, its CRC-32, its name.
uint3 packed_pixel(uint p, uint2 size) {
    float4 c = _HostImage.Load(int3(p % size.x, size.y - 1 - min(p / size.x, size.y - 1), 0));
    float3 seen = c.rgb <= 0.0031308 ? c.rgb * 12.92 : 1.055 * pow(abs(c.rgb), 1.0 / 2.4) - 0.055;   // the texture is sRGB
    return (uint3)(saturate(seen) * 255.0 + 0.5);
}
uint packed_word(uint at) {
    uint2 size;
    uint levels;
    _HostImage.GetDimensions(0, size.x, size.y, levels);
    // bytes at to at + 3 are in two pixels: six bytes from the first one's red
    uint p = at / 3, k = at % 3;
    uint3 a = packed_pixel(p, size), b = packed_pixel(p + 1, size);
    uint six[6] = {a.r, a.g, a.b, b.r, b.g, b.b};
    return six[k] | (six[k + 1] << 8) | (six[k + 2] << 16) | (six[k + 3] << 24);
}
bool fetch_packed() {
    return _FetchDeliver == 3 && packed_word(0) == 0x44414853u && packed_word(4) == 0x4d455245u && packed_word(8) == 0x31461a55u;
}

uint fetch_word(uint n) {
    [branch]
    if (fetch_packed()) return packed_word(_FetchOffset + 4 * n);
    [branch]
    if (_FetchDeliver == 2 || _FetchDeliver == 3) {
        n += _FetchOffset / 4;
        if (n >= _FetchW * _FetchH) return 0;
        uint2 size;
        uint levels;
        _HostImage.GetDimensions(0, size.x, size.y, levels);
        uint2 at = uint2(n % _FetchW, _FetchH - 1 - n / _FetchW);
        float4 c = _HostImage.Load(int3(min(at * size / uint2(_FetchW, _FetchH), size - 1), 0));
        float3 seen = c.rgb <= 0.0031308 ? c.rgb * 12.92 : 1.055 * pow(abs(c.rgb), 1.0 / 2.4) - 0.055;
        seen = lerp(1.0, seen, c.a);   // what shows through is the page
        uint3 b = (uint3)(saturate(seen) * 255.0 + 0.5);
        return (b.r << 16) | (b.g << 8) | b.b;
    }
    uint4 b = (uint4)(_HostData.Load(int3(n & 255, n >> 8, 0)) * 255.0 + 0.5);
    return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
#endif

#ifdef GPU_NET
// The network device (docs/system/lan.md). The guest sends one packet at a time through a slot the
// host reads back, and takes packets from a ring this pass fills from the host's texture.
#define NET_GUEST 0x70003eu   // the guest's: packets sent, packets taken from the ring
#define NET_HOST  0x70003fu   // the host's: packets it took, packets delivered, the machine's number
#define NET_RX    0x76b840u   // the ring: NET_SLOTS places of NET_SLOT texels (length, number, 8 bytes, the packet)
#define NET_SLOT  40u
#define NET_SLOTS 8u
// Texel k of the ring's place `slot`, if a packet arrives there in this pass: row e of _NetData
// is the place's 640 bytes, four to a texel in memory order.
uint net_word(uint x, uint e) {
    uint4 b = (uint4)(_NetData.Load(int3(x, e, 0)) * 255.0 + 0.5);
    return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
bool net_arrives(uint slot, uint k, out uint4 texel) {
    texel = 0;
    uint e = (slot - _NetRxSeq) & (NET_SLOTS - 1);
    if (e >= _NetRxCount) return false;
    texel = uint4(net_word(4 * k, e), net_word(4 * k + 1, e), net_word(4 * k + 2, e), net_word(4 * k + 3, e));
    return true;
}
#endif

#ifdef GPU_SOUND
#include "sound.cginc"
#endif

// GPUControl: the machine's control words. Once a submitted list has been drawn, take the
// submit word back and count it; and deliver the host's keyboard and pointer (docs/machine/input.md).
// The counters (programs/mc/mc.h, MC_COUNT_*): drawn when the host has read "added" in the head,
// each total is the sum of that word in all 256 regions (which only grow).
#define COUNT_TOTAL 0x6c1000u
#define COUNT_REGIONS (COUNT_TOTAL + 0x100u)
#define COUNT_HEAD 0x700032u   // { added, COUNT_MERGED, merges, passes so far }: the control pass's
#define COUNT_MERGED 0x4d475245u
uint4 count_total(uint index) {
    uint4 sum = uint4(0, 0, 0, 0);
    [loop] for (uint k = 0; k < 256; k++) sum += ram(COUNT_REGIONS + 0x100u * k + (index - COUNT_TOTAL));
    return sum;
}

uint4 gpu_control(uint2 pos) {
#ifdef GVM_RAM
    uint index = gvm_texel(pos) + 0x600000u;   // (pos: a texel of gpu_vm's RAM texture)
    uint4 keep = ram(index);
#else
    uint4 keep = GPU_STATE[pos];
    if (pos.y < 64) return keep;
    uint index = (pos.y - 64) * 2048 + pos.x;
#endif
    [branch]
    if (index >= COUNT_TOTAL && index < COUNT_REGIONS) return count_total(index);
    // A submitted list has been drawn by now: the GPU is free for the next one. A copy it asked
    // for is noted in GPU_COPY, for the next commit to make.
    // (A list that needs passes the host is not drawing yet waits, submitted, until it is.)
#ifdef GPU_INPUT
    bool served = (SUBMIT_PASSES(ram(GPU_CTRL).r) & ~_GpuPasses) == 0;
#ifdef GVM_RAM
    served = served && _GvmListDrawn != 0;   // (the harness's lazy list: a list not drawn this frame stays submitted)
#endif
#else
    bool served = true;
#endif
    if (index == GPU_CTRL) return (keep.r == 0 || !served) ? keep : uint4(0, keep.g, keep.b, keep.a + 1);
    if (index == GPU_COPY) {
        uint4 ctrl = ram(GPU_CTRL), into = ram(GPU_INTO);
#ifdef GVM_RAM
        // (gpu_vm: the copy noted here was made by the writeback pass before this one: it is cleared here, not by
        // the commit)
        if (!served || (ctrl.r & SUBMIT_DRAW) == 0 || (ctrl.r & SUBMIT_COPIES) == 0) return keep.r != 0 ? (uint4)0 : keep;
#else
        if (!served || (ctrl.r & SUBMIT_DRAW) == 0 || (ctrl.r & SUBMIT_COPIES) == 0) return keep;
#endif
        return uint4(ctrl.r & SUBMIT_COPIES, into.r, (into.g & 0xffff) | (into.b << 16), into.a);
    }
#if defined(CORES) && CORES > 1
    [branch]
    if (index >= MC_STATS && index < MC_STATS + 4) {
        uint first = (index - MC_STATS) * 4;
        return uint4(mc_stat_count(first), mc_stat_count(first + 1), mc_stat_count(first + 2), mc_stat_count(first + 3));
    }
    [branch]
    if (index == MC_STATS + 4) {
        // how many cores there are, which of the first 32 run and sleep, how many there could be
        uint2 bits = mc_stat_bits(0);
        return uint4(mc_stat_cores(), bits.x, bits.y, CORES);
    }
#if CORES > 16
    [branch]
    if (index >= MC_STATS_ALL && index < MC_STATS_ALL + 64) {
        uint first = (index - MC_STATS_ALL) * 4;
        return uint4(mc_stat_count(first), mc_stat_count(first + 1), mc_stat_count(first + 2), mc_stat_count(first + 3));
    }
    [branch]
    if (index >= MC_STATS_ALL + 64 && index < MC_STATS_ALL + 72) {
        uint2 bits = mc_stat_bits((index - (MC_STATS_ALL + 64)) * 32);
        return uint4(bits.x, bits.y, 0, 0);
    }
#endif
#endif
#ifdef GPU_INPUT
    if (index == CON_HOST) return uint4(CON_IN_TAG, CON_IN_CHARS, 0, 0);
#ifdef GVM_RAM
    if (index == GPU_CLOCK) return uint4(keep.r, _HostMs, keep.b + (ram(GPU_COPY).r != 0 ? 1u : 0u), _HostFlags);   // (and a copy made)
#else
    if (index == GPU_CLOCK) return uint4(keep.r, _HostMs, keep.b, _HostFlags);
#endif
    if (index == COUNT_HEAD) {
        // word 3: the commits so far, a pass each (a monitor's measure of what a core can run)
#ifdef GVM_RAM
        uint passes = _GvmPasses;
#else
        uint passes = GPU_STATE[uint2(28, 0)].b;
#endif
#ifdef GPU_COUNT_MERGE
        if (_CountMerged != 0)
            return _CountMerged == 2 ? uint4(0, COUNT_MERGED, keep.b + 1, passes) : uint4(keep.r, COUNT_MERGED, keep.b, passes);
#endif
        return uint4(keep.rgb, passes);
    }
#ifdef HOST_STATE_WORDS
    if (index == HOST_STATE) return uint4(HOST_STATE_WORDS(0), HOST_STATE_WORDS(1), HOST_STATE_WORDS(2), HOST_STATE_WORDS(3));
    if (index == HOST_STATE + 1) return uint4(HOST_STATE_WORDS(4), HOST_STATE_WORDS(5), HOST_STATE_WORDS(6), HOST_STATE_WORDS(7));
#endif
    if (index == INPUT_STATE) {
        uint4 state = uint4(keep.r, keep.g, _InputButtons, _InputKeySeq + _InputKeyCount);
        uint4 disp = ram(GPU_DISPLAY);
        float2 size = float2(disp.g, disp.b), panel = _InputPointer.zw;
        if (panel.x > 16 && panel.y > 16 && disp.g != 0 && disp.b != 0) {
            // the host gives the pointer in window pixels over the display panel; the picture
            // fills it centred, scaled exactly as the display shows it
            float scale = min(panel.x / size.x, panel.y / size.y);
            float2 q = clamp((_InputPointer.xy - (panel - size * scale) * 0.5) / scale, 0, size - 1);
            state.rg = (uint2)q;
        }
        return state;
    }
    if (_FetchDeliver == 4) {
        // the host has something for the guest to open: _HostData's first 64 words are its address
        if (index == HOST_OPEN) return uint4(_FetchSeq, _FetchLength, keep.b, keep.a);
        uint from = index > HOST_OPEN && index < HOST_OPEN + 12 ? (index - HOST_OPEN - 1) * 4
                  : index >= HOST_OPEN_MORE && index < HOST_OPEN_MORE + 5 ? 44 + (index - HOST_OPEN_MORE) * 4 : 0xffffffff;
        if (from != 0xffffffff) return uint4(fetch_word(from), fetch_word(from + 1), fetch_word(from + 2), fetch_word(from + 3));
    } else if (_FetchDeliver != 0) {
        // the host has an answer: its bytes and the words that say so arrive in one pass
        if (index == FETCH_REPLY) {
            // (a file a picture carries says its own length; 201 is "a file with its header")
            if (fetch_packed()) return uint4(_FetchSeq, 256 + packed_word(12), 201, 0);
            return uint4(_FetchSeq, _FetchLength, _FetchStatus, _FetchInfo | _FetchW | (_FetchH << 16));
        }
        if (index >= FETCH_DATA && index < FETCH_DATA + 0x4000) {
            uint n = (index - FETCH_DATA) * 4;
            return uint4(fetch_word(n), fetch_word(n + 1), fetch_word(n + 2), fetch_word(n + 3));
        }
    }
#ifdef GPU_NET
    if (index == NET_HOST) return uint4(_NetTxAck, _NetRxSeq + _NetRxCount, _NetId, 0);
    if (index >= NET_RX && index < NET_RX + NET_SLOT * NET_SLOTS) {
        uint4 packet;
        [branch]
        if (net_arrives((index - NET_RX) / NET_SLOT, (index - NET_RX) % NET_SLOT, packet)) return packet;
        return keep;
    }
#endif
    if (index >= INPUT_KEYS && index < INPUT_KEYS + 8) {
        // up to four new events a frame, at ring positions (sequence number) mod 32
        uint4 ring = keep;
        uint events[4] = {_InputKey0, _InputKey1, _InputKey2, _InputKey3};
        for (uint e = 0; e < 4; e++) {
            uint slot = (_InputKeySeq + e) & 31;
            if (e < _InputKeyCount && (slot >> 2) == index - INPUT_KEYS) {
                // FXC cannot assign through a computed component index
                uint k = slot & 3, v = events[e];
                ring = uint4(k == 0 ? v : ring.r, k == 1 ? v : ring.g, k == 2 ? v : ring.b, k == 3 ? v : ring.a);
            }
        }
        return ring;
    }
#endif
#ifdef GPU_SOUND
    return sound_control(index, keep);
#else
    return keep;
#endif
}

#endif
