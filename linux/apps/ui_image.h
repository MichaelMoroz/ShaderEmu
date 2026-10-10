/*
 * PNG and JPEG pictures, decoded on the machine's worker cores (docs/multicore.md,
 * docs/nanox.md): ui_image_to_ppm() turns one into a PPM file, which is what the display
 * draws (GrDrawImageFromFile: the GPU scales it, nothing is drawn a pixel at a time).
 *
 * What costs on this machine is not the arithmetic but the writing: a core keeps about 6 KB
 * of new stores a pass, and a picture is megabytes. So the decoders are laid out by who
 * writes what, each core its own memory, and the picture's pixels are written once, into
 * the file's own pages (it is mapped), by as many cores as there are:
 *
 *   PNG    worker 1 inflates the stream into the filtered rows; worker 2 follows it and
 *          takes the filters off, into rows of its own; worker 3 follows that and writes
 *          the pixels. Each looks at a count the one before it keeps.
 *   JPEG   worker 1 reads the Huffman codes into a short list of the coefficients that are
 *          not zero; the others take strips of 16 rows each, as they are ready, and do the
 *          transform and the colours for them on their stacks, writing only pixels.
 *
 * With fewer workers, or none, the same functions run one after another on this core.
 * Not read: interlaced PNG, progressive and arithmetic-coded JPEG, CMYK.
 */
#ifndef SHADEREMU_APPS_UI_IMAGE_H
#define SHADEREMU_APPS_UI_IMAGE_H

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mcw.h"

/* What the stages share. A count is written by one stage and read by the next; each is in 16
 * bytes of its own (mcw_alloc), as is everything a worker writes. */
struct img_job {
	const uint8_t *file;	/* the picture's file, mapped */
	uint32_t size;
	uint8_t *rgb;		/* the PPM's pixels, mapped: 3 bytes a pixel, rows one after another */
	int w, h;
	volatile uint32_t *failed;	/* any stage: the picture cannot be read */
	/* PNG */
	int depth, type, channels, row_bytes, pixel_bytes;
	uint8_t palette[256][3];
	uint8_t *raw;		/* the filtered rows, each after its filter's byte */
	uint8_t *rows;		/* the rows without their filters, row_pitch apart */
	int row_pitch;
	void *codes;		/* the inflating stage's tables */
	volatile uint32_t *inflated;	/* bytes of raw there are */
	volatile uint32_t *unfiltered;	/* rows of rows there are */
	/* JPEG */
	int components, mcu_w, mcu_h, mcus_x, mcus_y, restart;
	struct { int h, v, table, dc, ac; } comp[3];
	uint16_t quant[4][64];
	struct img_huff { uint8_t length[17]; uint8_t symbol[256]; uint16_t fast[512]; int32_t max[18]; int32_t first[17]; } huff[2][2];
	uint32_t scan_at;	/* where the entropy-coded data begins in the file */
	uint8_t *list;		/* the coefficients that are not zero, a strip of MCU rows after another */
	uint32_t list_room;
	uint32_t *strip_at;	/* where each MCU row's part of the list begins (mcus_y + 1 of them) */
	volatile uint32_t *listed;	/* MCU rows of the list there are */
	int strips, sharers;
};

static struct img_job *img;	/* (one picture at a time) */

static inline uint32_t img_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Waits for a count another core keeps. A pass's stores are seen a pass later, so there is
 * nothing to do but look again; on this core alone the count is already there. */
static inline int
img_wait(volatile uint32_t *count, uint32_t least)
{
	while (*count < least)
		if (*img->failed)
			return 0;
	return 1;
}

/* ---------------------------------------------------------------- PNG: inflate */

struct img_bits { const uint8_t *at, *chunk_end, *file_end; uint32_t hold; int count, bad, over; };

/* The next byte of the IDAT chunks, which lie one after another with a header between. Past
 * the last of them there are zeros (a table looks a few bits ahead of the stream's end). */
static int
img_idat_next(struct img_bits *b)
{
	while (b->at >= b->chunk_end) {
		const uint8_t *next = b->chunk_end + 4;	/* past the chunk's check */
		uint32_t length;

		if (next + 8 > b->file_end || memcmp(next + 4, "IDAT", 4) != 0) {
			if (++b->over > 8)
				b->bad = 1;
			return 0;
		}
		length = img_be32(next);
		b->at = next + 8;
		b->chunk_end = b->at + length;
		if (b->chunk_end > b->file_end) {
			b->bad = 1;
			return 0;
		}
	}
	return *b->at++;
}

static inline void
img_need(struct img_bits *b, int n)
{
	while (b->count < n) {
		b->hold |= (uint32_t)(b->at < b->chunk_end ? *b->at++ : img_idat_next(b)) << b->count;
		b->count += 8;
	}
}

static inline uint32_t
img_take(struct img_bits *b, int n)
{
	uint32_t v;

	img_need(b, n);
	v = b->hold & ((1u << n) - 1);
	b->hold >>= n;
	b->count -= n;
	return v;
}

/* A code's symbols by length, and for the codes of up to IMG_QUICK bits a table from the next
 * bits of the stream straight to the symbol and its length (length << 12 | symbol; 0: longer). */
