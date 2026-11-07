/* The GIF encoder (docs/system/media.md, "GIF encoding"): a picture's own 256 colours or fewer, else
 * 6 x 7 x 6 fixed ones dithered, rows on every core. The LZW codes are pieces of rows, each begun
 * after a clear code and ended on a byte by a core of its own, then put one after another in
 * sub-blocks: a decoder sees one stream that clears now and then. */
#include <stdlib.h>
#include <sys/mman.h>
#include "m_int.h"

#define SET 512			/* a core's colours: slots, for up to 257 of them */
#define HASH 8192		/* a core's LZW strings: slots of key << 12 | code */
#define PIECE_LEAST 16384	/* pixels a piece at least: each starts its strings anew */
#define PIECES_MOST 1024

struct genc {
	const struct media_image *in;
	int w, h, alpha;		/* alpha: 4 channels whose fourth is kept (not bgr) */
	int colours, exact, see_through, min_code;
	uint32_t *sets, *counts;	/* each core's colours (key + 1, 0 empty) and how many (16 bytes a core) */
	uint32_t key[1024];		/* the exact colours: key + 1, and their index */
	uint8_t index[1024];
	uint8_t dither[3][16][256];	/* the fixed colours: a channel's part of the index, by threshold */
	uint8_t *indices;		/* w * h */
	uint32_t *table;		/* HASH words a core */
	uint32_t pieces, piece_rows, room;
	uint8_t *codes;			/* each piece's bytes, room apart */
	uint32_t *piece_bytes;		/* 16 bytes a piece */
	uint32_t *piece_at;		/* where each piece's bytes begin in the codes put together */
	uint8_t *file;
	uint32_t head, total, bytes;	/* the file's bytes before the codes; the codes'; the file's */
};

static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

