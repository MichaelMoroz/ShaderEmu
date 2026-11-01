/*
 * PNG pictures on the worker cores (docs/system/media.md).
 *
 *   segments   a file this library wrote (its mdPR chunk): each IDAT is rows of their own, a
 *              deflate stream that refers to nothing before it and a first row filtered without
 *              the row above. Every core inflates and unfilters segments, in place; then the
 *              pixels are made over every core.
 *   one stream a stream of 64 KB or more is inflated on every core (m_inflate.c), then workers take
 *              the filters off a band of the row each, each a row behind the band before it, into
 *              the pixels themselves where they are the rows, or then over every core. Smaller, or
 *              a stream that cannot be shared: this core inflates, the workers follow it and the
 *              last writes the pixels.
 *   interlaced this core inflates; the seven passes are unfiltered by a core each, and the
 *              pixels gathered from them over every core.
 */
#include <stdlib.h>
#include "m_int.h"

/* The private chunk: segments (a word), then each segment's first row (a word each). */
#define SEGMENTS_CHUNK "mdPR"
#define SEGMENTS_MOST 256
#define M_BANDS 16
#define WHOLE_LEAST 65536	/* a stream shorter than this is one core's: a part is 16 KB of it (m_inflate.c) */
#define WHOLE_CHUNKS 4096	/* a stream in more IDATs than this too */

struct png {
	const uint8_t *file;
	uint32_t size;
	struct media_image *out;
	int w, h, depth, type, channels, row_bytes, pixel_bytes, interlaced;
	uint8_t palette[256][4];
	int trns, trns_gray, trns_r, trns_g, trns_b;
	uint32_t first_idat;
	volatile uint32_t *failed;
	/* one stream */
	uint8_t *raw, *rows;
	uint32_t raw_bytes, rows_bytes;
	int row_pitch;
	volatile uint32_t *inflated, *banded[M_BANDS];
	int bands, band_at[M_BANDS + 1], whole;	/* whole: the stream is inflated already */
	/* segments */
	int segments;
	struct codes *seg_tables;	/* three tables a core, made here before the jobs */
	int seg_pixels;			/* a segment's job makes its pixels too */
	uint32_t seg_row[SEGMENTS_MOST + 1], seg_chunk[SEGMENTS_MOST], seg_raw[SEGMENTS_MOST + 1];
	/* interlaced: each pass's rows, unfiltered */
	uint8_t *pass_rows[7];
	uint32_t pass_at[8];		/* where each pass's filtered bytes begin in raw */
	int pass_w[7], pass_h[7], pass_rb[7];
};

static struct png *P;

static const int adam_x0[7] = {0, 4, 0, 2, 0, 1, 0}, adam_y0[7] = {0, 0, 4, 0, 2, 0, 1};
static const int adam_dx[7] = {8, 8, 4, 4, 2, 2, 1}, adam_dy[7] = {8, 8, 8, 4, 4, 2, 2};

/* ---------------------------------------------------------------- inflate */

struct bits { const uint8_t *at, *chunk_end, *file_end; uint32_t hold; int count, bad, over, one_chunk; };

/* The next byte of the IDAT chunks, which lie one after another with a header between. Past
 * the last of them there are zeros (a table looks a few bits ahead of the stream's end). */
static int
idat_next(struct bits *b)
{
	while (b->at >= b->chunk_end) {
		const uint8_t *next = b->chunk_end + 4;	/* past the chunk's check */
		uint32_t length;

		if (b->one_chunk || next + 8 > b->file_end || memcmp(next + 4, "IDAT", 4) != 0) {
			if (++b->over > 8)
				b->bad = 1;
			return 0;
		}
		length = m_be32(next);
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
need(struct bits *b, int n)
{
	while (b->count < n) {
		b->hold |= (uint32_t)(b->at < b->chunk_end ? *b->at++ : idat_next(b)) << b->count;
		b->count += 8;
	}
}

static inline uint32_t
take(struct bits *b, int n)
{
	uint32_t v;

	need(b, n);
	v = b->hold & ((1u << n) - 1);
	b->hold >>= n;
	b->count -= n;
	return v;
}

#define QUICK 10
struct codes { uint16_t count[16], symbol[288], quick[1 << QUICK]; };

static void
codes_make(struct codes *c, const uint8_t *lengths, int n)
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
		if (length <= QUICK) {
			/* the code's bits arrive lowest first: turned round, then every filling of the bits above */
			int straight = next[length], turned = 0, k;

			for (k = 0; k < length; k++)
				turned |= (straight >> k & 1) << (length - 1 - k);
			for (k = turned; k < 1 << QUICK; k += 1 << length)
				c->quick[k] = (uint16_t)(length << 12 | i);
		}
		next[length]++;
	}
}

