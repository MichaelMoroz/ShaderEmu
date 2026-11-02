/*
 * ttf.h: a TrueType face's outlines as pixels' coverage, for aoeprep (prep.c): the tables it needs (head, hhea, hmtx,
 * cmap format 4, loca, glyf with composite glyphs), quadratic curves cut into lines, and each line's signed area
 * added into a row of cells whose running sum is the coverage. No hinting. Memory from the caller's `alloc`.
 */
#ifndef SHADEREMU_AOE_TTF_H
#define SHADEREMU_AOE_TTF_H

#include <math.h>
#include <stdint.h>
#include <string.h>

struct ttf {
	const uint8_t *d;
	uint32_t size, glyf, loca, hmtx, cmap4;
	int units, long_loca, metrics, glyphs, ascent, descent;
};

static uint32_t ttf_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int ttf_u16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static int ttf_s16(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }

static uint32_t
ttf_table(const struct ttf *t, const char *tag)
{
	int n = ttf_u16(t->d + 4), i;

	for (i = 0; i < n; i++) {
		const uint8_t *e = t->d + 12 + i * 16;

		if (!memcmp(e, tag, 4) && ttf_u32(e + 8) + ttf_u32(e + 12) <= t->size)
			return ttf_u32(e + 8);
	}
	return 0;
}

/* 0, or -1 when it is not a TrueType face this reads */
static int
ttf_open(struct ttf *t, const uint8_t *d, uint32_t size)
{
	uint32_t head, hhea, maxp, cmap;
	int n, i;

	memset(t, 0, sizeof *t);
	t->d = d, t->size = size;
	if (size < 12 || (ttf_u32(d) != 0x00010000 && memcmp(d, "true", 4)))
		return -1;
	head = ttf_table(t, "head"), hhea = ttf_table(t, "hhea"), maxp = ttf_table(t, "maxp"), cmap = ttf_table(t, "cmap");
	t->glyf = ttf_table(t, "glyf"), t->loca = ttf_table(t, "loca"), t->hmtx = ttf_table(t, "hmtx");
	if (!head || !hhea || !maxp || !cmap || !t->glyf || !t->loca || !t->hmtx)
		return -1;
	t->units = ttf_u16(d + head + 18), t->long_loca = ttf_s16(d + head + 50);
	t->ascent = ttf_s16(d + hhea + 4), t->descent = ttf_s16(d + hhea + 6), t->metrics = ttf_u16(d + hhea + 34);
	t->glyphs = ttf_u16(d + maxp + 4);
	n = ttf_u16(d + cmap + 2);
	for (i = 0; i < n; i++) {	/* Windows' Unicode map, format 4 */
		const uint8_t *e = d + cmap + 4 + i * 8;
		uint32_t at = cmap + ttf_u32(e + 4);

		if (ttf_u16(e) == 3 && (ttf_u16(e + 2) == 1 || ttf_u16(e + 2) == 0) && at + 14 < size && ttf_u16(d + at) == 4) {
			t->cmap4 = at;
			break;
		}
	}
	return t->units > 0 && t->cmap4 ? 0 : -1;
}

static int
ttf_glyph_index(const struct ttf *t, int code)
{
	const uint8_t *m = t->d + t->cmap4;
	int segs = ttf_u16(m + 6) / 2, i;

	if (code > 0xffff)
		return 0;
	for (i = 0; i < segs; i++) {
		int end = ttf_u16(m + 14 + i * 2), start, delta, range;

		if (code > end)
			continue;
		start = ttf_u16(m + 16 + segs * 2 + i * 2), delta = ttf_s16(m + 16 + segs * 4 + i * 2);
		range = ttf_u16(m + 16 + segs * 6 + i * 2);
		if (code < start)
			return 0;
		if (!range)
			return (code + delta) & 0xffff;
		{
			const uint8_t *g = m + 16 + segs * 6 + i * 2 + range + (code - start) * 2;
			int id;

			if (g + 2 > t->d + t->size)
				return 0;
			id = ttf_u16(g);
			return id ? (id + delta) & 0xffff : 0;
		}
	}
	return 0;
}

static int
ttf_advance(const struct ttf *t, int glyph)
{
	int k = glyph < t->metrics ? glyph : t->metrics - 1;

	return ttf_u16(t->d + t->hmtx + k * 4);
}

