#ifndef GPU_H
#define GPU_H

// The GPU device. See docs/gpu.md for the guest's view; programs/common/gpu.h has the same
// numbers for C. GPU_STATE is the machine's state texture (RAM from row 64 on).
//
// Addresses below are RAM texel indices: (physical address - 0x80000000) / 16.
#define GPU_DISPLAY 0x700000u   // display control: mode, width, height
#define GPU_CTRL    0x700001u   // submit, command list address, command count, frames done

#define GPU_TARGET 2048.0       // the GPU's render target is GPU_TARGET pixels square
#define GPU_MAX_COMMANDS 256

#define CMD_END 0
#define CMD_CLEAR 1     // 3 vertices
#define CMD_RECT 2      // 6 vertices
#define CMD_DRAW 3      // `count` vertices

// How a draw's vertices are projected.
#define VERTEX_SCREEN 0   // position is already in pixels (x, y) and depth (z, 0..1)
#define VERTEX_CLIP 1     // uniforms c0-c3 are a clip matrix; colour is the vertex colour
#define VERTEX_LIT 2      // also c4-c6 normal matrix, c7 light direction, c8 diffuse, c9 ambient

// How fragments are coloured (low byte), plus flags.
#define FRAGMENT_COLOUR 0     // interpolated colour
#define FRAGMENT_TEXTURE 1    // 0x00RRGGBB texture times colour
#define FRAGMENT_INDEXED 2    // 8-bit texture through the display palette, times colour
#define FRAGMENT_KEYED 0x100  // flag: texels equal to the key colour (or index) are not drawn

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
// Guest numbers are 16.16 fixed point.
float4 from_fixed(uint4 t) {
    return float4(asint(t)) / 65536.0;
}
float4 colour_of(uint v) {
    return float4((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 255) / 255.0;
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
    return float4(p.x * 2.0 / GPU_TARGET - 1.0, 1.0 - p.y * 2.0 / GPU_TARGET, depth, 1.0);
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
    uint4 ctrl = ram(GPU_CTRL);
    if (ctrl.r == 0) return o;
    uint4 disp = ram(GPU_DISPLAY);
    float2 size = float2(disp.g, disp.b);
    uint list = texel_of(ctrl.g), count = min(ctrl.b, GPU_MAX_COMMANDS);
    [loop]
    for (uint c = 0; c < count; c++) {
        uint base = list + 4 * c;
        uint4 head = ram(base);   // op, a, b, first slot
        uint op = head.r;
        if (op == CMD_END) break;
        uint slots = op == CMD_CLEAR ? 3 : op == CMD_RECT ? 6 : op == CMD_DRAW ? head.b : 0;
        if (id < head.a || id >= head.a + slots) continue;
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
        } else {
            // head.g vertex buffer, head.b count; then vertex mode, fragment mode, uniforms,
            // texture address; width, height, key
            uint4 how = ram(base + 1), more = ram(base + 2);
            uint v = texel_of(head.g) + 4 * k, u = texel_of(how.b);
            float4 pos = from_fixed(ram(v)), normal = from_fixed(ram(v + 1));
            o.uv = from_fixed(ram(v + 2)).xy;
            o.colour = from_fixed(ram(v + 3));
            o.texture_info = uint4(how.g, how.a, more.r, more.g);
            o.key = more.b;
            [branch]
            if (how.r == VERTEX_SCREEN) {
                o.position = place_pixels(pos.xy, pos.z);
            } else {
                float4 clip = float4(dot(from_fixed(ram(u)), pos), dot(from_fixed(ram(u + 1)), pos),
                                     dot(from_fixed(ram(u + 2)), pos), dot(from_fixed(ram(u + 3)), pos));
                // The picture is the top-left size.x by size.y pixels of the target; clip z
                // arrives as -w..w (the OpenGL convention) and leaves as 0..w.
                float2 part = size / GPU_TARGET;
                o.position = float4((clip.x + clip.w) * part.x - clip.w, clip.w - (clip.w - clip.y) * part.y,
                                    (clip.z + clip.w) * 0.5, clip.w);
                if (how.r == VERTEX_LIT) {
                    float3 n = float3(dot(from_fixed(ram(u + 4)).xyz, normal.xyz), dot(from_fixed(ram(u + 5)).xyz, normal.xyz),
                                      dot(from_fixed(ram(u + 6)).xyz, normal.xyz));
                    float facing = max(dot(n, from_fixed(ram(u + 7)).xyz), 0.0);
                    o.colour = float4(facing * from_fixed(ram(u + 8)).rgb + from_fixed(ram(u + 9)).rgb, 1.0);
                }
            }
        }
        break;
    }
    return o;
}

float4 gpu_fragment(gpu_varyings i) {
    uint mode = i.texture_info.r & 0xff, width = i.texture_info.b, height = i.texture_info.a;
    float4 c = i.colour;
    [branch]
    if (mode != FRAGMENT_COLOUR && width != 0 && height != 0) {
        // nearest texel, repeating
        uint x = min((uint)(frac(i.uv.x) * width), width - 1), y = min((uint)(frac(i.uv.y) * height), height - 1);
        uint n = y * width + x, texel;
        if (mode == FRAGMENT_INDEXED) {
            texel = (ram_word(i.texture_info.g + (n & ~3u)) >> (8 * (n & 3))) & 0xff;
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
            texel = ram_word(0x87000400 + 4 * texel);
        } else {
            texel = ram_word(i.texture_info.g + 4 * n);
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
        }
        c *= colour_of(texel);
    }
    return float4(saturate(c.rgb), 1.0);
}

// GPUControl: once a submitted list has been drawn, take the submit word back and count it.
uint4 gpu_control(uint2 pos) {
    uint4 keep = GPU_STATE[pos];
    if (pos.y < 64 || (pos.y - 64) * 2048 + pos.x != GPU_CTRL || keep.r == 0) return keep;
    return uint4(0, keep.g, keep.b, keep.a + 1);
}

#endif