static inline int
code_slow(struct bits *b, const struct codes *c)
{
	int code = 0, first = 0, index = 0, length;

	need(b, 15);
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

/*
 * Inflates from `b` into out[0..total), telling `told` (when given) every 1,024 bytes; three
 * tables of `struct codes` at `tables`. What it made.
 */
static uint32_t
inflate(struct bits *bb, uint8_t *out, uint32_t total, volatile uint32_t *told_to, struct codes *tables, int zlib)
{
	static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	struct bits b = *bb;
#define literals tables[0]
#define distances tables[1]
#define meta tables[2]
	uint8_t lengths[320];
	uint32_t hold, n = 0, last, told = 0, looked = 0, said = 0;
	int count;
	const uint8_t *at, *end;
	int on_first = !mcw_on_worker() && mcw_count() > 0;
/* Two bytes at once while the chunk has them; at a chunk's end, and for a long code, by b. */
#define BY_B(what) do { \
		b.hold = hold, b.count = count, b.at = at; \
		what; \
		hold = b.hold, count = b.count, at = b.at, end = b.chunk_end; \
		if (b.bad) \
			goto stopped; \
	} while (0)
#define FILL(want) do { \
		if (count < (want)) { \
			if (at + 2 <= end) { \
				hold |= (uint32_t)(at[0] | at[1] << 8) << count; \
				at += 2, count += 16; \
			} else \
				BY_B(need(&b, want)); \
		} \
	} while (0)
#define CODE(symbol, codes) do { \
		uint16_t quick; \
		FILL(15); \
		quick = (codes).quick[hold & ((1 << QUICK) - 1)]; \
		if (quick) { \
			hold >>= quick >> 12, count -= quick >> 12; \
			symbol = quick & 0xfff; \
		} else \
			BY_B(symbol = code_slow(&b, &(codes))); \
	} while (0)

	if (zlib)
		take(&b, 16);	/* the zlib header */
	do {
		uint32_t kind;

		last = take(&b, 1);
		kind = take(&b, 2);
		if (kind == 0) {
			uint32_t length;

			take(&b, b.count & 7);
			length = take(&b, 16);
			take(&b, 16);
			while (length && n < total && !b.bad) {
				/* whole bytes from the chunk while the bit buffer is empty */
				if (b.count == 0 && b.at < b.chunk_end) {
					uint32_t run = (uint32_t)(b.chunk_end - b.at);

					if (run > length) run = length;
					if (run > total - n) run = total - n;
					memcpy(out + n, b.at, run);
					b.at += run, n += run, length -= run;
					continue;
				}
				out[n++] = (uint8_t)take(&b, 8);
				length--;
			}
		} else if (kind == 1 || kind == 2) {
			int i;

			if (kind == 1) {
				for (i = 0; i < 288; i++)
					lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
				codes_make(&literals, lengths, 288);
				for (i = 0; i < 30; i++)
					lengths[i] = 5;
				codes_make(&distances, lengths, 30);
			} else {
				int hlit = (int)take(&b, 5) + 257, hdist = (int)take(&b, 5) + 1, hclen = (int)take(&b, 4) + 4;

				memset(lengths, 0, 19);
				for (i = 0; i < hclen; i++)
					lengths[order[i]] = (uint8_t)take(&b, 3);
				codes_make(&meta, lengths, 19);
				for (i = 0; i < hlit + hdist && !b.bad;) {
					int symbol = code_slow(&b, &meta), repeat = 1, value = symbol;

					if (symbol == 16) {
						value = i ? lengths[i - 1] : 0;
						repeat = 3 + (int)take(&b, 2);
					} else if (symbol == 17) {
						value = 0;
						repeat = 3 + (int)take(&b, 3);
					} else if (symbol == 18) {
						value = 0;
						repeat = 11 + (int)take(&b, 7);
					}
					while (repeat-- && i < hlit + hdist)
						lengths[i++] = (uint8_t)value;
				}
				codes_make(&literals, lengths, hlit);
				codes_make(&distances, lengths + hlit, hdist);
			}
			/* the reader's state in registers here: a store to out could be one to b, for all the compiler knows */
			hold = b.hold, count = b.count, at = b.at, end = b.chunk_end;
			while (!b.bad) {
				int symbol, extra;

				CODE(symbol, literals);
				/* literals come in runs: one check a byte, for the next telling or the rows' end */
				if (symbol < 256) {
					uint8_t *to = out + n, *stop = out + (total - told > 1024 ? told + 1024 : total);

					do {
						*to++ = (uint8_t)symbol;
						if (to == stop) {
							n = (uint32_t)(to - out);
							goto tell;
						}
						CODE(symbol, literals);
					} while (symbol < 256);
					n = (uint32_t)(to - out);
				}
				if (symbol == 256) {
					break;
				} else if (symbol < 286) {
					uint32_t length, distance;
					int d;

					extra = length_extra[symbol - 257];
					FILL(extra);
					length = length_base[symbol - 257] + (hold & ((1u << extra) - 1));
					hold >>= extra, count -= extra;
					CODE(d, distances);
					if (d >= 30) {
						b.bad = 1;
						break;
					}
					extra = distance_extra[d];
					FILL(extra);
					distance = distance_base[d] + (hold & ((1u << extra) - 1));
					hold >>= extra, count -= extra;
					if (distance > n) {
						b.bad = 1;
						break;
					}
					if (length > total - n)
						length = total - n;
					{
						uint8_t *to = out + n, *stop = to + length;
						const uint8_t *from = to - distance;

						n += length;
						while (to < stop)
							*to++ = *from++;
					}
				} else {
					b.bad = 1;
				}
tell:
				if (n - told >= 1024) {
					told = n;
					if (told_to)
						*told_to = told;
					if (told_to && n - said >= 32768)
						said = n, m_progress(n, total);
					/* (a worker that stopped at a page goes on when this core looks) */
					if (on_first && n - looked >= 65536) {
						looked = n;
						m_look();
					}
				}
				if (n >= total)
					break;	/* (every row is there: what the stream still has is not looked at) */
			}
stopped:
			b.hold = hold, b.count = count, b.at = at;
		} else {
			b.bad = 1;
		}
		if (told_to)
			*told_to = n;
	} while (!last && !b.bad && n < total);
	*bb = b;
	return n;
