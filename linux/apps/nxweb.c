/*
 * nxweb: a browser for plain HTML. Text, headings, links, lists and rules; no style sheets and
 * no scripts. A page in the image is read as a file; any other address is asked of the host
 * (docs/fetch.md), which is the only part of this machine that has a network.
 *   nxweb [ADDRESS or FILE]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include "ui.h"

#define BAR 26
#define STATUS 18
#define MARGIN 8
#define WIDTH 600
#define HEIGHT 420
#define HOME "/usr/share/web-index.html"
#define FETCH_MOST 0x40000	/* bytes the host can hand over */
#define REG_REQUEST 64		/* words of the control block: requests made, length of the address, kind */
#define REG_REPLY 68		/* requests answered, bytes, status, a picture's width | height << 16 */
#define MAX_PICTURES 12
#define REG_ADDRESS 72		/* the address, 256 bytes */

enum { BOLD = 1, LINK = 2, FIXED = 4, HEAD = 8, RULE = 16, PICTURE = 32, DIM = 64 };

struct run { int x, y, text, length, width, link; unsigned char style; };

static GR_WINDOW_ID window;
static int width = WIDTH, height = HEIGHT;
static unsigned char var_w[256];
static int line_h;

static char *page;		/* the HTML as it came */
static int page_length;
static char address[256], typed[256], status[200], title[80];
static int editing;		/* the address field has the keyboard */
static char history[16][256];
static int history_count;

static struct run *runs;
static int run_count, run_room;
static char *pool;		/* the text of every run */
static int pool_length, pool_room;
static char *links;		/* every cur_link's address, one string after another */
static int links_length, links_room, link_count;
static int page_height, top;	/* in pixels; top is a multiple of line_h */
static int dragging;

static volatile uint32_t *regs;
static const unsigned char *fetched;
static int loading;		/* the request the host has not answered yet, or 0 */
static int waited;
static int by_hand;		/* the host may not open this address itself: asked again until it may */

/* The page's pictures. The host fetches and decodes one and hands over its pixels (docs/fetch.md);
 * they are kept as a file, which the GPU then draws from. */
enum { PICTURE_WANTED, PICTURE_HERE, PICTURE_FAILED, PICTURE_BY_HAND };
static struct picture { char address[256], file[40]; int w, h, state, placed; time_t asked; } pictures[MAX_PICTURES];
static int picture_count;
static int asking = -1;		/* the picture the request that is out is for, or -1: the page */

static int resolve(const char *href, char *whole);

static struct picture *
picture_for(const char *where)
{
	int i;

	for (i = 0; i < picture_count; i++)
		if (!strcmp(pictures[i].address, where))
			return &pictures[i];
	if (picture_count == MAX_PICTURES)
		return NULL;
	snprintf(pictures[i].address, sizeof pictures[i].address, "%s", where);
	snprintf(pictures[i].file, sizeof pictures[i].file, "/tmp/nxweb-%d-%d.ppm", (int)getpid(), i);
	pictures[i].state = PICTURE_WANTED;
	pictures[i].placed = 0;
	return &pictures[picture_count++];
}

/* ---- layout ---- */

static int lx, ly, indent, blank, in_pre, bold, heading, cur_link = -1, skip, list_depth;
static int hidden;		/* inside an element whose own style says not to show it: how deep */
static char hidden_tag[16];	/* that element's name: only its like are counted, so a stray tag cannot lose the page */

static void *
grow(void *block, int *room, int need, int size)
{
	if (need <= *room)
		return block;
	*room = need * 2 + 256;
	return realloc(block, (size_t)*room * size);
}

static int
text_width(const char *text, int n, int style)
{
	int w = 0;

	if (style & FIXED)
		return n * ui_fw;
	while (n-- > 0)
		w += var_w[(unsigned char)*text++];
	return w + ((style & (BOLD | HEAD)) ? 1 : 0);
}

static void
new_line(void)
{
	lx = indent;
	ly += line_h;
}

