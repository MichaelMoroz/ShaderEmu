/*
 * nxweb: a browser (docs/apps/web.md). The page is HTML laid out by web.h and web_layout.h with
 * the sheets css.h reads, in the fonts of the ROM; no scripts. It is drawn once into a pixmap
 * the GPU keeps, and scrolling copies from that. A page in the image is read as a file; any
 * http address the machine can reach (its LAN, itself) is fetched over its own sockets (web_net.h, a
 * step at a time: the window never waits for one), and any other is asked of the host
 * (docs/system/fetch.md), unless the image keeps a copy (web/preload.py).
 *   nxweb [ADDRESS or FILE]
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include "ui.h"
#include "registry.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_glyph.h"
#include "ui_launch.h"
#include "web_layout.h"
#include "web_net.h"
#include "mcw.h"
#include "media.h"

#define MENU_H 20
#define BAR 26
#define TOP (MENU_H + BAR)	/* the window's row where the page begins */
#define STATUS 18
#define STATUS_CELL 96		/* the status line's right part: what is to come, or the page's size */
#define WIDTH 600
#define HEIGHT 440
#define TOOLS 5
#define TOOL_W 26
#define FIELD_X (3 + TOOLS * (TOOL_W + 3))
#define HOME "/usr/share/web-index.html"
#define HISTORY 16
#define BOOKMARKS 24		/* a menu of them fits the screen, and the registry stays small */
#define FETCH_MOST 0x40000	/* bytes the host hands over at a time */
#define PAGE_MOST (3 << 19)	/* of a page that are read */
#define SHEET_MOST (3 << 18)
#define REG_REQUEST 64		/* words of the control block: requests made, length of the address, kind, offset */
#define REG_REPLY 68		/* requests answered, bytes, status, a picture's width | height << 16 */
#define REG_ADDRESS 72		/* the address, 256 bytes */
#define REG_PIXMAPS (0x1800 / 4 + 28)	/* pixmaps the GPU keeps, counted by the screen's driver */
#define REG_HOST 15		/* bits 8-15, 16-23: the GPU's target as this host has it, in 16s of pixels (0: not said) */
#define MAX_PICTURES 160
#define TILE_BYTES (3200 * 1024)	/* of the GPU's memory for the page as drawn */

static GR_WINDOW_ID window;
static int width = WIDTH, height = HEIGHT;
static int line_h;

static char *page;		/* the HTML as it came */
static int page_length;
static char address[256], typed[256], status[200];
static int editing;		/* the address field has the keyboard */
static int offered;		/* its text is still the one it opened with: a typed character replaces it */
static int picked = -1;		/* the link the keyboard is on (Tab): its element, or -1 */
static struct { char address[256], title[60]; int top; char *post; int post_n; char post_type[80]; } history[HISTORY];	/* post: the form it came of */
static int history_count, history_at;	/* the pages of this session, and which of them is shown */
static struct bookmark { char address[256], title[60]; } bookmarks[BOOKMARKS];
static int bookmark_count;
static char home[256] = HOME;
static int load_pictures = 1;
static char sought[64];		/* what Find looks for */
static int finding;		/* it is being typed, in the status line */
static int found = -1, found_from;	/* the text it was found in (an item), and from which character */
static int menu_at = -1;	/* the menu of the bar that is open, or -1 */
static int shown_busy, shown_done, shown_total, shown_failed;	/* what is to come as last shown (loading_shown) */
static char shown_text[200], shown_cell[24];	/* and what the status line says of it */
static char page_note[200];	/* the status line as the page was shown: replaced by what is loading */
static int print;		/* NXWEB_PRINT: says what a key, a click or a menu's row left */
static char link_asked[256];	/* the link the right button's menu is about */
static int top, goal;		/* the page's row at the view's top, and where it is scrolling to */
static int dragging;
static int sent_on;		/* pages in a row that sent their reader on to another */
static int verbose, dump;
static int check_due;		/* NXWEB_CHECK: the page's strips are checked once it is shown */
static unsigned page_began;	/* (NXWEB_TIME) the first core's count when the page's text was here */
static struct timespec page_began_at;	/* and the machine's clock */
static int page_shown;
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ)

static volatile uint32_t *regs;
static const unsigned char *fetched;
static int loading;		/* the request the host has not answered yet, or 0 */
static int waited, ticked;	/* ticked: the event just had was the wait running out */
static int by_hand;		/* the host may not open this address itself: asked again until it may */
static char *part;		/* a page or a sheet that comes in parts: what is here of it */
static int part_have;

/* The page's pictures. A PPM file the image keeps is drawn where it lies in the ROM. One kept as
 * a JPEG or PNG, and one the host fetches (its pixels arrive and are written as a PPM file, docs/
 * fetch.md), is decoded on the worker cores to the size it is drawn at, into the GPU's memory. */
/* FAILED: a box that says why (`why`); SKIPPED: not fetched by choice (a spacer), not shown. */
enum { PICTURE_WANTED, PICTURE_HERE, PICTURE_FAILED, PICTURE_BY_HAND, PICTURE_SKIPPED };
static struct picture {
	char address[256];
	char why[64];		/* why it is not here, when FAILED */
	int tracked;		/* it was fetched or decoded for this page: counted by the loading bar */
	int boxed;		/* FAILED, and laid out as such */
	char file[64];		/* the PPM the window system draws at any size ("": none), ours to remove if `ours` */
	char source[64];	/* the file decoded to the size drawn ("": none) */
	char kept[64];		/* the decoded pixels as a PPM, when the window system draws them (`kept_there`) */
	int w, h, state, sized, ours, kept_there;
	int dw, dh;		/* the size it is decoded at (or a PPM's of the ROM); 0: not yet */
	int later, later_w, later_h;	/* its decoding is waited for (the order asked in), at that size */
	time_t asked;
	uint32_t rom;		/* where those pixels lie, ROM or GPU memory, for a strip's list (0: not looked for, 1: not there) */
	uint8_t *pixels;	/* in the GPU's memory, 3 bytes a pixel (fragment mode 4's), or NULL */
} *pictures;
static int picture_count;
static int asking = -1;		/* the picture the request that is out is for, or -1: the page */
static FILE *part_file;		/* a picture that comes in parts: its file, its size, pixels so far */
static int part_w, part_h, part_done;
static unsigned pictures_decoding;	/* (NXWEB_TIME) the first core's instructions decoding the page's pictures */
static int pictures_now;	/* a picture is decoded when asked for, not after the view is shown (NXWEB_CHECK) */
static int pictures_later;	/* decodings asked for so far: the next `later` */
#define SHEET 1000		/* `asking` for sheet 0 */
static char sheet_why[WEB_SHEETS][64];	/* why a sheet is FAILED */
static int sheet_tracked[WEB_SHEETS];	/* it was asked of the host */
static int failed_due;		/* a picture failed: its box drawn, or the page laid out again (failures_show) */
static int failed_told;		/* failures the status line has said */
static int part_total;		/* bytes of the page that comes in parts */
static int spin;		/* the loading bar's step while nothing can be counted */

/* A request over this machine's own sockets (web_net.h): `loading` is -1 while it is out. */
static struct net_fetch net;
static int net_watched = -1;	/* the socket the window system's wait also looks at */
static int net_kind;		/* 0 a page, 1 a picture, 2 a sheet */
static int net_sink;		/* the body goes to `net_file`: 1 the picture a page is, 2 saved */
static int net_hops;		/* redirects in a row */
static int net_shown;		/* bytes of the page already shown while it came, or 0 */
static int net_posting;		/* the page's request sends the form its history entry keeps */
static char net_file[128];
static char *post_next;		/* the form go() is about to send */
static int post_next_n;
static char post_next_type[80];
static int post_again;		/* go(): the visitor said to send the form again */
static int top_wanted = -1;	/* where the next page shown is to be scrolled to */
static int page_ours;		/* the page is the browser's own words: it may link to this computer */
static char *raw, *raw_next;	/* what came, when the page shown is that as HTML (text/plain): saved and shown as source */
static int raw_n, raw_next_n;

/* The page as drawn, in a pixmap `tile_h` rows high: strips of it (tile_make). */
static GR_WINDOW_ID tile;
static int tile_w, tile_h, tile_gpu;
static GR_FONT_ID fonts[WEB_FACES][2];

/* What a visitor has typed into the page's fields. */
static struct field { int node, n, room, checked, option; unsigned short *text; } *fields;
static int field_count, focus = -1;

static void show_view(void);
static const char *picture_file(struct picture *p, int w, int h);
static void strips_wait(void);
static int strips_open(void);
static void strips_forget_rows(int y0, int y1);
static GR_WINDOW_ID gpu_pixmap(int w, int h);
static uint32_t pixmap_address(GR_WINDOW_ID p, int w);
static void say_time(const char *what, unsigned began);
static int wanted_open(void);
static int bookmark_of(const char *where);
static void others(GR_EVENT *event);