#undef BY_B
#undef FILL
#undef CODE
#undef literals
#undef distances
#undef meta
}

/* ---------------------------------------------------------------- filters, pixels */

static inline int
paeth(int a, int b, int c)
{
	/* how far each of the three is from a + b - c: the nearest, a before b before c */
	int pa = b - c, pb = a - c, pc = pa + pb;

	pa = pa < 0 ? -pa : pa, pb = pb < 0 ? -pb : pb, pc = pc < 0 ? -pc : pc;
	if (pb < pa)
		a = b, pa = pb;
	return pc < pa ? c : a;
}

/* Bytes x0..x1-1 of a row without its filter: `in` the filtered bytes (after the filter's byte), `up` the row above or NULL. */
static int
unfilter(int filter, const uint8_t *in, uint8_t *out, const uint8_t *up, int x0, int x1, int bpp)
{
	int x = x0;

	switch (filter) {
	case 0:
		if (in != out)
			memmove(out + x0, in + x0, x1 - x0);
		break;
	case 1:
		for (; x < bpp && x < x1; x++)
			out[x] = in[x];
		for (; x < x1; x++)
			out[x] = (uint8_t)(in[x] + out[x - bpp]);
		break;
	case 2:
		if (!up)
			memmove(out + x0, in + x0, x1 - x0);
		else
			for (; x < x1; x++)
				out[x] = (uint8_t)(in[x] + up[x]);
		break;
	case 3:
		for (; x < bpp && x < x1; x++)
			out[x] = (uint8_t)(in[x] + ((up ? up[x] : 0) >> 1));
		for (; x < x1; x++)
			out[x] = (uint8_t)(in[x] + ((out[x - bpp] + (up ? up[x] : 0)) >> 1));
		break;
	case 4:
		for (; x < bpp && x < x1; x++)
			out[x] = (uint8_t)(in[x] + (up ? up[x] : 0));
		if (up && (bpp == 3 || bpp == 4) && x + bpp <= x1) {
			/* a pixel at a time: what is left of each byte and above that are kept, not loaded again */
			int a0 = out[x - bpp], a1 = out[x - bpp + 1], a2 = out[x - bpp + 2], a3 = bpp == 4 ? out[x - 1] : 0;
			int c0 = up[x - bpp], c1 = up[x - bpp + 1], c2 = up[x - bpp + 2], c3 = bpp == 4 ? up[x - 1] : 0, b;

#define PAETH(a, c, k) (b = up[x + k], a = (uint8_t)(in[x + k] + paeth(a, b, c)), c = b, out[x + k] = (uint8_t)a)
			for (; x + bpp <= x1; x += bpp) {
				PAETH(a0, c0, 0);
				PAETH(a1, c1, 1);
				PAETH(a2, c2, 2);
				if (bpp == 4)
					PAETH(a3, c3, 3);
			}
#undef PAETH
		}
		if (up)
			for (; x < x1; x++)
				out[x] = (uint8_t)(in[x] + paeth(out[x - bpp], up[x], up[x - bpp]));
		else
			for (; x < x1; x++)
				out[x] = (uint8_t)(in[x] + out[x - bpp]);
		break;
	default:
		return 0;
	}
	return 1;
}

/* Pixel x of an unfiltered row. */
static inline void
png_pixel(const struct png *p, const uint8_t *in, int x, int *r, int *g, int *b, int *a)
{
	int wide = p->depth == 16 ? 2 : 1;

	*a = 255;
	if (p->type == 3 || (p->type == 0 && p->depth < 8)) {
		int shift = 8 - p->depth - (x * p->depth & 7), v = in[x * p->depth >> 3] >> shift & ((1 << p->depth) - 1);

		if (p->type == 3) {
			*r = p->palette[v][0], *g = p->palette[v][1], *b = p->palette[v][2], *a = p->palette[v][3];
		} else {
			*r = *g = *b = v * 255 / ((1 << p->depth) - 1);
			if (p->trns && v == p->trns_gray)
				*a = 0;
		}
		return;
	}
	in += x * p->channels * wide;
	if (p->type == 0 || p->type == 4) {
		*r = *g = *b = in[0];
		if (p->type == 4)
			*a = in[wide];
		else if (p->trns && (wide == 2 ? in[0] << 8 | in[1] : in[0]) == p->trns_gray)
			*a = 0;
	} else {
		*r = in[0], *g = in[wide], *b = in[2 * wide];
		if (p->type == 6)
			*a = in[3 * wide];
		else if (p->trns && (wide == 2 ? (in[0] << 8 | in[1]) == p->trns_r && (in[2] << 8 | in[3]) == p->trns_g && (in[4] << 8 | in[5]) == p->trns_b
					   : in[0] == p->trns_r && in[1] == p->trns_g && in[2] == p->trns_b))
			*a = 0;
	}
}

