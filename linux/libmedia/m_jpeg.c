/*
 * JPEG pictures on the worker cores (docs/system/media.md). Three ways, by what the file allows:
 *
 *   restart markers   every core takes strips of 16 rows and reads their codes itself, from
 *                     the interval each begins in: the whole picture in parallel. The markers
 *                     are found by every core too, each a part of the file.
 *   baseline          this core reads the codes into a short list of the coefficients that
 *                     are not zero; the workers take strips as their codes are there.
 *   progressive       this core reads every scan into the coefficients; then strips everywhere.
 *
 * What a core writes is only its strips' pixels (16 rows: whole 16 bytes whatever the width).
 * Not read: arithmetic coding, 12-bit samples, lossless.
 */
#include <stdlib.h>
#include <sys/mman.h>
#include "m_int.h"

struct scan { const uint8_t *at, *end; uint32_t hold; int count; int bad; };
/* An MCU row's codes: where they begin in the file, and the predictions and the restart count there. */
struct row_at { const uint8_t *at; uint32_t hold; int count, left; int pred[4]; };

struct huff { uint8_t length[17]; uint8_t symbol[256]; uint16_t fast[512]; int16_t whole[512]; int32_t max[18]; int32_t first[17]; };

struct jpg {
	const uint8_t *file;
	uint32_t size;
	struct media_image *out;
	int w, h, components, progressive, adobe, transform;
	int mcu_w, mcu_h, mcus_x, mcus_y, restart, hmax, vmax;
	int shrink, bs, ow, oh, omcu_w, omcu_h;	/* the output: 1, 2, 4 or 8 times smaller; a block's side there */
	int no_tables;		/* a probe: the codes' tables are not made */
	struct { int id, h, v, table, dc, ac, bw, bh; int16_t *coef; } comp[4];
	uint16_t quant[4][64];
	struct huff huff[2][4];
	uint32_t scan_at;
	volatile uint32_t *failed;
	/* baseline: the list of coefficients */
	uint32_t *list, list_room, *strip_at;
	struct row_at *rows_at;	/* or where each MCU row's codes begin (the workers read them) */
	volatile uint32_t *listed;
	int strips, sharers;
	/* restart markers: where each interval's codes begin */
	uint32_t *interval_at, intervals;
	uint32_t *found;	/* each part's markers, found_most words apart */
	uint32_t *found_n, parts, part_bytes, found_most;
};

static struct jpg *J;
static uint8_t clip_of[1024];	/* a sample plus a colour term (-384 to 639), clipped */
/* each chroma byte's terms: red's and blue's added, green's two summed then shifted */
static int16_t cr_r[256], cb_b[256];
static int32_t cb_g[256], cr_g[256];

static void
colour_tables(void)
{
	static int made;
	int k;

	if (made)
		return;
	made = 1;
	for (k = 0; k < 1024; k++)
		clip_of[k] = m_clip(k - 384);
	for (k = 0; k < 256; k++) {
		cr_r[k] = (int16_t)((91881 * (k - 128) + 32768) >> 16);
		cb_b[k] = (int16_t)((116130 * (k - 128) + 32768) >> 16);
		cb_g[k] = 22554 * (k - 128);
		cr_g[k] = 46802 * (k - 128) + 32768;
	}
}

/* ---------------------------------------------------------------- the codes */

static inline void
scan_fill(struct scan *s)
{
	while (s->count <= 24) {
		int byte = 0;

		/* two bytes at once where neither can be a marker's */
		if (s->count <= 16 && s->at + 2 <= s->end && s->at[0] != 0xff && s->at[1] != 0xff) {
			s->hold |= (uint32_t)(s->at[0] << 8 | s->at[1]) << (16 - s->count);
			s->count += 16;
			s->at += 2;
			continue;
		}
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
scan_bits(struct scan *s, int n)
{
	int v;

	if (!n)
		return 0;
	if (s->count < n)
		scan_fill(s);
	v = (int)(s->hold >> (32 - n));
	s->hold <<= n;
	s->count -= n;
	return v;
}

static void
huff_make(struct huff *h)
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
	for (i = 0; i < 512; i++) {
		int fast = h->fast[i], run = (fast & 255) >> 4, size = fast & 15, taken = fast >> 8;

		h->whole[i] = 0;
		if (fast && size && taken + size <= 9) {
			int v = (i << taken & 511) >> (9 - size);

			if (v < 1 << (size - 1))
				v += 1 - (1 << size);
			if (v >= -128 && v <= 127)
				h->whole[i] = (int16_t)(v * 256 + run * 16 + taken + size);
		}
	}
}

static inline int
huff_symbol(struct scan *s, const struct huff *h)
{
	int code, length;
	uint16_t fast;

	if (s->count < 16)
		scan_fill(s);
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
extend(int v, int bits)
{
	return bits && v < (1 << (bits - 1)) ? v - (1 << bits) + 1 : v;
}

/*
 * One block's codes. With `list` the coefficients that are not zero go there (a word each:
 * value << 8 | place), else into `block` dequantised (place in the natural order). The count.
 */
static const uint8_t zigzag[64 + 16] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,	/* (a run past the end lands here) */
};

static inline int
block_codes(struct scan *s, const struct huff *dc, const struct huff *ac, int *pred, uint32_t *list)
{
	uint32_t *out = list;
	int bits = huff_symbol(s, dc), k, v;

	*pred += extend(scan_bits(s, bits), bits);
	if (*pred)
		*out++ = (uint32_t)*pred << 8;
	for (k = 1; k < 64; k++) {
		int symbol, run, size;

		if (s->count < 16)
			scan_fill(s);
		/* a short code and its value in one look */
		v = ac->whole[s->hold >> 23];
		if (v) {
			k += v >> 4 & 15;
			s->hold <<= v & 15;
			s->count -= v & 15;
			if (k < 64)
				*out++ = (uint32_t)(v >> 8) << 8 | (uint32_t)k;
			continue;
		}
		symbol = huff_symbol(s, ac), run = symbol >> 4, size = symbol & 15;
		if (!size) {
			if (run != 15)
				break;
			k += 15;
			continue;
		}
		k += run;
		v = extend(scan_bits(s, size), size);
		if (k < 64)
			*out++ = (uint32_t)v << 8 | (uint32_t)k;
	}
	return (int)(out - list);
}

/* A block's codes read, nothing kept but the prediction. */
static inline void
block_skip(struct scan *s, const struct huff *dc, const struct huff *ac, int *pred)
{
	int bits = huff_symbol(s, dc), k, v;

	*pred += extend(scan_bits(s, bits), bits);
	for (k = 1; k < 64; k++) {
		int symbol, size;

		if (s->count < 16)
			scan_fill(s);
		v = ac->whole[s->hold >> 23];
		if (v) {
			k += v >> 4 & 15;
			s->hold <<= v & 15;
			s->count -= v & 15;
			continue;
		}
		symbol = huff_symbol(s, ac), size = symbol & 15;
		if (!size) {
			if (symbol >> 4 != 15)
				break;
			k += 15;
			continue;
		}
		k += symbol >> 4;
		scan_bits(s, size);
	}
}

/* ---------------------------------------------------------------- the transform, the colours */

#define W1 2841
#define W2 2676
#define W3 2408
#define W5 1609
#define W6 1108
#define W7 565

static void
idct(int *b, uint8_t *out, int pitch)
{
	int i, x0, x1, x2, x3, x4, x5, x6, x7, x8;

	for (i = 0; i < 64; i += 8) {
		int *r = b + i;

		if (!((x1 = r[4] << 11) | (x2 = r[6]) | (x3 = r[2]) | (x4 = r[1]) | (x5 = r[7]) | (x6 = r[5]) | (x7 = r[3]))) {
			r[0] = r[1] = r[2] = r[3] = r[4] = r[5] = r[6] = r[7] = r[0] << 3;
			continue;
		}
		x0 = (r[0] << 11) + 128;
		x8 = W7 * (x4 + x5);
		x4 = x8 + (W1 - W7) * x4;
		x5 = x8 - (W1 + W7) * x5;
		x8 = W3 * (x6 + x7);
		x6 = x8 - (W3 - W5) * x6;
		x7 = x8 - (W3 + W5) * x7;
		x8 = x0 + x1;
		x0 -= x1;
		x1 = W6 * (x3 + x2);
		x2 = x1 - (W2 + W6) * x2;
		x3 = x1 + (W2 - W6) * x3;
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
			uint8_t v = m_clip(((c[0] + 32) >> 6) + 128);
			int k;

			for (k = 0; k < 8; k++)
				out[k * pitch] = v;
			continue;
		}
		x0 = (c[0] << 8) + 8192;
		x8 = W7 * (x4 + x5) + 4;
		x4 = (x8 + (W1 - W7) * x4) >> 3;
		x5 = (x8 - (W1 + W7) * x5) >> 3;
		x8 = W3 * (x6 + x7) + 4;
		x6 = (x8 - (W3 - W5) * x6) >> 3;
		x7 = (x8 - (W3 + W5) * x7) >> 3;
		x8 = x0 + x1;
		x0 -= x1;
		x1 = W6 * (x3 + x2) + 4;
		x2 = (x1 - (W2 + W6) * x2) >> 3;
		x3 = (x1 + (W2 - W6) * x3) >> 3;
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
		out[0 * pitch] = m_clip(((x7 + x1) >> 14) + 128), out[1 * pitch] = m_clip(((x3 + x2) >> 14) + 128);
		out[2 * pitch] = m_clip(((x0 + x4) >> 14) + 128), out[3 * pitch] = m_clip(((x8 + x6) >> 14) + 128);
		out[4 * pitch] = m_clip(((x8 - x6) >> 14) + 128), out[5 * pitch] = m_clip(((x0 - x4) >> 14) + 128);
		out[6 * pitch] = m_clip(((x3 - x2) >> 14) + 128), out[7 * pitch] = m_clip(((x7 - x1) >> 14) + 128);
	}
}

