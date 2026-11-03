// A menu's picture for 800 x 600 brought down whole to a smaller window (docs/games/aoe.md, "Windowed"; aoe_slp.h): the trial
// has no pictures for 640 x 480. aoeprep made them on the workers once the trial was unpacked (aoe-menus.bin in AOE_DATA), and
// a menu takes its own from there; one that is not there is made here, on this core, once (a few seconds of the machine's).
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include "../include/Shape.h"
#include "aoe_slp.h"
#undef open

// aoeprep's pictures, mapped once
static const unsigned char* menus;
static long menus_bytes;
static const unsigned char* menu_of(int picture, int palette, int w, int h, long* bytes) {
    static int looked;
    if (!looked) {
        looked = 1;
        const char* dir = getenv("AOE_DATA");
        char path[300];
        snprintf(path, sizeof path, "%s/aoe-menus.bin", dir ? dir : "/usr/share/aoe");
        int fd = ::open(path, O_RDONLY);
        if (fd >= 0) {
            long n = lseek(fd, 0, SEEK_END);
            void* m = n > (long) sizeof(slp_menus_head) ? mmap(0, n, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
            close(fd);
            if (m != MAP_FAILED && !memcmp(m, "AOEMENU1", 8)) { menus = (const unsigned char*) m; menus_bytes = n; }
        }
    }
    if (!menus) return 0;
    const slp_menus_head* head = (const slp_menus_head*) menus;
    const slp_menus_entry* e = (const slp_menus_entry*) (head + 1);
    for (unsigned i = 0; i < head->count; i++, e++)
        if (e->picture == picture && e->palette == palette && e->width == w && e->height == h && e->at + e->bytes <= (unsigned long) menus_bytes) {
            *bytes = e->bytes;
            return menus + e->at;
        }
    return 0;
}

// The palette's nearest index to a 15-bit colour, found as colours come, kept while the palette is the same
static unsigned short nearest_of[32768];          // index + 1, or 0 not yet known
static unsigned char table_rgb[768];
static unsigned nearest_made;
static int nearest_here(void* ctx, unsigned key) {
    unsigned short n = nearest_of[key];
    if (n) return n - 1;
    nearest_made++;
    int best = slp_nearest((const unsigned char*) ctx, key);
    nearest_of[key] = (unsigned short) (best + 1);
    return best;
}

static TShape* shape_of(const unsigned char* blob, long bytes) {
    unsigned char* f = (unsigned char*) malloc(bytes);
    if (!f) return 0;
    memcpy(f, blob, bytes);
    TShape* s = new TShape();
    s->FShape = (SLhape_File_Header*) f;
    s->shape_info = (Shape_Info*) (s->FShape + 1);
    s->load_type = 1;   // (freed by its destructor)
    s->load_size = bytes;
    return s;
}

// A menu's picture larger than the area it is drawn on, brought down to fit it (both sides by one factor), in place. The
// picture's and the palette's resource numbers find aoeprep's.
void aoe_picture_fit(TShape** pic, void* palette, int area_w, int area_h, int picture_id, int palette_id) {
    if (!*pic || !(*pic)->FShape || !palette) return;
    Shape_Info* info = (*pic)->shape_info;
    int w = info->Width, h = info->Height;
    if (w <= area_w && h <= area_h) return;
    int to_w = area_w, to_h = h * area_w / w;
    if (to_h > area_h) { to_h = area_h; to_w = w * area_h / h; }
    unsigned t0; __asm__ volatile("rdinstret %0" : "=r"(t0));
    const char* how = "aoeprep's";
    long bytes = 0;
    const unsigned char* made = menu_of(picture_id, palette_id, to_w, to_h, &bytes);
    TShape* s = 0;
    unsigned colours = nearest_made;
    if (made) s = shape_of(made, bytes);
    else {
        // Not made by aoeprep: here. The last few made are kept, as a menu shown again asks again.
        how = "made";
        PALETTEENTRY pal[256];
        unsigned char rgb[768];
        if (!GetPaletteEntries((HPALETTE) palette, 0, 256, pal)) return;
        for (int i = 0; i < 256; i++) { rgb[i * 3] = pal[i].peRed; rgb[i * 3 + 1] = pal[i].peGreen; rgb[i * 3 + 2] = pal[i].peBlue; }
        if (memcmp(rgb, table_rgb, sizeof rgb)) { memcpy(table_rgb, rgb, sizeof rgb); memset(nearest_of, 0, sizeof nearest_of); }
        enum { KEPT = 3 };
        static struct { int picture, palette, w, h; unsigned char* blob; long bytes; } kept[KEPT];
        static int kept_next;
        for (int i = 0; i < KEPT && !s; i++)
            if (kept[i].blob && kept[i].picture == picture_id && kept[i].palette == palette_id && kept[i].w == to_w && kept[i].h == to_h) {
                s = shape_of(kept[i].blob, kept[i].bytes);
                how = "kept";
            }
        if (!s) {
            const unsigned char* slp = (const unsigned char*) (*pic)->FShape;
            struct slp_cover* cover = (struct slp_cover*) malloc(sizeof(struct slp_cover) * (to_w + to_h));
            short* rows = (short*) malloc(sizeof(short) * w * 3);
            short* out = (short*) malloc(sizeof(short) * to_w * to_h);
            unsigned char* blob = (unsigned char*) malloc(slp_bound(to_w, to_h));
            if (cover && rows && out && blob && slp_covers(slp, to_w, to_h, cover)) {
                slp_scale_rows(slp, table_rgb, to_w, 0, to_h, nearest_here, table_rgb, cover, rows, out);
                bytes = slp_encode(slp, out, to_w, to_h, blob);
                s = shape_of(blob, bytes);
                int k = kept_next++ % KEPT;
                free(kept[k].blob);
                kept[k].blob = (unsigned char*) realloc(blob, bytes);
                kept[k].bytes = bytes; kept[k].picture = picture_id; kept[k].palette = palette_id; kept[k].w = to_w; kept[k].h = to_h;
                blob = 0;
            }
            free(cover); free(rows); free(out); free(blob);
        }
    }
    unsigned t1; __asm__ volatile("rdinstret %0" : "=r"(t1));
    if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: picture %d (palette %d) of %d x %d brought to %d x %d (%s), %u instructions, %u new colours\n",
        picture_id, palette_id, w, h, to_w, to_h, how, t1 - t0, nearest_made - colours);
    if (!s) return;
    delete *pic;
    *pic = s;
}
