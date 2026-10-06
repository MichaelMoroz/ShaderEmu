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
#define GPU_CLOCK   0x700003u   // word 1: the host's clock in milliseconds; word 2: copies made so far

// Submit word: what to do with the list.
#define SUBMIT_DRAW 1        // draw it
#define SUBMIT_WRITEBACK 2   // afterwards copy the picture into the RAM framebuffer
#define SUBMIT_INTO 4        // afterwards copy it into the rectangle of RAM at GPU_INTO, whose size the picture has
#define SUBMIT_COPIES (SUBMIT_WRITEBACK | SUBMIT_INTO)
// Bits 9-15: the list has commands for passes 1-7. It is drawn, and the submit word taken back,
// once the host draws all of those (it learns of them by reading these words back).
#define SUBMIT_PASSES(submit) (((submit) >> 8) & 0xfe)

#define GPU_TARGET 2048.0       // the GPU's render target is GPU_TARGET pixels square
#define GPU_MAX_COMMANDS 4096

#define CMD_END 0
#define CMD_CLEAR 1     // 3 vertices
#define CMD_RECT 2      // 6 vertices
#define CMD_DRAW 3      // `count` vertices

// How a draw's vertices are projected.
#define VERTEX_SCREEN 0   // position is already in pixels (x, y) and depth (z, 0..1)
#define VERTEX_CLIP 1     // uniforms c0-c3 are a clip matrix; colour is the vertex colour
#define VERTEX_LIT 2      // also c4-c6 normal matrix, c7 light direction, c8 diffuse, c9 ambient
#define VERTEX_MODELVIEW 0x100   // flag: c0-c3 is the projection alone and c4-c6 the modelview's rows
#define VERTEX_QUADS 0x200       // flag: the buffer holds four corners per quad, in order round it, for every six vertices drawn
#define VERTEX_COMPACT 0x400     // flag: a vertex is one texel: x, y, z, then u | v << 16 in 1/1024ths; the colour is the command's word 11