/* A pixel's key: 0xRRGGBB, or 1 << 24 for one that is see-through. */
static inline uint32_t
key_of(const struct genc *e, const uint8_t *p)
{
	if (e->alpha && p[3] < 128)
		return 1u << 24;
	if (e->in->bgr)
		return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
	return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static inline uint32_t slot_of(uint32_t key, int bits) { return (key * 2654435761u) >> (32 - bits); }

/* ---------------------------------------------------------------- the colours */

static int
by_value(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

static void
count_rows(void *ctx, uint32_t from, uint32_t to)
{
	struct genc *e = ctx;
	int self = mcw_self(), x;
	uint32_t *set = e->sets + self * SET, *count = e->counts + self * 4, y, last = 0xffffffffu;

	for (y = from; y < to && *count <= 256; y++) {
		const uint8_t *p = e->in->pixels + y * (uint32_t)e->in->stride;

		for (x = 0; x < e->w; x++, p += e->in->channels) {
			uint32_t k = key_of(e, p), s;

			if (k == last)
				continue;
			last = k;
			for (s = slot_of(k, 9); set[s] && set[s] != k + 1; s = (s + 1) & (SET - 1))
				;
			if (!set[s]) {
				set[s] = k + 1;
				if (++*count > 256)
					return;
			}
		}
	}
}

static int
exact_index(const struct genc *e, uint32_t k)
{
	uint32_t s;

	for (s = slot_of(k, 10); e->key[s] != k + 1; s = (s + 1) & 1023)
		;
	return e->index[s];
}

static void
map_rows(void *ctx, uint32_t from, uint32_t to)
{
	struct genc *e = ctx;
	uint32_t y;
	int x;

	for (y = from; y < to; y++) {
		const uint8_t *p = e->in->pixels + y * (uint32_t)e->in->stride;
		uint8_t *o = e->indices + y * (uint32_t)e->w;

		if (e->exact) {
			uint32_t last = 0xffffffffu;
			int at = 0;

			for (x = 0; x < e->w; x++, p += e->in->channels) {
				uint32_t k = key_of(e, p);

				if (k != last)
					last = k, at = exact_index(e, k);
				o[x] = (uint8_t)at;
			}
			continue;
		}
		for (x = 0; x < e->w; x++, p += e->in->channels) {
			int d = bayer[(y & 3) * 4 + (x & 3)], r, g, b;

			if (e->alpha && p[3] < 128) {
				o[x] = 252;
				continue;
			}
			r = e->in->bgr ? p[2] : p[0], g = p[1], b = e->in->bgr ? p[0] : p[2];
			o[x] = (uint8_t)(e->dither[0][d][r] + e->dither[1][d][g] + e->dither[2][d][b]);
		}
	}
}

/* The 6 x 7 x 6 colours: a channel's level for a value at threshold d, times its place. */
static void
fixed_colours(struct genc *e, uint8_t palette[256][3])
{
	static const int levels[3] = { 6, 7, 6 }, place[3] = { 42, 6, 1 };
	int c, d, v, r, g, b;

	for (c = 0; c < 3; c++)
		for (d = 0; d < 16; d++)
			for (v = 0; v < 256; v++) {
				int l = (v * (levels[c] - 1) + (d * 2 + 1) * 255 / 32) / 255;

				e->dither[c][d][v] = (uint8_t)((l < levels[c] ? l : levels[c] - 1) * place[c]);
			}
	for (r = 0; r < 6; r++)
		for (g = 0; g < 7; g++)
			for (b = 0; b < 6; b++) {
				uint8_t *p = palette[r * 42 + g * 6 + b];

				p[0] = (uint8_t)(r * 51), p[1] = (uint8_t)((g * 255 + 3) / 6), p[2] = (uint8_t)(b * 51);
			}
	e->colours = 252 + e->see_through;
}

/* ---------------------------------------------------------------- LZW, a piece a core */

struct bits { uint8_t *o; uint32_t hold; int n; };

static inline void
put(struct bits *b, uint32_t code, int size)
{
	b->hold |= code << b->n;
	b->n += size;
	while (b->n >= 8) {
		*b->o++ = (uint8_t)b->hold;
		b->hold >>= 8, b->n -= 8;
	}
}

static void
lzw_pieces(void *ctx, uint32_t from, uint32_t to)
{
	struct genc *e = ctx;
	uint32_t *t = e->table + mcw_self() * HASH, piece;
	int clear = 1 << e->min_code, first_size = e->min_code + 1;

	for (piece = from; piece < to; piece++) {
		uint32_t y0 = piece * e->piece_rows, y1 = y0 + e->piece_rows, i, n, prefix;
		const uint8_t *in = e->indices + y0 * (uint32_t)e->w;
		struct bits b = { e->codes + piece * e->room, 0, 0 };
		int size = first_size, next = clear + 2;

		if (y1 > (uint32_t)e->h)
			y1 = (uint32_t)e->h;
		n = (y1 - y0) * (uint32_t)e->w;
		memset(t, 0, HASH * 4);
		if (!piece)
			put(&b, (uint32_t)clear, size);
		prefix = in[0];
		for (i = 1; i < n; i++) {
			uint32_t key = prefix << 8 | in[i], s = slot_of(key, 13), w;

			for (; (w = t[s]) != 0; s = (s + 1) & (HASH - 1))
				if (w >> 12 == key)
					break;
			if (w) {
				prefix = w & 4095;
				continue;
			}
			put(&b, prefix, size);
			t[s] = key << 12 | (uint32_t)next;
			/* (the decoder makes this string a code later: its width grows a code after ours) */
			if (++next > 1 << size && size < 12)
				size++;
			if (next == 4096) {
				put(&b, (uint32_t)clear, size);
				size = first_size, next = clear + 2;
				memset(t, 0, HASH * 4);
			}
			prefix = in[i];
		}
		put(&b, prefix, size);
		if (++next > 1 << size && size < 12)
			size++;
		if (piece + 1 < e->pieces) {
			/* a clear, then clears of the first width (odd) until the piece ends on a byte */
			put(&b, (uint32_t)clear, size);
			while (b.n)
				put(&b, (uint32_t)clear, first_size);
		} else {
			put(&b, (uint32_t)clear + 1, size);
			if (b.n)
				put(&b, 0, 8 - b.n);
		}
		e->piece_bytes[piece * 4] = (uint32_t)(b.o - (e->codes + piece * e->room));
	}
}

/* ---------------------------------------------------------------- the file */

/* The file's bytes from..to (whole pages of it, but the first and last) of the codes in sub-blocks:
 * a length byte, then 255 bytes of the codes (fewer in the last). */
#define PAGE 4096
static void
blocks_pages(void *ctx, uint32_t from, uint32_t to)
{
	struct genc *e = ctx;
	uint32_t area = e->total + (e->total + 254) / 255, o = from * PAGE, end = to * PAGE, k = 0;

	if (o < e->head)
		o = e->head;
	if (end > e->head + area)
		end = e->head + area;
	while (o < end) {
		uint32_t at = o - e->head, block = at / 256, in = at % 256, d = block * 255 + in - 1, n;

		if (!in) {
			e->file[o++] = (uint8_t)(e->total - block * 255 < 255 ? e->total - block * 255 : 255);
			continue;
		}
		n = 256 - in;
		if (n > end - o)
			n = end - o;
		if (n > e->total - d)
			n = e->total - d;
		/* (the codes' pieces one after another: the one d is in) */
		while (e->piece_at[k + 1] <= d)
			k++;
		if (n > e->piece_at[k + 1] - d)
			n = e->piece_at[k + 1] - d;
		memcpy(e->file + o, e->codes + k * e->room + (d - e->piece_at[k]), n);
		o += n;
	}
	/* the end and the trailer by the core of the last page (the first core storing to it after would share its texel) */
	if (to * PAGE >= e->bytes)
		e->file[e->bytes - 2] = 0, e->file[e->bytes - 1] = 0x3b;
}

uint8_t *
media_gif_encode(const struct media_image *image, uint32_t *size, int workers)
{
	static struct genc job;
	static uint32_t *kept_table;
	static uint32_t kept_cores;
	struct genc *e = &job;
	uint8_t palette[256][3], *f = NULL, *p;
	uint32_t found[256];
	uint32_t pixels, k, codes_bytes = 0, cores, s;
	int got, bits, c;

	memset(e, 0, sizeof *e);
	e->in = image, e->w = image->width, e->h = image->height;
	e->alpha = image->channels == 4 && !image->bgr;
	if (e->w < 1 || e->h < 1 || e->w > 65535 || e->h > 65535 || (image->channels != 3 && image->channels != 4))
		return NULL;
	pixels = (uint32_t)e->w * (uint32_t)e->h;
	got = m_workers_begin(workers);
	cores = (uint32_t)mcw_count() + 1;
	e->sets = m_map(cores * SET * 4);
	e->counts = m_map(cores * 16);
	e->indices = m_map(pixels);
	if (!e->sets || !e->counts || !e->indices)
		goto done;

	/* the colours: each core's, then all of them here (more than 256: the fixed ones) */
	m_for((uint32_t)e->h, count_rows, e, 0, got);
	memset(palette, 0, sizeof palette);
	e->exact = 1;
	for (c = 0; c < (int)cores && e->exact; c++) {
		if (e->counts[c * 4] > 256) {
			e->exact = 0;
			break;
		}
		for (k = 0; k < SET; k++) {
			uint32_t key = e->sets[c * SET + k];

			if (!key--)
				continue;
			for (s = slot_of(key, 10); e->key[s] && e->key[s] != key + 1; s = (s + 1) & 1023)
				;
			if (e->key[s])
				continue;
			if (e->colours == 256) {
				e->exact = 0;
				break;
			}
			e->key[s] = key + 1;
			found[e->colours++] = key;
		}
	}
	/* (in the colours' order, not the cores': the same file from any number of workers) */
	qsort(found, (size_t)e->colours, 4, by_value);
	for (k = 0; e->exact && k < (uint32_t)e->colours; k++) {
		for (s = slot_of(found[k], 10); e->key[s] != found[k] + 1; s = (s + 1) & 1023)
			;
		e->index[s] = (uint8_t)k;
		if (found[k] >> 24)
			e->see_through = 1;	/* (its palette entry stays black) */
		else
			palette[k][0] = (uint8_t)(found[k] >> 16), palette[k][1] = (uint8_t)(found[k] >> 8), palette[k][2] = (uint8_t)found[k];
	}
	if (!e->exact) {
		e->see_through = e->alpha;	/* (an index kept for see-through pixels, whether there are any) */
		fixed_colours(e, palette);
	}
	m_for((uint32_t)e->h, map_rows, e, (uint32_t)e->w, got);

	/* the codes: pieces of rows; a minimum code size that is even makes the first width odd */
	for (bits = 1; (1 << bits) < e->colours; bits++)
		;
	e->min_code = bits < 2 ? 2 : (bits + 1) & ~1;
	/* (by the picture alone, not the workers: the same file from any number of them) */
	e->piece_rows = (PIECE_LEAST + (uint32_t)e->w - 1) / (uint32_t)e->w;
	if (e->piece_rows * PIECES_MOST < (uint32_t)e->h)
		e->piece_rows = ((uint32_t)e->h + PIECES_MOST - 1) / PIECES_MOST;
	e->pieces = ((uint32_t)e->h + e->piece_rows - 1) / e->piece_rows;
	/* (12 bits a pixel at worst, and the clears) */
	e->room = (e->piece_rows * (uint32_t)e->w * 2 + 64 + 15) & ~15u;
	codes_bytes = e->room * e->pieces;
	if (cores > kept_cores) {
		m_unmap(kept_table, kept_cores * HASH * 4);
		kept_cores = (kept_table = m_map(cores * HASH * 4)) ? cores : 0;
	}
	e->table = kept_table;
	/* room for the codes: only the pages written are memory, and a quarter of each piece's is there first */
	e->codes = mmap(NULL, (codes_bytes + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	e->piece_bytes = m_map(e->pieces * 16);
	if (!e->table || e->codes == MAP_FAILED || !e->piece_bytes) {
		e->codes = NULL;
		goto done;
	}
	for (k = 0; k < e->pieces; k++)
		mcw_touch(e->codes + k * e->room, e->room / 4);
	m_for(e->pieces, lzw_pieces, e, 0, got);

	/* the file: the header here, the codes in sub-blocks on every core, whole pages each */
	e->piece_at = m_map((e->pieces + 1) * 4);
	if (!e->piece_at)
		goto done;
	for (k = 0; k < e->pieces; k++)
		e->piece_at[k + 1] = e->piece_at[k] + e->piece_bytes[k * 4];
	e->total = e->piece_at[e->pieces];
	e->head = 13 + 3u * (2u << (bits - 1)) + (e->see_through ? 8 : 0) + 10 + 1;
	*size = e->head + e->total + (e->total + 254) / 255 + 1 + 1;
	if (!(f = m_map(*size)))
		goto done;
	p = f;
	memcpy(p, "GIF89a", 6), p += 6;
	*p++ = (uint8_t)e->w, *p++ = (uint8_t)(e->w >> 8), *p++ = (uint8_t)e->h, *p++ = (uint8_t)(e->h >> 8);
	*p++ = (uint8_t)(0x80 | 0x70 | (bits - 1)), *p++ = 0, *p++ = 0;
	memcpy(p, palette, 3u << bits), p += 3u << bits;
	if (e->see_through) {
		int clear_index = e->exact ? exact_index(e, 1u << 24) : 252;

		*p++ = 0x21, *p++ = 0xf9, *p++ = 4, *p++ = 1, *p++ = 0, *p++ = 0, *p++ = (uint8_t)clear_index, *p++ = 0;
	}
	*p++ = 0x2c, *p++ = 0, *p++ = 0, *p++ = 0, *p++ = 0;
	*p++ = (uint8_t)e->w, *p++ = (uint8_t)(e->w >> 8), *p++ = (uint8_t)e->h, *p++ = (uint8_t)(e->h >> 8), *p++ = 0;
	*p++ = (uint8_t)e->min_code;
	e->file = f;
	e->bytes = *size;
	m_for((*size + PAGE - 1) / PAGE, blocks_pages, e, 0, got);
done:
	m_workers_end();
	m_unmap(e->sets, cores * SET * 4);
	m_unmap(e->counts, cores * 16);
	m_unmap(e->indices, pixels);
	m_unmap(e->codes, codes_bytes);
	m_unmap(e->piece_bytes, e->pieces * 16);
	m_unmap(e->piece_at, (e->pieces + 1) * 4);
	return f;
}

/* ---------------------------------------------------------------- animations */

/* An animation: every frame a picture of the encoder's (over every core) with its colours as its
 * own table, after a control block (its delay, its see-through index), all on one canvas that
 * loops `loops` times (0: for ever). Frames are the canvas's size. */
uint8_t *
media_gif_anim_encode(const struct media_image *frames, int count, const int *delays_ms, int loops, uint32_t *size,
		      int workers)
{
	uint8_t *out = NULL, *grown;
	uint32_t used = 0, room = 0;
	int n, got;

	if (count < 1 || frames[0].width < 1 || frames[0].height < 1)
		return NULL;
	got = m_workers_begin(workers);
	for (n = 0; n < count; n++) {
		uint32_t bytes, at, need;
		uint8_t *one, *p, gct, table[768];
		int bits = 0, see = -1, k;

		if (frames[n].width != frames[0].width || frames[n].height != frames[0].height ||
		    !(one = media_gif_encode(&frames[n], &bytes, got)))
			goto failed;
		/* the picture's own file: its table, a see-through index, then the image and its codes */
		gct = one[10];
		bits = (gct & 7) + 1;
		memcpy(table, one + 13, 3u << bits);
		at = 13 + (3u << bits);
		if (one[at] == 0x21 && one[at + 1] == 0xf9) {
			if (one[at + 3] & 1)
				see = one[at + 6];
			at += 8;
		}
		need = used + 19 + 8 + 10 + (3u << bits) + (bytes - at - 10) + 1;
		if (need > room) {
			room = need * 2 + 4096;
			if (!(grown = realloc(out, room))) {
				m_unmap(one, bytes);
				goto failed;
			}
			out = grown;
		}
		p = out + used;
		if (!n) {
			memcpy(p, "GIF89a", 6), p += 6;
			*p++ = (uint8_t)frames[0].width, *p++ = (uint8_t)(frames[0].width >> 8);
			*p++ = (uint8_t)frames[0].height, *p++ = (uint8_t)(frames[0].height >> 8);
			*p++ = 0, *p++ = 0, *p++ = 0;	/* (no table of the canvas's: each frame has its own) */
			memcpy(p, "\x21\xff\x0bNETSCAPE2.0\x03\x01", 16), p += 16;
			*p++ = (uint8_t)loops, *p++ = (uint8_t)(loops >> 8), *p++ = 0;
		}
		k = delays_ms ? (delays_ms[n] + 5) / 10 : 10;
		*p++ = 0x21, *p++ = 0xf9, *p++ = 4;
		/* (a frame is the whole canvas: one with see-through pixels is cleared after, so that the
		 * one before does not show through it; another is left in place) */
		*p++ = (uint8_t)(see >= 0 ? 2 << 2 | 1 : 1 << 2);
		*p++ = (uint8_t)k, *p++ = (uint8_t)(k >> 8), *p++ = (uint8_t)(see >= 0 ? see : 0), *p++ = 0;
		/* the image descriptor with its own table, then the codes as they were */
		memcpy(p, one + at, 9), p += 9;
		*p++ = (uint8_t)(0x80 | (bits - 1));
		memcpy(p, table, 3u << bits), p += 3u << bits;
		memcpy(p, one + at + 10, bytes - at - 10 - 1), p += bytes - at - 10 - 1;	/* (all but the trailer) */
		used = (uint32_t)(p - out);
		m_unmap(one, bytes);
	}
	m_workers_end();
	/* the file in the library's own memory, as every encoder gives it (media_free) */
	if (!(grown = m_map(used + 1))) {
		free(out);
		return NULL;
	}
	memcpy(grown, out, used);
	grown[used] = 0x3b;
	free(out);
	*size = used + 1;
	return grown;
failed:
	m_workers_end();
	free(out);
	return NULL;
}
