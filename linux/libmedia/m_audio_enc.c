/*
 * libmedia's sound encoders (docs/system/media.md, "Sound encoded"): WAV, and FLAC of fixed predictors and
 * Rice codes. A FLAC block is 4,096 frames, each encoded by whichever core takes it into a room of its own
 * (a round of 256 blocks at a time); this core then puts the round's blocks one after another. Every block
 * is as long but the last, and numbered: m_audio_flac.c decodes such a file in pieces on every core.
 */
#include <stdlib.h>
#include "m_int.h"

/* ---------------------------------------------------------------- WAV */

uint8_t *
media_wav_encode(const int16_t *frames, uint32_t count, int rate, int channels, uint32_t *size)
{
	uint32_t data = count * (uint32_t)channels * 2, total = 44 + data;
	uint8_t *f, *p;

	if (channels < 1 || channels > 2 || rate < 1 || !(f = m_map(total)))
		return NULL;
	p = f;
#define L32(v) (p[0] = (uint8_t)(v), p[1] = (uint8_t)((v) >> 8), p[2] = (uint8_t)((v) >> 16), p[3] = (uint8_t)((v) >> 24), p += 4)
#define L16(v) (p[0] = (uint8_t)(v), p[1] = (uint8_t)((v) >> 8), p += 2)
	memcpy(p, "RIFF", 4), p += 4, L32(total - 8);
	memcpy(p, "WAVEfmt ", 8), p += 8, L32(16u);
	L16(1u), L16((uint32_t)channels), L32((uint32_t)rate), L32((uint32_t)rate * (uint32_t)channels * 2), L16((uint32_t)channels * 2), L16(16u);
	memcpy(p, "data", 4), p += 4, L32(data);
#undef L32
#undef L16
	memcpy(p, frames, data);	/* (the machine's samples are little-endian, as a WAV's) */
	*size = total;
	return f;
}

/* ---------------------------------------------------------------- FLAC */

#define BLOCK 4096
#define ROUND 256
#define MOST_ORDER 8	/* partitions of 16 samples at the finest */
/* a block at most: its header (16 bytes), two subframes verbatim (a side channel's 17 bits a sample), the check */
#define ROOM ((16 + 2 * (2 + BLOCK * 17 / 8) + 2 + 15) & ~15)

static uint8_t crc8_table[256];
static uint16_t crc16_table[256];

static void
crc_tables(void)
{
	int i, k;

	if (crc16_table[1])
		return;
	for (i = 0; i < 256; i++) {
		uint32_t c8 = (uint32_t)i, c16 = (uint32_t)i << 8;

		for (k = 0; k < 8; k++) {
			c8 = c8 & 0x80 ? (c8 << 1 ^ 0x07) & 0xff : c8 << 1 & 0xff;
			c16 = c16 & 0x8000 ? (c16 << 1 ^ 0x8005) & 0xffff : c16 << 1 & 0xffff;
		}
		crc8_table[i] = (uint8_t)c8;
		crc16_table[i] = (uint16_t)c16;
	}
}

struct bits { uint8_t *p, *end; uint64_t acc; int n; };

static inline void
put(struct bits *b, uint32_t v, int n)
{
	if (n < 32)
		v &= (1u << n) - 1;
	b->acc = b->acc << n | v;
	b->n += n;
	while (b->n >= 8) {
		b->n -= 8;
		if (b->p < b->end)
			*b->p++ = (uint8_t)(b->acc >> b->n);
		else
			b->end = NULL, b->p = NULL;	/* (cannot be: the room is a verbatim block's) */
		if (!b->p)
			return;
	}
}

static inline void
put_zeros(struct bits *b, uint32_t n)
{
	for (; n > 24; n -= 24)
		put(b, 0, 24);
	put(b, 0, (int)n);
}

struct fenc {
	const int16_t *pcm;
	uint32_t frames, first_block;	/* the round's first block */
	int channels;
	uint8_t *rooms;
	uint32_t *bytes;		/* each of the round's blocks', 16 bytes apart */
	int32_t *work;			/* a piece's channels (left, right, side, mid), WORK bytes a piece */
	uint32_t grain;
};
#define WORK (4 * BLOCK * 4)

/* One channel's choice: the fixed predictor's order, the partition order and each partition's parameter. */
struct choice { int order, porder, kind; uint32_t bits; uint8_t k[1 << MOST_ORDER]; };
enum { CONSTANT, VERBATIM, FIXED };