/* A glyph's outline in the table: its place and length (0: an empty glyph) */
static uint32_t
ttf_glyph_at(const struct ttf *t, int glyph, uint32_t *length)
{
	uint32_t a, b;

	if (glyph < 0 || glyph >= t->glyphs)
		return *length = 0, 0;
	if (t->long_loca)
		a = ttf_u32(t->d + t->loca + glyph * 4), b = ttf_u32(t->d + t->loca + glyph * 4 + 4);
	else
		a = 2u * ttf_u16(t->d + t->loca + glyph * 2), b = 2u * ttf_u16(t->d + t->loca + glyph * 2 + 2);
	*length = b > a ? b - a : 0;
	return *length && t->glyf + b <= t->size ? t->glyf + a : 0;
}

/* The pixels a glyph covers at `scale` (pixels a font unit), y down from the baseline: [x0, x1) x [y0, y1) */
static void
ttf_box(const struct ttf *t, int glyph, float scale, int *x0, int *y0, int *x1, int *y1)
{
	uint32_t length, at = ttf_glyph_at(t, glyph, &length);

	*x0 = *y0 = *x1 = *y1 = 0;
	if (!at)
		return;
	*x0 = (int)floorf(ttf_s16(t->d + at + 2) * scale), *y1 = (int)ceilf(-ttf_s16(t->d + at + 4) * scale);
	*x1 = (int)ceilf(ttf_s16(t->d + at + 6) * scale), *y0 = (int)floorf(-ttf_s16(t->d + at + 8) * scale);
}

/* ---------------------------------------------------------------- coverage */

struct ttf_raster {
	float *cells;		/* (w + 2) x h, a row's running sum its coverage */
	int w, h;
	float dx, dy, scale;	/* a font unit's place: x * scale + dx, dy - y * scale */
	uint8_t *mem;		/* room for the points of the glyphs (a composite's parts each) */
	size_t left;
};

/* A line's signed area into the cells it crosses, row by row */
static void
ttf_line(struct ttf_raster *r, float x0, float y0, float x1, float y1)
{
	float dir, dxdy, x;
	int y, last;

	if (y0 == y1)
		return;
	if (y0 < y1)
		dir = 1.0f;
	else {
		float s;

		dir = -1.0f;
		s = x0, x0 = x1, x1 = s;
		s = y0, y0 = y1, y1 = s;
	}
	dxdy = (x1 - x0) / (y1 - y0);
	x = x0;
	if (y0 < 0) {
		x -= y0 * dxdy;
		y0 = 0;
	}
	last = (int)ceilf(y1) < r->h ? (int)ceilf(y1) : r->h;
	for (y = (int)y0; y < last; y++) {
		float *row = r->cells + (size_t)y * (r->w + 2);
		float dy = (y + 1 < y1 ? (float)(y + 1) : y1) - (y > y0 ? (float)y : y0), next = x + dxdy * dy, d = dy * dir;
		float lo = x < next ? x : next, hi = x < next ? next : x;
		int i0, i1, i;

		lo = lo < 0 ? 0 : lo > (float)r->w ? (float)r->w : lo;
		hi = hi < 0 ? 0 : hi > (float)r->w ? (float)r->w : hi;
		i0 = (int)floorf(lo), i1 = (int)ceilf(hi);
		if (i1 <= i0 + 1) {
			/* within one cell: its share right of the line's middle there, the rest to the next */
			float mid = 0.5f * (lo + hi) - (float)i0;

			row[i0] += d - d * mid;
			row[i0 + 1] += d * mid;
		} else {
			float s = 1.0f / (hi - lo), f0 = lo - (float)i0, a0 = 0.5f * s * (1 - f0) * (1 - f0);
			float f1 = hi - (float)i1 + 1, am = 0.5f * s * f1 * f1;

			row[i0] += d * a0;
			if (i1 == i0 + 2)
				row[i0 + 1] += d * (1 - a0 - am);
			else {
				float a1 = s * (1.5f - f0), a2;

				row[i0 + 1] += d * (a1 - a0);
				for (i = i0 + 2; i < i1 - 1; i++)
					row[i] += d * s;
				a2 = a1 + (float)(i1 - i0 - 3) * s;
				row[i1 - 1] += d * (1 - a2 - am);
			}
			row[i1] += d * am;
		}
		x = next;
	}
}