static void
row_pixels(const struct png *p, const uint8_t *in, uint8_t *out)
{
	const struct media_image *o = p->out;
	int x, r, g, b, a;

	if (p->type == 2 && p->depth == 8 && !p->trns && o->channels == 3) {
		memcpy(out, in, (size_t)p->w * 3);
		return;
	}
	if (p->type == 6 && p->depth == 8 && o->channels == 4 && !o->bgr && !o->opaque) {
		memcpy(out, in, (size_t)p->w * 4);
		return;
	}
	if ((p->type == 6 || (p->type == 2 && !p->trns)) && p->depth == 8 && o->channels == 4 && o->bgr && !o->opaque &&
	    !((uintptr_t)out & 3)) {
		/* 0x00RRGGBB words with alpha above: a word stored a pixel */
		uint32_t *w = (uint32_t *)out;

		if (p->type == 6)
			for (x = 0; x < p->w; x++, in += 4)
				w[x] = (uint32_t)in[3] << 24 | (uint32_t)in[0] << 16 | (uint32_t)in[1] << 8 | in[2];
		else
			for (x = 0; x < p->w; x++, in += 3)
				w[x] = 0xff000000u | (uint32_t)in[0] << 16 | (uint32_t)in[1] << 8 | in[2];
		return;
	}
	if (p->type == 6 && p->depth == 8 && o->channels == 3) {
		for (x = 0; x < p->w; x++, out += 3, in += 4)
			if (in[3] == 255)
				out[0] = in[0], out[1] = in[1], out[2] = in[2];
			else
				m_put(o, out, in[0], in[1], in[2], in[3]);
		return;
	}
	for (x = 0; x < p->w; x++, out += o->channels) {
		png_pixel(p, in, x, &r, &g, &b, &a);
		m_put(o, out, r, g, b, a);
	}
}

/* ---------------------------------------------------------------- one stream: the stages */

static uint32_t
inflate_job(uint32_t unused0, uint32_t unused1)
{
	struct png *p = P;
	struct bits b;
	uint32_t total = (uint32_t)(p->row_bytes + 1) * (uint32_t)p->h, n;
	struct codes *tables = (struct codes *)(p->rows + p->rows_bytes);	/* (memory of this stage's own, after the rows) */

	(void)unused0, (void)unused1;
	memset(&b, 0, sizeof b);
	b.file_end = p->file + p->size;
	b.at = p->file + p->first_idat + 8;
	b.chunk_end = b.at + m_be32(p->file + p->first_idat);
	n = inflate(&b, p->raw, total, p->inflated, tables, 1);
	if (b.bad || n < total)
		*p->failed = 1;
	*p->inflated = total;
	return n;
}

/* A band of every row without its filter, following the inflating, and the band before it a row behind. */
static uint32_t
unfilter_job(uint32_t band, uint32_t unused)
{
	struct png *p = P;
	int y, n = p->row_bytes, x0 = p->band_at[band], x1 = p->band_at[band + 1];

	(void)unused;
	for (y = 0; y < p->h; y++) {
		const uint8_t *in = p->raw + (uint32_t)y * (uint32_t)(n + 1);
		uint8_t *out = p->rows + (uint32_t)y * (uint32_t)p->row_pitch;

		if (!m_wait(p->inflated, (uint32_t)(y + 1) * (uint32_t)(n + 1), p->failed))
			break;
		if (band && !m_wait(p->banded[band - 1], (uint32_t)y + 1, p->failed))
			break;
		if (!unfilter(in[0], in + 1, out, y ? out - p->row_pitch : NULL, x0, x1, p->pixel_bytes))
			*p->failed = 1;
		*p->banded[band] = (uint32_t)y + 1;
	}
	*p->banded[band] = (uint32_t)p->h;
	return (uint32_t)y;
}

static uint32_t
pixels_job(uint32_t unused0, uint32_t unused1)
{
	struct png *p = P;
	int y;

	(void)unused0, (void)unused1;
	for (y = 0; y < p->h; y++) {
		if (!m_wait(p->banded[p->bands - 1], (uint32_t)y + 1, p->failed))
			break;
		row_pixels(p, p->rows + (uint32_t)y * (uint32_t)p->row_pitch, p->out->pixels + (uint32_t)y * (uint32_t)p->out->stride);
	}
	return (uint32_t)y;
}

/* The pipeline's items: 0 inflates, 1 to bands unfilter a band each, the last makes the pixels. */
static uint32_t
one_stream_stage(uint32_t k, uint32_t unused)
{
	struct png *p = P;

	(void)unused;
	if (!k)
		return p->whole ? 0 : inflate_job(0, 0);
	return k <= (uint32_t)p->bands ? unfilter_job(k - 1, 0) : pixels_job(0, 0);
}

/* The rows' pixels over every core, once all are unfiltered. */
static void
stream_pixels(void *ctx, uint32_t from, uint32_t to)
{
	struct png *p = ctx;
	uint32_t y;

	for (y = from; y < to; y++)
		row_pixels(p, p->rows + y * (uint32_t)p->row_pitch, p->out->pixels + y * (uint32_t)p->out->stride);
}

/* Whether rows without their filters are the output's pixels as they stand, a band whole 16 bytes of them. */
static int
rows_are_pixels(const struct png *p)
{
	const struct media_image *o = p->out;

	return p->depth == 8 && !((uintptr_t)o->pixels & 15) && o->stride == p->row_pitch &&
	       ((p->type == 2 && !p->trns && o->channels == 3) || (p->type == 6 && o->channels == 4 && !o->bgr && !o->opaque));
}

/* The IDATs put together, whole 16 bytes on a core: 300 KB on one core ended 60 passes full. */
struct joining { const uint8_t *file; uint8_t *to; uint32_t bytes, chunks; const uint32_t *at; };