// How fragments are coloured (low byte), plus flags.
#define FRAGMENT_COLOUR 0     // interpolated colour
#define FRAGMENT_TEXTURE 1    // 0x00RRGGBB texture times colour
#define FRAGMENT_INDEXED 2    // 8-bit texture through the display palette, times colour
#define FRAGMENT_MASK 3       // 1-bit texture, rows of whole bytes, leftmost bit highest: set bits take the colour
#define FRAGMENT_RGB24 4      // texture of three bytes a pixel (red, green, blue), rows with no padding, times colour
#define FRAGMENT_KEYED 0x100  // flag: texels equal to the key colour (or index) are not drawn
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
// Guest numbers are 16.16 fixed point.
float4 from_fixed(uint4 t) {
    return float4(asint(t)) / 65536.0;
}
// A colour word is 0xTTRRGGBB: T is transparency, so that the usual 0x00RRGGBB is opaque.
float4 colour_of(uint v) {
    return float4((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 255 - (v >> 24)) / 255.0;
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
    uint list = texel_of(ctrl.g), count = min(ctrl.b, GPU_MAX_COMMANDS);
    if ((ctrl.r & SUBMIT_DRAW) == 0 || count == 0) return o;
#ifdef GPU_PASS_UNIFORMS
    if ((SUBMIT_PASSES(ctrl.r) & ~_GpuPasses) != 0) return o;   // not until every pass it needs is drawn
#endif
    uint4 disp = ram(GPU_DISPLAY);
    float2 size = float2(disp.g, disp.b);
    if (ctrl.r & SUBMIT_INTO) {
        uint4 into = ram(GPU_INTO);
        size = float2(into.g, into.b);
    }
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
    {
        uint base = list + 4 * lo;
        uint4 head = ram(base);   // op, a, b, first slot
        uint op = head.r;
        uint slots = op == CMD_CLEAR ? 3 : op == CMD_RECT ? 6 : op == CMD_DRAW ? head.b : 0;
        if (id < head.a || id >= head.a + slots) return o;
        uint k = id - head.a;
#ifdef GPU_PASS_UNIFORMS
        uint drawn_in = op == CMD_RECT ? FRAGMENT_PASS(ram(base + 2).r) : op == CMD_DRAW ? FRAGMENT_PASS(ram(base + 1).g) : 0;
        if (drawn_in != _GpuPass) return o;
#endif
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
            uint u = texel_of(how.b);
            if (how.r & VERTEX_QUADS) {
                // triangles 0 1 2 and 0 2 3 of each quad's corners
                uint part = k % 6;
                k = (k / 6) * 4 + (part < 3 ? part : part == 3 ? 0 : part - 2);
            }
            float4 pos, normal = 0;
            [branch]
            if (how.r & VERTEX_COMPACT) {
                uint4 t = ram(texel_of(head.g) + k);
                pos = float4(from_fixed(t).xyz, 1.0);
                o.uv = float2(asint(t.a << 16) >> 16, asint(t.a) >> 16) / 1024.0;
                o.colour = colour_of(more.a);
            } else {
                uint v = texel_of(head.g) + 4 * k;
                pos = from_fixed(ram(v));
                normal = from_fixed(ram(v + 1));
                o.uv = from_fixed(ram(v + 2)).xy;
                o.colour = from_fixed(ram(v + 3));
            }
            o.texture_info = uint4(how.g, how.a, more.r, more.g);
            o.key = more.b;
            [branch]
            uint vertex_mode = how.r & 0xff;
            if (vertex_mode == VERTEX_SCREEN) {
                o.position = place_pixels(pos.xy, pos.z);
            } else {
                // with VERTEX_MODELVIEW the GPU does the matrix product the program would have done
                if (how.r & VERTEX_MODELVIEW)
                    pos = float4(dot(from_fixed(ram(u + 4)), pos), dot(from_fixed(ram(u + 5)), pos),
                                 dot(from_fixed(ram(u + 6)), pos), pos.w);
                float4 clip = float4(dot(from_fixed(ram(u)), pos), dot(from_fixed(ram(u + 1)), pos),
                                     dot(from_fixed(ram(u + 2)), pos), dot(from_fixed(ram(u + 3)), pos));
                // The picture is the top-left size.x by size.y pixels of the target; clip z
                // arrives as -w..w (the OpenGL convention) and leaves as 0..w.
                float2 part = size / GPU_TARGET;
                o.position = float4((clip.x + clip.w) * part.x - clip.w, clip.w - (clip.w - clip.y) * part.y,
                                    (clip.z + clip.w) * 0.5, clip.w);
                if (vertex_mode == VERTEX_LIT) {
                    float3 n = float3(dot(from_fixed(ram(u + 4)).xyz, normal.xyz), dot(from_fixed(ram(u + 5)).xyz, normal.xyz),
                                      dot(from_fixed(ram(u + 6)).xyz, normal.xyz));
                    float facing = max(dot(n, from_fixed(ram(u + 7)).xyz), 0.0);
                    o.colour = float4(facing * from_fixed(ram(u + 8)).rgb + from_fixed(ram(u + 9)).rgb, 1.0);
                }
            }
        }
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
            // the three bytes may run into the next word
            uint at = i.texture_info.g + ((3 * n) & ~3u), shift = 8 * ((3 * n) & 3);
            texel = ram_word(at) >> shift;
            if (shift > 8) texel |= ram_word(at + 4) << (32 - shift);
            texel = (texel & 0xff) << 16 | (texel & 0xff00) | ((texel >> 16) & 0xff);
        } else {
            texel = ram_word(i.texture_info.g + 4 * n);
            if ((i.texture_info.r & FRAGMENT_KEYED) && texel == i.key) discard;
        }
        c *= colour_of(texel);
    }
    return saturate(c);
}

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
#ifdef GPU_INPUT
    if (index == GPU_CLOCK) return uint4(keep.r, _HostMs, keep.b, keep.a);
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
    return keep;
}

#endif
