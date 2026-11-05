#ifndef GPU_H
#define GPU_H
#include "layout.h"

// The GPU device. See docs/machine/gpu.md for the guest's view; programs/common/gpu.h has the same
// numbers for C. GPU_STATE is the machine's state texture (RAM from row 64 on).
//
// Addresses below are RAM texel indices: (physical address - 0x80000000) / 16; a device's are
// DEV_T of its offset from the devices' first texel (layout.h).
#define GPU_DISPLAY DEV_T(0x100000u)   // display control: mode, width, height
#define GPU_CTRL    DEV_T(0x100001u)   // submit, command list address, command count, frames done
#define GPU_PIXELS  DEV_T(0x100100u)   // the display's framebuffer in RAM, for writing the picture back
#define GPU_INTO    DEV_T(0x100005u)   // where SUBMIT_INTO writes the picture: address, width, height, row length
#define GPU_COPY    DEV_T(0x100007u)   // the copy still to do for the list just drawn: how (0: none), address, width | height << 16, row length
#define GPU_CLOCK   DEV_T(0x100003u)   // word 1: the host's clock in milliseconds; word 2: copies made so far; word 3: host flags

// Submit word: what to do with the list.
#define SUBMIT_DRAW 1        // draw it
#define SUBMIT_WRITEBACK 2   // afterwards copy the picture into the RAM framebuffer
#define SUBMIT_INTO 4        // afterwards copy it into the rectangle of RAM at GPU_INTO, whose size the picture has
#define SUBMIT_COPIES (SUBMIT_WRITEBACK | SUBMIT_INTO)
// The host draws a list's CMD_UPLOADs into the VRAM before its passes, every frame a list is drawn (GPUUpload). The VRAM's mip
// levels it makes anew only when asked: SUBMIT_UPLOADS, the list has an upload of a texture with levels (VRAM_LOD not 0).
#define SUBMIT_UPLOADS 8
// _GpuPasses' bit 16: the host made the levels this frame. A list with SUBMIT_UPLOADS waits, submitted and not drawn, until it
// has (as one waits for a pass the host does not draw yet), so its draws find their textures' levels; one whose uploads are
// all of textures without levels (a light map changed) waits for nothing.
#define GPU_UPLOADED 0x10000u
// The picture is GPU_INTO's size (its width and height words) though nothing is copied into RAM: a framebuffer's list drawn
// for its copies into the VRAM alone (gles.c's glBindFramebufferOES), placed as the window's would be.
#define SUBMIT_SIZED 0x1000000u
#define SUBMIT_WAITS(submit, passes) ((SUBMIT_PASSES(submit) & ~(passes)) != 0 || (((submit) & SUBMIT_UPLOADS) != 0 && ((passes) & GPU_UPLOADED) == 0))
// Bits 9-23: the list has commands for passes 1-15. It is drawn, and the submit word taken back,
// once the host draws all of those (it learns of them by reading these words back).
#define SUBMIT_PASSES(submit) (((submit) >> 8) & 0xfffe)

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
#define CMD_SPRITES 5   // 6 vertices a sprite record (docs/machine/gpu.md, "Sprites of a paletted picture")
#define CMD_UPLOAD 6    // 6 vertices: a texture copied into the VRAM with its mip levels (docs/machine/gpu.md, "VRAM")
#define CMD_COPY 7      // 6 vertices: a rectangle of the picture copied into a slot of the VRAM after the list's passes ("Copies")
#define COPY_TURNED 1u  // (CMD_COPY's flags) the rows turned over: the picture's top row is the slot's last (OpenGL's order)

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
#define VERTEX_BONES 0x40000     // flag (floats, not compact): a skinned model's vertex, made from its indices, vertices, bones and light (words 1, 12-15; gpu.md)
#define VERTEX_ENV 0x80000       // flag (compact, floats): u and v made from a normal a vertex (word 12, a texel each) and the eye (words 13-15), as rd-vanilla's environment map
#define VERTEX_ARRAYS 0x100000   // flag (floats): a vertex from a renderer's arrays by its index, made as a compact vertex of them would be (words 1, 11, 12-15; gpu_arrays_vertex)

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
#define FRAGMENT_SPRITE 9     // (sprites) a record's pixel as an index of a paletted picture: the kinds below
#define TEXT_MORE 0x100       // flag (text): words 9-11 are a scale, a slant and what a glyph's bitmap address is from
#define FRAGMENT_KEYED 0x100  // flag: texels equal to the key colour (or index) are not drawn
#define FRAGMENT_LAID 0x200   // flag (draws): the texture lies on the picture, not on the surface (a sky)
#define FRAGMENT_SMOOTH 0x400 // flag (word, indexed and three-byte textures): the four texels round the point, weighed (bilinear)
#define FRAGMENT_CLAMPED 0x1000 // flag: the texture does not repeat: beyond its edge the edge goes on
#define FRAGMENT_LIT 0x800    // flag (compact draws): times a light map, a three-byte texture, at a second pair of coordinates a vertex (words 12-14)
// Bits 16-19 of the same word: the pass the command is drawn in. Passes are drawn in order, each
// with fixed blending and depth use; within a pass commands keep the list's order.
//   0 opaque   1 alpha blend   2 additive   3 multiply     depth tested; only pass 0 writes it
//   8 the colour times (1 - what is there's alpha), added   9 the colour times what is there, plus what is
//     there times (1 - its alpha)                            depth tested (drawn after 3, before 4)
//   4 opaque   5 alpha blend   6 additive   7 multiply     no depth
//   12, 13 as 8 and 9                                       no depth (drawn after 7)
// The picture's alpha is kept as GL keeps it (each blending's own factors on alpha), for 8, 9, 12 and 13:
// GL_ONE_MINUS_DST_ALPHA GL_ONE (Jedi Outcast's shine over its logo), GL_DST_COLOR GL_ONE_MINUS_DST_ALPHA
// (Quake III's glints masked by an earlier stage's alpha). 10, 11, 14 and 15 are not passes yet.
#define FRAGMENT_PASS(mode) (((mode) >> 16) & 15)
#define PASS_BLENDING(pass) (((pass) & 3) | (((pass) >> 1) & 4))   // 0-3 as above, 4 and 5 those of 8 and 9
#define PASS_DEPTH(pass) (((pass) & 4) == 0)
// The blending a pass has in the world's volume and HUD layer (docs/world/holodeck.md), drawn over the world's picture,
// whose alpha is no game's: 9 and 13 there multiply (Quake III's light maps), 8 and 12 (a shine) are not drawn.
#define VOLUME_BLENDING(pass) (PASS_BLENDING(pass) == 5 ? 3u : PASS_BLENDING(pass))
// A sprite record's kind (CMD_SPRITES, bits 22-25 of its fourth word). A sprite's byte 0 is a hole but in RAW.
#define SPRITE_COPY 0         // the byte
#define SPRITE_REMAP 1        // the byte through a table of 256 bytes (word 1); a 0 there is a hole
#define SPRITE_BLACK 2        // index 0 where the sprite has a pixel
#define SPRITE_SHADE 3        // where the sprite has a pixel, word 1 added to the picture's green (pass 2): a remap of what is there
#define SPRITE_FILL 4         // word 0's low byte over the whole rectangle
#define SPRITE_FILL_SHADE 5   // word 1 added to green over the whole rectangle (pass 2)
#define SPRITE_CHECKER 6      // word 0's low byte where x + y is odd (bit 8 of word 0 set) or even
#define SPRITE_WORDS 7        // a picture these commands drew, a word a pixel: red the index, green its remaps (the command's table)
#define SPRITE_RAW 8          // the byte, 0 too
#define SPRITE_SHADES(kind) ((kind) == SPRITE_SHADE || (kind) == SPRITE_FILL_SHADE)

