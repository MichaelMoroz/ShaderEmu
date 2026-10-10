#ifndef GPU_H
#define GPU_H

// The GPU device. See docs/gpu.md for the guest's view; programs/common/gpu.h has the same
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

// How fragments are coloured (low byte), plus flags.
#define FRAGMENT_COLOUR 0     // interpolated colour
#define FRAGMENT_TEXTURE 1    // 0x00RRGGBB texture times colour
#define FRAGMENT_INDEXED 2    // 8-bit texture through the display palette, times colour
#define FRAGMENT_MASK 3       // 1-bit texture, rows of whole bytes, leftmost bit highest: set bits take the colour
#define FRAGMENT_RGB24 4      // texture of three bytes a pixel (red, green, blue), rows with no padding, at any byte address, in RAM or ROM; times colour
#define FRAGMENT_KEYED 0x100  // flag: texels equal to the key colour (or index) are not drawn
#define FRAGMENT_LAID 0x200   // flag (draws): the texture lies on the picture, not on the surface (a sky)
#define FRAGMENT_SMOOTH 0x400 // flag (word and indexed textures): the four texels round the point, weighed (bilinear)
// Bits 16-18 of the same word: the pass the command is drawn in. Passes are drawn in order, each
// with fixed blending and depth use; within a pass commands keep the list's order.
//   0 opaque   1 alpha blend   2 additive   3 multiply     depth tested; only pass 0 writes it
//   4 opaque   5 alpha blend   6 additive   7 multiply     no depth
#define FRAGMENT_PASS(mode) (((mode) >> 16) & 7)

uint4 ram(uint index) {
    return GPU_STATE[uint2(index % 2048, 64 + index / 2048)];
}
uint texel_of(uint address) {
    return (address & 0x7fffffff) >> 4;
}
uint ram_word(uint address) {
    uint4 t = ram(texel_of(address));
    uint k = (address >> 2) & 3;
    return k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a;
}
// A word of a texture: RAM, or with GPU_ROM the ROM (addresses from 0x40000000), read as the
// CPU reads it: four textures, one for each word of a 16-byte texel, rows bottom up.
#ifdef GPU_ROM
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
#else
#define texture_word ram_word
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
        uint p = ram_word(head.g + 4 * k);
        t = uint4(p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff, (ram_word(ram(base + 3).r + 4 * k) & 0xffffff) | (p & 0xff000000));
        pos = float4(float3(t.xyz), 1.0);
    } else {
        t = ram(texel_of(head.g) + k);
        pos = float4(gpu_number((how.r & VERTEX_FLOAT) != 0, t).xyz, 1.0);
    }
    gpu_compact(how.r, t.a, more.a, uv, colour);
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

