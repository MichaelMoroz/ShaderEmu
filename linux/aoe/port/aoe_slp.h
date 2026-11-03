/*
 * A menu's picture for 800 x 600 brought down whole to a smaller window (docs/games/aoe.md, "Windowed"): the trial has no
 * pictures for 640 x 480. Shared by aoeprep (prep.c), which makes the menus' pictures on the workers once the trial is
 * unpacked, and the game (port/aoe_scale.cpp), which reads them and makes one itself only if one is not there.
 *
 * A shape (SLP) is a header, a frame's information, its rows' outlines and offsets, and each row's commands. Its first frame
 * is read into pixels, each pixel of the smaller picture is the average of the area it covers in the palette's colours (one
 * more than half transparent is left transparent), mapped back to the palette by the nearest colour of its 15 bits, and the
 * result is written as a shape of copy and skip commands, which every drawing of the game takes as it took the old one.
 */
#ifndef AOE_SLP_H
#define AOE_SLP_H
#include <stdint.h>
#include <string.h>

/* the file's header (32 bytes) and a frame's information (32 bytes), as Shape.h has them */
struct slp_head { char version[4]; int32_t frames; char comment[24]; };
struct slp_frame { uint32_t data, outline; int32_t palette, properties, width, height, hot_x, hot_y; };

/* One row of a frame into pixels: its index, or -1 where nothing is drawn. As ASMDraw_Sprite reads the commands. */
static void slp_row(const uint8_t *s, int16_t lpad, int width, int16_t *out)
{
	long at = lpad, n, i;

	for (i = 0; i < width; i++)
		out[i] = -1;
	if (lpad & 0x8000)
		return;
	for (;;) {
		unsigned c = *s++;
		switch (c & 15) {
		case 0: case 4: case 8: case 12: n = c >> 2; goto copy;
		case 1: case 5: case 9: case 13: at += c >> 2; continue;
		case 2: n = (c & 0xf0) << 4 | *s++; goto copy;
		case 3: n = (c & 0xf0) << 4 | *s++; at += n; continue;
		case 6: n = c >> 4; if (!n) n = *s++; goto copy;		/* a player's colour: as it is */
		case 7: case 10: {
			uint8_t v;
			n = c >> 4; if (!n) n = *s++;
			v = *s++;
			for (i = 0; i < n; i++, at++)
				if (at >= 0 && at < width) out[at] = v;
			continue;
		}
		case 11: n = c >> 4; if (!n) n = *s++; at += n; continue;	/* a shadow: nothing of the picture */
		case 14:
			switch (c >> 4) {
			case 1: at += 1; continue;
			case 2: at += *s++; continue;
			case 4: {						/* the next command, passed over */
				unsigned e = *s++;
				long k = 0;
				switch (e & 15) {
				case 0: case 4: case 8: case 12: k = e >> 2; s += k; break;
				case 1: case 5: case 9: case 13: k = e >> 2; break;
				case 2: k = (e & 0xf0) << 4 | *s++; s += k; break;
				case 3: k = (e & 0xf0) << 4 | *s++; break;
				case 6: k = e >> 4; if (!k) k = *s++; s += k; break;
				case 7: case 10: k = e >> 4; if (!k) k = *s++; s++; break;
				case 11: k = e >> 4; if (!k) k = *s++; break;
				case 14: if ((e >> 4) == 1) k = 1; else if ((e >> 4) == 2) k = *s++; break;
				default: break;
				}
				at += k;
				continue;
			}
			default: continue;
			}
		default: return;	/* 15: the row's end */
		}
	copy:
		for (i = 0; i < n; i++, at++)
			if (at >= 0 && at < width) out[at] = s[i];
		s += n;
	}
}

