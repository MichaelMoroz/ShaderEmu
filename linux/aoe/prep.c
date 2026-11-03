/*
 * aoeprep: what Age of Empires' port reads, made in the machine from the trial's own files once they are unpacked
 * (docs/games/aoe.md, "The trial in the machine"); it was make_data.py on the build computer before.
 *
 *	aoeprep DIR		DIR/language.dll's string table into DIR/language.txt (as LoadString reads it), the
 *				trial's six TrueType faces at 8 to 36 pixels into DIR/aoe-fonts.bin, and the menus' pictures for
 *				800 x 600 brought down to the window's 640 x 480 into DIR/aoe-menus.bin, on the worker cores
 *
 * The faces are rasterised by ttf.h (the outlines' coverage, no hinting), not FreeType's hinting through Pillow as
 * make_data.py did: the bitmaps differ a little ("Text"). AOEPREP_WORKERS=N: the most worker cores (255).
 * Exit 0 when all three are made; 2 and a line otherwise.
 */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "mcw.h"
#include "mcwx.h"
#include "ttf.h"
#include "port/aoe_slp.h"

/* a core's own memory for a glyph's cells and points (a worker may not use the heap: multicore.md) */
#define ARENA (64u << 10)
static uint8_t *arenas;

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static uint8_t *
read_all(const char *dir, const char *name, uint32_t *size)
{
	char path[600];
	struct stat st;
	uint8_t *d;
	int fd;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st))
		return NULL;
	d = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	*size = (uint32_t)st.st_size;
	return d == MAP_FAILED ? NULL : d;
}

static int
write_all(const char *dir, const char *name, const void *data, uint32_t size)
{
	char path[600];
	FILE *f;

	snprintf(path, sizeof path, "%s/%s", dir, name);
	if (!(f = fopen(path, "wb")))
		return -1;
	if (fwrite(data, 1, size, f) != size) {
		fclose(f);
		return -1;
	}
	return fclose(f);
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8; }

/* ---------------------------------------------------------------- the strings */

/* Windows-1252's 0x80 to 0x9f, the text's bytes (0: none, '?') */
static const uint16_t cp1252_high[32] = {
	0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
	0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178,
};

static int
cp1252_of(uint32_t u)
{
	int i;

	if (u < 0x80 || (u >= 0xa0 && u < 0x100))
		return (int)u;
	for (i = 0; i < 32; i++)
		if (cp1252_high[i] == u)
			return 0x80 + i;
	return '?';
}

