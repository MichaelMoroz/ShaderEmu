/*
 * The browser's page engine on the host (docs/apps/web.md): lays a page out with the image's own
 * fonts and what tools of linux/apps/web/preload.py cached, says what came of it, and draws
 * it into a PPM file. No window system and no machine: a change to css.h, web.h or
 * web_layout.h is tried here first.
 *
 *   gcc -O1 -g -fsanitize=address,undefined -Ilinux/apps tools/web_test.c -o web_test
 *   web_test DIR ADDRESS|FILE [--width 572] [--ppm OUT.ppm] [--items] [--sheets no] [--why X Y] [--failed]
 *
 * A FILE whose name ends in .md is Markdown, made HTML as the editor's reader makes it (nxedit_md.h);
 * with --sections it is laid out a section at a time as the reader does (nxedit_page.h), and the
 * line it prints is the one `NXEDIT_DUMP=1 nxedit FILE` prints in the machine, for the same width.
 *
 * --failed: a picture not in the cache is laid out as the browser lays out one it could not have,
 * a box with a line that says why (web_broken_size), not as its description.
 *
 * DIR has the fonts (font-*.sef) and the cache (web-cache.idx, webc-*). The cache's JPEGs and
 * PNGs are drawn grey unless libmedia is in: -DWEB_TEST_MEDIA -Ilinux/libmedia -Iprograms/mc,
 * linux/libmedia/m_{core,simple,png,png_enc,jpeg,jpeg_enc,gif}.c, linux/libmedia/test/mcw_host.c, -lpthread -lm.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char cache_dir[400];
#define WEB_CACHE_DIR cache_dir
#include "web_layout.h"
#include "nxedit_page.h"
#ifdef WEB_TEST_MEDIA
#include "media.h"
#endif

static char *
slurp(const char *path, int *size)
{
	FILE *f = fopen(path, "rb");
	char *data;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	data = malloc(n + 1);
	*size = (int)fread(data, 1, n, f);
	data[*size] = 0;
	fclose(f);
	return data;
}

static const void *
web_file(const char *name, int *size)
{
	char path[600];

	snprintf(path, sizeof path, "%s/%s", cache_dir, name);
	return slurp(path, size);
}

static char pictures[512][300];	/* the cache's file, or "" for a box that says why it is not */
static int picture_count, failed_boxes;

static int
web_picture(int node, const char *address, int *w, int *h)
{
	char file[400], alt[160], label[240];

	if (picture_count == 512)
		return -1;
	if (!web_cache_find(address, 'I', file, sizeof file, w, h)) {
		if (!failed_boxes)
			return -1;
		/* (as nxweb.c's broken_label says it) */
		if (web_attr_text(node, "alt", alt, sizeof alt) && alt[0])
			snprintf(label, sizeof label, "%s: not in the cache", alt);
		else
			snprintf(label, sizeof label, "Picture not loaded: not in the cache");
		web_broken_size(label, w, h);
		file[0] = 0;
	}
	strcpy(pictures[picture_count], file);
	return picture_count++;
}

/* ---- drawing, as the GPU would ---- */

static unsigned char *pixels;
static int pw, ph;

static void
fill(int x, int y, int w, int h, int colour)
{
	int i, j;

	for (j = y < 0 ? 0 : y; j < y + h && j < ph; j++)
		for (i = x < 0 ? 0 : x; i < x + w && i < pw; i++) {
			unsigned char *p = pixels + 3 * (j * pw + i);

			p[0] = colour >> 16, p[1] = colour >> 8, p[2] = colour;
		}
}

/* A face's atlas of distance fields (font-NAME.msdf for font-NAME-N.sef), read once. */
static const unsigned char *
atlas_of(int face)
{
	static const unsigned char *atlas[WEB_FACES];
	static char tried[WEB_FACES];
	char name[80], *dash;
	int size;

	if (!tried[face]) {
		tried[face] = 1;
		snprintf(name, sizeof name, "%s", web_faces[face].name);
		if ((dash = strrchr(name, '-')) != NULL) {
			strcpy(dash, ".msdf");
			atlas[face] = web_file(name, &size);
		}
	}
	return atlas[face];
}