static void
join_pieces(void *ctx, uint32_t from, uint32_t to)
{
	const struct joining *j = ctx;
	uint32_t place = from * 16, end = to * 16 < j->bytes ? to * 16 : j->bytes, c = 0;

	/* (at: pairs of where a chunk's bytes lie in the file, and where they go) */
	while (c + 1 < j->chunks && j->at[2 * c + 3] <= place)
		c++;
	while (place < end) {
		uint32_t in = place - j->at[2 * c + 1], left = (c + 1 < j->chunks ? j->at[2 * c + 3] : j->bytes) - place;

		if (left > end - place)
			left = end - place;
		memcpy(j->to + place, j->file + j->at[2 * c] + in, left);
		place += left, c++;
	}
}

/*
 * The whole stream into p->raw on every core. The IDAT chunks lie with headers between: a stream of
 * one chunk is read where it lies, others are put together first. 0: not so, and one core is to do it.
 */
static int
inflate_whole(struct png *p, uint32_t total, int workers)
{
	const uint8_t *f = p->file;
	uint32_t at = p->first_idat, bytes = 0, chunks = 0, made = 0, length = 0, room = 0;
	uint8_t *joined = NULL;
	const uint8_t *in = f + at + 8;
	int ok;

	while (at + 12 <= p->size && !memcmp(f + at + 4, "IDAT", 4)) {
		length = m_be32(f + at);
		if (length > p->size - at - 12)
			return 0;
		bytes += length, chunks++;
		at += 12 + length;
	}
	if (bytes < WHOLE_LEAST || workers < 1 || chunks > WHOLE_CHUNKS)
		return 0;
	if (chunks > 1) {
		struct joining j;
		uint32_t *table, c, put = 0;

		room = ((bytes + 15) & ~15u) + chunks * 8;
		if (!(joined = m_map(room)))
			return 0;
		table = (uint32_t *)(joined + ((bytes + 15) & ~15u));
		for (at = p->first_idat, c = 0; c < chunks; c++, at += 12 + length) {
			length = m_be32(f + at);
			table[2 * c] = at + 8, table[2 * c + 1] = put;
			put += length;
		}
		j.file = f, j.to = joined, j.bytes = bytes, j.chunks = chunks, j.at = table;
		m_for((bytes + 15) / 16, join_pieces, &j, 16, workers);
		in = joined;
	}
	/* (past the zlib header; a stream that cannot be shared or says otherwise is read again by one core) */
	ok = !m_inflate_stream_plain(in + 2, bytes - 2, p->raw, total, &made, workers) && made == total;
	if (joined)
		m_unmap(joined, room);
	return ok;
}

/* ---------------------------------------------------------------- segments */

static void
segment_jobs(void *ctx, uint32_t from, uint32_t to)
{
	struct png *p = ctx;
	struct codes *tables = p->seg_tables + 3 * mcw_self();
	uint32_t k;

	for (k = from; k < to; k++) {
		const uint8_t *chunk = p->file + p->seg_chunk[k];
		uint32_t rows = p->seg_row[k + 1] - p->seg_row[k], line = (uint32_t)p->row_bytes + 1, total = rows * line, y;
		uint8_t *raw = p->raw + p->seg_raw[k];
		struct bits b;

		memset(&b, 0, sizeof b);
		b.file_end = p->file + p->size;
		b.at = chunk + 8;
		b.chunk_end = b.at + m_be32(chunk);
		b.one_chunk = 1;
		if (inflate(&b, raw, total, NULL, tables, k == 0) < total || b.bad) {
			*p->failed = 1;
			continue;
		}
		/* in place: a row's bytes move one to the left over its filter's byte */
		for (y = 0; y < rows; y++) {
			uint8_t *row = raw + y * line;

			if (!unfilter(row[0], row + 1, row, y ? row - line : NULL, 0, p->row_bytes, p->pixel_bytes))
				*p->failed = 1;
			/* (segments on whole 16 rows of the output: their pixels too, in the same job) */
			if (p->seg_pixels)
				row_pixels(p, row, p->out->pixels + (p->seg_row[k] + y) * (uint32_t)p->out->stride);
		}
	}
}

/* The row that is row y of the picture, after the segments are unfiltered. */
static inline const uint8_t *
segment_row(const struct png *p, int y)
{
	int k = 0;

	while (k + 1 < p->segments && (uint32_t)y >= p->seg_row[k + 1])
		k++;
	return p->raw + p->seg_raw[k] + ((uint32_t)y - p->seg_row[k]) * (uint32_t)(p->row_bytes + 1);
}

static void
segment_pixels(void *ctx, uint32_t from, uint32_t to)
{
	struct png *p = ctx;
	uint32_t y;

	for (y = from; y < to; y++)
		row_pixels(p, segment_row(p, (int)y), p->out->pixels + y * (uint32_t)p->out->stride);
}

/* ---------------------------------------------------------------- interlaced */

static void
pass_jobs(void *ctx, uint32_t from, uint32_t to)
{
	struct png *p = ctx;
	uint32_t k;
	int y;

	for (k = from; k < to; k++) {
		uint32_t line = (uint32_t)p->pass_rb[k] + 1;

		for (y = 0; y < p->pass_h[k]; y++) {
			const uint8_t *in = p->raw + p->pass_at[k] + (uint32_t)y * line;
			uint8_t *out = p->pass_rows[k] + (uint32_t)y * (uint32_t)p->pass_rb[k];

			if (!unfilter(in[0], in + 1, out, y ? out - p->pass_rb[k] : NULL, 0, p->pass_rb[k], p->pixel_bytes))
				*p->failed = 1;
		}
	}
}