/* The DLL's RT_STRING table as make_data.py's pe_strings wrote it: "id<TAB>text" a line, by id, \\ \n \r escaped. */
static const char *
strings(const char *dir, int *count)
{
	uint32_t size, pe, nsec, opt, rva, i, base = 0, cap = 1 << 20, n = 0;
	uint8_t *d = read_all(dir, "language.dll", &size);
	char *out;
	struct { uint32_t va, sz, raw; } sec[32];
	int found = 0;

	*count = 0;
	if (!d || size < 0x40 || (pe = le32(d + 0x3c)) + 24 > size || memcmp(d + pe, "PE\0\0", 4))
		return "language.dll is not a Windows library";
	nsec = le16(d + pe + 6), opt = le16(d + pe + 20);
	rva = le32(d + pe + 24 + 112);	/* the resources' directory (a 32-bit one's) */
	for (i = 0; i < nsec && i < 32; i++) {
		const uint8_t *s = d + pe + 24 + opt + i * 40;

		sec[i].va = le32(s + 12), sec[i].sz = le32(s + 8) > le32(s + 16) ? le32(s + 8) : le32(s + 16), sec[i].raw = le32(s + 20);
		if (rva >= sec[i].va && rva < sec[i].va + sec[i].sz)
			base = rva - sec[i].va + sec[i].raw, found = 1;
	}
	if (!found || !(out = malloc(cap)))
		return "language.dll has no resources";
#define OFF(r, o) do { uint32_t k_; o = 0; for (k_ = 0; k_ < nsec && k_ < 32; k_++) \
		if ((r) >= sec[k_].va && (r) < sec[k_].va + sec[k_].sz) o = (r) - sec[k_].va + sec[k_].raw; } while (0)
	{
		/* type, then block (id / 16 + 1), then language; ids in order as the blocks come in order */
		const uint8_t *t = d + base;
		uint32_t types = le16(t + 12) + le16(t + 14), a;

		for (a = 0; a < types; a++) {
			uint32_t name = le32(t + 16 + a * 8), sub = le32(t + 20 + a * 8), b, blocks;
			const uint8_t *bt;

			if (name != 6)	/* RT_STRING */
				continue;
			bt = d + base + (sub & 0x7fffffff);
			blocks = le16(bt + 12) + le16(bt + 14);
			for (b = 0; b < blocks; b++) {
				uint32_t bid = le32(bt + 16 + b * 8), sub2 = le32(bt + 20 + b * 8), l, langs;
				const uint8_t *lt = d + base + (sub2 & 0x7fffffff);

				langs = le16(lt + 12) + le16(lt + 14);
				for (l = 0; l < langs; l++) {
					const uint8_t *leaf = d + base + le32(lt + 20 + l * 8);
					uint32_t o, k;

					OFF(le32(leaf), o);
					if (!o)
						continue;
					for (k = 0; k < 16; k++) {
						uint32_t len = le16(d + o), c;

						o += 2;
						if (len) {
							if (n + 16 + len * 2 + 2 > cap && !(out = realloc(out, cap *= 2)))
								return "not enough memory";
							{	/* (the number by hand: a printf a string was most of the time) */
								char digits[12];
								uint32_t id = (bid - 1) * 16 + k, d = 0;

								do
									digits[d++] = (char)('0' + id % 10), id /= 10;
								while (id);
								while (d)
									out[n++] = digits[--d];
								out[n++] = '\t';
							}
							for (c = 0; c < len; c++) {
								int ch = cp1252_of(le16(d + o + c * 2));

								if (ch == '\\')
									out[n++] = '\\', out[n++] = '\\';
								else if (ch == '\n')
									out[n++] = '\\', out[n++] = 'n';
								else if (ch == '\r')
									out[n++] = '\\', out[n++] = 'r';
								else
									out[n++] = (char)ch;
							}
							out[n++] = '\n';
							++*count;
						}
						o += len * 2;
					}
				}
			}
		}
	}
#undef OFF
	if (write_all(dir, "language.txt", out, n))
		return "language.txt could not be written";
	free(out);
	return NULL;
}


/* ---------------------------------------------------------------- the faces */

#define FIRST_SIZE 8
#define SIZES 29		/* 8 to 36 pixels */
#define FACES 6
#define FONTS (FACES * SIZES)
#define HEAD (24 + 8 + 256 + 1024)
static const struct { const char *name, *file; int bold; } faces[FACES] = {
	{ "Arial", "Arial.ttf", 0 }, { "Arial", "Arialbd.ttf", 1 }, { "Comic Sans MS", "Comic.ttf", 0 },
	{ "Comic Sans MS", "Comicbd.ttf", 1 }, { "Copperplate", "Coprgtl.ttf", 0 }, { "Copperplate", "Coprgtb.ttf", 1 },
};

#define GROUP 32		/* cells a job: a font's 256 in 8, so that the largest are shared out */
#define GROUPS (256 / GROUP)

struct font_job {
	uint32_t size, height, ascent, pad;
	uint8_t widths[256];
	uint32_t at[256];			/* each cell's bits in the file; a group's begin on whole texels */
} __attribute__((aligned(16)));

static struct {
	struct ttf face[FACES];
	struct font_job *job;
	uint8_t *file;
} f;

/* What a byte of the faces' Latin-1 cells is in Unicode (Windows-1252, as make_data.py read it; none: U+FFFD) */
static int
unicode_of(int c)
{
	if (c >= 0x80 && c < 0xa0)
		return cp1252_high[c - 0x80] ? cp1252_high[c - 0x80] : 0xfffd;
	return c;
}

