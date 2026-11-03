/*
 * The PNG encoder (docs/system/media.md): the rows are cut into segments, and every core filters and
 * deflates segments of its own. Each segment is an IDAT of its own whose stream refers to
 * nothing before it, ends on a byte, and whose first row is filtered without the row above;
 * the mdPR chunk lists where they begin, so this library decodes them on every core too. The
 * zlib stream's check and each chunk's are put together from each segment's on this core.
 */
#include <stdlib.h>
#include "m_int.h"

#define SEG_MOST 256
#define BLOCK_SYMBOLS 8192
#define HASH_BITS 14
#define ENC_WORKERS 32	/* each has some 150 KB of its own, mapped here first */
#define WINDOW 32768

struct seg {
	uint32_t row0, rows;
	uint8_t *out;		/* the compressed bytes (a mapping of their own) */
	uint32_t room, bytes, adler, crc, raw_bytes;
};

struct penc {
	const struct media_image *in;
	int channels, chain, last_seg;	/* 3 or 4 written; the longest chain looked down */
	uint32_t row_bytes;
	struct seg seg[SEG_MOST];
	int segs;
	uint8_t *per_core;		/* each core's work and rows, core_bytes apart (core 0 first) */
	uint32_t core_bytes, per_core_bytes;
};

/* ---------------------------------------------------------------- checks */

static uint32_t crc_table[256];