// Mesh vertex `id`, which the command at `base` claims (gpu_command), placed and coloured.
gpu_varyings gpu_vertex_of(uint id, uint base, uint4 head) {
    gpu_varyings o;
    o.position = float4(2, 2, 2, 1);
    o.colour = 0;
    o.uv = 0;
    o.texture_info = 0;
    o.key = 0;
    uint4 ctrl = ram(GPU_CTRL);
    uint4 disp = ram(GPU_DISPLAY);
    float2 size = float2(disp.g, disp.b);
    if (ctrl.r & SUBMIT_INTO) {
        uint4 into = ram(GPU_INTO);
        size = float2(into.g, into.b);
    }
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
            // starts along the line. The table has a texel a glyph: bitmap, width, height.
            uint4 at = ram(base + 1);
            uint part = k % 6, corner = part < 3 ? part : (part == 3 ? 2 : (part == 4 ? 1 : 3));
            uint letter = ram_word(at.a + 4 * (k / 6));
            uint4 glyph = ram(texel_of(head.b) + (letter & 0xffff));
            bool right = (corner & 1) != 0, low = (corner & 2) != 0;
            o.position = place_pixels(float2(asint(at.r) + (int)(letter >> 16) + (right ? (int)glyph.g : 0),
                                             asint(at.g) + (low ? (int)glyph.b : 0)), 0.0);
            o.colour = colour_of(head.g);
            o.uv = float2(right ? 1.0 : 0.0, low ? 1.0 : 0.0);
            o.texture_info = uint4(FRAGMENT_MASK | (ram(base + 2).r & 0x70000), glyph.r, glyph.g, glyph.b);
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

// The mesh's vertex `id` belongs to whichever command claims that slot. Vertices no command
// claims, and everything while no list is submitted, collapse to nothing.
gpu_varyings gpu_vertex(uint id) {
    gpu_varyings o;
    o.position = float4(2, 2, 2, 1);   // outside the clip volume
    o.colour = 0;
    o.uv = 0;
    o.texture_info = 0;
    o.key = 0;
    uint base;
    uint4 head;
    [branch]
    if (gpu_command(id, 1, base, head)) o = gpu_vertex_of(id, base, head);
    return o;
}

// A texel of an indexed or a word texture as a colour, and whether it is drawn at all (the key
// colour is not, when the draw is keyed).
float4 gpu_texel(gpu_varyings i, uint mode, uint x, uint y, uint width, out float there) {
    uint n = y * width + x, texel;
    if (mode == FRAGMENT_INDEXED) {
        texel = (ram_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
        there = (i.texture_info.r & FRAGMENT_KEYED) && texel == i.key ? 0.0 : 1.0;
        texel = ram_word(0x87000400 + 4 * texel);
    } else {
        texel = ram_word(i.texture_info.g + 4 * n);
        there = (i.texture_info.r & FRAGMENT_KEYED) && texel == i.key ? 0.0 : 1.0;
    }
    return colour_of(texel);
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
    if ((i.texture_info.r & FRAGMENT_SMOOTH) && (mode == FRAGMENT_INDEXED || mode == FRAGMENT_TEXTURE) && width != 0 && height != 0) {
        // the four texels round the point, repeating, each by how near its centre is. A texel
        // of the key colour counts for nothing; where those are most of the four, nothing is drawn.
        float2 at = frac(uv) * float2(width, height) - 0.5;
        float2 low = floor(at), part = at - low;
        uint x0 = (uint)((int)low.x + (int)width) % width, y0 = (uint)((int)low.y + (int)height) % height;
        uint x1 = (x0 + 1) % width, y1 = (y0 + 1) % height;
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
        uint x = min((uint)(frac(uv.x) * width), width - 1), y = min((uint)(frac(uv.y) * height), height - 1);
        uint n = y * width + x, texel;
        if (mode == FRAGMENT_MASK) {
            n = y * ((width + 7) >> 3) + (x >> 3);
            texel = (ram_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
            if (((texel << (x & 7)) & 0x80) == 0) discard;
            return saturate(c);
        }
        if (mode == FRAGMENT_INDEXED) {
            texel = (ram_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
            texel = ram_word(0x87000400 + 4 * texel);
        } else if (mode == FRAGMENT_RGB24) {
            // the texture may start at any byte, and three bytes may run into the next word
            uint first = i.texture_info.g + 3 * n, at = first & ~3u, shift = 8 * (first & 3);
            texel = texture_word(at) >> shift;
            if (shift > 8) texel |= texture_word(at + 4) << (32 - shift);
            texel = (texel & 0xff) << 16 | (texel & 0xff00) | ((texel >> 16) & 0xff);
        } else {
            texel = ram_word(i.texture_info.g + 4 * n);
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
        }
        c *= colour_of(texel);
    }
    return saturate(c);
}

#ifdef GPU_VOLUME
// The volume display (docs/volume.md): a host that shows a 3D program's last whole frame from
// a point of view of its own, instead of the picture the GPU drew of it.
#define VOLUME_LIST 0x700030u   // that frame's list address, command count, frames so far

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
// The 4 MB bands of RAM the pending copy writes, one bit each.
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
    uint lo = (first >> 22) & 31, hi = min((first + bytes) >> 22, 31u);
    return hi < lo ? 0 : ((2u << hi) - 1) & ~((1u << lo) - 1);
}

// For the Commit pass. The control pass leaves the copy a drawn list asked for in GPU_COPY; the
// next commit makes it: the RAM texels it covers take the picture (one 0x00RRGGBB word per
// pixel) and GPU_COPY is cleared. False for every other texel.
bool gpu_writeback(uint2 pos, out uint4 result) {
    result = 0;
    if (pos.y < 64) return false;
    uint index = (pos.y - 64) * 2048 + pos.x;
    if (index < 0x600000u) return false;   // below anything a GPU program may own
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
#define FETCH_REPLY 0x700011u   // requests the host has answered, bytes, status (docs/fetch.md)
#define FETCH_DATA  0x76c000u   // what the host fetched: 0x4000 texels
// What the cores did, for a guest to show (docs/multicore.md): 16 words, core k's count of
// instructions in word k; then a texel of how many cores, which run and which are asleep (a bit each).
#define MC_STATS    0x700038u

#if defined(CORES) && CORES > 1
// Where worker `core`'s tiles begin in the strip (the geometry: src/types.h has the same), or
// 0xffff when there is no such worker; core 0's state is at the left.
uint mc_rows_of_bits(uint bits) {
    return bits == 0 ? 0 : (44 + 8 + (2u << bits) * 4 + 63) / 64;
}
uint2 mc_place(uint core) {
    if (core == 0) return uint2(0, 0);
    uint4 geo = ram(0x70003du);
#if CORES > 16
    uint4 geo1 = ram(0x6c00f0u), geo2 = ram(0x6c00f1u);   // MC_GEOMETRY_MORE
#endif
    uint row = 0, at = 0xffff;
    for (uint k = 1; k < CORES; k++) {
        uint j = k - 1;
#if CORES > 16
        uint word = j < 8 ? geo.g : j < 16 ? geo.b : j < 24 ? geo.a : j < 32 ? geo1.r : j < 40 ? geo1.g : j < 48 ? geo1.b : j < 56 ? geo1.a : geo2.r;
        uint bits = (word >> (4 * (j & 7))) & 15;
#else
        uint bits = ((j < 8 ? geo.g >> (4 * j) : geo.b >> (4 * (j - 8))) & 15);
#endif
        if (k == core && bits != 0 && (geo.r & 1) == 0) at = row;
        row += mc_rows_of_bits(bits);
    }
    return uint2(64, at);
}
// Texel w (below 64) of a core's state: a worker's is in the first of its tiles, eight to a row.
uint4 mc_state_texel(uint2 at, uint w) {
    if (at.x == 0) return GPU_STATE[uint2(w, 0)];
    return GPU_STATE[uint2(64 + (at.y << 3) + (w & 7), w >> 3)];
}
uint mc_stat_count(uint core) {
    uint2 at = mc_place(core);
    return core < CORES && at.y != 0xffff ? mc_state_texel(at, 28).g : 0;
}
#endif

#ifdef GPU_INPUT
// A word of what the host fetched: its texture holds one byte a channel, one word a texel.
// With _FetchDeliver 2 it is a picture the host still has as a texture of its own (_HostImage,
// any size, sRGB, rows bottom up): word n is pixel n of it scaled to _FetchW by _FetchH.
uint fetch_word(uint n) {
    [branch]
    if (_FetchDeliver == 2) {
        if (n >= _FetchW * _FetchH) return 0;
        uint2 size;
        uint levels;
        _HostImage.GetDimensions(0, size.x, size.y, levels);
        // from the level whose texels are about the size of ours
        uint level = min((uint)max(log2((float)size.x / _FetchW), 0.0), levels - 1);
        uint2 at = uint2(n % _FetchW, _FetchH - 1 - n / _FetchW), small = max(size >> level, 1u);
        float4 c = _HostImage.Load(int3(min(at * small / uint2(_FetchW, _FetchH), small - 1), level));
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
// The network device (docs/lan.md). The guest sends one packet at a time through a slot the
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
#include "sound.h"
#endif

// GPUControl: the machine's control words. Once a submitted list has been drawn, take the
// submit word back and count it; and deliver the host's keyboard and pointer (docs/input.md).
uint4 gpu_control(uint2 pos) {
    uint4 keep = GPU_STATE[pos];
    if (pos.y < 64) return keep;
    uint index = (pos.y - 64) * 2048 + pos.x;
    // A submitted list has been drawn by now: the GPU is free for the next one. A copy it asked
    // for is noted in GPU_COPY, for the next commit to make.
    // (A list that needs passes the host is not drawing yet waits, submitted, until it is.)
#ifdef GPU_INPUT
    bool served = (SUBMIT_PASSES(ram(GPU_CTRL).r) & ~_GpuPasses) == 0;
#else
    bool served = true;
#endif
    if (index == GPU_CTRL) return (keep.r == 0 || !served) ? keep : uint4(0, keep.g, keep.b, keep.a + 1);
    if (index == GPU_COPY) {
        uint4 ctrl = ram(GPU_CTRL), into = ram(GPU_INTO);
        if (!served || (ctrl.r & SUBMIT_DRAW) == 0 || (ctrl.r & SUBMIT_COPIES) == 0) return keep;
        return uint4(ctrl.r & SUBMIT_COPIES, into.r, (into.g & 0xffff) | (into.b << 16), into.a);
    }
#if defined(CORES) && CORES > 1
    if (index >= MC_STATS && index < MC_STATS + 4) {
        uint first = (index - MC_STATS) * 4;
        return uint4(mc_stat_count(first), mc_stat_count(first + 1), mc_stat_count(first + 2), mc_stat_count(first + 3));
    }
    if (index == MC_STATS + 4) {
        // a worker's state word (41,0).a: bit 0 while it runs, bit 1 while it sleeps on its job word
        // how many cores there are as the geometry has them now, and how many there could be
        uint running = 1, asleep = 0, count = 1;
        for (uint core = 1; core < CORES; core++) {
            uint2 at = mc_place(core);
            if (at.y == 0xffff) continue;
            uint word = mc_state_texel(at, 41).a;
            if (core < 32) {   // (a bit each for the first 32)
                running |= (word & 1) << core;
                asleep |= ((word >> 1) & 1) << core;
            }
            count = core + 1;
        }
        return uint4(count, running, asleep, CORES);
    }
#endif
#ifdef GPU_INPUT
    if (index == GPU_CLOCK) return uint4(keep.r, _HostMs, keep.b, _HostFlags);
    if (index == INPUT_STATE) {
        uint4 state = uint4(keep.r, keep.g, _InputButtons, _InputKeySeq + _InputKeyCount);
        uint4 disp = ram(GPU_DISPLAY);
        float2 size = float2(disp.g, disp.b), panel = _InputPointer.zw;
        if (panel.x > 16 && panel.y > 16 && disp.g != 0 && disp.b != 0) {
            // the host gives the pointer in window pixels over the display panel; the picture
            // sits in it centred and scaled exactly as the display shows it
            float scale = min((panel.x - 16) / size.x, (panel.y - 16) / size.y);
            if (scale >= 1) scale = floor(scale);
            float2 q = clamp((_InputPointer.xy - (panel - size * scale) * 0.5) / scale, 0, size - 1);
            state.rg = (uint2)q;
        }
        return state;
    }
    if (_FetchDeliver != 0) {
        // the host has an answer: its bytes and the words that say so arrive in one pass
        if (index == FETCH_REPLY) return uint4(_FetchSeq, _FetchLength, _FetchStatus, _FetchInfo | _FetchW | (_FetchH << 16));
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
