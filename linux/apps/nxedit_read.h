/*
 * nxedit_read.h: the editor's reader (docs/system/nanox.md, "The editor's reader"). A text read as
 * Markdown is made HTML (nxedit_md.h) and laid out by the browser's engine (web.h, web_layout.h)
 * for the window's width, a section at a time (nxedit_page.h), with the image's fonts
 * (docs/system/fonts.md); the page is drawn where the text's rows would be. What is in view is
 * laid out first, the rest while no event waits; all of it again only when the text, the tab or
 * the width changes. Included by nxedit.c, whose text, window and drawing it uses.
 */
#include <sys/mman.h>
#define CSS_ATOMS 1024		/* (a page of Markdown has its tags and a few names: the browser's 8192 are 232 KB) */
#define WEB_FACES_LATER		/* (the engine's 60 KB of faces allocated when a page is first laid out, not cleared at every start) */
#include "nxedit_page.h"

#define READ_LINE 40		/* pixels an arrow key or a notch of the wheel moves */

static struct doc *laid_doc;	/* the text the page is of, at lex_gen laid_gen, laid_width wide */
static unsigned laid_gen;
static int laid_width = -1, laid_count, read_drawn_y = -1, read_drawn_layout = -1, read_drawn_h = -1;
static int read_plain_tried, read_dump, read_dumped, read_timed;
static struct timespec read_began;
static GR_FONT_ID read_fonts[WEB_FACES][2];

/* ---- what the engine asks for ---- */

static const void *
web_file(const char *name, int *size)
{
	char path[80];
	struct stat st;
	void *bytes;
	int fd;

	snprintf(path, sizeof path, "/usr/share/%s", name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return NULL;
	bytes = fstat(fd, &st) == 0 ? mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
	close(fd);
	*size = (int)st.st_size;
	return bytes == MAP_FAILED ? NULL : bytes;
}

static int
web_picture(int node, const char *address, int *w, int *h)
{
	(void)node, (void)address, (void)w, (void)h;
	return -1;	/* (a picture is its description: md_inline) */
}

/* ---- laying out ---- */

static int
read_view_w(void)
{
	return width - UI_SCROLL_W;
}

static int
read_view_h(void)
{
	int h = height - text_top - STATUS;

	return h > 1 ? h : 1;
}

/* The ROM's fonts, when the image has them and the window system draws them; else the window
 * system's own (as nxweb does). */
static void
read_fonts_check(void)
{
	static unsigned char widths[256];
	GR_FONT_INFO info, sef;
	GR_FONT_ID test = 0;
	int plain = web_face_for(0, 0, CSS_EM), c;

	if (read_plain_tried)
		return;
	read_plain_tried = 1;
	if ((web_faces = calloc(WEB_FACES, sizeof *web_faces)) == NULL) {
		fprintf(stderr, "reader: out of memory\n");
		exit(1);
	}
	memset(&sef, 0, sizeof sef);
	if (web_face(plain)->file && (test = GrCreateFontEx("/usr/share/font-sans-13.sef", 0, 0, NULL)) != 0)
		GrGetFontInfo(test, &sef);
	if (!test || sef.lastchar != 0xffff || getenv("NXEDIT_PLAIN")) {
		GrGetFontInfo(ui_var, &info);
		for (c = 0; c < 256; c++)
			widths[c] = info.widths[c >= info.firstchar && c <= info.lastchar ? c : info.firstchar];
		memset(web_faces, 0, WEB_FACES * sizeof *web_faces);
		web_plain = widths;
		web_plain_height = info.height;
		web_plain_ascent = info.baseline;
		if (test)
			GrDestroyFont(test);
	} else if (!strcmp(web_faces[plain].name, "font-sans-13.sef"))
		read_fonts[plain][0] = test;
}

static const char *
read_line_of(int i, int *n)
{
	*n = laid_doc->lines[i].length;
	return laid_doc->lines[i].text;
}

static long
read_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)((t.tv_sec - read_began.tv_sec) * 1000 + (t.tv_nsec - read_began.tv_nsec) / 1000000);
}