static inline uint32_t zig(int32_t r) { return (uint32_t)(r << 1) ^ (uint32_t)(r >> 31); }

static inline int32_t
residual(const int32_t *x, int i, int order)
{
	switch (order) {
	case 0: return x[i];
	case 1: return x[i] - x[i - 1];
	case 2: return x[i] - 2 * x[i - 1] + x[i - 2];
	case 3: return x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3];
	default: return x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4];
	}
}

/* The best Rice parameter for n values summing to s, and its bits (at most: the sum of floors is no more than the floor of the sum). */
static uint32_t
rice_best(uint64_t s, uint32_t n, uint8_t *k_out)
{
	uint64_t best = ~0ull;
	int k, kb = 0;

	for (k = 0; k <= 14; k++) {
		uint64_t b = (uint64_t)n * (uint32_t)(k + 1) + (s >> k);

		if (b < best)
			best = b, kb = k;
	}
	*k_out = (uint8_t)kb;
	return best > 0xffffffffu ? 0xffffffffu : (uint32_t)best;
}

/* What one channel of n samples (bps bits each) costs at its best, and how (in *c). */
static uint32_t
choose(const int32_t *x, uint32_t n, int bps, struct choice *c)
{
	uint64_t sums[5] = { 0, 0, 0, 0, 0 }, part[1 << MOST_ORDER];
	uint32_t i, best_bits = ~0u, verbatim = (uint32_t)bps * n;
	int o, p, most_p, order = 0, j;

	for (i = 1; i < n && x[i] == x[0]; i++)
		;
	if (i == n) {
		c->kind = CONSTANT, c->bits = (uint32_t)bps;
		return c->bits;
	}
	/* the order whose residual is least in magnitude (as libFLAC's fixed encoder) */
	for (i = 4; i < n; i++)
		for (o = 0; o <= 4; o++) {
			int32_t r = residual(x, (int)i, o);

			sums[o] += (uint32_t)(r < 0 ? -r : r);
		}
	for (o = 1; o <= 4; o++)
		if (sums[o] < sums[order])
			order = o;
	if (n <= (uint32_t)order) {
		c->kind = VERBATIM, c->bits = verbatim;
		return verbatim;
	}
	/* partitions: as fine as whole partitions of 16 or more allow, the first one's after the warm-up */
	for (most_p = MOST_ORDER; most_p > 0 && ((n & ((1u << most_p) - 1)) || (n >> most_p) <= (uint32_t)order || (n >> most_p) < 16); most_p--)
		;
	memset(part, 0, sizeof part);
	{
		uint32_t size = n >> most_p;

		for (i = (uint32_t)order; i < n; i++)
			part[i / size] += zig(residual(x, (int)i, order));
	}
	for (p = most_p; p >= 0; p--) {
		uint32_t parts = 1u << p, size = n >> p, bits = 6 + 4 * parts + (uint32_t)(order * bps);
		uint8_t k[1 << MOST_ORDER];

		for (j = 0; j < (int)parts; j++) {
			uint32_t count = j ? size : size - (uint32_t)order;

			bits += rice_best(part[j], count, &k[j]);
		}
		if (bits < best_bits) {
			best_bits = bits;
			c->porder = p;
			memcpy(c->k, k, parts);
		}
		/* the next coarser order's sums: pairs added */
		for (j = 0; j < (int)parts / 2; j++)
			part[j] = part[2 * j] + part[2 * j + 1];
	}
	if (best_bits >= verbatim) {
		c->kind = VERBATIM, c->bits = verbatim;
		return verbatim;
	}
	c->kind = FIXED, c->order = order, c->bits = best_bits;
	return best_bits;
}