/* A font's sizes: its line, ascent and each cell's width, from the face's own numbers (no pixels yet) */
static void
measure(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t k;

	(void)ctx;
	for (k = from; k < to; k++) {
		struct font_job *j = &f.job[k];
		const struct ttf *t = &f.face[k / SIZES];
		float scale = (float)j->size / (float)t->units;
		int c;

		j->ascent = (uint32_t)ceilf(t->ascent * scale);
		j->height = j->ascent + (uint32_t)ceilf(-t->descent * scale);
		for (c = 0; c < 256; c++) {
			int w = 0;

			if (c >= 32) {
				w = (int)lrintf(ttf_advance(t, ttf_glyph_index(t, unicode_of(c))) * scale);
				w = w > 255 ? 255 : w < 0 ? 0 : w;
			}
			j->widths[c] = (uint8_t)w;
		}
	}
}

/* A group of a font's cells, a bit a pixel set where the glyph covers 110 of 255 or more, rows (width + 7) / 8 bytes, each
 * glyph with its origin at the cell's left and its baseline at the ascent (Pillow's "la") */
static void
raster(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t k;

	(void)ctx;
	for (k = from; k < to; k++) {
		const struct font_job *j = &f.job[k / GROUPS];
		const struct ttf *t = &f.face[k / GROUPS / SIZES];
		float scale = (float)j->size / (float)t->units;
		uint8_t *mem = arenas + (size_t)mcw_self() * ARENA;
		int c, first = (int)(k % GROUPS) * GROUP;

		for (c = first; c < first + GROUP; c++) {
			int w = j->widths[c], h = (int)j->height, stride = (w + 7) / 8, x0, y0, x1, y1, gw, gh;
			int glyph = ttf_glyph_index(t, unicode_of(c));
			uint8_t *at = f.file + j->at[c];
			size_t cells;

			if (!w)
				continue;
			memset(at, 0, (size_t)stride * h);
			ttf_box(t, glyph, scale, &x0, &y0, &x1, &y1);
			gw = x1 - x0, gh = y1 - y0;
			/* (the cells from the arena's start, all 0 between glyphs; the points in its last quarter) */
			cells = (size_t)(gw + 2) * gh * 4;
			if (gw > 0 && gh > 0 && cells <= ARENA / 4 * 3)
				ttf_bits(t, glyph, scale, x0, y0, gw, gh, (float *)mem, mem + ARENA / 4 * 3, ARENA / 4, 110, at, stride, w, h,
					 (int)j->ascent);
		}
	}
}