/* One more section of the page; what the page is said to be once it is all there. */
static void
read_step(void)
{
	rp_more();
	laid_count++;
	if (rp.done && getenv("NXEDIT_TIME"))
		fprintf(stderr, "nxedit: %s read: %d items, %d x %d, all in %ld ms\n", laid_doc->path, rp.count, rp.width, rp.h, read_ms());
	if (rp.done && read_dump && !read_dumped) {
		/* what was laid out, and a sum of it, as tools/web_test.c --sections says a file */
		read_dumped = 1;
		printf("nxedit: read %s: %d nodes, %d x %d; %d texts (%d characters, %d links), %d boxes; %d texts past the edge; sum %08x\n",
		       laid_doc->path, rp.nodes, rp.width, rp.h, rp.texts, rp.chars, rp.links, rp.boxes, rp.wide, rp.sum);
		fflush(stdout);
	}
}

/* The page of the text as it is now, for the window's width, laid out down to page row `y`
 * (and everything when `y` is past its end). */
static void
read_layout(int y)
{
	int w = read_view_w();

	if (laid_doc != d || laid_gen != lex_gen || laid_width != w) {
		clock_gettime(CLOCK_MONOTONIC, &read_began);
		read_fonts_check();
		laid_doc = d, laid_gen = lex_gen, laid_width = w;
		read_dumped = read_timed = 0;
		rp_begin(d->count, read_line_of, w);
		laid_count++;
	}
	while (!rp.done && rp.h < y)
		read_step();
	if (!read_timed && rp.line && (read_timed = 1) && getenv("NXEDIT_TIME"))
		fprintf(stderr, "nxedit: %s read: the first %d lines in %ld ms\n", d->path, rp.line, read_ms());
}

/* Whether part of the page is still to be laid out (and is, while nothing else is to be done). */
static int
read_pending(void)
{
	return d->reading && laid_doc == d && laid_gen == lex_gen && !rp.done;
}

static int
read_most(void)
{
	int most = rp.h - read_view_h();

	return most > 0 ? most : 0;
}

/* The view's top at page row y, the page laid out under it as far as the view goes. */
static void
read_to(int y)
{
	read_layout(y + read_view_h());
	d->read_y = y > read_most() ? read_most() : y < 0 ? 0 : y;
}

/* ---- drawing ---- */

static GR_COLOR
read_rgb(int rgb)
{
	return MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
}

/* Characters in one of the ROM's faces, their top left corner at x, y (nxweb's page_text). */
static void
read_text(int x, int y, const unsigned short *text, int n, int face, int italic, int colour)
{
	GR_FONT_ID *font = &read_fonts[face][italic];

	if (web_plain) {
		/* the window system's own font: Latin letters, bold by drawing twice */
		char plain[256];
		int i, bold = face < FACE_UNI && (face / WEB_SIZES) % 2;

		for (; n > 0; n -= i, text += i, x += ui_width(&ui_var_widths, ui_var, plain, i)) {
			for (i = 0; i < n && i < 255; i++)
				plain[i] = text[i] >= 32 && text[i] < 127 ? text[i] : text[i] < 32 ? ' ' : '?';
			ui_text(window, x, y, plain, i, read_rgb(colour), 0);
			if (bold)
				ui_text(window, x + 1, y, plain, i, read_rgb(colour), 0);
		}
		return;
	}
	if (!*font) {
		char path[80];

		snprintf(path, sizeof path, "/usr/share/%s", web_faces[face].name);
		/* (the height asks for a size, the width for a slant in 256ths of a pixel a row) */
		*font = GrCreateFontEx(path, web_faces[face].twice ? 32 : 0, italic ? 51 : 0, NULL);
	}
	GrSetGCFont(ui_gc, *font);
	GrSetGCForeground(ui_gc, read_rgb(colour));
	GrText(window, ui_gc, x, y, (void *)text, n, GR_TFUC16 | GR_TFTOP);
}

/* The page's part in view, on the window where the text's rows are. What it draws past the
 * view's edges (a line half in view) the menu, the tabs and the status line cover again. */