static void
ttf_curve(struct ttf_raster *r, float x0, float y0, float cx, float cy, float x1, float y1)
{
	float dx = x0 - 2 * cx + x1, dy = y0 - 2 * cy + y1, dd = sqrtf(dx * dx + dy * dy), px = x0, py = y0;
	int n = 1 + (int)sqrtf(dd * 3.0f), i;

	for (i = 1; i <= n; i++) {
		float t = (float)i / (float)n, u = 1 - t;
		float qx = u * u * x0 + 2 * u * t * cx + t * t * x1, qy = u * u * y0 + 2 * u * t * cy + t * t * y1;

		ttf_line(r, px, py, qx, qy);
		px = qx, py = qy;
	}
}

/* A glyph's contours (a composite's parts too, moved and scaled as it says) into the cells; m is the 2 x 3 matrix of
 * font units to the glyph's. `depth` keeps a bad face from going round. */
static void
ttf_outline(const struct ttf *t, struct ttf_raster *r, int glyph, const float m[6], int depth)
{
	uint32_t length, at = ttf_glyph_at(t, glyph, &length), end;
	const uint8_t *p, *flags;
	int contours, points, i, c, first;

	if (!at || depth > 8)
		return;
	end = at + length;
	contours = ttf_s16(t->d + at);
	p = t->d + at + 10;
	if (contours < 0) {
		int more = 1;

		while (more && p + 4 <= t->d + end) {
			int fl = ttf_u16(p), part = ttf_u16(p + 2), ox, oy;
			float a = 1, b = 0, cc = 0, dd = 1, n[6];

			p += 4;
			if (fl & 1)
				ox = ttf_s16(p), oy = ttf_s16(p + 2), p += 4;
			else
				ox = (int8_t)p[0], oy = (int8_t)p[1], p += 2;
			if (!(fl & 2))
				ox = oy = 0;	/* (points matched, not offsets: not in these faces) */
			if (fl & 8)
				a = dd = ttf_s16(p) / 16384.0f, p += 2;
			else if (fl & 0x40)
				a = ttf_s16(p) / 16384.0f, dd = ttf_s16(p + 2) / 16384.0f, p += 4;
			else if (fl & 0x80)
				a = ttf_s16(p) / 16384.0f, b = ttf_s16(p + 2) / 16384.0f, cc = ttf_s16(p + 4) / 16384.0f, dd = ttf_s16(p + 6) / 16384.0f, p += 8;
			n[0] = m[0] * a + m[2] * b, n[1] = m[1] * a + m[3] * b;
			n[2] = m[0] * cc + m[2] * dd, n[3] = m[1] * cc + m[3] * dd;
			n[4] = m[0] * ox + m[2] * oy + m[4], n[5] = m[1] * ox + m[3] * oy + m[5];
			ttf_outline(t, r, part, n, depth + 1);
			more = fl & 0x20;
		}
		return;
	}
	{
		/* the points: their ends, the instructions passed by, flags, then x and y as deltas */
		const uint8_t *ends = p, *xs, *ys;
		float *px, *py;
		uint8_t *on;
		int x = 0, y = 0;
		size_t need;

		points = contours ? ttf_u16(ends + (contours - 1) * 2) + 1 : 0;
		need = (size_t)points * 9 + 16;
		if (need > r->left)
			return;
		px = (float *)r->mem, py = px + points, on = (uint8_t *)(py + points);
		r->mem += (need + 15) & ~(size_t)15, r->left -= (need + 15) & ~(size_t)15;
		p = ends + contours * 2;
		p += 2 + ttf_u16(p);
		flags = p;
		/* the flags' length first, then x's */
		{
			const uint8_t *q = flags;
			int k = 0, xbytes = 0;

			while (k < points) {
				int fl = *q++, rep = 1;

				if (fl & 8)
					rep += *q++;
				xbytes += rep * ((fl & 2) ? 1 : (fl & 16) ? 0 : 2);
				k += rep;
			}
			xs = q, ys = q + xbytes;
		}
		for (i = 0; i < points;) {
			int fl = *flags++, rep = 1, k;

			if (fl & 8)
				rep += *flags++;
			for (k = 0; k < rep && i < points; k++, i++) {
				if (fl & 2)
					x += (fl & 16) ? *xs : -*xs, xs++;
				else if (!(fl & 16))
					x += ttf_s16(xs), xs += 2;
				on[i] = fl & 1;
				px[i] = (float)x;
			}
		}
		flags = p;	/* again, for y */
		for (i = 0; i < points;) {
			int fl = *flags++, rep = 1, k;

			if (fl & 8)
				rep += *flags++;
			for (k = 0; k < rep && i < points; k++, i++) {
				if (fl & 4)
					y += (fl & 32) ? *ys : -*ys, ys++;
				else if (!(fl & 32))
					y += ttf_s16(ys), ys += 2;
				py[i] = (float)y;
			}
		}
		for (i = 0; i < points; i++) {
			float fx = px[i], fy = py[i];

			px[i] = (m[0] * fx + m[2] * fy + m[4]) * r->scale + r->dx;
			py[i] = r->dy - (m[1] * fx + m[3] * fy + m[5]) * r->scale;
		}
		for (c = 0, first = 0; c < contours; c++) {
			int last = ttf_u16(ends + c * 2), n = last - first + 1, s, k;
			float sx, sy, cx = 0, cy = 0, lx, ly;
			int curving = 0;

			if (n < 2 || last >= points) {
				first = last + 1;
				continue;
			}
			/* begin on a point on the curve (or between two off it) */
			for (s = 0; s < n && !on[first + s]; s++)
				continue;
			if (s == n)
				sx = 0.5f * (px[first] + px[first + 1]), sy = 0.5f * (py[first] + py[first + 1]), s = 0;
			else
				sx = px[first + s], sy = py[first + s], s++;
			lx = sx, ly = sy;
			for (k = 0; k < n; k++) {
				int j = first + (s + k) % n;

				if (on[j]) {
					if (curving)
						ttf_curve(r, lx, ly, cx, cy, px[j], py[j]);
					else
						ttf_line(r, lx, ly, px[j], py[j]);
					lx = px[j], ly = py[j], curving = 0;
				} else {
					if (curving) {
						float mx = 0.5f * (cx + px[j]), my = 0.5f * (cy + py[j]);

						ttf_curve(r, lx, ly, cx, cy, mx, my);
						lx = mx, ly = my;
					}
					cx = px[j], cy = py[j], curving = 1;
				}
			}
			if (curving)
				ttf_curve(r, lx, ly, cx, cy, sx, sy);
			else
				ttf_line(r, lx, ly, sx, sy);
			first = last + 1;
		}
	}
}