/* A glyph of distance fields, as the GPU's FRAGMENT_MSDF draws it (src/gpu.h). */
static void
draw_msdf(const struct sef_head *h, const unsigned char *atlas, const uint32_t *entry, int x, int y, int italic, int colour)
{
	int w = entry[1] & 0xffff, gh = entry[1] >> 16, px, py, k;
	double qx = x + (int16_t)(entry[3] & 0xffff) / 64.0, qy = y + (int16_t)(entry[3] >> 16) / 64.0;
	double qw = (entry[2] & 0xffff) / 64.0, qh = (entry[2] >> 16) / 64.0, range = h->range / 65536.0;

	if (w < 2 || gh < 2 || qw <= 0 || qh <= 0)
		return;
	range = 2 * (range < 1 ? 1 : range) * 1.2;	/* a quarter pixel's, times MSDF_SHARP */
	for (py = (int)qy; py <= (int)(qy + qh) + 1; py++)
		for (px = (int)qx - 4; px <= (int)(qx + qw) + 5; px++) {
			double lean = italic ? (h->ascent - (py + 0.5 - y)) / 5.0 : 0;
			double u = (px + 0.5 - lean - qx) / qw, v = (py + 0.5 - qy) / qh, cover = 0;
			int n, c;

			if (u < 0 || u >= 1 || v < 0 || v >= 1 || px < 0 || py < 0 || px >= pw || py >= ph)
				continue;
			/* four samples a pixel, as the fragment's 2 x 2 */
			for (n = 0; n < 4; n++) {
				double su = u + (n & 1 ? 0.25 : -0.25) / qw, sv = v + (n & 2 ? 0.25 : -0.25) / qh;
				double ax, ay, fx, fy, s[3], d;
				int x0, y0;

				su = su < 0 ? 0 : su > 1 ? 1 : su, sv = sv < 0 ? 0 : sv > 1 ? 1 : sv;
				ax = su * (w - 1), ay = sv * (gh - 1);
				x0 = (int)ax < w - 2 ? (int)ax : w - 2, y0 = (int)ay < gh - 2 ? (int)ay : gh - 2;
				fx = ax - x0, fy = ay - y0;
				for (c = 0; c < 3; c++) {
					const unsigned char *p = atlas + entry[0] + 3 * (y0 * w + x0) + c;

					s[c] = ((p[0] * (1 - fx) + p[3] * fx) * (1 - fy) + (p[3 * w] * (1 - fx) + p[3 * w + 3] * fx) * fy) / 255;
				}
				d = s[0] < s[1] ? (s[1] < s[2] ? s[1] : s[0] < s[2] ? s[2] : s[0]) : (s[0] < s[2] ? s[0] : s[1] < s[2] ? s[2] : s[1]);
				d = range * (d - 0.5) + 0.5;
				cover += d < 0 ? 0 : d > 1 ? 1 : d;
			}
			if ((cover /= 4) <= 0)
				continue;
			for (k = 0; k < 3; k++) {
				unsigned char *q = pixels + 3 * (py * pw + px) + k;

				*q = (unsigned char)(*q * (1 - cover) + (colour >> (16 - 8 * k) & 255) * cover + 0.5);
			}
		}
}

static void
draw_text(const struct web_item *it)
{
	const struct web_face *f = &web_faces[it->face];
	const struct sef_head *h = (const struct sef_head *)f->file;
	const unsigned char *atlas = h->bpp == SEF_MSDF ? atlas_of(it->face) : NULL;
	int x = it->x, i, gx, gy;

	for (i = 0; i < it->b; i++) {
		int g = sef_glyph(f->file, web_u[it->a + i] < 32 ? 32 : web_u[it->a + i]);
		const uint32_t *entry;
		const unsigned char *bits;
		int gw, gh, dx, dy;

		if (g < 0)
			continue;
		entry = (const uint32_t *)(f->file + h->table_at + 16 * g);
		if (h->bpp == SEF_MSDF) {
			if (atlas && entry[0])
				draw_msdf(h, atlas, entry, x, it->y, it->flags & IF_ITALIC, it->colour);
			x += sef_advance(f->file, g);
			continue;
		}
		bits = f->file + entry[0];
		gw = entry[1], gh = entry[2];
		dx = (signed char)(entry[3] & 255), dy = (signed char)(entry[3] >> 8 & 255);
		for (gy = 0; gy < gh << f->twice; gy++)
			for (gx = 0; gx < gw << f->twice; gx++) {
				int sx = gx >> f->twice, sy = gy >> f->twice, level;
				int px = x + (dx << f->twice) + gx, py = it->y + (dy << f->twice) + gy, k;
				unsigned char *p;

				if (h->bpp == 1)
					level = bits[sy * ((gw + 7) / 8) + sx / 8] << (sx & 7) & 0x80 ? 15 : 0;
				else
					level = sx & 1 ? bits[sy * ((gw + 1) / 2) + sx / 2] & 15 : bits[sy * ((gw + 1) / 2) + sx / 2] >> 4;
				if (it->flags & IF_ITALIC)
					px += (f->ascent - (dy << f->twice) - gy) / 5;
				if (!level || px < 0 || py < 0 || px >= pw || py >= ph)
					continue;
				p = pixels + 3 * (py * pw + px);
				for (k = 0; k < 3; k++)
					p[k] = (p[k] * (15 - level) + (it->colour >> (16 - 8 * k) & 255) * level) / 15;
			}
		x += sef_advance(f->file, g) << f->twice;
	}
	if (it->flags & IF_UNDER)
		fill(it->x, it->y + f->ascent + 1, it->w, 1, it->colour);
	if (it->flags & IF_THROUGH)
		fill(it->x, it->y + f->ascent * 2 / 3, it->w, 1, it->colour);
}

