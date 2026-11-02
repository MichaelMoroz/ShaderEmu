/*
 * aoeprep: what Age of Empires' port reads, made in the machine from the trial's own files once they are unpacked
 * (docs/games/aoe.md, "The trial in the machine"); it was make_data.py on the build computer before.
 *
 *	aoeprep DIR		DIR/language.dll's string table into DIR/language.txt (as LoadString reads it), and the
 *				trial's six TrueType faces at 8 to 36 pixels into DIR/aoe-fonts.bin, on the worker cores
 *
 * The faces are rasterised by ttf.h (the outlines' coverage, no hinting), not FreeType's hinting through Pillow as
 * make_data.py did: the bitmaps differ a little ("Text"). AOEPREP_WORKERS=N: the most worker cores (255).
 * Exit 0 when both are made; 2 and a line otherwise.
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

extern const char etext[];
extern void _start(void);

int
main(int argc, char **argv)
{
	const char *why, *e = getenv("AOEPREP_WORKERS");
	int count = 0, workers = e ? atoi(e) : 255, got = 0;
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
	mcw_close();
	return 0;
}
