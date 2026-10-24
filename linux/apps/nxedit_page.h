/*
 * nxedit_page.h: the reader's page (nxedit_read.h), a Markdown text laid out a section at a time.
 * The browser's engine lays out a whole page at once, about 300 instructions a byte of a
 * document (nanox.md, 110 KB: 37 million, 12 s at the machine's speed); here a section is the
 * text from one heading to a later one (RP_FIRST bytes for the first, RP_MORE after), made HTML
 * (nxedit_md.h) and laid out alone, and what it draws is copied into a list of the reader's own,
 * under the sections before it. The first screen is there once its section is; the rest comes
 * while nobody asks for anything. A heading at the start of a line, outside a fenced block,
 * ends every block of the text before it: a section is laid out as it would be in the whole.
 * No part of the window system: tools/web_test.c --sections lays a file out this way too.
 */
#ifndef NXEDIT_PAGE_H
#define NXEDIT_PAGE_H

#include "web_layout.h"
#include "nxedit_md.h"

#define RP_FIRST 2048		/* bytes of text in the first section, at least */
#define RP_MORE 8192		/* and in each after it */

/* A thing to draw (a web_item, its text and its link the page's own). */
struct rp_item { int x, y, w, h, colour, b, text, n, link; unsigned char kind, face, flags; };

struct rp_page {
	struct rp_item *items;
	int count, room;
	unsigned short *text;		/* the items' characters */
	int text_n, text_room;
	char *names;			/* links' addresses and headings' ids, each ending in a 0 */
	int names_n, names_room;
	struct { int name, y; } *anchors;	/* headings: their id (in names) and where they are */
	int anchor_count, anchor_room;
	int width, h, line, done;	/* laid out for this width, so high so far, from the text's line `line` on */
	int next;			/* where the next section's first box goes */
	int lines;			/* the text's lines */
	long long bytes, bytes_done;	/* its bytes, and those laid out (for the page's height before it is all there) */
	unsigned sum;			/* of what was laid out, as web_test sums a page (NXEDIT_DUMP) */
	int nodes, texts, chars, links, boxes, wide;
};

static struct rp_page rp;
static const char *(*rp_line_of)(int i, int *n);
static int rp_base;

static void *
rp_grow(void *block, int *room, int need, int size)
{
	void *to;

	if (need <= *room)
		return block;
	*room = need * 2 + 64;
	if ((to = realloc(block, (size_t)*room * size)) == NULL) {
		fprintf(stderr, "reader: out of memory\n");
		exit(1);
	}
	return to;
}

/* A new page of `lines` lines (line_of gives each) for a width. */
static void
rp_begin(int lines, const char *(*line_of)(int i, int *n), int width)
{
	int i, n;

	rp.count = rp.text_n = rp.names_n = rp.anchor_count = 0;
	rp.width = width, rp.h = rp.next = 0, rp.line = 0, rp.done = lines == 0, rp.lines = lines;
	rp.sum = 2166136261u;
	rp.nodes = rp.texts = rp.chars = rp.links = rp.boxes = rp.wide = 0;
	rp_line_of = line_of;
	for (rp.bytes = rp.bytes_done = 0, i = 0; i < lines; i++) {
		line_of(i, &n);
		rp.bytes += n + 1;
	}
}

static const char *
rp_shifted(int i, int *n)
{
	return rp_line_of(rp_base + i, n);
}

/* Where the section from line `from` ends: a heading at a line's start, outside a fenced block,
 * after `least` bytes. */
static int
rp_section_end(int from, int least)
{
	int i, n, k, bytes = 0, fence = 0, fence_n = 0;
	const char *l;

	for (i = from; i < rp.lines; i++) {
		l = rp_line_of(i, &n);
		for (k = 0; k < n && k < 4 && l[k] == ' '; k++)
			;
		if (k < 4 && k + 2 < n && (l[k] == '`' || l[k] == '~') && l[k + 1] == l[k] && l[k + 2] == l[k]) {
			int run = 0;

			while (k + run < n && l[k + run] == l[k])
				run++;
			if (!fence)
				fence = l[k], fence_n = run;
			else if (l[k] == fence && run >= fence_n)
				fence = 0;
		} else if (!fence && i > from && bytes >= least && n && l[0] == '#') {
			for (k = 0; k < n && l[k] == '#'; k++)
				;
			if (k <= 6 && (k == n || l[k] == ' '))
				return i;
		}
		bytes += n + 1;
	}
	return rp.lines;
}

static int
rp_name(const char *s, int n)
{
	int at = rp.names_n;

	rp.names = rp_grow(rp.names, &rp.names_room, rp.names_n + n + 1, 1);
	memcpy(rp.names + at, s, n);
	rp.names[at + n] = 0;
	rp.names_n += n + 1;
	return at;
}