static void
interlaced_pixels(void *ctx, uint32_t from, uint32_t to)
{
	struct png *p = ctx;
	const struct media_image *o = p->out;
	uint32_t y;
	int x, k, r, g, b, a;

	for (y = from; y < to; y++) {
		uint8_t *out = o->pixels + y * (uint32_t)o->stride;

		for (x = 0; x < p->w; x++) {
			/* the pass a pixel is in, by where it lies in its 8 x 8 */
			int px = x & 7, py = (int)y & 7;

			for (k = 6; k >= 0; k--)
				if ((px - adam_x0[k]) % adam_dx[k] == 0 && px >= adam_x0[k] && (py - adam_y0[k]) % adam_dy[k] == 0 && py >= adam_y0[k])
					break;
			png_pixel(p, p->pass_rows[k] + (uint32_t)(((int)y - adam_y0[k]) / adam_dy[k]) * (uint32_t)p->pass_rb[k],
				  (x - adam_x0[k]) / adam_dx[k], &r, &g, &b, &a);
			m_put(o, out + x * o->channels, r, g, b, a);
		}
	}
}

/* ---------------------------------------------------------------- the header */

static const char *
header(struct png *p)
{
	const uint8_t *f = p->file;
	uint32_t at = 8, i;
	int idats = 0, have_segments = 0;

	if (p->size < 33 || m_be32(f + 8) != 13 || memcmp(f + 12, "IHDR", 4) != 0)
		return "not a PNG picture";
	p->w = (int)m_be32(f + 16);
	p->h = (int)m_be32(f + 20);
	p->depth = f[24];
	p->type = f[25];
	p->interlaced = f[28] == 1;
	p->channels = p->type == 2 ? 3 : p->type == 4 ? 2 : p->type == 6 ? 4 : 1;
	if ((p->type != 0 && p->type != 2 && p->type != 3 && p->type != 4 && p->type != 6) || f[28] > 1 ||
	    (p->depth != 8 && p->depth != 16 && !(p->depth < 8 && (p->type == 0 || p->type == 3))) || (p->type == 3 && p->depth == 16))
		return "a PNG picture of a kind this does not read";
	if (p->w < 1 || p->h < 1)
		return "a PNG picture that is damaged";
	p->row_bytes = (int)(((uint64_t)p->w * p->channels * p->depth + 7) / 8);
	p->pixel_bytes = p->channels * p->depth / 8 ? p->channels * p->depth / 8 : 1;
	for (i = 0; i < 256; i++)
		p->palette[i][3] = 255;
	while (at + 12 <= p->size) {
		uint32_t length = m_be32(f + at);
		const uint8_t *data = f + at + 8;

		if (at + 12 + length > p->size)
			break;
		if (!memcmp(f + at + 4, "PLTE", 4)) {
			for (i = 0; i < length / 3 && i < 256; i++)
				memcpy(p->palette[i], data + 3 * i, 3);
		} else if (!memcmp(f + at + 4, "tRNS", 4)) {
			p->trns = 1;
			if (p->type == 3)
				for (i = 0; i < length && i < 256; i++)
					p->palette[i][3] = data[i];
			else if (p->type == 0 && length >= 2)
				p->trns_gray = (int)(data[0] << 8 | data[1]) & (p->depth == 16 ? 0xffff : (1 << p->depth) - 1);
			else if (p->type == 2 && length >= 6) {
				p->trns_r = data[0] << 8 | data[1], p->trns_g = data[2] << 8 | data[3], p->trns_b = data[4] << 8 | data[5];
				if (p->depth == 8)
					p->trns_r &= 255, p->trns_g &= 255, p->trns_b &= 255;
			}
			if (p->type == 3)
				p->trns = 0;	/* (in the palette's alpha) */
		} else if (!memcmp(f + at + 4, SEGMENTS_CHUNK, 4) && length >= 8 && !p->interlaced) {
			uint32_t n = m_be32(data);

			if (n >= 1 && n <= SEGMENTS_MOST && length == 4 + 4 * n) {
				p->segments = (int)n;
				for (i = 0; i < n; i++)
					p->seg_row[i] = m_be32(data + 4 + 4 * i);
				p->seg_row[n] = (uint32_t)p->h;
				have_segments = p->seg_row[0] == 0;
				for (i = 0; i < n; i++)
					if (p->seg_row[i + 1] <= p->seg_row[i])
						have_segments = 0;
			}
		} else if (!memcmp(f + at + 4, "IDAT", 4)) {
			if (!p->first_idat)
				p->first_idat = at;
			if (idats < SEGMENTS_MOST)
				p->seg_chunk[idats] = at;
			idats++;
		} else if (!memcmp(f + at + 4, "IEND", 4)) {
			break;
		}
		at += 12 + length;
	}
	if (!p->first_idat)
		return "a PNG picture with nothing in it";
	if (!have_segments || idats != p->segments)
		p->segments = 0;
	return NULL;
}