#define IMG_QUICK 10
struct img_codes { uint16_t count[16], symbol[288], quick[1 << IMG_QUICK]; };

static void
img_codes_make(struct img_codes *c, const uint8_t *lengths, int n)
{
	uint16_t offset[16], next[16];
	int i, code = 0;

	memset(c->count, 0, sizeof c->count);
	memset(c->quick, 0, sizeof c->quick);
	for (i = 0; i < n; i++)
		c->count[lengths[i]]++;
	c->count[0] = 0;
	offset[1] = 0;
	for (i = 1; i < 15; i++)
		offset[i + 1] = offset[i] + c->count[i];
	for (i = 1; i <= 15; i++) {
		next[i] = (uint16_t)code;
		code = (code + c->count[i]) << 1;
	}
	for (i = 0; i < n; i++) {
		int length = lengths[i];

		if (!length)
			continue;
		c->symbol[offset[length]++] = (uint16_t)i;
		if (length <= IMG_QUICK) {
			/* the code's bits arrive lowest first: turned round, then every filling of the bits above */
			int straight = next[length], turned = 0, k;

			for (k = 0; k < length; k++)
				turned |= (straight >> k & 1) << (length - 1 - k);
			for (k = turned; k < 1 << IMG_QUICK; k += 1 << length)
				c->quick[k] = (uint16_t)(length << 12 | i);
		}
		next[length]++;
	}
}