static void
subframe(struct bits *b, const int32_t *x, uint32_t n, int bps, const struct choice *c)
{
	uint32_t i;
	int j;

	put(b, 0, 1);
	if (c->kind == CONSTANT) {
		put(b, 0, 6), put(b, 0, 1), put(b, (uint32_t)x[0], bps);
		return;
	}
	if (c->kind == VERBATIM) {
		put(b, 1, 6), put(b, 0, 1);
		for (i = 0; i < n; i++)
			put(b, (uint32_t)x[i], bps);
		return;
	}
	put(b, 8u | (uint32_t)c->order, 6), put(b, 0, 1);
	for (j = 0; j < c->order; j++)
		put(b, (uint32_t)x[j], bps);
	put(b, 0, 2);			/* Rice, 4-bit parameters */
	put(b, (uint32_t)c->porder, 4);
	{
		uint32_t parts = 1u << c->porder, size = n >> c->porder, p;

		for (p = 0, i = (uint32_t)c->order; p < parts; p++) {
			uint32_t end = (p + 1) * size;
			int k = c->k[p];

			put(b, (uint32_t)k, 4);
			for (; i < end; i++) {
				uint32_t u = zig(residual(x, (int)i, c->order));

				put_zeros(b, u >> k);
				put(b, 1, 1);
				if (k)
					put(b, u, k);
			}
		}
	}
}

/* Block `block` of the stream into `room`: its bytes. */
static uint32_t
block_encode(const struct fenc *e, uint32_t block, uint8_t *room, int32_t (*ch)[BLOCK])
{
	static const int bps_of[4][2] = { { 16, 16 }, { 16, 17 }, { 17, 16 }, { 16, 17 } };	/* L/R, L/S, S/R, M/S */
	uint32_t first = block * BLOCK, n = e->frames - first < BLOCK ? e->frames - first : BLOCK, i, cost[4], v;
	struct choice c[4];
	struct bits b = { room, room + ROOM, 0, 0 };
	int how = 0, k, m, len;
	uint8_t crc8 = 0;
	uint16_t crc16 = 0;
	uint8_t *at;

	if (e->channels == 1) {
		for (i = 0; i < n; i++)
			ch[0][i] = e->pcm[first + i];
		choose(ch[0], n, 16, &c[0]);
	} else {
		for (i = 0; i < n; i++) {
			int32_t l = e->pcm[(first + i) * 2], r = e->pcm[(first + i) * 2 + 1];

			ch[0][i] = l, ch[1][i] = r, ch[2][i] = l - r, ch[3][i] = (l + r) >> 1;
		}
		cost[0] = choose(ch[0], n, 16, &c[0]);
		cost[1] = choose(ch[1], n, 16, &c[1]);
		cost[2] = choose(ch[2], n, 17, &c[2]);
		cost[3] = choose(ch[3], n, 16, &c[3]);
		{
			uint32_t ways[4] = { cost[0] + cost[1], cost[0] + cost[2], cost[2] + cost[1], cost[3] + cost[2] };

			for (k = 1; k < 4; k++)
				if (ways[k] < ways[how])
					how = k;
		}
	}
	/* the header: sync, fixed blocks; 4,096 frames (or the last block's, 16 bits after the number); the rate and the
	 * sample size from STREAMINFO's; the channels; the block's number as UTF-8 */
	put(&b, 0xfff8, 16);
	put(&b, n == BLOCK ? 12 : 7, 4), put(&b, 0, 4);
	put(&b, e->channels == 1 ? 0 : how == 0 ? 1 : how == 1 ? 8 : how == 2 ? 9 : 10, 4);
	put(&b, 4, 3), put(&b, 0, 1);	/* 16 bits a sample */
	v = block;
	if (v < 0x80) {
		put(&b, v, 8);
	} else {
		for (len = 2; len < 6 && v >= 1u << (5 * len + 1); len++)
			;
		put(&b, (0xff00u >> len) | (v >> (6 * (len - 1))), 8);
		for (m = len - 2; m >= 0; m--)
			put(&b, 0x80u | (v >> (6 * m) & 0x3f), 8);
	}
	if (n != BLOCK)
		put(&b, n - 1, 16);
	for (at = room; at < b.p; at++)
		crc8 = crc8_table[crc8 ^ *at];
	put(&b, crc8, 8);
	if (e->channels == 1) {
		subframe(&b, ch[0], n, 16, &c[0]);
	} else {
		static const int which[4][2] = { { 0, 1 }, { 0, 2 }, { 2, 1 }, { 3, 2 } };

		for (k = 0; k < 2; k++)
			subframe(&b, ch[which[how][k]], n, bps_of[how][k], &c[which[how][k]]);
	}
	if (b.n)
		put(&b, 0, 8 - b.n);
	if (!b.p)
		return 0;
	for (at = room; at < b.p; at++)
		crc16 = (uint16_t)(crc16 << 8) ^ crc16_table[(crc16 >> 8) ^ *at];
	put(&b, crc16, 16);
	return b.p ? (uint32_t)(b.p - room) : 0;
}