static void
draw_image(const struct web_item *it)
{
	int size, w, h, start = 0, stride, i, j;
	char *data = pictures[it->a][0] ? slurp(pictures[it->a], &size) : NULL;
	const unsigned char *src;
#ifdef WEB_TEST_MEDIA
	struct media_image out = { 0 };

	/* the cache's JPEGs and PNGs, decoded as the browser decodes them */
	out.channels = 3;
	if (data && size > 2 && strncmp(data, "P6", 2) && !media_image_decode(data, (uint32_t)size, &out, 0)) {
		free(data);
		data = malloc((size_t)out.stride * out.height + 32);
		w = out.width, h = out.height;
		start = sprintf(data, "P6 %d %d 255", w, h);
		memcpy(data + start + 1, out.pixels, (size_t)out.stride * h);
		stride = out.stride;
		media_image_free(&out);
	} else
#endif
	if (!pictures[it->a][0]) {
		/* a box that says why: its frame and its sign (the text is the browser's) */
		fill(it->x, it->y, it->w, it->h, 0xa0a0a0);
		fill(it->x + 1, it->y + 1, it->w - 2, it->h - 2, 0xf0f0f0);
		if (it->w >= 18 && it->h >= 18)
			fill(it->x + 4, it->y + 4, 12, 12, 0xd02020);
		return;
	}
	if (!data || sscanf(data, "P6 %d %d 255%n", &w, &h, &start) != 2) {
		fill(it->x, it->y, it->w, it->h, 0xe0e0e0);
		free(data);
		return;
	} else
		stride = 3 * w;
	start++;
	src = (const unsigned char *)data + start;
	for (j = 0; j < it->h; j++)
		for (i = 0; i < it->w; i++) {
			const unsigned char *from = src + (j * h / it->h) * stride + 3 * (i * w / it->w);
			int x = it->x + i, y = it->y + j;

			if (x >= 0 && y >= 0 && x < pw && y < ph)
				memcpy(pixels + 3 * (y * pw + x), from, 3);
		}
	free(data);
}

static void
draw(const char *out)
{
	FILE *f;
	int i;

	pixels = malloc(3 * pw * ph);
	fill(0, 0, pw, ph, web_page_bg);
	for (i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];

		if (it->kind == I_BOX) {
			if (it->colour >= 0)
				fill(it->x, it->y, it->w, it->h, it->colour);
			if (it->flags) {
				fill(it->x, it->y, it->w, 1, it->b), fill(it->x, it->y + it->h - 1, it->w, 1, it->b);
				fill(it->x, it->y, 1, it->h, it->b), fill(it->x + it->w - 1, it->y, 1, it->h, it->b);
			}
		} else if (it->kind == I_TEXT)
			draw_text(it);
		else if (it->kind == I_IMAGE)
			draw_image(it);
		else {
			fill(it->x, it->y, it->w, it->h, 0x808080);
			fill(it->x + 1, it->y + 1, it->w - 2, it->h - 2, 0xffffff);
		}
	}
	f = fopen(out, "wb");
	fprintf(f, "P6\n%d %d\n255\n", pw, ph);
	fwrite(pixels, 3, pw * ph, f);
	fclose(f);
}

