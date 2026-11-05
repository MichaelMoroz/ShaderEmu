#ifndef GPU_UPLOADS_H
#define GPU_UPLOADS_H
// The GPU draws from its VRAM alone (docs/machine/gpu.md, "VRAM"): every texture, light map, glyph and sprite record a list
// names in RAM or the ROM is copied there by a CMD_UPLOAD of the same list. gpu_list_uploads() does it for a finished list,
// before it is submitted: each such texture gets a block of the scratch band (the VRAM's rows GPU_SCRATCH_Y to 4095, 2,048 of them, on
// shelves, with a texel of padding round it and no levels), an upload is added at the list's end, and the command is
// changed to draw the slot. Lists are drawn one at a time and their uploads first, so every program's lists share the band.
// A texture named twice in a list is copied once. Header only: a program's GPU library includes it.
#include <stdint.h>

#define GPU_SCRATCH_Y 2048u
#define GPU_SCRATCH_SIDE 4096u   // (the VRAM's side)

typedef struct {
    uint8_t* gpu;             // GPU memory as this program maps it, and where it is on the device (GPU_PHYS) and how large:
    uint32_t gpu_phys, gpu_size;   // an address the commands name there is read through the mapping, any other as it is
    uint32_t* list;           // the list's first command (16 words each), as this program writes it, and its device address
    uint32_t list_gpu;
    uint32_t count, max;      // its commands, and the most it may hold: the uploads are added after them
    uint32_t mesh, mesh_max;  // the device mesh's next vertex, and its end
    uint32_t* table;          // words of the list's own memory for the characters' slots (text), and their GPU address
    uint32_t table_gpu, table_left;
} gpu_uploads_list;

// (the device's words: shader_vm/src/gpu.h)
enum { GPU_UP_RECT = 2, GPU_UP_DRAW = 3, GPU_UP_TEXT = 4, GPU_UP_SPRITES = 5, GPU_UP_UPLOAD = 6 };
enum { GPU_UP_LIT = 0x800, GPU_UP_KEYED = 0x100, GPU_UP_TEXT_MORE = 0x100, GPU_UP_COMPACT = 0x400, GPU_UP_SPRITE = 9,
       GPU_UP_MSDF = 8, GPU_UP_COVER = 7, GPU_UP_MASK = 3, GPU_UP_RGB24 = 4, GPU_UP_FRAG_CLAMPED = 0x1000 };
#define GPU_UP_PREMULTIPLIED 0x08000000u
#define GPU_UP_CLAMPED 0x10000000u
#define GPU_UP_IS_SLOT(a) (((a) >> 30) == 0u)

// The band's shelves and the textures copied so far, for one list (a caller's: lists made at once on several cores each
// have one).
#define GPU_UP_SEEN 128
typedef struct {
    uint32_t x, y, shelf;
    uint32_t seen[GPU_UP_SEEN][5];   // address, kind, width, height, key: a texture copied already, and its slot
    uint32_t slots[GPU_UP_SEEN];
    uint32_t n;
    int full;
} __attribute__((aligned(16))) gpu_up_state;   // (16 bytes apart: two cores never store to one texel)

// A device address as this program reads it.
static inline const volatile uint32_t* gpu_up_at(const gpu_uploads_list* l, uint32_t address) {
    uint32_t off = address - l->gpu_phys;
    return (const volatile uint32_t*)(off < l->gpu_size ? (uintptr_t)(l->gpu + off) : (uintptr_t)address);
}

static inline volatile uint32_t* gpu_up_command(gpu_uploads_list* l, gpu_up_state* s) {
    if (l->count >= l->max || l->mesh + 6 > l->mesh_max) { s->full = 1; return 0; }
    volatile uint32_t* c = l->list + 16 * l->count++;
    for (int k = 0; k < 16; k++) c[k] = 0;
    c[0] = GPU_UP_UPLOAD;
    c[3] = l->mesh;
    l->mesh += 6;
    return c;
}

// A block of w x h and a texel round it on the band's shelves: its slot (texel 0, 0), or 0 when the band is full.
static inline uint32_t gpu_up_place(gpu_up_state* s, uint32_t w, uint32_t h) {
    uint32_t bw = w + 2, bh = h + 2;
    if (bw > GPU_SCRATCH_SIDE) return 0;
    if (s->x + bw > GPU_SCRATCH_SIDE) s->x = 0, s->y += s->shelf, s->shelf = 0;
    if (s->y + bh > GPU_SCRATCH_SIDE) { s->full = 1; return 0; }
    uint32_t slot = (s->x + 1) | (s->y + 1) << 12;
    s->x += bw;
    if (bh > s->shelf) s->shelf = bh;
    return slot;
}

// The slot a texture of fragment `kind` (its mode and FRAGMENT_KEYED) at `address`, w x h, is drawn from: copied once a list.
static inline uint32_t gpu_up_texture(gpu_uploads_list* l, gpu_up_state* s, uint32_t address, uint32_t kind, uint32_t w,
                                      uint32_t h, uint32_t key, uint32_t flags) {
    if (GPU_UP_IS_SLOT(address) || w == 0 || h == 0) return address;
    kind &= 0xff | GPU_UP_KEYED;
    for (uint32_t k = 0; k < s->n; k++)
        if (s->seen[k][0] == address && s->seen[k][1] == (kind | flags) && s->seen[k][2] == w && s->seen[k][3] == h &&
            (!(kind & GPU_UP_KEYED) || s->seen[k][4] == key))
            return s->slots[k];
    uint32_t slot = gpu_up_place(s, w, h);
    volatile uint32_t* c = slot ? gpu_up_command(l, s) : 0;
    if (!c) return address;   // (drawn magenta: the band or the list is full)
    slot |= flags | ((kind & GPU_UP_KEYED) ? GPU_UP_PREMULTIPLIED : 0);
    c[1] = address, c[2] = kind, c[4] = w, c[5] = h, c[6] = slot, c[7] = key;
    if (s->n < GPU_UP_SEEN) {
        uint32_t* e = s->seen[s->n];
        e[0] = address, e[1] = kind | flags, e[2] = w, e[3] = h, e[4] = key;
        s->slots[s->n++] = slot;
    }
    return slot;
}