static void
read_draw_page(void)
{
	int vw = read_view_w(), vh = read_view_h(), top = d->read_y, i, y;

	ui_fill(window, 0, text_top, vw, vh, WHITE);
	for (i = 0; i < rp.count; i++) {
		const struct rp_item *it = &rp.items[i];

		if (it->y >= top + vh || it->y + it->h <= top)
			continue;
		y = it->y - top + text_top;
		switch (it->kind) {
		case I_BOX:
			if (it->colour >= 0 && it->w > 0 && it->h > 0)
				ui_fill(window, it->x, y, it->w, it->h, read_rgb(it->colour));
			if (it->flags && it->w > 0) {
				GrSetGCForeground(ui_gc, read_rgb(it->b));
				GrRect(window, ui_gc, it->x, y, it->w, it->h);
			}
			break;
		case I_TEXT:
			read_text(it->x, y, rp.text + it->text, it->n, it->face, (it->flags & IF_ITALIC) != 0, it->colour);
			if (it->flags & IF_UNDER)
				ui_fill(window, it->x, y + web_faces[it->face].ascent + 1, it->w, 1, read_rgb(it->colour));
			if (it->flags & IF_THROUGH)
				ui_fill(window, it->x, y + web_faces[it->face].ascent * 2 / 3, it->w, 1, read_rgb(it->colour));
			break;
		default:
			ui_fill(window, it->x, y, it->w, it->h, MWRGB(232, 232, 232));
		}
	}
	read_drawn_y = d->read_y, read_drawn_layout = laid_count;
}

static void
read_scrollbar(void)
{
	ui_scrollbar(window, width - UI_SCROLL_W, text_top, height - text_top - STATUS, rp_height_guess(), read_view_h(), d->read_y);
	read_drawn_h = rp_height_guess();
}

/* flush() for a text being read. */
static void
read_flush(void)
{
	read_to(d->read_y);
	if (dirty_all || read_drawn_y != d->read_y) {
		read_draw_page();
		draw_bar();
		draw_tabs();
		read_scrollbar();
		draw_status();
	} else {
		if (read_drawn_layout != laid_count) {
			/* (a section laid out under the view: only the bar's length changes) */
			read_drawn_layout = laid_count;
			if (read_drawn_h != rp_height_guess())
				read_scrollbar();
			status_dirty = 1;
		}
		if (tabs_dirty)
			draw_tabs();
		if (status_dirty)
			draw_status();
	}
	dirty_from = FAR, dirty_to = -1, dirty_all = tabs_dirty = 0;
	drawn_top = -1;		/* (the rows are not on the window: the editor draws them all again) */
}

/* The status line's right part while reading. */
static void
read_status(char *right)
{
	int most = rp_height_guess() - read_view_h(), part = most > 0 ? (int)((long long)d->read_y * 100 / most) : 100;

	part = part > 100 ? 100 : part;
	put(put(put(put_number(put(right, rp.done ? "Reading    " : "Reading (laying out)    "), part), "%"), "    Ctrl+R edits"), "");
}

/* ---- moving, and links ---- */

static void
read_scroll(int by)
{
	read_to(d->read_y + by);
}

/* Where the text's view and the page's agree, roughly: the same share of each. */
static void
read_from_rows(void)
{
	read_layout(0);
	read_to(d->count > 1 ? (int)((long long)d->top * rp_height_guess() / d->count) : 0);
}

static void
rows_from_read(void)
{
	int h = rp_height_guess(), y = h > 0 && laid_doc == d ? (int)((long long)d->read_y * d->count / h) : d->top;

	d->top = y < 0 ? 0 : y >= d->count ? d->count - 1 : y;
	d->sub = 0;
	if (d->cy < d->top || d->cy >= d->top + rows)
		d->cy = d->top, d->cx = 0, d->marked = 0;
}

/* The heading whose id is `id` brought to the view's top; false if there is none. */
static int
read_anchor(const char *id)
{
	int y;

	read_layout(0);
	while ((y = rp_anchor(id)) < 0 && !rp.done)
		read_step();
	if (y < 0)
		return 0;
	read_to(y - 4);
	return 1;
}

static void
read_back_push(const char *path)
{
	char **to = realloc(d->back, (d->back_count + 1) * sizeof *d->back);

	if (!to)
		return;
	d->back = to;
	if ((d->back[d->back_count] = strdup(path)) != NULL)
		d->back_count++;
}