static void
blocks_job(void *ctx, uint32_t from, uint32_t to)
{
	const struct fenc *e = ctx;
	int32_t (*ch)[BLOCK] = (int32_t (*)[BLOCK])(e->work + (from / e->grain) * (WORK / 4));

	for (; from < to; from++)
		e->bytes[from * 4] = block_encode(e, e->first_block + from, e->rooms + from * ROOM, ch);
}

/* Bytes after the file so far: the mapping grown by doubling. */
static int
append(uint8_t **f, uint32_t *have, uint32_t *room, const void *bytes, uint32_t n)
{
	if (*have + n > *room) {
		uint32_t more = *room * 2 > *have + n ? *room * 2 : (*have + n) * 2;
		uint8_t *g = m_map(more);

		if (!g)
			return 0;
		memcpy(g, *f, *have);
		m_unmap(*f, *room);
		*f = g, *room = more;
	}
	memcpy(*f + *have, bytes, n);
	*have += n;
	return 1;
}

uint8_t *
media_flac_encode(const int16_t *frames, uint32_t count, int rate, int channels, uint32_t *size, int workers)
{
	static struct fenc job;
	struct fenc *e = &job;
	uint32_t blocks = (count + BLOCK - 1) / BLOCK, b, have = 0, room = 65536, least = ~0u, most = 0, i, pieces = 1;
	uint8_t *f, head[42], *p = head;
	int got;

	if (channels < 1 || channels > 2 || rate < 1 || rate >= 1 << 20 || !count)
		return NULL;
	crc_tables();
	memset(e, 0, sizeof *e);
	e->pcm = frames, e->frames = count, e->channels = channels;
	if (!(f = m_map(room)) || !(e->rooms = m_map(ROUND * ROOM)) || !(e->bytes = m_map(ROUND * 16))) {
		m_unmap(f, room);
		m_unmap(e->rooms, ROUND * ROOM);
		return NULL;
	}
	/* "fLaC" and STREAMINFO, written again at the end with the frames' sizes */
	memset(head, 0, sizeof head);
	append(&f, &have, &room, head, sizeof head);
	got = m_workers_begin(workers);
	e->grain = m_grain(blocks < ROUND ? blocks : ROUND, got);
	pieces = ((blocks < ROUND ? blocks : ROUND) + e->grain - 1) / e->grain;
	if (!(e->work = m_map(pieces * WORK))) {
		m_workers_end();
		m_unmap(f, room), f = NULL;
		goto done;
	}
	for (b = 0; b < blocks; b += ROUND) {
		uint32_t n = blocks - b < ROUND ? blocks - b : ROUND;

		e->first_block = b;
		m_for_grain(n, blocks_job, e, e->grain, got);
		m_progress(b + n, blocks);
		for (i = 0; i < n; i++) {
			uint32_t bytes = e->bytes[i * 4];

			if (!bytes || !append(&f, &have, &room, e->rooms + i * ROOM, bytes)) {
				m_workers_end();
				m_unmap(f, room), f = NULL;
				goto done;
			}
			least = bytes < least ? bytes : least;
			most = bytes > most ? bytes : most;
		}
	}
	m_workers_end();
#define B(v) (*p++ = (uint8_t)(v))
	memcpy(p, "fLaC", 4), p += 4;
	B(0x80), B(0), B(0), B(34);	/* the last block of metadata, STREAMINFO, 34 bytes */
	B(BLOCK >> 8), B(BLOCK & 255), B(BLOCK >> 8), B(BLOCK & 255);
	B(least >> 16), B(least >> 8), B(least);
	B(most >> 16), B(most >> 8), B(most);
	B(rate >> 12), B(rate >> 4), B((rate & 15) << 4 | (channels - 1) << 1 | 0);	/* 16 bits a sample: 15 in five bits */
	B(0xf0), B(count >> 24), B(count >> 16), B(count >> 8), B(count);	/* bits 4 of the sample size, then 36 of the count */
#undef B
	/* (the MD5 of the samples: zeros, "not computed") */
	memcpy(f, head, 42);
	*size = have;
done:
	m_unmap(e->work, pieces * WORK);
	m_unmap(e->rooms, ROUND * ROOM);
	m_unmap(e->bytes, ROUND * 16);
	return f;
}