// The VRAM (docs/machine/gpu.md, "VRAM"): a texture of VRAM_SIDE x VRAM_SIDE colours, RGBA8, with mip levels the host makes
// of all of it after the uploads (GenerateMips), into which CMD_UPLOAD copies textures from RAM or the ROM. A texture lies in
// a block padded with its own texels as it repeats (or with VRAM_CLAMPED its edge's), 2^L of them each side, the block's
// corner and size whole multiples of 2^L: so its levels up to L are its own, and a sample of a level up to L weighs only its
// texels. A draw's texture address (or a light map's) with its top two bits clear is not an address but such a texture:
#define VRAM_SIDE 4096
#define VRAM_X(slot) ((slot) & 0xfffu)               // its texel (0, 0), past the padding
#define VRAM_Y(slot) (((slot) >> 12) & 0xfffu)
#define VRAM_LOD(slot) (((slot) >> 24) & 7u)         // L, the most a sample is minified (2^L); its padding is 2^L (1 for L = 0)
#define VRAM_PREMULTIPLIED 0x08000000u               // uploaded with a key: colours times alpha, the key's texels 0
#define VRAM_CLAMPED 0x10000000u                     // padded with the edge's texels (a light map), not the other side's
#define VRAM_IS(address) (((address) >> 30) == 0u)
#ifndef GPU_STATE_ROW
#define GPU_STATE_ROW 64u   // the state texture's row RAM starts in
#endif
uint4 ram(uint index) {
    return GPU_STATE[uint2(index & 2047, GPU_STATE_ROW + (index >> 11))];
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
// CPU reads it: a 16-byte texel a pixel, from the first row.
#if defined(GPU_ROM)
uint texture_word(uint address) {
    [branch]
    if ((address >> 30) != 1) return ram_word(address);
    uint texel = (address - 0x40000000) >> 4;
    return asuint(_Data_ROM[uint2(texel & 2047, texel >> 11)])[(address >> 2) & 3];
}
// A 16-byte texel there.
uint4 texture_texel(uint address) {
    [branch]
    if ((address >> 30) != 1) return ram(texel_of(address));
    uint texel = (address - 0x40000000) >> 4;
    return asuint(_Data_ROM[uint2(texel & 2047, texel >> 11)]);
}
#else
#define texture_word ram_word
uint4 texture_texel(uint address) { return ram(texel_of(address)); }
#endif
#ifdef GPU_WRITEBACK
// For the Commit pass. The control pass leaves the copy a drawn list asked for in GPU_COPY; the
// next commit makes it: the RAM texels it covers take the picture (one 0x00RRGGBB word per
// pixel) and GPU_COPY is cleared. False for every other texel.
bool gpu_writeback(uint2 pos, out uint4 result) {
    result = 0;
    if (pos.y < 64) return false;
    uint index = (pos.y - 64) * 2048 + pos.x;
    if (index - DEV_T(0u) >= ((GPU_MEM_END - DEV_AT) >> 4)) return false;   // outside what a GPU program may own (DEV_AT to GPU_MEM_END)
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

#define INPUT_STATE DEV_T(0x100002u)   // pointer x, y (display pixels), buttons, key events so far
#define INPUT_KEYS  DEV_T(0x100008u)   // ring of the last 32 key events, one word each
#define FETCH_REPLY DEV_T(0x100011u)   // requests the host has answered, bytes, status (docs/system/fetch.md)
#define CON_HOST    DEV_T(0x16b828u)   // the console (docs/machine/console.md): characters delivered, the last of them
#ifndef CON_IN_TAG
#define CON_IN_TAG _UdonUARTInTag
#define CON_IN_CHARS _UdonUARTInChar
#endif
#define FETCH_DATA  DEV_T(0x16c000u)   // what the host fetched: 0x4000 texels
// "Open this" from the host (docs/system/open.md): requests so far and the address's length, then the
// address, 176 bytes of it in the eleven texels after and 80 more from HOST_OPEN_MORE.
#define HOST_OPEN      DEV_T(0x100024u)
#define HOST_OPEN_MORE DEV_T(0x100031u)
// What the cores did, for a guest to show (docs/machine/multicore.md): 16 words, core k's count of
// instructions in word k; then a texel of how many cores, which run and which are asleep (a bit each).
// With more than 16 cores: every core's count again in 64 texels at MC_STATS_ALL (the
// network's row, after its ring: DEV_AT + 0x16ba000), then eight texels, one for every 32 cores: a
// word of which of them run, a bit each, and a word of which are asleep. (A pixel that loops
// over every core takes as long as hundreds of instructions of the tick: none does.)
#define MC_STATS    DEV_T(0x100038u)
#define MC_STATS_ALL DEV_T(0x16ba00u)
// What the host says of where the machine is shown (docs/world/holodeck.md): two texels.
#define HOST_STATE  DEV_T(0x100036u)

#if defined(CORES) && CORES > 1
// How many workers the geometry has (src/types.h has the same: not every pass that has this file has it).
uint mc_geo_count(uint4 geo) {
    return (geo.r & 1) == 0 && geo.b >= 3 && geo.b <= 5 ? min(geo.g, (uint)CORES - 1) : 0;
}
// Texel w (below 64) of a core's state: core 0's in the first row, a worker's in the first tile of its slot.
uint4 mc_state_texel(uint core, uint w) {
    if (core == 0) return GPU_STATE[uint2(w, 0)];
    return GPU_STATE[uint2(((core & 31) << 6) + (w & 7), ((core >> 5) << 3) + (w >> 3))];
}
uint mc_stat_count(uint core) {
    return core == 0 || core <= mc_geo_count(ram(DEV_T(0x10003du))) ? mc_state_texel(core, 28).g : 0;
}
// Which of the 32 cores from `from` run, and which are asleep: a worker's state word
// (41,0).a has bit 0 while it runs and bit 1 while it sleeps on its job word. (Every load
// made, a core that is not there masked off, and the loop unrolled: the 32 loads go out
// together, not one after the last came back.)
uint2 mc_stat_bits(uint from) {
    uint running = 0, asleep = 0, count = mc_geo_count(ram(DEV_T(0x10003du)));
    [unroll] for (uint n = 0; n < 32; n++) {
        uint core = from + n;
        uint word = mc_state_texel(core & 255, 41).a;
        word = core == 0 ? 1 : core <= count ? word : 0;
        running |= (word & 1) << n;
        asleep |= ((word >> 1) & 1) << n;
    }
    return uint2(running, asleep);
}
// How many cores there are as the geometry has them now.
uint mc_stat_cores() {
    return 1 + mc_geo_count(ram(DEV_T(0x10003du)));
}
#endif

#ifdef GPU_INPUT
// A word of what the host fetched: its texture holds one byte a channel, one word a texel.
// With _FetchDeliver 2 it is a picture the host still has as a texture of its own (_HostImage,
// any size, sRGB, rows bottom up): _FetchW by _FetchH pixels of it, a word each, rows from the
// top, of which this part begins at byte _FetchOffset (a host that hands over the picture's
// own size hands over its pixels as they are). A pixel is its four bytes red, green, blue and
// alpha in that order, the alpha not laid over anything (3 November 2026; before, 0x00RRGGBB over
// white): the reply's size word has FETCH_RGBA set to say so (docs/system/fetch.md).
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
        uint4 b = (uint4)(saturate(float4(seen, c.a)) * 255.0 + 0.5);
        return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    }
    uint4 b = (uint4)(_HostData.Load(int3(n & 255, n >> 8, 0)) * 255.0 + 0.5);
    return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
}
#endif

