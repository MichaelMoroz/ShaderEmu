/*
 * The JPEG encoder (docs/system/media.md): baseline, the standard tables scaled to the quality asked,
 * colours at half size each way under quality 90. Every MCU row is a restart interval, encoded
 * by whichever core takes it into memory of its own; this core then puts them one after another
 * with their markers. A file written so is decoded by every core (m_jpeg.c, "restart markers").
 */
#include <stdlib.h>
#include <sys/mman.h>
#include "m_int.h"

static const uint8_t zigzag[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static const uint8_t base_luma[64] = {
	16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};
static const uint8_t base_chroma[64] = {
	17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

static const uint8_t dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_values[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_luma_values[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chroma_values[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

struct code { uint16_t code; uint8_t length; };

struct jenc {
	const struct media_image *in;
	int w, h, sub, mcu, mcus_x, mcus_y, blocks;	/* sub: colours at half size; mcu: 8 or 16 */
	uint8_t quant[2][64];				/* zigzag order, as written */
	int32_t step[2][64];				/* natural order: 8 * q (the transform is 8 times) */
	struct code dc[2][12], ac[2][256];
	uint8_t *rows;					/* each interval's bytes, `room` apart */
	uint32_t room, *row_bytes, restart, intervals;	/* MCUs an interval, and how many */
	volatile uint32_t *failed;
};

static void
codes_make(const uint8_t *bits, const uint8_t *values, struct code *out)
{
	int code = 0, k = 0, length, i;

	for (length = 1; length <= 16; length++) {
		for (i = 0; i < bits[length - 1]; i++, k++, code++)
			out[values[k]].code = (uint16_t)code, out[values[k]].length = (uint8_t)length;
		code <<= 1;
	}
}

/* The forward transform (libjpeg's slow integer one): 8 times the true coefficients. */
#define CB 13
#define PB 2
#define DESCALE(x, n) (((x) + (1 << ((n) - 1))) >> (n))
static void
fdct(int *d)
{
	int i, *p;

	for (p = d, i = 0; i < 8; i++, p += 8) {
		int t0 = p[0] + p[7], t7 = p[0] - p[7], t1 = p[1] + p[6], t6 = p[1] - p[6];
		int t2 = p[2] + p[5], t5 = p[2] - p[5], t3 = p[3] + p[4], t4 = p[3] - p[4];
		int t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2, z1, z2, z3, z4, z5;

		p[0] = (t10 + t11) << PB;
		p[4] = (t10 - t11) << PB;
		z1 = (t12 + t13) * 4433;
		p[2] = DESCALE(z1 + t13 * 6270, CB - PB);
		p[6] = DESCALE(z1 - t12 * 15137, CB - PB);
		z1 = t4 + t7, z2 = t5 + t6, z3 = t4 + t6, z4 = t5 + t7;
		z5 = (z3 + z4) * 9633;
		t4 *= 2446, t5 *= 16819, t6 *= 25172, t7 *= 12299;
		z1 *= -7373, z2 *= -20995, z3 *= -16069, z4 *= -3196;
		z3 += z5, z4 += z5;
		p[7] = DESCALE(t4 + z1 + z3, CB - PB);
		p[5] = DESCALE(t5 + z2 + z4, CB - PB);
		p[3] = DESCALE(t6 + z2 + z3, CB - PB);
		p[1] = DESCALE(t7 + z1 + z4, CB - PB);
	}
	for (p = d, i = 0; i < 8; i++, p++) {
		int t0 = p[0] + p[56], t7 = p[0] - p[56], t1 = p[8] + p[48], t6 = p[8] - p[48];
		int t2 = p[16] + p[40], t5 = p[16] - p[40], t3 = p[24] + p[32], t4 = p[24] - p[32];
		int t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2, z1, z2, z3, z4, z5;

		p[0] = DESCALE(t10 + t11, PB);
		p[32] = DESCALE(t10 - t11, PB);
		z1 = (t12 + t13) * 4433;
		p[16] = DESCALE(z1 + t13 * 6270, CB + PB);
		p[48] = DESCALE(z1 - t12 * 15137, CB + PB);
		z1 = t4 + t7, z2 = t5 + t6, z3 = t4 + t6, z4 = t5 + t7;
		z5 = (z3 + z4) * 9633;
		t4 *= 2446, t5 *= 16819, t6 *= 25172, t7 *= 12299;
		z1 *= -7373, z2 *= -20995, z3 *= -16069, z4 *= -3196;
		z3 += z5, z4 += z5;
		p[56] = DESCALE(t4 + z1 + z3, CB + PB);
		p[40] = DESCALE(t5 + z2 + z4, CB + PB);
		p[24] = DESCALE(t6 + z2 + z3, CB + PB);
		p[8] = DESCALE(t7 + z1 + z4, CB + PB);
	}
}

struct bitw { uint8_t *p, *end; uint32_t hold; int count; };

static inline void
bits_put(struct bitw *b, uint32_t v, int n)
{
	b->hold = b->hold << n | (v & ((1u << n) - 1));
	b->count += n;
	while (b->count >= 8) {
		uint8_t byte = (uint8_t)(b->hold >> (b->count - 8));

		b->count -= 8;
		if (b->p + 2 <= b->end) {
			*b->p++ = byte;
			if (byte == 0xff)
				*b->p++ = 0;
		} else {
			b->end = NULL, b->p = NULL;	/* (full: told below) */
			b->count = -10000;
		}
	}
}

static inline int
magnitude(int v)
{
	int n = 0;

	if (v < 0)
		v = -v;
	while (v)
		n++, v >>= 1;
	return n;
}

static void
block_put(const struct jenc *e, struct bitw *b, int *d, int t, int *pred)
{
	int k, run = 0, v, n;

	fdct(d);
	{
		int q[64];

		for (k = 0; k < 64; k++) {
			int c = d[zigzag[k]], s = e->step[t][zigzag[k]];

			/* rounded to the nearest, away from zero at a half */
			q[k] = c >= 0 ? (c + (s >> 1)) / s : -((-c + (s >> 1)) / s);
		}
		v = q[0] - *pred;
		*pred = q[0];
		n = magnitude(v);
		bits_put(b, e->dc[t][n].code, e->dc[t][n].length);
		if (n)
			bits_put(b, (uint32_t)(v < 0 ? v - 1 : v), n);
		for (k = 1; k < 64; k++) {
			v = q[k];
			if (!v) {
				run++;
				continue;
			}
			while (run >= 16) {
				bits_put(b, e->ac[t][0xf0].code, e->ac[t][0xf0].length);
				run -= 16;
			}
			n = magnitude(v);
			bits_put(b, e->ac[t][run << 4 | n].code, e->ac[t][run << 4 | n].length);
			bits_put(b, (uint32_t)(v < 0 ? v - 1 : v), n);
			run = 0;
		}
		if (run)
			bits_put(b, e->ac[t][0].code, e->ac[t][0].length);
	}
}

static inline void
pixel_at(const struct media_image *in, int x, int y, int *r, int *g, int *b)
{
	const uint8_t *p;

	if (x >= in->width) x = in->width - 1;
	if (y >= in->height) y = in->height - 1;
	p = in->pixels + (uint32_t)y * (uint32_t)in->stride + (uint32_t)x * (uint32_t)in->channels;
	if (in->bgr)
		*r = p[2], *g = p[1], *b = p[0];
	else
		*r = p[0], *g = p[1], *b = p[2];
}

/* Interval i's MCUs into b (its last byte filled with ones). */
static void
one_interval(const struct jenc *e, uint32_t i, struct bitw *b)
{
	uint32_t m, mcus = (uint32_t)(e->mcus_x * e->mcus_y);
	int pred[3] = {0, 0, 0};
	uint32_t end = (i + 1) * e->restart < mcus ? (i + 1) * e->restart : mcus;

	for (m = i * e->restart; m < end; m++) {
		int Y[4][64], cb[64], cr[64], x, y, k, mx = (int)(m % (uint32_t)e->mcus_x), my = (int)(m / (uint32_t)e->mcus_x);

		memset(cb, 0, sizeof cb);
		memset(cr, 0, sizeof cr);
		for (y = 0; y < e->mcu; y++)
			for (x = 0; x < e->mcu; x++) {
				int r, g, bl, py = my * e->mcu + y, px = mx * e->mcu + x, lum, u, v;

				pixel_at(e->in, px, py, &r, &g, &bl);
				lum = (19595 * r + 38470 * g + 7471 * bl + 32768) >> 16;
				u = (-11059 * r - 21709 * g + 32768 * bl + (128 << 16) + 32767) >> 16;
				v = (32768 * r - 27439 * g - 5329 * bl + (128 << 16) + 32767) >> 16;
				Y[(y >> 3) * 2 + (x >> 3)][(y & 7) * 8 + (x & 7)] = lum - 128;
				if (e->sub) {
					cb[(y >> 1) * 8 + (x >> 1)] += u, cr[(y >> 1) * 8 + (x >> 1)] += v;
				} else {
					cb[y * 8 + x] = u - 128, cr[y * 8 + x] = v - 128;
				}
			}
		if (e->sub)
			for (k = 0; k < 64; k++)
				cb[k] = ((cb[k] + 2) >> 2) - 128, cr[k] = ((cr[k] + 2) >> 2) - 128;
		for (k = 0; k < (e->sub ? 4 : 1); k++)
			block_put(e, b, Y[k], 0, &pred[0]);
		block_put(e, b, cb, 1, &pred[1]);
		block_put(e, b, cr, 1, &pred[2]);
	}
	/* the interval's last byte filled with ones */
	if (b->count > 0)
		bits_put(b, 0x7f, 8 - b->count);
}

/* Intervals from..to-1, each into its own room. */
static void
intervals_job(void *ctx, uint32_t from, uint32_t to)
{
	struct jenc *e = ctx;
	uint32_t i;

	for (i = from; i < to; i++) {
		struct bitw b = { e->rows + i * e->room, e->rows + (i + 1) * e->room, 0, 0 };

		one_interval(e, i, &b);
		if (!b.p)
			*e->failed = 1;
		e->row_bytes[i * 4] = b.p ? (uint32_t)(b.p - (e->rows + i * e->room)) : 0;	/* (a count in 16 bytes of its own) */
	}
}

/* The tables for a picture and a quality, and its intervals; false for a size a JPEG cannot have. */
static int
setup(struct jenc *e, const struct media_image *image, int quality)
{
	int k, t, scale;

	memset(e, 0, sizeof *e);
	e->in = image, e->w = image->width, e->h = image->height;
	if (e->w < 1 || e->h < 1 || e->w > 65535 || e->h > 65535)
		return 0;
	if (quality < 1) quality = 1;
	if (quality > 100) quality = 100;
	e->sub = quality < 90;
	e->mcu = e->sub ? 16 : 8;
	e->blocks = e->sub ? 6 : 3;
	e->mcus_x = (e->w + e->mcu - 1) / e->mcu, e->mcus_y = (e->h + e->mcu - 1) / e->mcu;
	scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
	for (t = 0; t < 2; t++)
		for (k = 0; k < 64; k++) {
			int q = ((t ? base_chroma : base_luma)[zigzag[k]] * scale + 50) / 100;

			q = q < 1 ? 1 : q > 255 ? 255 : q;
			e->quant[t][k] = (uint8_t)q;
			e->step[t][zigzag[k]] = 8 * q;
		}
	codes_make(dc_luma_bits, dc_values, e->dc[0]);
	codes_make(dc_chroma_bits, dc_values, e->dc[1]);
	codes_make(ac_luma_bits, ac_luma_values, e->ac[0]);
	codes_make(ac_chroma_bits, ac_chroma_values, e->ac[1]);
	/* some 512 intervals: a decoder's cores take them one by one */
	e->restart = ((uint32_t)(e->mcus_x * e->mcus_y) + 511) / 512;
	e->intervals = ((uint32_t)(e->mcus_x * e->mcus_y) + e->restart - 1) / e->restart;
	return 1;
}

#define HEADERS (2 + 18 + 2 * 69 + 19 + 4 * 2 + 33 + 179 + 33 + 179 + 6 + 14)

/* Everything before the scan's codes: HEADERS bytes. */
static uint8_t *
headers_put(const struct jenc *e, uint8_t *p)
{
	int t, k;

#define B(v) (*p++ = (uint8_t)(v))
#define W(v) (B((v) >> 8), B(v))
	W(0xffd8);
	/* JFIF */
	W(0xffe0), W(16), B('J'), B('F'), B('I'), B('F'), B(0), B(1), B(1), B(0), W(1), W(1), B(0), B(0);
	for (t = 0; t < 2; t++) {
		W(0xffdb), W(67), B(t);
		for (k = 0; k < 64; k++)
			B(e->quant[t][k]);
	}
	W(0xffc0), W(8 + 3 * 3), B(8), W(e->h), W(e->w), B(3);
	B(1), B(e->sub ? 0x22 : 0x11), B(0);
	B(2), B(0x11), B(1);
	B(3), B(0x11), B(1);
	{
		static const struct { int cls; const uint8_t *bits, *values; int n; } tables[4] = {
			{ 0x00, dc_luma_bits, dc_values, 12 }, { 0x10, ac_luma_bits, ac_luma_values, 162 },
			{ 0x01, dc_chroma_bits, dc_values, 12 }, { 0x11, ac_chroma_bits, ac_chroma_values, 162 },
		};

		for (t = 0; t < 4; t++) {
			W(0xffc4), W(2 + 1 + 16 + tables[t].n), B(tables[t].cls);
			for (k = 0; k < 16; k++)
				B(tables[t].bits[k]);
			for (k = 0; k < tables[t].n; k++)
				B(tables[t].values[k]);
		}
	}
	W(0xffdd), W(4), W(e->restart);
	W(0xffda), W(12), B(3), B(1), B(0x00), B(2), B(0x11), B(3), B(0x11), B(0), B(63), B(0);
#undef B
#undef W
	return p;
}

uint8_t *
media_jpeg_encode(const struct media_image *image, int quality, uint32_t *size, int workers)
{
	static struct jenc job;
	struct jenc *e = &job;
	uint32_t total, i, rows_bytes = 0, per;
	uint8_t *f = NULL, *p;
	int got;

	if (!setup(e, image, quality))
		return NULL;
	/* room for an interval: 105 bytes a block first, and where an interval needs more, 420, far more than any but noise
	 * needs (a machine without an MMU maps all of it, in one piece: 5 MB for 800 x 600, which a full memory had not) */
	if (!(e->failed = mcw_alloc(16)))
		return NULL;
	for (per = 105;; per = 420) {
		e->room = (e->restart * (uint32_t)e->blocks * per + 64u + 15u) & ~15u;
		rows_bytes = e->room * e->intervals;
		e->rows = mmap(NULL, (rows_bytes + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		e->row_bytes = m_map(e->intervals * 16);
		if (e->rows == MAP_FAILED || !e->row_bytes) {
			if (e->rows != MAP_FAILED)
				munmap(e->rows, (rows_bytes + 4095) & ~4095u);
			m_unmap(e->row_bytes, e->intervals * 16);
			return NULL;
		}
		*e->failed = 0;
		/* (the first part of each interval's room there before any core writes it: what most take) */
		for (i = 0; i < e->intervals; i++)
			mcw_touch(e->rows + i * e->room, e->room / 8);
		got = m_workers_begin(workers);
		m_for(e->intervals, intervals_job, e, 0, got);
		m_workers_end();
		if (!*e->failed)
			break;
		if (per == 420)
			goto done;
		munmap(e->rows, (rows_bytes + 4095) & ~4095u);
		m_unmap(e->row_bytes, e->intervals * 16);
	}
	total = HEADERS + 2;
	for (i = 0; i < e->intervals; i++)
		total += e->row_bytes[i * 4] + 2;
	*size = total;
	if (!(f = m_map(total)))
		goto done;
	p = headers_put(e, f);
	for (i = 0; i < e->intervals; i++) {
		memcpy(p, e->rows + i * e->room, e->row_bytes[i * 4]);
		p += e->row_bytes[i * 4];
		if (i + 1 < e->intervals)
			*p++ = 0xff, *p++ = (uint8_t)(0xd0 + (i & 7));
	}
	*p++ = 0xff, *p++ = 0xd9;
	*size = (uint32_t)(p - f);
done:
	munmap(e->rows, (rows_bytes + 4095) & ~4095u);
	m_unmap(e->row_bytes, e->intervals * 16);
	return f;
}

uint32_t
m_jpeg_encode_room(void)
{
	return (sizeof(struct jenc) + 15) & ~15u;
}

uint32_t
m_jpeg_encode_alone(const struct media_image *image, int quality, void *work, uint8_t *out, uint32_t room)
{
	struct jenc *e = work;
	uint8_t *p, *end = out + room;
	uint32_t i;

	if (room < HEADERS + 4 || !setup(e, image, quality))
		return 0;
	p = headers_put(e, out);
	for (i = 0; i < e->intervals; i++) {
		struct bitw b = { p, end - 2, 0, 0 };	/* (room for the marker after it) */

		one_interval(e, i, &b);
		if (!b.p)
			return 0;
		p = b.p;
		if (i + 1 < e->intervals)
			*p++ = 0xff, *p++ = (uint8_t)(0xd0 + (i & 7));
	}
	*p++ = 0xff, *p++ = 0xd9;
	return (uint32_t)(p - out);
}
