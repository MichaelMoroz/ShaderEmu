/*
 * One deflate stream inflated on many cores (docs/system/media.md, "One deflate stream on many cores").
 *
 * The compressed bytes are cut into as many parts as there are cores. A core finds the first
 * block of its part (a dynamic block whose header and codes are whole and which decodes: a bad
 * one sends it on looking) and inflates in the same go to the first such block past its part,
 * into 16-bit values: a byte, or which byte of the 32 KB before its part a copy took (not known
 * yet). The values are a ring, of 32 KB where the stream's size is not known; the parts are then
 * followed from the stream's start, each window made from the one before over every core, and
 * a part all of whose values are there has them made bytes into its place; another is inflated
 * again, now knowing its window. The first part is made in place at once. CRC-32 on the way,
 * joined at the end.
 */
#include <stdlib.h>
#include "m_int.h"

/* ---------------------------------------------------------------- CRC-32 */

static uint32_t crc_table[8][256];

static void
crc_init(void)
{
	uint32_t i, k, c;

	if (crc_table[0][1])
		return;
	for (i = 0; i < 256; i++) {
		for (c = i, k = 0; k < 8; k++)
			c = c & 1 ? (c >> 1) ^ 0xedb88320u : c >> 1;
		crc_table[0][i] = c;
	}
	for (i = 0; i < 256; i++)
		for (k = 1; k < 8; k++)
			crc_table[k][i] = (crc_table[k - 1][i] >> 8) ^ crc_table[0][crc_table[k - 1][i] & 255];
}

uint32_t
media_crc32(uint32_t crc, const void *data, uint32_t bytes)
{
	const uint8_t *p = data;

	crc_init();
	crc = ~crc;
	while (bytes && ((uintptr_t)p & 3))
		crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 255], bytes--;
	for (; bytes >= 8; bytes -= 8, p += 8) {
		uint32_t a = crc ^ *(const uint32_t *)p, b = *(const uint32_t *)(p + 4);

/* (a byte's place in a table as a shift and a mask: the machine has no shifted add) */
#define T(k, at) (*(const uint32_t *)((const uint8_t *)crc_table[k] + (at)))
		crc = T(7, (a << 2) & 0x3fc) ^ T(6, (a >> 6) & 0x3fc) ^ T(5, (a >> 14) & 0x3fc) ^ T(4, (a >> 22) & 0x3fc) ^
		      T(3, (b << 2) & 0x3fc) ^ T(2, (b >> 6) & 0x3fc) ^ T(1, (b >> 14) & 0x3fc) ^ T(0, (b >> 22) & 0x3fc);
#undef T
	}
	while (bytes--)
		crc = (crc >> 8) ^ crc_table[0][(crc ^ *p++) & 255];
	return ~crc;
}

/* (a times b) modulo the CRC's polynomial, bit-reflected; x^(2^n) for the join */
static uint32_t
mult_mod(uint32_t a, uint32_t b)
{
	uint32_t m = 1u << 31, p = 0;

	for (;;) {
		if (a & m) {
			p ^= b;
			if (!(a & (m - 1)))
				break;
		}
		m >>= 1;
		b = b & 1 ? (b >> 1) ^ 0xedb88320u : b >> 1;
	}
	return p;
}

uint32_t
media_crc32_join(uint32_t crc1, uint32_t crc2, uint32_t bytes2)
{
	uint32_t p = 1u << 31, x = 1u << 30, n = bytes2, k;

	/* x^(8 * bytes2): x^(2^k) squared up as the bits of 8 * bytes2 call for it */
	for (k = 0; k < 3; k++)
		x = mult_mod(x, x);
	while (n) {
		if (n & 1)
			p = mult_mod(x, p);
		n >>= 1;
		x = mult_mod(x, x);
	}
	return mult_mod(p, crc1) ^ crc2;
}

/* ---------------------------------------------------------------- bits and codes */

struct bits { const uint8_t *in; uint32_t size, pos, hold; int count, over; };

static inline void
bits_at(struct bits *b, const uint8_t *in, uint32_t size, uint32_t bit)
{
	b->in = in, b->size = size, b->pos = bit >> 3, b->hold = 0, b->count = 0, b->over = 0;
	if (bit & 7) {
		b->hold = b->pos < size ? in[b->pos] >> (bit & 7) : 0;
		b->count = 8 - (int)(bit & 7);
		b->pos++;
	}
}

static inline uint32_t bits_pos(const struct bits *b) { return b->pos * 8 - (uint32_t)b->count; }