/* A transformed block's samples brought to bs x bs: each the mean of the samples under it. */
static void
block_shrunk(int *block, uint8_t *to, int bs)
{
	uint8_t full[64];
	int k = 8 / bs, half = k * k / 2, x, y, i, j;

	idct(block, full, 8);
	for (y = 0; y < bs; y++, to += 16)
		for (x = 0; x < bs; x++) {
			int sum = half;

			for (j = 0; j < k; j++)
				for (i = 0; i < k; i++)
					sum += full[(y * k + j) * 8 + x * k + i];
			to[x] = (uint8_t)(sum / (k * k));
		}
}

/* A block from its list (value << 8 | place) into bs x bs samples (8: the block itself); only
 * the mean is worked out when there is nothing else, or when one sample is all that is asked. */
static inline void
list_block(const uint32_t *in, int n, const uint16_t *q, uint8_t *to, int bs)
{
	int block[64], k;

	if (n == 0 || (n == 1 && !(in[0] & 255)) || bs == 1) {
		int mean = n && !(in[0] & 255) ? ((int32_t)in[0] >> 8) * q[0] << 3 : 0;
		uint8_t v = m_clip(((mean + 32) >> 6) + 128);

		for (k = 0; k < bs; k++, to += 16)
			memset(to, v, bs);
		return;
	}
	memset(block, 0, sizeof block);
	for (k = 0; k < n; k++)
		block[zigzag[in[k] & 255]] = ((int32_t)in[k] >> 8) * q[in[k] & 255];
	if (bs == 8)
		idct(block, to, 16);
	else
		block_shrunk(block, to, bs);
}

/* One MCU's samples (each component at its own size, 16 a row) into pixels of the output. */
static void
mcu_pixels(const struct jpg *j, uint8_t plane[4][16 * 16], int mx, int my)
{
	const struct media_image *o = j->out;
	int most_x = j->ow - mx * j->omcu_w, sx[4], sy[4], c, x, y, ch = o->channels;
	int rgb3 = ch == 3, swap = ch == 4 && o->bgr, words = ch == 4 && !(((uintptr_t)o->pixels | (uint32_t)o->stride) & 3);

	if (most_x > j->omcu_w)
		most_x = j->omcu_w;
	for (c = 0; c < j->components; c++) {
		sx[c] = j->comp[c].h == j->hmax ? 0 : j->hmax / j->comp[c].h == 2 ? 1 : 2;
		sy[c] = j->comp[c].v == j->vmax ? 0 : j->vmax / j->comp[c].v == 2 ? 1 : 2;
	}
	for (y = 0; y < j->omcu_h; y++) {
		int py = my * j->omcu_h + y;
		const uint8_t *l = plane[0] + (y >> sy[0]) * 16;
		uint8_t *out;

		if (py >= j->oh)
			break;
		out = o->pixels + (uint32_t)py * (uint32_t)o->stride + (uint32_t)(mx * j->omcu_w) * (uint32_t)ch;
		if (j->components == 3 && j->transform) {
			const uint8_t *u = plane[1] + (y >> sy[1]) * 16, *v = plane[2] + (y >> sy[2]) * 16;

			x = 0;
			if (!sx[0] && sx[1] == 1 && sx[2] == 1 && (rgb3 || words)) {
				/* a colour sample for two pixels (the usual kind): its three terms once */
				for (; x < most_x; x += 2) {
					int cb = u[x >> 1], cr = v[x >> 1], lum = l[x], two = x + 1 < most_x;
					const uint8_t *r = clip_of + 384 + cr_r[cr], *g = clip_of + 384 - ((cb_g[cb] + cr_g[cr]) >> 16);
					const uint8_t *b = clip_of + 384 + cb_b[cb];

					if (rgb3) {
						out[0] = r[lum], out[1] = g[lum], out[2] = b[lum];
						if (two)
							lum = l[x + 1], out[3] = r[lum], out[4] = g[lum], out[5] = b[lum];
						out += 6;
					} else {
						/* a word a pixel (the output's rows are on 4 bytes) */
						uint32_t *w = (uint32_t *)out;

						w[0] = swap ? 0xff000000u | r[lum] << 16 | g[lum] << 8 | b[lum] : 0xff000000u | b[lum] << 16 | g[lum] << 8 | r[lum];
						if (two)
							lum = l[x + 1], w[1] = swap ? 0xff000000u | r[lum] << 16 | g[lum] << 8 | b[lum]
										: 0xff000000u | b[lum] << 16 | g[lum] << 8 | r[lum];
						out += 8;
					}
				}
				continue;
			}
			for (; x < most_x; x++, out += ch) {
				int lum = l[x >> sx[0]], cb = u[x >> sx[1]], cr = v[x >> sx[2]];
				int r = clip_of[384 + lum + cr_r[cr]];
				int g = clip_of[384 + lum - ((cb_g[cb] + cr_g[cr]) >> 16)];
				int b = clip_of[384 + lum + cb_b[cb]];

				if (rgb3)
					out[0] = (uint8_t)r, out[1] = (uint8_t)g, out[2] = (uint8_t)b;
				else if (swap)
					out[0] = (uint8_t)b, out[1] = (uint8_t)g, out[2] = (uint8_t)r, out[3] = 255;
				else
					out[0] = (uint8_t)r, out[1] = (uint8_t)g, out[2] = (uint8_t)b, out[3] = 255;
			}
		} else if (j->components == 3) {
			const uint8_t *u = plane[1] + (y >> sy[1]) * 16, *v = plane[2] + (y >> sy[2]) * 16;

			for (x = 0; x < most_x; x++, out += ch)
				m_put(o, out, l[x >> sx[0]], u[x >> sx[1]], v[x >> sx[2]], 255);
		} else if (j->components == 4) {
			const uint8_t *u = plane[1] + (y >> sy[1]) * 16, *v = plane[2] + (y >> sy[2]) * 16, *k = plane[3] + (y >> sy[3]) * 16;

			for (x = 0; x < most_x; x++, out += ch) {
				int a = l[x >> sx[0]], b = u[x >> sx[1]], c = v[x >> sx[2]], kk = k[x >> sx[3]];

				if (j->transform == 2) {
					int cb = b - 128, cr = c - 128;

					/* YCCK: the colours of Y, Cb, Cr are the inks' complements */
					b = 255 - clip_of[384 + a - ((22554 * cb + 46802 * cr + 32768) >> 16)];
					c = 255 - clip_of[384 + a + ((116130 * cb + 32768) >> 16)];
					a = 255 - clip_of[384 + a + ((91881 * cr + 32768) >> 16)];
				}
				/* Adobe's CMYK is stored inverted: each colour is the ink's complement times the black's */
				m_put(o, out, (a * kk + 127) / 255, (b * kk + 127) / 255, (c * kk + 127) / 255, 255);
			}
		} else {
			for (x = 0; x < most_x; x++, out += ch)
				m_put(o, out, l[x], l[x], l[x], 255);
		}
	}
}