static void
crc_make(void)
{
	uint32_t c, n, k;

	if (crc_table[1])
		return;
	for (n = 0; n < 256; n++) {
		for (c = n, k = 0; k < 8; k++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}

static uint32_t
crc_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
	return ~crc;
}

/* zlib's crc32_combine: the check of A then B from both and B's length. */
static uint32_t
gf2_times(const uint32_t *mat, uint32_t vec)
{
	uint32_t sum = 0;

	while (vec) {
		if (vec & 1)
			sum ^= *mat;
		vec >>= 1;
		mat++;
	}
	return sum;
}

static void
gf2_square(uint32_t *square, const uint32_t *mat)
{
	int n;

	for (n = 0; n < 32; n++)
		square[n] = gf2_times(mat, mat[n]);
}

static uint32_t
crc_combine(uint32_t crc1, uint32_t crc2, uint32_t len2)
{
	uint32_t even[32], odd[32], row = 1;
	int n;

	if (!len2)
		return crc1;
	odd[0] = 0xedb88320u;
	for (n = 1; n < 32; n++)
		odd[n] = row, row <<= 1;
	gf2_square(even, odd);
	gf2_square(odd, even);
	do {
		gf2_square(even, odd);
		if (len2 & 1)
			crc1 = gf2_times(even, crc1);
		len2 >>= 1;
		if (!len2)
			break;
		gf2_square(odd, even);
		if (len2 & 1)
			crc1 = gf2_times(odd, crc1);
		len2 >>= 1;
	} while (len2);
	return crc1 ^ crc2;
}

#define ADLER_BASE 65521u

static uint32_t
adler_update(uint32_t adler, const uint8_t *p, uint32_t n)
{
	uint32_t a = adler & 0xffff, b = adler >> 16;

	while (n) {
		uint32_t k = n < 5552 ? n : 5552;

		n -= k;
		while (k--)
			a += *p++, b += a;
		a %= ADLER_BASE, b %= ADLER_BASE;
	}
	return b << 16 | a;
}

static uint32_t
adler_combine(uint32_t adler1, uint32_t adler2, uint32_t len2)
{
	uint32_t rem = len2 % ADLER_BASE, sum1 = adler1 & 0xffff, sum2 = rem * sum1 % ADLER_BASE;

	sum1 += (adler2 & 0xffff) + ADLER_BASE - 1;
	sum2 += (adler1 >> 16) + (adler2 >> 16) + ADLER_BASE - rem;
	if (sum1 >= ADLER_BASE) sum1 -= ADLER_BASE;
	if (sum1 >= ADLER_BASE) sum1 -= ADLER_BASE;
	if (sum2 >= ADLER_BASE << 1) sum2 -= ADLER_BASE << 1;
	if (sum2 >= ADLER_BASE) sum2 -= ADLER_BASE;
	return sum2 << 16 | sum1;
}

/* ---------------------------------------------------------------- bits out */

struct out { uint8_t *p, *end; uint32_t hold; int count; };

static inline void
put(struct out *o, uint32_t bits, int n)
{
	o->hold |= bits << o->count;
	o->count += n;
	while (o->count >= 8) {
		if (o->p < o->end)
			*o->p++ = (uint8_t)o->hold;
		o->hold >>= 8;
		o->count -= 8;
	}
}

static inline void
align(struct out *o)
{
	if (o->count)
		put(o, 0, 8 - o->count);
}

/* ---------------------------------------------------------------- Huffman codes */

/* Code lengths for the counts, none over `most` (counts halved until none is). */
static void
lengths_of(const uint32_t *count_in, int n, int most, uint8_t *length)
{
	uint32_t count[288], weight[2 * 288];
	int parent[2 * 288], heap[288], hn, i, nodes, used, deepest;

	memcpy(count, count_in, (size_t)n * 4);
	for (;;) {
		memset(length, 0, (size_t)n);
		hn = 0;
		for (i = 0; i < n; i++)
			if (count[i])
				heap[hn++] = i, weight[i] = count[i];
		if (hn == 0)
			return;
		if (hn == 1) {
			length[heap[0]] = 1;
			return;
		}
		used = hn;
		nodes = n;
		/* a plain selection: n is small */
		while (hn > 1) {
			int a = 0, b = 1, k;

			if (weight[heap[b]] < weight[heap[a]])
				a = 1, b = 0;
			for (k = 2; k < hn; k++)
				if (weight[heap[k]] < weight[heap[a]])
					b = a, a = k;
				else if (weight[heap[k]] < weight[heap[b]])
					b = k;
			weight[nodes] = weight[heap[a]] + weight[heap[b]];
			parent[heap[a]] = parent[heap[b]] = nodes;
			if (a > b) { int t = a; a = b; b = t; }
			heap[a] = nodes++;
			heap[b] = heap[--hn];
		}
		parent[heap[0]] = -1;
		deepest = 0;
		for (i = 0; i < n; i++)
			if (count[i]) {
				int d = 0, k = i;

				while (parent[k] >= 0)
					k = parent[k], d++;
				length[i] = (uint8_t)d;
				if (d > deepest)
					deepest = d;
			}
		(void)used;
		if (deepest <= most)
			return;
		for (i = 0; i < n; i++)
			if (count[i])
				count[i] = (count[i] + 1) / 2;
	}
}

/* Codes from lengths, turned round (deflate sends a code's bits from the highest down, read lowest first). */
static void
codes_of(const uint8_t *length, int n, uint16_t *code)
{
	int bl_count[16] = {0}, next[16], i, c = 0, b;

	for (i = 0; i < n; i++)
		bl_count[length[i]]++;
	bl_count[0] = 0;
	for (b = 1; b < 16; b++) {
		c = (c + bl_count[b - 1]) << 1;
		next[b] = c;
	}
	for (i = 0; i < n; i++)
		if (length[i]) {
			int v = next[length[i]]++, r = 0, k;

			for (k = 0; k < length[i]; k++)
				r |= (v >> k & 1) << (length[i] - 1 - k);
			code[i] = (uint16_t)r;
		}
}

static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static inline int
length_code(int len)
{
	int c = 0;

	while (c < 28 && length_base[c + 1] <= len)
		c++;
	return c;
}

static inline int
distance_code(int d)
{
	int c = 0;

	while (c < 29 && distance_base[c + 1] <= d)
		c++;
	return c;
}

/* The workspace of a segment's core. */
struct work {
	int32_t head[1 << HASH_BITS];
	int32_t *prev;			/* WINDOW words where chains are followed (above level 1), else NULL */
	uint32_t sym[BLOCK_SYMBOLS];	/* literal: byte; match: 1 << 31 | length << 16 | distance */
	uint32_t lit[286], dist[30];
	uint8_t *raw, *prev_row, *cur;	/* a segment's filtered rows, the row before and this one */
};

/* One block of symbols, dynamic codes, or stored when that is shorter. */
static void
block_out(struct out *o, struct work *w, int nsym, const uint8_t *raw, uint32_t raw_n, int final)
{
	uint8_t ll[286 + 30], dl[30], cl[19], all[286 + 30];
	uint16_t lc[286], dc[30], cc[19];
	uint32_t cl_count[19] = {0}, bits = 0;
	int hlit = 286, hdist = 30, hclen = 19, i, k, n;
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint8_t rle[286 + 30];
	uint8_t rle_extra[286 + 30];
	int nrle = 0;

	w->lit[256] = 1;
	lengths_of(w->lit, 286, 15, ll);
	lengths_of(w->dist, 30, 15, dl);
	/* at least one distance code, as some decoders want (only where there is none: another made
	 * beside used ones over-subscribes the code) */
	for (i = 0; i < 30 && !dl[i]; i++)
		;
	if (i == 30)
		dl[0] = 1;
	while (hlit > 257 && !ll[hlit - 1])
		hlit--;
	while (hdist > 1 && !dl[hdist - 1])
		hdist--;
	memcpy(all, ll, hlit);
	memcpy(all + hlit, dl, hdist);
	/* the lengths in runs: 16 repeats the last 3-6 times, 17 and 18 zeros */
	for (i = 0; i < hlit + hdist;) {
		int v = all[i], run = 1;

		while (i + run < hlit + hdist && all[i + run] == v)
			run++;
		if (v == 0 && run >= 3) {
			int r = run > 138 ? 138 : run;

			rle[nrle] = (uint8_t)(r >= 11 ? 18 : 17), rle_extra[nrle++] = (uint8_t)(r >= 11 ? r - 11 : r - 3);
			i += r;
		} else if (v != 0 && run >= 4) {
			int r = run - 1 > 6 ? 6 : run - 1;

			rle[nrle] = (uint8_t)v, rle_extra[nrle++] = 0;
			rle[nrle] = 16, rle_extra[nrle++] = (uint8_t)(r - 3);
			i += 1 + r;
		} else {
			rle[nrle] = (uint8_t)v, rle_extra[nrle++] = 0;
			i++;
		}
	}
	for (i = 0; i < nrle; i++)
		cl_count[rle[i]]++;
	lengths_of(cl_count, 19, 7, cl);
	while (hclen > 4 && !cl[order[hclen - 1]])
		hclen--;
	codes_of(ll, 286, lc);
	codes_of(dl, 30, dc);
	codes_of(cl, 19, cc);
	/* how long it comes to, against the bytes stored */
	bits = 3 + 14 + 3 * (uint32_t)hclen;
	for (i = 0; i < nrle; i++)
		bits += cl[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
	for (i = 0; i < 286; i++)
		bits += w->lit[i] * (ll[i] + (i > 256 ? length_extra[i - 257] : 0));
	for (i = 0; i < 30; i++)
		bits += w->dist[i] * (dl[i] + distance_extra[i]);
	if (bits / 8 > raw_n + 5 + 5 * (raw_n / 65535)) {
		/* stored: in pieces of 65,535 */
		uint32_t at = 0;

		do {
			uint32_t piece = raw_n - at > 65535 ? 65535 : raw_n - at;

			put(o, final && at + piece == raw_n ? 1 : 0, 1);
			put(o, 0, 2);
			align(o);
			put(o, piece, 16);
			put(o, ~piece & 0xffff, 16);
			for (k = 0; k < (int)piece; k++)
				put(o, raw[at + (uint32_t)k], 8);
			at += piece;
		} while (at < raw_n);
		goto cleared;
	}
	put(o, final ? 1 : 0, 1);
	put(o, 2, 2);
	put(o, (uint32_t)(hlit - 257), 5);
	put(o, (uint32_t)(hdist - 1), 5);
	put(o, (uint32_t)(hclen - 4), 4);
	for (i = 0; i < hclen; i++)
		put(o, cl[order[i]], 3);
	for (i = 0; i < nrle; i++) {
		put(o, cc[rle[i]], cl[rle[i]]);
		if (rle[i] == 16)
			put(o, rle_extra[i], 2);
		else if (rle[i] == 17)
			put(o, rle_extra[i], 3);
		else if (rle[i] == 18)
			put(o, rle_extra[i], 7);
	}
	for (n = 0; n < nsym; n++) {
		uint32_t s = w->sym[n];

		if (!(s >> 31)) {
			put(o, lc[s], ll[s]);
		} else {
			int len = (int)(s >> 16 & 0x7fff), d = (int)(s & 0xffff), c = length_code(len), e = distance_code(d);

			put(o, lc[257 + c], ll[257 + c]);
			if (length_extra[c])
				put(o, (uint32_t)(len - length_base[c]), length_extra[c]);
			put(o, dc[e], dl[e]);
			if (distance_extra[e])
				put(o, (uint32_t)(d - distance_base[e]), distance_extra[e]);
		}
	}
	put(o, lc[256], ll[256]);
cleared:
	memset(w->lit, 0, sizeof w->lit);
	memset(w->dist, 0, sizeof w->dist);
}

/* Deflates in[0..n): greedy matches down hash chains of `chain` at most. */
static void
deflate(struct out *o, struct work *w, const uint8_t *in, uint32_t n, int chain, int final)
{
	uint32_t i = 0, block_at = 0;
	int nsym = 0, final_done = 0;

	for (i = 0; i < (1u << HASH_BITS); i++)
		w->head[i] = -1;
	memset(w->lit, 0, sizeof w->lit);
	memset(w->dist, 0, sizeof w->dist);
	i = 0;
	while (i < n) {
		int best = 0, best_d = 0;

		if (i + 3 <= n) {
			uint32_t h = ((uint32_t)in[i] << 16 | in[i + 1] << 8 | in[i + 2]) * 2654435761u >> (32 - HASH_BITS);
			int32_t cand = w->head[h], tries = w->prev ? chain : 1;
			uint32_t most = n - i > 258 ? 258 : n - i;

			while (cand >= 0 && i - (uint32_t)cand <= WINDOW - 1 && tries--) {
				const uint8_t *a = in + cand, *b = in + i;

				if (a[best] == b[best] && a[0] == b[0]) {
					uint32_t l = 0;

					while (l < most && a[l] == b[l])
						l++;
					if ((int)l > best) {
						best = (int)l, best_d = (int)(i - (uint32_t)cand);
						if (l == most)
							break;
					}
				}
				cand = w->prev ? w->prev[cand & (WINDOW - 1)] : -1;
			}
			if (w->prev)
				w->prev[i & (WINDOW - 1)] = w->head[h];
			w->head[h] = (int32_t)i;
		}
		if (best >= 3) {
			uint32_t k;

			w->sym[nsym++] = 1u << 31 | (uint32_t)best << 16 | (uint32_t)best_d;
			w->lit[257 + length_code(best)]++;
			w->dist[distance_code(best_d)]++;
			/* the bytes the match covers go into the chains too (with no chains, only its last) */
			for (k = w->prev ? 1 : (uint32_t)best - 1; k < (uint32_t)best && i + k + 3 <= n; k++) {
				uint32_t h = ((uint32_t)in[i + k] << 16 | in[i + k + 1] << 8 | in[i + k + 2]) * 2654435761u >> (32 - HASH_BITS);

				if (w->prev)
					w->prev[(i + k) & (WINDOW - 1)] = w->head[h];
				w->head[h] = (int32_t)(i + k);
			}
			i += (uint32_t)best;
		} else {
			w->sym[nsym++] = in[i];
			w->lit[in[i]]++;
			i++;
		}
		if (nsym == BLOCK_SYMBOLS) {
			final_done = final && i == n;
			block_out(o, w, nsym, in + block_at, i - block_at, final_done);
			nsym = 0, block_at = i;
		}
	}
	if (!final_done && (nsym || final || n == 0))
		block_out(o, w, nsym, in + block_at, i - block_at, final);
	if (!final) {
		/* an empty stored block: the stream ends on a byte here, and goes on in the next segment */
		put(o, 0, 3);
		align(o);
		put(o, 0, 16);
		put(o, 0xffff, 16);
	}
	align(o);
}

/* ---------------------------------------------------------------- segments */

/* Row y's bytes as written (3 or 4 a pixel, red first). */
static void
row_bytes_of(const struct penc *e, int y, uint8_t *to)
{
	const struct media_image *in = e->in;
	const uint8_t *p = in->pixels + (uint32_t)y * (uint32_t)in->stride;
	int x;

	if (in->channels == e->channels && !in->bgr) {
		memcpy(to, p, e->row_bytes);
		return;
	}
	for (x = 0; x < in->width; x++, p += in->channels, to += e->channels) {
		if (in->bgr)
			to[0] = p[2], to[1] = p[1], to[2] = p[0];
		else
			to[0] = p[0], to[1] = p[1], to[2] = p[2];
		if (e->channels == 4)
			to[3] = p[3];
	}
}

static inline uint32_t
magnitude8(int d)
{
	d &= 255;
	return (uint32_t)(d < 128 ? d : 256 - d);
}

/* Row y filtered into `to` (its filter's byte first): None, Sub or Up, the least sum of
 * magnitudes (Average and Paeth are most of a decoder's time, and saved 0 to 2%); a segment's
 * first row, with no row above it, None or Sub. */
static void
filter_row(const uint8_t *cur, const uint8_t *prev, int n, int bpp, uint8_t *to)
{
	uint32_t s0 = 0, s1 = 0, s2 = prev ? 0 : 0xffffffffu;
	int x;

	for (x = 0; x < n; x++) {
		int v = cur[x];

		s0 += magnitude8(v);
		s1 += magnitude8(x >= bpp ? v - cur[x - bpp] : v);
		if (prev)
			s2 += magnitude8(v - prev[x]);
	}
	if (s2 < s0 && s2 < s1) {
		to[0] = 2;
		for (x = 0; x < n; x++)
			to[1 + x] = (uint8_t)(cur[x] - prev[x]);
	} else if (s1 < s0) {
		to[0] = 1;
		for (x = 0; x < bpp && x < n; x++)
			to[1 + x] = cur[x];
		for (; x < n; x++)
			to[1 + x] = (uint8_t)(cur[x] - cur[x - bpp]);
	} else {
		to[0] = 0;
		memcpy(to + 1, cur, (size_t)n);
	}
}

static void
segment_job(void *ctx, uint32_t from, uint32_t to)
{
	struct penc *e = ctx;
	struct work *w = (struct work *)(e->per_core + (uint32_t)mcw_self() * e->core_bytes);
	uint32_t line = e->row_bytes + 1, k;

	for (k = from; k < to; k++) {
		struct seg *s = &e->seg[k];
		struct out o;
		uint32_t y;

		for (y = 0; y < s->rows; y++) {
			uint8_t *swap_row;

			row_bytes_of(e, (int)(s->row0 + y), w->cur);
			filter_row(w->cur, y ? w->prev_row : NULL, (int)e->row_bytes, e->channels, w->raw + y * line);
			swap_row = w->prev_row, w->prev_row = w->cur, w->cur = swap_row;
		}
		s->raw_bytes = s->rows * line;
		s->adler = adler_update(1, w->raw, s->raw_bytes);
		o.p = s->out, o.end = s->out + s->room, o.hold = 0, o.count = 0;
		deflate(&o, w, w->raw, s->raw_bytes, e->chain, (int)k == e->last_seg);
		s->bytes = o.p >= o.end ? 0 : (uint32_t)(o.p - s->out);
		s->crc = crc_update(0, s->out, s->bytes);
	}
}

uint8_t *
media_png_encode(const struct media_image *image, int level, uint32_t *size, int workers)
{
	static struct penc job;
	struct penc *e = &job;
	uint32_t total_raw, at, adler = 1, i;
	uint8_t *f = NULL, *p;
	int k, got, rows_a_seg;
	static const int chains[10] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 1024};

	memset(e, 0, sizeof *e);
	crc_make();
	e->in = image;
	e->channels = image->channels == 4 && !image->bgr ? 4 : 3;
	e->chain = chains[level < 0 ? 0 : level > 9 ? 9 : level];
	e->row_bytes = (uint32_t)image->width * (uint32_t)e->channels;
	total_raw = (e->row_bytes + 1) * (uint32_t)image->height;
	/* segments of 16 rows (more where SEG_MOST would not reach), each 16 KB of rows or more */
	rows_a_seg = 16;
	while ((image->height + rows_a_seg - 1) / rows_a_seg > SEG_MOST || (uint32_t)rows_a_seg * (e->row_bytes + 1) < 16384)
		rows_a_seg += 16;
	(void)total_raw;
	for (k = 0, at = 0; at < (uint32_t)image->height; k++) {
		struct seg *s = &e->seg[k];

		s->row0 = at;
		s->rows = (uint32_t)image->height - at < (uint32_t)rows_a_seg ? (uint32_t)image->height - at : (uint32_t)rows_a_seg;
		s->room = s->rows * (e->row_bytes + 1) + s->rows * (e->row_bytes + 1) / 64 + 1024;
		at += s->rows;
		if (!(s->out = m_map(s->room)))
			goto failed;
	}
	e->segs = k;
	e->last_seg = k - 1;
	got = m_workers_begin(workers > ENC_WORKERS ? ENC_WORKERS : workers);
	/* each core's memory mapped here, its pages there before any job (a worker stops at a new one) */
	e->core_bytes = ((uint32_t)sizeof(struct work) + 15) & ~15u;
	if (e->chain > 1)
		e->core_bytes += WINDOW * 4;
	e->core_bytes += ((uint32_t)rows_a_seg * (e->row_bytes + 1) + 15) & ~15u;
	e->core_bytes += 2 * ((e->row_bytes + 15) & ~15u);
	e->per_core_bytes = e->core_bytes * (uint32_t)(mcw_count() + 1);
	if (!(e->per_core = m_map(e->per_core_bytes))) {
		m_workers_end();
		goto failed;
	}
	for (k = 0; k <= mcw_count(); k++) {
		uint8_t *at = e->per_core + (uint32_t)k * e->core_bytes;
		struct work *w = (struct work *)at;

		at += ((uint32_t)sizeof(struct work) + 15) & ~15u;
		w->prev = NULL;
		if (e->chain > 1)
			w->prev = (int32_t *)at, at += WINDOW * 4;
		w->raw = at, at += ((uint32_t)rows_a_seg * (e->row_bytes + 1) + 15) & ~15u;
		w->prev_row = at, at += (e->row_bytes + 15) & ~15u;
		w->cur = at;
	}
	m_for((uint32_t)e->segs, segment_job, e, 0, got);
	m_workers_end();
	m_unmap(e->per_core, e->per_core_bytes);
	e->per_core = NULL;
	/* the file: signature, IHDR, mdPR, an IDAT a segment, IEND */
	at = 8 + 25 + 12 + 4 + 4 * (uint32_t)e->segs + 12;
	for (k = 0; k < e->segs; k++) {
		if (!e->seg[k].bytes)
			goto failed;
		at += 12 + e->seg[k].bytes + (k == 0 ? 2 : 0) + (k == e->last_seg ? 4 : 0);
	}
	*size = at;
	if (!(f = m_map(*size)))
		goto failed;
	p = f;
#define BE32(v) (p[0] = (uint8_t)((v) >> 24), p[1] = (uint8_t)((v) >> 16), p[2] = (uint8_t)((v) >> 8), p[3] = (uint8_t)(v), p += 4)
	memcpy(p, "\x89PNG\r\n\x1a\n", 8), p += 8;
	BE32(13u);
	memcpy(p, "IHDR", 4), p += 4;
	BE32((uint32_t)image->width);
	BE32((uint32_t)image->height);
	p[0] = 8, p[1] = (uint8_t)(e->channels == 4 ? 6 : 2), p[2] = p[3] = p[4] = 0, p += 5;
	BE32(crc_update(0, p - 17, 17));
	BE32(4 + 4 * (uint32_t)e->segs);
	memcpy(p, "mdPR", 4), p += 4;
	BE32((uint32_t)e->segs);
	for (k = 0; k < e->segs; k++)
		BE32(e->seg[k].row0);
	BE32(crc_update(0, p - 8 - 4 * (uint32_t)e->segs, 8 + 4 * (uint32_t)e->segs));
	for (k = 0; k < e->segs; k++) {
		struct seg *s = &e->seg[k];
		uint32_t length = s->bytes + (k == 0 ? 2 : 0) + (k == e->last_seg ? 4 : 0), crc;
		uint8_t *start;

		adler = k ? adler_combine(adler, s->adler, s->raw_bytes) : s->adler;
		BE32(length);
		start = p;
		memcpy(p, "IDAT", 4), p += 4;
		if (k == 0) {
			p[0] = 0x78, p[1] = 0x01;	/* (deflate, a 32 KB window, the fastest level: what the header says does not matter) */
			p += 2;
		}
		crc = crc_update(0, start, (uint32_t)(p - start));
		memcpy(p, s->out, s->bytes), p += s->bytes;
		crc = crc_combine(crc, s->crc, s->bytes);
		if (k == e->last_seg) {
			uint8_t *a = p;

			BE32(adler);
			crc = crc_update(crc, a, 4);
		}
		BE32(crc);
	}
	BE32(0u);
	memcpy(p, "IEND", 4), p += 4;
	BE32(crc_update(0, (const uint8_t *)"IEND", 4));
#undef BE32
	for (i = 0; i < (uint32_t)e->segs; i++)
		m_unmap(e->seg[i].out, e->seg[i].room);
	return f;
failed:
	for (k = 0; k < SEG_MOST; k++)
		if (e->seg[k].out)
			m_unmap(e->seg[k].out, e->seg[k].room);
	return NULL;
}

/* ---------------------------------------------------------------- gzip (media_gzip) */

#define GZ_PIECE 65536

struct gzenc {
	const uint8_t *in;
	uint32_t n, pieces;
	int chain;
	struct seg seg[SEG_MOST];	/* row0: where the piece starts, rows: its bytes; crc: the input's */
	uint8_t *per_core;
	uint32_t core_bytes;
};

static void
gzip_job(void *ctx, uint32_t from, uint32_t to)
{
	struct gzenc *g = ctx;
	struct work *w = (struct work *)(g->per_core + (uint32_t)mcw_self() * g->core_bytes);
	uint32_t k;

	for (k = from; k < to; k++) {
		struct seg *s = &g->seg[k];
		struct out o;

		o.p = s->out, o.end = s->out + s->room, o.hold = 0, o.count = 0;
		deflate(&o, w, g->in + s->row0, s->rows, g->chain, k == g->pieces - 1);
		s->bytes = o.p >= o.end ? 0 : (uint32_t)(o.p - s->out);
		s->crc = crc_update(0, g->in + s->row0, s->rows);
	}
}

uint8_t *
media_gzip(const void *bytes, uint32_t n, int level, uint32_t *size, int workers)
{
	static struct gzenc job;
	static const int chains[10] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 1024};
	struct gzenc *g = &job;
	uint32_t k, at, crc = 0;
	uint8_t *f = NULL, *p;
	int got;

	memset(g, 0, sizeof *g);
	crc_make();
	g->in = bytes;
	g->n = n;
	g->chain = chains[level < 0 ? 0 : level > 9 ? 9 : level];
	g->pieces = n ? (n + GZ_PIECE - 1) / GZ_PIECE : 1;
	if (g->pieces > SEG_MOST)
		return NULL;
	for (k = 0; k < g->pieces; k++) {
		struct seg *s = &g->seg[k];

		s->row0 = k * GZ_PIECE;
		s->rows = n - s->row0 < GZ_PIECE ? n - s->row0 : GZ_PIECE;
		s->room = s->rows + s->rows / 64 + 1024;
		if (!(s->out = m_map(s->room)))
			goto failed;
	}
	got = m_workers_begin(workers > ENC_WORKERS ? ENC_WORKERS : workers);
	g->core_bytes = ((uint32_t)sizeof(struct work) + 15) & ~15u;
	if (g->chain > 1)
		g->core_bytes += WINDOW * 4;
	if (!(g->per_core = m_map(g->core_bytes * (uint32_t)(mcw_count() + 1)))) {
		m_workers_end();
		goto failed;
	}
	for (k = 0; k <= (uint32_t)mcw_count(); k++) {
		struct work *w = (struct work *)(g->per_core + k * g->core_bytes);

		w->prev = g->chain > 1 ? (int32_t *)((uint8_t *)w + (((uint32_t)sizeof(struct work) + 15) & ~15u)) : NULL;
	}
	m_for(g->pieces, gzip_job, g, 0, got);
	m_workers_end();
	m_unmap(g->per_core, g->core_bytes * (uint32_t)(mcw_count() + 1));
	g->per_core = NULL;
	at = 10 + 8;
	for (k = 0; k < g->pieces; k++) {
		if (!g->seg[k].bytes)
			goto failed;
		at += g->seg[k].bytes;
		crc = k ? crc_combine(crc, g->seg[k].crc, g->seg[k].rows) : g->seg[k].crc;
	}
	*size = at;
	if (!(f = m_map(at)))
		goto failed;
	p = f;
	memcpy(p, "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10), p += 10;	/* deflate, no name, no time, unknown system */
	for (k = 0; k < g->pieces; k++)
		memcpy(p, g->seg[k].out, g->seg[k].bytes), p += g->seg[k].bytes;
	p[0] = (uint8_t)crc, p[1] = (uint8_t)(crc >> 8), p[2] = (uint8_t)(crc >> 16), p[3] = (uint8_t)(crc >> 24);
	p[4] = (uint8_t)n, p[5] = (uint8_t)(n >> 8), p[6] = (uint8_t)(n >> 16), p[7] = (uint8_t)(n >> 24);
failed:
	for (k = 0; k < SEG_MOST; k++)
		if (g->seg[k].out)
			m_unmap(g->seg[k].out, g->seg[k].room);
	return f;
}
