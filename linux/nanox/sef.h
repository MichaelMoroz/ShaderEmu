/*
 * A font file of ours (docs/system/fonts.md, tools/make_fonts.py): glyphs the GPU draws from where the
 * file lies in the ROM. The window system's driver and the programs that measure text read it.
 */
#ifndef SHADEREMU_SEF_H
#define SHADEREMU_SEF_H

#include <stdint.h>

struct sef_head {
	char magic[4];			/* "SEF1" */
	uint16_t height, ascent;	/* of a line, in pixels */
	uint16_t bpp, em;		/* 1: a bit a pixel; 4: sixteen levels of coverage; SEF_MSDF */
	uint32_t glyphs;
	uint32_t ranges, ranges_at;	/* runs of code points, rising: struct sef_range */
	uint32_t advances_at;		/* a byte a glyph */
	uint32_t table_at;		/* a 16-byte texel a glyph: bitmap, width, height, dx | dy << 8 */
	uint32_t bits_at, size;
	uint32_t missing;		/* the glyph for a code point the font lacks, or ~0 */
	uint32_t range;			/* SEF_MSDF: the atlas's distance range in pixels at this size, 16.16 */
};

/* Glyphs that are distance fields in the face's atlas (font-NAME.msdf beside font-NAME-N.sef):
 * a table texel is where in the atlas, its width | height << 16, and the quad's width | height
 * << 16 and corner x | y << 16 (signed) in 64ths of a pixel at size `em`. */
#define SEF_MSDF 24

struct sef_atlas {
	char magic[4];			/* "SEM1" */
	uint16_t em, range;		/* its pixels an em; its distance range in those pixels */
	uint32_t glyphs, sizes;
	struct { uint16_t em, height; } size[11];	/* the .sef files made of it */
};

struct sef_range { uint32_t first, count, glyph; };

static inline int
sef_ok(const void *file, uint32_t size)
{
	const struct sef_head *h = file;

	return size >= sizeof *h && !__builtin_memcmp(h->magic, "SEF1", 4) && h->size <= size &&
	       h->table_at + 16 * h->glyphs <= h->size && !(h->table_at & 15);
}

/* The glyph of a code point, or -1 when the font has none. */
static inline int
sef_glyph(const void *file, uint32_t code)
{
	const struct sef_head *h = file;
	const struct sef_range *r = (const struct sef_range *)((const char *)file + h->ranges_at);
	uint32_t lo = 0, hi = h->ranges;

	/* (nearly every character is in the first range: 80 instructions a glyph by the halving, 2% of a desktop's start) */
	if (hi && code - r[0].first < r[0].count)
		return (int)(r[0].glyph + code - r[0].first);
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (code < r[mid].first)
			hi = mid;
		else if (code - r[mid].first >= r[mid].count)
			lo = mid + 1;
		else
			return (int)(r[mid].glyph + code - r[mid].first);
	}
	return -1;
}

static inline int
sef_advance(const void *file, int glyph)
{
	const struct sef_head *h = file;

	return ((const unsigned char *)file + h->advances_at)[glyph];
}

#endif