/* The next section laid out and put under the page so far. */
static void
rp_more(void)
{
	static char *html;
	char name[600];
	int to, size, i, j, dy, top, end, bottom, last_node = -1, last_link = -1, last_name = -1, first = rp.line == 0;

	if (rp.done)
		return;
	to = rp_section_end(rp.line, first ? RP_FIRST : RP_MORE);
	rp_base = rp.line;
	free(html);
	html = md_html(to - rp.line, rp_shifted, &size);
	web_find_sheets(html, size, rp.width);
	web_read_sheets(rp.width, 400);
	web_parse(html, size);
	web_style_all();
	web_layout(rp.width);
	/* the section's first box goes where the last section's end block is (md_html: where a
	 * heading after it would have been) */
	for (top = -1, end = 0, i = 0; i < web_node_count; i++)
		if (web_nodes[i].c.tag != T_TEXT && web_nodes[i].c.parent >= 0 && web_nodes[i].c.parent == web_body) {
			if (top < 0)
				top = web_nodes[i].y;
			end = web_nodes[i].y;
		}
	dy = first ? 0 : rp.next - top;
	rp.next = end + dy;
	for (bottom = 0, i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];
		struct rp_item *r;
		int v[8];

		rp.items = rp_grow(rp.items, &rp.room, rp.count + 1, sizeof *rp.items);
		r = &rp.items[rp.count++];
		r->x = it->x, r->y = it->y + dy, r->w = it->w, r->h = it->h, r->colour = it->colour, r->b = it->b;
		r->kind = it->kind, r->face = it->face, r->flags = it->flags, r->text = 0, r->n = 0, r->link = -1;
		if (it->kind == I_TEXT) {
			rp.text = rp_grow(rp.text, &rp.text_room, rp.text_n + it->b, sizeof *rp.text);
			memcpy(rp.text + rp.text_n, web_u + it->a, it->b * sizeof *rp.text);
			r->text = rp.text_n, r->n = it->b;
			rp.text_n += it->b;
			/* the link it is in: its address kept once for all its texts */
			if (it->node != last_node) {
				last_node = it->node;
				j = web_link_of(it->node);
				if (j >= 0 && j != last_link) {
					rp.links++;
					last_name = web_attr_text(j, "href", name, sizeof name) ? rp_name(name, (int)strlen(name)) : -1;
				}
				last_link = j;
			}
			r->link = last_link >= 0 ? last_name : -1;
			rp.texts++, rp.chars += it->b;
			rp.wide += it->x + it->w > rp.width || it->x < 0;
			for (j = 0; j < it->b; j++)
				rp.sum = (rp.sum ^ web_u[it->a + j]) * 16777619u;
		} else
			rp.boxes++;
		v[0] = r->kind, v[1] = r->x, v[2] = r->y, v[3] = r->w, v[4] = r->h, v[5] = r->face, v[6] = r->colour, v[7] = r->flags;
		for (j = 0; j < 8; j++)
			rp.sum = (rp.sum ^ (unsigned)v[j]) * 16777619u;
		if (it->y + it->h > bottom)
			bottom = it->y + it->h;
	}
	for (i = 0; i < web_node_count; i++)
		if (web_nodes[i].c.tag != T_TEXT && web_attr_text(i, "id", name, sizeof name)) {
			rp.anchors = rp_grow(rp.anchors, &rp.anchor_room, rp.anchor_count + 1, sizeof *rp.anchors);
			rp.anchors[rp.anchor_count].name = rp_name(name, (int)strlen(name));
			rp.anchors[rp.anchor_count++].y = web_nodes[i].y + dy;
		}
	rp.nodes += web_node_count;
	rp.h = bottom + dy;
	for (; rp.line < to; rp.line++) {
		rp_line_of(rp.line, &size);
		rp.bytes_done += size + 1;
	}
	if (to >= rp.lines) {
		rp.done = 1;
		rp.h += web_scaled(MD_BOTTOM);
	}
}

/* Where the heading of id `id` is; -1 when the page so far has none. */
static int
rp_anchor(const char *id)
{
	int i;

	for (i = 0; i < rp.anchor_count; i++)
		if (!strcmp(rp.names + rp.anchors[i].name, id))
			return rp.anchors[i].y;
	return -1;
}

/* How high the whole page will be, from what is laid out of it so far. */
static int
rp_height_guess(void)
{
	if (rp.done || !rp.bytes_done)
		return rp.h;
	return (int)((long long)rp.h * rp.bytes / rp.bytes_done);
}

#endif