/* Instructions the first core has run (the low word of its count): what a step cost. */
static unsigned
now(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static int
view_height(void)
{
	return height - TOP - STATUS;
}

static int
view_width(void)
{
	return width - UI_SCROLL_W;
}

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

/* What the host's status for an address says, in a few words (docs/system/fetch.md). */
static void
why_of(char *why, int room, int code)
{
	if (code == 404 || code == 410)
		snprintf(why, room, "not found (%d)", code);
	else if (code == 401 || code == 403)
		snprintf(why, room, "refused (%d)", code);
	else if (code == 415)
		snprintf(why, room, "unsupported format");
	else if (code == 3)
		snprintf(why, room, "an address the host may not open");
	else if (code == 0 || code == 2)
		snprintf(why, room, "no answer");
	else if (code >= 500)
		snprintf(why, room, "the server's error (%d)", code);
	else
		snprintf(why, room, "the host said %d", code);
}

static void
picture_fail(struct picture *p, const char *why)
{
	p->state = PICTURE_FAILED;
	p->later = 0;
	snprintf(p->why, sizeof p->why, "%.63s", why);
	failed_due = 1;
}

static void
sheet_fail(int i, const char *why)
{
	web_sheets[i].state = SHEET_FAILED;
	snprintf(sheet_why[i], sizeof sheet_why[0], "%.63s", why);
}

/* A picture of the image that is not a PPM: decoded as the cache's are, its size from its header. */
static void
picture_local(struct picture *p, const char *where)
{
	struct media_info info;
	struct stat st;
	const char *why = "not found";
	void *data = MAP_FAILED;
	int fd = strlen(where) < 60 ? open(where, O_RDONLY) : -1;
	size_t size = 0;

	if (fd >= 0 && !fstat(fd, &st) && st.st_size > 0)
		data = mmap(NULL, size = (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (fd >= 0)
		close(fd);
	if (data == MAP_FAILED)
		data = NULL;
	if (strlen(where) >= 60)
		why = "an address too long";
	else if (fd >= 0 && !data)
		why = "cannot be read";
	else if (data && !(why = media_probe(data, (uint32_t)size, &info)) && info.width > 0 && info.height > 0) {
		snprintf(p->source, sizeof p->source, "%s", where);
		p->file[0] = 0;
		p->w = info.width, p->h = info.height;
		p->state = PICTURE_HERE;
	}
	if (data)
		munmap(data, size);
	if (p->state != PICTURE_HERE)
		picture_fail(p, data ? "unsupported format" : why);	/* (what libmedia does not read) */
}

/* A picture of a file: a PPM as the window system draws it, any other as libmedia decodes it. */
static void
picture_from(struct picture *p, const char *file)
{
	snprintf(p->file, sizeof p->file, "%.60s", file);
	p->state = PICTURE_WANTED;
	if (ui_picture_size(file, &p->w, &p->h)) {
		memcpy(p->source, p->file, sizeof p->source);
		p->state = PICTURE_HERE;
	} else
		picture_local(p, file);
}

/* What came over this machine's sockets of the pictures and sheets, in /tmp for this session:
 * another page of the site has the same ones. Reload asks again; "no-store" is never found. */
#define CACHED 48
#define CACHED_BYTES (8 << 20)
static struct cached { char address[256], file[40]; int size, gone; } *cached;
static int cached_count, cached_serial;

/* (the browser closes) */
static void
cached_remove(void)
{
	while (cached_count)
		unlink(cached[--cached_count].file);
}

static const char *
cached_file(const char *where)
{
	int i;

	for (i = 0; i < cached_count; i++)
		if (!cached[i].gone && !strcmp(cached[i].address, where))
			return cached[i].file;
	return NULL;
}

static void
cached_name(char *name, int room)
{
	snprintf(name, room, "/tmp/nxweb-%d-c%d", (int)getpid(), cached_serial++);
}

static void
cached_add(const char *where, const char *file, int size, int gone)
{
	int i, all = size;

	if (!cached && (cached = malloc(CACHED * sizeof *cached)) == NULL)
		return;
	for (i = 0; i < cached_count; i++)
		all += cached[i].size;
	while (cached_count && (cached_count == CACHED || all > CACHED_BYTES)) {
		unlink(cached[0].file);
		all -= cached[0].size;
		memmove(&cached[0], &cached[1], sizeof *cached * --cached_count);
	}
	for (i = 0; i < cached_count; i++)
		if (!strcmp(cached[i].address, where))
			cached[i].gone = 1;
	snprintf(cached[cached_count].address, sizeof cached[0].address, "%s", where);
	snprintf(cached[cached_count].file, sizeof cached[0].file, "%s", file);
	cached[cached_count].size = size;
	cached[cached_count++].gone = gone;
}

/* What a picture that is not here says in its box: its description and why, on one line. */
static void
broken_label(int node, const struct picture *p, char *label, int room)
{
	char alt[160];

	if (web_attr_text(node, "alt", alt, sizeof alt) && alt[0])
		snprintf(label, room, "%s: %s", alt, p->why);
	else
		snprintf(label, room, "Picture not loaded: %s", p->why);
}

static int
web_picture(int node, const char *where, int *w, int *h)
{
	struct picture *p;
	const char *here;
	int i, said_w = web_attr_int(node, "width", -1), said_h = web_attr_int(node, "height", -1);

	if (!load_pictures)
		return -1;	/* (the options: the page says what each would have been) */
	for (i = 0; i < picture_count && strcmp(pictures[i].address, where); i++)
		;
	if (i == picture_count) {
		if (picture_count == MAX_PICTURES)
			return -1;
		p = &pictures[picture_count++];
		memset(p, 0, sizeof *p);
		snprintf(p->address, sizeof p->address, "%s", where);
		snprintf(p->kept, sizeof p->kept, "/tmp/nxweb-%d-%d-drawn.ppm", (int)getpid(), i);
		if (where[0] == '/') {
			/* a picture in the image: a PPM drawn by the GPU from the file where it lies */
			snprintf(p->file, sizeof p->file, "%.60s", where);
			p->state = PICTURE_WANTED;
			if (strlen(where) < 60 && ui_picture_size(where, &p->w, &p->h))
				p->state = PICTURE_HERE;
			else
				picture_local(p, where);
		} else if ((here = cached_file(where)) != NULL) {
			picture_from(p, here);
		} else if (web_cache_find(where, 'I', p->file, sizeof p->file, &p->w, &p->h)) {
			const char *dot = strrchr(p->file, '.');

			p->state = PICTURE_HERE;
			if (dot && strcmp(dot, ".ppm")) {
				/* (a JPEG or PNG: decoded when it is first drawn) */
				memcpy(p->source, p->file, sizeof p->source);
				p->file[0] = 0;
			}
		} else {
			snprintf(p->file, sizeof p->file, "/tmp/nxweb-%d-%d.ppm", (int)getpid(), i);
			p->ours = 1;
			/* (one the page says is a dot or nothing is a spacer or counts visitors: not fetched) */
			p->state = (said_w >= 0 && said_w <= 2) || (said_h >= 0 && said_h <= 2) ? PICTURE_SKIPPED : PICTURE_WANTED;
			p->tracked = p->state == PICTURE_WANTED;
			if (!regs && p->state == PICTURE_WANTED)
				picture_fail(p, "no network");
		}
	}
	p = &pictures[i];
	if (p->state == PICTURE_SKIPPED)
		return -1;
	if (p->state == PICTURE_FAILED) {
		/* a box that says why, the page's size for it or one for its line */
		char label[240];

		broken_label(node, p, label, sizeof label);
		web_broken_size(label, w, h);
		p->boxed = 1;
		return i;
	}
	*w = p->state == PICTURE_HERE ? p->w : 0;
	*h = p->state == PICTURE_HERE ? p->h : 0;
	if (p->state != PICTURE_HERE && (said_w < 0 || said_h < 0))
		p->sized = 0;	/* (laid out before it came, the page not saying its size: laid out again when it does) */
	return i;
}

/* ---- fields ---- */

/* A character more of a field's text. */
static void
field_put(struct field *f, int c)
{
	if (f->n == f->room) {
		int room = f->room ? f->room * 2 : 64;
		unsigned short *more = room <= 32768 ? realloc(f->text, room * sizeof *more) : NULL;

		if (!more)
			return;
		f->text = more;
		f->room = room;
	}
	f->text[f->n++] = c;
}

static struct field *
field_of(int node)
{
	char value[300];
	struct field *f;
	int i, used, left;
	const char *v;

	for (i = 0; i < field_count; i++)
		if (fields[i].node == node)
			return &fields[i];
	fields = realloc(fields, (field_count + 1) * sizeof *fields);
	f = &fields[field_count++];
	memset(f, 0, sizeof *f);
	f->node = node;
	f->checked = web_attr(node, "checked", &v, &i);
	if (web_nodes[node].c.tag == T_input && web_attr_text(node, "value", value, sizeof value))
		for (v = value, left = strlen(value); left > 0; v += used, left -= used)
			field_put(f, web_char((const unsigned char *)v, left, &used));
	if (web_nodes[node].c.tag == T_textarea)
		for (i = web_nodes[node].first; i >= 0; i = web_nodes[i].next)
			if (web_nodes[i].c.tag == T_TEXT)
				for (used = 0; used < web_nodes[i].text_n; used++)
					field_put(f, web_u[web_nodes[i].text + used]);
	if (web_nodes[node].c.tag == T_select) {
		int kid, k = 0;

		for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next)
			if (web_nodes[kid].c.tag == T_option) {
				if (web_attr(kid, "selected", &v, &i))
					f->option = k;
				k++;
			}
	}
	return f;
}

static const char *
field_type(int node)
{
	static char type[24];

	strcpy(type, "text");
	if (web_nodes[node].c.tag == T_input)
		web_attr_text(node, "type", type, sizeof type);
	else
		strcpy(type, web_nodes[node].c.tag == T_select ? "select" : "textarea");
	return type;
}

static int
field_is_button(const char *type)
{
	/* (<input type=file> is not one: a click chooses a file) */
	return !strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset") || !strcasecmp(type, "image");
}

/* ---- drawing ---- */

static GR_COLOR
mw(int rgb)
{
	return MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
}

/* Characters in one of the ROM's faces, their top left corner at x, y. */
static void
page_text(GR_DRAW_ID to, int x, int y, const unsigned short *text, int n, int face, int italic, int colour)
{
	GR_FONT_ID *font = &fonts[face][italic];

	if (web_plain) {
		/* the window system's own font: Latin letters, bold by drawing twice */
		char plain[256];
		int i, bold = face < FACE_UNI && (face / WEB_SIZES) % 2;

		for (; n > 0; n -= i, text += i, x += ui_width(&ui_var_widths, ui_var, plain, i)) {
			for (i = 0; i < n && i < 255; i++)
				plain[i] = text[i] >= 32 && text[i] < 127 ? text[i] : text[i] < 32 ? ' ' : '?';
			ui_text(to, x, y, plain, i, mw(colour), 0);
			if (bold)
				ui_text(to, x + 1, y, plain, i, mw(colour), 0);
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
	GrSetGCForeground(ui_gc, mw(colour));
	GrText(to, ui_gc, x, y, (void *)text, n, GR_TFUC16 | GR_TFTOP);
}

static int
plain_face(void)
{
	int face = web_face_for(0, 0, CSS_EM);

	web_face(face);
	return face;
}

static void
draw_field(GR_DRAW_ID to, const struct web_item *it, int y)
{
	struct field *f = field_of(it->node);
	const char *type = field_type(it->node);
	unsigned short label[128];
	char text[200];
	int face = plain_face(), n = 0, w = 0, i, grey = 0, used, left;
	const char *v;

	if (web_nodes[it->node].c.tag == T_textarea) {
		/* its lines as they wrap: the first ones, or the last ones while it is typed into */
		int rows = (it->h - 6) / web_faces[face].height, room = it->w - 8, starts[64], lines = 0, at = 0, first;

		ui_fill(to, it->x, y, it->w, it->h, WHITE);
		GrSetGCForeground(ui_gc, focus == it->node ? BLACK : UI_SHADOW);
		GrRect(to, ui_gc, it->x, y, it->w, it->h);
		rows = rows < 1 ? 1 : rows > 32 ? 32 : rows;
		starts[lines++ % 64] = 0;
		for (i = 0; i < f->n; i++) {
			int adv = f->text[i] == '\n' ? 0 : web_most(0, web_advance(face, f->text[i]));

			if (f->text[i] == '\n' || w + adv > room) {
				starts[lines++ % 64] = i + (f->text[i] == '\n');
				w = 0;
				if (focus != it->node && lines > rows)
					break;
			}
			w += adv;
		}
		first = focus == it->node && lines > rows ? lines - rows : 0;
		if (lines - first > 64)
			first = lines - 64;
		for (at = 0; at < rows && first + at < lines; at++) {
			int from = starts[(first + at) % 64], end = first + at + 1 < lines ? starts[(first + at + 1) % 64] : f->n;

			while (end > from && f->text[end - 1] == '\n')
				end--;
			if (end > from)
				page_text(to, it->x + 4, y + 3 + at * web_faces[face].height, f->text + from, end - from, face, 0, 0);
		}
		if (focus == it->node)
			ui_fill(to, it->x + 5 + (f->n && f->text[f->n - 1] != '\n' ? w : 0), y + 3 + (lines - first - 1) * web_faces[face].height, 1, web_faces[face].height, BLACK);
		return;
	}
	if (field_is_button(type)) {
		ui_fill(to, it->x, y, it->w, it->h, UI_FACE);
		ui_bevel(to, it->x, y, it->w, it->h, 0);
		if (!web_attr_text(it->node, "value", text, sizeof text) && !web_attr_text(it->node, "alt", text, sizeof text))
			strcpy(text, !strcasecmp(type, "reset") ? "Reset" : "Submit");
		for (v = text, left = strlen(text); left > 0 && n < 127; v += used, left -= used)
			label[n++] = web_char((const unsigned char *)v, left, &used);
		for (i = 0; i < n; i++)
			w += web_most(0, web_advance(face, label[i]));
		page_text(to, it->x + (it->w - w) / 2, y + 3, label, n, face, 0, 0);
		return;
	}
	ui_fill(to, it->x, y, it->w, it->h, WHITE);
	GrSetGCForeground(ui_gc, focus == it->node ? BLACK : UI_SHADOW);
	GrRect(to, ui_gc, it->x, y, it->w, it->h);
	if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) {
		if (f->checked)
			ui_fill(to, it->x + 3, y + 3, it->w - 6, it->h - 6, BLACK);
		return;
	}
	if (!strcasecmp(type, "select")) {
		int kid, k = 0;

		for (kid = web_nodes[it->node].first; kid >= 0; kid = web_nodes[kid].next)
			if (web_nodes[kid].c.tag == T_option && k++ == f->option && web_nodes[kid].first >= 0 &&
			    web_nodes[web_nodes[kid].first].c.tag == T_TEXT) {
				const struct web_node *t = &web_nodes[web_nodes[kid].first];

				n = web_least(t->text_n, 127);
				memcpy(label, web_u + t->text, n * sizeof *label);
			}
		ui_fill(to, it->x + it->w - 16, y + 1, 15, it->h - 2, UI_FACE);
		label[127] = 'v';
		page_text(to, it->x + it->w - 12, y + 3, label + 127, 1, face, 0, 0);
	} else {
		/* (a field's last 127 characters: as many as could be seen of it and more) */
		n = web_least(f->n, 127);
		if (n)
			memcpy(label, f->text + (focus == it->node ? f->n - n : 0), n * sizeof *label);
		if (!n && web_attr_text(it->node, "placeholder", text, sizeof text)) {
			for (v = text, left = strlen(text); left > 0 && n < 127; v += used, left -= used)
				label[n++] = web_char((const unsigned char *)v, left, &used);
			grey = 1;
		}
		if (!strcasecmp(type, "password"))
			for (i = 0; i < n && !grey; i++)
				label[i] = '*';
	}
	/* as much of the text as the field holds: its end, while it is typed into */
	for (i = n, w = 0; i > 0 && w + web_most(0, web_advance(face, label[i - 1])) < it->w - 8 - (it->h > 30 ? 0 : 0); i--)
		w += web_most(0, web_advance(face, label[i - 1]));
	if (focus != it->node || grey) {
		for (i = 0, w = 0; i < n && w + web_most(0, web_advance(face, label[i])) < it->w - 8; i++)
			w += web_most(0, web_advance(face, label[i]));
		n = i;
		i = 0;
	}
	if (n > i)
		page_text(to, it->x + 4, y + 3, label + i, n - i, face, 0, grey ? 0x808080 : 0);
	if (focus == it->node)
		ui_fill(to, it->x + 4 + (grey ? 0 : w) + 1, y + 3, 1, it->h - 6, BLACK);
}

/* How wide `n` characters of a text are, from its character `from`. */
static int
item_width(const struct web_item *it, int from, int n)
{
	int i, w = 0;

	for (i = 0; i < n; i++)
		w += web_most(0, web_advance(it->face, web_u[it->a + from + i]));
	return w;
}

/* As much of `text` (UTF-8) as `room` pixels hold, in the plain face, its top left at x, y. */
static void
fit_text(GR_DRAW_ID to, int x, int y, int room, const char *text, int colour)
{
	const unsigned char *at = (const unsigned char *)text;
	unsigned short u[160];
	int face = plain_face(), n = 0, w = 0, used, left = (int)strlen(text), a;

	for (; left > 0 && n < 160; at += used, left -= used, n++) {
		u[n] = web_char(at, left, &used);
		if ((a = web_most(0, web_advance(face, u[n]))) + w > room)
			break;
		w += a;
	}
	page_text(to, x, y, u, n, face, 0, colour);
}

/* A picture that could not be had: a grey box, a sign, its description and why, as they fit. */
static void
draw_broken(GR_DRAW_ID to, const struct web_item *it, int y)
{
	const struct picture *p = &pictures[it->a];
	char alt[160], line[240];
	int lh = web_faces[plain_face()].height, x = it->x, room = it->w - WEB_BROKEN_TEXT - 4;

	ui_fill(to, x, y, it->w, it->h, mw(0xf0f0f0));
	GrSetGCForeground(ui_gc, mw(0xa0a0a0));
	GrRect(to, ui_gc, x, y, it->w, it->h);
	if (it->w < 18 || it->h < 18)
		return;
	/* the sign: a picture crossed out in red */
	ui_fill(to, x + 4, y + 4, 12, 12, WHITE);
	GrSetGCForeground(ui_gc, mw(0x808080));
	GrRect(to, ui_gc, x + 4, y + 4, 12, 12);
	GrSetGCForeground(ui_gc, mw(0xd02020));
	GrLine(to, ui_gc, x + 6, y + 6, x + 13, y + 13);
	GrLine(to, ui_gc, x + 13, y + 6, x + 6, y + 13);
	if (room < 8)
		return;
	if (it->h >= 2 * lh + 8) {
		if (!web_attr_text(it->node, "alt", alt, sizeof alt) || !alt[0])
			strcpy(alt, "Picture not loaded");
		fit_text(to, x + WEB_BROKEN_TEXT, y + 3, room, alt, 0x303030);
		fit_text(to, x + WEB_BROKEN_TEXT, y + 3 + lh, room, p->why, 0xa00000);
	} else {
		broken_label(it->node, p, line, sizeof line);
		fit_text(to, x + WEB_BROKEN_TEXT, y + (it->h - lh) / 2, room, line, 0x802020);
	}
}

/* One thing of the page, `dy` above where the page has it. */
static void
draw_item(GR_DRAW_ID to, const struct web_item *it, int dy)
{
	const char *file;
	int y = it->y - dy;

	switch (it->kind) {
	case I_BOX:
		if (it->colour >= 0 && it->w > 0 && it->h > 0)
			ui_fill(to, it->x, y, it->w, it->h, mw(it->colour));
		if (it->flags && it->w > 0) {
			GrSetGCForeground(ui_gc, mw(it->b));
			GrRect(to, ui_gc, it->x, y, it->w, it->h);
		}
		break;
	case I_TEXT:
		page_text(to, it->x, y, web_u + it->a, it->b, it->face, (it->flags & IF_ITALIC) != 0, it->colour);
		if (it->flags & IF_UNDER)
			ui_fill(to, it->x, y + web_faces[it->face].ascent + 1, it->w, 1, mw(it->colour));
		if (it->flags & IF_THROUGH)
			ui_fill(to, it->x, y + web_faces[it->face].ascent * 2 / 3, it->w, 1, mw(it->colour));
		if (found >= 0 && it == &web_items[found]) {
			/* what Find found: those characters again, white on the selection's colour */
			int n = web_least((int)strlen(sought), it->b - found_from), x = it->x + item_width(it, 0, found_from);

			ui_fill(to, x, y, item_width(it, found_from, n), it->h, UI_SELECTED);
			page_text(to, x, y, web_u + it->a + found_from, n, it->face, (it->flags & IF_ITALIC) != 0, 0xffffff);
		}
		if (picked >= 0 && web_link_of(it->node) == picked) {
			GrSetGCForeground(ui_gc, mw(it->colour));
			GrRect(to, ui_gc, it->x - 2, y - 1, it->w + 4, it->h + 2);
		}
		break;
	case I_IMAGE:
		if (pictures[it->a].state == PICTURE_FAILED)
			draw_broken(to, it, y);
		else if (pictures[it->a].state != PICTURE_HERE || !(file = picture_file(&pictures[it->a], it->w, it->h)))
			ui_fill(to, it->x, y, it->w, it->h, MWRGB(232, 232, 232));	/* not here yet */
		else
			GrDrawImageFromFile(to, ui_gc, it->x, y, it->w, it->h, (char *)file, 0);
		break;
	case I_FIELD:
		draw_field(to, it, y);
		break;
	}
}

/* Draws the page's rows y0 to y1 on `to`, whose own top row is the page's `dy`. */
static void
draw_rows(GR_DRAW_ID to, int dy, int y0, int y1)
{
	unsigned began = now();
	int i, drawn = 0;

	ui_fill(to, 0, y0 - dy, view_width(), y1 - y0, mw(web_page_bg));
	for (i = 0; i < web_item_count; i++) {
		const struct web_item *it = &web_items[i];

		if (it->y >= y1 || it->y + it->h + 2 <= y0)
			continue;
		draw_item(to, it, dy);
		drawn++;
	}
	if (verbose)
		printf("nxweb: asked for %d of %d things, rows %d to %d, %u thousand instructions\n", drawn, web_item_count, y0, y1,
		       (now() - began) / 1000);
}

/* ---- the pixmap as a ring of strips (docs/apps/web.md, "Strips") ---- */

/* Strip k of the page (rows from k * STRIP) is in slot k % slot_count: a scroll keeps every row
 * drawn, and the view is one copy, two where the ring wraps. A strip is a GPU list of its own made
 * in its slot's memory by a worker (or this core); one with a field or a picture not in the ROM is
 * drawn by requests on `scratch` and copied in (docs/apps/web.md, "Strips"). */
#define STRIP 64
#define SLOTS_MOST 32
#define STRIP_COMMANDS 256
#define STRIP_CHARS 2048
#define STRIP_BYTES (STRIP_COMMANDS * 64 + STRIP_CHARS * 4)	/* a slot's list and its strings */
#define STRIP_WORKERS 4
#define PICTURE_WORKERS 16	/* opened, for the pictures' decoding: a 250 x 180 JPEG 646 ms with 4, 275 with 16 or more */
#define REG_SUBMIT 4		/* the GPU's words (docs/machine/gpu.md): submit, list, count, lists drawn */
#define REG_LIST 5
#define REG_COUNT 6
#define REG_DRAWN 7
#define REG_CLOCK 13
#define REG_COPIES 14
#define REG_INTO 20		/* where the picture is copied: address, width, height, row */
#define REG_LOCK 24
#define REG_COPY 28		/* nonzero: the list drawn last has its copy still to be made */
#define GPU_PHYS 0x80000000u
#define PASS_OVER (5u << 16)	/* blends by alpha, as the window system draws text of coverage */

enum { SLOT_EMPTY, SLOT_OUT, SLOT_MADE, SLOT_THERE };	/* nothing; a job makes its list; made; drawn or submitted */
static struct slot {
	int strip, state, worker;
	uint32_t made;		/* the job's answer: commands | 1 << 16 when it blends; 0: drawn by requests */
	int listed, requested;	/* a list of it the GPU may not have drawn (since `drawn`); requests on its rows */
	uint32_t drawn, landed;	/* the GPU's count of lists drawn when it was submitted; of copies once it is in */
} slots[SLOTS_MOST];
static int slot_count;
static GR_WINDOW_ID scratch, lists;
static uint8_t *list_memory;	/* the slots' lists, mapped; NULL: every strip by requests */
static uint32_t list_phys, tile_phys;
static size_t list_mapped;
static int gpu_fd = -1;
static int strip_workers = -1, worker_strip[STRIP_WORKERS + 1];	/* (-1: none opened yet) the slot each works on, or -1 */
static int *strip_items, *strip_at, strip_total;	/* strip k's items: strip_items[strip_at[k] .. strip_at[k + 1] - 1] */
/* What a job draws besides the items, as the first core last had it with no job out. */
static struct { int stale, found, found_x, found_w, found_from, found_n, picked; } strip_shown = { 1 };
static struct { unsigned requests, here, workers, syncs, waits; } strip_counts;

/* A face's first 256 glyphs and advances, and where its file lies in the ROM (1: not there). */
static struct strip_face { uint16_t glyph[256]; uint8_t advance[256]; uint32_t rom, scale, atlas; } strip_faces[WEB_FACES];

/* Strips by requests only (NXWEB_STRIPS=requests); by the first core alone (=here). */
static int
strips_by(const char *how)
{
	const char *by = getenv("NXWEB_STRIPS");

	return by && !strcmp(by, how);
}

/* For each strip, the items that touch its rows, in the order they are drawn. */
static void
strips_index(void)
{
	int i, k, last, *fill;

	free(strip_items);
	free(strip_at);
	strip_total = (web_page_h + 2048) / STRIP + 1;	/* (a view below a short page is strips too) */
	strip_at = calloc(strip_total + 1, sizeof *strip_at);
	fill = calloc(strip_total + 1, sizeof *fill);
	for (i = 0; i < web_item_count; i++) {
		/* (a link's frame is a row above the item, a line under it two below) */
		k = web_items[i].y - 2 < 0 ? 0 : (web_items[i].y - 2) / STRIP;
		last = (web_items[i].y + web_items[i].h + 1) / STRIP;
		for (last = last < strip_total ? last : strip_total - 1; k <= last; k++)
			strip_at[k + 1]++;
	}
	for (k = 0; k < strip_total; k++)
		strip_at[k + 1] += strip_at[k];
	strip_items = malloc((strip_at[strip_total] + 1) * sizeof *strip_items);
	for (i = 0; i < web_item_count; i++) {
		k = web_items[i].y - 2 < 0 ? 0 : (web_items[i].y - 2) / STRIP;
		last = (web_items[i].y + web_items[i].h + 1) / STRIP;
		for (last = last < strip_total ? last : strip_total - 1; k <= last; k++)
			strip_items[strip_at[k] + fill[k]++] = i;
	}
	free(fill);
}

static int
strip_glyph(const unsigned char *file, unsigned code)
{
	const struct sef_head *h = (const struct sef_head *)file;
	int g = sef_glyph(file, code < 32 ? 32 : code);

	return g >= 0 ? g : h->missing < h->glyphs ? (int)h->missing : 0;
}

/* Where a file lies in the ROM, as the window system finds a font (rom_address in
 * scr_shaderemu.c): romfs numbers a file by its header's place, the data after its name. */
static uint32_t
rom_of(int fd, const unsigned char *begins)
{
	static int mtd = -2;
	unsigned char head[16 + 256], data[64];
	struct stat st;
	int n, at;

	if (mtd == -2)
		mtd = open("/dev/mtd0", O_RDONLY | O_CLOEXEC);
	if (mtd < 0 || fstat(fd, &st) || (n = pread(mtd, head, sizeof head, st.st_ino)) < 32)
		return 0;
	for (at = 16; at < n && head[at]; at++)
		continue;
	if (at >= n || ((uint32_t)head[8] << 24 | head[9] << 16 | head[10] << 8 | head[11]) != (uint32_t)st.st_size ||
	    pread(mtd, data, 64, st.st_ino + 16 + (at & ~15)) != 64 || memcmp(data, begins, 64))
		return 0;
	return 0x40000000u + (uint32_t)st.st_ino + 16 + (at & ~15u);
}

/* Whether a face can be drawn by a strip's list: its tables made here, once. */
static int
strip_face_ready(int face)
{
	struct strip_face *f = &strip_faces[face];
	const struct web_face *w = web_face(face);
	char path[80];
	int fd, c, g;
	uint32_t rom_of_sef = 0;

	if (!f->rom) {
		f->rom = 1;
		snprintf(path, sizeof path, "/usr/share/%s", w->name);
		if (w->file && (fd = open(path, O_RDONLY)) >= 0) {
			uint32_t rom = rom_of(fd, w->file);

			f->rom = rom ? rom : 1;
			rom_of_sef = f->rom;
			close(fd);
		}
		if (f->rom > 1 && ((const struct sef_head *)w->file)->bpp == SEF_MSDF) {
			/* its atlas: font-NAME.msdf beside font-NAME-N.sef */
			unsigned char head[64];
			char *dash = strrchr(path, '-');

			f->rom = 1;
			if (dash && (strcpy(dash, ".msdf"), fd = open(path, O_RDONLY)) >= 0) {
				if (read(fd, head, 64) == 64 && (f->atlas = rom_of(fd, head)) != 0)
					f->rom = rom_of_sef;
				close(fd);
			}
		}
		for (c = 0; f->rom > 1 && c < 256; c++) {
			g = strip_glyph(w->file, c);
			f->glyph[c] = (uint16_t)g;
			f->advance[c] = (uint8_t)sef_advance(w->file, g);
		}
		f->scale = w->twice ? 2u << 16 : 0;
	}
	return f->rom > 1;
}

/* ---- the page's pictures in the GPU's memory ---- */

/* One pixmap used as memory, taken from in turn and given back whole with the page's pictures:
 * the window system has eight pixmaps of the GPU's for every program. */
#define PICTURE_BYTES (2048 * 1024)
#define PICTURE_ROW 1024	/* the pixmap's width */
static GR_WINDOW_ID picture_pixmap;
static uint8_t *picture_memory;
static uint32_t picture_phys, picture_room, picture_used;

/* Room for `bytes` of pixels on 16 bytes, written by this core first; NULL where there is none. */
static uint8_t *
picture_take(uint32_t bytes)
{
	GR_PIXELVAL pixel;
	uint8_t *at;
	int rows;

	if (!picture_pixmap && !picture_room && regs && gpu_fd >= 0) {
		/* (the most, else less: the pool is every window's) */
		for (rows = PICTURE_BYTES / (4 * PICTURE_ROW); !picture_pixmap && rows >= 64; rows /= 2)
			picture_pixmap = gpu_pixmap(PICTURE_ROW, rows);
		rows *= 2;
		picture_phys = picture_pixmap ? pixmap_address(picture_pixmap, PICTURE_ROW) : 0;
		picture_memory = picture_phys ? mmap(NULL, (size_t)rows * PICTURE_ROW * 4, PROT_READ | PROT_WRITE, MAP_SHARED, gpu_fd,
						     picture_phys - GPU_PHYS) : MAP_FAILED;
		if (picture_memory == MAP_FAILED) {
			picture_memory = NULL;
			if (picture_pixmap)
				GrDestroyWindow(picture_pixmap);
			picture_pixmap = 0;
			picture_room = 1;	/* (not tried again) */
			if (verbose)
				printf("nxweb: no memory of the GPU's for pictures (%s): they are files\n", picture_phys ? "not mapped" :
				       "no pixmap with an address");
		} else {
			GrReadArea(picture_pixmap, 0, 0, 1, 1, &pixel);	/* (the GPU has made the new pixmap black by now) */
			picture_room = (uint32_t)rows * PICTURE_ROW * 4;
		}
	}
	if (!picture_memory || bytes > picture_room - picture_used)
		return NULL;
	at = picture_memory + picture_used;
	picture_used = (picture_used + bytes + 15) & ~15u;
	mcw_touch(at, bytes);
	return at;
}

/* A picture's pixels as a PPM file. */
static int
picture_write(const char *path, const uint8_t *pixels, int w, int h, int stride)
{
	FILE *f = fopen(path, "wb");
	int y, ok;

	if (!f)
		return 0;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (y = 0, ok = 1; y < h && ok; y++)
		ok = fwrite(pixels + (size_t)y * stride, 3 * w, 1, f) == 1;
	return !fclose(f) && ok;
}

/* `in` stretched over `out` from the nearest pixels, for a page that draws a picture in another shape. */
static void
picture_stretch(const struct media_image *in, struct media_image *out)
{
	int x, y;

	for (y = 0; y < out->height; y++) {
		const uint8_t *row = in->pixels + (size_t)((2 * y + 1) * in->height / (2 * out->height)) * in->stride;
		uint8_t *to = out->pixels + (size_t)y * out->stride;

		for (x = 0; x < out->width; x++)
			memcpy(to + 3 * x, row + 3 * ((2 * x + 1) * in->width / (2 * out->width)), 3);
	}
}

/* A picture decoded on the worker cores to the size it is drawn at, w x h, into the GPU's memory,
 * where a strip's list copies it and the window system draws its PPM (`kept`) pixel for pixel; where
 * the GPU's memory has no room, into that file alone. Whether it is there at that size. */
static int
picture_decode(struct picture *p, int w, int h)
{
	struct media_image in, out;
	struct stat st;
	const char *why = "it could not be read";
	char note[96];
	unsigned began = now();
	struct timespec t0, t1;
	uint8_t *data, *spare = NULL;
	int fd, workers = mcw_most();

	if (p->dw || !p->source[0] || w <= 0 || h <= 0)
		return p->dw == w && p->dh == h;	/* (one size a picture: another is drawn from its file, scaled) */
	if (!pictures_now) {
		/* (the page is shown first, the picture a placeholder until pictures_meanwhile() decodes it) */
		if (!p->later)
			p->later = ++pictures_later, p->later_w = w, p->later_h = h, p->tracked = 1;
		return 0;
	}
	p->later = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	fd = open(p->source, O_RDONLY);
	data = fd < 0 || fstat(fd, &st) || st.st_size < 16 ? MAP_FAILED : mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	if (fd >= 0)
		close(fd);
	if (data == MAP_FAILED) {
		p->source[0] = 0;
		if (!p->file[0])
			picture_fail(p, "cannot be read");
		return 0;
	}
	strips_open();	/* (the library decodes on the workers this program has open) */
	strips_wait();
	memset(&in, 0, sizeof in);
	memset(&out, 0, sizeof out);
	out.width = w, out.height = h, out.channels = 3, out.stride = 3 * w;
	if ((out.pixels = picture_take((uint32_t)(3 * w * h))) == NULL)
		out.pixels = spare = malloc((size_t)3 * w * h);
	if (!out.pixels)
		why = "no memory";
	else if (p->w == w && p->h == h)
		why = media_image_decode(data, (uint32_t)st.st_size, &out, workers);
	else {
		/* (its own size first, in the GPU's memory after the picture while there is room: no pages to clear) */
		uint32_t mark = picture_used;

		in.width = p->w, in.height = p->h, in.channels = 3, in.stride = (3 * p->w + 15) & ~15;
		in.pixels = spare ? NULL : picture_take((uint32_t)in.stride * (uint32_t)p->h);
		if (!in.pixels)
			in.width = in.height = in.stride = 0;
		if (!(why = media_image_decode(data, (uint32_t)st.st_size, &in, workers))) {
			/* (smaller: the mean of the pixels under each; a shape within 2% is covered, another stretched) */
			if (abs(in.width * h - in.height * w) * 50 <= in.width * h)
				media_image_fit(&in, &out, 1, workers);
			else
				picture_stretch(&in, &out);
		}
		if (in.owned)
			media_image_free(&in);
		else if (in.pixels)
			picture_used = mark;
	}
	munmap(data, (size_t)st.st_size);
	if (!why && spare)
		p->kept_there = picture_write(p->kept, spare, w, h, 3 * w);
	else if (!why) {
		p->pixels = out.pixels;
		p->rom = picture_phys + (uint32_t)(out.pixels - picture_memory);
	}
	free(spare);
	if (!why && (p->pixels || p->kept_there))
		p->dw = w, p->dh = h;
	pictures_decoding += now() - began;
	if (verbose) {
		clock_gettime(CLOCK_MONOTONIC, &t1);
		printf("nxweb: %s, %d x %d drawn %d x %d: %s, %u thousand instructions, %ld ms\n", p->source, p->w, p->h, w, h,
		       why ? why : p->pixels ? "decoded into the GPU's memory" : "decoded into a file", (now() - began) / 1000,
		       (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
	}
	if (!p->dw)
		p->source[0] = 0;	/* (not tried again) */
	if (!p->dw && !p->file[0]) {
		snprintf(note, sizeof note, "cannot be decoded: %s", why ? why : "no room");
		picture_fail(p, note);
	}	/* (a fetched one is drawn from its PPM) */
	return p->dw == w && p->dh == h;
}

/* The PPM file the window system draws a picture from at w x h: its decoded pixels' pixel for pixel
 * when they are that size (written when first asked for), else the file it came as; NULL while its
 * decoding is waited for (a placeholder, as a strip's list draws it). */
static const char *
picture_file(struct picture *p, int w, int h)
{
	if (picture_decode(p, w, h) || (!p->file[0] && p->dw)) {
		if (!p->kept_there && p->pixels)
			p->kept_there = picture_write(p->kept, p->pixels, p->dw, p->dh, 3 * p->dw);
		if (p->kept_there)
			return p->kept;
	}
	return p->file[0] && !p->later ? p->file : NULL;
}

/* The page's pictures go: their files, their room in the GPU's memory. */
static void
pictures_drop(void)
{
	int i;

	strips_wait();
	for (i = 0; i < picture_count; i++) {
		if (pictures[i].ours)
			unlink(pictures[i].file);
		if (pictures[i].kept_there)
			unlink(pictures[i].kept);
	}
	picture_count = 0;
	picture_used = 0;
	pictures_decoding = 0;
	pictures_later = 0;
}

static int
pictures_waiting(void)
{
	int i;

	for (i = 0; i < picture_count; i++)
		if (pictures[i].later && pictures[i].state == PICTURE_HERE)
			return 1;
	return 0;
}

/* The picture waited for longest decoded, and its rows drawn again: whether there was one. */
static int
pictures_meanwhile(void)
{
	struct picture *p = NULL;
	int i;

	for (i = 0; i < picture_count; i++)
		if (pictures[i].later && pictures[i].state == PICTURE_HERE && (!p || pictures[i].later < p->later))
			p = &pictures[i];
	if (!p)
		return 0;
	pictures_now = 1;
	picture_decode(p, p->later_w, p->later_h);
	pictures_now = 0;
	p->later = 0;
	for (i = 0; i < web_item_count; i++)
		if (web_items[i].kind == I_IMAGE && web_items[i].a == (int)(p - pictures))
			strips_forget_rows(web_items[i].y, web_items[i].y + web_items[i].h);
	return 1;
}

/* After a layout: a picture decoded to a size no item has any more is decoded again when drawn
 * (its old pixels stay in the GPU's memory until the page goes); one waited for is asked for again. */
static void
pictures_relaid(void)
{
	int i, k;

	for (k = 0; k < picture_count; k++) {
		struct picture *p = &pictures[k];

		p->later = 0;
		for (i = 0; p->source[0] && p->dw && i < web_item_count; i++)
			if (web_items[i].kind == I_IMAGE && web_items[i].a == k && web_items[i].w == p->dw && web_items[i].h == p->dh)
				break;
		if (!p->source[0] || !p->dw || i < web_item_count)
			continue;
		if (p->kept_there)
			unlink(p->kept);
		p->kept_there = p->dw = p->dh = 0;
		p->pixels = NULL;
		p->rom = 0;
	}
}

/* Whether a strip's list can draw a picture at w x h: its pixels decoded to that size in the GPU's
 * memory, or a PPM file in the ROM sampled where it lies at any size, as the window system draws
 * one (gpu_picture in scr_shaderemu.c). */
static int
strip_picture_ready(struct picture *p, int w, int h)
{
	unsigned char head[65];
	int fd, pw, ph, most, start;
	uint32_t rom;

	if (p->source[0] || p->pixels || p->kept_there)
		return picture_decode(p, w, h) && p->pixels;
	if (!p->rom) {
		p->rom = 1;
		if ((fd = open(p->file, O_RDONLY)) < 0)
			return 0;
		head[64] = 0;
		if (read(fd, head, 64) == 64 && sscanf((const char *)head, "P6 %d %d %d%n", &pw, &ph, &most, &start) == 3 && most == 255 &&
		    pw == p->w && ph == p->h && (rom = rom_of(fd, head)) != 0) {
			p->rom = rom + (uint32_t)start + 1;	/* (one white space ends the header) */
			p->dw = pw, p->dh = ph;
		}
		close(fd);
	}
	return p->rom > 1;
}

/* A list being made in a slot's memory: its commands, its strings, the GPU's vertices. */
struct strip_list { uint32_t *c, *end, *words, *word, *words_end, phys_words, mesh; int blended, first, full; };

/* A rectangle, x1 and y1 exclusive, cut to the strip as the window system cuts to a pixmap. */
static void
list_rect(struct strip_list *l, int x, int y, int w, int h, int colour)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tile_w ? tile_w : x + w, y1 = y + h > STRIP ? STRIP : y + h;
	uint32_t *c = l->c;

	if (w <= 0 || h <= 0 || x0 >= x1 || y0 >= y1)
		return;
	if (c == l->end) {
		l->full = 1;
		return;
	}
	c[0] = 2;
	c[1] = (uint32_t)colour & 0xffffff;
	c[2] = 0;
	c[3] = l->mesh;
	c[4] = x0, c[5] = y0, c[6] = x1, c[7] = y1;
	c[8] = l->blended && !l->first ? PASS_OVER : 0;
	l->mesh += 6;
	l->c += 16;
	l->first = 0;
}

/* A picture of the ROM stretched over a rectangle, its part in the strip sampled as the window
 * system samples it (PICTURE_U and PICTURE_V in scr_shaderemu.c). */
static void
list_picture(struct strip_list *l, int x, int y, int w, int h, const struct picture *p)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > tile_w ? tile_w : x + w, y1 = y + h > STRIP ? STRIP : y + h;
	uint32_t *c = l->c;

	if (w <= 0 || h <= 0 || x0 >= x1 || y0 >= y1)
		return;
	if (c == l->end) {
		l->full = 1;
		return;
	}
	c[0] = 2;
	c[1] = 0xffffff;
	c[2] = 0xff00ff;
	c[3] = l->mesh;
	c[4] = x0, c[5] = y0, c[6] = x1, c[7] = y1;
	c[8] = 4 | (l->blended && !l->first ? PASS_OVER : 0);
	c[9] = p->rom;
	c[10] = p->dw;
	c[11] = p->dh;
	c[12] = (uint32_t)((((long long)(x0 - x) * p->dw) << 16) / ((long long)w * p->dw));
	c[13] = (uint32_t)((((long long)(y0 - y) * p->dh) << 16) / ((long long)h * p->dh));
	c[14] = (uint32_t)((((long long)(x1 - x) * p->dw) << 16) / ((long long)w * p->dw));
	c[15] = (uint32_t)((((long long)(y1 - y) * p->dh) << 16) / ((long long)h * p->dh));
	l->mesh += 6;
	l->c += 16;
	l->first = 0;
}

/* GrRect's lines (GdRect in the engine). */
static void
list_frame(struct strip_list *l, int x, int y, int w, int h, int colour)
{
	if (w <= 0 || h <= 0)
		return;
	list_rect(l, x, y, w, 1, colour);
	if (h > 1)
		list_rect(l, x, y + h - 1, w, 1, colour);
	if (h < 3)
		return;
	list_rect(l, x, y + 1, 1, h - 2, colour);
	if (w > 1)
		list_rect(l, x + w - 1, y + 1, 1, h - 2, colour);
}

/* A text as one command and a word a character, as romfont_draw in scr_shaderemu.c makes it. */
static void
list_text(struct strip_list *l, int x, int y, const unsigned short *text, int n, int face, int italic, int colour)
{
	const struct strip_face *f = &strip_faces[face];
	const unsigned char *file = web_faces[face].file;
	const struct sef_head *h = (const struct sef_head *)file;
	uint32_t *c = l->c, *word = l->word, scale = f->scale;
	int at = 0, i, g, w, height;

	if (n <= 0)
		return;
	if (c == l->end || word + n > l->words_end) {
		l->full = 1;
		return;
	}
	for (i = 0; i < n; i++) {
		unsigned code = text[i];

		g = code < 256 ? f->glyph[code] : strip_glyph(file, code);
		word[i] = (uint32_t)g | (uint32_t)(scale ? (at * (int)scale + 0x8000) >> 16 : at) << 16;
		at += code < 256 ? f->advance[code] : sef_advance(file, g);
	}
	w = scale ? (at * (int)scale + 0x8000) >> 16 : at;
	height = scale ? (h->height * (int)scale + 0x8000) >> 16 : h->height;
	if (x + w - 1 < 0 || x >= tile_w || y + height - 1 < 0 || y >= STRIP)
		return;
	if (h->bpp == 4 || h->bpp == SEF_MSDF)
		l->blended = 1;
	c[0] = 4;
	c[1] = (uint32_t)colour & 0xffffff;
	c[2] = f->rom + h->table_at;
	c[3] = l->mesh;
	c[4] = (uint32_t)x;
	c[5] = (uint32_t)y;
	c[6] = (uint32_t)n;
	c[7] = l->phys_words + 4 * (uint32_t)(word - l->words);
	c[8] = (h->bpp == SEF_MSDF ? 8 : h->bpp == 4 ? 7 : 3) | 0x100 | (l->blended ? PASS_OVER : 0);
	c[9] = scale;
	c[10] = (italic ? 51u : 0) | (uint32_t)h->ascent << 16;
	c[11] = h->bpp == SEF_MSDF ? f->atlas : f->rom;
	c[12] = h->bpp == SEF_MSDF ? h->range : 0;
	l->mesh += 6 * (uint32_t)n;
	l->c += 16;
	l->word += (n + 3) & ~3;
	l->first = 0;
}

/* A worker's job (or this core's): strip k's list in slot s's memory, as draw_item would draw
 * it. Reads the page and writes nothing but the slot's memory. Its answer as `made` says. */
static uint32_t
strip_job(uint32_t k, uint32_t s)
{
	uint32_t *memory = (uint32_t *)(list_memory + s * STRIP_BYTES), *words = memory + 16 * STRIP_COMMANDS;
	struct strip_list l = { .c = memory, .end = words, .words = words, .word = words, .words_end = words + STRIP_CHARS,
				.phys_words = list_phys + s * STRIP_BYTES + 64 * STRIP_COMMANDS, .first = 1 };
	int i, y0 = (int)k * STRIP;

	list_rect(&l, 0, 0, tile_w, STRIP, web_page_bg);
	for (i = strip_at[k]; i < strip_at[k + 1] && !l.full; i++) {
		const struct web_item *it = &web_items[strip_items[i]];
		int y = it->y - y0, italic = (it->flags & IF_ITALIC) != 0;

		switch (it->kind) {
		case I_BOX:
			if (it->colour >= 0 && it->w > 0 && it->h > 0)
				list_rect(&l, it->x, y, it->w, it->h, it->colour);
			if (it->flags && it->w > 0)
				list_frame(&l, it->x, y, it->w, it->h, it->b);
			break;
		case I_TEXT:
			list_text(&l, it->x, y, web_u + it->a, it->b, it->face, italic, it->colour);
			if (it->flags & IF_UNDER)
				list_rect(&l, it->x, y + web_faces[it->face].ascent + 1, it->w, 1, it->colour);
			if (it->flags & IF_THROUGH)
				list_rect(&l, it->x, y + web_faces[it->face].ascent * 2 / 3, it->w, 1, it->colour);
			if (strip_items[i] == strip_shown.found) {
				list_rect(&l, strip_shown.found_x, y, strip_shown.found_w, it->h, 0x000080);
				list_text(&l, strip_shown.found_x, y, web_u + it->a + strip_shown.found_from, strip_shown.found_n, it->face, italic, 0xffffff);
			}
			if (strip_shown.picked >= 0 && web_link_of(it->node) == strip_shown.picked)
				list_frame(&l, it->x - 2, y - 1, it->w + 4, it->h + 2, it->colour);
			break;
		case I_IMAGE:
			if (pictures[it->a].rom > 1)
				list_picture(&l, it->x, y, it->w, it->h, &pictures[it->a]);
			else
				list_rect(&l, it->x, y, it->w, it->h, 0xe8e8e8);	/* not here yet */
			break;
		default:
			l.full = 1;
		}
	}
	return l.full ? 0 : (uint32_t)(l.c - memory) / 16 | (uint32_t)l.blended << 16;
}

/* Whether strip k may be a list: nothing in it that only the window system draws. */
static int
strip_by_list(int k)
{
	int i;

	if (!list_memory)
		return 0;
	for (i = strip_at[k]; i < strip_at[k + 1]; i++) {
		const struct web_item *it = &web_items[strip_items[i]];

		if (it->kind == I_FIELD || (it->kind == I_IMAGE && pictures[it->a].state == PICTURE_FAILED) ||
		    (it->kind == I_IMAGE && pictures[it->a].state == PICTURE_HERE &&
		    !strip_picture_ready(&pictures[it->a], it->w, it->h) && !pictures[it->a].later) ||
		    (it->kind == I_TEXT && !strip_face_ready(it->face)))
			return 0;
	}
	return 1;
}

/* Strip k drawn by requests: on the scratch pixmap, then copied into slot s. */
static void
strip_requests(int k, int s)
{
	int i;

	ui_fill(scratch, 0, 0, tile_w, STRIP, mw(web_page_bg));
	for (i = strip_at[k]; i < strip_at[k + 1]; i++)
		draw_item(scratch, &web_items[strip_items[i]], k * STRIP);
	GrCopyArea(tile, ui_gc, 0, s * STRIP, tile_w, STRIP, scratch, 0, 0, MWROP_COPY);
	slots[s].strip = k;
	slots[s].state = SLOT_THERE;
	slots[s].requested = 1;
	strip_counts.requests++;
}

/* Slot s's list to the GPU, copied into its rows of the pixmap; 0 while the GPU has another. */
static int
strip_submit(int s)
{
	struct slot *sl = &slots[s];
	GR_PIXELVAL pixel;
	int i;

	if (sl->requested) {
		/* the window system's drawing there must reach the GPU first: a read waits for it */
		GrReadArea(tile, 0, s * STRIP, 1, 1, &pixel);
		for (i = 0; i < slot_count; i++)
			slots[i].requested = 0;
		strip_counts.syncs++;
	}
	if (__atomic_exchange_n(&regs[REG_LOCK], 1, __ATOMIC_ACQUIRE))
		return 0;
	if (regs[REG_SUBMIT]) {
		__atomic_store_n(&regs[REG_LOCK], 0, __ATOMIC_RELEASE);
		return 0;
	}
	sl->drawn = regs[REG_DRAWN];
	sl->landed = regs[REG_COPIES] + (regs[REG_COPY] != 0) + 1;
	regs[REG_INTO] = tile_phys + (uint32_t)(s * STRIP * tile_w * 4);
	regs[REG_INTO + 1] = tile_w;
	regs[REG_INTO + 2] = STRIP;
	regs[REG_INTO + 3] = tile_w;
	regs[REG_LIST] = list_phys + s * STRIP_BYTES;
	regs[REG_COUNT] = sl->made & 0xffff;
	regs[REG_SUBMIT] = 1 | 4 | (sl->made >> 16 ? 1u << 13 : 0);
	__atomic_store_n(&regs[REG_LOCK], 0, __ATOMIC_RELEASE);
	sl->listed = 1;
	sl->state = SLOT_THERE;
	return 1;
}

/* The workers, borrowed: a program that keeps its own has them, and this core makes the strips. */
static int
strips_open(void)
{
	extern char __DATA_BEGIN__[], _end[], _start[], etext[];
	int k;

	if (strip_workers < 0) {
		strip_workers = 0;
		if (strips_by("here"))
			return 0;
		mcw_lend();
		/* (more for the pictures' decoding, which the library does on every one open) */
		strip_workers = mcw_open(PICTURE_WORKERS);
		strip_workers = strip_workers < STRIP_WORKERS ? strip_workers : STRIP_WORKERS;
		if (strip_workers > 0) {
			mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
			/* (from _start: a flat program's __executable_start is 0) */
			mcw_touch_code(_start, (unsigned)(etext - _start));
			atexit(mcw_close);
		} else
			strip_workers = 0;
		for (k = 0; k <= STRIP_WORKERS; k++)
			worker_strip[k] = -1;
	}
	return strip_workers;
}

/* A job out to a worker: its answer taken when it is there. */
static void
strip_collect(int s, int wait)
{
	struct slot *sl = &slots[s];

	if (sl->state != SLOT_OUT || (!wait && !mcw_done(sl->worker)))
		return;
	if (wait)
		mcw_wait(sl->worker);
	sl->made = mcw_result(sl->worker);
	sl->state = SLOT_MADE;
	worker_strip[sl->worker] = -1;
}

/* What the jobs are to draw besides the items, taken again after it changed (no job is out
 * then: they were waited for when it went stale). */
static void
strips_shown(void)
{
	const struct web_item *it = found >= 0 ? &web_items[found] : NULL;

	if (!strip_shown.stale)
		return;
	strip_shown.found = found;
	strip_shown.found_from = found_from;
	strip_shown.found_n = it ? web_least((int)strlen(sought), it->b - found_from) : 0;
	strip_shown.found_x = it ? it->x + item_width(it, 0, found_from) : 0;
	strip_shown.found_w = it ? item_width(it, found_from, strip_shown.found_n) : 0;
	strip_shown.picked = picked;
	strip_shown.stale = 0;
}

/* The strips the ring is to hold now, the view's first, then the way the scroll goes. */
static int
strips_wanted(int *order)
{
	int first = top / STRIP, last = (top + view_height() - 1) / STRIP, end = (web_page_h - 1) / STRIP;
	int spare = slot_count - (last - first + 1), behind, from, n = 0, k;

	end = end > last ? end : last;
	behind = goal > top ? (spare > 0) : goal < top ? spare - (spare > 0) : spare / 2;
	from = first - behind;
	from = from > end + 1 - slot_count ? end + 1 - slot_count : from;
	from = from < 0 ? 0 : from;
	for (k = first; k <= last && k <= end; k++)
		order[n++] = k;
	if (goal >= top)
		for (k = last + 1; k < from + slot_count && k <= end; k++)
			order[n++] = k;
	for (k = first - 1; k >= from; k--)
		order[n++] = k;
	if (goal < top)
		for (k = last + 1; k < from + slot_count && k <= end; k++)
			order[n++] = k;
	return n;
}

/* Some of the ring's work, in the order the strips are wanted: a list submitted, a strip given to a
 * worker, or one made here (`now`: the view's, not waited for). 0: the ring holds what is wanted;
 * 1: more can be done at once; 2: a worker or the GPU is waited for. */
static int
strips_work(int now)
{
	int order[SLOTS_MOST], n, i, k, s, waiting = 0, view = (top + view_height() - 1) / STRIP - top / STRIP + 1;

	if (!slot_count)
		return 0;
	strips_shown();
	for (s = 0; s < slot_count; s++)
		strip_collect(s, 0);
	n = strips_wanted(order);
	for (i = 0; i < n; i++) {
		struct slot *sl;
		int w = 0;

		k = order[i];
		sl = &slots[s = k % slot_count];
		if (sl->strip == k && sl->state == SLOT_THERE)
			continue;
		if (sl->strip == k && sl->state == SLOT_MADE) {
			if (!sl->made) {
				strip_requests(k, s);	/* (too much for its list) */
				return 1;
			}
			if (strip_submit(s))
				return 1;
			waiting = 1;
			continue;
		}
		if (sl->state == SLOT_OUT) {
			if (sl->strip == k && now && i < view) {
				strip_collect(s, 1);	/* (the view's: waited for) */
				return 1;
			}
			waiting = 1;
			continue;
		}
		if (sl->listed && regs[REG_DRAWN] == sl->drawn) {
			waiting = 1;	/* its last list is still to be drawn */
			continue;
		}
		sl->listed = 0;
		if (!strip_by_list(k)) {
			strip_requests(k, s);
			return 1;
		}
		if (!(now && i < view) && strips_open())
			for (w = 1; w <= strip_workers && worker_strip[w] >= 0; w++)
				;
		if (w >= 1 && w <= strip_workers) {
			sl->strip = k;
			sl->state = SLOT_OUT;
			sl->worker = w;
			worker_strip[w] = s;
			/* (one whose core another program has back is run here, at once) */
			if (mcw_bound(w))
				strip_counts.workers++;
			else
				strip_counts.here++;
			mcw_post(w, strip_job, (uint32_t)k, (uint32_t)s);
			waiting = 1;
			continue;
		}
		if (w > 0) {
			waiting = 1;	/* every worker has one (w 0: none looked for, as for the view's) */
			continue;
		}
		sl->strip = k;
		sl->made = strip_job((uint32_t)k, (uint32_t)s);
		sl->state = SLOT_MADE;
		strip_counts.here++;
		return 1;
	}
	return waiting ? 2 : 0;
}

/* Every job out finished and its answer taken. */
static void
strips_wait(void)
{
	int s;

	for (s = 0; s < slot_count; s++)
		strip_collect(s, 1);
}

/* No strip is there any more (the page, or what is shown of it, changed). */
static void
strips_forget(void)
{
	int s;

	strips_wait();
	for (s = 0; s < slot_count; s++) {
		slots[s].strip = -1;
		slots[s].state = SLOT_EMPTY;
	}
	strip_shown.stale = 1;
}

/* The strips that hold page rows y0 to y1 are not there any more. */
static void
strips_forget_rows(int y0, int y1)
{
	int k, s;

	for (k = (y0 - 2 < 0 ? 0 : y0 - 2) / STRIP; slot_count && k <= (y1 + 1) / STRIP; k++)
		if (slots[s = k % slot_count].strip == k) {
			strip_collect(s, 1);
			slots[s].state = SLOT_EMPTY;
			slots[s].strip = -1;
		}
}

/* The view's strips are all there: made now where they are not. */
static void
strips_view(void)
{
	int k, missing;

	for (;;) {
		for (k = top / STRIP, missing = 0; k <= (top + view_height() - 1) / STRIP; k++)
			missing += !(slots[k % slot_count].strip == k && slots[k % slot_count].state == SLOT_THERE);
		if (!missing)
			return;
		if (strips_work(1) == 2) {
			strip_counts.waits++;
			mcw_rest();
		}
	}
}

/* Strips made ahead while nothing else is to be done, a pass at a time: until an event is
 * there, or `ms` have gone. The milliseconds left. */
static int
strips_meanwhile(int ms)
{
	GR_EVENT event;
	uint32_t from = regs ? regs[REG_CLOCK] : 0;
	int r;

	if (!slot_count || !window)
		return ms;
	GrFlush();
	while ((r = strips_work(0)) && !GrPeekEvent(&event)) {
		if ((int)(regs[REG_CLOCK] - from) >= ms)
			return 0;
		if (r == 2)
			mcw_rest();
		GrFlush();
	}
	ms -= (int)(regs[REG_CLOCK] - from);
	return ms > 0 ? ms : 0;
}

static void
tile_free(void)
{
	strips_forget();
	if (tile)
		GrDestroyWindow(tile);
	if (scratch)
		GrDestroyWindow(scratch);
	if (lists)
		GrDestroyWindow(lists);
	if (list_memory)
		munmap(list_memory - (list_phys & 4095), list_mapped);
	tile = scratch = lists = 0;
	list_memory = NULL;
	slot_count = 0;
}

/* A pixmap the GPU keeps, or 0 (the driver counts them). */
static GR_WINDOW_ID
gpu_pixmap(int w, int h)
{
	GR_FONT_INFO info;
	uint32_t before = regs[REG_PIXMAPS];
	GR_WINDOW_ID p = GrNewPixmap(w, h, NULL);

	GrGetFontInfo(ui_var, &info);	/* (an answer: the pixmap is made by now) */
	if (regs[REG_PIXMAPS] != before)
		return p;
	GrDestroyWindow(p);
	return 0;
}

/* Where a pixmap the GPU keeps has its pixels, or 0. A server from before says nothing for a
 * pixmap (the words are left as they were): a buffer of the pool is whole pages, a row its width. */
static uint32_t
pixmap_address(GR_WINDOW_ID p, int w)
{
	GR_WINDOW_INFO info;

	GrGetWindowInfo(p, &info);
	return info.surface_address >= GPU_PHYS && info.surface_address < GPU_PHYS + 0xc00000u && !(info.surface_address & 4095) &&
	       info.surface_row == (uint32_t)w ? info.surface_address : 0;
}

/* The pixmap the page is drawn in, for the window as it is now: as high as TILE_BYTES allow
 * and no higher than the GPU's target, whose rows are all the window system draws on it (720
 * in the Unity world), in whole strips; the scratch strip, and the lists' memory where the
 * window system says where a pixmap's pixels are. 0: none, the page is drawn on the window. */
static int
tile_make(void)
{
	GR_PIXELVAL pixel;
	int w = view_width(), h = TILE_BYTES / (4 * w), s;
	int most = regs && (regs[REG_HOST] >> 16 & 255) ? (regs[REG_HOST] >> 16 & 255) * 16 : 2048;
	int need = (view_height() + STRIP - 1) / STRIP + 1;
	const char *rows = getenv("NXWEB_ROWS");	/* (a test: no more rows, as under the world's target) */

	most = rows && atoi(rows) > 0 && atoi(rows) < most ? atoi(rows) : most;
	h = h > most ? most : h;
	if (tile_w == w && (slot_count >= need || !tile_gpu))
		return tile_gpu;	/* (made, or tried in vain, for this width already) */
	tile_free();
	tile_gpu = 0;
	tile_w = w;
	slot_count = h / STRIP < SLOTS_MOST ? h / STRIP : SLOTS_MOST;
	if (!regs || slot_count < need || getenv("NXWEB_DIRECT") || !(tile = gpu_pixmap(w, slot_count * STRIP)) ||
	    !(scratch = gpu_pixmap(w, STRIP))) {
		tile_free();
		return 0;
	}
	tile_h = slot_count * STRIP;
	for (s = 0; s < slot_count; s++)
		slots[s].strip = -1, slots[s].state = SLOT_EMPTY, slots[s].listed = slots[s].requested = 0;
	tile_phys = pixmap_address(tile, w);
	if (tile_phys && gpu_fd >= 0 && !web_plain && !strips_by("requests") && (lists = gpu_pixmap(256, (slot_count * STRIP_BYTES + 1023) / 1024))) {
		list_phys = pixmap_address(lists, 256);
		list_mapped = (list_phys & 4095) + slot_count * STRIP_BYTES;
		list_memory = list_phys ? mmap(NULL, list_mapped, PROT_READ | PROT_WRITE, MAP_SHARED, gpu_fd, (list_phys - GPU_PHYS) & ~4095u) : MAP_FAILED;
		if (list_memory == MAP_FAILED)
			list_memory = NULL;
		else {
			list_memory += list_phys & 4095;
			mcw_touch(list_memory, slot_count * STRIP_BYTES);
		}
	}
	/* (a new pixmap is made black by a list of the window system's: that is drawn before ours) */
	GrReadArea(lists ? lists : scratch, 0, 0, 1, 1, &pixel);
	tile_gpu = 1;
	strip_shown.stale = 1;
	if (!strip_at)
		strips_index();
	return 1;
}

/* NXWEB_CHECK (a test): every strip of the page that may be a list made both ways, by its
 * list into the ring and by requests on the scratch pixmap, and the two read back and compared. */
static void
strips_check(void)
{
	GR_WINDOW_INFO info;
	GR_PIXELVAL pixel;
	const uint32_t *ring, *drawn;
	void *a, *b;
	size_t ring_bytes, scratch_bytes;
	int k, s, i, end = (web_page_h - 1) / STRIP, made = 0, not_lists = 0, strips_differ = 0;
	unsigned long differ = 0, far = 0;

	if (!tile_make() || !list_memory) {
		printf("nxweb: check of %s: no strips as lists here (pixmap %d, its address %08x, %d slots, the lists %s)\n", address, tile_gpu,
		       (unsigned)tile_phys, slot_count, lists ? "made, not mapped" : "not made");
		return;
	}
	GrGetWindowInfo(scratch, &info);
	ring_bytes = (tile_phys & 4095) + (size_t)tile_h * tile_w * 4;
	scratch_bytes = (info.surface_address & 4095) + (size_t)STRIP * tile_w * 4;
	a = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, gpu_fd, (tile_phys - GPU_PHYS) & ~4095u);
	b = mmap(NULL, scratch_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, gpu_fd, (info.surface_address - GPU_PHYS) & ~4095u);
	if (a == MAP_FAILED || b == MAP_FAILED) {
		printf("nxweb: check: the pixmaps could not be mapped (%s; %08x %u %p, %08x %u %p)\n", strerror(errno), (unsigned)tile_phys,
		       (unsigned)ring_bytes, a, (unsigned)info.surface_address, (unsigned)scratch_bytes, b);
		return;
	}
	ring = (const uint32_t *)((char *)a + (tile_phys & 4095));
	drawn = (const uint32_t *)((char *)b + (info.surface_address & 4095));
	strips_forget();
	strips_shown();
	pictures_now = 1;	/* (every picture decoded as its strip is made) */
	for (k = 0; k <= end; k++) {
		unsigned long here = 0;

		s = k % slot_count;
		while (slots[s].listed && regs[REG_DRAWN] == slots[s].drawn)
			mcw_rest();
		slots[s].strip = k;
		if (!strip_by_list(k) || !(slots[s].made = strip_job((uint32_t)k, (uint32_t)s))) {
			not_lists++;
			continue;
		}
		while (!strip_submit(s))
			mcw_rest();
		while ((int32_t)(regs[REG_COPIES] - slots[s].landed) < 0)
			mcw_rest();
		ui_fill(scratch, 0, 0, tile_w, STRIP, mw(web_page_bg));
		for (i = strip_at[k]; i < strip_at[k + 1]; i++)
			draw_item(scratch, &web_items[strip_items[i]], k * STRIP);
		GrReadArea(scratch, 0, 0, 1, 1, &pixel);	/* (answered once that is drawn) */
		for (i = 0; i < STRIP * tile_w; i++) {
			uint32_t p = ring[s * STRIP * tile_w + i], q = drawn[i];
			int most = 0, c;

			if (((p ^ q) & 0xffffff) == 0)
				continue;
			for (c = 0; c < 24; c += 8)
				most = abs((int)(p >> c & 255) - (int)(q >> c & 255)) > most ? abs((int)(p >> c & 255) - (int)(q >> c & 255)) : most;
			here++;
			far += most > 8;
		}
		made++;
		differ += here;
		if (here) {
			strips_differ++;
			if (strips_differ <= 8)
				printf("nxweb: check: strip %d (rows %d to %d): %lu pixels differ\n", k, k * STRIP, k * STRIP + STRIP, here);
		}
	}
	munmap(a, ring_bytes);
	munmap(b, scratch_bytes);
	pictures_now = 0;
	strips_forget();
	printf("nxweb: check of %s: %d strips as lists against requests, %d strips not lists; %d strips differ, %lu pixels, %lu by more than 8 levels\n",
	       address, made, not_lists, strips_differ, differ, far);
}

static const char *const menu_names[] = { "File", "Edit", "View", "Go", "Bookmarks", "Help" };
#define MENUS 6
static int menu_x[MENUS + 1];

static void
draw_menus(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < MENUS; i++) {
		if (i == menu_at)
			ui_fill(window, menu_x[i], 1, menu_x[i + 1] - menu_x[i], MENU_H - 3, UI_SELECTED);
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, i == menu_at ? WHITE : BLACK, 0);
	}
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

/* A key of the bar: back, forward, again (Stop while something is to come), home, bookmark. */
static void
draw_tool(int i)
{
	static const int signs[TOOLS] = { UI_G_LEFT, UI_G_RIGHT, UI_G_AGAIN, UI_G_HOME, UI_G_MARK };
	int how = i == 0 ? (history_at < 1) * 2 : i == 1 ? (history_at + 1 >= history_count) * 2 : i == 4 && bookmark_of(address) >= 0;

	ui_tool(window, 3 + i * (TOOL_W + 3), MENU_H + 3, TOOL_W, BAR - 6, i == 2 && shown_busy ? UI_G_STOP : signs[i], how);
}

/* While something is to come, the part of it here as a bar inside a field, or a block that
 * moves while that cannot be counted. */
static void
draw_meter(int x, int y, int w, int h, int colour)
{
	int b = w / 4, at = (spin * 24) % (w + b) - b;

	if (!shown_busy || w <= 0)
		return;
	if (shown_total > 0)
		ui_fill(window, x, y, (int)((long long)w * shown_done / shown_total), h, mw(colour));
	else
		ui_fill(window, x + (at < 0 ? 0 : at), y, (at + b > w ? w : at + b) - (at < 0 ? 0 : at), h, mw(colour));
}

/* The address, or the one being typed: its end, when the field does not hold it all. Behind
 * the address shown, what is loading (draw_meter). */
static void
draw_address(void)
{
	const char *text = editing ? typed : address;
	int x = FIELD_X, y = MENU_H + 3, w = width - x - 3, room = w - 12, tw;

	ui_fill(window, x, y, w, BAR - 6, WHITE);
	ui_bevel(window, x, y, w, BAR - 6, 1);
	if (!editing)
		draw_meter(x + 2, y + 2, w - 4, BAR - 10, 0xc4dcfa);
	/* and the same in a line across the window under the bar */
	ui_fill(window, 0, MENU_H + BAR - 3, width, 3, UI_FACE);
	draw_meter(0, MENU_H + BAR - 3, width, 3, 0x2f6fd8);
	while (editing && text[0] && ui_width(&ui_var_widths, ui_var, text, -1) > room)
		text++;
	tw = ui_width(&ui_var_widths, ui_var, text, -1);
	if (editing && offered)
		ui_fill(window, x + 3, y + 2, (tw < room ? tw : room) + 4, BAR - 10, UI_SELECTED);
	ui_text_fit(window, x + 5, y + 4, room, text, editing && offered ? WHITE : BLACK);
	if (editing && !offered)
		ui_fill(window, x + 5 + tw, y + 3, 1, BAR - 12, BLACK);
}

static void
draw_bar(void)
{
	int i;

	ui_fill(window, 0, MENU_H, width, BAR, UI_FACE);
	for (i = 0; i < TOOLS; i++)
		draw_tool(i);
	draw_address();
}

/* What the browser has to say, or what Find is being told, or what is loading; in the right
 * part how much of that is here, how much did not come, or how much of the page there was. */
static void
draw_status(void)
{
	char text[240], cell[40];
	int y = height - STATUS, w = width - STATUS_CELL - 4, colour = 0;

	if (finding)
		snprintf(text, sizeof text, "Find: %s_   (Enter finds, Escape leaves it)", sought);
	else if (shown_busy && (!status[0] || !strcmp(status, page_note) || !strncmp(status, "asking the host", 15)))
		snprintf(text, sizeof text, "%s", shown_text);
	else
		snprintf(text, sizeof text, "%s", status[0] ? status : "Done");
	if (shown_busy)
		snprintf(cell, sizeof cell, "%s", shown_cell);
	else if (shown_failed)
		snprintf(cell, sizeof cell, "%d not loaded", shown_failed), colour = 0xa00000;
	else
		snprintf(cell, sizeof cell, page_length < 10000 ? "%d bytes" : "%d KB", page_length < 10000 ? page_length : page_length / 1000);
	ui_fill(window, 0, y, width, STATUS, UI_FACE);
	ui_bevel(window, 1, y + 1, w, STATUS - 2, 1);
	ui_bevel(window, w + 2, y + 1, STATUS_CELL, STATUS - 2, 1);
	draw_meter(w + 4, y + 3, STATUS_CELL - 4, STATUS - 6, 0x80b4f0);
	ui_text_fit(window, 6, y + 3, w - 10, text, BLACK);
	ui_text_fit(window, w + 7, y + 3, STATUS_CELL - 10, cell, mw(colour));
}

/* Says something in the status line. */
static void
note(const char *text)
{
	snprintf(status, sizeof status, "%.199s", text);
	if (window)
		draw_status();
}

/* The view shows rows `top` on: its strips are made where they are not there yet, and the
 * view is a copy of the ring, two where it wraps. With no pixmap the rows are drawn on the window. */
static void
show_view(void)
{
	int view = view_height(), most = web_page_h - view, at, first;

	if (!window)
		return;
	top = top > most ? most : top;
	top = top < 0 ? 0 : top;
	if (!tile_make()) {
		draw_rows(window, top - TOP, top, top + view);
		draw_menus();
		draw_bar();
		draw_status();
	} else {
		strips_view();
		at = top % tile_h;
		first = tile_h - at < view ? tile_h - at : view;
		GrCopyArea(window, ui_gc, 0, TOP, view_width(), first, tile, 0, at, MWROP_COPY);
		if (first < view)
			GrCopyArea(window, ui_gc, 0, TOP + first, view_width(), view - first, tile, 0, 0, MWROP_COPY);
	}
	ui_scrollbar(window, width - UI_SCROLL_W, TOP, view, web_page_h, view, top);
	if (verbose && !page_shown) {
		struct timespec t;

		page_shown = 1;
		clock_gettime(CLOCK_MONOTONIC, &t);
		say_time("the page's first picture", page_began);
		printf("nxweb: the first picture %ld ms after the page's text; its pictures decoded in %u thousand instructions\n",
		       (long)((t.tv_sec - page_began_at.tv_sec) * 1000 + (t.tv_nsec - page_began_at.tv_nsec) / 1000000), pictures_decoding / 1000);
	}
}

static void
draw_all(void)
{
	draw_menus();
	draw_bar();
	draw_status();
	show_view();
}

static void
scroll_to(int y, int at_once)
{
	int most = web_page_h - view_height();

	y = y > most ? most : y;
	y = y < 0 ? 0 : y;
	goal = y;
	if (at_once && top != y) {
		top = y;
		show_view();
	}
}

/* A step of a scroll that is under way: a third of what is left, eight rows at least. NXWEB_TIME:
 * at its end a scroll says the longest gap between two steps by the host's clock (word 13) and in
 * the first core's instructions, a step's own most, and how many steps' copies were made before
 * the next (word 14 counts them: a step whose picture never got to the window was not seen). */
static void
scroll_step(void)
{
	static unsigned first_ms, last_ms, longest, copies, shown, steps, last_n, gap_most, own_most;
	int left = goal - top, step = left / 3;
	unsigned began = now();

	if (!left)
		return;
	if (abs(step) < 8)
		step = abs(left) < 8 ? left : left > 0 ? 8 : -8;
	top += step;
	show_view();
	if (verbose && regs) {
		unsigned ms = regs[13], made = regs[14], own = now() - began;

		if (!steps)
			first_ms = ms, longest = shown = gap_most = 0, own_most = own;
		else {
			longest = ms - last_ms > longest ? ms - last_ms : longest;
			gap_most = began - last_n > gap_most ? began - last_n : gap_most;
			own_most = own > own_most ? own : own_most;
			shown += made != copies;
		}
		last_ms = ms, copies = made, last_n = began, steps++;
		if (top == goal) {
			printf("nxweb: a scroll of %u steps in %u ms, %u ms the longest between two; %u of the %u before the last were on the screen by the next; "
			       "%u thousand instructions the longest between two, %u thousand the most a step of its own\n",
			       steps, ms - first_ms, longest, shown, steps - 1, gap_most / 1000, own_most / 1000);
			printf("nxweb: strips since the last scroll: %u by requests, %u lists made here, %u by workers; %u waits for the view's, %u for the window system\n",
			       strip_counts.requests, strip_counts.here, strip_counts.workers, strip_counts.waits, strip_counts.syncs);
			memset(&strip_counts, 0, sizeof strip_counts);
			steps = 0;
		}
	}
}

/* ---- pages ---- */

static void
say_time(const char *what, unsigned began)
{
	if (verbose)
		printf("nxweb: %s %u thousand instructions\n", what, (now() - began) / 1000);
}

/* ---- styles on the worker cores ---- */

/* A worker's count of the names round the element it styles (css_inside is the first core's). */
static unsigned short style_inside[STRIP_WORKERS + 1][CSS_ATOMS] __attribute__((aligned(16)));
static int style_out[STRIP_WORKERS + 1], styles_touched;
static unsigned styles_away;

static uint32_t
style_job(uint32_t root, uint32_t end)
{
	web_style_range(style_inside[mcw_self()], (int)root, (int)end);
	return 0;
}

/* web_style_elsewhere: an element's inside to a worker that has nothing to do, or no. */
static int
styles_elsewhere(int root, int end)
{
	int k;

	if (root < 0) {
		for (k = 1; k <= strip_workers; k++)
			if (style_out[k])
				mcw_wait(k), style_out[k] = 0;
		styles_touched = 0;
		return 1;
	}
	if (strips_open() <= 0)
		return 0;
	for (k = 1; k <= strip_workers && style_out[k] && !mcw_done(k); k++)
		;
	if (k > strip_workers)
		return 0;
	if (!styles_touched) {
		/* (the styles are stored to by the workers: their pages here first) */
		mcw_touch(web_styles, (unsigned)(web_style_count * sizeof *web_styles));
		styles_touched = 1;
	}
	style_out[k] = 1;
	styles_away += end - root - 1;
	mcw_post(k, style_job, (uint32_t)root, (uint32_t)end);
	return 1;
}

/* NXWEB_CHECK: the styles as the workers had a part in them, against all made on this core. */
static void
styles_check(void)
{
	struct css_style *had = malloc(web_style_count * sizeof *had);
	struct web_node *nodes = malloc(web_node_count * sizeof *nodes);
	int i, differ = 0;

	if (!had || !nodes)
		return;
	memcpy(had, web_styles, web_style_count * sizeof *had);
	memcpy(nodes, web_nodes, web_node_count * sizeof *nodes);
	web_style_elsewhere = NULL;
	web_style_all();
	web_style_elsewhere = styles_elsewhere;
	for (i = 0; i < web_node_count; i++)
		differ += web_nodes[i].c.tag != T_TEXT && (memcmp(&had[web_nodes[i].style], &web_styles[web_nodes[i].style], sizeof *had) ||
							   nodes[i].flags != web_nodes[i].flags);
	printf("nxweb: check of %s: %d elements styled (%u of the tree's %d nodes on workers); %d differ from this core's\n", address,
	       web_style_count, styles_away, web_node_count, differ);
	free(had);
	free(nodes);
}

/* Lays the page out again: its sheets read (0: they are as they were), its tree made, its
 * styles worked out, each only when something it comes from changed. */
static void
lay_out(int sheets, int tree)
{
	unsigned began = now();
	int i, texts = 0, chars = 0, boxes = 0, images = 0, items = 0;

	strips_forget();	/* (the jobs out read the tree and the items) */
	if (sheets) {
		web_read_sheets(view_width(), view_height());
		say_time("sheets", began);
	}
	if (sheets || tree) {
		began = now();
		web_parse(page, page_length);
		say_time("tree", began);
		began = now();
		styles_away = 0;
		web_style_all();
		say_time("styles", began);
		if (verbose)
			printf("nxweb: %u of the tree's %d nodes styled on workers\n", styles_away, web_node_count);
		if (getenv("NXWEB_CHECK"))
			styles_check();
	}
	began = now();
	for (i = 0; i < picture_count; i++)
		pictures[i].sized = 1;
	web_layout(view_width());
	say_time("layout", began);
	found = -1;
	strips_index();
	pictures_relaid();
	if (dump) {
		for (i = 0; i < web_item_count; i++) {
			const struct web_item *it = &web_items[i];

			texts += it->kind == I_TEXT, boxes += it->kind == I_BOX, images += it->kind == I_IMAGE, items += it->kind == I_FIELD;
			chars += it->kind == I_TEXT ? it->b : 0;
		}
		printf("nxweb: %s: %d nodes, %d rules, %d x %d; %d texts (%d characters), %d boxes, %d pictures, %d fields\n", address,
		       web_node_count, css_rule_count, view_width(), web_page_h, texts, chars, boxes, images, items);
	}
}

static void
set_title(void)
{
	char caption[160];

	if (!window)
		return;
	snprintf(caption, sizeof caption, "%s%s - Web", shown_busy == 1 ? "Loading: " : "", web_title[0] ? web_title : address);
	GrSetWindowTitle(window, caption);
}

/* Where the element with an id is on the page, or -1. */
static int
anchor(const char *id)
{
	const char *value;
	int i, n, m = strlen(id);

	for (i = 0; m && i < web_node_count; i++)
		if (web_nodes[i].c.tag != T_TEXT && (web_attr(i, "id", &value, &n) || (web_nodes[i].c.tag == T_a && web_attr(i, "name", &value, &n))) &&
		    n == m && !strncmp(value, id, n))
			return web_nodes[i].y;
	return -1;
}

/* Shows a page whose text is the caller's allocation of n + 1 bytes or more: it is kept. */
static void
show_kept(char *html, int n, const char *note)
{
	const char *hash = strchr(address, '#');
	int i;

	unsigned began = now();

	page_began = began;
	clock_gettime(CLOCK_MONOTONIC, &page_began_at);
	page_shown = 0;
	free(page);
	page = html;
	page_length = n;
	free(raw);
	raw = raw_next;
	raw_n = raw_next_n;
	raw_next = NULL;
	top = goal = 0;
	picked = focus = -1;
	/* another page: the last one's pictures and what was typed go */
	pictures_drop();
	for (i = 0; i < field_count; i++)
		free(fields[i].text);
	field_count = 0;
	asking = -1;
	snprintf(web_address, sizeof web_address, "%s", address);
	web_find_sheets(page, page_length, view_width());
	memset(sheet_tracked, 0, sizeof sheet_tracked);
	failed_told = failed_due = 0;
	for (i = 0; i < web_sheet_count; i++) {
		struct web_sheet *sh = &web_sheets[i];
		char file[80];
		FILE *f;

		if (sh->state != SHEET_WANTED)
			continue;
		snprintf(sheet_why[i], sizeof sheet_why[0], "%s", sh->address[0] == '/' ? "not found" : "no network");
		/* one in the image, or one the image keeps a copy of, is read at once */
		if (sh->address[0] == '/')
			snprintf(file, sizeof file, "%.79s", sh->address);
		else if (cached_file(sh->address))
			snprintf(file, sizeof file, "%s", cached_file(sh->address));
		else if (!web_cache_find(sh->address, 'S', file, sizeof file, NULL, NULL))
			file[0] = 0;
		sh->state = regs && sh->address[0] != '/' ? SHEET_WANTED : SHEET_FAILED;
		if (file[0] && (f = fopen(file, "rb")) != NULL) {
			if ((sh->text = malloc(SHEET_MOST)) != NULL) {
				sh->length = fread(sh->text, 1, SHEET_MOST, f);
				sh->own = 1;
				sh->state = SHEET_HERE;
			}
			fclose(f);
		}
		sheet_tracked[i] = sh->state == SHEET_WANTED;
	}
	say_time("finding the sheets", began);
	lay_out(1, 1);
	if (getenv("NXWEB_AGAIN"))
		lay_out(1, 1);	/* (what a page costs once the program's own pages are in memory) */
	snprintf(status, sizeof status, "%s", note);
	snprintf(page_note, sizeof page_note, "%s", status);
	snprintf(history[history_at].title, sizeof history[0].title, "%.59s", web_title);
	set_title();
	if (hash && (i = anchor(hash + 1)) >= 0)
		top = goal = i;
	if (top_wanted >= 0) {
		/* (back to a page, or the rest of one that was shown as it came) */
		top = goal = top_wanted > web_page_h - view_height() ? web_most(web_page_h - view_height(), 0) : top_wanted;
		top_wanted = -1;
	}
	check_due = getenv("NXWEB_CHECK") != NULL;
	if (window)
		draw_all();
}

static void
show(const char *html, int n, const char *note)
{
	char *copy = malloc(n + 1);

	memcpy(copy, html, n);
	copy[n] = 0;
	show_kept(copy, n, note);
}

/* A text as HTML's text. One at a time. */
static const char *
esc(const char *text)
{
	static char out[1000];
	int n = 0;

	for (; *text && n < (int)sizeof out - 7; text++)
		if (*text == '<' || *text == '>' || *text == '&' || *text == '"')
			n += sprintf(out + n, "&#%d;", *text);
		else
			out[n++] = *text;
	out[n] = 0;
	return out;
}

/* The browser's own page: what happened, the address again if it is one to try again, and home. */
static void
say(const char *heading_text, const char *text)
{
	char html[2400], again[700] = "";
	int n;

	if (strstr(address, "://"))
		snprintf(again, sizeof again, "<a href=\"%s\">Try again</a> &nbsp; ", esc(address));
	n = snprintf(html, sizeof html, "<html><head><title>%s</title></head><body style=\"font-family:sans-serif\"><h2>%s</h2><p>%s</p><p>%s<a href=\"%s%s\">Home</a></p>",
		     heading_text, heading_text, text, again, home[0] == '/' ? "file://" : "", home);
	show(html, n, "");
	page_ours = 1;
}

static void request(const char *what, int kind);

/* A picture is here: the page laid out for it if it could not be before, else its rows made again. */
static void
picture_arrived(struct picture *p, int k)
{
	int i;

	if (!p->sized)
		lay_out(0, 0);
	else
		for (i = 0; i < web_item_count; i++)
			if (web_items[i].kind == I_IMAGE && web_items[i].a == k)
				strips_forget_rows(web_items[i].y, web_items[i].y + web_items[i].h);
	show_view();
}

/* Asks the host for what is at an address, a page (kind 0), a picture (1) or a sheet (2), from
 * a byte of it on: it sees the address and the request's number, and answers when it has it. */
static void
ask_part(const char *what, int kind, uint32_t offset)
{
	const char *hash = strchr(what, '#');
	int n = hash ? (int)(hash - what) : (int)strlen(what);

	memset((void *)&regs[REG_ADDRESS], 0, 256);
	memcpy((void *)&regs[REG_ADDRESS], what, n);
	regs[REG_REQUEST + 1] = n;
	regs[REG_REQUEST + 2] = kind;
	regs[REG_REQUEST + 3] = offset;
	loading = regs[REG_REQUEST] + 1;
	if (!loading)
		loading = 1;
	regs[REG_REQUEST] = loading;
	waited = 0;
}

static void
ask(const char *what, int kind)
{
	part_have = part_total = 0;
	ask_part(what, kind, 0);
}

static int
read_file(const char *path, const char *note)
{
	FILE *file = fopen(path, "rb");
	char *text;
	int n;

	if (!file)
		return 0;
	text = malloc(PAGE_MOST + 1);
	n = fread(text, 1, PAGE_MOST, file);
	text[n] = 0;
	fclose(file);
	show_kept(text, n, note);
	return 1;
}

/* Help's page (about:keys). */
static const char keys_page[] =
	"<html><head><title>The browser's keys</title></head><body style=\"font-family:sans-serif\"><h2>The browser's keys</h2>"
	"<table cellpadding=3>"
	"<tr><td><b>Ctrl+L</b>, <b>F6</b><td>the address: type one and press Enter (Escape leaves it)"
	"<tr><td><b>Alt+Left</b>, <b>Backspace</b><td>back"
	"<tr><td><b>Alt+Right</b><td>forward"
	"<tr><td><b>Alt+Home</b><td>the home page (View, Options says which)"
	"<tr><td><b>F5</b>, <b>Ctrl+R</b><td>the page again, from the network"
	"<tr><td><b>Escape</b><td>stops what is still to come"
	"<tr><td><b>Ctrl+D</b><td>a bookmark for this page, or none any more"
	"<tr><td><b>Ctrl+F</b>, <b>F3</b><td>find in the page, and the next place"
	"<tr><td><b>Ctrl+O</b>, <b>Ctrl+S</b><td>open a file, save the page as one"
	"<tr><td><b>Ctrl+U</b><td>the page's source, in the editor"
	"<tr><td><b>Ctrl+C</b>, <b>Ctrl+V</b><td>the address to the clipboard, an address from it"
	"<tr><td><b>Tab</b>, <b>Enter</b><td>from link to link, and follow the one picked"
	"<tr><td>arrows, <b>Page Up</b>, <b>Page Down</b>, <b>Space</b>, <b>Home</b>, <b>End</b><td>scroll"
	"<tr><td><b>F10</b><td>the menus: arrows, Enter, Escape"
	"<tr><td><b>Ctrl+Q</b><td>close"
	"</table><p>The right button on a link or on the page has a menu of its own. "
	"Bookmarks and options are kept in the registry, which lasts when the machine's storage is saved.</p>";

/* ---- over this machine's own sockets (web_net.h, docs/apps/web.md) ---- */

/* The window system's wait looks at the socket once there is something to read from it. Before a
 * socket that was closed is waited on: a wait with a closed one in its set says only that. */
static void
net_watch(void)
{
	int fd = net.state >= NET_HEAD && net.state < NET_DONE ? net.fd : -1;

	if (fd == net_watched)
		return;
	if (net_watched >= 0)
		GrUnregisterInput(net_watched);
	if (fd >= 0)
		GrRegisterInput(fd);
	net_watched = fd;
}

/* What is out is given up, and what it was writing goes. */
static void
net_cancel(void)
{
	int was = net.state && net.state < NET_DONE;

	net_forget(&net);
	net_watch();
	if (was && net_file[0])
		unlink(net_file);
	net_file[0] = 0;
	net_sink = 0;
}

/* Why what was asked for will not come. */
static void
net_failed(const char *why)
{
	char text[700];

	loading = 0;
	if (asking >= SHEET)
		sheet_fail(asking - SHEET, why);
	else if (asking >= 0)
		picture_fail(&pictures[asking], why);
	else {
		if (dump) {
			printf("nxweb: failed: %s: %s\n", address, why);
			fflush(stdout);
		}
		snprintf(text, sizeof text, "<b>%s</b><br><br>%s", esc(address), why);
		say("This page could not be opened", text);
	}
	asking = -1;
}

/* Asks for what is at an address, over this machine's sockets when it can reach it, else of the
 * host; in the end a page, or a picture's or a sheet's `asking`, as check_reply has them. */
static void
request(const char *what, int kind)
{
	char why[200];
	uint32_t ip;
	int route = net_route(what, &ip, why, sizeof why);
	int post = kind == 0 && net_posting;

	if (route == ROUTE_NONE) {
		net_failed(why);
		return;
	}
	if (route == ROUTE_HOST) {
		if (post)
			net_failed("A form is posted only to machines this one reaches itself: the host's fetch asks for addresses, and sends nothing.");
		else if (!regs)
			net_failed("This machine has no way to ask its host for it.");
		else
			ask(what, kind);
		return;
	}
	net_kind = kind;
	net_sink = 0;
	net_shown = 0;
	net_file[0] = 0;
	part_have = part_total = 0;
	net_start(&net, what, ip, post ? "POST" : NULL, post ? history[history_at].post : NULL, post ? history[history_at].post_n : 0,
		  post ? history[history_at].post_type : NULL);
	loading = -1;
	waited = 0;
}

/* The name to save an answer under: what it says, or the end of its address, in /root/Downloads. */
static void
save_name(char *file, int room)
{
	char name[64] = "";
	const char *at = strcasestr(net.disposition, "filename="), *slash;
	int i, k, tried;
	struct stat st;

	if (at) {
		at += 9 + (at[9] == '"');
		snprintf(name, sizeof name, "%.*s", (int)strcspn(at, "\";"), at);
	} else if ((at = strstr(net.address, "://")) != NULL && (slash = strchr(at + 3, '/')) != NULL) {
		const char *end = slash + strcspn(slash, "?#");

		while (end > slash && end[-1] != '/')
			end--;
		snprintf(name, sizeof name, "%.*s", (int)strcspn(end, "?#"), end);
	}
	for (i = k = 0; name[i]; i++)
		if (isalnum((unsigned char)name[i]) || (name[i] && strchr("._-", name[i]) && (k || name[i] != '.')))
			name[k++] = name[i];
	name[k] = 0;
	if (!k)
		strcpy(name, "download");
	mkdir("/root/Downloads", 0755);
	for (tried = 0; tried < 1000; tried++) {
		if (tried)
			snprintf(file, room, "/root/Downloads/%d-%s", tried, name);
		else
			snprintf(file, room, "/root/Downloads/%s", name);
		if (stat(file, &st))
			return;
	}
}

/* The head has come: where the body goes. */
static void
net_headed(void)
{
	if (net.code >= 300 && net.code < 400 && net.code != 304 && net.location[0]) {
		net_end(&net, NET_DONE);	/* (a redirect's own text is not wanted) */
		return;
	}
	if (net_kind == 2) {
		net_body(&net, NULL, SHEET_MOST);
	} else if (net_kind == 1) {
		if (net.code != 200) {
			net_end(&net, NET_DONE);
			return;
		}
		cached_name(net_file, sizeof net_file);
		net_body(&net, net_file, 32 << 20);
	} else {
		int kind = net_kind_of(&net);

		if (net.code == 200 && kind == NK_IMAGE) {
			net_sink = 1;
			cached_name(net_file, sizeof net_file);
			net_body(&net, net_file, 32 << 20);
		} else if (net.code == 200 && kind == NK_SAVE) {
			net_sink = 2;
			save_name(net_file, sizeof net_file);
			net_body(&net, net_file, 64 << 20);
		} else
			net_body(&net, NULL, PAGE_MOST);
	}
}

static void
net_pump(void)
{
	if (net.state && net.state < NET_DONE && net_poll(&net) == NET_HEADED)
		net_headed();
	net_watch();
}

/* What came of a page, as the page to show: its own bytes if they are HTML, else as a listing. */
static char *
net_page(const char *from, int n, int kind, int *size)
{
	char *html;
	int i, k;

	if (kind == NK_HTML) {
		html = malloc(n + 1);
		memcpy(html, from, n);
		html[n] = 0;
		*size = n;
		return html;
	}
	html = malloc(strlen(address) * 6 + 256 + n * 5 + 1);
	k = sprintf(html, "<html><head><title>%s</title></head><body><pre style=\"white-space:pre-wrap;font-family:monospace\">", esc(address));
	for (i = 0; i < n; i++) {
		char c = from[i];

		if (c == '<')
			k += sprintf(html + k, "&lt;");
		else if (c == '&')
			k += sprintf(html + k, "&amp;");
		else if (c != '\r')
			html[k++] = c;
	}
	k += sprintf(html + k, "</pre>");
	*size = k;
	raw_next = malloc(n + 1);
	if (raw_next) {
		memcpy(raw_next, from, n);
		raw_next_n = n;
	}
	return html;
}

/* The page as far as it came. `note` for the status line. */
static void
net_show(const char *note)
{
	int kind = net_kind_of(&net), size;
	char *html;

	if (kind == NK_SNIFF)
		kind = net_sniff(net.body, net.n);
	snprintf(web_http_charset, sizeof web_http_charset, "%s", net.charset);
	html = net_page(net.body, net.n, kind == NK_HTML ? NK_HTML : NK_TEXT, &size);
	if (net_shown)
		top_wanted = top;
	show_kept(html, size, note);
}

/* An answer whose head said nothing of its kind and whose bytes are not text, or one to be saved:
 * it is a file now, and the page that was shown stays. */
static void
net_saved(long long bytes)
{
	char text[500];
	int back = history_at > 0 && page_length;

	if (dump) {
		printf("nxweb: saved %s as %s, %lld bytes\n", net.address, net_file, bytes);
		fflush(stdout);
	}
	if (back) {
		history_count = history_at--;
		snprintf(address, sizeof address, "%s", history[history_at].address);
		snprintf(status, sizeof status, "saved as %.100s, %lld bytes", net_file, bytes);
		snprintf(page_note, sizeof page_note, "%s", status);
		set_title();
		draw_bar();
		draw_status();
		return;
	}
	snprintf(text, sizeof text, "%lld bytes, of a kind this browser does not show, are saved as <b>%s</b>.", bytes, net_file);
	say("Saved", text);
}

/* A form's or a redirect's address made whole against another than the page's. */
static int
resolve_from(const char *base, const char *href, char *whole)
{
	char a[256], b[256];
	int fine;

	strcpy(a, web_address);
	strcpy(b, web_base);
	snprintf(web_address, sizeof web_address, "%s", base);
	web_base[0] = 0;
	fine = web_resolve(href, whole);
	strcpy(web_address, a);
	strcpy(web_base, b);
	return fine;
}

/* The answer is all here, or `stopped` takes it for that: what it was asked for gets it. */
static void
net_finish(int stopped)
{
	char note[240], whole[256];

	loading = 0;
	net_watch();
	if (net.state == NET_FAILED) {
		char why[160];

		if (net.no_name && regs && !(net_kind == 0 && net_posting)) {
			/* no name server here knows it: the host may */
			strcpy(whole, net.address);
			net_forget(&net);
			ask(whole, net_kind);
			return;
		}
		snprintf(why, sizeof why, "%s", net.why);
		if (net_file[0])
			unlink(net_file);
		net_file[0] = 0;
		net_failed(why);
		net_forget(&net);
		return;
	}
	if (dump) {
		printf("nxweb: http: %s: %d%s%s, %lld bytes%s%s\n", net.address, net.code, net.type[0] ? " " : "", net.type, net.got,
		       net.chunked ? ", chunked" : "", net.short_ended ? ", cut short" : "");
		fflush(stdout);
	}
	if (net.code >= 300 && net.code < 400 && net.code != 304 && net.location[0]) {
		if (++net_hops > 8) {
			net_forget(&net);
			net_failed("It sends its reader on and on: eight redirects in a row.");
			return;
		}
		if (!resolve_from(net.address, net.location, whole)) {
			net_forget(&net);
			net_failed("It sends its reader to an address that is none, or too long.");
			return;
		}
		if (net.code != 307 && net.code != 308)
			net_posting = 0;
		net_forget(&net);
		if (net_kind == 0) {
			/* the address is where the page came from: what its links are made whole with */
			snprintf(address, sizeof address, "%s", whole);
			strcpy(history[history_at].address, whole);
			if (window)
				draw_bar();
		}
		if (whole[0] == '/')
			net_failed("It sends its reader to a file of this computer.");
		else
			request(whole, net_kind);
		return;
	}
	if (net_kind == 2) {
		struct web_sheet *sh = &web_sheets[asking - SHEET];

		if (net.code != 200 || net.n <= 0) {
			why_of(note, sizeof note, net.code == 200 ? 204 : net.code);
			sheet_fail(asking - SHEET, net.code == 200 ? "empty" : note);
			asking = -1;
			net_forget(&net);
			return;
		}
		asking = -1;
		if (!net.no_store) {
			/* (kept for the site's next page) */
			char name[40];
			FILE *f;

			cached_name(name, sizeof name);
			if ((f = fopen(name, "wb")) != NULL) {
				fwrite(net.body, 1, net.n, f);
				fclose(f);
				cached_add(sh->address, name, net.n, 0);
			}
		}
		sh->text = net.body;
		sh->length = net.n;
		sh->own = 1;
		sh->state = SHEET_HERE;
		net.body = NULL;
		net_forget(&net);
		lay_out(1, 1);
		show_view();
		return;
	}
	if (net_kind == 1) {
		struct picture *p = &pictures[asking];
		int k = asking;

		asking = -1;
		p->asked = time(NULL);
		if (net.code != 200) {
			why_of(note, sizeof note, net.code);
			picture_fail(p, note);
		} else {
			cached_add(p->address, net_file, (int)net.got, net.no_store);
			p->ours = 0;
			p->file[0] = p->source[0] = 0;
			picture_from(p, net_file);
			if (p->state == PICTURE_HERE)
				picture_arrived(p, k);
		}
		net_forget(&net);
		net_file[0] = 0;
		return;
	}
	if (net_sink == 2) {
		if (net.short_ended || stopped) {
			unlink(net_file);
			net_failed(stopped ? "The file was being saved, and the saving was stopped." : "The file ended before its end: nothing was saved.");
		} else
			net_saved(net.got);
		net_forget(&net);
		net_file[0] = 0;
		net_sink = 0;
		return;
	}
	if (net_sink == 1) {
		/* a picture is the page */
		char html[800];
		int n = snprintf(html, sizeof html, "<html><head><title>%s</title></head><body><img src=\"%s\">", esc(address), esc(address));

		cached_add(address, net_file, (int)net.got, 0);
		snprintf(note, sizeof note, "a picture, %lld bytes", net.got);
		net_forget(&net);
		net_file[0] = 0;
		net_sink = 0;
		show(html, n, note);
		return;
	}
	if (net.code >= 400 && !net.n) {
		char text[600];

		snprintf(text, sizeof text, "<b>%s</b><br><br>The server said %d %s.", esc(address), net.code, net.reason);
		net_forget(&net);
		say("This page could not be opened", text);
		return;
	}
	if (net_shown && net_shown == net.n) {
		/* (all of it was on the screen already) */
		snprintf(status, sizeof status, "%d bytes from %.100s", net.n, net.host);
		snprintf(page_note, sizeof page_note, "%s", status);
		if (window)
			draw_status();
		net_forget(&net);
		return;
	}
	if (net_kind_of(&net) == NK_SNIFF && net.n && net_sniff(net.body, net.n) == NK_SAVE) {
		FILE *f;

		save_name(net_file, sizeof net_file);
		if ((f = fopen(net_file, "wb")) != NULL) {
			fwrite(net.body, 1, net.n, f);
			fclose(f);
		}
		net_saved(net.n);
		net_forget(&net);
		net_file[0] = 0;
		return;
	}
	if (net.code == 200 && !net.n && !stopped) {
		char text[600];

		snprintf(text, sizeof text, "<b>%s</b><br><br>The server answered, and sent nothing.", esc(address));
		net_forget(&net);
		say("An empty page", text);
		return;
	}
	if (!net.n) {
		net_forget(&net);
		return;
	}
	snprintf(note, sizeof note, "%s%.32s%s%d bytes from %.100s%s%s%s", net.code == 200 ? "" : "the server said ", net.code == 200 ? "" : net.reason, net.code == 200 ? "" : ": ", net.n, net.host,
		 net.cut ? " (cut short here)" : "", net.short_ended ? " (the connection ended early)" : "", stopped ? " (stopped)" : "");
	net_show(note);
	net_forget(&net);
}

/* While a request of ours is out: as far as it has got, and the page as it comes. */
static void
net_check(void)
{
	net_pump();
	if (net.state >= NET_DONE) {
		net_finish(0);
		return;
	}
	/* a page that takes its time is shown as far as it is here, once; the rest when it ends */
	if (net_kind == 0 && !net_sink && net.state == NET_BODY && net.n > 0 && !net_shown && net_ms() - net.head_at > 1500 &&
	    (net_kind_of(&net) != NK_SNIFF || net_sniff(net.body, net.n) != NK_SAVE)) {
		net_shown = net.n;
		net_show("still loading");
	}
}

/* Goes to an address. `how`: 1 remembers the page left behind, 2 asks again rather than show a
 * copy kept. A new page's form to send is in post_next. */
static void
go(const char *where, int how)
{
	char file[80], plain[256], why[200], next[256];
	char *hash;
	uint32_t ip;
	int route, i;

	if (how)
		sent_on = 0;
	snprintf(next, sizeof next, "%s", strncmp(where, "file://", 7) ? where : where + 7);
	net_cancel();
	net_hops = 0;
	net_posting = 0;
	web_http_charset[0] = 0;
	page_ours = 0;
	if (how == 1 && history_count) {
		/* a new page: the ones Forward would have gone to are forgotten */
		history[history_at].top = top;
		for (i = history_at + 1; i < history_count; i++) {
			free(history[i].post);
			history[i].post = NULL;
		}
		history_count = history_at + 1;
		if (history_count == HISTORY) {
			free(history[0].post);
			memmove(&history[0], &history[1], sizeof history[0] * --history_count);
			history[history_count].post = NULL;
		}
		history_at = history_count++;
		history[history_at].title[0] = 0;
	}
	history_count += !history_count;
	if (how == 1 || strcmp(history[history_at].address, next)) {
		/* (this entry no longer is that form's answer) */
		free(history[history_at].post);
		history[history_at].post = post_next;
		history[history_at].post_n = post_next_n;
		snprintf(history[history_at].post_type, sizeof history[0].post_type, "%s", post_next_type);
		post_next = NULL;
	}
	snprintf(address, sizeof address, "%s", next);
	strcpy(history[history_at].address, address);
	editing = finding = loading = by_hand = 0;
	picked = -1;
	if (part_file)
		fclose(part_file);
	part_file = NULL;
	snprintf(plain, sizeof plain, "%s", address);
	if ((hash = strchr(plain, '#')) != NULL)
		*hash = 0;
	if (!strncmp(address, "about:", 6)) {
		show(keys_page, sizeof keys_page - 1, "");
		return;
	}
	if (address[0] == '/') {
		if (!read_file(plain, "from this computer")) {
			char text[600];

			snprintf(text, sizeof text, "%s", esc(address));
			say("No such page", text);
		}
		return;
	}
	asking = -1;
	if (history[history_at].post && how != 1 && !post_again) {
		/* back to, or Reload of, the answer to a form: it is not sent twice unasked */
		char text[800];

		snprintf(text, sizeof text, "<b>%s</b> is what a form sent answered. Going back to it, or loading it again, sends that form again. "
			 "<a href=\"about:resend\">Send the form again</a>", esc(address));
		say("A form's answer", text);
		return;
	}
	net_posting = history[history_at].post != NULL;
	post_again = 0;
	route = net_route(plain, &ip, why, sizeof why);
	if (route == ROUTE_DIRECT) {
		if (how == 2)
			for (i = 0; i < cached_count; i++)
				cached[i].gone = 1;	/* (Reload: the sheets and pictures from their server too) */
		request(plain, 0);
		snprintf(status, sizeof status, "connecting to %.150s ...", address);
		if (window) {
			draw_bar();
			draw_status();
		}
		return;
	}
	if (how != 2 && route == ROUTE_HOST && !net_posting && web_cache_find(plain, 'P', file, sizeof file, NULL, NULL) && read_file(file, "a copy this computer keeps (Reload asks the network)"))
		return;
	if (!regs && route == ROUTE_HOST && !net_posting) {
		say("No network", "This machine has no way to ask its host for a page.");
		return;
	}
	request(address, 0);
	if (!loading)
		return;
	snprintf(status, sizeof status, "asking the host for %.150s ...", address);
	if (window) {
		draw_bar();
		draw_status();
	}
}

/* Text that comes in parts (a page, a sheet): true when all that is wanted of it is in `part`. */
static int
part_text(const char *what, int kind, int total, int most)
{
	int here;

	if (total > most)
		total = most;
	part_total = total;
	here = total - part_have < FETCH_MOST ? total - part_have : FETCH_MOST;
	if (!part_have)
		part = malloc(total + 1);
	if (!part || here <= 0)
		return 1;
	memcpy(part + part_have, fetched, here);
	part_have += here;
	part[part_have] = 0;
	if (part_have >= total)
		return 1;
	ask_part(what, kind, (uint32_t)part_have);
	return 0;
}

static void
by_hand_note(void)
{
	if (!strstr(status, "by the screen")) {
		snprintf(status, sizeof status, "styles and pictures need their addresses pasted: the button by the screen");
		draw_status();
	}
}

/* Called while a request is out: shows the answer once the host has put it in place. */
static void
check_reply(void)
{
	char note[80];
	int n, code;

	if (net.state) {
		net_check();
		return;
	}
	if ((int)regs[REG_REPLY] != loading) {
		if ((waited += ticked) > 300) {	/* (tenths of a second: other events are not time) */
			loading = 0;
			if (asking >= SHEET)
				sheet_fail(asking - SHEET, "no answer in 30 s");
			else if (asking >= 0)
				picture_fail(&pictures[asking], "no answer in 30 s");
			else if (!by_hand)
				say("No answer", "The host did not answer in 30 seconds.");
			if (part_file)
				fclose(part_file);
			part_file = NULL;
			asking = -1;
		}
		return;
	}
	loading = 0;
	n = regs[REG_REPLY + 1];
	code = regs[REG_REPLY + 2];
	if (asking >= SHEET) {
		struct web_sheet *sh = &web_sheets[asking - SHEET];

		sh->state = code == 1 ? SHEET_BY_HAND : SHEET_WANTED;
		sh->asked = time(NULL);
		if (code == 1)
			by_hand_note();
		else if (code != 200 || n <= 0) {
			why_of(note, sizeof note, code == 200 ? 204 : code);
			sheet_fail(asking - SHEET, code == 200 ? "empty" : note);
		}
		if (code != 200 || n <= 0 || !part_text(sh->address, 2, n, SHEET_MOST)) {
			if (code != 200 || n <= 0)
				asking = -1;
			return;	/* (or more of it to come) */
		}
		asking = -1;
		if (!part)
			return;
		sh->text = part;
		sh->length = part_have;
		sh->own = 1;
		sh->state = SHEET_HERE;
		part = NULL;
		lay_out(1, 1);
		show_view();
		return;
	}
	if (asking >= 0) {
		struct picture *p = &pictures[asking];
		int w = regs[REG_REPLY + 3] & 0xffff, h = regs[REG_REPLY + 3] >> 16, i, here;
		FILE *file = part_file;

		part_file = NULL;
		p->state = code == 1 ? PICTURE_BY_HAND : PICTURE_WANTED;
		p->asked = time(NULL);
		if (code == 1)
			by_hand_note();
		if (code != 200 || w <= 0 || h <= 0 || (file && (w != part_w || h != part_h)) ||
		    (!file && (file = fopen(p->file, "wb")) == NULL)) {
			if (code == 200)
				snprintf(note, sizeof note, w <= 0 || h <= 0 ? "no pixels came" : file ? "it changed while it came" : "cannot be written to /tmp");
			else
				why_of(note, sizeof note, code);
			if (code != 1)
				picture_fail(p, note);
			if (file)
				fclose(file);
			asking = -1;
			return;
		}
		/* a word a pixel, 0x00RRGGBB, kept as a PPM file; 65,536 pixels a part */
		if (!ftell(file)) {
			fprintf(file, "P6\n%d %d\n255\n", w, h);
			part_done = 0;
			part_w = w;
			part_h = h;
		}
		here = w * h - part_done < FETCH_MOST / 4 ? w * h - part_done : FETCH_MOST / 4;
		for (i = 0; i < here; i++) {
			uint32_t c = ((const uint32_t *)fetched)[i];

			fputc(c >> 16 & 255, file);
			fputc(c >> 8 & 255, file);
			fputc(c & 255, file);
		}
		part_done += here;
		if (part_done < w * h) {
			part_file = file;
			ask_part(p->address, 1, (uint32_t)part_done * 4);
			return;
		}
		fclose(file);
		asking = -1;
		/* (decoded to the size it is drawn when it is first drawn, as the image's are) */
		memcpy(p->source, p->file, sizeof p->source);
		p->w = w;
		p->h = h;
		p->state = PICTURE_HERE;
		picture_arrived(p, (int)(p - pictures));
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

		snprintf(text, sizeof text, "The host could not get %.180s (%d).%s", esc(address), code,
			 code == 3 ? " VRChat opens only addresses that begin with https, and this machine has no way to it itself." : "");
		say("Page not loaded", text);
		return;
	}
	if (!part_text(address, 0, n, PAGE_MOST))
		return;
	snprintf(note, sizeof note, "%d bytes from the host%s", part_have, n > PAGE_MOST ? " (cut short)" : "");
	n = part_have;
	show_kept(part, n, note);
	part = NULL;
}

static void
follow(const char *href)
{
	char whole[256];
	int y;

	if (href[0] == '#') {
		if ((y = href[1] ? anchor(href + 1) : 0) >= 0)
			scroll_to(y, 0);
		return;
	}
	if (!strcmp(href, "about:resend")) {
		post_again = 1;
		strcpy(whole, address);
		go(whole, 0);
		return;
	}
	if (!web_resolve(href, whole))
		return;
	/* (a page from elsewhere does not open this computer's files) */
	if (whole[0] == '/' && strstr(address, "://") && !page_ours) {
		note("a page from the network may not open this computer's files");
		return;
	}
	go(whole, 1);
}

/* The sheet or picture to ask the host for next, or -1: the first three it does not have yet,
 * in the page's order (the host has room for three), one that waits for a visitor once a second. */
static int
next_wanted(void)
{
	time_t now = time(NULL);
	int i, open = 0;

	/* style sheets first: one changes the whole page */
	for (i = 0; i < web_sheet_count && open < 3; i++) {
		if (web_sheets[i].state != SHEET_WANTED && web_sheets[i].state != SHEET_BY_HAND)
			continue;
		open++;
		if (web_sheets[i].state == SHEET_WANTED || web_sheets[i].asked != now)
			return SHEET + i;
	}
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
wanted_open(void)
{
	int i, open = 0;

	for (i = 0; i < picture_count; i++)
		open += pictures[i].state == PICTURE_WANTED || pictures[i].state == PICTURE_BY_HAND;
	for (i = 0; i < web_sheet_count; i++)
		open += web_sheets[i].state == SHEET_WANTED || web_sheets[i].state == SHEET_BY_HAND;
	return open;
}

/* The page's pictures and sheets that could not be had; `sheets`: how many of them are sheets. */
static int
failures(int *sheets)
{
	int i, n = 0, s = 0;

	for (i = 0; i < picture_count; i++)
		n += pictures[i].state == PICTURE_FAILED;
	for (i = 0; i < web_sheet_count; i++)
		s += web_sheets[i].state == SHEET_FAILED && web_sheets[i].address[0];
	if (sheets)
		*sheets = s;
	return n + s;
}

/* What is still to come, in words for the status line and for its right part, and how much of
 * it is here (`total` 0: not known). 0: nothing is; 2: only what waits for a visitor's paste. */
static int
progress(char *text, int room, char *cell, int cell_room, int *done, int *total)
{
	const char *at = strstr(address, "://");
	char host[64];
	int i, sheets = 0, sheets_done = 0, pics = 0, pics_done = 0, hand = 0, failed = failures(NULL), n;

	*done = *total = 0;
	at = at ? at + 3 : strrchr(address, '/') ? strrchr(address, '/') + 1 : address;
	snprintf(host, sizeof host, "%.*s", (int)strcspn(at, "/?#"), at);
	snprintf(cell, cell_room, "Loading...");
	if (loading == -1 && asking == -1) {
		i = net.got >> 12 << 2;	/* (in 4 KB: a line of the status every few packets, not every one) */
		if (net.state < NET_SEND)
			snprintf(text, room, "Connecting to %s ...", host);
		else if (net.state < NET_HEADED)
			snprintf(text, room, "Waiting for an answer from %s ...", host);
		else if (net.length > 0) {
			*done = i;
			*total = (int)(net.length >> 12 << 2) + 4;
			snprintf(text, room, "%s %s: %d of %d KB", net_sink == 2 ? "Saving from" : "Loading", host, *done, *total);
			snprintf(cell, cell_room, "%d of %d KB", *done, *total);
		} else {
			snprintf(text, room, "%s %s: %d KB", net_sink == 2 ? "Saving from" : "Loading", host, i);
			snprintf(cell, cell_room, "%d KB", i);
		}
		return 1;
	}
	if ((loading || by_hand) && asking == -1) {
		if (by_hand)
			snprintf(text, room, "Waiting for %s: its address is to be pasted (the button by the screen)", host);
		else if (part_total) {
			*done = part_have / 1000, *total = part_total / 1000 + 1;
			snprintf(text, room, "Loading %s: %d of %d KB", host, *done, *total);
			snprintf(cell, cell_room, "%d of %d KB", *done, *total);
		} else
			snprintf(text, room, "Loading %s ...", host);
		return by_hand ? 2 : 1;
	}
	for (i = 0; i < web_sheet_count; i++) {
		sheets += sheet_tracked[i];
		sheets_done += sheet_tracked[i] && (web_sheets[i].state == SHEET_HERE || web_sheets[i].state == SHEET_FAILED);
		hand += web_sheets[i].state == SHEET_BY_HAND;
	}
	for (i = 0; i < picture_count; i++) {
		pics += pictures[i].tracked;
		pics_done += pictures[i].tracked && (pictures[i].state == PICTURE_FAILED || (pictures[i].state == PICTURE_HERE && !pictures[i].later));
		hand += pictures[i].state == PICTURE_BY_HAND;
	}
	if (sheets_done == sheets && pics_done == pics)
		return 0;
	n = snprintf(text, room, "Loading %s:", host);
	if (sheets)
		n += snprintf(text + n, room - n, " %d of %d style sheet%s%s", sheets_done, sheets, sheets > 1 ? "s" : "", pics ? "," : "");
	if (pics && n < room)
		n += snprintf(text + n, room - n, " %d of %d picture%s", pics_done, pics, pics > 1 ? "s" : "");
	if (failed && n < room)
		n += snprintf(text + n, room - n, " (%d not loaded)", failed);
	if (hand && n < room)
		snprintf(text + n, room - n, "; %d to paste: the button by the screen", hand);
	*done = sheets_done + pics_done, *total = sheets + pics;
	snprintf(cell, cell_room, "%d of %d", *done, *total);
	return hand == *total - *done ? 2 : 1;
}

/* "2 pictures and 1 style sheet could not be loaded", and what a click does. */
static void
failures_say(char *text, int room)
{
	int sheets, all = failures(&sheets), pics = all - sheets, n = 0;

	if (pics)
		n = snprintf(text, room, "%d picture%s%s", pics, pics > 1 ? "s" : "", sheets ? " and " : "");
	if (sheets)
		n += snprintf(text + n, room - n, "%d style sheet%s", sheets, sheets > 1 ? "s" : "");
	snprintf(text + n, room - n, " could not be loaded: click here to see why");
}

/* Pictures that failed since the last look: their boxes drawn where the page gives their size,
 * the page laid out again for one whose size it does not say. */
static void
failures_show(void)
{
	int i, k, again = 0;

	failed_due = 0;
	for (k = 0; k < picture_count; k++) {
		struct picture *p = &pictures[k];

		if (p->state != PICTURE_FAILED || p->boxed)
			continue;
		if (!p->sized) {
			again = 1;
			continue;
		}
		p->boxed = 1;
		for (i = 0; i < web_item_count; i++)
			if (web_items[i].kind == I_IMAGE && web_items[i].a == k)
				strips_forget_rows(web_items[i].y, web_items[i].y + web_items[i].h);
	}
	if (again)
		lay_out(0, 0);
	show_view();
}

/* Says what is still to come, after anything that may have changed it: the Stop key, the bar in
 * the address field, the status line, the window's caption and the pointer; and when all has
 * come, what could not be. `tick`: the wait ran out (the bar's block moves). */
static void
loading_shown(int tick)
{
	static GR_CURSOR_ID hourglass;
	char text[200], cell[24];
	int done, total, now = progress(text, sizeof text, cell, sizeof cell, &done, &total), failed = failures(NULL), was = shown_busy, i;

	if (now && !total && tick)
		spin++;
	else if (now == shown_busy && done == shown_done && total == shown_total && failed == shown_failed && !strcmp(text, shown_text))
		return;
	if (dump && now && strcmp(text, shown_text))
		printf("nxweb: loading: %s\n", text);
	shown_busy = now, shown_done = done, shown_total = total, shown_failed = failed;
	snprintf(shown_text, sizeof shown_text, "%s", now ? text : "");
	snprintf(shown_cell, sizeof shown_cell, "%s", cell);
	if (!now && failed != failed_told) {
		failed_told = failed;
		if (failed)
			failures_say(status, sizeof status);
		for (i = 0; dump && i < web_sheet_count; i++)
			if (web_sheets[i].state == SHEET_FAILED && web_sheets[i].address[0])
				printf("nxweb: not loaded: style sheet %s: %s\n", web_sheets[i].address, sheet_why[i]);
		for (i = 0; dump && i < picture_count; i++)
			if (pictures[i].state == PICTURE_FAILED)
				printf("nxweb: not loaded: picture %s: %s\n", pictures[i].address, pictures[i].why);
		if (dump)
			printf("nxweb: %s: all here, %d not loaded\n", address, failed);
	}
	if (dump || print)
		fflush(stdout);
	if (!window)
		return;
	if ((now == 1) != (was == 1)) {
		/* (not while it is only a visitor's paste that is waited for) */
		if (!hourglass)
			hourglass = ui_launch_cursor();
		GrSetWindowCursor(window, now == 1 ? hourglass : 0);
		set_title();
	}
	draw_tool(2);
	draw_address();
	draw_status();
}

/* ---- what did not come ---- */

#define WHY_W 560
#define WHY_ROWS 16

/* Each picture and sheet of the page that could not be had and why, in a window of its own
 * until Close, Enter or Escape. */
static void
failures_list(void)
{
	GR_WINDOW_ID w;
	GR_EVENT event;
	char line[400];
	int i, n = 0, all = failures(NULL), h = 64 + (all < WHY_ROWS ? all : WHY_ROWS + 1) * 2 * line_h, y, x, done = 0;

	if (!all) {
		note("everything of this page was loaded");
		return;
	}
	w = GrNewWindowEx(UI_FIXED, "Not loaded", GR_ROOT_WINDOW_ID, -1, -1, WHY_W, h, UI_FACE);
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	GrRaiseWindow(w);
	GrSetFocus(w);
	if (print) {
		for (i = 0; i < web_sheet_count; i++)
			if (web_sheets[i].state == SHEET_FAILED && web_sheets[i].address[0])
				printf("nxweb: list: style sheet %s: %s\n", web_sheets[i].address, sheet_why[i]);
		for (i = 0; i < picture_count; i++)
			if (pictures[i].state == PICTURE_FAILED)
				printf("nxweb: list: picture %s: %s\n", pictures[i].address, pictures[i].why);
		fflush(stdout);
	}
	while (!done) {
		GrGetNextEvent(&event);
		if (event.general.wid != w) {
			if (event.type != GR_EVENT_TYPE_NONE)
				others(&event);
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			ui_fill(w, 0, 0, WHY_W, h, UI_FACE);
			snprintf(line, sizeof line, "Not loaded for %.200s:", address);
			ui_text_fit(w, 10, 10, WHY_W - 20, line, BLACK);
			/* a row each, sheets first: what and why, then its address under it */
			for (i = 0, n = 0, y = 10 + line_h + 6; i < web_sheet_count + picture_count && n < WHY_ROWS; i++) {
				int sheet = i < web_sheet_count;
				const char *where = sheet ? web_sheets[i].address : pictures[i - web_sheet_count].address;

				if (sheet ? web_sheets[i].state != SHEET_FAILED || !where[0] : pictures[i - web_sheet_count].state != PICTURE_FAILED)
					continue;
				snprintf(line, sizeof line, "%s: %.63s", sheet ? "Style sheet" : "Picture", sheet ? sheet_why[i] : pictures[i - web_sheet_count].why);
				ui_text_fit(w, 10, y, WHY_W - 20, line, mw(0xa00000));
				ui_text_fit(w, 26, y + line_h, WHY_W - 36, where, mw(0x404040));
				y += 2 * line_h, n++;
			}
			if (all > n) {
				snprintf(line, sizeof line, "and %d more", all - n);
				ui_text(w, 10, y, line, -1, BLACK, 0);
			}
			ui_button(w, WHY_W - 86, h - 32, 76, 22, "Close", 0);
			ui_focus(w, WHY_W - 82, h - 28, 68, 14);
			if (print) {
				ui_window_origin(w, &x, &y);
				printf("nxweb: the list at %d,%d\n", x, y);
				fflush(stdout);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			done = 1;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			done = ui_inside(event.button.x, event.button.y, WHY_W - 86, h - 32, 76, 22);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			done = event.keystroke.ch == MWKEY_ESCAPE || event.keystroke.ch == MWKEY_ENTER || event.keystroke.ch == ' ';
			break;
		}
	}
	GrDestroyWindow(w);
}

/* ---- forms ---- */

/* What a form sends, growing. */
struct form_out { char *s; int n, room; };

static void
form_bytes(struct form_out *o, const void *bytes, int n)
{
	if (o->n + n + 1 > o->room) {
		int room = (o->n + n + 1) * 2 + 256;
		char *more = realloc(o->s, room);

		if (!more)
			return;
		o->s = more;
		o->room = room;
	}
	memcpy(o->s + o->n, bytes, n);
	o->n += n;
	o->s[o->n] = 0;
}

static void
form_text(struct form_out *o, const char *text)
{
	form_bytes(o, text, (int)strlen(text));
}

/* Text as UTF-8, escaped for an address and a form's body, or not. */
static void
form_chars(struct form_out *o, const unsigned short *value, int n, int escaped)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;

	for (i = 0; i < n; i++) {
		unsigned c = value[i];
		unsigned char utf[3];
		int bytes = c < 0x80 ? 1 : c < 0x800 ? 2 : 3, b;

		if (escaped && c == '\n') {
			form_text(o, "%0D%0A");
			continue;
		}
		if (!escaped && c == '\n') {
			form_text(o, "\r\n");
			continue;
		}
		if (escaped && c == ' ') {
			form_text(o, "+");
			continue;
		}
		utf[0] = bytes == 1 ? c : bytes == 2 ? 0xc0 | c >> 6 : 0xe0 | c >> 12;
		utf[1] = bytes == 2 ? 0x80 | (c & 0x3f) : 0x80 | (c >> 6 & 0x3f);
		utf[2] = 0x80 | (c & 0x3f);
		if (!escaped || (bytes == 1 && (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~'))) {
			form_bytes(o, utf, bytes);
			continue;
		}
		for (b = 0; b < bytes; b++) {
			char three[3] = { '%', hex[utf[b] >> 4], hex[utf[b] & 15] };

			form_bytes(o, three, 3);
		}
	}
}

/* A name and its value in the form's way of sending: `boundary` for multipart/form-data. */
static void
form_add(struct form_out *o, const char *name, const unsigned short *value, int n, const char *boundary, const char *filename)
{
	unsigned short wide[80];
	int k;

	if (!boundary) {
		if (o->n)
			form_text(o, "&");
		for (k = 0; name[k] && k < 80; k++)
			wide[k] = (unsigned char)name[k];
		form_chars(o, wide, k, 1);
		form_text(o, "=");
		form_chars(o, value, n, 1);
		return;
	}
	form_text(o, "--");
	form_text(o, boundary);
	form_text(o, "\r\nContent-Disposition: form-data; name=\"");
	form_text(o, name);
	form_text(o, "\"");
	if (filename) {
		const char *base = strrchr(filename, '/');

		form_text(o, "; filename=\"");
		form_text(o, base ? base + 1 : filename);
		form_text(o, "\"\r\nContent-Type: application/octet-stream");
	}
	form_text(o, "\r\n\r\n");
	if (filename) {
		/* a file, as it lies */
		FILE *f = fopen(filename, "rb");
		char part[1024];
		size_t got;

		while (f && (got = fread(part, 1, sizeof part, f)) > 0)
			form_bytes(o, part, (int)got);
		if (f)
			fclose(f);
	} else
		form_chars(o, value, n, 0);
	form_text(o, "\r\n");
}

/* The nodes inside a form: from `form` + 1 up to the first that is not. */
static int
form_end(int form)
{
	int end, i;

	for (end = form + 1; end < web_node_count; end++) {
		for (i = end; i >= 0 && i != form; i = web_nodes[i].c.parent)
			;
		if (i < 0)
			break;
	}
	return end;
}

/* The form of a field or a button, or -1. */
static int
form_of(int node)
{
	while (node >= 0 && web_nodes[node].c.tag != T_form)
		node = web_nodes[node].c.parent;
	return node;
}

/* What a <select>'s chosen option sends. */
static int
select_value(int node, const struct field *f, unsigned short *wide, int room)
{
	char value[200];
	const char *v;
	int kid, k = 0, n = 0, used, left;

	for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next)
		if (web_nodes[kid].c.tag == T_option && k++ == f->option) {
			if (web_attr_text(kid, "value", value, sizeof value)) {
				for (v = value, left = strlen(value); left > 0 && n < room; v += used, left -= used)
					wide[n++] = web_char((const unsigned char *)v, left, &used);
			} else if (web_nodes[kid].first >= 0) {
				n = web_least(web_nodes[web_nodes[kid].first].text_n, room);
				memcpy(wide, web_u + web_nodes[web_nodes[kid].first].text, n * sizeof *wide);
			}
		}
	return n;
}

/* A form's fields as the page gave them. */
static void
form_reset(int form)
{
	int end = form_end(form), i;

	for (i = 0; i < field_count; i++)
		if (fields[i].node > form && fields[i].node < end) {
			free(fields[i].text);
			fields[i--] = fields[--field_count];
		}
	strips_forget();
	show_view();
}

/* Sends the form a field is in: as an address with the fields' values after it, or by POST with
 * them as the request's body. `pressed` is the button that was pressed, or -1. */
static void
submit(int node, int pressed)
{
	struct form_out out = { NULL, 0, 0 };
	char action[600] = "", method[16] = "get", enctype[48] = "", name[80], whole[256], boundary[40] = "", type[24], *cut;
	unsigned short wide[128];
	int form = form_of(node), end, i, n, post, multi;
	const char *why = NULL;

	if (form < 0)
		return;
	if (pressed >= 0) {
		strcpy(type, web_nodes[pressed].c.tag == T_button ? "submit" : "");
		web_attr_text(pressed, "type", type, sizeof type);
		if (!strcasecmp(type, "reset")) {
			form_reset(form);
			return;
		}
		if (!strcasecmp(type, "button"))
			return;	/* (it would be a script's) */
	}
	web_attr_text(form, "method", method, sizeof method);
	web_attr_text(form, "enctype", enctype, sizeof enctype);
	post = !strcasecmp(method, "post");
	multi = post && !strcasecmp(enctype, "multipart/form-data");
	if (multi)
		snprintf(boundary, sizeof boundary, "----nxweb%08x", (unsigned)net_ms());
	end = form_end(form);
	for (i = form + 1; i < end; i++) {
		const char *t, *v;
		struct field *f;
		int is_button = web_nodes[i].c.tag == T_button;

		if ((!web_is_field(i) && !is_button) || !web_attr_text(i, "name", name, sizeof name) || !name[0] || web_attr(i, "disabled", &v, &n))
			continue;
		if (is_button) {
			char value[200];
			int used, left;

			if (i != pressed)
				continue;
			n = 0;
			if (web_attr_text(i, "value", value, sizeof value))
				for (v = value, left = strlen(value); left > 0 && n < 127; v += used, left -= used)
					wide[n++] = web_char((const unsigned char *)v, left, &used);
			form_add(&out, name, wide, n, multi ? boundary : NULL, NULL);
			continue;
		}
		t = field_type(i);
		f = field_of(i);
		if (field_is_button(t) && i != pressed)
			continue;
		if ((!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio")) && !f->checked)
			continue;
		if (!strcasecmp(t, "select")) {
			n = select_value(i, f, wide, 127);
			form_add(&out, name, wide, n, multi ? boundary : NULL, NULL);
		} else if (!strcasecmp(t, "file")) {
			char path[200];
			int k;

			for (k = 0; k < f->n && k < 199; k++)
				path[k] = (char)f->text[k];
			path[k] = 0;
			if (multi)
				form_add(&out, name, wide, 0, boundary, path);
			else
				form_add(&out, name, f->text, f->n, NULL, NULL);
		} else if (!f->n && (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio"))) {
			wide[0] = 'o', wide[1] = 'n';
			form_add(&out, name, wide, 2, multi ? boundary : NULL, NULL);
		} else
			form_add(&out, name, f->text, f->n, multi ? boundary : NULL, NULL);
	}
	if (multi) {
		form_text(&out, "--");
		form_text(&out, boundary);
		form_text(&out, "--\r\n");
	}
	if (!web_attr_text(form, "action", action, sizeof action) || !action[0])
		snprintf(action, sizeof action, "%s", address);
	if (!post && (cut = strpbrk(action, "?#")) != NULL)
		*cut = 0;
	if (!post && strlen(action) + out.n + 2 > sizeof action)
		why = "the form's address is longer than this browser keeps";
	else if (!post) {
		strcat(action, "?");
		if (out.s)
			strcat(action, out.s);
	}
	if (!why && !web_resolve(action, whole))
		why = "the form's address is longer than this browser keeps";
	if (why) {
		snprintf(status, sizeof status, "%s", why);
		draw_status();
		free(out.s);
		return;
	}
	if (post) {
		uint32_t ip;
		char no[200];

		if (net_route(whole, &ip, no, sizeof no) != ROUTE_DIRECT) {
			snprintf(status, sizeof status, "this form is posted, and the host's fetch only asks for addresses: it goes only to machines this one reaches itself");
			draw_status();
			free(out.s);
			return;
		}
		post_next = out.s ? out.s : strdup("");
		post_next_n = out.n;
		if (multi)
			snprintf(post_next_type, sizeof post_next_type, "multipart/form-data; boundary=%s", boundary);
		else
			snprintf(post_next_type, sizeof post_next_type, "application/x-www-form-urlencoded");
	} else
		free(out.s);
	go(whole, 1);
}

/* A field drawn again where it is: its strips made again, and the view. */
static void
field_again(int node)
{
	int i;

	for (i = 0; i < web_item_count; i++)
		if (web_items[i].kind == I_FIELD && web_items[i].node == node)
			strips_forget_rows(web_items[i].y, web_items[i].y + web_items[i].h);
	show_view();
}

/* ---- what is kept ---- */

/* The options and the bookmarks are keys of the desktop's registry (registry.h): web.home,
 * web.pictures, web.bookmarks (how many) and web.bookmark.N, each "ADDRESS TITLE". */
static void
kept_read(void)
{
	char key[32], *space;
	int i, count = reg_int("web.bookmarks", 0);

	snprintf(home, sizeof home, "%s", reg_get("web.home", HOME));
	load_pictures = reg_int("web.pictures", 1) != 0;
	for (i = 0; i < count && bookmark_count < BOOKMARKS; i++) {
		struct bookmark *b = &bookmarks[bookmark_count];

		snprintf(key, sizeof key, "web.bookmark.%d", i);
		snprintf(b->address, sizeof b->address, "%s", reg_get(key, ""));
		if ((space = strchr(b->address, ' ')) != NULL)
			*space++ = 0;
		snprintf(b->title, sizeof b->title, "%s", space ? space : "");
		bookmark_count += b->address[0] != 0;
	}
}

static int
options_write(void)
{
	return reg_set("web.home", home) & reg_set_int("web.pictures", load_pictures);
}

/* Writes the bookmarks from number `from` on, and how many there are; one more key than
 * there are bookmarks is taken out (the last, when one was taken away). */
static int
bookmarks_write(int from)
{
	char key[32], value[340];
	int i, fine = 1;

	for (i = from; i < bookmark_count; i++) {
		snprintf(key, sizeof key, "web.bookmark.%d", i);
		snprintf(value, sizeof value, "%s%s%.50s", bookmarks[i].address, bookmarks[i].title[0] ? " " : "", bookmarks[i].title);
		fine &= reg_set(key, value);
	}
	snprintf(key, sizeof key, "web.bookmark.%d", bookmark_count);
	reg_set(key, NULL);
	return fine & reg_set_int("web.bookmarks", bookmark_count);
}

/* The bookmark of an address, or -1. */
static int
bookmark_of(const char *where)
{
	int i;

	for (i = 0; i < bookmark_count; i++)
		if (!strcmp(bookmarks[i].address, where))
			return i;
	return -1;
}

/* Adds a bookmark for an address; with `toggle`, one that is there is taken away instead. */
static void
bookmark(const char *where, const char *title, int toggle)
{
	int at = bookmark_of(where);
	const char *did = "a bookmark is added";

	if (at >= 0 && !toggle) {
		note("that link has a bookmark already");
		return;
	}
	if (at < 0 && bookmark_count == BOOKMARKS) {
		note("there is room for 24 bookmarks: take one away first (Bookmarks, Remove)");
		return;
	}
	if (at < 0 && (strchr(where, ' ') || strlen(where) > 180)) {
		note("an address with a space in it, or one over 180 characters, gets no bookmark");
		return;
	}
	if (at >= 0) {
		memmove(&bookmarks[at], &bookmarks[at + 1], sizeof bookmarks[0] * (--bookmark_count - at));
		did = "the bookmark is taken away";
	} else {
		at = bookmark_count++;
		snprintf(bookmarks[at].address, sizeof bookmarks[0].address, "%s", where);
		snprintf(bookmarks[at].title, sizeof bookmarks[0].title, "%s", title);
	}
	snprintf(status, sizeof status, "%s%s", did, bookmarks_write(at) ? "" : " (not kept: /persistence cannot be written)");
	draw_status();
	draw_tool(4);
}

/* ---- what the menus' rows do ---- */

static int
busy(void)
{
	return loading || by_hand || (regs && wanted_open()) || pictures_waiting();
}

/* Gives up what the host has not sent yet, the page or its sheets and pictures, and the
 * pictures still to be decoded. */
static void
stop(void)
{
	int i;

	if (net.state && net.state < NET_DONE) {
		if (net_kind == 0 && !net_sink && net.n > net_shown) {
			/* the page as far as it came */
			net_end(&net, NET_DONE);
			net_finish(1);
		} else if (net_kind == 0 && net_sink == 2) {
			net_end(&net, NET_DONE);
			net_finish(1);
		} else
			net_cancel();
	}
	loading = by_hand = 0;
	for (i = 0; i < picture_count; i++)
		if (pictures[i].state == PICTURE_WANTED || pictures[i].state == PICTURE_BY_HAND ||
		    (pictures[i].state == PICTURE_HERE && pictures[i].later))
			picture_fail(&pictures[i], "stopped");
	for (i = 0; i < web_sheet_count; i++)
		if (web_sheets[i].state == SHEET_WANTED || web_sheets[i].state == SHEET_BY_HAND)
			sheet_fail(i, "stopped");
	if (part_file)
		fclose(part_file);
	part_file = NULL;
	asking = -1;
	note("stopped");
}

/* The page laid out anew with its pictures looked for again, or with none (the options). */
static void
pictures_again(void)
{
	if (loading && asking >= 0 && asking < SHEET) {
		net_cancel();
		loading = 0;
		if (part_file)
			fclose(part_file);
		part_file = NULL;
		asking = -1;
	}
	pictures_drop();
	lay_out(0, 0);
	top = goal = top > web_page_h - view_height() ? web_page_h - view_height() : top;
	show_view();
}

/* The next place the page's text has what is sought, from the last one found or from the
 * view's top, and round to the page's start. A place is inside one text of one face. */
static void
find_next(void)
{
	int n = strlen(sought), item = found, from = found_from + 1, tried, i, j;

	if (!n || !web_item_count)
		return;
	if (item < 0)
		for (item = from = 0; item < web_item_count - 1 && web_items[item].y + web_items[item].h <= top; item++)
			;
	for (tried = 0; tried <= web_item_count; tried++, item = (item + 1) % web_item_count, from = 0) {
		const struct web_item *it = &web_items[item];

		for (i = from; it->kind == I_TEXT && i + n <= it->b; i++) {
			for (j = 0; j < n && web_u[it->a + i + j] < 128 && tolower(web_u[it->a + i + j]) == tolower((unsigned char)sought[j]); j++)
				;
			if (j < n)
				continue;
			found = item, found_from = i;
			if (it->y < top || it->y + it->h > top + view_height())
				top = goal = it->y - view_height() / 3;
			snprintf(status, sizeof status, "found \"%s\" (F3: the next place)", sought);
			strips_forget();
			show_view();
			draw_status();
			return;
		}
	}
	snprintf(status, sizeof status, "\"%s\" is not in the page's text", sought);
	if (found >= 0) {
		found = -1;
		strips_forget();
		show_view();
	}
	draw_status();
}

/* A key of what Find is told, in the status line. */
static void
find_key(int ch)
{
	int n = strlen(sought);

	if (ch == MWKEY_ESCAPE) {
		finding = 0;
	} else if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
		finding = 0;
		find_next();
		return;
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			sought[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < (int)sizeof sought - 1) {
		sought[n] = ch, sought[n + 1] = 0;
	}
	draw_status();
}

/* Opens the address field with a text in it, which `select` shows as one to type over. */
static void
field_open(const char *text, int select)
{
	int was = focus, told = finding;

	snprintf(typed, sizeof typed, "%.250s", text);
	editing = 1;
	offered = select;
	finding = 0;
	focus = -1;
	if (was >= 0)
		field_again(was);
	draw_address();
	if (told)
		draw_status();
}

/* What was typed as an address, as one to go to. A file's path and an address with its scheme are
 * what they say. 10.0.0.11, localhost, a word with no dot in it, a name /etc/hosts has and anything
 * with a port are machines of this network, by http; a bare name with dots is a site, by https. */
static void
typed_address(const char *text, char *where, int room)
{
	char word[256], host[128];
	const char *scheme, *at;
	int n, port, k;

	while (isspace((unsigned char)*text))
		text++;
	snprintf(word, sizeof word, "%.253s", text);
	for (n = (int)strlen(word); n > 0 && isspace((unsigned char)word[n - 1]); n--)
		word[n - 1] = 0;
	scheme = strstr(word, "://");
	if (word[0] == '/' || !strncasecmp(word, "about:", 6) || !strncasecmp(word, "world:", 6) || !strncasecmp(word, "file:", 5) ||
	    !strncasecmp(word, "persist:", 8) || !strncasecmp(word, "host:", 5) || (scheme && strncasecmp(word, "http", 4))) {
		snprintf(where, room, "%s", word);
		return;
	}
	if (scheme) {
		/* the scheme in small letters, as the rest of the browser compares it */
		for (k = 0; word + k < scheme; k++)
			word[k] = tolower((unsigned char)word[k]);
		snprintf(where, room, "%s", word);
	} else {
		at = word + strcspn(word, ":/?#");
		k = (int)(at - word);
		port = *at == ':' && isdigit((unsigned char)at[1]);
		snprintf(host, sizeof host, "%.*s", k < 127 ? k : 127, word);
		snprintf(where, room, "%s%s", net_typed_is_local(host, port) ? "http://" : "https://", word);
	}
	/* a site with no path is its root */
	at = strstr(where, "://") + 3;
	if (!strchr(at, '/') && !strpbrk(at, "?#") && strlen(where) < 254)
		strcat(where, "/");
	where[255] = 0;
}

/* To another page of this session's, where it was left. */
static void
travel(int to)
{
	char where[256];

	history[history_at].top = top;
	history_at = to;
	top_wanted = history[to].top;
	strcpy(where, history[to].address);
	go(where, 0);
}

static void
resized(int w, int h)
{
	int wider = w != width;

	if (w == width && h == height)
		return;
	width = w;
	height = h;
	if (wider)
		lay_out(1, 1);	/* (the sheets' media queries ask about the width) */
	draw_all();
}

/* This window's events while the options or a file's window is up. */
static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_FDINPUT)
		net_pump();	/* (a page coming while a window is up: it is read, and shown when the window goes) */
	else if (event->type == GR_EVENT_TYPE_UPDATE && event->update.wid == window && event->update.utype == GR_UPDATE_SIZE)
		resized(event->update.width, event->update.height);
	else if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
}

/* ---- the options' window ---- */

#define OPT_W 400
#define OPT_H 150
enum { O_HOME, O_THIS, O_FIRST, O_PICTURES, O_OK, O_CANCEL, O_COUNT };
static const struct { int x, y, w, h; const char *label; } opt_at[O_COUNT] = {
	{ 84, 10, OPT_W - 94, 20, NULL }, { 84, 36, 100, 22, "This page" }, { 190, 36, 100, 22, "The first one" },
	{ 10, 72, 180, 15, NULL }, { OPT_W - 170, OPT_H - 32, 76, 22, "OK" }, { OPT_W - 86, OPT_H - 32, 76, 22, "Cancel" },
};
static struct { char home[256]; int pictures, focus; } opt;

static void
options_draw(GR_WINDOW_ID w)
{
	const char *text = opt.home;
	int i, j, room = opt_at[O_HOME].w - 12;

	ui_fill(w, 0, 0, OPT_W, OPT_H, UI_FACE);
	ui_text(w, 10, 14, "Home page", -1, BLACK, 0);
	ui_fill(w, opt_at[O_HOME].x, opt_at[O_HOME].y, opt_at[O_HOME].w, opt_at[O_HOME].h, WHITE);
	ui_bevel(w, opt_at[O_HOME].x, opt_at[O_HOME].y, opt_at[O_HOME].w, opt_at[O_HOME].h, 1);
	while (text[0] && ui_width(&ui_var_widths, ui_var, text, -1) > room)
		text++;
	ui_text(w, opt_at[O_HOME].x + 5, 14, text, -1, BLACK, 0);
	if (opt.focus == O_HOME)
		ui_fill(w, opt_at[O_HOME].x + 5 + ui_width(&ui_var_widths, ui_var, text, -1), 13, 1, 14, BLACK);
	for (i = O_THIS; i < O_COUNT; i++) {
		if (opt_at[i].label)
			ui_button(w, opt_at[i].x, opt_at[i].y, opt_at[i].w, opt_at[i].h, opt_at[i].label, 0);
		if (i == opt.focus)
			ui_focus(w, opt_at[i].x + 4, opt_at[i].y + 4, opt_at[i].w - 8, opt_at[i].h - 8);
	}
	/* a box with a tick in it when the choice is on */
	ui_fill(w, 14, 73, 13, 13, WHITE);
	ui_bevel(w, 14, 73, 13, 13, 1);
	GrSetGCForeground(ui_gc, BLACK);
	for (j = 0; opt.pictures && j < 7; j++)
		GrLine(w, ui_gc, 17 + j, 73 + (j < 3 ? 5 + j : 9 - j), 17 + j, 73 + (j < 3 ? 7 + j : 11 - j));
	ui_text(w, 34, 74, "Load the pages' pictures", -1, BLACK, 0);
	ui_text(w, 10, 98, "Kept in the registry, which lasts when the storage is saved.", -1, UI_SHADOW, 0);
}

/* The options, in a window of their own until OK or Cancel: the home page's address, typed,
 * and whether pictures are loaded. Tab goes round, Enter is OK, Escape is Cancel. */
static void
options(void)
{
	GR_WINDOW_ID w = GrNewWindowEx(UI_FIXED, "Options", GR_ROOT_WINDOW_ID, -1, -1, OPT_W, OPT_H, UI_FACE);
	GR_EVENT event;
	int done = 0, press, i, n, ch, x, y;

	snprintf(opt.home, sizeof opt.home, "%s", home);
	opt.pictures = load_pictures;
	opt.focus = O_HOME;
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	GrRaiseWindow(w);
	GrSetFocus(w);
	while (!done) {
		GrGetNextEvent(&event);
		if (event.general.wid != w) {
			if (event.type != GR_EVENT_TYPE_NONE)
				others(&event);
			continue;
		}
		press = -1;
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (print) {	/* where it is, for a test to click in */
				ui_window_origin(w, &x, &y);
				printf("nxweb: options at %d,%d\n", x, y);
				fflush(stdout);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			done = 2;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			for (i = 0; i < O_COUNT && !ui_wheel(&event); i++)
				if (ui_inside(event.button.x, event.button.y, opt_at[i].x, opt_at[i].y, opt_at[i].w, opt_at[i].h))
					press = opt.focus = i;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			ch = event.keystroke.ch;
			n = strlen(opt.home);
			if (ch == MWKEY_ESCAPE)
				done = 2;
			else if (ui_tab(&event, &opt.focus, O_COUNT))
				break;
			else if (ch == MWKEY_ENTER)
				press = opt.focus == O_HOME || opt.focus == O_PICTURES ? O_OK : opt.focus;
			else if (opt.focus != O_HOME)
				press = ch == ' ' ? opt.focus : -1;
			else if ((ch == MWKEY_BACKSPACE || ch == 127) && n)
				opt.home[n - 1] = 0;
			else if (ch >= 32 && ch < 127 && n < 250)
				opt.home[n] = ch, opt.home[n + 1] = 0;
			break;
		default:
			continue;
		}
		switch (press) {
		case O_THIS: snprintf(opt.home, sizeof opt.home, "%s", address); break;
		case O_FIRST: strcpy(opt.home, HOME); break;
		case O_PICTURES: opt.pictures = !opt.pictures; break;
		case O_OK: done = 1; break;
		case O_CANCEL: done = 2; break;
		}
		if (!done)
			options_draw(w);
	}
	GrDestroyWindow(w);
	if (done != 1)
		return;
	if (opt.home[0]) {
		char where[280];

		typed_address(opt.home, where, sizeof where);
		strcpy(home, where);
	} else
		strcpy(home, HOME);
	i = opt.pictures != load_pictures;
	load_pictures = opt.pictures;
	note(options_write() ? "the options are in the registry" : "the options hold until the browser closes (/persistence cannot be written)");
	if (i)
		pictures_again();
}

/* ---- the menus ---- */

enum { C_ADDRESS = 1, C_OPEN, C_SAVE, C_CLOSE, C_COPY, C_PASTE, C_FIND, C_NEXT, C_STOP, C_RELOAD, C_PICTURES, C_SOURCE, C_OPTIONS, C_FAILED,
       C_BACK, C_FORWARD, C_HOME, C_MARK, C_KEYS, C_ABOUT, C_LINK, C_LINK_COPY, C_LINK_MARK,
       C_VISIT = 100, C_BOOKMARK = 200, C_REMOVE = 300 };	/* and from each of these three, a row a page */

/* A menu is made when it opens: what its rows say and which are grey depend on the page. */
static struct ui_menu_item rows[HISTORY + BOOKMARKS + 8], removals[BOOKMARKS];
static char labels[BOOKMARKS][56];
static int row_count;

static struct ui_menu_item *
row(const char *label, int id, int flags)
{
	struct ui_menu_item *r = &rows[row_count++];

	memset(r, 0, sizeof *r);
	r->label = label;
	r->icon = -1;
	r->id = id;
	r->flags = flags;
	return r;
}

/* A page as a row: its title, or its address when it has none. */
static const char *
page_label(int k, const char *title, const char *where)
{
	const char *text = title[0] ? title : where;

	snprintf(labels[k], sizeof labels[0], "%.48s%s", text, strlen(text) > 48 ? "..." : "");
	return labels[k];
}

static void
menu_make(int which)
{
	int i, marked = bookmark_of(address) >= 0, grey = UI_MENU_GREY;

	row_count = 0;
	switch (which) {
	case 0:
		row("Open address...   Ctrl+L", C_ADDRESS, 0);
		row("Open file...   Ctrl+O", C_OPEN, 0);
		row("Save page as...   Ctrl+S", C_SAVE, page_length ? 0 : grey);
		row("Close   Ctrl+Q", C_CLOSE, UI_MENU_LINE);
		break;
	case 1:
		row("Copy the address   Ctrl+C", C_COPY, 0);
		row("Paste an address   Ctrl+V", C_PASTE, 0);
		row("Find in page...   Ctrl+F", C_FIND, UI_MENU_LINE);
		row("Find next   F3", C_NEXT, sought[0] ? 0 : grey);
		break;
	case 2:
		row("Stop   Escape", C_STOP, busy() ? 0 : grey);
		row("Reload   F5", C_RELOAD, 0);
		row("Pictures", C_PICTURES, UI_MENU_LINE | (load_pictures ? UI_MENU_CHECK : 0));
		row("What was not loaded...", C_FAILED, failures(NULL) ? 0 : grey);
		row("Page source   Ctrl+U", C_SOURCE, page_length ? 0 : grey);
		row("Options...", C_OPTIONS, UI_MENU_LINE);
		break;
	case 3:
		row("Back   Alt+Left", C_BACK, history_at > 0 ? 0 : grey);
		row("Forward   Alt+Right", C_FORWARD, history_at + 1 < history_count ? 0 : grey);
		row("Home   Alt+Home", C_HOME, 0);
		/* the session's pages, the newest first, a tick at the one shown */
		for (i = history_count - 1; i >= 0; i--)
			row(page_label(i, history[i].title, history[i].address), C_VISIT + i,
			    (i == history_count - 1 ? UI_MENU_LINE : 0) | (i == history_at ? UI_MENU_CHECK : 0));
		break;
	case 4:
		row(marked ? "Remove this page   Ctrl+D" : "Add this page   Ctrl+D", C_MARK, 0);
		row("Remove", 0, bookmark_count ? 0 : grey)->sub = removals;
		rows[1].sub_count = bookmark_count;
		for (i = 0; i < bookmark_count; i++) {
			row(page_label(i, bookmarks[i].title, bookmarks[i].address), C_BOOKMARK + i, i ? 0 : UI_MENU_LINE)->icon = ui_icon_of("page");
			removals[i] = rows[row_count - 1];
			removals[i].id = C_REMOVE + i;
			removals[i].flags = 0;
		}
		break;
	case 5:
		row("Keys", C_KEYS, 0);
		row("About this machine", C_ABOUT, 0);
		break;
	}
}

/* NXWEB_PRINT: the rows of the menu just opened, for a test to read. */
static void
menu_say(const char *name)
{
	int i;

	for (i = 0; print && i < row_count; i++)
		printf("nxweb: menu %s %d: %s%s%s\n", name, i, rows[i].label, (rows[i].flags & UI_MENU_GREY) ? " (grey)" : "",
		       (rows[i].flags & UI_MENU_CHECK) ? " (on)" : "");
}

/* Opens a menu of the bar under its name; `keyboard` picks out its first row. */
static void
menu_open(int which, int keyboard)
{
	int x, y;

	ui_window_origin(window, &x, &y);
	menu_make(which);
	menu_at = which;
	draw_menus();
	ui_menu_open(rows, row_count, x + menu_x[which], y + MENU_H, 0, keyboard);
	menu_say(menu_names[which]);
}

/* What a menu's row, a key of the bar, or the key that stands for one does. */
static void
command(int id)
{
	char chosen[512], name[128], again[256], *text;
	FILE *file;
	int n;

	switch (id) {
	case C_ADDRESS:
		field_open(address, 1);
		break;
	case C_PASTE:
		if ((text = ui_clip_get(&n)) == NULL) {
			note("the clipboard is empty");
			break;
		}
		if (focus >= 0 && !editing) {
			/* into the page's field */
			const unsigned char *at = (const unsigned char *)text;
			int used, left = (int)strlen(text), lines = web_nodes[focus].c.tag == T_textarea;
			struct field *f = field_of(focus);

			for (; left > 0; at += used, left -= used) {
				int c = web_char(at, left, &used);

				if (c >= 32 || (lines && c == '\n'))
					field_put(f, c);
			}
			free(text);
			field_again(focus);
			break;
		}
		text[strcspn(text, "\r\n\t")] = 0;
		field_open(text, 0);
		free(text);
		break;
	case C_COPY:
		ui_clip_set(address, strlen(address));
		note("the address is on the clipboard");
		break;
	case C_OPEN:
		if (ui_choose_file("Open a page", "/root", ".html .htm", chosen, sizeof chosen, others) && strlen(chosen) < sizeof address)
			go(chosen, 1);
		break;
	case C_SAVE:
		snprintf(name, sizeof name, "%.100s", strrchr(address, '/') ? strrchr(address, '/') + 1 : "");
		name[strcspn(name, "?#")] = 0;
		if (!strchr(name, '.'))
			strcpy(name, "page.html");
		if (!page_length || !ui_save_file("Save the page as", "/root", NULL, name, chosen, sizeof chosen, others))
			break;
		file = fopen(chosen, "wb");
		/* (what came, when what is shown is that as HTML) */
		n = raw ? file && fwrite(raw, 1, raw_n, file) == (size_t)raw_n : file && fwrite(page, 1, page_length, file) == (size_t)page_length;
		if (file && fclose(file))
			n = 0;
		if (n)
			snprintf(status, sizeof status, "saved as %.180s", chosen);
		else
			snprintf(status, sizeof status, "%.170s could not be written", chosen);
		draw_status();
		break;
	case C_CLOSE:
		cached_remove();
		GrClose();
		exit(0);
	case C_FIND:
		finding = 1;
		editing = 0;
		sought[0] = 0;
		draw_address();
		draw_status();
		break;
	case C_NEXT:
		find_next();
		break;
	case C_STOP:
		stop();
		break;
	case C_RELOAD:
		strcpy(again, address);
		top_wanted = top;
		go(again, 2);
		break;
	case C_PICTURES:
		load_pictures = !load_pictures;
		options_write();
		pictures_again();
		note(load_pictures ? "pictures are loaded" : "pictures are not loaded: the page says what each would be");
		break;
	case C_SOURCE:
		if (!page_length || (file = fopen("/tmp/nxweb-source.html", "wb")) == NULL)
			break;
		if (raw)
			fwrite(raw, 1, raw_n, file);
		else
			fwrite(page, 1, page_length, file);
		fclose(file);
		ui_run("nxedit", "/tmp/nxweb-source.html");
		break;
	case C_OPTIONS:
		options();
		break;
	case C_FAILED:
		failures_list();
		break;
	case C_BACK:
		if (history_at > 0)
			travel(history_at - 1);
		break;
	case C_FORWARD:
		if (history_at + 1 < history_count)
			travel(history_at + 1);
		break;
	case C_HOME:
		go(home, 1);
		break;
	case C_MARK:
		bookmark(address, web_title, 1);
		break;
	case C_KEYS:
		go("about:keys", 1);
		break;
	case C_ABOUT:
		go("/usr/share/web-about.html", 1);
		break;
	case C_LINK:
		go(link_asked, 1);
		break;
	case C_LINK_COPY:
		ui_clip_set(link_asked, strlen(link_asked));
		note("the link's address is on the clipboard");
		break;
	case C_LINK_MARK:
		bookmark(link_asked, "", 0);
		break;
	default:
		if (id >= C_REMOVE && id < C_REMOVE + bookmark_count)
			bookmark(bookmarks[id - C_REMOVE].address, "", 1);
		else if (id >= C_BOOKMARK && id < C_BOOKMARK + bookmark_count)
			go(bookmarks[id - C_BOOKMARK].address, 1);
		else if (id >= C_VISIT && id < C_VISIT + history_count && id - C_VISIT != history_at)
			travel(id - C_VISIT);
	}
}

/* ---- the pointer and the keys ---- */

/* The address of the link at a place of the window, made whole; false where there is none. */
static int
link_at(int x, int y, char *whole)
{
	char href[600];
	int i, link;

	for (i = web_item_count - 1; i >= 0; i--) {
		const struct web_item *it = &web_items[i];

		if (it->kind == I_BOX || !ui_inside(x, y - TOP + top, it->x, it->y, it->w, it->h))
			continue;
		return (it->kind == I_TEXT || it->kind == I_IMAGE) && (link = web_link_of(it->node)) >= 0 &&
		       web_attr_text(link, "href", href, sizeof href) && href[0] != '#' && web_resolve(href, whole);
	}
	return 0;
}

/* The right button's menu: what can be done with the link under it, and with the page. */
static void
context(const GR_EVENT *event)
{
	int x = event->button.x, y = event->button.y, has;

	if (y < TOP || y >= height - STATUS || x >= width - UI_SCROLL_W)
		return;
	has = link_at(x, y, link_asked);
	row_count = 0;
	if (has) {
		row("Open the link", C_LINK, 0);
		row("Copy the link's address", C_LINK_COPY, 0);
		row("Bookmark the link", C_LINK_MARK, 0);
		note(link_asked);
	}
	row("Back", C_BACK, (has ? UI_MENU_LINE : 0) | (history_at > 0 ? 0 : UI_MENU_GREY));
	row("Forward", C_FORWARD, history_at + 1 < history_count ? 0 : UI_MENU_GREY);
	row("Reload", C_RELOAD, 0);
	row(bookmark_of(address) >= 0 ? "Remove this page's bookmark" : "Bookmark this page", C_MARK, UI_MENU_LINE);
	row("Page source", C_SOURCE, page_length ? 0 : UI_MENU_GREY);
	ui_menu_open(rows, row_count, event->button.rootx, event->button.rooty, 0, 0);
	menu_say("pointer");
}

/* A radio button of a form: the others of its name go off. */
static void
radio_choose(int node)
{
	char name[80], other[80], type[24];
	int form = form_of(node), end, i;

	if (form >= 0 && web_attr_text(node, "name", name, sizeof name) && name[0]) {
		for (i = form + 1, end = form_end(form); i < end; i++)
			if (i != node && web_nodes[i].c.tag == T_input && web_attr_text(i, "type", type, sizeof type) && !strcasecmp(type, "radio") &&
			    web_attr_text(i, "name", other, sizeof other) && !strcmp(name, other) && field_of(i)->checked) {
				field_of(i)->checked = 0;
				field_again(i);
			}
	}
	field_of(node)->checked = 1;
	field_again(node);
}

/* What a click, or Space on the keyboard, does to a field that is no text: true if it was one. */
static int
field_press(int node)
{
	const char *type = field_type(node);
	struct field *f = field_of(node);

	if (!strcasecmp(type, "radio")) {
		radio_choose(node);
	} else if (!strcasecmp(type, "checkbox")) {
		f->checked = !f->checked;
		field_again(node);
	} else if (!strcasecmp(type, "select")) {
		int kid, options = 0;

		for (kid = web_nodes[node].first; kid >= 0; kid = web_nodes[kid].next)
			options += web_nodes[kid].c.tag == T_option;
		f->option = options ? (f->option + 1) % options : 0;
		field_again(node);
	} else if (!strcasecmp(type, "file")) {
		char path[256];

		if (ui_choose_file("A file to send", "/root", NULL, path, sizeof path, others)) {
			int k;

			f->n = 0;
			for (k = 0; path[k]; k++)
				field_put(f, (unsigned char)path[k]);
			field_again(node);
		}
	} else
		return 0;
	return 1;
}

static void
click(int x, int y)
{
	static const int tools[TOOLS] = { C_BACK, C_FORWARD, C_RELOAD, C_HOME, C_MARK };
	int i, was = focus;

	if (y < MENU_H) {
		for (i = 0; i < MENUS; i++)
			if (x >= menu_x[i] && x < menu_x[i + 1])
				menu_open(i, 0);
		return;
	}
	if (y < TOP) {
		if (x < FIELD_X) {
			i = (x - 3) / (TOOL_W + 3);
			if (x >= 3 && i < TOOLS && (x - 3) % (TOOL_W + 3) < TOOL_W)
				command(i == 2 && busy() ? C_STOP : tools[i]);
		} else if (editing) {
			offered = 0;	/* a second click: the text is to be added to */
			draw_address();
		} else
			field_open(address, 1);
		return;
	}
	if (editing) {
		editing = 0;
		draw_address();
	}
	if (finding) {
		finding = 0;
		draw_status();
	}
	if (x >= width - UI_SCROLL_W) {
		dragging = 1;
		scroll_to(ui_scroll_to(y, TOP, view_height(), web_page_h, view_height()), 1);
		return;
	}
	if (y >= height - STATUS) {
		if (failures(NULL) && !shown_busy)
			failures_list();
		return;
	}
	focus = -1;
	/* the thing drawn last at that place is the one on top */
	for (i = web_item_count - 1; i >= 0; i--) {
		const struct web_item *it = &web_items[i];
		int node = it->node, link, up, disabled_n;
		const char *disabled;
		char href[600];

		if (it->kind == I_BOX || !ui_inside(x, y - TOP + top, it->x, it->y, it->w, it->h))
			continue;
		if (it->kind == I_FIELD) {
			const char *type = field_type(node);

			if (web_attr(node, "disabled", &disabled, &disabled_n))
				break;
			if (field_is_button(type)) {
				submit(node, node);
			} else if (!field_press(node)) {
				focus = node;
				field_again(node);
			}
			break;
		}
		for (up = node; up >= 0; up = web_nodes[up].c.parent) {
			if (web_nodes[up].c.tag == T_summary && web_nodes[up].c.parent >= 0) {
				web_nodes[web_nodes[up].c.parent].flags ^= N_OPEN;
				lay_out(0, 0);
				show_view();
				return;
			}
			if (web_nodes[up].c.tag == T_button) {
				submit(up, up);
				return;
			}
		}
		if ((link = web_link_of(node)) >= 0 && web_attr_text(link, "href", href, sizeof href)) {
			follow(href);
			return;
		}
		break;
	}
	if (was >= 0 && was != focus)
		field_again(was);
}

/* Moves the keyboard to the page's next link or field, or the one before, and brings it into view.
 * Past the last one nothing has it, and the next press starts over. */
static void
pick_link(int back)
{
	int i, at = -1, link_was = picked, focus_was = focus, y = -1, step = back ? -1 : 1;
	char href[600];

	for (i = 0; i < web_item_count && at < 0; i++)
		if (focus >= 0 ? web_items[i].kind == I_FIELD && web_items[i].node == focus :
		    picked >= 0 && (web_items[i].kind == I_TEXT || web_items[i].kind == I_IMAGE) && web_link_of(web_items[i].node) == picked)
			at = i;
	focus = picked = -1;
	for (i = at >= 0 ? at + step : back ? web_item_count - 1 : 0; i >= 0 && i < web_item_count; i += step) {
		int link = web_items[i].kind == I_TEXT || web_items[i].kind == I_IMAGE ? web_link_of(web_items[i].node) : -1;

		if (web_items[i].kind == I_FIELD) {
			focus = web_items[i].node;
			y = web_items[i].y;
			break;
		}
		if (link >= 0 && link != link_was) {
			picked = link;
			y = web_items[i].y;
			break;
		}
	}
	if (focus_was >= 0)
		field_again(focus_was);
	if (picked >= 0 || link_was >= 0)
		strips_forget();
	if (picked >= 0 && web_attr_text(picked, "href", href, sizeof href)) {
		snprintf(status, sizeof status, "%.190s", href);
		draw_status();
	}
	if (y >= 0 && (y < top || y + line_h > top + view_height()))
		top = goal = web_most(y - view_height() / 3, 0);
	if (focus >= 0)
		field_again(focus);
	else
		show_view();
}

/* The command a letter with Ctrl is, or 0. */
static int
command_of(int letter)
{
	static const char letters[] = "losqwcvfdur";
	static const int ids[] = { C_ADDRESS, C_OPEN, C_SAVE, C_CLOSE, C_CLOSE, C_COPY, C_PASTE, C_FIND, C_MARK, C_SOURCE, C_RELOAD };
	const char *at = letter ? strchr(letters, letter) : NULL;

	return at ? ids[at - letters] : 0;
}

/* Keys: the menus' own (they say which), then with the address field or one of the page's
 * fields open they are typed into it. Otherwise arrows and the paging keys scroll, Tab goes
 * from link to link and Enter follows, Backspace goes back, Escape stops. */
static void
key(int ch, int modifiers)
{
	int n = strlen(typed), view = view_height(), letter = 0;

	if (finding) {
		find_key(ch);
		return;
	}
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((modifiers & MWKMOD_CTRL) && ch < 128 && isalpha(ch))
		letter = tolower(ch);
	else if (ch >= 1 && ch <= 26 && ch != MWKEY_BACKSPACE && ch != MWKEY_TAB && ch != '\n' && ch != MWKEY_ENTER)
		letter = 'a' + ch - 1;
	if (letter) {
		if (command_of(letter))
			command(command_of(letter));
		return;
	}
	switch (ch) {
	case MWKEY_F3: command(C_NEXT); return;
	case MWKEY_F5: command(C_RELOAD); return;
	case MWKEY_F6: command(C_ADDRESS); return;
	case MWKEY_F10: menu_open(0, 1); return;
	case MWKEY_LEFT: case MWKEY_RIGHT: case MWKEY_HOME:
		if (!(modifiers & MWKMOD_ALT))
			break;
		command(ch == MWKEY_LEFT ? C_BACK : ch == MWKEY_RIGHT ? C_FORWARD : C_HOME);
		return;
	}
	if (focus >= 0 && !editing) {
		struct field *f = field_of(focus);

		const char *fixed, *type = field_type(focus);
		int lines = web_nodes[focus].c.tag == T_textarea, most = web_attr_int(focus, "maxlength", 32767), fixed_n;

		if (field_is_button(type) && (ch == MWKEY_ENTER || ch == '\n' || ch == '\r' || ch == ' ')) {
			submit(focus, focus);
			return;
		}
		if (ch == ' ' && field_press(focus))
			return;
		if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
			if (lines && !web_attr(focus, "readonly", &fixed, &fixed_n)) {
				field_put(f, '\n');
				field_again(focus);
				return;
			}
			submit(focus, -1);
			return;
		}
		if (ch == MWKEY_TAB) {
			pick_link(modifiers & MWKMOD_SHIFT);
			return;
		}
		if (ch == MWKEY_ESCAPE) {
			int was = focus;

			focus = -1;
			field_again(was);
			return;
		}
		if (web_attr(focus, "readonly", &fixed, &fixed_n) || field_is_button(type) || !strcasecmp(type, "checkbox") || !strcasecmp(type, "radio") ||
		    !strcasecmp(type, "select") || !strcasecmp(type, "file"))
			return;
		if ((ch == MWKEY_BACKSPACE || ch == 127) && f->n > 0)
			f->n--;
		else if (ch >= 32 && ch < 127 && f->n < most)
			field_put(f, ch);
		else
			return;
		field_again(focus);
		return;
	}
	if (!editing) {
		switch (ch) {
		case MWKEY_TAB: pick_link(modifiers & MWKMOD_SHIFT); break;
		case MWKEY_ENTER:
			if (picked >= 0) {
				char href[600];

				if (web_attr_text(picked, "href", href, sizeof href))
					follow(href);
			}
			break;
		case MWKEY_UP: scroll_to(goal - 3 * line_h, 0); break;
		case MWKEY_DOWN: scroll_to(goal + 3 * line_h, 0); break;
		case MWKEY_PAGEUP: scroll_to(goal - view + line_h, 0); break;
		case MWKEY_PAGEDOWN: case ' ': scroll_to(goal + view - line_h, 0); break;
		case MWKEY_HOME: scroll_to(0, 0); break;
		case MWKEY_END: scroll_to(web_page_h, 0); break;
		case MWKEY_BACKSPACE: command(C_BACK); break;
		case MWKEY_ESCAPE:
			if (busy())
				stop();
			else if (found >= 0) {
				found = -1;
				strips_forget();
				show_view();
			}
			break;
		}
		return;
	}
	if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r') {
		char where[280];
		const char *space = typed + strspn(typed, " ");

		if (strchr(space, ' ') && strspn(space + strcspn(space, " "), " ") != strlen(space + strcspn(space, " "))) {
			note("an address has no spaces in it (this browser has no search of its own: a site's page has)");
			return;
		}
		typed_address(typed, where, sizeof where);
		go(where, 1);
		return;
	}
	if (ch == MWKEY_ESCAPE)
		editing = 0;
	else if (ch == MWKEY_BACKSPACE || ch == 127)
		typed[n ? n - 1 : 0] = 0;
	else if (ch >= 32 && ch < 127 && n < 250) {
		n = offered ? 0 : n;
		typed[n] = ch, typed[n + 1] = 0;
	}
	offered = 0;
	draw_address();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	GR_FONT_INFO info;
	int fd, id;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	signal(SIGCHLD, SIG_IGN);	/* (the editor, started for a page's source) */
	verbose = getenv("NXWEB_TIME") != NULL;
	dump = getenv("NXWEB_DUMP") != NULL;
	print = getenv("NXWEB_PRINT") != NULL;
	int c, plain;

	GrGetFontInfo(ui_var, &info);
	/* (the same answer is what ui_width would ask for at the first label) */
	for (c = 0; c < 256; c++)
		ui_var_widths.of[c] = info.widths[c >= info.firstchar && c <= info.lastchar ? c : info.firstchar];
	ui_var_widths.known = 1;
	line_h = (info.height > ui_fh ? info.height : ui_fh) + 2;
	for (c = 0, menu_x[0] = 2; c < MENUS; c++)
		menu_x[c + 1] = menu_x[c] + ui_width(&ui_var_widths, ui_var, menu_names[c], -1) + 16;
	kept_read();
	pictures = malloc(MAX_PICTURES * sizeof *pictures);
	fd = open("/dev/gpu", O_RDWR);
	if (fd >= 0) {
		void *words = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
		void *data = mmap(NULL, FETCH_MOST, PROT_READ, MAP_SHARED, fd, 0x016c0000);

		if (words != MAP_FAILED && data != MAP_FAILED) {
			regs = words;
			fetched = data;
			gpu_fd = fd;	/* (kept: the strips' lists are mapped through it) */
		}
	}
	{
		/* the ROM's fonts, if the image has them and the window system draws them (its font
		 * of such a file says it has every character); else the window system's own */
		static unsigned char widths[256];
		GR_FONT_INFO sef;
		GR_FONT_ID test;

		plain = web_face_for(0, 0, CSS_EM);
		test = web_face(plain)->file ? GrCreateFontEx("/usr/share/font-sans-13.sef", 0, 0, NULL) : 0;

		memset(&sef, 0, sizeof sef);
		if (test)
			GrGetFontInfo(test, &sef);
		if (!test || sef.lastchar != 0xffff || getenv("NXWEB_PLAIN")) {
			fprintf(stderr, "nxweb: the window system's own font (no fonts of the ROM: tools/make_fonts.py, docs/system/fonts.md)\n");
			memcpy(widths, info.widths, sizeof widths);
			memset(web_faces, 0, sizeof web_faces);
			web_plain = widths;
			web_plain_height = info.height;
			web_plain_ascent = info.baseline;
		} else if (!strcmp(web_faces[plain].name, "font-sans-13.sef"))
			fonts[plain][0] = test;	/* (the page's plain face is this file: not made a second time) */
	}
	/* (the elements' styles on workers too; NXWEB_STYLES=here: on this core alone) */
	web_style_elsewhere = getenv("NXWEB_STYLES") && !strcmp(getenv("NXWEB_STYLES"), "here") ? NULL : styles_elsewhere;
	web_page_bg = 0xffffff;	/* (a view before the first page comes) */
	if (argc > 1 && access(argv[1], F_OK)) {
		/* an address as one would type it (not a file of the folder it was started in) */
		char where[280];

		typed_address(argv[1], where, sizeof where);
		go(where, 0);
	} else
		go(argc > 1 ? argv[1] : home, 0);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Web", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, WHITE);
	set_title();
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		int menu, wait = top != goal ? 15 : loading == -1 && net.state < NET_HEAD ? 30 : loading ? 100 : by_hand ? 3000 : regs && wanted_open() ? 400 : 60000;

		/* (the pictures waited for are decoded, not in a scroll or with an event there, then the
		 * strips ahead of the view made, while the wait lasts) */
		if (window && top == goal && !GrPeekEvent(&event) && pictures_meanwhile()) {
			show_view();
			if (verbose && !pictures_waiting()) {
				struct timespec t;

				clock_gettime(CLOCK_MONOTONIC, &t);
				printf("nxweb: the pictures waited for %ld ms after the page's text; decoded in %u thousand instructions\n",
				       (long)((t.tv_sec - page_began_at.tv_sec) * 1000 + (t.tv_nsec - page_began_at.tv_nsec) / 1000000),
				       pictures_decoding / 1000);
			}
			wait = 0;
		}
		wait = strips_meanwhile(wait);
		net_watch();
		GrGetNextEventTimeout(&event, wait ? wait : GR_TIMEOUT_POLL);
		ui_origin_note(&event);
		if (menu_at >= 0 && ui_menu_depth == 1 && event.type == GR_EVENT_TYPE_KEY_DOWN &&
		    (event.keystroke.ch == MWKEY_LEFT || (event.keystroke.ch == MWKEY_RIGHT &&
		     !(ui_menu_level[0].hover >= 0 && ui_menu_level[0].items[ui_menu_level[0].hover].sub)))) {
			/* Left and Right in a menu of the bar: the menu beside it */
			menu_open((menu_at + (event.keystroke.ch == MWKEY_LEFT ? MENUS - 1 : 1)) % MENUS, 1);
			menu = UI_MENU_TOOK;
		} else
			menu = ui_menu_event(&event, &id);
		if (menu == UI_MENU_CHOSEN || menu == UI_MENU_CLOSED) {
			int was = menu_at;

			menu_at = -1;
			if (was >= 0)
				draw_menus();
			GrSetFocus(window);
			if (menu == UI_MENU_CHOSEN)
				command(id);
		}
		switch (menu ? GR_EVENT_TYPE_NONE : event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && event.update.wid == window)
				resized(event.update.width, event.update.height);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event))
				scroll_to(goal + 3 * line_h * ui_wheel_sum(&event), 0);
			else if (event.button.changebuttons & GR_BUTTON_R)
				context(&event);
			else {
				click(event.button.x, event.button.y);
				ui_moves(window, EVENTS, dragging);	/* (the pointer's moves, while the scroll bar is held) */
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = 0;
			ui_moves(window, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L))
				scroll_to(ui_scroll_to(event.mouse.y, TOP, view_height(), web_page_h, view_height()), 1);
			else {
				dragging = 0;
				ui_moves(window, EVENTS, 0);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid != window)	/* (a menu's may come after the menu went) */
				break;
			cached_remove();
			GrClose();
			return 0;
		}
		if (print && (menu == UI_MENU_CHOSEN || (!menu && (event.type == GR_EVENT_TYPE_KEY_DOWN || event.type == GR_EVENT_TYPE_BUTTON_DOWN)))) {
			int i;

			printf("nxweb: at %s, page %d of %d, top %d, %d bookmarks, home %s, pictures %d, typing %d \"%s\", status \"%s\"\n", address,
			       history_at + 1, history_count, top, bookmark_count, home, load_pictures, editing, editing ? typed : "", status);
			/* where the link the keyboard is on stands in the window, for a test to click */
			for (i = 0; picked >= 0 && i < web_item_count; i++)
				if (web_items[i].kind == I_TEXT && web_link_of(web_items[i].node) == picked) {
					printf("nxweb: the link picked is at %d,%d\n", web_items[i].x + 2, web_items[i].y - top + TOP + 2);
					break;
				}
			fflush(stdout);
		}
		if (check_due && window) {
			check_due = 0;
			strips_check();
			show_view();
		}
		ticked = event.type == GR_EVENT_TYPE_TIMEOUT && top == goal;
		if (top != goal)
			scroll_step();
		if (web_refresh[0] && !loading && sent_on < 3) {
			/* the page sends its reader on (three times in a row at most) */
			char where[256];

			strcpy(where, web_refresh);
			web_refresh[0] = 0;
			sent_on++;
			go(where, 0);
		}
		if (loading)
			check_reply();
		else if (by_hand && event.type == GR_EVENT_TYPE_TIMEOUT)
			ask(address, 0);
		else if (!by_hand && regs && (asking = next_wanted()) >= SHEET)
			request(web_sheets[asking - SHEET].address, 2);
		else if (!by_hand && regs && asking >= 0)
			request(pictures[asking].address, 1);
		if (failed_due && window)
			failures_show();
		if (top == goal)
			loading_shown(event.type == GR_EVENT_TYPE_TIMEOUT);	/* (not a scroll's steps: they are counted) */
	}
}
