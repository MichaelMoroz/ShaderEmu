// The trial's TrueType faces rasterised when the image is made (linux/aoe/make_data.py, aoe-fonts.bin):
// a bit a pixel, Latin-1. docs/games/aoe.md, "Text".
#ifndef AOE_FONTS_H
#define AOE_FONTS_H
struct AoeFont {
    char face[24];
    unsigned short size, bold;
    unsigned short height, ascent;
    unsigned char widths[256];
    unsigned offsets[256];   // from the file's start
};
const AoeFont* aoe_font_get(const char* face, int size, int bold);
extern const unsigned char* aoe_font_file;
static inline int aoe_glyph_width(const AoeFont* f, unsigned char c) { return f ? f->widths[c] : 6; }
static inline const unsigned char* aoe_glyph_bits(const AoeFont* f, unsigned char c) { return aoe_font_file + f->offsets[c]; }
#endif