const char *
m_png_probe(const uint8_t *f, uint32_t size, struct media_info *info)
{
	static struct png probe;
	const char *why;

	memset(&probe, 0, sizeof probe);
	probe.file = f, probe.size = size;
	if ((why = header(&probe)) != NULL)
		return why;
	info->kind = MEDIA_PNG;
	info->width = probe.w, info->height = probe.h;
	info->alpha = probe.type == 4 || probe.type == 6 || probe.trns || probe.type == 3;
	info->parallel = probe.segments > 1;
	return NULL;
}

const char *
m_png_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers)
{
	static struct png job;
	struct png *p = &job;
	const char *why;
	int got = 0, k;
	uint32_t line;

	memset(p, 0, sizeof *p);
	P = p;
	p->file = f, p->size = size, p->out = out;
	if ((why = header(p)) != NULL)
		return why;
	line = (uint32_t)p->row_bytes + 1;
	/* (the stages' counts, made once and kept) */
	{
		static volatile uint32_t *failed, *inflated, *banded[M_BANDS];

		if (!failed) {
			failed = mcw_alloc(16), inflated = mcw_alloc(16);
			for (k = 0; k < M_BANDS; k++)
				banded[k] = mcw_alloc(16);
		}
		p->failed = failed, p->inflated = inflated;
		for (k = 0; k < M_BANDS; k++)
			p->banded[k] = banded[k];
	}
	*p->failed = 0;
	if (p->segments) {
		uint32_t at = 0;

		/* each segment's rows begin on 16 bytes: no two cores store beside each other */
		for (k = 0; k < p->segments; k++) {
			p->seg_raw[k] = at;
			at += ((p->seg_row[k + 1] - p->seg_row[k]) * line + 15) & ~15u;
		}
		p->seg_raw[p->segments] = p->raw_bytes = at;
		if (!(p->raw = m_scratch(M_PNG_RAW, p->raw_bytes + 16)))
			return "not enough memory for the picture";
		/* (a small picture is this core's: a round of the workers costs more) */
		got = m_workers_begin(p->raw_bytes < 32768 ? 0 : workers);
		p->seg_pixels = !((uintptr_t)out->pixels & 15);
		for (k = 1; k < p->segments; k++)
			if ((p->seg_row[k] * (uint32_t)out->stride) & 15)
				p->seg_pixels = 0;
		if (!(p->seg_tables = m_scratch(M_PNG_TABLES, (uint32_t)(mcw_count() + 1) * 3 * sizeof(struct codes)))) {
			m_workers_end();
			m_scratch_done(M_PNG_RAW, p->raw, p->raw_bytes + 16);
			return "not enough memory for the picture";
		}
		m_for((uint32_t)p->segments, segment_jobs, p, 0, got);
		if (!*p->failed && !p->seg_pixels)
			m_for((uint32_t)p->h, segment_pixels, p, (uint32_t)out->stride, got);
	} else if (p->interlaced) {
		uint32_t at = 0, room = 0;

		for (k = 0; k < 7; k++) {
			p->pass_w[k] = p->w > adam_x0[k] ? (p->w - adam_x0[k] + adam_dx[k] - 1) / adam_dx[k] : 0;
			p->pass_h[k] = p->h > adam_y0[k] ? (p->h - adam_y0[k] + adam_dy[k] - 1) / adam_dy[k] : 0;
			if (!p->pass_w[k])
				p->pass_h[k] = 0;
			p->pass_rb[k] = (int)(((uint64_t)p->pass_w[k] * p->channels * p->depth + 7) / 8);
			p->pass_at[k] = at;
			at += (uint32_t)p->pass_h[k] * (uint32_t)(p->pass_rb[k] + (p->pass_h[k] ? 1 : 0));
		}
		p->pass_at[7] = p->raw_bytes = at;
		p->rows_bytes = 0;
		for (k = 0; k < 7; k++)
			room += ((uint32_t)p->pass_h[k] * (uint32_t)p->pass_rb[k] + 15) & ~15u;
		p->rows_bytes = room;
		got = m_workers_begin(workers);
		if (!(p->raw = m_scratch(M_PNG_RAW, p->raw_bytes + 16)) || !(p->rows = m_scratch(M_PNG_ROWS, room + 3 * sizeof(struct codes) + 16)))
			why = "not enough memory for the picture";
		else {
			struct bits b;
			uint32_t n;

			for (at = 0, k = 0; k < 7; k++) {
				p->pass_rows[k] = p->rows + at;
				at += ((uint32_t)p->pass_h[k] * (uint32_t)p->pass_rb[k] + 15) & ~15u;
			}
			memset(&b, 0, sizeof b);
			b.file_end = f + size;
			b.at = f + p->first_idat + 8;
			b.chunk_end = b.at + m_be32(f + p->first_idat);
			n = inflate(&b, p->raw, p->raw_bytes, NULL, (struct codes *)(p->rows + room), 1);
			if (b.bad || n < p->raw_bytes)
				*p->failed = 1;
			if (!*p->failed)
				m_for(7, pass_jobs, p, 0, got);
			if (!*p->failed)
				m_for((uint32_t)p->h, interlaced_pixels, p, (uint32_t)out->stride, got);
		}
	} else {
		uint32_t total = line * (uint32_t)p->h;
		int want = workers > M_BANDS ? M_BANDS : workers, direct;

		p->row_pitch = (p->row_bytes + 15) & ~15;
		p->raw_bytes = total + 16;
		p->rows_bytes = (uint32_t)p->row_pitch * (uint32_t)p->h;
		if (!(p->raw = m_scratch(M_PNG_RAW, p->raw_bytes + 16)))
			return "not enough memory for the picture";
		*p->inflated = 0;
		/* (a large stream has a part a core, up to all it is given) */
		if (workers > want && size / 16384 > (uint32_t)want)
			want = (int)(size / 16384) < workers ? (int)(size / 16384) : workers;
		got = m_workers_begin(size >= WHOLE_LEAST ? want : workers > 5 ? 5 : workers);
		p->whole = !getenv("MEDIA_PNG_ONE") && inflate_whole(p, total, got);
		direct = p->whole && rows_are_pixels(p);
		p->rows = direct ? out->pixels : m_scratch(M_PNG_ROWS, p->rows_bytes + 3 * sizeof(struct codes) + 16);
		if (!p->rows) {
			m_workers_end();
			m_scratch_done(M_PNG_RAW, p->raw, p->raw_bytes + 16);
			return "not enough memory for the picture";
		}
		/* a band a worker (the last writes the pixels, with one core inflating); a band is 96 bytes of a row or more */
		p->bands = p->whole ? got : got - 1;
		if (!p->whole && p->bands > 4)
			p->bands = 4;
		if (p->bands > M_BANDS)
			p->bands = M_BANDS;
		if (p->bands < 1)
			p->bands = 1;
		while (p->bands > 1 && p->row_bytes / p->bands < 96)
			p->bands--;
		for (k = 0; k < p->bands; k++) {
			p->band_at[k] = p->row_bytes / p->bands * k / 48 * 48;
			*p->banded[k] = 0;
		}
		p->band_at[p->bands] = p->row_bytes;
		if (p->whole) {
			*p->inflated = total;
			/* (no stage of pixels: those come after, or are these) */
			m_stages((uint32_t)p->bands + 1, one_stream_stage, got);
			if (!direct && !*p->failed)
				m_for((uint32_t)p->h, stream_pixels, p, (uint32_t)out->stride, got);
		} else {
			/* the longest stage is this core's own: it waits for nobody until the end */
			m_stages((uint32_t)p->bands + 2, one_stream_stage, got);
		}
		if (direct)
			p->rows = NULL;
	}
	if (!why && *p->failed)
		why = "the picture is damaged";
	m_scratch_done(M_PNG_RAW, p->raw, p->raw_bytes + 16);
	m_scratch_done(M_PNG_ROWS, p->rows, p->rows_bytes + 3 * sizeof(struct codes) + 16);
	m_scratch_done(M_PNG_TABLES, p->seg_tables, (uint32_t)(mcw_count() + 1) * 3 * sizeof(struct codes));
	m_workers_end();
	return why;
}