/* The nearest of a palette's colours to a 15-bit one (r << 10 | g << 5 | b). */
static int slp_nearest(const uint8_t *rgb, unsigned key)
{
	int r = (key >> 10) << 3 | (key >> 12), g = ((key >> 5) & 31) << 3 | ((key >> 7) & 7), b = (key & 31) << 3 | ((key >> 2) & 7);
	int best = 0, bd = 1 << 30, i;

	for (i = 0; i < 256; i++) {
		int dr = r - rgb[i * 3], d = dr * dr, dg, db;
		if (d >= bd) continue;
		dg = g - rgb[i * 3 + 1];
		d += dg * dg;
		if (d >= bd) continue;
		db = b - rgb[i * 3 + 2];
		d += db * db;
		if (d < bd) bd = d, best = i;
	}
	return best;
}

/* The smaller picture's columns or rows: output o is source pixels first[o] and first[o] + 1, with weights w0 and w1 (a source
 * pixel `unit`, an output pixel `span` of them). 0 when an output pixel covers more than two: only down to a half, and then
 * only sizes that keep to two (800 to 640 does: 4 and 1, 3 and 2, 2 and 3, 1 and 4). */
struct slp_cover { int16_t first; uint8_t w0, w1; };
static int slp_cover_make(int from, int to, struct slp_cover *c)
{
	int a = from, b = to, unit, span, o;

	while (b) { int t = a % b; a = b; b = t; }
	unit = to / a, span = from / a;
	for (o = 0; o < to; o++) {
		int u0 = o * span, u1 = u0 + span, first = u0 / unit, end1 = (first + 1) * unit;
		if (u1 > end1 + unit || (first + 1 >= from && u1 > end1))
			return 0;
		c[o].first = (int16_t)(first + 1 < from ? first : from - 2);
		c[o].w0 = (uint8_t)((u1 < end1 ? u1 : end1) - u0);
		c[o].w1 = (uint8_t)(u1 > end1 ? u1 - end1 : 0);
		if (first + 1 >= from) c[o].w1 = c[o].w0, c[o].w0 = 0;
	}
	return span;
}

/* Both coverings of frame 0 of `slp` brought to to_w x to_h, into cx (to_w + to_h of them); 0 if the sizes cannot be. */
static int slp_covers(const uint8_t *slp, int to_w, int to_h, struct slp_cover *cx)
{
	const struct slp_frame *f = (const struct slp_frame *)(slp + sizeof(struct slp_head));

	return slp_cover_make(f->width, to_w, cx) && slp_cover_make(f->height, to_h, cx + to_w);
}

/* A job's part: rows `from` to `to` of the smaller picture, `to_w` x `to_h`, of frame 0 of `slp`, into out[row * to_w + x]
 * (index, or -1), by the coverings slp_covers made. `nearest` maps a 15-bit colour to an index. rows3: 3 x width int16 of
 * the core's own. */
typedef int (*slp_nearest_fn)(void *ctx, unsigned key);
static void slp_scale_rows(const uint8_t *slp, const uint8_t *rgb, int to_w, int from, int to, slp_nearest_fn nearest, void *ctx,
			   const struct slp_cover *cx, int16_t *rows3, int16_t *out)
{
	const struct slp_frame *f = (const struct slp_frame *)(slp + sizeof(struct slp_head));
	const uint32_t *data = (const uint32_t *)(slp + f->data);
	const int16_t *outline = (const int16_t *)(slp + f->outline);
	const struct slp_cover *cy = cx + to_w;
	int w = f->width, have[3] = {-1, -1, -1}, oy, whole = (cx[0].w0 + cx[0].w1) * (cy[0].w0 + cy[0].w1);

	for (oy = from; oy < to; oy++) {
		const int16_t *s[2];
		int16_t *o = out + oy * to_w;
		int k, ox, wy0 = cy[oy].w0, wy1 = cy[oy].w1;

		for (k = 0; k < 2; k++) {
			int sy = cy[oy].first + k, slot = sy % 3;
			if (have[slot] != sy) {
				slp_row(slp + data[sy], outline[sy * 2], w, rows3 + slot * w);
				have[slot] = sy;
			}
			s[k] = rows3 + slot * w;
		}
		for (ox = 0; ox < to_w; ox++) {
			int x = cx[ox].first, p00 = s[0][x], p01 = s[0][x + 1], p10 = s[1][x], p11 = s[1][x + 1];
			int wx0 = cx[ox].w0, wx1 = cx[ox].w1, r = 0, g = 0, b = 0, opaque = 0, wt;

			if (p00 == p01 && p10 == p11 && p00 == p10) { o[ox] = (int16_t)p00; continue; }
			/* (the weight of each, its colour added that many times) */
#define SLP_ADD(p, wgt) if ((p) >= 0 && (wt = (wgt))) r += rgb[(p) * 3] * wt, g += rgb[(p) * 3 + 1] * wt, b += rgb[(p) * 3 + 2] * wt, opaque += wt
			SLP_ADD(p00, wx0 * wy0);
			SLP_ADD(p01, wx1 * wy0);
			SLP_ADD(p10, wx0 * wy1);
			SLP_ADD(p11, wx1 * wy1);
#undef SLP_ADD
			if (opaque * 2 <= whole) { o[ox] = -1; continue; }
			r = (r + opaque / 2) / opaque, g = (g + opaque / 2) / opaque, b = (b + opaque / 2) / opaque;
			o[ox] = (int16_t)nearest(ctx, (unsigned)(r >> 3) << 10 | (unsigned)(g >> 3) << 5 | (unsigned)(b >> 3));
		}
	}
}