static inline int
img_code(struct img_bits *b, const struct img_codes *c)
{
	int code = 0, first = 0, index = 0, length;
	uint16_t quick;

	img_need(b, 15);
	quick = c->quick[b->hold & ((1 << IMG_QUICK) - 1)];
	if (quick) {
		b->hold >>= quick >> 12;
		b->count -= quick >> 12;
		return quick & 0xfff;
	}
	for (length = 1; length <= 15; length++) {
		int count = c->count[length];

		code |= (int)(b->hold & 1);
		b->hold >>= 1;
		b->count--;
		if (code - count < first)
			return c->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	b->bad = 1;
	return 0;
}

/* Stage 1: the compressed stream into the filtered rows. */
static uint32_t
img_png_inflate(uint32_t first_chunk, uint32_t unused)
{
	static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	struct img_job *j = img;
	struct img_bits b;
	/* the three tables in memory of this stage's own (a worker's variables must not lie beside another core's) */
	struct img_codes *tables = j->codes;
#define literals tables[0]
#define distances tables[1]
#define meta tables[2]
	uint8_t lengths[320];
	uint8_t *out = j->raw;
	uint32_t total = (uint32_t)(j->row_bytes + 1) * (uint32_t)j->h, n = 0, last, told = 0;

	(void)unused;
	memset(&b, 0, sizeof b);
	b.file_end = j->file + j->size;
	b.at = j->file + first_chunk + 8;
	b.chunk_end = b.at + img_be32(j->file + first_chunk);
	img_take(&b, 16);	/* the zlib header */
	do {
		uint32_t kind;

		last = img_take(&b, 1);
		kind = img_take(&b, 2);
		if (kind == 0) {
			uint32_t length;

			img_take(&b, b.count & 7);
			length = img_take(&b, 16);
			img_take(&b, 16);
			while (length-- && n < total && !b.bad)
				out[n++] = (uint8_t)img_take(&b, 8);
		} else if (kind == 1 || kind == 2) {
			int i;

			if (kind == 1) {
				for (i = 0; i < 288; i++)
					lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
				img_codes_make(&literals, lengths, 288);
				for (i = 0; i < 30; i++)
					lengths[i] = 5;
				img_codes_make(&distances, lengths, 30);
			} else {
				int hlit = (int)img_take(&b, 5) + 257, hdist = (int)img_take(&b, 5) + 1, hclen = (int)img_take(&b, 4) + 4;

				memset(lengths, 0, 19);
				for (i = 0; i < hclen; i++)
					lengths[order[i]] = (uint8_t)img_take(&b, 3);
				img_codes_make(&meta, lengths, 19);
				for (i = 0; i < hlit + hdist && !b.bad;) {
					int symbol = img_code(&b, &meta), repeat = 1, value = symbol;

					if (symbol == 16) {
						value = i ? lengths[i - 1] : 0;
						repeat = 3 + (int)img_take(&b, 2);
					} else if (symbol == 17) {
						value = 0;
						repeat = 3 + (int)img_take(&b, 3);
					} else if (symbol == 18) {
						value = 0;
						repeat = 11 + (int)img_take(&b, 7);
					}
					while (repeat-- && i < hlit + hdist)
						lengths[i++] = (uint8_t)value;
				}
				img_codes_make(&literals, lengths, hlit);
				img_codes_make(&distances, lengths + hlit, hdist);
			}
			while (!b.bad) {
				int symbol = img_code(&b, &literals);

				if (symbol < 256) {
					if (n < total)
						out[n++] = (uint8_t)symbol;
				} else if (symbol == 256) {
					break;
				} else if (symbol < 286) {
					uint32_t length = length_base[symbol - 257] + img_take(&b, length_extra[symbol - 257]);
					int d = img_code(&b, &distances);
					uint32_t distance;

					if (d >= 30) {
						b.bad = 1;
						break;
					}
					distance = distance_base[d] + img_take(&b, distance_extra[d]);
					if (distance > n) {
						b.bad = 1;
						break;
					}
					while (length-- && n < total) {
						out[n] = out[n - distance];
						n++;
					}
				} else {
					b.bad = 1;
				}
				/* the next stage is told now and then, not at every byte */
				if (n - told >= 1024)
					*j->inflated = told = n;
			}
		} else {
			b.bad = 1;
		}
		*j->inflated = n;
	} while (!last && !b.bad && n < total);
	if (b.bad || n < total)
		*j->failed = 1;
	*j->inflated = total;
	return n;
#undef literals
#undef distances
#undef meta
}

/* ---------------------------------------------------------------- PNG: filters, pixels */

static inline int
img_paeth(int a, int b, int c)
{
	int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;

	return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* Stage 2: each row without its filter, into a row of its own. */
static uint32_t
img_png_unfilter(uint32_t unused0, uint32_t unused1)
{
	struct img_job *j = img;
	int y, x, n = j->row_bytes, bpp = j->pixel_bytes;

	(void)unused0, (void)unused1;
	for (y = 0; y < j->h; y++) {
		const uint8_t *in = j->raw + (uint32_t)y * (uint32_t)(n + 1);
		uint8_t *out = j->rows + (uint32_t)y * (uint32_t)j->row_pitch;
		const uint8_t *up = y ? out - j->row_pitch : NULL;

		if (!img_wait(j->inflated, (uint32_t)(y + 1) * (uint32_t)(n + 1)))
			break;
		switch (in[0]) {
		case 0:
			memcpy(out, in + 1, n);
			break;
		case 1:
			for (x = 0; x < bpp && x < n; x++)
				out[x] = in[1 + x];
			for (; x < n; x++)
				out[x] = (uint8_t)(in[1 + x] + out[x - bpp]);
			break;
		case 2:
			if (!up)
				memcpy(out, in + 1, n);
			else
				for (x = 0; x < n; x++)
					out[x] = (uint8_t)(in[1 + x] + up[x]);
			break;
		case 3:
			for (x = 0; x < bpp && x < n; x++)
				out[x] = (uint8_t)(in[1 + x] + ((up ? up[x] : 0) >> 1));
			for (; x < n; x++)
				out[x] = (uint8_t)(in[1 + x] + ((out[x - bpp] + (up ? up[x] : 0)) >> 1));
			break;
		case 4:
			for (x = 0; x < bpp && x < n; x++)
				out[x] = (uint8_t)(in[1 + x] + (up ? up[x] : 0));
			if (up)
				for (; x < n; x++)
					out[x] = (uint8_t)(in[1 + x] + img_paeth(out[x - bpp], up[x], up[x - bpp]));
			else
				for (; x < n; x++)
					out[x] = (uint8_t)(in[1 + x] + out[x - bpp]);
			break;
		default:
			*j->failed = 1;
		}
		if ((y & 3) == 3 || y == j->h - 1)
			*j->unfiltered = (uint32_t)y + 1;
	}
	*j->unfiltered = (uint32_t)j->h;
	return (uint32_t)y;
}

/* Stage 3: the rows as pixels of red, green and blue; what is see-through over white. */
static uint32_t
img_png_pixels(uint32_t unused0, uint32_t unused1)
{
	struct img_job *j = img;
	int y, x, wide = j->depth == 16 ? 2 : 1;

	(void)unused0, (void)unused1;
	for (y = 0; y < j->h; y++) {
		const uint8_t *in = j->rows + (uint32_t)y * (uint32_t)j->row_pitch;
		uint8_t *out = j->rgb + (uint32_t)y * (uint32_t)j->w * 3;

		if (!img_wait(j->unfiltered, (uint32_t)y + 1))
			break;
		if (j->type == 2 && j->depth == 8) {
			memcpy(out, in, (size_t)j->w * 3);
			continue;
		}
		if (j->type == 6 && j->depth == 8) {
			for (x = 0; x < j->w; x++, out += 3, in += 4) {
				int a = in[3];

				if (a == 255) {
					out[0] = in[0], out[1] = in[1], out[2] = in[2];
				} else {
					int white = 255 * (255 - a) + 127;

					out[0] = (uint8_t)((in[0] * a + white) / 255);
					out[1] = (uint8_t)((in[1] * a + white) / 255);
					out[2] = (uint8_t)((in[2] * a + white) / 255);
				}
			}
			continue;
		}
		for (x = 0; x < j->w; x++, out += 3) {
			int r, g, b, a = 255;

			if (j->type == 3 || (j->type == 0 && j->depth < 8)) {
				int shift = 8 - j->depth - (x * j->depth & 7), v = in[x * j->depth >> 3] >> shift & ((1 << j->depth) - 1);

				if (j->type == 3) {
					r = j->palette[v][0], g = j->palette[v][1], b = j->palette[v][2];
				} else {
					r = g = b = v * 255 / ((1 << j->depth) - 1);
				}
			} else {
				const uint8_t *p = in + x * j->channels * wide;

				if (j->type == 0 || j->type == 4) {
					r = g = b = p[0];
					if (j->type == 4)
						a = p[wide];
				} else {
					r = p[0], g = p[wide], b = p[2 * wide];
					if (j->type == 6)
						a = p[3 * wide];
				}
			}
			if (a != 255) {
				r = (r * a + 255 * (255 - a)) / 255;
				g = (g * a + 255 * (255 - a)) / 255;
				b = (b * a + 255 * (255 - a)) / 255;
			}
			out[0] = (uint8_t)r, out[1] = (uint8_t)g, out[2] = (uint8_t)b;
		}
	}
	return (uint32_t)y;
}

/* ---------------------------------------------------------------- JPEG: the codes */

struct img_scan { const uint8_t *at, *end; uint32_t hold; int count; int bad; };

static inline void
img_scan_fill(struct img_scan *s)
{
	while (s->count <= 24) {
		int byte = 0;

		if (s->at < s->end) {
			byte = *s->at;
			if (byte == 0xff) {
				if (s->at + 1 < s->end && s->at[1] == 0)
					s->at += 2;
				else
					byte = 0;	/* a marker: zeros until someone looks */
			} else {
				s->at++;
			}
		}
		s->hold |= (uint32_t)byte << (24 - s->count);
		s->count += 8;
	}
}

static inline int
img_scan_bits(struct img_scan *s, int n)
{
	int v;

	if (!n)
		return 0;
	if (s->count < n)
		img_scan_fill(s);
	v = (int)(s->hold >> (32 - n));
	s->hold <<= n;
	s->count -= n;
	return v;
}

static void
img_huff_make(struct img_huff *h)
{
	int code = 0, k = 0, length, i;

	memset(h->fast, 0, sizeof h->fast);
	for (length = 1; length <= 16; length++) {
		h->first[length] = k - code;
		for (i = 0; i < h->length[length]; i++, k++, code++)
			if (length <= 9) {
				int from = code << (9 - length), n = 1 << (9 - length);

				while (n--)
					h->fast[from + n] = (uint16_t)(length << 8 | h->symbol[k]);
			}
		h->max[length] = h->length[length] ? code : -1;
		code <<= 1;
	}
	h->max[17] = 0x7fffffff;
}

static inline int
img_huff_symbol(struct img_scan *s, const struct img_huff *h)
{
	int code, length;
	uint16_t fast;

	if (s->count < 16)
		img_scan_fill(s);
	fast = h->fast[s->hold >> 23];
	if (fast) {
		s->hold <<= fast >> 8;
		s->count -= fast >> 8;
		return fast & 255;
	}
	code = (int)(s->hold >> 22);
	for (length = 10; length <= 16; length++, code = (int)(s->hold >> (32 - length)))
		if (h->max[length] >= 0 && code < h->max[length]) {
			s->hold <<= length;
			s->count -= length;
			return h->symbol[(h->first[length] + code) & 255];
		}
	s->bad = 1;
	return 0;
}

static inline int
img_extend(int v, int bits)
{
	return bits && v < (1 << (bits - 1)) ? v - (1 << bits) + 1 : v;
}

/*
 * Stage 1: the scan's codes into the list. A block is a count, then for each coefficient
 * that is not zero its place in the block (as the file orders them) and its value: three
 * bytes each, where the block itself would be 128 written.
 */
static uint32_t
img_jpeg_codes(uint32_t unused0, uint32_t unused1)
{
	struct img_job *j = img;
	struct img_scan s;
	uint8_t *out = j->list, *end = j->list + j->list_room - 4096;
	int dc[3] = {0, 0, 0}, my, mx, c, by, bx, k, left = j->restart;

	(void)unused0, (void)unused1;
	memset(&s, 0, sizeof s);
	s.at = j->file + j->scan_at;
	s.end = j->file + j->size;
	for (my = 0; my < j->mcus_y && !s.bad; my++) {
		j->strip_at[my] = (uint32_t)(out - j->list);
		for (mx = 0; mx < j->mcus_x && !s.bad; mx++) {
			if (j->restart && left-- == 0) {
				/* a restart: to the next byte, past the marker, the predictions at zero */
				s.count = 0;
				s.hold = 0;
				while (s.at + 1 < s.end && !(s.at[0] == 0xff && s.at[1] >= 0xd0 && s.at[1] <= 0xd7))
					s.at++;
				s.at += 2;
				dc[0] = dc[1] = dc[2] = 0;
				left = j->restart - 1;
			}
			for (c = 0; c < j->components; c++)
				for (by = 0; by < j->comp[c].v; by++)
					for (bx = 0; bx < j->comp[c].h; bx++) {
						uint8_t *count = out++;
						int bits = img_huff_symbol(&s, &j->huff[0][j->comp[c].dc]), n = 0;

						dc[c] += img_extend(img_scan_bits(&s, bits), bits);
						if (dc[c]) {
							out[0] = 0, out[1] = (uint8_t)dc[c], out[2] = (uint8_t)(dc[c] >> 8);
							out += 3, n++;
						}
						for (k = 1; k < 64; k++) {
							int symbol = img_huff_symbol(&s, &j->huff[1][j->comp[c].ac]), run = symbol >> 4, size = symbol & 15, v;

							if (!size) {
								if (run != 15)
									break;
								k += 15;
								continue;
							}
							k += run;
							v = img_extend(img_scan_bits(&s, size), size);
							if (k < 64) {
								out[0] = (uint8_t)k, out[1] = (uint8_t)v, out[2] = (uint8_t)(v >> 8);
								out += 3, n++;
							}
						}
						*count = (uint8_t)n;
					}
			if (out > end)
				s.bad = 1;
		}
		j->strip_at[my + 1] = (uint32_t)(out - j->list);
		*j->listed = (uint32_t)my + 1;
	}
	if (s.bad)
		*j->failed = 1;
	*j->listed = (uint32_t)j->mcus_y;
	return (uint32_t)(out - j->list);
}

/* ---------------------------------------------------------------- JPEG: the transform, the colours */

static const uint8_t img_zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

#define IMG_W1 2841
#define IMG_W2 2676
#define IMG_W3 2408
#define IMG_W5 1609
#define IMG_W6 1108
#define IMG_W7 565

static inline uint8_t
img_clip(int v)
{
	return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* The inverse transform of one block, in integers (as the small public decoders do it). */
static void
img_idct(int *b, uint8_t *out, int pitch)
{
	int i, x0, x1, x2, x3, x4, x5, x6, x7, x8;

	for (i = 0; i < 64; i += 8) {
		int *r = b + i;

		if (!((x1 = r[4] << 11) | (x2 = r[6]) | (x3 = r[2]) | (x4 = r[1]) | (x5 = r[7]) | (x6 = r[5]) | (x7 = r[3]))) {
			r[0] = r[1] = r[2] = r[3] = r[4] = r[5] = r[6] = r[7] = r[0] << 3;
			continue;
		}
		x0 = (r[0] << 11) + 128;
		x8 = IMG_W7 * (x4 + x5);
		x4 = x8 + (IMG_W1 - IMG_W7) * x4;
		x5 = x8 - (IMG_W1 + IMG_W7) * x5;
		x8 = IMG_W3 * (x6 + x7);
		x6 = x8 - (IMG_W3 - IMG_W5) * x6;
		x7 = x8 - (IMG_W3 + IMG_W5) * x7;
		x8 = x0 + x1;
		x0 -= x1;
		x1 = IMG_W6 * (x3 + x2);
		x2 = x1 - (IMG_W2 + IMG_W6) * x2;
		x3 = x1 + (IMG_W2 - IMG_W6) * x3;
		x1 = x4 + x6;
		x4 -= x6;
		x6 = x5 + x7;
		x5 -= x7;
		x7 = x8 + x3;
		x8 -= x3;
		x3 = x0 + x2;
		x0 -= x2;
		x2 = (181 * (x4 + x5) + 128) >> 8;
		x4 = (181 * (x4 - x5) + 128) >> 8;
		r[0] = (x7 + x1) >> 8, r[1] = (x3 + x2) >> 8, r[2] = (x0 + x4) >> 8, r[3] = (x8 + x6) >> 8;
		r[4] = (x8 - x6) >> 8, r[5] = (x0 - x4) >> 8, r[6] = (x3 - x2) >> 8, r[7] = (x7 - x1) >> 8;
	}
	for (i = 0; i < 8; i++, out++) {
		int *c = b + i;

		if (!((x1 = c[8 * 4] << 8) | (x2 = c[8 * 6]) | (x3 = c[8 * 2]) | (x4 = c[8 * 1]) | (x5 = c[8 * 7]) | (x6 = c[8 * 5]) | (x7 = c[8 * 3]))) {
			uint8_t v = img_clip(((c[0] + 32) >> 6) + 128);
			int k;

			for (k = 0; k < 8; k++)
				out[k * pitch] = v;
			continue;
		}
		x0 = (c[0] << 8) + 8192;
		x8 = IMG_W7 * (x4 + x5) + 4;
		x4 = (x8 + (IMG_W1 - IMG_W7) * x4) >> 3;
		x5 = (x8 - (IMG_W1 + IMG_W7) * x5) >> 3;
		x8 = IMG_W3 * (x6 + x7) + 4;
		x6 = (x8 - (IMG_W3 - IMG_W5) * x6) >> 3;
		x7 = (x8 - (IMG_W3 + IMG_W5) * x7) >> 3;
		x8 = x0 + x1;
		x0 -= x1;
		x1 = IMG_W6 * (x3 + x2) + 4;
		x2 = (x1 - (IMG_W2 + IMG_W6) * x2) >> 3;
		x3 = (x1 + (IMG_W2 - IMG_W6) * x3) >> 3;
		x1 = x4 + x6;
		x4 -= x6;
		x6 = x5 + x7;
		x5 -= x7;
		x7 = x8 + x3;
		x8 -= x3;
		x3 = x0 + x2;
		x0 -= x2;
		x2 = (181 * (x4 + x5) + 128) >> 8;
		x4 = (181 * (x4 - x5) + 128) >> 8;
		out[0 * pitch] = img_clip(((x7 + x1) >> 14) + 128), out[1 * pitch] = img_clip(((x3 + x2) >> 14) + 128);
		out[2 * pitch] = img_clip(((x0 + x4) >> 14) + 128), out[3 * pitch] = img_clip(((x8 + x6) >> 14) + 128);
		out[4 * pitch] = img_clip(((x8 - x6) >> 14) + 128), out[5 * pitch] = img_clip(((x0 - x4) >> 14) + 128);
		out[6 * pitch] = img_clip(((x3 - x2) >> 14) + 128), out[7 * pitch] = img_clip(((x7 - x1) >> 14) + 128);
	}
}

/*
 * Stage 2, on every core that has no other: strips of 16 rows of pixels, every sharers-th
 * one from `first` on. A strip's first and last bytes are on 16-byte boundaries of the
 * picture (16 rows of 3 bytes a pixel are a multiple of 16 bytes whatever the width), so two
 * cores never write the same 16 bytes.
 */
static uint32_t
img_jpeg_strips(uint32_t first, uint32_t unused)
{
	struct img_job *j = img;
	int rows_a_strip = 16 / j->mcu_h, strip, done = 0;	/* MCU rows in a strip: 1 or 2 */
	uint8_t plane[3][16 * 16];	/* one MCU's samples, each component at its own size */
	int block[64];

	(void)unused;
	for (strip = (int)first; strip < j->strips; strip += j->sharers, done++) {
		int my0 = strip * rows_a_strip, my1 = my0 + rows_a_strip, my, mx, c, by, bx, k, x, y;

		if (my1 > j->mcus_y)
			my1 = j->mcus_y;
		if (!img_wait(j->listed, (uint32_t)my1))
			break;
		for (my = my0; my < my1; my++) {
			const uint8_t *in = j->list + j->strip_at[my];

			for (mx = 0; mx < j->mcus_x; mx++) {
				for (c = 0; c < j->components; c++)
					for (by = 0; by < j->comp[c].v; by++)
						for (bx = 0; bx < j->comp[c].h; bx++) {
							const uint16_t *q = j->quant[j->comp[c].table];
							int n = *in++;

							memset(block, 0, sizeof block);
							for (k = 0; k < n; k++, in += 3)
								block[img_zigzag[in[0]]] = (int16_t)(in[1] | in[2] << 8) * q[in[0]];
							img_idct(block, plane[c] + by * 8 * 16 + bx * 8, 16);
						}
				/* the MCU's pixels, each component stretched to the MCU's size */
				{
					int most_x = j->w - mx * j->mcu_w, sx[3], sy[3];

					if (most_x > j->mcu_w)
						most_x = j->mcu_w;
					for (c = 0; c < j->components; c++) {
						sx[c] = j->comp[c].h * 8 == j->mcu_w ? 0 : 1;
						sy[c] = j->comp[c].v * 8 == j->mcu_h ? 0 : 1;
					}
					for (y = 0; y < j->mcu_h; y++) {
						int py = my * j->mcu_h + y;
						const uint8_t *l = plane[0] + (y >> sy[0]) * 16;
						uint8_t *out;

						if (py >= j->h)
							break;
						out = j->rgb + ((uint32_t)py * (uint32_t)j->w + (uint32_t)(mx * j->mcu_w)) * 3;
						if (j->components == 3) {
							const uint8_t *u = plane[1] + (y >> sy[1]) * 16, *v = plane[2] + (y >> sy[2]) * 16;

							for (x = 0; x < most_x; x++, out += 3) {
								int lum = l[x >> sx[0]], cb = u[x >> sx[1]] - 128, cr = v[x >> sx[2]] - 128;

								out[0] = img_clip(lum + ((91881 * cr + 32768) >> 16));
								out[1] = img_clip(lum - ((22554 * cb + 46802 * cr + 32768) >> 16));
								out[2] = img_clip(lum + ((116130 * cb + 32768) >> 16));
							}
						} else {
							for (x = 0; x < most_x; x++, out += 3)
								out[0] = out[1] = out[2] = l[x];
						}
					}
				}
			}
		}
	}
	return (uint32_t)done;
}

/* ---------------------------------------------------------------- the files' headers */

static const char *
img_png_header(struct img_job *j, uint32_t *first_idat)
{
	const uint8_t *f = j->file;
	uint32_t at = 8;

	*first_idat = 0;
	if (j->size < 33 || img_be32(f + 8) != 13 || memcmp(f + 12, "IHDR", 4) != 0)
		return "not a PNG picture";
	j->w = (int)img_be32(f + 16);
	j->h = (int)img_be32(f + 20);
	j->depth = f[24];
	j->type = f[25];
	if (f[28] != 0)
		return "an interlaced PNG picture, which this does not read";
	j->channels = j->type == 2 ? 3 : j->type == 4 ? 2 : j->type == 6 ? 4 : 1;
	if ((j->type != 0 && j->type != 2 && j->type != 3 && j->type != 4 && j->type != 6) ||
	    (j->depth != 8 && j->depth != 16 && !(j->depth < 8 && (j->type == 0 || j->type == 3))) || (j->type == 3 && j->depth == 16))
		return "a PNG picture of a kind this does not read";
	j->row_bytes = (j->w * j->channels * j->depth + 7) / 8;
	j->pixel_bytes = j->channels * j->depth / 8 ? j->channels * j->depth / 8 : 1;
	while (at + 12 <= j->size) {
		uint32_t length = img_be32(f + at);

		if (at + 12 + length > j->size)
			break;
		if (!memcmp(f + at + 4, "PLTE", 4)) {
			uint32_t i;

			for (i = 0; i < length / 3 && i < 256; i++)
				memcpy(j->palette[i], f + at + 8 + 3 * i, 3);
		} else if (!memcmp(f + at + 4, "IDAT", 4)) {
			*first_idat = at;
			return NULL;
		}
		at += 12 + length;
	}
	return "a PNG picture with nothing in it";
}

static const char *
img_jpeg_header(struct img_job *j)
{
	const uint8_t *f = j->file;
	uint32_t at = 2;
	int i, k, hmax = 1, vmax = 1;

	while (at + 4 <= j->size) {
		int marker, length;

		if (f[at] != 0xff)
			return "a JPEG picture that is damaged";
		marker = f[at + 1];
		length = f[at + 2] << 8 | f[at + 3];
		if (at + 2 + (uint32_t)length > j->size)
			break;
		if (marker == 0xc0 || marker == 0xc1) {
			if (f[at + 4] != 8)
				return "a JPEG picture of a kind this does not read";
			j->h = f[at + 5] << 8 | f[at + 6];
			j->w = f[at + 7] << 8 | f[at + 8];
			j->components = f[at + 9];
			if (j->components != 1 && j->components != 3)
				return "a JPEG picture of four colours, which this does not read";
			for (i = 0; i < j->components; i++) {
				j->comp[i].h = f[at + 11 + 3 * i] >> 4;
				j->comp[i].v = f[at + 11 + 3 * i] & 15;
				j->comp[i].table = f[at + 12 + 3 * i] & 3;
				if (j->comp[i].h < 1 || j->comp[i].h > 2 || j->comp[i].v < 1 || j->comp[i].v > 2)
					return "a JPEG picture of a kind this does not read";
				if (j->comp[i].h > hmax) hmax = j->comp[i].h;
				if (j->comp[i].v > vmax) vmax = j->comp[i].v;
			}
			if (j->components == 1)
				j->comp[0].h = j->comp[0].v = hmax = vmax = 1;
		} else if (marker == 0xc2) {
			return "a progressive JPEG picture, which this does not read";
		} else if (marker >= 0xc3 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			return "a JPEG picture of a kind this does not read";
		} else if (marker == 0xdb) {
			for (k = 4; k < length + 2;) {
				int wide = f[at + k] >> 4, t = f[at + k] & 3;

				k++;
				for (i = 0; i < 64; i++, k += wide ? 2 : 1)
					j->quant[t][i] = wide ? (uint16_t)(f[at + k] << 8 | f[at + k + 1]) : f[at + k];
			}
		} else if (marker == 0xc4) {
			for (k = 4; k < length + 2;) {
				struct img_huff *h = &j->huff[f[at + k] >> 4 & 1][f[at + k] & 1];
				int n = 0;

				k++;
				h->length[0] = 0;
				for (i = 1; i <= 16; i++)
					n += h->length[i] = f[at + k++];
				for (i = 0; i < n && i < 256; i++)
					h->symbol[i] = f[at + k + i];
				k += n;
				img_huff_make(h);
			}
		} else if (marker == 0xdd) {
			j->restart = f[at + 4] << 8 | f[at + 5];
		} else if (marker == 0xda) {
			int n = f[at + 4];

			if (!j->w || n != j->components)
				return "a JPEG picture of a kind this does not read";
			for (i = 0; i < n; i++) {
				j->comp[i].dc = f[at + 6 + 2 * i] >> 4 & 1;
				j->comp[i].ac = f[at + 6 + 2 * i] & 1;
			}
			j->scan_at = at + 2 + (uint32_t)length;
			j->mcu_w = 8 * hmax;
			j->mcu_h = 8 * vmax;
			j->mcus_x = (j->w + j->mcu_w - 1) / j->mcu_w;
			j->mcus_y = (j->h + j->mcu_h - 1) / j->mcu_h;
			return NULL;
		}
		at += 2 + (uint32_t)length;
	}
	return "a JPEG picture with nothing in it";
}

/* Memory of this core's or a worker's own, there already (a worker stops at a page that is not). */
static void *
img_memory(uint32_t bytes)
{
	void *m = mmap(NULL, (bytes + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (m == MAP_FAILED)
		return NULL;
	mcw_touch(m, bytes);
	return m;
}

/*
 * Reads a PNG or JPEG file and writes it as a PPM file. NULL when that went well, with the
 * picture's size; else what was wrong. `workers` is how many worker cores it may ask for
 * (3 is what it has use for now; more share a JPEG's strips).
 */
static const char *
ui_image_to_ppm(const char *path, const char *ppm_path, int *pw, int *ph, int workers)
{
	static struct img_job job;
	struct img_job *j = &job;
	struct stat info;
	const char *wrong = NULL;
	char header[64];
	uint32_t first_idat = 0, i, pixels, mapped;
	int in = open(path, O_RDONLY), out = -1, got = 0, head, png, k;
	uint8_t *map = MAP_FAILED;
	volatile uint32_t sum = 0;

	memset(j, 0, sizeof *j);
	img = j;
	if (in < 0 || fstat(in, &info) != 0 || info.st_size < 16)
		return "the file cannot be read";
	j->size = (uint32_t)info.st_size;
	j->file = mmap(NULL, j->size, PROT_READ, MAP_PRIVATE, in, 0);
	close(in);
	if (j->file == MAP_FAILED)
		return "the file cannot be read";
	png = !memcmp(j->file, "\x89PNG\r\n\x1a\n", 8);
	if (png)
		wrong = img_png_header(j, &first_idat);
	else if (j->file[0] == 0xff && j->file[1] == 0xd8)
		wrong = img_jpeg_header(j);
	else
		wrong = "neither a PNG nor a JPEG picture";
	if (!wrong && (j->w < 1 || j->h < 1 || j->w > 4096 || j->h > 4096))
		wrong = "a picture too large for this machine";
	if (wrong)
		goto done;
	/* every page of the file there before a worker reads it */
	for (i = 0; i < j->size; i += 4096)
		sum += j->file[i];
	/* the PPM: its header made a multiple of 16 bytes long, so that the pixels begin on one */
	head = snprintf(header, sizeof header, "P6\n%d %d\n", j->w, j->h);
	while ((head + 4) & 15)
		header[head++] = ' ';
	memcpy(header + head, "255\n", 4);
	head += 4;
	pixels = (uint32_t)j->w * (uint32_t)j->h * 3;
	mapped = (uint32_t)head + pixels;
	out = open(ppm_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (out < 0 || ftruncate(out, mapped) != 0 || (map = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, out, 0)) == MAP_FAILED) {
		wrong = "the picture cannot be written";
		goto done;
	}
	memcpy(map, header, head);
	j->rgb = map + head;
	mcw_touch(map, mapped);
	j->failed = mcw_alloc(16);
	*j->failed = 0;
	/* no more than there is work for: a PNG is three steps, a JPEG its codes and its strips of 16 rows */
	if (workers > (png ? 3 : (j->h + 15) / 16 + 1))
		workers = png ? 3 : (j->h + 15) / 16 + 1;
	if (workers > 0)
		got = mcw_open(workers);
	if (png) {
		j->row_pitch = (j->row_bytes + 15) & ~15;
		j->raw = img_memory((uint32_t)(j->row_bytes + 1) * (uint32_t)j->h + 16);
		j->rows = img_memory((uint32_t)j->row_pitch * (uint32_t)j->h);
		j->inflated = mcw_alloc(16);
		j->unfiltered = mcw_alloc(16);
		j->codes = img_memory(3 * sizeof(struct img_codes));
		if (!j->raw || !j->rows || !j->codes) {
			wrong = "not enough memory for the picture";
			goto done;
		}
		*j->inflated = *j->unfiltered = 0;
		if (got >= 3) {
			mcw_post(1, img_png_inflate, first_idat, 0);
			mcw_post(2, img_png_unfilter, 0, 0);
			mcw_post(3, img_png_pixels, 0, 0);
			for (k = 1; k <= 3; k++)
				mcw_wait(k);
		} else {
			img_png_inflate(first_idat, 0);
			img_png_unfilter(0, 0);
			img_png_pixels(0, 0);
		}
	} else {
		uint32_t blocks = 0;

		for (k = 0; k < j->components; k++)
			blocks += (uint32_t)(j->comp[k].h * j->comp[k].v);
		blocks *= (uint32_t)(j->mcus_x * j->mcus_y);
		/* a coefficient that is not zero is at least two bits of the file */
		j->list_room = 12 * j->size + blocks + 8192;
		j->list = mmap(NULL, (j->list_room + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		j->strip_at = img_memory((uint32_t)(j->mcus_y + 1) * 4 + 16);
		j->listed = mcw_alloc(16);
		if (j->list == MAP_FAILED || !j->strip_at) {
			wrong = "not enough memory for the picture";
			goto done;
		}
		/* (the list is seldom a quarter of that: so much is made to be there, the rest when it is stepped on) */
		mcw_touch(j->list, j->list_room / 4 < 3 * j->size + blocks + 4096 ? j->list_room / 4 : 3 * j->size + blocks + 4096);
		*j->listed = 0;
		j->strips = (j->mcus_y * j->mcu_h + 15) / 16;
		if (got >= 2) {
			j->sharers = got - 1;
			mcw_post(1, img_jpeg_codes, 0, 0);
			for (k = 2; k <= got; k++)
				mcw_post(k, img_jpeg_strips, (uint32_t)(k - 2), 0);
			for (k = 1; k <= got; k++)
				mcw_wait(k);
		} else {
			j->sharers = 1;
			img_jpeg_codes(0, 0);
			img_jpeg_strips(0, 0);
		}
	}
	if (*j->failed)
		wrong = "the picture is damaged";
	*pw = j->w;
	*ph = j->h;
done:
	if (got)
		mcw_close();
	if (map != MAP_FAILED)
		munmap(map, mapped);
	if (out >= 0)
		close(out);
	if (wrong && out >= 0)
		unlink(ppm_path);
	munmap((void *)j->file, j->size);
	(void)sum;
	return wrong;
}

#endif