static inline void
need(struct bits *b, int n)
{
	while (b->count < n) {
		uint32_t byte = 0;

		if (b->pos < b->size)
			byte = b->in[b->pos];
		else
			b->over++;
		b->pos++;
		b->hold |= byte << b->count;
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

#define QB 10
struct huff { uint16_t quick[1 << QB], count[16], symbol[288]; };
enum { CODES, LENS, DISTS };

/* The canonical code of these lengths; -1 for a set no good encoder makes (as zlib refuses). */
static int
huff_make(struct huff *h, const uint8_t *lengths, int n, int kind)
{
	uint16_t offset[16], next[16];
	int i, code = 0, left = 1, most = 0;

	memset(h->count, 0, sizeof h->count);
	memset(h->quick, 0, sizeof h->quick);
	for (i = 0; i < n; i++)
		h->count[lengths[i]]++;
	h->count[0] = 0;
	for (i = 1; i < 16; i++) {
		left <<= 1;
		left -= h->count[i];
		if (left < 0)
			return -1;
		if (h->count[i])
			most = i;
	}
	if (!most)
		return kind == DISTS ? 0 : -1;	/* (a block of literals alone has no distances) */
	if (left > 0 && (kind == CODES || most != 1))
		return -1;
	offset[1] = 0;
	for (i = 1; i < 15; i++)
		offset[i + 1] = offset[i] + h->count[i];
	for (i = 1; i <= 15; i++) {
		next[i] = (uint16_t)code;
		code = (code + h->count[i]) << 1;
	}
	for (i = 0; i < n; i++) {
		int length = lengths[i];

		if (!length)
			continue;
		h->symbol[offset[length]++] = (uint16_t)i;
		if (length <= QB) {
			int straight = next[length], turned = 0, k;

			for (k = 0; k < length; k++)
				turned |= (straight >> k & 1) << (length - 1 - k);
			for (k = turned; k < 1 << QB; k += 1 << length)
				h->quick[k] = (uint16_t)(length << 12 | i);
		}
		next[length]++;
	}
	return 0;
}

static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
					   2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* The code lengths' code, of 7 bits at most: length << 8 | symbol by the next 7 bits, 0 where none. A header's search
 * tries many that come to nothing here: the two codes' tables, of kilobytes, are made only for one that holds. */
static int
meta_make(uint16_t table[128], const uint8_t lengths[19])
{
	uint8_t count[8] = {0};
	uint16_t next[9];
	int i, k, code = 0, left = 1;

	for (i = 0; i < 19; i++)
		count[lengths[i]]++;
	count[0] = 0;
	for (i = 1; i < 8; i++) {
		next[i] = (uint16_t)code;
		code = (code + count[i]) << 1;
		left = (left << 1) - count[i];
	}
	if (left)
		return -1;
	memset(table, 0, 128 * sizeof *table);
	for (i = 0; i < 19; i++) {
		int length = lengths[i], turned = 0, straight;

		if (!length)
			continue;
		straight = next[length]++;
		for (k = 0; k < length; k++)
			turned |= (straight >> k & 1) << (length - 1 - k);
		for (k = turned; k < 128; k += 1 << length)
			table[k] = (uint16_t)(length << 8 | i);
	}
	return 0;
}

/* A dynamic block's header from the bits after its type: its two codes. -1 if not a good one. */
static int
dynamic_codes(struct bits *b, struct huff *lit, struct huff *dist)
{
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint8_t lengths[320];
	uint16_t table[128];
	int hlit = (int)take(b, 5) + 257, hdist = (int)take(b, 5) + 1, hclen = (int)take(b, 4) + 4, i;

	if (hlit > 286 || hdist > 30)
		return -1;
	memset(lengths, 0, 19);
	for (i = 0; i < hclen; i++)
		lengths[order[i]] = (uint8_t)take(b, 3);
	if (meta_make(table, lengths))
		return -1;
	for (i = 0; i < hlit + hdist;) {
		int entry, symbol, repeat, value;

		need(b, 7);
		entry = table[b->hold & 127];
		if (!entry)
			return -1;
		b->hold >>= entry >> 8;
		b->count -= entry >> 8;
		symbol = entry & 255;
		if (symbol < 16) {
			lengths[i++] = (uint8_t)symbol;
			continue;
		}
		if (symbol == 16) {
			if (!i)
				return -1;
			value = lengths[i - 1];
			repeat = 3 + (int)take(b, 2);
		} else if (symbol == 17) {
			value = 0;
			repeat = 3 + (int)take(b, 3);
		} else {
			value = 0;
			repeat = 11 + (int)take(b, 7);
		}
		if (i + repeat > hlit + hdist)
			return -1;
		while (repeat--)
			lengths[i++] = (uint8_t)value;
	}
	if (!lengths[256])
		return -1;
	if (huff_make(lit, lengths, hlit, LENS) || huff_make(dist, lengths + hlit, hdist, DISTS))
		return -1;
	return b->over > 4 ? -1 : 0;
}

static void
fixed_codes(struct huff *lit, struct huff *dist)
{
	uint8_t lengths[288];
	int i;

	for (i = 0; i < 288; i++)
		lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
	huff_make(lit, lengths, 288, LENS);
	for (i = 0; i < 32; i++)
		lengths[i] = 5;	/* (30 and 31 complete the code and never come) */
	huff_make(dist, lengths, 32, LENS);
}

/* ---------------------------------------------------------------- one part's inflate */

#define WINDOW 32768u
#define MARK 0x8000u		/* a 16-bit value with this bit: byte (value & 0x7fff) of the window before the part */

struct dec {
	struct bits b;
	struct huff lit, dist;
	/* bytes: out[0..n) of cap, the window's wlen bytes before out[0] */
	uint8_t *out;
	uint32_t n, cap, wlen;
	const uint8_t *window;
	/* values: a ring of mask + 1 (WINDOW or more), rn made so far, the last that was a mark (+1) */
	uint16_t *ring;
	uint32_t rn, mask;
	int last, bad;
	uint32_t back_length, back_distance;	/* the copy BLOCK_BACK stopped at, its bits taken */
};

enum { BLOCK_OK, BLOCK_BAD, BLOCK_FULL, BLOCK_BACK };	/* BACK: a copy from before the bytes' window (wanted as values) */

/* A code's symbol the slow way (longer than QB bits) from 15 bits or more in hold; -1 if none. */
static inline int
slow_bits(uint32_t *hold, int *count, const struct huff *h)
{
	int code = 0, first = 0, index = 0, length;

	for (length = 1; length <= 15; length++) {
		int n = h->count[length];

		code |= (int)(*hold & 1);
		*hold >>= 1;
		(*count)--;
		if (code - n < first)
			return h->symbol[index + (code - first)];
		index += n;
		first += n;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/*
 * A block's symbols to its end, as bytes (wide 0) or values (wide 1). The reader and the output
 * are locals here: a store through a pointer could be one to *d, for all the compiler knows.
 */
static inline int
symbols(struct dec *d, int wide)
{
	const uint8_t *in = d->b.in;
	const struct huff *lit = &d->lit, *dist = &d->dist;
	uint32_t size = d->b.size, pos = d->b.pos, hold = d->b.hold, n = d->n, cap = d->cap, rn = d->rn;
	uint8_t *out = d->out;
	uint16_t *ring = d->ring;
	uint32_t mask = d->mask;
	int count = d->b.count, r = BLOCK_OK;
/* 25 bits or more in hold; past the input, zeros */
#define FILL() do { \
		if (pos + 4 <= size) { \
			while (count <= 24) \
				hold |= (uint32_t)in[pos++] << count, count += 8; \
		} else { \
			while (count <= 24) \
				hold |= (uint32_t)(pos < size ? in[pos] : 0) << count, pos++, count += 8; \
		} \
	} while (0)
#define DECODE(symbol, h) do { \
		uint16_t q_ = (h)->quick[hold & ((1 << QB) - 1)]; \
		if (q_) { \
			hold >>= q_ >> 12, count -= q_ >> 12; \
			symbol = q_ & 0xfff; \
		} else \
			symbol = slow_bits(&hold, &count, h); \
	} while (0)
#define BITS(v, k) do { v = hold & ((1u << (k)) - 1); hold >>= (k); count -= (k); } while (0)

	for (;;) {
		int symbol, ds, extra;
		uint32_t length, distance, more;

		FILL();
		DECODE(symbol, lit);
		if (symbol < 256) {
			if (symbol < 0) {
				r = BLOCK_BAD;
				break;
			}
			if (wide) {
				ring[rn++ & mask] = (uint16_t)symbol;
			} else {
				if (n >= cap) {
					r = BLOCK_FULL;
					break;
				}
				out[n++] = (uint8_t)symbol;
			}
			continue;
		}
		if (symbol == 256)
			break;
		if (symbol > 285) {
			r = BLOCK_BAD;
			break;
		}
		extra = length_extra[symbol - 257];
		BITS(more, extra);
		length = length_base[symbol - 257] + more;
		FILL();
		DECODE(ds, dist);
		if (ds < 0 || ds >= 30) {
			r = BLOCK_BAD;
			break;
		}
		extra = distance_extra[ds];
		if (extra > 8)
			FILL();
		BITS(more, extra);
		distance = distance_base[ds] + more;
		if (wide) {
			uint32_t stop = rn + length;

			if (distance > rn + WINDOW) {
				r = BLOCK_BAD;
				break;
			}
			/* from before the part: marks, until the copy reaches what the part made */
			for (; rn < stop && distance > rn; rn++)
				ring[rn & mask] = (uint16_t)(MARK | (WINDOW - (distance - rn)));
			if (rn < stop && stop <= mask + 1) {
				/* (not round the ring's end: this is most of the values) */
				uint16_t *to = ring + rn, *end = ring + stop;
				const uint16_t *from = to - distance;

				rn = stop;
				while (to < end)
					*to++ = *from++;
			} else {
				for (; rn < stop; rn++)
					ring[rn & mask] = ring[(rn - distance) & mask];
			}
		} else {
			uint8_t *to = out + n, *stop;

			if (length > cap - n) {
				r = BLOCK_FULL;
				break;
			}
			stop = to + length;
			if (distance <= n) {
				const uint8_t *from = to - distance;

				if (distance == 1) {
					uint8_t v = to[-1];

					while (to < stop)
						*to++ = v;
				} else {
					while (to < stop)
						*to++ = *from++;
				}
			} else {
				int32_t s = (int32_t)n - (int32_t)distance;

				if (distance - n > d->wlen) {
					r = BLOCK_BACK;
					d->back_length = length, d->back_distance = distance;
					break;
				}
				for (; to < stop; s++)
					*to++ = s < 0 ? d->window[(int32_t)d->wlen + s] : out[s];
			}
			n += length;
		}
	}
#undef FILL
#undef DECODE
#undef BITS
	if (pos * 8 - (uint32_t)count > size * 8)
		r = BLOCK_BAD;	/* (it took bits the input does not have) */
	d->b.pos = pos, d->b.hold = hold, d->b.count = count;
	d->n = n, d->rn = rn;
	return r;
}

/* One block from its header on, as bytes (wide 0) or as values (wide 1). */
static inline int
block(struct dec *d, int wide)
{
	struct bits *b = &d->b;
	uint32_t kind;

	d->last = (int)take(b, 1);
	kind = take(b, 2);
	if (kind == 0) {
		uint32_t length, check;

		take(b, b->count & 7);
		length = take(b, 16);
		check = take(b, 16);
		if ((length ^ 0xffff) != check)
			return BLOCK_BAD;
		/* whole bytes from here: what the bit buffer holds goes back */
		b->pos -= (uint32_t)b->count >> 3, b->count = 0, b->hold = 0;
		if (b->pos + length > b->size)
			return BLOCK_BAD;
		if (wide) {
			uint32_t i;

			for (i = 0; i < length; i++)
				d->ring[(d->rn + i) & d->mask] = b->in[b->pos + i];
			d->rn += length;
		} else {
			if (length > d->cap - d->n)
				return BLOCK_FULL;
			memcpy(d->out + d->n, b->in + b->pos, length);
			d->n += length;
		}
		b->pos += length;
		return BLOCK_OK;
	}
	if (kind == 3)
		return BLOCK_BAD;
	if (kind == 1)
		fixed_codes(&d->lit, &d->dist);
	else if (dynamic_codes(b, &d->lit, &d->dist))
		return BLOCK_BAD;
	return symbols(d, wide);
}

/* ---------------------------------------------------------------- finding a block */

/*
 * How far to step from a bit whose next 13 bits are x before a block's header could begin: 0
 * when x itself could be one (not final, dynamic, at most 286 and 30 codes). A bit further on
 * keeps fewer of the 13 known; a field not wholly known may still be good.
 */
static uint8_t skip[1 << 13];
static uint16_t kraft[1 << 12];	/* four code lengths of 3 bits: what they take of 128 */

static void
skip_init(void)
{
	uint32_t x, k, b;

	if (skip[0])
		return;
	for (x = 0; x < 1u << 13; x++) {
		for (k = 0; k < 13; k++) {
			uint32_t w = x >> k, known = 13 - k, good = 1;

			for (b = 0; b < 3 && b < known; b++)
				good &= ((w >> b) & 1) == ((4u >> b) & 1);
			if (known >= 8 && ((w >> 3) & 31) > 29)
				good = 0;
			if (known >= 13 && ((w >> 8) & 31) > 29)
				good = 0;
			if (good)
				break;
		}
		skip[x] = (uint8_t)k;
	}
	for (x = 0; x < 1u << 12; x++) {
		uint32_t sum = 0;

		for (b = 0; b < 4; b++)
			sum += (x >> (3 * b)) & 7 ? 128u >> ((x >> (3 * b)) & 7) : 0;
		kraft[x] = (uint16_t)sum;
	}
}

/* Whether a dynamic block's header could begin at this bit, whose next 13 are x: its code lengths' code whole. */
static inline int
could_begin(const uint8_t *in, uint32_t bit)
{
	const uint8_t *at = in + (bit >> 3);
	/* 3 bits each from bit 17, four at a time */
	uint32_t p = bit + 17;
	int hclen = (int)(((at[1] | at[2] << 8) >> ((bit & 7) + 5)) & 15) + 4, i, left = 128;

	for (i = 0; i < hclen && left >= 0; i += 4, p += 12) {
		const uint8_t *q = in + (p >> 3);
		uint32_t g = ((q[0] | q[1] << 8 | (uint32_t)q[2] << 16) >> (p & 7)) & 0xfff;

		if (hclen - i < 4)
			g &= (1u << (3 * (hclen - i))) - 1;
		left -= kraft[g];
	}
	return !left;
}

/* The first bit in [from, to) a dynamic block could begin at; ~0u if none. The bits are kept in a word: most steps load nothing. */
static uint32_t
find_block(const uint8_t *in, uint32_t size, uint32_t from, uint32_t to)
{
	uint32_t bit = from, hold = 0, pos;
	int count = 0;

	if (to > (size > 16 ? size - 16 : 0) * 8)
		to = (size > 16 ? size - 16 : 0) * 8;
	if (bit >= to)
		return ~0u;
	pos = bit >> 3;
	if (bit & 7)
		hold = (uint32_t)in[pos++] >> (bit & 7), count = 8 - (int)(bit & 7);
	while (bit < to) {
		uint32_t step;

		while (count <= 24)
			hold |= (uint32_t)in[pos++] << count, count += 8;
		step = skip[hold & 0x1fff];
		if (!step) {
			if (could_begin(in, bit))
				return bit;
			step = 1;
		}
		bit += step, hold >>= step, count -= (int)step;
	}
	return ~0u;
}

/* ---------------------------------------------------------------- the parts */

struct part {
	uint32_t start, end, len, crc;	/* bits; bytes made */
	uint32_t marked, fin, bad, bytes;	/* marked ~0u: no marks in its last WINDOW */
	uint8_t head[16], tail[16];
};
/* what a part's second job makes, apart from what the first core reads while it runs */
struct made { uint32_t crc, bad, pad[2]; uint8_t head[16], tail[16]; };

struct stream {
	const uint8_t *in;
	uint32_t in_size, parts, cap;
	uint8_t *out;
	struct part *p;		/* parts' records, 64 bytes each */
	struct made *made;	/* 48 bytes each */
	struct dec *dec;	/* a decoder for each part */
	uint16_t *rings;	/* WINDOW values each */
	uint8_t *after;		/* WINDOW bytes each: the window after the part */
	uint32_t *offset, *wlen;
	const uint8_t **window;
	uint32_t *next;		/* the part that follows each in the chain */
	uint32_t ring;		/* values a part keeps: a power of two, WINDOW or more */
	uint32_t *list;		/* the chain's parts, 1 up, as they come */
	/* the window a round of window_job makes */
	const uint8_t *was;
	const uint16_t *values;
	uint8_t *now;
	uint32_t was_lo, values_at, values_mask;
	volatile uint32_t window_bad;
	int no_crc;		/* a PNG's stream has a sum of its own, and this library does not check that */
};

/* A part's values, how many it keeps. The last parts keep twice as many: a final block is never looked for, and the part before it makes both. */
#define LONGER 4
static inline uint32_t
ring_of(const struct stream *s, uint32_t k)
{
	return k + LONGER >= s->parts && s->ring > WINDOW ? s->ring * 2 : s->ring;
}

static inline uint16_t *
values_of(const struct stream *s, uint32_t k)
{
	uint32_t first = s->parts > LONGER ? s->parts - LONGER : 0;

	return s->rings + (size_t)k * s->ring + (s->ring > WINDOW && k > first ? (size_t)(k - first) * s->ring : 0);
}

/* Part k's bit range ends here. */
static inline uint32_t
part_end(const struct stream *s, uint32_t k)
{
	return k + 1 < s->parts ? (uint32_t)((uint64_t)s->in_size * (k + 1) / s->parts) * 8 : ~0u;
}

/* Whether a part stops at this block's start: past its end, where the next part's search can have found it. */
static inline int
stops_at(const struct stream *s, uint32_t k, uint32_t at)
{
	uint32_t x;
	const uint8_t *q;

	if (at < part_end(s, k) || at >= (s->in_size > 16 ? s->in_size - 16 : 0) * 8)
		return 0;
	q = s->in + (at >> 3);
	x = ((q[0] | q[1] << 8 | (uint32_t)q[2] << 16) >> (at & 7)) & 0x1fff;
	return !skip[x] && could_begin(s->in, at);
}

/* Inflates d from where its bits are to the end of the stream, or to `stop` (~0u: to where a part stops). */
static void
inflate_to(struct stream *s, struct dec *d, uint32_t k, struct part *p, int wide, uint32_t stop)
{
	for (;;) {
		uint32_t at = bits_pos(&d->b);
		int r;

		if (stop != ~0u ? at == stop : stops_at(s, k, at))
			break;
		r = block(d, wide);
		if (r != BLOCK_OK) {
			p->bad = r == BLOCK_FULL ? 2 : 1;
			break;
		}
		if (d->last) {
			p->fin = 1;
			break;
		}
	}
	p->end = bits_pos(&d->b);
}

/* A part: its first block found, and inflated in the same go. A block that does not decode was not one. */
static void
parts_job(void *ctx, uint32_t from, uint32_t to)
{
	struct stream *s = ctx;
	uint32_t k;

	for (k = from; k < to; k++) {
		struct part *p = &s->p[k];
		struct dec *d = &s->dec[k];

		if (k == 0) {
			bits_at(&d->b, s->in, s->in_size, 0);
			d->out = s->out, d->n = 0, d->cap = s->cap, d->wlen = 0, d->window = NULL;
			inflate_to(s, d, k, p, 0, ~0u);
			p->len = d->n;
			p->crc = s->no_crc ? 0 : media_crc32(0, s->out, d->n);
			continue;
		}
		{
			uint32_t bit = (uint32_t)((uint64_t)s->in_size * k / s->parts) * 8, end = part_end(s, k);

			for (;;) {
				p->start = find_block(s->in, s->in_size, bit, end);
				if (p->start == ~0u)
					break;
				bits_at(&d->b, s->in, s->in_size, p->start);
				d->ring = values_of(s, k), d->mask = ring_of(s, k) - 1, d->rn = 0;
				p->bad = 0, p->fin = 0;
				inflate_to(s, d, k, p, 1, ~0u);
				if (!p->bad)
					break;
				bit = p->start + 1;
			}
		}
		if (p->start == ~0u)
			continue;
		p->len = d->rn;
		/* the window after it as bytes where they are known (all of it when no mark is left) */
		if (p->len >= WINDOW) {
			uint8_t *a = s->after + (size_t)k * WINDOW;
			uint32_t i, any = 0;

			for (i = 0; i < WINDOW; i++) {
				uint32_t v = d->ring[(p->len - WINDOW + i) & d->mask];

				a[i] = (uint8_t)v;
				any |= v;
			}
			p->bytes = 1;
			p->marked = any & MARK ? 0 : ~0u;
		}
	}
}

/* Part k, its window known, into its place: its values made bytes, or when they are not all there, inflated again. */
static void
second_job(struct stream *s, uint32_t k)
{
	struct part *p = &s->p[k], q = *p;
	struct made *m = &s->made[k];
	struct dec *d = &s->dec[k];
	uint8_t *at = s->out + s->offset[k];

	if (q.len <= ring_of(s, k)) {
		const uint16_t *v = values_of(s, k);
		const uint8_t *w = s->window[k];
		uint32_t lo = WINDOW - s->wlen[k], i, bad = 0;

		for (i = 0; i < q.len; i++) {
			uint32_t x = v[i];

			if (x & MARK) {
				x &= 0x7fff;
				if (x < lo) {
					bad = 1;
					break;
				}
				x = w[x - lo];
			}
			at[i] = (uint8_t)x;
		}
		m->bad = bad;
	} else {
		bits_at(&d->b, s->in, s->in_size, q.start);
		d->out = at, d->n = 0, d->cap = q.len, d->window = s->window[k], d->wlen = s->wlen[k];
		inflate_to(s, d, k, &q, 0, q.fin ? 0xfffffffeu : q.end);
		m->bad = d->n != q.len || q.bad;
	}
	m->crc = s->no_crc ? 0 : media_crc32(0, at, q.len);
	memcpy(m->head, at, q.len < 16 ? q.len : 16);
	if (q.len >= 16)
		memcpy(m->tail, at + q.len - 16, 16);
}

/* The parts of the chain, 1 up (part 0 is made). */
static void
second_jobs(void *ctx, uint32_t from, uint32_t to)
{
	struct stream *s = ctx;
	uint32_t i;

	for (i = from; i < to; i++)
		second_job(s, s->list[i]);
}

/* The marks among a window's values, each from the window before it: a piece of the 32 KB on each core. */
static void
window_job(void *ctx, uint32_t from, uint32_t to)
{
	struct stream *s = ctx;
	const uint8_t *w = s->was;
	const uint16_t *ring = s->values;
	uint8_t *a = s->now;
	uint32_t lo = s->was_lo, off = s->values_at, mask = s->values_mask, i;

	for (i = from; i < to; i++) {
		uint32_t v = ring[(off + i) & mask];

		if (v & MARK) {
			v &= 0x7fff;
			if (v < lo) {
				s->window_bad = 1;	/* (only of a damaged stream) */
				continue;
			}
			a[i] = w[v - lo];
		}
	}
}

uint32_t
media_inflate_room(void)
{
	return (uint32_t)sizeof(struct dec);
}

const char *
media_inflate_one(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used, uint32_t *crc,
		  void *room)
{
	struct dec *d = room;
	int r;

	crc_init();
	bits_at(&d->b, in, in_size, 0);
	d->out = out, d->n = 0, d->cap = out_size, d->wlen = 0, d->window = NULL;
	do
		r = block(d, 0);
	while (r == BLOCK_OK && !d->last);
	if (made)
		*made = d->n;
	if (used)
		*used = (bits_pos(&d->b) + 7) >> 3;
	if (crc)
		*crc = media_crc32(0, out, d->n);
	return r == BLOCK_FULL ? "larger than the room for it" : r != BLOCK_OK ? "the stream is damaged" : NULL;
}

static void *
take_room(uint8_t **room, uint32_t bytes)
{
	void *at = *room;

	*room += (bytes + 15) & ~15u;
	return at;
}

static const char *
inflate_stream(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used, uint32_t *crc,
	       int workers, int no_crc)
{
	struct stream s;
	mcw_for_opts o;
	uint32_t parts, k, c, total = 0, room_bytes, sum = 0, chain = 0;
	uint8_t *room, *at;
	const char *why = NULL;
	int got;

	crc_init();
	skip_init();
	memset(&s, 0, sizeof s);
	s.in = in, s.in_size = in_size, s.out = out, s.cap = out_size, s.no_crc = no_crc;
	/* a part looks for a block in 32 KB, less than gzip's blocks (some 68 KB packed) are apart:
	 * every block is then the first of some part's bits, and no part makes two. A PNG's blocks
	 * are some 15 to 40 KB apart: 16 KB, or a part goes on into a second block and is the longest. */
	parts = in_size / (no_crc ? 16384 : 32768);
	got = m_workers_begin(parts > 1 ? (int)parts - 1 < workers ? (int)parts - 1 : workers : 0);
	parts = parts < (uint32_t)got + 1 ? parts : (uint32_t)got + 1;
	if (parts < 1)
		parts = 1;
	s.parts = parts;
	/* where the output's size is known (a PNG's), room for a part's whole values: twice its share (half the parts find no
	 * block), 32 MB at most; a part that makes more is inflated again */
	s.ring = WINDOW;
	if (no_crc)
		while (s.ring < out_size / parts * 2 && (uint64_t)(parts + LONGER) * s.ring * 4 <= 32u << 20)
			s.ring <<= 1;
	room_bytes = parts * (uint32_t)(sizeof(struct part) + sizeof(struct made) + sizeof(struct dec) + 16 * 5 + 64) + (parts - 1) * WINDOW + (parts + LONGER) * s.ring * 2 + 64;
	/* (a machine without an MMU may have no room of that many bytes in one piece: rings half as long, down to the window,
	 * and a part that makes more than its ring is inflated again) */
	while (!(room = m_map(room_bytes)) && s.ring > WINDOW) {
		room_bytes -= (parts + LONGER) * s.ring;
		s.ring >>= 1;
	}
	if (!room) {
		m_workers_end();
		return "not enough memory";
	}
	at = room;
	s.p = take_room(&at, parts * (uint32_t)sizeof(struct part));
	s.made = take_room(&at, parts * (uint32_t)sizeof(struct made));
	s.dec = take_room(&at, parts * (uint32_t)sizeof(struct dec));
	s.offset = take_room(&at, parts * 4);
	s.wlen = take_room(&at, parts * 4);
	s.window = take_room(&at, parts * (uint32_t)sizeof(void *));
	s.next = take_room(&at, parts * 4);
	s.list = take_room(&at, parts * 4);
	s.rings = (uint16_t *)take_room(&at, (parts + LONGER) * s.ring * 2);	/* (part 0's goes unused) */
	s.after = (uint8_t *)take_room(&at, (parts - 1) * WINDOW) - WINDOW;
	memset(s.p, 0, parts * sizeof(struct part));
	memset((void *)s.window, 0, parts * sizeof(void *));

	memset(&o, 0, sizeof o);
	o.workers = got;
	o.grain = 1;
	mcw_for_opt(parts, parts_job, &s, &o);

	/* the chain: from part 0, each to the part that starts where it ended, each window from the one before */
	for (c = 0;;) {
		struct part *p = &s.p[c];

		if (p->bad) {
			why = p->bad == 2 ? "larger than the room for it" : "the stream is damaged";
			break;
		}
		s.offset[c] = total;
		total += p->len;
		if (total > out_size) {
			why = "larger than the room for it";
			break;
		}
		if (p->fin)
			break;
		for (k = c + 1; k < parts && s.p[k].start != p->end; k++)
			;
		if (k >= parts) {
			why = "the stream ends before its last block";
			break;
		}
		s.next[c] = k;
		/* its window: the 32 KB before it */
		if (c == 0) {
			s.wlen[k] = p->len < WINDOW ? p->len : WINDOW;
			s.window[k] = s.out + p->len - s.wlen[k];
		} else if (p->bytes) {
			/* the bytes its core knew; the marks among them from the window before it, over every core */
			uint8_t *a = s.after + (size_t)c * WINDOW;

			if (p->marked != ~0u) {
				mcw_for_opts w;

				s.was = s.window[c], s.was_lo = WINDOW - s.wlen[c];
				s.values = values_of(&s, c), s.values_at = p->len - WINDOW, s.values_mask = ring_of(&s, c) - 1, s.now = a;
				memset(&w, 0, sizeof w);
				w.workers = got;
				w.out_bytes = 1;	/* (each core whole 16 bytes of it) */
				w.grain = 2048;
				s.window_bad = 0;
				mcw_for_opt(WINDOW, window_job, &s, &w);
				if (s.window_bad) {
					why = "the stream is damaged";
					break;
				}
			}
			s.window[k] = a, s.wlen[k] = WINDOW;
		} else {
			/* the first core resolves its marks from the window before it */
			const uint8_t *w = s.window[c];
			uint32_t wl = s.wlen[c], keep = p->len >= WINDOW ? 0 : (WINDOW - p->len < wl ? WINDOW - p->len : wl);
			uint32_t from = p->len >= WINDOW ? p->len - WINDOW : 0, i;
			uint8_t *a = s.after + (size_t)c * WINDOW;
			const uint16_t *ring = values_of(&s, c);

			memmove(a, w + wl - keep, keep);
			for (i = from; i < p->len; i++) {
				uint32_t v = ring[i & (ring_of(&s, c) - 1)];

				if (v & MARK) {
					v &= 0x7fff;
					if (v < WINDOW - wl) {
						why = "the stream is damaged";
						break;
					}
					v = w[v - (WINDOW - wl)];
				}
				a[keep + i - from] = (uint8_t)v;
			}
			if (why)
				break;
			s.window[k] = a, s.wlen[k] = keep + p->len - from;
		}
		s.offset[k] = total;
		if (s.p[k].len > out_size - total) {
			why = "larger than the room for it";
			break;
		}
		m_progress(total, out_size);
		s.list[chain++] = k;
		c = k;
	}
	if (!why && chain)
		mcw_for_opt(chain, second_jobs, &s, &o);
	if (!why) {
		uint32_t last = c;

		/* the texels two parts share, as each made them; the CRC joined */
		for (c = 0;; c = s.next[c]) {
			struct part *p = &s.p[c];
			struct made *m = &s.made[c];

			if (c && m->bad) {
				why = "the stream is damaged";
				break;
			}
			if (c) {
				memcpy(s.out + s.offset[c], m->head, p->len < 16 ? p->len : 16);
				if (p->len >= 16)
					memcpy(s.out + s.offset[c] + p->len - 16, m->tail, 16);
			}
			if (!no_crc)
				sum = c ? media_crc32_join(sum, m->crc, p->len) : p->crc;
			if (c == last)
				break;
		}
		if (used)
			*used = (s.p[last].end + 7) >> 3;
	}
	if (made)
		*made = why ? 0 : total;
	if (crc)
		*crc = sum;
	m_unmap(room, room_bytes);
	m_workers_end();
	return why;
}

const char *
media_inflate_stream(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used, uint32_t *crc,
		     int workers)
{
	return inflate_stream(in, in_size, out, out_size, made, used, crc, workers, 0);
}

/* The same with no CRC-32 (a PNG's). */
const char *
m_inflate_stream_plain(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, int workers)
{
	return inflate_stream(in, in_size, out, out_size, made, NULL, NULL, workers, 1);
}

/* ---------------------------------------------------------------- MSZip: a cabinet's folder */

/*
 * A Microsoft cabinet's MSZip folder is blocks of 32 KB of output (the folder's last may be less), each
 * a deflate stream of its own whose dictionary is the block before it (docs/system/media.md, "A
 * cabinet's blocks"). Every block is a job: it is inflated in bytes (the run's first with its window,
 * which is known), and at its first copy from before itself goes on as values, a byte or a mark of the
 * place in the block before (the bytes made so far become values). Then each eighth of a block with
 * marks follows each back, through the values of the blocks before, to a byte: a round over every core
 * with no core waiting for another, an eighth a piece so that a block of long chains is shared out.
 */
#define MZ_PARTS 8
struct mz_block { uint32_t marks, bad, first, last; };	/* a texel each: its core's alone; marks in [first, last) */
struct mz_dec { struct dec d; } __attribute__((aligned(16)));
struct mszip {
	const uint8_t *const *in;
	const uint32_t *in_size, *plain;
	uint32_t count, wlen;
	uint8_t *out;
	uint16_t *values;	/* WINDOW a block */
	struct mz_block *b;
	uint32_t *part_bad;	/* a texel a part */
	struct mz_dec *dec;	/* a block's own */
};

uint32_t
media_mszip_room(uint32_t count)
{
	return count * (uint32_t)(WINDOW * 2 + sizeof(struct mz_block) + MZ_PARTS * 16 + sizeof(struct mz_dec)) + 64;
}

static void
mszip_inflate(void *ctx, uint32_t from, uint32_t to)
{
	struct mszip *z = ctx;
	uint32_t k;

	for (k = from; k < to; k++) {
		struct dec *d = &z->dec[k].d;
		struct mz_block *b = &z->b[k];
		uint8_t *out = z->out + (size_t)k * WINDOW;
		uint32_t plain = z->plain[k], marks = 0, first = 0, last = 0, i;
		int r;

		/* in bytes: the run's first with its window, another while it copies nothing from before itself */
		bits_at(&d->b, z->in[k], z->in_size[k], 0);
		d->out = out, d->n = 0, d->cap = plain;
		d->window = k ? NULL : out - z->wlen, d->wlen = k ? 0 : z->wlen;
		do
			r = block(d, 0);
		while (r == BLOCK_OK && !d->last);
		if (r == BLOCK_BACK) {
			/* on as values from that copy: the bytes so far are values, the copy is made, the block's symbols go on */
			uint16_t *v = z->values + (size_t)k * WINDOW;
			uint32_t n = d->n, length = d->back_length, distance = d->back_distance, stop = n + length;

			for (i = 0; i < n; i++)
				v[i] = out[i];
			first = n;
			if (distance > n + WINDOW || stop > WINDOW)
				r = BLOCK_BAD;
			else {
				for (; n < stop; n++)
					v[n] = distance > n ? (uint16_t)(MARK | (WINDOW - (distance - n))) : v[n - distance];
				d->ring = v, d->mask = WINDOW - 1, d->rn = n;
				r = symbols(d, 1);
				while (r == BLOCK_OK && !d->last && d->rn <= plain)
					r = block(d, 1);
			}
			if (r == BLOCK_OK && d->rn == plain) {
				for (i = first; i < plain; i++) {
					uint32_t x = v[i];

					if (x & MARK)
						marks++, last = i + 1;
					else
						out[i] = (uint8_t)x;
				}
			}
			d->n = d->rn;
		}
		b->bad = r != BLOCK_OK || !d->last || d->n != plain;
		b->marks = marks, b->first = first, b->last = last;
	}
}

/* Each mark of a block's part followed back to a byte: through the values of the blocks before while they are marks, to
 * a block made bytes whole, or to the window before the run. */
static void
mszip_marks(void *ctx, uint32_t from, uint32_t to)
{
	struct mszip *z = ctx;
	uint32_t p;

	for (p = from; p < to; p++) {
		uint32_t k = p / MZ_PARTS, part = WINDOW / MZ_PARTS, lo = WINDOW - z->wlen, bad = 0, i, end;
		uint16_t *v = z->values + (size_t)k * WINDOW;
		uint8_t *out = z->out + (size_t)k * WINDOW;
		const struct mz_block *b = &z->b[k];

		i = p % MZ_PARTS * part, end = i + part;
		if (!b->marks || i >= b->last || end <= b->first)
			continue;
		i = i > b->first ? i : b->first, end = end < b->last ? end : b->last;
		for (; i < end; i++) {
			uint32_t x = v[i], j, m = k;

			if (!(x & MARK))
				continue;
			j = x & 0x7fff;
			for (;;) {
				if (!m) {
					if (j < lo) {
						bad = 1;
						x = 0;
					} else
						x = z->out[(int32_t)j - (int32_t)WINDOW];
					break;
				}
				m--;
				if (!z->b[m].marks || j < z->b[m].first || j >= z->b[m].last) {
					x = z->out[(size_t)m * WINDOW + j];
					break;
				}
				x = z->values[(size_t)m * WINDOW + j];
				if (!(x & MARK))
					break;
				j = x & 0x7fff;
			}
			/* (the value a byte too: a later block's chain through here ends here, from the next pass on) */
			out[i] = (uint8_t)x, v[i] = (uint16_t)x;
		}
		z->part_bad[p * 4] = bad;
	}
}

const char *
media_mszip(const uint8_t *const *in, const uint32_t *in_size, const uint32_t *plain, uint32_t count, uint8_t *out,
	    uint32_t wlen, void *room, int workers)
{
	struct mszip z;
	mcw_for_opts o;
	uint32_t k, marked = 0;
	uint8_t *at = room;
	int got;

	if (!count)
		return NULL;
	if (((uintptr_t)out | (uintptr_t)room) & 15)
		return "not on 16 bytes";
	if (wlen > WINDOW)
		wlen = WINDOW;
	for (k = 0; k < count; k++)
		if (plain[k] > WINDOW || (k + 1 < count && plain[k] != WINDOW))
			return "a block not of 32 KB before the last";
	crc_init();
	memset(&z, 0, sizeof z);
	z.in = in, z.in_size = in_size, z.plain = plain, z.count = count, z.wlen = wlen, z.out = out;
	z.values = (uint16_t *)take_room(&at, count * WINDOW * 2);
	z.b = (struct mz_block *)take_room(&at, count * (uint32_t)sizeof(struct mz_block));
	z.part_bad = (uint32_t *)take_room(&at, count * MZ_PARTS * 16);
	z.dec = (struct mz_dec *)take_room(&at, count * (uint32_t)sizeof(struct mz_dec));
	memset(z.part_bad, 0, count * MZ_PARTS * 16);
	got = m_workers_begin(count > 1 ? (int)count - 1 < workers ? (int)count - 1 : workers : 0);
	memset(&o, 0, sizeof o);
	o.workers = got;
	o.grain = 1;
	mcw_for_opt(count, mszip_inflate, &z, &o);
	for (k = 0; k < count; k++) {
		if (z.b[k].bad)
			break;
		marked += z.b[k].marks != 0;
	}
	if (k == count && marked)
		mcw_for_opt(count * MZ_PARTS, mszip_marks, &z, &o);
	m_workers_end();
	for (k = 0; k < count; k++)
		if (z.b[k].bad)
			return "a block is damaged";
	for (k = 0; k < count * MZ_PARTS; k++)
		if (z.part_bad[k * 4])
			return "a block is damaged";
	return NULL;
}