/* ---------------------------------------------------------------- strips: from the list (baseline) */

static void
strip_from_list(struct jpg *j, int my0, int my1)
{
	uint8_t plane[4][16 * 16];
	int my, mx, c, bx, by;

	for (my = my0; my < my1; my++) {
		const uint32_t *in = j->list + j->strip_at[my];

		for (mx = 0; mx < j->mcus_x; mx++) {
			for (c = 0; c < j->components; c++)
				for (by = 0; by < j->comp[c].v; by++)
					for (bx = 0; bx < j->comp[c].h; bx++) {
						int n = (int)*in++;

						list_block(in, n, j->quant[j->comp[c].table], plane[c] + by * j->bs * 16 + bx * j->bs, j->bs);
						in += n;
					}
			mcu_pixels(j, plane, mx, my);
		}
	}
}

/* Stage 1 (baseline, no markers): the scan's codes into the list, a count after each MCU row. */
static uint32_t
codes_job(uint32_t unused0, uint32_t unused1)
{
	struct jpg *j = J;
	struct scan s;
	uint32_t *out = j->list, *end = j->list + (j->list_room - 4096) / 4;
	int pred[4] = {0, 0, 0, 0}, my, mx, c, by, bx, left = j->restart;

	(void)unused0, (void)unused1;
	memset(&s, 0, sizeof s);
	s.at = j->file + j->scan_at;
	s.end = j->file + j->size;
	for (my = 0; my < j->mcus_y && !s.bad; my++) {
		j->strip_at[my] = (uint32_t)(out - j->list);
		if (!mcw_on_worker() && mcw_count())
			m_look();
		m_progress((uint32_t)my, (uint32_t)j->mcus_y);
		for (mx = 0; mx < j->mcus_x && !s.bad; mx++) {
			if (j->restart && left-- == 0) {
				s.count = 0, s.hold = 0;
				while (s.at + 1 < s.end && !(s.at[0] == 0xff && s.at[1] >= 0xd0 && s.at[1] <= 0xd7))
					s.at++;
				s.at += 2;
				pred[0] = pred[1] = pred[2] = pred[3] = 0;
				left = j->restart - 1;
			}
			for (c = 0; c < j->components; c++)
				for (by = 0; by < j->comp[c].v; by++)
					for (bx = 0; bx < j->comp[c].h; bx++) {
						uint32_t *count = out++;

						*count = (uint32_t)block_codes(&s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &pred[c], out);
						out += *count;
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

/* Stage 2: strips of 16 rows, every sharers-th from `first` on, as their codes are there. */
static uint32_t
strips_job(uint32_t first, uint32_t unused)
{
	struct jpg *j = J;
	int per = 16 / j->omcu_h, strip, done = 0;

	(void)unused;
	for (strip = (int)first; strip < j->strips; strip += j->sharers, done++) {
		int my0 = strip * per, my1 = my0 + per;

		if (my1 > j->mcus_y)
			my1 = j->mcus_y;
		if (!m_wait(j->listed, (uint32_t)my1, j->failed))
			break;
		if (!mcw_on_worker() && mcw_count())
			m_look();
		m_progress((uint32_t)my0, (uint32_t)j->mcus_y);
		strip_from_list(j, my0, my1);
	}
	return (uint32_t)done;
}

/* ---------------------------------------------------------------- no markers: the codes read twice */

/* This core: through the codes, keeping only where each MCU row begins (no store a coefficient,
 * which on the machine is what bounds it); the workers follow a row behind. */
static uint32_t
index_job(uint32_t unused0, uint32_t unused1)
{
	struct jpg *j = J;
	struct scan s;
	int pred[4] = {0, 0, 0, 0}, my, mx, c, by, bx, left = j->restart;

	(void)unused0, (void)unused1;
	memset(&s, 0, sizeof s);
	s.at = j->file + j->scan_at;
	s.end = j->file + j->size;
	for (my = 0; my < j->mcus_y && !s.bad; my++) {
		struct row_at *r = &j->rows_at[my];

		r->at = s.at, r->hold = s.hold, r->count = s.count, r->left = left;
		memcpy(r->pred, pred, sizeof pred);
		*j->listed = (uint32_t)my + 1;
		if (!mcw_on_worker() && mcw_count())
			m_look();
		m_progress((uint32_t)my, (uint32_t)j->mcus_y);
		for (mx = 0; mx < j->mcus_x && !s.bad; mx++) {
			if (j->restart && left-- == 0) {
				s.count = 0, s.hold = 0;
				while (s.at + 1 < s.end && !(s.at[0] == 0xff && s.at[1] >= 0xd0 && s.at[1] <= 0xd7))
					s.at++;
				s.at += 2;
				pred[0] = pred[1] = pred[2] = pred[3] = 0;
				left = j->restart - 1;
			}
			for (c = 0; c < j->components; c++)
				for (by = 0; by < j->comp[c].v; by++)
					for (bx = 0; bx < j->comp[c].h; bx++)
						block_skip(&s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &pred[c]);
		}
	}
	if (s.bad)
		*j->failed = 1;
	*j->listed = (uint32_t)j->mcus_y;
	return 0;
}

/* n MCUs in order from (mx, my), their codes read from where r says. */
static void
run_mcus(struct jpg *j, const struct row_at *r, int mx, int my, int n)
{
	uint8_t plane[4][16 * 16];
	uint32_t list[64 + 16];
	int c, bx, by;

	{
		int pred[4], left;
		struct scan s;

		memset(&s, 0, sizeof s);
		s.at = r->at, s.hold = r->hold, s.count = r->count, s.end = j->file + j->size;
		memcpy(pred, r->pred, sizeof pred);
		left = r->left;
		for (; n > 0; n--, mx = mx + 1 == j->mcus_x ? 0 : mx + 1, my += !mx) {
			{
				if (j->restart && left-- == 0) {
					s.count = 0, s.hold = 0;
					while (s.at + 1 < s.end && !(s.at[0] == 0xff && s.at[1] >= 0xd0 && s.at[1] <= 0xd7))
						s.at++;
					s.at += 2;
					pred[0] = pred[1] = pred[2] = pred[3] = 0;
					left = j->restart - 1;
				}
				for (c = 0; c < j->components; c++)
					for (by = 0; by < j->comp[c].v; by++)
						for (bx = 0; bx < j->comp[c].h; bx++) {
							int n = block_codes(&s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &pred[c], list);

							list_block(list, n, j->quant[j->comp[c].table], plane[c] + by * j->bs * 16 + bx * j->bs, j->bs);
						}
				mcu_pixels(j, plane, mx, my);
			}
		}
		if (s.bad)
			*j->failed = 1;
	}
}

/* One strip of 16 rows, read from where its first MCU row begins (its index there already). */
static void
indexed_strip(struct jpg *j, int strip)
{
	int per = 16 / j->omcu_h, my0 = strip * per, my1 = my0 + per;

	if (my1 > j->mcus_y)
		my1 = j->mcus_y;
	run_mcus(j, &j->rows_at[my0], 0, my0, (my1 - my0) * j->mcus_x);
}

/* Strips of 16 rows, every sharers-th from `first` on, each as its row's place is found. */
static uint32_t
indexed_strips(uint32_t first, uint32_t unused)
{
	struct jpg *j = J;
	int per = 16 / j->omcu_h, strip, done = 0;

	(void)unused;
	for (strip = (int)first; strip < j->strips; strip += j->sharers, done++) {
		if (!m_wait(j->listed, (uint32_t)(strip * per) + 1, j->failed))
			break;
		indexed_strip(j, strip);
	}
	return (uint32_t)done;
}

/* ---------------------------------------------------------------- no markers: every core reads from a guess */

/* Where the codes are, to the bit, and which block of an MCU is next there. */
struct spot { const uint8_t *at; int bit, phase; };

/* A part of the scan, its own 48 bytes: where reading from a guessed start left it, and from
 * the part before's end (`found`, with the blocks and the DC differences' sums between). */
struct part {
	struct spot guessed, found;
	uint32_t blocks;
	int dc[4];
	int bad;
} __attribute__((aligned(16)));

#define PARTS_MOST 256
#define ROW_PIECES 16

static int m_gcd(int a, int b) { while (b) { int t = a % b; a = b, b = t; } return a; }
static struct part *parts;
static const uint8_t *part_at[PARTS_MOST + 1];
static int part_count, mcu_blocks;
static int8_t block_comp[16];
static uint32_t part_first[PARTS_MOST + 1];	/* each part's first block, and the predictions there */
static int row_pieces, piece_mcus;	/* an MCU row in pieces of so many MCUs, each read by itself */
static int part_pred[PARTS_MOST + 1][4];

/* The next bit not yet read: as many data bytes back from `at` as the hold has bits (an 0xff's
 * byte of 0 is not one). */
static struct spot
spot_of(const struct scan *s, int phase)
{
	const uint8_t *p = s->at;
	int q = s->count;
	struct spot o;

	for (; q > 0; q -= 8)
		p -= p[-1] == 0 && p[-2] == 0xff ? 2 : 1;
	o.at = p, o.bit = -q, o.phase = phase;
	return o;
}

static void
scan_from(struct scan *s, const struct jpg *j, struct spot o)
{
	s->at = o.at, s->end = j->file + j->size, s->hold = 0, s->count = 0, s->bad = 0;
	scan_fill(s);
	s->hold <<= o.bit, s->count -= o.bit;
}

/* Blocks from `s` until one would begin at or past `stop`: where, how many, the DC sums. 0 when
 * the codes are not codes (a guess that has not met the stream yet) or there are too many. */
static int
read_part(const struct jpg *j, struct scan *s, int phase, const uint8_t *stop, struct spot *out, uint32_t *blocks, int *dc)
{
	uint32_t most = (uint32_t)(stop - s->at + 64) * 4, n = 0;

	for (;;) {
		struct spot here = spot_of(s, phase);
		int c = block_comp[phase];

		if (here.at >= stop) {
			*out = here, *blocks = n;
			return 1;
		}
		if (++n > most)
			return 0;
		block_skip(s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &dc[c]);
		if (s->bad)
			return 0;
		phase = phase + 1 == mcu_blocks ? 0 : phase + 1;
	}
}

static void
guess_parts(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;

	for (; from < to; from++) {
		struct part *g = &parts[from];
		struct spot start = { part_at[from], 0, 0 };
		struct scan s;
		int dc[4] = {0, 0, 0, 0};

		/* (a guess that reads no codes starts a byte later; part 0 starts where the scan does) */
		for (;; start.at++) {
			if (start.at >= part_at[from + 1] || (from == 0 && start.at > part_at[0])) {
				g->bad = 1;
				break;
			}
			if (from && start.at[-1] == 0xff)
				continue;
			scan_from(&s, j, start);
			if (read_part(j, &s, 0, part_at[from + 1], &g->guessed, &g->blocks, dc)) {
				g->bad = 0;
				break;
			}
		}
		if (from == 0)
			g->found = g->guessed, memcpy(g->dc, dc, sizeof dc);
	}
}

/* Part k read again from `entry`, where part k - 1 ended. */
static int
count_part(struct jpg *j, uint32_t k, struct spot entry)
{
	struct part *g = &parts[k];
	struct scan s;

	memset(g->dc, 0, sizeof g->dc);
	scan_from(&s, j, entry);
	g->bad = !read_part(j, &s, entry.phase, part_at[k + 1], &g->found, &g->blocks, g->dc);
	return !g->bad;
}

/* (from the guess before's end: the true one whenever that guess met the stream in its part) */
static void
count_parts(void *ctx, uint32_t from, uint32_t to)
{
	for (from = from ? from : 1; from < to; from++)
		count_part(ctx, from, parts[from - 1].guessed);
}

/* Each MCU row's start in part k, read from the part's true start with the block's number and
 * the predictions there. */
static void
index_parts(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;
	uint32_t total = (uint32_t)(j->mcus_x * j->mcus_y) * (uint32_t)mcu_blocks;

	for (; from < to; from++) {
		struct spot start = from ? parts[from - 1].found : (struct spot){ j->file + j->scan_at, 0, 0 };
		const uint8_t *stop = from + 1 < (uint32_t)part_count ? part_at[from + 1] : NULL;
		uint32_t b = part_first[from];
		int pred[4], phase = start.phase;
		struct scan s;

		memcpy(pred, part_pred[from], sizeof pred);
		scan_from(&s, j, start);
		for (; b < total; b++) {
			int c = block_comp[phase];

			if (stop && spot_of(&s, phase).at >= stop)
				break;
			if (!phase && (b / (uint32_t)mcu_blocks) % (uint32_t)j->mcus_x % (uint32_t)piece_mcus == 0) {
				uint32_t mcu = b / (uint32_t)mcu_blocks, mx = mcu % (uint32_t)j->mcus_x;
				struct row_at *r = &j->rows_at[mcu / (uint32_t)j->mcus_x * (uint32_t)row_pieces + mx / (uint32_t)piece_mcus];

				r->at = s.at, r->hold = s.hold, r->count = s.count, r->left = 0;
				memcpy(r->pred, pred, sizeof pred);
			}
			block_skip(&s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &pred[c]);
			if (s.bad) {
				parts[from].bad = 1;
				break;
			}
			phase = phase + 1 == mcu_blocks ? 0 : phase + 1;
		}
	}
}

static void
strips_range(void *ctx, uint32_t from, uint32_t to)
{
	for (; from < to; from++)
		indexed_strip(ctx, (int)from);
}

/* Pieces of MCU rows: piece k is row k / row_pieces's (k % row_pieces)-th. */
static void
pieces_range(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;

	for (; from < to; from++) {
		int my = (int)from / row_pieces, mx = (int)from % row_pieces * piece_mcus;
		int n = j->mcus_x - mx < piece_mcus ? j->mcus_x - mx : piece_mcus;

		run_mcus(j, &j->rows_at[from], mx, my, n);
	}
}

/* The scan in parts, every part read on a core of its own from a guessed start; then again from
 * where the part before ended, and checked here; then every MCU row's start. 0: not so. */
static int
by_guesses(struct jpg *j, int got)
{
	const uint8_t *scan = j->file + j->scan_at, *end = j->file + j->size;
	int k, c, by, bx, n = 0;
	uint32_t bytes = (uint32_t)(end - scan);

	for (c = 0; c < j->components; c++)
		for (by = 0; by < j->comp[c].v; by++)
			for (bx = 0; bx < j->comp[c].h; bx++)
				if (n < 16)
					block_comp[n++] = (int8_t)c;
	mcu_blocks = n;
	part_count = got + 1 < PARTS_MOST ? got + 1 : PARTS_MOST;
	if (part_count > (int)(bytes / 1024))
		part_count = (int)(bytes / 1024);
	if (part_count < 3 || n > 10 || j->restart || getenv("MEDIA_JPEG_SERIAL"))
		return 0;
	if (!parts && !(parts = m_map(sizeof *parts * PARTS_MOST)))
		return 0;
	/* rows in pieces where the pieces' pixels are whole 16 bytes: some four pieces a core */
	row_pieces = 1, piece_mcus = j->mcus_x;
	if (!(((uintptr_t)j->out->pixels | (uint32_t)j->out->stride) & 15)) {
		int unit = 16 / m_gcd(j->omcu_w * j->out->channels, 16);

		row_pieces = (4 * (got + 1) + j->mcus_y - 1) / j->mcus_y;
		row_pieces = row_pieces > ROW_PIECES ? ROW_PIECES : row_pieces;
		piece_mcus = (j->mcus_x + row_pieces - 1) / row_pieces;
		piece_mcus = (piece_mcus + unit - 1) / unit * unit;
		row_pieces = (j->mcus_x + piece_mcus - 1) / piece_mcus;
	}
	mcw_touch(parts, sizeof *parts * (uint32_t)part_count);
	mcw_touch(j->rows_at, (uint32_t)(j->mcus_y * row_pieces) * sizeof(struct row_at));
	for (k = 0; k <= part_count; k++)
		part_at[k] = scan + (uint64_t)bytes * (uint32_t)k / (uint32_t)part_count;
	part_at[part_count] = end;
	m_for((uint32_t)part_count - 1, guess_parts, j, 0, got);	/* (the last needs none) */
	if (parts[0].bad)
		return 0;
	m_for((uint32_t)part_count - 1, count_parts, j, 0, got);
	/* a part whose guess had not met the stream by its end is read again from the true start */
	for (k = 1; k + 1 < part_count; k++) {
		if (parts[k].bad)
			return 0;
		if (memcmp(&parts[k].found, &parts[k].guessed, sizeof parts[k].found) && k + 1 < part_count - 1 &&
		    !count_part(j, (uint32_t)k + 1, parts[k].found))
			return 0;
	}
	part_first[0] = 0;
	memset(part_pred[0], 0, sizeof part_pred[0]);
	for (k = 1; k < part_count; k++) {
		part_first[k] = part_first[k - 1] + parts[k - 1].blocks;
		for (c = 0; c < 4; c++)
			part_pred[k][c] = part_pred[k - 1][c] + parts[k - 1].dc[c];
		if ((int)(part_first[k] % (uint32_t)mcu_blocks) != parts[k - 1].found.phase)
			return 0;
	}
	for (k = 0; k < part_count; k++)
		parts[k].bad = 0;
	m_for((uint32_t)part_count, index_parts, j, 0, got);
	for (k = 0; k < part_count; k++)
		if (parts[k].bad)
			return 0;
	if (row_pieces > 1 || !(((uintptr_t)j->out->pixels | (uint32_t)j->out->stride) & 15))
		m_for((uint32_t)(j->mcus_y * row_pieces), pieces_range, j, 0, got);
	else
		m_for((uint32_t)j->strips, strips_range, j, (uint32_t)j->out->stride * 16, got);
	return 1;
}

/* The pipelines' items: 0 reads the codes, k the strips of the k-th sharer. */
static uint32_t
indexed_stage(uint32_t k, uint32_t unused)
{
	return k ? indexed_strips(k - 1, unused) : index_job(0, 0);
}

static uint32_t
listed_stage(uint32_t k, uint32_t unused)
{
	return k ? strips_job(k - 1, unused) : codes_job(0, 0);
}

/* ---------------------------------------------------------------- restart markers: everything everywhere */

/* Part p of the codes: where the restart markers in it are (a marker that begins in it counts). */
static void
markers_part(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;
	uint32_t p;

	for (p = from; p < to; p++) {
		const uint8_t *f = j->file, *at = f + j->scan_at + p * j->part_bytes, *end = at + j->part_bytes;
		const uint8_t *stop = f + j->size - 1;
		uint32_t n = 0, *list = j->found + p * j->found_most;

		if (end > stop)
			end = stop;
		while (at < end) {
			const uint8_t *ff = memchr(at, 0xff, (size_t)(end - at));

			if (!ff)
				break;
			if (ff[1] >= 0xd0 && ff[1] <= 0xd7) {
				if (n < j->found_most)
					list[n] = (uint32_t)(ff + 2 - f);
				n++;	/* (more than there is room for: the caller reads the codes in order) */
			} else if (ff[1] != 0 && ff[1] != 0xff) {
				break;	/* another marker: the scan ends */
			}
			at = ff + 1;
		}
		j->found_n[p * 4] = n;	/* (each part's count in 16 bytes of its own) */
	}
}

/* Strips (16 rows each) from..to-1, each read from the start of the interval it begins in. */
static void
restart_strips(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;
	uint8_t plane[4][16 * 16];
	uint32_t list[64 + 16];
	int per = 16 / j->omcu_h, strip, c, bx, by;

	for (strip = (int)from; strip < (int)to; strip++) {
		int my0 = strip * per, my1 = my0 + per, pred[4] = {0, 0, 0, 0};
		uint32_t m, m_end, interval = 0xffffffffu;
		struct scan s;

		if (my1 > j->mcus_y)
			my1 = j->mcus_y;
		m = (uint32_t)(my0 * j->mcus_x);
		m_end = (uint32_t)(my1 * j->mcus_x);
		/* from the interval's first MCU: those before this strip's are read and dropped */
		for (uint32_t k = m / (uint32_t)j->restart * (uint32_t)j->restart; k < m_end; k++) {
			int keep = k >= m;

			if (k / (uint32_t)j->restart != interval) {
				interval = k / (uint32_t)j->restart;
				memset(&s, 0, sizeof s);
				s.at = j->file + j->interval_at[interval];
				s.end = j->file + j->size;
				pred[0] = pred[1] = pred[2] = pred[3] = 0;
			}
			for (c = 0; c < j->components; c++)
				for (by = 0; by < j->comp[c].v; by++)
					for (bx = 0; bx < j->comp[c].h; bx++) {
						int n = block_codes(&s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &pred[c], list);

						if (keep)
							list_block(list, n, j->quant[j->comp[c].table], plane[c] + by * j->bs * 16 + bx * j->bs, j->bs);
					}
			if (s.bad)
				*j->failed = 1;
			if (keep)
				mcu_pixels(j, plane, (int)(k % (uint32_t)j->mcus_x), (int)(k / (uint32_t)j->mcus_x));
		}
	}
}

/* Intervals from..to-1, each read and written whole. */
static void
restart_intervals(void *ctx, uint32_t from, uint32_t to)
{
	struct jpg *j = ctx;
	uint8_t plane[4][16 * 16];
	uint32_t list[64 + 16], i, k, mcus = (uint32_t)(j->mcus_x * j->mcus_y);
	int c, bx, by;

	for (i = from; i < to; i++) {
		struct scan s;
		int pred[4] = {0, 0, 0, 0};
		uint32_t end = (i + 1) * (uint32_t)j->restart < mcus ? (i + 1) * (uint32_t)j->restart : mcus;

		memset(&s, 0, sizeof s);
		s.at = j->file + j->interval_at[i];
		s.end = j->file + j->size;
		for (k = i * (uint32_t)j->restart; k < end; k++) {
			for (c = 0; c < j->components; c++)
				for (by = 0; by < j->comp[c].v; by++)
					for (bx = 0; bx < j->comp[c].h; bx++) {
						int n = block_codes(&s, &j->huff[0][j->comp[c].dc], &j->huff[1][j->comp[c].ac], &pred[c], list);

						list_block(list, n, j->quant[j->comp[c].table], plane[c] + by * j->bs * 16 + bx * j->bs, j->bs);
					}
			mcu_pixels(j, plane, (int)(k % (uint32_t)j->mcus_x), (int)(k / (uint32_t)j->mcus_x));
		}
		if (s.bad)
			*j->failed = 1;
	}
}

/* Whether every interval's start was found; then the strips are decoded on every core. */
static int
by_restarts(struct jpg *j, int got)
{
	uint32_t mcus = (uint32_t)(j->mcus_x * j->mcus_y), p, k, n = 1, bytes = j->size - j->scan_at;

	j->intervals = (mcus + (uint32_t)j->restart - 1) / (uint32_t)j->restart;
	if (j->intervals < 2)
		return 0;
	j->parts = (uint32_t)(got + 1) * 4;
	if (j->parts > j->intervals)
		j->parts = j->intervals;
	j->part_bytes = (bytes + j->parts - 1) / j->parts;
	/* room for three times a part's share of the markers, each part's on 16 bytes */
	j->found_most = (j->intervals / j->parts * 3 + 8 + 3) & ~3u;
	j->interval_at = m_scratch(M_JPEG_INTERVALS, (j->intervals + 1) * 4);
	j->found = m_scratch(M_JPEG_FOUND, j->parts * j->found_most * 4);
	j->found_n = m_scratch(M_JPEG_COUNTS, j->parts * 16);
	if (!j->interval_at || !j->found || !j->found_n)
		return 0;
	m_for(j->parts, markers_part, j, 0, got);
	j->interval_at[0] = j->scan_at;
	for (p = 0; p < j->parts && n < j->intervals; p++)
		if (j->found_n[p * 4] > j->found_most)
			break;
		else
			for (k = 0; k < j->found_n[p * 4] && n < j->intervals; k++)
				j->interval_at[n++] = j->found[p * j->found_most + k];
	if (p < j->parts && n < j->intervals) {
		/* (a part had more markers than its room: they are looked for here, all of them) */
		const uint8_t *at = j->file + j->scan_at, *stop = j->file + j->size - 1;

		for (n = 1; at < stop && n < j->intervals; at++)
			if (at[0] == 0xff && at[1] >= 0xd0 && at[1] <= 0xd7)
				j->interval_at[n++] = (uint32_t)(at + 2 - j->file);
			else if (at[0] == 0xff && at[1] != 0 && at[1] != 0xff)
				break;
	}
	if (n != j->intervals)
		return 0;
	/* an MCU's pixels on whole 16 bytes of every row: then an interval is a job by itself */
	if (!(((uintptr_t)j->out->pixels | (uint32_t)j->out->stride | (uint32_t)(j->omcu_w * j->out->channels)) & 15))
		m_for(j->intervals, restart_intervals, j, 0, got);
	else
		m_for((uint32_t)j->strips, restart_strips, j, (uint32_t)j->out->stride * 16, got);
	return 1;
}

/* ---------------------------------------------------------------- headers */

static const char *
header(struct jpg *j, int whole)
{
	const uint8_t *f = j->file;
	uint32_t at = 2;
	int i, k;

	j->hmax = j->vmax = 1;
	j->transform = -1;
	while (at + 4 <= j->size) {
		int marker, length;

		if (f[at] != 0xff)
			return "a JPEG picture that is damaged";
		marker = f[at + 1];
		if (marker == 0xff) {
			at++;
			continue;
		}
		length = f[at + 2] << 8 | f[at + 3];
		if (at + 2 + (uint32_t)length > j->size)
			break;
		if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
			if (f[at + 4] != 8)
				return "a JPEG picture of a kind this does not read";
			j->progressive = marker == 0xc2;
			j->h = f[at + 5] << 8 | f[at + 6];
			j->w = f[at + 7] << 8 | f[at + 8];
			j->components = f[at + 9];
			if (j->components != 1 && j->components != 3 && j->components != 4)
				return "a JPEG picture of a kind this does not read";
			for (i = 0; i < j->components; i++) {
				j->comp[i].id = f[at + 10 + 3 * i];
				j->comp[i].h = f[at + 11 + 3 * i] >> 4;
				j->comp[i].v = f[at + 11 + 3 * i] & 15;
				j->comp[i].table = f[at + 12 + 3 * i] & 3;
				if (j->comp[i].h < 1 || j->comp[i].h > 4 || j->comp[i].v < 1 || j->comp[i].v > 4 ||
				    j->comp[i].h == 3 || j->comp[i].v == 3)
					return "a JPEG picture of a kind this does not read";
				if (j->comp[i].h > j->hmax) j->hmax = j->comp[i].h;
				if (j->comp[i].v > j->vmax) j->vmax = j->comp[i].v;
			}
			if (j->components == 1)
				j->comp[0].h = j->comp[0].v = j->hmax = j->vmax = 1;
			for (i = 0; i < j->components; i++)
				if (j->hmax / j->comp[i].h > 2 && j->hmax / j->comp[i].h != 4)
					return "a JPEG picture of a kind this does not read";
			/* (a sample stretched four times: only to 16 in an MCU, which 4 x 4 of 8 would pass) */
			if (j->hmax * 8 > 16 || j->vmax * 8 > 16)
				return "a JPEG picture of a kind this does not read";
			j->mcu_w = 8 * j->hmax, j->mcu_h = 8 * j->vmax;
			j->mcus_x = (j->w + j->mcu_w - 1) / j->mcu_w;
			j->mcus_y = (j->h + j->mcu_h - 1) / j->mcu_h;
			if (!whole)
				return j->w && j->h ? NULL : "a JPEG picture that is damaged";
		} else if (marker >= 0xc3 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			return "a JPEG picture of a kind this does not read (arithmetic or lossless)";
		} else if (marker == 0xee && length >= 12 && !memcmp(f + at + 4, "Adobe", 5)) {
			j->adobe = 1;
			j->transform = f[at + 15];
		} else if (marker == 0xdb) {
			for (k = 4; k < length + 2;) {
				int wide = f[at + k] >> 4, t = f[at + k] & 3;

				k++;
				for (i = 0; i < 64; i++, k += wide ? 2 : 1)
					j->quant[t][i] = wide ? (uint16_t)(f[at + k] << 8 | f[at + k + 1]) : f[at + k];
			}
		} else if (marker == 0xc4) {
			for (k = 4; k < length + 2;) {
				struct huff *h = &j->huff[f[at + k] >> 4 & 1][f[at + k] & 3];
				int n = 0;

				k++;
				h->length[0] = 0;
				for (i = 1; i <= 16; i++)
					n += h->length[i] = f[at + k++];
				for (i = 0; i < n && i < 256; i++)
					h->symbol[i] = f[at + k + i];
				k += n;
				if (!j->no_tables)
					huff_make(h);
			}
		} else if (marker == 0xdd) {
			j->restart = f[at + 4] << 8 | f[at + 5];
		} else if (marker == 0xda) {
			int n = f[at + 4], c;

			if (!j->w)
				return "a JPEG picture of a kind this does not read";
			if (!j->progressive && n != j->components)
				return "a JPEG picture of a kind this does not read (scans of single components)";
			for (i = 0; i < n; i++)
				for (c = 0; c < j->components; c++)
					if (j->comp[c].id == f[at + 5 + 2 * i]) {
						j->comp[c].dc = f[at + 6 + 2 * i] >> 4 & 3;
						j->comp[c].ac = f[at + 6 + 2 * i] & 3;
					}
			j->scan_at = at + 2 + (uint32_t)length;
			if (j->transform < 0)
				j->transform = j->components == 3 ? 1 : j->components == 4 ? 0 : 0;
			return NULL;
		}
		at += 2 + (uint32_t)length;
	}
	return j->w ? "a JPEG picture with nothing in it" : "a JPEG picture that is damaged";
}

const char *
m_jpeg_probe(const uint8_t *f, uint32_t size, struct media_info *info)
{
	static struct jpg probe;
	const char *why;

	memset(&probe, 0, sizeof probe);
	probe.file = f, probe.size = size, probe.no_tables = 1;
	if ((why = header(&probe, 1)) != NULL && !probe.w)
		return why;
	info->kind = MEDIA_JPEG;
	info->width = probe.w, info->height = probe.h;
	info->parallel = probe.restart && !probe.progressive;
	return why;
}

#include "m_jpeg_prog.h"

const char *
m_jpeg_decode_shrunk(const uint8_t *f, uint32_t size, struct media_image *out, int workers, int shrink);

const char *
m_jpeg_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers)
{
	return m_jpeg_decode_shrunk(f, size, out, workers, 1);
}

uint32_t
m_jpeg_room(void)
{
	colour_tables();	/* (made here, on the first core, before any core decodes alone) */
	return (sizeof(struct jpg) + 15) & ~15u;
}

/* Room for a picture's state and its intervals' places (m_jpeg_parts), for a file of `size`. */
uint32_t
m_jpeg_parts_room(uint32_t size)
{
	colour_tables();
	return ((sizeof(struct jpg) + 15) & ~15u) + 16 + (size / 2 + 4) * 4;
}

/* A baseline picture's tables read and its restart intervals found, into `room`: how many, for
 * m_jpeg_part on any core; 0 when it has no markers or an MCU is not whole 16 bytes of a row. */
uint32_t
m_jpeg_parts(const uint8_t *f, uint32_t size, struct media_image *out, void *room)
{
	struct jpg *j = room;
	uint32_t mcus, n = 1, at;
	const uint8_t *stop;

	memset(j, 0, sizeof *j);
	j->file = f, j->size = size, j->out = out, j->shrink = 1;
	if (header(j, 1) || j->progressive || !j->restart || out->width != j->w || out->height != j->h)
		return 0;
	j->bs = 8, j->ow = j->w, j->oh = j->h, j->omcu_w = j->mcu_w, j->omcu_h = j->mcu_h;
	if (((uintptr_t)out->pixels | (uint32_t)out->stride | (uint32_t)(j->omcu_w * out->channels)) & 15)
		return 0;
	j->failed = (volatile uint32_t *)((uint8_t *)room + ((sizeof(struct jpg) + 15) & ~15u));
	*j->failed = 0;
	j->interval_at = (uint32_t *)((uint8_t *)j->failed + 16);
	mcus = (uint32_t)(j->mcus_x * j->mcus_y);
	j->intervals = (mcus + (uint32_t)j->restart - 1) / (uint32_t)j->restart;
	if (j->intervals > size / 2 + 4)
		return 0;
	j->interval_at[0] = j->scan_at;
	for (at = j->scan_at, stop = f + size - 1; f + at < stop && n < j->intervals; at++)
		if (f[at] == 0xff && f[at + 1] >= 0xd0 && f[at + 1] <= 0xd7)
			j->interval_at[n++] = at + 2;
		else if (f[at] == 0xff && f[at + 1] != 0 && f[at + 1] != 0xff)
			break;
	return n == j->intervals ? n : 0;
}

/* Intervals from..to-1 of a picture m_jpeg_parts found, on the core that calls; with `clear` the
 * words' top byte 0 where they were written (the GPU's top byte is transparency). 0: damaged. */
int
m_jpeg_part(void *room, uint32_t from, uint32_t to, int clear)
{
	struct jpg *j = room;
	uint32_t i, k, mcus = (uint32_t)(j->mcus_x * j->mcus_y);

	restart_intervals(j, from, to);
	if (clear && j->out->channels == 4)
		for (i = from; i < to; i++)
			for (k = i * (uint32_t)j->restart; k < (i + 1) * (uint32_t)j->restart && k < mcus; k++) {
				int mx = (int)(k % (uint32_t)j->mcus_x), my = (int)(k / (uint32_t)j->mcus_x), x, y;
				int x1 = (mx + 1) * j->omcu_w < j->ow ? (mx + 1) * j->omcu_w : j->ow;

				for (y = my * j->omcu_h; y < (my + 1) * j->omcu_h && y < j->oh; y++) {
					uint32_t *row = (uint32_t *)(j->out->pixels + (uint32_t)y * (uint32_t)j->out->stride);

					for (x = mx * j->omcu_w; x < x1; x++)
						row[x] &= 0xffffffu;
				}
			}
	return !*j->failed;
}

/* A baseline picture whole on the core that calls, its state in `room` (m_jpeg_room() bytes): for
 * pictures decoded side by side, a core each. A progressive one is refused. */
const char *
m_jpeg_decode_alone(const uint8_t *f, uint32_t size, struct media_image *out, void *room)
{
	struct jpg *j = room;
	volatile uint32_t failed = 0;
	struct row_at r;
	const char *why;

	memset(j, 0, sizeof *j);
	j->file = f, j->size = size, j->out = out, j->shrink = 1;
	if ((why = header(j, 1)) != NULL)
		return why;
	if (j->progressive)
		return "a progressive JPEG, which is decoded over all cores only";
	if (out->width != j->w || out->height != j->h)
		return "the picture is not the size its pixels were given for";
	j->failed = &failed;
	j->bs = 8, j->ow = j->w, j->oh = j->h, j->omcu_w = j->mcu_w, j->omcu_h = j->mcu_h;
	memset(&r, 0, sizeof r);
	r.at = f + j->scan_at, r.left = j->restart;
	run_mcus(j, &r, 0, 0, j->mcus_x * j->mcus_y);
	return failed ? "the picture is damaged" : NULL;
}

/* The same, 1, 2, 4 or 8 times smaller each way as m_jpeg_decode_shrunk makes it, on the core that calls. */
const char *
m_jpeg_decode_alone_shrunk(const uint8_t *f, uint32_t size, struct media_image *out, void *room, int shrink)
{
	struct jpg *j = room;
	volatile uint32_t failed = 0;
	struct row_at r;
	const char *why;

	memset(j, 0, sizeof *j);
	j->file = f, j->size = size, j->out = out;
	j->shrink = shrink == 2 || shrink == 4 || shrink == 8 ? shrink : 1;
	if ((why = header(j, 1)) != NULL)
		return why;
	if (j->progressive)
		return "a progressive JPEG, which is decoded over all cores only";
	if (out->width != (j->w + j->shrink - 1) / j->shrink || out->height != (j->h + j->shrink - 1) / j->shrink)
		return "the picture is not the size its pixels were given for";
	j->failed = &failed;
	j->bs = 8 / j->shrink;
	j->ow = (j->w + j->shrink - 1) / j->shrink, j->oh = (j->h + j->shrink - 1) / j->shrink;
	j->omcu_w = j->mcu_w / j->shrink, j->omcu_h = j->mcu_h / j->shrink;
	memset(&r, 0, sizeof r);
	r.at = f + j->scan_at, r.left = j->restart;
	run_mcus(j, &r, 0, 0, j->mcus_x * j->mcus_y);
	return failed ? "the picture is damaged" : NULL;
}

/* The same, 1, 2, 4 or 8 times smaller each way (out is (w + shrink - 1) / shrink wide). */
const char *
m_jpeg_decode_shrunk(const uint8_t *f, uint32_t size, struct media_image *out, int workers, int shrink)
{
	static struct jpg job;
	struct jpg *j = &job;
	const char *why;
	int got = 0, k;
	uint32_t blocks = 0;

	memset(j, 0, sizeof *j);
	J = j;
	j->file = f, j->size = size, j->out = out;
	j->shrink = shrink == 2 || shrink == 4 || shrink == 8 ? shrink : 1;
	if ((why = header(j, 1)) != NULL)
		return why;
	if (out->width != (j->w + j->shrink - 1) / j->shrink || out->height != (j->h + j->shrink - 1) / j->shrink)
		return "the picture is not the size its pixels were given for";
	colour_tables();
	/* (the stages' counts, made once and kept) */
	{
		static volatile uint32_t *failed, *listed;

		if (!failed)
			failed = mcw_alloc(16), listed = mcw_alloc(16);
		j->failed = failed, j->listed = listed;
	}
	*j->failed = 0;
	j->bs = 8 / j->shrink;
	j->ow = (j->w + j->shrink - 1) / j->shrink, j->oh = (j->h + j->shrink - 1) / j->shrink;
	j->omcu_w = j->mcu_w / j->shrink, j->omcu_h = j->mcu_h / j->shrink;
	j->strips = (j->oh + 15) / 16;
	/* (a small picture is this core's: a round of the workers costs more; no more workers than
	 * jobs: strips, or the intervals of a file with markers, or pieces of rows) */
	{
		int jobs = j->restart ? (j->mcus_x * j->mcus_y + j->restart - 1) / j->restart : j->strips * ROW_PIECES;

		jobs = jobs > j->strips ? jobs : j->strips;
		got = m_workers_begin(j->w * j->h < 16384 ? 0 : workers > jobs ? jobs : workers);
	}
	if (j->progressive) {
		why = progressive_decode(j, got);
		goto done;
	}
	if (j->restart && by_restarts(j, got))
		goto checked;
	*j->failed = 0;
	if (got >= 1 && !getenv("MEDIA_JPEG_LIST")) {
		/* this core finds where each MCU row begins; the workers read the codes from there */
		if (!(j->rows_at = m_scratch(M_JPEG_STRIPS, (uint32_t)j->mcus_y * ROW_PIECES * sizeof(struct row_at)))) {
			why = "not enough memory for the picture";
			goto done;
		}
		if (by_guesses(j, got))
			goto checked;
		*j->failed = 0;
		*j->listed = 0;
		j->sharers = got;
		m_stages((uint32_t)got + 1, indexed_stage, got);
		goto checked;
	}
	for (k = 0; k < j->components; k++)
		blocks += (uint32_t)(j->comp[k].h * j->comp[k].v);
	blocks *= (uint32_t)(j->mcus_x * j->mcus_y);
	/* a coefficient that is not zero is at least two bits of the file, and a word here */
	j->list_room = 16 * j->size + 4 * blocks + 8192;
	j->list = mmap(NULL, (j->list_room + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	j->strip_at = m_scratch(M_JPEG_STRIPS, (uint32_t)(j->mcus_y + 1) * 4 + 16);
	if (j->list == MAP_FAILED || !j->strip_at || !j->listed) {
		why = "not enough memory for the picture";
		goto done;
	}
	/* (the list is seldom a quarter of that: so much is made to be there, the rest when it is stepped on) */
	mcw_touch(j->list, j->list_room / 4 < 4 * j->size + 4 * blocks + 4096 ? j->list_room / 4 : 4 * j->size + 4 * blocks + 4096);
	*j->listed = 0;
	if (got >= 1) {
		/* The codes are one stream, and what the picture waits for: this core has them (it keeps
		 * four times a worker's stores a pass), and every worker has strips. */
		j->sharers = got;
		m_stages((uint32_t)got + 1, listed_stage, got);
	} else {
		j->sharers = 1;
		codes_job(0, 0);
		strips_job(0, 0);
	}
checked:
	if (*j->failed)
		why = "the picture is damaged";
done:
	if (j->list && j->list != MAP_FAILED)
		munmap(j->list, (j->list_room + 4095) & ~4095u);
	m_scratch_done(M_JPEG_STRIPS, j->strip_at, (uint32_t)(j->mcus_y + 1) * 4 + 16);
	m_scratch_done(M_JPEG_STRIPS, j->rows_at, (uint32_t)j->mcus_y * ROW_PIECES * sizeof(struct row_at));
	m_scratch_done(M_JPEG_FOUND, j->found, j->parts * j->found_most * 4);
	m_scratch_done(M_JPEG_COUNTS, j->found_n, j->parts * 16);
	m_scratch_done(M_JPEG_INTERVALS, j->interval_at, (j->intervals + 1) * 4);
	for (k = 0; k < 4; k++)
		m_unmap(j->comp[k].coef, (uint32_t)j->comp[k].bw * (uint32_t)j->comp[k].bh * 128);
	m_workers_end();
	return why;
}