/* ---------------------------------------------------------------- deflate in segments (media_inflate) */

struct segments { const uint8_t *file; uint32_t size, piece, count, total; uint8_t *out; struct codes *tables; volatile uint32_t *failed; };

static void
segments_job(void *ctx, uint32_t from, uint32_t to)
{
	struct segments *s = ctx;
	struct codes *tables = s->tables + 3 * mcw_self();
	uint32_t k;

	for (k = from; k < to; k++) {
		uint32_t at = m_le32(s->file + 16 + 4 * k), end = m_le32(s->file + 20 + 4 * k);
		uint32_t want = k + 1 < s->count ? s->piece : s->total - k * s->piece;
		struct bits b;

		memset(&b, 0, sizeof b);
		b.file_end = s->file + s->size;
		b.at = s->file + at, b.chunk_end = s->file + end, b.one_chunk = 1;
		if (at > end || end > s->size || inflate(&b, s->out + k * s->piece, want, NULL, tables, 0) < want || b.bad)
			*s->failed = 1;
	}
}

uint32_t
media_inflated_size(const void *file, uint32_t size)
{
	const uint8_t *f = file;

	return size >= 16 && !memcmp(f, "MDZ1", 4) ? m_le32(f + 4) : 0;
}

const char *
media_inflate(const void *file, uint32_t size, void *out, uint32_t out_size, int workers)
{
	static volatile uint32_t *failed;
	struct segments s;
	uint32_t tables_bytes;
	int got;

	s.file = file, s.size = size, s.out = out;
	if (!(s.total = media_inflated_size(file, size)) || s.total > out_size)
		return "not deflate in segments, or larger than the room for it";
	s.piece = m_le32(s.file + 8), s.count = m_le32(s.file + 12);
	if (!s.piece || (s.piece & 15) || s.count != (s.total + s.piece - 1) / s.piece || 20 + 4 * s.count > size)
		return "the segments' table is damaged";
	if (!failed && !(failed = mcw_alloc(16)))
		return "not enough memory";
	*failed = 0;
	got = m_workers_begin(workers < (int)s.count - 1 ? workers : (int)s.count - 1);	/* (no more than the segments want) */
	tables_bytes = (uint32_t)(mcw_count() + 1) * 3 * sizeof(struct codes);
	if (!(s.tables = m_scratch(M_PNG_TABLES, tables_bytes))) {
		m_workers_end();
		return "not enough memory";
	}
	s.failed = failed;
	m_for(s.count, segments_job, &s, s.piece, got);
	m_scratch_done(M_PNG_TABLES, s.tables, tables_bytes);
	m_workers_end();
	return *failed ? "a segment is damaged" : NULL;
}