/* Markdown's lines, for md_html */
static const char *md_text;
static int md_ends[200000], md_lines;

static const char *
md_line_of(int i, int *n)
{
	int from = i ? md_ends[i - 1] + 1 : 0;

	*n = md_ends[i] - from;
	return md_text + from;
}

static char *
md_page(const char *text, int *n)
{
	int i;

	md_text = text;
	for (i = 0, md_lines = 0; i < *n && md_lines < 200000; i++)
		if (text[i] == '\n')
			md_ends[md_lines++] = i;
	if ((!md_lines || md_ends[md_lines - 1] != *n - 1) && *n && md_lines < 200000)
		md_ends[md_lines++] = *n;
	return md_html(md_lines, md_line_of, n);
}

int
main(int argc, char **argv)
{
	const char *out = NULL, *where;
	char file[400];
	char *html;
	int width = 572, n, i, list = 0, sheets = 1, texts = 0, boxes = 0, images = 0, fields = 0, wide = 0, over = 0, chars = 0;
	int links = 0, last_link = -1, repeat = 1, why_x = 0, why_y = -1, sections = 0;
	clock_t began = clock();

	if (argc < 3) {
		fprintf(stderr, "web_test DIR ADDRESS|FILE [--width N] [--ppm OUT] [--items] [--sheets no]\n");
		return 2;
	}
	snprintf(cache_dir, sizeof cache_dir, "%s", argv[1]);
	where = argv[2];
	for (i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--width") && i + 1 < argc)
			width = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
			out = argv[++i];
		else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--why") && i + 2 < argc)
			why_x = atoi(argv[i + 1]), why_y = atoi(argv[i + 2]), i += 2;
		else if (!strcmp(argv[i], "--items"))
			list = 1;
		else if (!strcmp(argv[i], "--failed"))
			failed_boxes = 1;
		else if (!strcmp(argv[i], "--sections"))
			sections = 1;
		else if (!strcmp(argv[i], "--sheets") && i + 1 < argc)
			sheets = strcmp(argv[++i], "no") != 0;
	}
	snprintf(web_address, sizeof web_address, "%s", where);
	if (strstr(where, "://")) {
		if (!web_cache_find(where, 'P', file, sizeof file, NULL, NULL)) {
			fprintf(stderr, "not in the cache: %s\n", where);
			return 1;
		}
	} else
		snprintf(file, sizeof file, "%s", where);
	if ((html = slurp(file, &n)) == NULL) {
		fprintf(stderr, "cannot read %s\n", file);
		return 1;
	}
	if (sections && strlen(file) > 3 && !strcmp(file + strlen(file) - 3, ".md")) {
		/* as the editor's reader lays it out, a section at a time (nxedit_page.h), and says it (NXEDIT_DUMP) */
		static struct web_item *copy;

		md_page(html, &n);
		for (i = 0; i < repeat; i++) {
			rp_begin(md_lines, md_line_of, width);
			while (!rp.done)
				rp_more();
		}
		printf("nxedit: read %s: %d nodes, %d x %d; %d texts (%d characters, %d links), %d boxes; %d texts past the edge; sum %08x; %.0f ms\n",
		       where, rp.nodes, rp.width, rp.h, rp.texts, rp.chars, rp.links, rp.boxes, rp.wide, rp.sum,
		       (clock() - began) * 1000.0 / CLOCKS_PER_SEC);
		if (out) {
			copy = calloc(rp.count + 1, sizeof *copy);
			for (i = 0; i < rp.count; i++) {
				const struct rp_item *r = &rp.items[i];

				copy[i] = (struct web_item){ r->x, r->y, r->w, r->h, r->text, r->kind == I_TEXT ? r->n : r->b, r->colour, 0, r->kind, r->face, r->flags };
			}
			web_items = copy, web_item_count = rp.count, web_u = rp.text, web_page_bg = 0xffffff;
			pw = width;
			ph = rp.h < 12000 ? rp.h + 8 : 12000;
			draw(out);
		}
		return 0;
	}
	if (strlen(file) > 3 && !strcmp(file + strlen(file) - 3, ".md"))
		html = md_page(html, &n);
	web_find_sheets(html, n, width);
	for (i = 0; i < web_sheet_count; i++) {
		struct web_sheet *sh = &web_sheets[i];
		char path[400];

		if (sh->state != SHEET_WANTED)
			continue;
		sh->state = SHEET_FAILED;
		if (sheets && (sh->address[0] == '/' ? (snprintf(path, sizeof path, "%s", sh->address), 1) :
			       web_cache_find(sh->address, 'S', path, sizeof path, NULL, NULL)) &&
		    (sh->text = slurp(path, &sh->length)) != NULL)
			sh->state = SHEET_HERE, sh->own = 1;
		printf("sheet %s: %s\n", sh->address, sh->state == SHEET_HERE ? "here" : "not here");
	}
	if (!sheets)
		web_sheet_count = 0;
	for (i = 0; i < repeat; i++) {	/* (--repeat: for a profile) */
		web_read_sheets(width, 400);
		web_parse(html, n);
		web_style_all();
		web_layout(width);
	}
	for (i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];
		int j;

		texts += it->kind == I_TEXT, boxes += it->kind == I_BOX, images += it->kind == I_IMAGE, fields += it->kind == I_FIELD;
		if (it->kind == I_TEXT) {
			chars += it->b;
			wide += it->x + it->w > width || it->x < 0;
			if (web_link_of(it->node) >= 0 && web_link_of(it->node) != last_link)
				links++, last_link = web_link_of(it->node);
			/* text over text is a mistake of the layout's: the items just before it are looked at */
			for (j = i - 1; j >= 0 && j > i - 40; j--) {
				const struct web_item *o = &web_items[j];

				if (o->kind == I_TEXT && it->x < o->x + o->w && o->x < it->x + it->w && it->y + 2 < o->y + o->h - 2 && o->y + 2 < it->y + it->h - 2 &&
				    it->w > 0 && o->w > 0) {
					over++;
					break;
				}
			}
		}
		if (why_y >= 0 && it->kind != I_BOX && why_x >= it->x && why_x < it->x + it->w && why_y >= it->y && why_y < it->y + it->h) {
			/* what the thing at that place is inside, from the inside out */
			int up;

			for (up = web_nodes[it->node].c.tag == T_TEXT ? web_nodes[it->node].c.parent : it->node; up >= 0; up = web_nodes[up].c.parent) {
				const struct css_style *st = web_style_of(up);
				const char *cls = "";
				int cn = 0;

				web_attr(up, "class", &cls, &cn);
				printf("  <%s class=\"%.*s\"> display %d float %d white %d width %d maxw %d minw %d m %d,%d p %d,%d y %d flex-column %d wrap %d grow %d basis %d grid %d\n",
				       css_names + css_name_at[web_nodes[up].c.tag], cn > 60 ? 60 : cn, cls, st->display, st->floated, st->white, st->width, st->maxw, st->minw,
				       st->m[3], st->m[1], st->p[3], st->p[1], web_nodes[up].y, st->column, st->wrap, st->grow, st->basis, st->grid);
			}
			why_y = -1;
		}
		if (list) {
			printf("%c %4d %5d %4d %3d", "TBIF"[it->kind], it->x, it->y, it->w, it->h);
			if (it->kind == I_TEXT) {
				printf(" %06x f%d ", it->colour, it->face);
				for (j = 0; j < it->b && j < 60; j++)
					putchar(web_u[it->a + j] < 127 ? web_u[it->a + j] : '~');
			} else if (it->kind == I_BOX)
				printf(" %06x", it->colour & 0xffffff);
			printf("  <%s>\n", web_nodes[it->node].c.tag == T_TEXT ? "text" : css_names + css_name_at[web_nodes[it->node].c.tag]);
		}
	}
	printf("%s: \"%s\" %d bytes, %d nodes, %d rules, %d sheets; %d x %d; %d texts (%d characters, %d links), %d boxes, %d pictures, %d fields; "
	       "%d texts past the edge, %d over another; %.0f ms\n", where, web_title, n, web_node_count, css_rule_count, web_sheet_count, width,
	       web_page_h, texts, chars, links, boxes, images, fields, wide, over, (clock() - began) * 1000.0 / CLOCKS_PER_SEC);
	if (out) {
		pw = width;
		ph = web_page_h < 12000 ? web_page_h + 8 : 12000;
		draw(out);
	}
	return 0;
}