/* The most bytes slp_encode writes. */
static long slp_bound(int to_w, int to_h)
{
	return (long)(sizeof(struct slp_head) + sizeof(struct slp_frame)) + (long)to_h * (8 + to_w * 3 + 8);
}

/* The pixels as a shape of one frame, with the old one's header and its hot spot brought down too. Its size. */
static long slp_encode(const uint8_t *old, const int16_t *out, int to_w, int to_h, uint8_t *f)
{
	const struct slp_frame *of = (const struct slp_frame *)(old + sizeof(struct slp_head));
	struct slp_head *fh = (struct slp_head *)f;
	struct slp_frame *nf = (struct slp_frame *)(fh + 1);
	int16_t *outline;
	uint32_t *data, at;
	int y;

	memcpy(fh, old, sizeof *fh);
	fh->frames = 1;
	*nf = *of;
	nf->width = to_w, nf->height = to_h;
	nf->hot_x = of->hot_x * to_w / of->width, nf->hot_y = of->hot_y * to_h / of->height;
	nf->palette = nf->properties = 0;
	nf->outline = sizeof *fh + sizeof *nf;
	nf->data = nf->outline + to_h * 4;
	outline = (int16_t *)(f + nf->outline);
	data = (uint32_t *)(f + nf->data);
	at = nf->data + to_h * 4;
	for (y = 0; y < to_h; y++) {
		const int16_t *p = out + y * to_w;
		int left = 0, right = to_w - 1, x;

		while (left < to_w && p[left] < 0) left++;
		while (right >= left && p[right] < 0) right--;
		data[y] = at;
		if (left > right) {
			outline[y * 2] = (int16_t)0x8000, outline[y * 2 + 1] = 0;
			f[at++] = 0x0f;
			continue;
		}
		outline[y * 2] = (int16_t)left, outline[y * 2 + 1] = (int16_t)(to_w - 1 - right);
		for (x = left; x <= right;) {
			int opaque = p[x] >= 0, n = 0, i;
			while (x + n <= right && (p[x + n] >= 0) == opaque && n < 4095) n++;
			f[at++] = (uint8_t)((n >> 8) << 4 | (opaque ? 2 : 3));
			f[at++] = (uint8_t)n;
			if (opaque)
				for (i = 0; i < n; i++) f[at++] = (uint8_t)p[x + i];
			x += n;
		}
		f[at++] = 0x0f;
	}
	return at;
}

/* aoeprep's file of the menus' pictures (aoe-menus.bin): "AOEMENU1", a count, then for each its picture's and palette's
 * resource numbers, its size, and where its shape lies in the file and how long it is; the shapes, each on 16 bytes. */
struct slp_menus_head { char magic[8]; uint32_t count, pad; };
struct slp_menus_entry { int32_t picture, palette; uint16_t width, height; uint32_t at, bytes, pad; };
#endif