#ifdef GPU_NET
// The network device (docs/system/lan.md). The guest sends one packet at a time through a slot the
// host reads back, and takes packets from a ring this pass fills from the host's texture.
#define NET_GUEST DEV_T(0x10003eu)   // the guest's: packets sent, packets taken from the ring
#define NET_HOST  DEV_T(0x10003fu)   // the host's: packets it took, packets delivered, the machine's number
#define NET_RX    DEV_T(0x16b840u)   // the ring: NET_SLOTS places of NET_SLOT texels (length, number, 8 bytes, the packet)
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
// submit word back and count it; and deliver the host's keyboard and pointer (docs/machine/input.md).
// The counters (programs/mc/mc.h, MC_COUNT_*): drawn when the host has read "added" in the head,
// each total is the sum of that word in all 256 regions (which only grow).
#define COUNT_TOTAL DEV_T(0xc1000u)
#define COUNT_REGIONS (COUNT_TOTAL + 0x100u)
#define COUNT_HEAD DEV_T(0x100032u)   // { added, COUNT_MERGED, merges, passes so far }: the control pass's
#define COUNT_MERGED 0x4d475245u
uint4 count_total(uint index) {
    uint4 sum = uint4(0, 0, 0, 0);
    [loop] for (uint k = 0; k < 256; k++) sum += ram(COUNT_REGIONS + 0x100u * k + (index - COUNT_TOTAL));
    return sum;
}