/* Another file read in this tab, its place `id` in view: the one before it is kept for Back.
 * A text with changes stays: the file goes in a tab of its own. */
static void
read_go(const char *path, const char *id, int forward)
{
	char **back = d->back, was[512];
	int count = d->back_count, i;

	for (i = 0; i < doc_count; i++)
		if (docs[i].named && !strcmp(docs[i].path, path))
			break;
	if (i < doc_count || changed(d) || !d->named) {
		open_file(path);
		d->reading = 1;
	} else {
		snprintf(was, sizeof was, "%s", d->path);
		d->back = NULL, d->back_count = 0;
		clear_doc(d);
		load(path);
		d->reading = 1;
		d->back = back, d->back_count = count;
		if (forward)
			read_back_push(was);
		switch_to(current);
		tabs_dirty = 1;
	}
	if (id && id[0] && !read_anchor(id))
		say("no such heading in this text");
	dirty_all = 1;
}

static void
read_back(void)
{
	char path[512];

	if (!d->back_count) {
		say("nothing to go back to");
		return;
	}
	snprintf(path, sizeof path, "%s", d->back[--d->back_count]);
	free(d->back[d->back_count]);
	read_go(path, NULL, 0);
}

/* What the link under a point of the window leads to: another text of the image (read here),
 * a heading of this one, or an address (given to open). */
static int
read_link(int px, int py)
{
	char href[600], path[700], *hash, *slash;
	int i, y = py - text_top + d->read_y;
	struct stat st;

	for (i = rp.count - 1; i >= 0; i--) {
		const struct rp_item *it = &rp.items[i];

		if (it->kind != I_TEXT || !ui_inside(px, y, it->x, it->y, it->w, it->h))
			continue;
		if (it->link < 0)
			return 0;
		snprintf(href, sizeof href, "%s", rp.names + it->link);
		if (href[0] == '#') {
			if (!read_anchor(href + 1))
				say("no such heading in this text");
			return 1;
		}
		if (strstr(href, "://") || !strncmp(href, "mailto:", 7)) {
			signal(SIGCHLD, SIG_IGN);
			ui_run("open", href);
			snprintf(notice, sizeof notice, "opening %.150s", href);
			return 1;
		}
		if ((hash = strchr(href, '#')) != NULL)
			*hash++ = 0;
		if (href[0] == '/')
			snprintf(path, sizeof path, "%s", href);
		else {
			snprintf(path, sizeof path, "%s", d->path);
			slash = strrchr(path, '/');
			snprintf(slash ? slash + 1 : path, sizeof path - (slash ? slash + 1 - path : 0), "%s", href);
		}
		if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
			snprintf(notice, sizeof notice, "%.100s is not in the image", href);
			return 1;
		}
		/* (a file of another kind is opened as the file manager would open it) */
		if (strlen(path) < 3 || strcasecmp(path + strlen(path) - 3, ".md")) {
			signal(SIGCHLD, SIG_IGN);
			ui_run("open", path);
			return 1;
		}
		read_go(path, hash, 1);
		return 1;
	}
	return 0;
}

/* A key while reading: the view moves; Backspace and Alt+Left go back. */
static void
read_key(const GR_EVENT_KEYSTROKE *k)
{
	int page = read_view_h() - READ_LINE, alt = (k->modifiers & MWKMOD_ALT) != 0;

	switch (k->ch) {
	case MWKEY_UP: read_scroll(-READ_LINE); break;
	case MWKEY_DOWN: read_scroll(READ_LINE); break;
	case MWKEY_PAGEUP: read_scroll(-page); break;
	case MWKEY_PAGEDOWN: case ' ': read_scroll((k->modifiers & MWKMOD_SHIFT) ? -page : page); break;
	case MWKEY_HOME: read_to(0); break;
	case MWKEY_END: read_to(1 << 30); break;
	case MWKEY_BACKSPACE: read_back(); break;
	case MWKEY_LEFT:
		if (alt)
			read_back();
		break;
	case MWKEY_ESCAPE:
		break;
	default:
		if (k->ch >= 32 && k->ch < 127)
			say("reading: Ctrl+R to edit the text");
	}
	status_dirty = 1;
}