/* Ends the line being filled and leaves a gap above what comes next. */
static void
block(int gap)
{
	if (lx != indent)
		new_line();
	if (gap && !blank)
		ly += line_h / 2;
	blank = 1;
}

static void
add_run(const char *text, int n, int style, int w)
{
	struct run *r;

	runs = grow(runs, &run_room, run_count + 1, sizeof *runs);
	pool = grow(pool, &pool_room, pool_length + n + 2, 1);
	r = &runs[run_count++];
	r->x = lx;
	r->y = ly;
	r->text = pool_length;
	r->length = n;
	r->width = w;
	r->link = cur_link;
	r->style = style;
	memcpy(pool + pool_length, text, n);
	pool_length += n;
	blank = 0;
}

static int
style_now(void)
{
	return (bold ? BOLD : 0) | (heading ? HEAD : 0) | (cur_link >= 0 ? LINK : 0) | (in_pre ? FIXED : 0);
}

/* A word after the last one: on the same line if it fits, joined to a run of the same kind. */
static void
add_word(const char *text, int n)
{
	int style = style_now(), w = text_width(text, n, style), space = style & FIXED ? ui_fw : var_w[' '];
	struct run *last = run_count ? &runs[run_count - 1] : NULL;
	int most = width - UI_SCROLL_W - MARGIN;

	if (lx > indent && lx + space + w > most)
		new_line();
	if (lx > indent && last && last->y == ly && last->style == style && last->link == cur_link &&
	    last->text + last->length == pool_length) {
		pool = grow(pool, &pool_room, pool_length + n + 2, 1);
		pool[pool_length++] = ' ';
		memcpy(pool + pool_length, text, n);
		pool_length += n;
		last->length += n + 1;
		last->width += space + w;
		lx += space + w;
		return;
	}
	if (lx > indent)
		lx += space;
	add_run(text, n, style, w);
	lx += w;
}

static int
tag_is(const char *tag, int n, const char *name)
{
	return (int)strlen(name) == n && !strncasecmp(tag, name, n);
}

/* The value of an attribute in a tag's text, or 0. */
static int
attribute(const char *tag, int n, const char *name, char *out, int room)
{
	int m = strlen(name), i, k = 0;

	for (i = 0; i + m < n; i++) {
		char quote;

		if (strncasecmp(tag + i, name, m) || (i > 0 && !isspace((unsigned char)tag[i - 1])))
			continue;
		i += m;
		while (i < n && isspace((unsigned char)tag[i]))
			i++;
		if (i >= n || tag[i] != '=')
			continue;
		for (i++; i < n && isspace((unsigned char)tag[i]); i++)
			;
		quote = i < n && (tag[i] == '"' || tag[i] == '\'') ? tag[i++] : 0;
		while (i < n && k < room - 1 && (quote ? tag[i] != quote : !isspace((unsigned char)tag[i]))) {
			if (i + 4 < n && !strncmp(tag + i, "&amp;", 5))
				i += 4;		/* an address has its & written out */
			out[k++] = tag[i++];
		}
		out[k] = 0;
		return 1;
	}
	return 0;
}