/*
 * A glyph's pixels as bits: the glyph's box is w x h with its top left at (x0, y0) of ttf_box, and a pixel whose coverage
 * is `least` of 255 or more sets bit (0x80 >> (x & 7)) of out[y * stride + x / 8] at x = x0 + i, y = y0 + j + oy within
 * out_w x out_h. `cells` has (w + 2) * h floats, all 0, and is left so (only cells a line wrote are stored to: a core's
 * stores a pass are few); `mem` `left` bytes for its points (16-aligned).
 */
static void
ttf_bits(const struct ttf *t, int glyph, float scale, int x0, int y0, int w, int h, float *cells, void *mem, size_t left,
	 int least, uint8_t *out, int stride, int out_w, int out_h, int oy)
{
	static const float unit[6] = { 1, 0, 0, 1, 0, 0 };
	struct ttf_raster r;
	float edge = (least - 0.5f) / 255.0f;
	int x, y;

	r.cells = cells, r.w = w, r.h = h, r.scale = scale, r.dx = (float)-x0, r.dy = (float)-y0, r.mem = mem, r.left = left;
	ttf_outline(t, &r, glyph, unit, 0);
	for (y = 0; y < h; y++) {
		float *row = cells + (size_t)y * (w + 2), sum = 0;
		int py = y0 + y + oy;

		for (x = 0; x < w + 2; x++) {
			int px = x0 + x;

			if (row[x] != 0) {
				sum += row[x];
				row[x] = 0;
			}
			if (x < w && px >= 0 && px < out_w && py >= 0 && py < out_h && fabsf(sum) >= edge)
				out[py * stride + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
		}
	}
}

#endif