// Every texture of the list's commands from `first` on in the
// VRAM (a program's kept commands too, given back their own textures first): 0, or -1 when the band or the list had no
// room for one (that draw is magenta).
// What a slot of the list `s` was made for was copied from (a list's sum goes by that, not by where a texture lies); 0 if none.
static inline uint32_t gpu_up_source(const gpu_up_state* s, uint32_t slot) {
    for (uint32_t k = 0; k < s->n; k++)
        if (s->slots[k] == slot) return s->seen[k][0];
    return 0;
}
static inline int gpu_list_uploads(gpu_uploads_list* l, uint32_t first, gpu_up_state* s) {
    s->x = 0, s->y = GPU_SCRATCH_Y, s->shelf = 0, s->n = 0, s->full = 0;
    const uint32_t end = l->count;
    for (uint32_t i = first; i < end; i++) {
        volatile uint32_t* c = l->list + 16 * i;
        uint32_t op = c[0];
        if (op == GPU_UP_RECT) {
            // words 8 to 11: mode, address, width, height; word 2 the key
            if ((c[8] & 0xff) != 0) c[9] = gpu_up_texture(l, s, c[9], c[8], c[10], c[11], c[2], (c[8] & GPU_UP_FRAG_CLAMPED) ? GPU_UP_CLAMPED : 0);
        } else if (op == GPU_UP_DRAW) {
            // word 5 the fragment, 7 the texture, 8 to 10 width, height, key; a compact lit draw's light map at 13, 14 its size
            uint32_t frag = c[5];
            if ((frag & 0xff) != 0 && (frag & 0xff) != GPU_UP_SPRITE)
                c[7] = gpu_up_texture(l, s, c[7], frag, c[8], c[9], c[10], (frag & GPU_UP_FRAG_CLAMPED) ? GPU_UP_CLAMPED : 0);
            if ((frag & GPU_UP_LIT) && (c[4] & GPU_UP_COMPACT))
                c[13] = gpu_up_texture(l, s, c[13], GPU_UP_RGB24, c[14] & 0xffff, c[14] >> 16, 0, GPU_UP_CLAMPED);
        } else if (op == GPU_UP_TEXT) {
            // word 2 the font's table (a texel a glyph: bitmap, width, height, corner), 6 the count, 7 the string; word 15
            // the characters' slots, a word each, which the device draws in place of the glyphs' bitmaps
            uint32_t n = c[6], mode = c[8] & 0xff, more = (c[8] & GPU_UP_TEXT_MORE) ? c[11] : 0;
            if (n > l->table_left) { s->full = 1; continue; }
            uint32_t kind = mode == GPU_UP_MSDF ? GPU_UP_MSDF : mode == GPU_UP_COVER ? GPU_UP_COVER : GPU_UP_MASK;
            for (uint32_t j = 0; j < n; j++) {
                uint32_t letter = gpu_up_at(l, c[7] + 4 * j)[0];
                const volatile uint32_t* g = gpu_up_at(l, c[2] + 16 * (letter & 0xffff));
                uint32_t w = kind == GPU_UP_MSDF ? g[1] & 0xffff : g[1], h = kind == GPU_UP_MSDF ? g[1] >> 16 : g[2];
                l->table[j] = gpu_up_texture(l, s, g[0] + more, kind, w, h, 0, 0);
            }
            c[15] = l->table_gpu;
            l->table += n, l->table_gpu += 4 * n, l->table_left -= n;
        } else if (op == GPU_UP_SPRITES) {
            // word 1 the records (a texel each: pixels, remap, x | y | stride, w | h | kind | stride), 2 how many: each record's
            // pixels made through its remaps by the upload (FRAGMENT_SPRITE: the record's and the command's addresses), and
            // drawn from its slot, a word each at word 15. A fill, a checker and a shade over the whole need none.
            uint32_t n = c[2];
            if (n > l->table_left) { s->full = 1; continue; }
            for (uint32_t r = 0; r < n; r++) {
                const volatile uint32_t* rec = gpu_up_at(l, c[1] + 16 * r);
                uint32_t kind = (rec[3] >> 22) & 15, w = rec[3] & 0x7ff, h = (rec[3] >> 11) & 0x7ff;
                l->table[r] = 0;
                if (kind == 4 || kind == 5 || kind == 6 || w == 0 || h == 0) continue;   // (SPRITE_FILL, _FILL_SHADE, _CHECKER)
                uint32_t slot = gpu_up_place(s, w, h);
                volatile uint32_t* u = slot ? gpu_up_command(l, s) : 0;
                if (!u) break;
                u[1] = rec[0], u[2] = GPU_UP_SPRITE, u[4] = w, u[5] = h, u[6] = slot;
                u[7] = c[1] + 16 * r, u[8] = c[6], u[9] = l->list_gpu + 64 * i;
                l->table[r] = slot;
            }
            c[15] = l->table_gpu;
            l->table += n, l->table_gpu += 4 * n, l->table_left -= n;
        }
    }
    return s->full ? -1 : 0;
}

#endif