/* One character for an entity such as &amp; or &#8217;, and how much of the text it was. */
static int
entity(const char *at, int left, int *used)
{
	static const struct { const char *name; char ch; } known[] = {
		{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
		{ "mdash", '-' }, { "ndash", '-' }, { "rsquo", '\'' }, { "lsquo", '\'' }, { "rdquo", '"' },
		{ "ldquo", '"' }, { "hellip", '.' }, { "copy", 'c' }, { "middot", '.' }, { "bull", '*' },
	};
	int i, n = 1, code;

	while (n < left && n < 10 && at[n] != ';')
		n++;
	if (n >= left || at[n] != ';')
		return 0;
	*used = n + 1;
	if (at[1] == '#') {
		code = at[2] == 'x' || at[2] == 'X' ? (int)strtol(at + 3, NULL, 16) : atoi(at + 2);
		return code >= 32 && code < 127 ? code : code == 160 ? ' ' : code == 8217 || code == 8216 ? '\'' :
		       code == 8220 || code == 8221 ? '"' : code == 8211 || code == 8212 ? '-' : '?';
	}
	for (i = 0; i < (int)(sizeof known / sizeof known[0]); i++)
		if ((int)strlen(known[i].name) == n - 1 && !strncmp(at + 1, known[i].name, n - 1))
			return known[i].ch;
	return 0;
}

/* The ASCII character to show for the UTF-8 sequence at `at`, and its length. */
static int
utf8(const unsigned char *at, int left, int *used)
{
	int n = *at >= 0xf0 ? 4 : *at >= 0xe0 ? 3 : *at >= 0xc0 ? 2 : 1, code, i;

	if (n > left)
		n = left;
	*used = n;
	code = n == 1 ? *at : *at & (0xff >> (n + 1));
	for (i = 1; i < n; i++)
		code = code << 6 | (at[i] & 0x3f);
	return code == 0xa0 ? ' ' : code == 0x2018 || code == 0x2019 ? '\'' : code == 0x201c || code == 0x201d ? '"' :
	       code == 0x2013 || code == 0x2014 ? '-' : code == 0x2022 || code == 0xb7 ? '*' : code == 0x2026 ? '.' : '?';
}

static void
open_tag(const char *tag, int n)
{
	char value[256];
	int name = 0, closing = n > 0 && tag[0] == '/';

	if (closing)
		tag++, n--;
	while (name < n && (isalnum((unsigned char)tag[name])))
		name++;
	if (hidden) {
		if (tag_is(tag, name, hidden_tag))
			hidden += closing ? -1 : 1;
		return;
	}
	if (!closing && name > 0 && name < (int)sizeof hidden_tag && n > 0 && tag[n - 1] != '/' &&
	    attribute(tag + name, n - name, "style", value, sizeof value) &&
	    (strstr(value, "display:none") || strstr(value, "display: none"))) {
		snprintf(hidden_tag, sizeof hidden_tag, "%.*s", name, tag);
		hidden = 1;
		return;
	}
	if (tag_is(tag, name, "script") || tag_is(tag, name, "style") || tag_is(tag, name, "head") ||
	    tag_is(tag, name, "svg") || tag_is(tag, name, "noscript") || tag_is(tag, name, "template")) {
		skip += closing ? (skip > 0 ? -1 : 0) : 1;
		return;
	}
	if (tag_is(tag, name, "body"))
		skip = 0;	/* a head that was never closed ends here */
	if (skip && !tag_is(tag, name, "title"))
		return;
	if (tag_is(tag, name, "br")) {
		if (lx == indent)
			ly += line_h;
		else
			new_line();
	} else if (tag_is(tag, name, "p") || tag_is(tag, name, "div") || tag_is(tag, name, "table") ||
		   tag_is(tag, name, "blockquote") || tag_is(tag, name, "section") || tag_is(tag, name, "article") ||
		   tag_is(tag, name, "header") || tag_is(tag, name, "footer") || tag_is(tag, name, "form")) {
		block(tag_is(tag, name, "div") ? 0 : 1);
	} else if (tag_is(tag, name, "tr") || tag_is(tag, name, "dt") || tag_is(tag, name, "dd")) {
		block(0);
	} else if (tag_is(tag, name, "td") || tag_is(tag, name, "th")) {
		if (!closing && lx > indent)
			lx += 2 * var_w[' '];
	} else if (name == 2 && (tag[0] == 'h' || tag[0] == 'H') && tag[1] >= '1' && tag[1] <= '6') {
		block(1);
		heading = !closing;
	} else if (tag_is(tag, name, "ul") || tag_is(tag, name, "ol")) {
		block(list_depth == 0);
		list_depth += closing ? (list_depth > 0 ? -1 : 0) : 1;
		indent = MARGIN + 18 * list_depth;
		lx = indent;
	} else if (tag_is(tag, name, "li")) {
		block(0);
		if (!closing) {
			lx = indent - 10;
			add_run("*", 1, 0, var_w['*']);
			lx = indent;
			blank = 1;
		}
	} else if (tag_is(tag, name, "hr")) {
		block(1);
		add_run("", 0, RULE, width - UI_SCROLL_W - 2 * MARGIN);
		new_line();
		block(1);
	} else if (tag_is(tag, name, "pre")) {
		block(1);
		in_pre = !closing;
	} else if (tag_is(tag, name, "b") || tag_is(tag, name, "strong")) {
		bold = !closing;
	} else if (tag_is(tag, name, "a")) {
		cur_link = -1;
		if (!closing && attribute(tag + name, n - name, "href", value, sizeof value) && value[0] != '#') {
			int m = strlen(value) + 1;

			links = grow(links, &links_room, links_length + m, 1);
			memcpy(links + links_length, value, m);
			cur_link = links_length;
			links_length += m;
			link_count++;
		}
	} else if (tag_is(tag, name, "img") && !closing) {
		char label[300], whole[256];
		int pw = 0, ph = 0, here = 0;

		if (attribute(tag + name, n - name, "src", value, sizeof value)) {
			struct picture *p;

			if (address[0] == '/' && value[0] == '/' && strlen(value) > 4 && !strcmp(value + strlen(value) - 4, ".ppm")) {
				/* a picture in the image: drawn by the GPU from the file where it lies */
				here = ui_picture_size(value, &pw, &ph);
				strcpy(whole, value);
			} else if (address[0] != '/' && resolve(value, whole) && (p = picture_for(whole)) != NULL) {
				/* one the host fetches: its pixels come to a file of ours. A page that says
				 * how large it is gets the room now, and need not be laid out again then. */
				char number[16];

				if (p->state == PICTURE_HERE) {
					here = 1;
					pw = p->w;
					ph = p->h;
				} else if (attribute(tag + name, n - name, "width", number, sizeof number) && (pw = atoi(number)) > 0 &&
					   attribute(tag + name, n - name, "height", number, sizeof number) && (ph = atoi(number)) > 0) {
					here = p->placed = 1;
				}
				strcpy(whole, p->file);
			}
		}
		if (here) {
			int most = width - UI_SCROLL_W - 2 * MARGIN;

			if (pw > most)
				ph = ph * most / pw, pw = most;
			block(0);
			add_run(whole, strlen(whole), PICTURE, pw);
			runs[run_count - 1].link = ph;
			ly += (ph + line_h - 1) / line_h * line_h;
			block(0);
			return;
		}
		if (!attribute(tag + name, n - name, "alt", value, sizeof value) || !value[0])
			strcpy(value, "picture");
		snprintf(label, sizeof label, "[%s]", value);
		add_word(label, strlen(label));
		runs[run_count - 1].style |= DIM;
	}
}

/* Lays the page out for the window's width. */
static void
layout(void)
{
	char word[200], name[sizeof title];
	int i = 0, n = 0, in_title = 0, title_n = 0;

	run_count = pool_length = links_length = link_count = 0;
	lx = indent = MARGIN;
	ly = MARGIN;
	blank = 1;
	in_pre = bold = heading = skip = list_depth = hidden = 0;
	cur_link = -1;
	title[0] = 0;
	while (i < page_length) {
		unsigned char c = page[i];
		int used = 1, ch = c;

		if (c == '<') {
			int end = i + 1, quote;

			if (n)
				add_word(word, n), n = 0;
			if (!strncmp(page + i, "<!--", 4)) {
				char *close = memmem(page + i, page_length - i, "-->", 3);

				i = close ? (int)(close - page) + 3 : page_length;
				continue;
			}
			/* to the tag's end: a > inside a quoted value is not it */
			for (quote = 0; end < page_length && (quote || page[end] != '>'); end++)
				if (page[end] == '"' || page[end] == '\'')
					quote = quote == page[end] ? 0 : quote ? quote : page[end];
			if (in_title) {
				name[title_n] = 0;
				snprintf(title, sizeof title, "%s", name);
				in_title = 0;
			} else if (!strncasecmp(page + i, "<title", 6)) {
				in_title = 1;
				title_n = 0;
			}
			open_tag(page + i + 1, end - i - 1);
			i = end + 1;
			continue;
		}
		if (c == '&')
			ch = entity(page + i, page_length - i, &used);
		else if (c >= 0x80)
			ch = utf8((unsigned char *)page + i, page_length - i, &used);
		if (!ch)
			ch = c, used = 1;
		i += used;
		if (in_title) {
			if (title_n < (int)sizeof name - 1)
				name[title_n++] = ch < 32 ? ' ' : ch;
			continue;
		}
		if (skip || hidden)
			continue;
		if (in_pre && ch == '\n') {
			if (n)
				add_word(word, n), n = 0;
			new_line();
			blank = 0;
		} else if (isspace(ch) && !(in_pre && ch == ' ' && n == 0)) {
			if (n)
				add_word(word, n), n = 0;
		} else if (isspace(ch)) {
			lx += ui_fw;	/* leading spaces of a preformatted line */
		} else {
			if (n == (int)sizeof word - 1)
				add_word(word, n), n = 0;
			word[n++] = ch < 32 || ch > 126 ? '?' : ch;
		}
	}
	if (n)
		add_word(word, n);
	page_height = ly + line_h + MARGIN;
}

/* ---- drawing ---- */

static int
view_height(void)
{
	return height - BAR - STATUS;
}

static void
draw_bar(void)
{
	static const char *names[] = { "Back", "Home", "Reload" };
	int i, x = 3 + 3 * 55;

	ui_fill(window, 0, 0, width, BAR, UI_FACE);
	for (i = 0; i < 3; i++)
		ui_button(window, 3 + i * 55, 3, 52, BAR - 6, names[i], 0);
	ui_fill(window, x, 3, width - x - 3, BAR - 6, WHITE);
	ui_bevel(window, x, 3, width - x - 3, BAR - 6, 1);
	ui_text(window, x + 5, 7, editing ? typed : address, -1, BLACK, 0);
	if (editing)
		ui_fill(window, x + 5 + text_width(typed, strlen(typed), 0), 6, 1, BAR - 12, BLACK);
}

static void
draw_status(void)
{
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_text(window, 6, height - STATUS + 3, status, -1, BLACK, 0);
}

static void
draw_page(void)
{
	int i, view = view_height();

	ui_fill(window, 0, BAR, width - UI_SCROLL_W, view, WHITE);
	for (i = 0; i < run_count; i++) {
		struct run *r = &runs[i];
		int y = BAR + r->y - top, tall = r->style & PICTURE ? r->link : line_h;
		GR_COLOR colour = r->style & LINK ? MWRGB(0, 0, 200) : r->style & DIM ? UI_SHADOW : BLACK;

		if (y + tall <= BAR || y >= BAR + view)
			continue;
		if (r->style & RULE) {
			ui_fill(window, r->x, y + line_h / 2, r->width, 1, UI_SHADOW);
		} else if (r->style & PICTURE) {
			char path[256];
			int k;

			snprintf(path, sizeof path, "%.*s", r->length, pool + r->text);
			for (k = 0; k < picture_count && strcmp(pictures[k].file, path); k++)
				;
			if (k < picture_count && pictures[k].state != PICTURE_HERE)
				ui_fill(window, r->x, y, r->width, r->link, MWRGB(232, 232, 232));	/* not here yet */
			else
				GrDrawImageFromFile(window, ui_gc, r->x, y, r->width, r->link, path, 0);
		} else {
			ui_text(window, r->x, y, pool + r->text, r->length, colour, r->style & FIXED);
			if (r->style & (BOLD | HEAD))
				ui_text(window, r->x + 1, y, pool + r->text, r->length, colour, r->style & FIXED);
			if (r->style & (LINK | HEAD))
				ui_fill(window, r->x, y + line_h - 2, r->width, 1, colour);
		}
	}
	/* what was drawn over the edges of the view */
	draw_bar();
	draw_status();
	ui_scrollbar(window, width - UI_SCROLL_W, BAR, view, page_height, view, top);
}

static void
scroll_to(int y)
{
	int most = page_height - view_height();

	y -= y % line_h;
	if (y > most)
		y = most + line_h - 1 - (most + line_h - 1) % line_h;
	if (y < 0)
		y = 0;
	if (y != top) {
		top = y;
		draw_page();
	}
}

/* ---- pages ---- */

static void
show(const char *html, int n, const char *note)
{
	int i;

	free(page);
	page = malloc(n + 1);
	memcpy(page, html, n);
	page_length = n;
	top = 0;
	/* another page: the last one's pictures go */
	for (i = 0; i < picture_count; i++)
		if (pictures[i].state == PICTURE_HERE)
			unlink(pictures[i].file);
	picture_count = 0;
	asking = -1;
	layout();
	snprintf(status, sizeof status, "%s", note);
	if (window) {
		char caption[120];

		snprintf(caption, sizeof caption, "%s - Web", title[0] ? title : address);
		GrSetWindowTitle(window, caption);
		draw_page();
	}
}

static void
say(const char *heading_text, const char *text)
{
	char html[800];
	int n = snprintf(html, sizeof html, "<h1>%s</h1><p>%s</p><p><a href=\"" HOME "\">Home</a></p>", heading_text, text);

	show(html, n, "");
}

/* Asks the host for what is at an address, a page (kind 0) or a picture (1): it sees the
 * address and the request's number, and answers when it has it. */
static void
ask(const char *what, int kind)
{
	memset((void *)&regs[REG_ADDRESS], 0, 256);
	memcpy((void *)&regs[REG_ADDRESS], what, strlen(what));
	regs[REG_REQUEST + 1] = strlen(what);
	regs[REG_REQUEST + 2] = kind;
	loading = regs[REG_REQUEST] + 1;
	if (!loading)
		loading = 1;
	regs[REG_REQUEST] = loading;
	waited = 0;
}

static void
go(const char *where, int remember)
{
	if (remember && address[0] && history_count < 16)
		strcpy(history[history_count++], address);
	else if (remember && address[0]) {
		memmove(history[0], history[1], sizeof history[0] * 15);
		strcpy(history[15], address);
	}
	snprintf(address, sizeof address, "%s", strncmp(where, "file://", 7) ? where : where + 7);
	editing = loading = by_hand = 0;
	if (address[0] == '/') {
		FILE *file = fopen(address, "rb");
		char *text;
		int n;

		if (!file) {
			say("No such page", address);
			return;
		}
		text = malloc(FETCH_MOST);
		n = fread(text, 1, FETCH_MOST, file);
		fclose(file);
		show(text, n, "from this computer");
		free(text);
		return;
	}
	if (!regs) {
		say("No network", "This machine has no way to ask its host for a page.");
		return;
	}
	asking = -1;
	ask(address, 0);
	snprintf(status, sizeof status, "asking the host for %.150s ...", address);
	if (window) {
		draw_bar();
		draw_status();
	}
}

/* Called while a request is out: shows the answer once the host has put it in place. */
static void
check_reply(void)
{
	char note[80];
	int n, code;

	if ((int)regs[REG_REPLY] != loading) {
		if (++waited > 300) {
			loading = 0;
			if (asking >= 0)
				pictures[asking].state = PICTURE_FAILED;
			else if (!by_hand)
				say("No answer", "The host did not answer in 30 seconds.");
			asking = -1;
		}
		return;
	}
	loading = 0;
	n = regs[REG_REPLY + 1];
	code = regs[REG_REPLY + 2];
	if (n > FETCH_MOST)
		n = FETCH_MOST;
	if (asking >= 0) {
		struct picture *p = &pictures[asking];
		int w = regs[REG_REPLY + 3] & 0xffff, h = regs[REG_REPLY + 3] >> 16, i;
		FILE *file;

		asking = -1;
		p->state = code == 1 ? PICTURE_BY_HAND : PICTURE_FAILED;
		p->asked = time(NULL);
		if (code == 1 && !strstr(status, "by the screen")) {
			snprintf(status, sizeof status, "pictures need their addresses pasted: the button by the screen");
			draw_status();
		}
		if (code != 200 || w <= 0 || h <= 0 || w * h * 4 > n || (file = fopen(p->file, "wb")) == NULL)
			return;
		/* a word a pixel, 0x00RRGGBB, kept as a PPM file */
		fprintf(file, "P6\n%d %d\n255\n", w, h);
		for (i = 0; i < w * h; i++) {
			uint32_t c = ((const uint32_t *)fetched)[i];

			fputc(c >> 16 & 255, file);
			fputc(c >> 8 & 255, file);
			fputc(c & 255, file);
		}
		fclose(file);
		p->w = w;
		p->h = h;
		p->state = PICTURE_HERE;
		if (!p->placed)
			layout();
		draw_page();
		return;
	}
	if (code == 1) {
		/* VRChat: a visitor has to hand the world this address. Said once; then the page is
		 * asked for again every few seconds, and comes when they have. */
		if (!by_hand)
			say("One step by hand",
			    "VRChat does not let a world open an address by itself, only one a visitor gives it. "
			    "This link's address is now in the field marked <b>Link to copy</b> on the panel in the room. "
			    "Copy it from there and paste it into the field marked <b>Paste here</b> beside it. "
			    "The page then opens by itself.");
		by_hand = 1;
		return;
	}
	by_hand = 0;
	if (code != 200) {
		char text[480];

		snprintf(text, sizeof text, "The host could not get %.180s (%d).%s", address, code,
			 code == 3 ? " VRChat opens only addresses that begin with https." : "");
		say("Page not loaded", text);
		return;
	}
	snprintf(note, sizeof note, "%d bytes from the host%s", n, n == FETCH_MOST ? " (cut short)" : "");
	show((const char *)fetched, n, note);
}

/* An address a page gives (256 bytes of room), made whole with the page's own. 0 for what is
 * not an address to fetch: mailto: and the like, and pictures written into the page itself. */
static int
resolve(const char *href, char *whole)
{
	char base[256];
	char *slash, *host;

	if (!strncmp(href, "data:", 5) || !href[0])
		return 0;
	if (strstr(href, "://") || !strncmp(href, "world:", 6)) {
		snprintf(whole, 256, "%s", href);
	} else if (strchr(href, ':') && !strchr(href, '/')) {
		return 0;
	} else {
		snprintf(base, sizeof base, "%s", address);
		host = strstr(base, "://");
		if (!strncmp(href, "//", 2) && host) {
			host[1] = 0;
			snprintf(whole, 256, "%s%s", base, href);
		} else if (href[0] == '/') {
			/* from the site's root, or from this computer's for a page that is a file */
			if (host && (slash = strchr(host + 3, '/')) != NULL)
				*slash = 0;
			snprintf(whole, 256, "%s%s", host ? base : "", href);
		} else {
			slash = strrchr(base, '/');
			if (slash && (!host || slash > host + 2))
				slash[1] = 0;
			else
				strcat(base, "/");
			snprintf(whole, 256, "%s%s", base, href);
		}
	}
	return 1;
}

static void
follow(const char *href)
{
	char whole[256];

	if (resolve(href, whole))
		go(whole, 1);
}

/* The picture to ask the host for next, or -1: the first three it does not have yet, in the
 * page's order (the host has room for three), one that waits for a visitor once a second. */
static int
next_picture(void)
{
	time_t now = time(NULL);
	int i, open = 0;

	for (i = 0; i < picture_count && open < 3; i++) {
		if (pictures[i].state != PICTURE_WANTED && pictures[i].state != PICTURE_BY_HAND)
			continue;
		open++;
		if (pictures[i].state == PICTURE_WANTED || pictures[i].asked != now)
			return i;
	}
	return -1;
}

static int
pictures_open(void)
{
	int i, open = 0;

	for (i = 0; i < picture_count; i++)
		open += pictures[i].state == PICTURE_WANTED || pictures[i].state == PICTURE_BY_HAND;
	return open;
}

static void
click(int x, int y)
{
	int i;

	if (y < BAR) {
		if (x < 3 + 3 * 55) {
			editing = 0;
			if (x < 55 && history_count)
				go(history[--history_count], 0);
			else if (x >= 55 && x < 110)
				go(HOME, 1);
			else if (x >= 110)
				go(address, 0);
			return;
		}
		editing = 1;
		strcpy(typed, address);
		draw_bar();
		return;
	}
	if (editing) {
		editing = 0;
		draw_bar();
	}
	if (x >= width - UI_SCROLL_W) {
		dragging = 1;
		scroll_to(ui_scroll_to(y, BAR, view_height(), page_height, view_height()));
		return;
	}
	for (i = 0; i < run_count; i++) {
		struct run *r = &runs[i];

		if ((r->style & LINK) && ui_inside(x, y - BAR + top, r->x, r->y, r->width, line_h)) {
			char href[256];

			snprintf(href, sizeof href, "%s", links + r->link);
			follow(href);
			return;
		}
	}
}

static void
key(int ch)
{
	int n = strlen(typed), view = view_height();

	if (!editing) {
		switch (ch) {
		case MWKEY_UP: scroll_to(top - line_h); break;
		case MWKEY_DOWN: scroll_to(top + line_h); break;
		case MWKEY_PAGEUP: scroll_to(top - view + line_h); break;
		case MWKEY_PAGEDOWN: case ' ': scroll_to(top + view - line_h); break;
		case MWKEY_HOME: scroll_to(0); break;
		case MWKEY_END: scroll_to(page_height); break;
		case MWKEY_BACKSPACE: click(10, 10); break;
		}
		return;
	}
	if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
		char where[280];

		/* a bare name is a site */
		snprintf(where, sizeof where, "%s%s", typed[0] == '/' || strstr(typed, ":") ? "" : "https://", typed);
		where[255] = 0;
		go(where, 1);
		return;
	}
	if (ch == MWKEY_ESCAPE)
		editing = 0;
	else if ((ch == MWKEY_BACKSPACE || ch == 127) && n > 0)
		typed[n - 1] = 0;
	else if (ch >= 32 && ch < 127 && n < 250)
		typed[n] = ch, typed[n + 1] = 0;
	draw_bar();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	GR_FONT_INFO info;
	int fd;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	GrGetFontInfo(ui_var, &info);
	memcpy(var_w, info.widths, sizeof var_w);
	line_h = (info.height > ui_fh ? info.height : ui_fh) + 2;
	fd = open("/dev/gpu", O_RDWR);
	if (fd >= 0) {
		void *words = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
		void *data = mmap(NULL, FETCH_MOST, PROT_READ, MAP_SHARED, fd, 0x016c0000);

		if (words != MAP_FAILED && data != MAP_FAILED) {
			regs = words;
			fetched = data;
		}
	}
	go(argc > 1 ? argv[1] : HOME, 0);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Web", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, WHITE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP |
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEventTimeout(&event, loading ? 100 : by_hand ? 3000 : regs && pictures_open() ? 400 : 60000);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_page();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && (event.update.width != width || event.update.height != height)) {
				width = event.update.width;
				height = event.update.height;
				layout();
				scroll_to(top);
				draw_page();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event))
				scroll_to(top + 3 * line_h * ui_wheel_sum(&event));
			else
				click(event.button.x, event.button.y);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L))
				scroll_to(ui_scroll_to(event.mouse.y, BAR, view_height(), page_height, view_height()));
			else
				dragging = 0;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
		if (loading)
			check_reply();
		else if (by_hand && event.type == GR_EVENT_TYPE_TIMEOUT)
			ask(address, 0);
		else if (!by_hand && regs && (asking = next_picture()) >= 0)
			ask(pictures[asking].address, 1);
	}
}