uint4 gpu_control(uint2 pos) {
    uint4 keep = GPU_STATE[pos];
    if (pos.y < 64) return keep;
    uint index = (pos.y - 64) * 2048 + pos.x;
    [branch]
    if (index >= COUNT_TOTAL && index < COUNT_REGIONS) return count_total(index);
    // A submitted list has been drawn by now: the GPU is free for the next one. A copy it asked
    // for is noted in GPU_COPY, for the next commit to make.
    // (A list that needs passes the host is not drawing yet waits, submitted, until it is.)
#ifdef GPU_INPUT
    bool served = !SUBMIT_WAITS(ram(GPU_CTRL).r, _GpuPasses);
#if defined(GPU_LIST_SKIP)
    served = served && _GpuListSkipped == 0;   // (rvc_harness's RVC_LIST_LAZY: the same; Unity draws it every frame)
#endif
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
    if (index == GPU_CLOCK) return uint4(keep.r, _HostMs, keep.b, _HostFlags);
    if (index == COUNT_HEAD) {
        // word 3: the commits so far, a pass each (a monitor's measure of what a core can run)
        uint passes = GPU_STATE[uint2(28, 0)].b;
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
            // (FETCH_RGBA: the size word's bit 15, a picture's pixels as RGBA bytes; a host's own _FetchInfo says it itself)
            return uint4(_FetchSeq, _FetchLength, _FetchStatus, _FetchInfo | _FetchW | (_FetchH << 16) | ((_FetchW | _FetchH) != 0 ? 0x8000u : 0u));
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