static const char *
fonts(const char *dir, uint32_t *bytes)
{
	uint32_t k, start = 16 + HEAD * FONTS, total;
	mcw_for_opts o;
	char path[600], part[610];
	void *mapped = MAP_FAILED;
	int i, fd;

	for (i = 0; i < FACES; i++) {
		uint32_t size;
		const uint8_t *data = read_all(dir, faces[i].file, &size);

		if (!data || ttf_open(&f.face[i], data, size))
			return "a face of the trial's is missing or not TrueType";
	}
	if (posix_memalign((void **)&f.job, 16, FONTS * sizeof *f.job))
		return "not enough memory";
	memset(f.job, 0, FONTS * sizeof *f.job);
	for (k = 0; k < FONTS; k++)
		f.job[k].size = FIRST_SIZE + k % SIZES;
	memset(&o, 0, sizeof o);
	o.grain = 1;
	mcw_for_opt(FONTS, measure, NULL, &o);
	/* where each cell's bits go: a group's on whole texels (a core's own) */
	for (total = start, k = 0; k < FONTS; k++) {
		struct font_job *j = &f.job[k];
		int c;

		for (c = 0; c < 256; c++) {
			if (c % GROUP == 0)
				total = (total + 15) & ~15u;
			j->at[c] = total;
			total += (j->widths[c] + 7) / 8 * j->height;
		}
	}
	total = (total + 15) & ~15u;
	/* the file's own memory where it can be (tmpfs: a run of pages, mapped), which the workers write in place */
	snprintf(path, sizeof path, "%s/aoe-fonts.bin", dir);
	snprintf(part, sizeof part, "%s.part", path);
	if ((fd = open(part, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0)
		return "aoe-fonts.bin could not be written";
	if (ftruncate(fd, total) == 0 && (mapped = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED)
		f.file = mapped;
	else if (posix_memalign((void **)&f.file, 16, total))
		return "not enough memory";
	/* (a core's cells start all 0 and are left so: ttf_bits) */
	memset(arenas, 0, (size_t)(mcw_count() + 1) * ARENA);
	mcw_for_opt(FONTS * GROUPS, raster, NULL, &o);
	/* the heads, as make_data.py wrote them: the face's name, size, bold, line, ascent, widths, each cell's offset */
	memset(f.file, 0, start);
	((uint32_t *)f.file)[0] = FONTS;
	for (k = 0; k < FONTS; k++) {
		const struct font_job *j = &f.job[k];
		uint8_t *h = f.file + 16 + k * HEAD;

		strncpy((char *)h, faces[k / SIZES].name, 23);
		h[24] = (uint8_t)j->size, h[25] = (uint8_t)(j->size >> 8), h[26] = (uint8_t)faces[k / SIZES].bold;
		h[28] = (uint8_t)j->height, h[29] = (uint8_t)(j->height >> 8), h[30] = (uint8_t)j->ascent, h[31] = (uint8_t)(j->ascent >> 8);
		memcpy(h + 32, j->widths, 256);
		memcpy(h + 288, j->at, 1024);
	}
	*bytes = total;
	if (mapped != MAP_FAILED)
		munmap(mapped, total);
	else if (pwrite(fd, f.file, total, 0) != (ssize_t)total)
		return "aoe-fonts.bin could not be written";
	close(fd);
	if (rename(part, path))
		return "aoe-fonts.bin could not be written";
	return NULL;
}

/* ---------------------------------------------------------------- the menus' pictures */

/* The trial has the menus' pictures only for 800 x 600; the window is 640 x 480. Each menu's (its information file, a binary
 * resource of Interfac.drs, as TEasy_Panel::setup reads it: no picture of its own for 640 and one for 800) brought down whole
 * in its palette (port/aoe_slp.h), on the workers, into aoe-menus.bin, which the game reads (port/aoe_scale.cpp). */
#define MENU_W 640
#define MENU_H 480
#define MENUS_MOST 64

struct drs_res { uint32_t id, at, bytes; };
static const uint8_t *drs;
static uint32_t drs_bytes;

/* A resource of a kind ("slp ", "bina"; the file keeps the four letters backwards), or NULL. */
static const struct drs_res *
drs_find(const char *kind, int32_t id)
{
	uint32_t tables, t, n, i;

	if (!drs || drs_bytes < 64)
		return NULL;
	tables = le32(drs + 56);
	for (t = 0; t < tables && 64 + t * 12 + 12 <= drs_bytes; t++) {
		const uint8_t *h = drs + 64 + t * 12;
		uint32_t at = le32(h + 4);
		if (!(h[0] == kind[3] && h[1] == kind[2] && h[2] == kind[1] && h[3] == kind[0]) && memcmp(h, kind, 4))
			continue;
		n = le32(h + 8);
		for (i = 0; i < n && at + i * 12 + 12 <= drs_bytes; i++) {
			const struct drs_res *r = (const struct drs_res *)(drs + at + i * 12);
			if ((int32_t)r->id == id && r->at + r->bytes <= drs_bytes)
				return r;
		}
	}
	return NULL;
}

/* A palette resource (JASC-PAL text) as 256 x RGB; 0 if it is not one. */
static int
drs_palette(int32_t id, uint8_t *rgb)
{
	const struct drs_res *r = drs_find("bina", id);
	char text[16384];
	const char *p;
	int i, n, c[3];

	if (!r || r->bytes >= sizeof text)
		return 0;
	memcpy(text, drs + r->at, r->bytes);
	text[r->bytes] = 0;
	if (strncmp(text, "JASC-PAL", 8))
		return 0;
	for (p = text, i = 0; i < 3; i++)		/* the name, the version, the count */
		if (!(p = strchr(p, '\n')) || !*++p)
			return 0;
	for (i = 0; i < 256; i++) {
		if (sscanf(p, "%d %d %d%n", &c[0], &c[1], &c[2], &n) != 3)
			return 0;
		rgb[i * 3] = (uint8_t)c[0], rgb[i * 3 + 1] = (uint8_t)c[1], rgb[i * 3 + 2] = (uint8_t)c[2];
		p += n;
	}
	return 1;
}

static struct {
	const uint8_t *rgb;
	uint8_t *table;			/* 32768: the nearest of the palette's to each 15-bit colour */
	const uint8_t *slp;
	struct slp_cover *cover;
	int16_t *out;
} m;

static void
nearest_part(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t k;

	(void)ctx;
	for (k = from; k < to; k++)
		m.table[k] = (uint8_t)slp_nearest(m.rgb, k);
}

static int
nearest_table(void *ctx, unsigned key)
{
	return ((const uint8_t *)ctx)[key];
}

static void
rows_part(void *ctx, uint32_t from, uint32_t to)
{
	(void)ctx;
	slp_scale_rows(m.slp, m.rgb, MENU_W, (int)from, (int)to, nearest_table, m.table, m.cover,
		       (int16_t *)(arenas + (size_t)mcw_self() * ARENA), m.out);
}

static const char *
menus(const char *dir, int *made)
{
	struct { int32_t picture, palette; } want[MENUS_MOST];
	struct slp_menus_entry entry[MENUS_MOST];
	const struct drs_res *bina;
	uint32_t tables, t, i, n, total;
	int count = 0, k, j;
	uint8_t *file;
	static uint8_t rgb[768];
	int32_t palette_done = -1;
	mcw_for_opts o;

	*made = 0;
	if (!(drs = read_all(dir, "Interfac.drs", &drs_bytes)))
		return "Interfac.drs is missing";
	/* every information file: a menu with no picture for 640 and one of 800 x 600 or more */
	tables = le32(drs + 56);
	for (t = 0; t < tables; t++) {
		const uint8_t *h = drs + 64 + t * 12;
		if (memcmp(h, "anib", 4) && memcmp(h, "bina", 4))
			continue;
		n = le32(h + 8);
		for (i = 0; i < n; i++) {
			char text[4096], l[4][260], a1[260], a2[260], b1[260], b2[260], c1[260], c2[260], pal[260];
			int a1i, a2i, b1i, b2i, c1i, c2i, pali, two[2], q;
			const struct slp_frame *f;
			const struct drs_res *pic;

			bina = (const struct drs_res *)(drs + le32(h + 4) + i * 12);
			if (bina->bytes >= sizeof text || bina->at + bina->bytes > drs_bytes)
				continue;
			memcpy(text, drs + bina->at, bina->bytes);
			text[bina->bytes] = 0;
			/* (TEasy_Panel::setup's reading, to the palette's number) */
			if (sscanf(text, "%259s %259s %259s %d %d %259s %259s %259s %d %d %259s %259s %259s %d %d %259s %259s %d", l[0], a1, a2, &a1i,
				   &a2i, l[1], b1, b2, &b1i, &b2i, l[2], c1, c2, &c1i, &c2i, l[3], pal, &pali) != 18)
				continue;
			if (!strcmp(b1, "none") || drs_find("slp ", a1i))
				continue;
			two[0] = b1i, two[1] = b2i;
			for (q = 0; q < 2; q++) {
				if (!(pic = drs_find("slp ", two[q])))
					continue;
				f = (const struct slp_frame *)(drs + pic->at + sizeof(struct slp_head));
				if (f->width * MENU_H != f->height * MENU_W || f->width <= MENU_W)
					continue;
				for (k = 0; k < count && !(want[k].picture == two[q] && want[k].palette == pali); k++)
					;
				if (k == count && count < MENUS_MOST)
					want[count].picture = two[q], want[count++].palette = pali;
			}
		}
	}
	if (!count)
		return "no menu's picture to bring down";
	if (posix_memalign((void **)&m.table, 16, 32768) || posix_memalign((void **)&m.out, 16, MENU_W * MENU_H * 2) ||
	    !(m.cover = malloc(sizeof(struct slp_cover) * (MENU_W + MENU_H))))
		return "not enough memory";
	m.rgb = rgb;
	/* the file: its head, then each picture on 16 bytes */
	total = (sizeof(struct slp_menus_head) + count * sizeof(struct slp_menus_entry) + 15) & ~15u;
	if (!(file = malloc(total + (size_t)count * ((slp_bound(MENU_W, MENU_H) + 15) & ~15))))
		return "not enough memory";
	memset(file, 0, total);
	for (k = j = 0; k < count; k++) {
		const struct drs_res *pic = drs_find("slp ", want[k].picture);
		if (want[k].palette != palette_done) {
			if (!drs_palette(want[k].palette, rgb))
				continue;
			palette_done = want[k].palette;
			memset(&o, 0, sizeof o);
			o.grain = 512, o.out_bytes = 1, o.touch = m.table, o.touch_bytes = 32768;
			mcw_for_opt(32768, nearest_part, NULL, &o);
		}
		m.slp = drs + pic->at;
		if (!slp_covers(m.slp, MENU_W, MENU_H, m.cover))
			continue;
		memset(&o, 0, sizeof o);
		o.grain = 4, o.out_bytes = MENU_W * 2, o.touch = m.out, o.touch_bytes = MENU_W * MENU_H * 2;
		mcw_for_opt(MENU_H, rows_part, NULL, &o);
		entry[j].picture = want[k].picture, entry[j].palette = want[k].palette;
		entry[j].width = MENU_W, entry[j].height = MENU_H, entry[j].pad = 0;
		entry[j].at = total;
		entry[j].bytes = (uint32_t)slp_encode(m.slp, m.out, MENU_W, MENU_H, file + total);
		total = (total + entry[j].bytes + 15) & ~15u;
		j++;
	}
	memcpy(file, "AOEMENU1", 8);
	((struct slp_menus_head *)file)->count = (uint32_t)j;
	memcpy(file + sizeof(struct slp_menus_head), entry, j * sizeof *entry);
	if (write_all(dir, "aoe-menus.bin", file, total))
		return "aoe-menus.bin could not be written";
	free(file);
	*made = j;
	return NULL;
}

extern const char etext[];
extern void _start(void);

int
main(int argc, char **argv)
{
	const char *why, *e = getenv("AOEPREP_WORKERS");
	int count = 0, workers = e ? atoi(e) : 255, got = 0, pictures = 0;
	uint32_t bytes = 0;
	double t0 = now(), t1;

	if (argc != 2) {
		fprintf(stderr, "usage: aoeprep DIR\n");
		return 2;
	}
	if ((why = strings(argv[1], &count))) {
		fprintf(stderr, "aoeprep: %s\n", why);
		return 2;
	}
	t1 = now();
	if (workers > 0) {
#ifndef AOEPREP_HOST
		/* a worker stops at every page of code no core has run: the text read here once (from _start's page) */
		const char *text = (const char *)((uintptr_t)_start & ~(uintptr_t)4095);

		mcw_touch_code(text, (unsigned)(etext - text));
#endif
		got = mcw_open(workers < FONTS - 1 ? workers : FONTS - 1);
	}
	if (posix_memalign((void **)&arenas, 16, (size_t)(got + 1) * ARENA)) {
		fprintf(stderr, "aoeprep: not enough memory\n");
		return 2;
	}
	if ((why = fonts(argv[1], &bytes))) {
		fprintf(stderr, "aoeprep: %s\n", why);
		return 2;
	}
	printf("aoeprep: %d strings in %.2f s, %d fonts (%u bytes) in %.2f s on %d workers\n", count, t1 - t0, FONTS, bytes, now() - t1,
	       got);
	t1 = now();
	if ((why = menus(argv[1], &pictures))) {
		/* (the game then makes a menu's picture itself, more slowly: an empty file says it was tried) */
		struct slp_menus_head none = {"AOEMENU1", 0, 0};

		fprintf(stderr, "aoeprep: %s\n", why);
		write_all(argv[1], "aoe-menus.bin", &none, sizeof none);
	}
	printf("aoeprep: %d menus' pictures brought to %d x %d in %.2f s\n", pictures, MENU_W, MENU_H, now() - t1);
	mcw_close();
	return 0;
}
