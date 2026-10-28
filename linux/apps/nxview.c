/* nxview: a picture viewer in the manner of Windows Photo Viewer (docs/system/nanox.md): any picture
 * libmedia reads (JPEG, PNG, GIF and its animations, BMP, TGA, PNM) or XPM, under a bar of
 * commands and over a row of tools (zoom, actual size, the folder's pictures, a slide show,
 * turns, Delete). Fitted, a picture is decoded on the worker cores into a pixmap the GPU keeps;
 * zoomed or turned, the part in view is made from its pixels kept in memory. */
#define _GNU_SOURCE
#define UI_PICTURES
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_ask.h"
#include "ui_glyph.h"

enum { NONE, PPM, CODED, XPM, ANIM };	/* how the picture is shown */

static GR_WINDOW_ID window, full, view;	/* the framed window, the one over the whole screen, the one drawn */
static int width, height;		/* the framed window's inside */
static int room_x, room_y, room_w, room_h;	/* where the picture goes in `view` */
static char path[512], notice[160];
static const char *wrong;		/* why the picture cannot be shown */
static int kind, pic_w, pic_h;		/* the picture's own size, unturned */
static long file_bytes;
static GR_IMAGE_ID image;		/* an XPM, the engine's */
static struct media_image pic;		/* its pixels (3 a pixel), for a zoom or a turn */
static int pic_turns, pic_mine;		/* the turns `pic` has; its pixels are ours to free */
static int turns;			/* quarter turns to the right it is shown with */
static int fitted = 1;			/* shown as large as fits (never larger than its own size) */
static float scale = 1, ox, oy;		/* the screen's pixels to a picture's, and its top left in the room */
static GR_WINDOW_ID zoom_pixmap;	/* the part in view, made at the size it is shown */
static int zoom_pw, zoom_ph;
static int held_workers;
static int closing;			/* the window's box was pressed while a file was being chosen */
static int pic_failed;			/* its pixels could not be had: not tried again at every redraw */
static int hidden;			/* the framed window is minimised: an animation waits */
static int slide;			/* the slide show runs: 2 while paused */
static long slide_due, anim_due;
static long last_click;
static int last_x, last_y;
/* an animation: its file, its pictures and the one shown */
static struct media_anim *anim;
static uint8_t *anim_file;
static uint32_t anim_size;
static int anim_frame, anim_delay;
static GR_WINDOW_ID anim_pixmap;
static int anim_w, anim_h;
/* the pictures of the folder, by name */
static char folder[512];
static char **names;
static int count, at = -1;

#define TOP 28
#define TOOLS 40
#define STATUS 20
#define MAXS 16.0f		/* the most a picture is zoomed: 1,600% */
#define PIC_MOST (8 << 20)	/* the most pixels kept in memory for a zoom or a turn */
#define DARK MWRGB(40, 40, 40)
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | \
	GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP)

static long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int
workers_asked(void)
{
	const char *asked = getenv("NXVIEW_WORKERS");

	return asked ? atoi(asked) : mcw_most();	/* as many as the machine has (and the picture has work for) */
}

/* Decodes a picture file into a PPM file and says what that took. */
static const char *
decode(const char *from, const char *to, int *pw, int *ph)
{
	const char *why;
	int workers = workers_asked();
	struct timeval then, now;
	unsigned then_n, now_n;

	gettimeofday(&then, NULL);
	__asm__ volatile("rdcycle %0" : "=r"(then_n));
	why = media_image_to_ppm(from, to, pw, ph, workers);
	__asm__ volatile("rdcycle %0" : "=r"(now_n));
	gettimeofday(&now, NULL);
	if (!why) {
		/* a sum of the pixels, for a test to compare with the same decoder's on another machine */
		unsigned sum = 0, n = 0;
		FILE *file = getenv("NXVIEW_SUM") ? fopen(to, "rb") : NULL;
		int c;

		while (file && (c = fgetc(file)) != EOF)
			sum = (sum << 5 | sum >> 27) ^ (unsigned)c, n++;
		if (file)
			fclose(file);
		fprintf(stderr, "nxview: %s, %d x %d: decoded in %ld ms, %u thousand instructions on this core, asking for %d workers",
			from, *pw, *ph, (long)((now.tv_sec - then.tv_sec) * 1000 + (now.tv_usec - then.tv_usec) / 1000), (now_n - then_n) / 1000, workers);
		if (file)
			fprintf(stderr, "; sum %08x of %u bytes", sum, n);
		fprintf(stderr, "\n");
	}
	return why;
}

/* Whether a file is decoded to be shown: every kind libmedia reads but a PPM the GPU draws as it lies. */
static int
is_coded(const char *name)
{
	int pw, ph, fd;
	unsigned char head[4] = "";

	if (ui_picture_size(name, &pw, &ph))
		return 0;
	if (media_kind_of_name(name) != MEDIA_UNKNOWN)
		return 1;
	/* (a name with no ending of a picture's, as `open` may give: its first bytes) */
	if ((fd = open(name, O_RDONLY)) >= 0) {
		if (read(fd, head, sizeof head) < 4)
			head[0] = 0;
		close(fd);
	}
	return !memcmp(head, "\x89PNG", 4) || (head[0] == 0xff && head[1] == 0xd8) || !memcmp(head, "GIF8", 4) ||
	       (head[0] == 'B' && head[1] == 'M') || (head[0] == 'P' && head[1] >= '1' && head[1] <= '5');
}

static const char *
leaf_of(const char *file)
{
	const char *slash = strrchr(file, '/');

	return slash ? slash + 1 : file;
}

/* ---- the picture's pixels, kept for a zoom or a turn ---- */

static void
forget_pic(void)
{
	if (pic_mine)
		free(pic.pixels);
	else if (pic.pixels)
		media_image_free(&pic);
	memset(&pic, 0, sizeof pic);
	pic_mine = pic_turns = 0;
}

/* The workers stay open while a picture is zoomed or plays: opening them is most of a redraw. */
static void
hold_workers(int on)
{
	if (on && !held_workers && !mcw_count())
		held_workers = mcw_open(workers_asked()) > 0;
	else if (!on && held_workers)
		mcw_close(), held_workers = 0;
}

static int
need_pic(void)
{
	const char *why;

	if (pic.pixels)
		return 1;
	if (kind != PPM && kind != CODED)
		return 0;
	if ((long long)pic_w * pic_h > PIC_MOST) {
		snprintf(notice, sizeof notice, "too large to zoom or turn");
		return 0;
	}
	if (pic_failed)
		return 0;
	hold_workers(1);
	memset(&pic, 0, sizeof pic);
	if ((why = media_image_load(path, &pic, 3, workers_asked())) != NULL) {
		snprintf(notice, sizeof notice, "%s", why);
		memset(&pic, 0, sizeof pic);
		pic_failed = 1;
		return 0;
	}
	return 1;
}

/* The kept pixels turned to `turns`: a new buffer, the old one freed. */
static int
turn_pic(void)
{
	int by = (turns - pic_turns) & 3, w = pic.width, h = pic.height, nw, nh, stride, x, y;
	uint8_t *to;

	if (!by)
		return 1;
	nw = by & 1 ? h : w, nh = by & 1 ? w : h;
	stride = (nw * 3 + 15) & ~15;
	if (!(to = malloc((size_t)stride * (size_t)nh))) {
		snprintf(notice, sizeof notice, "not enough memory to turn it");
		return 0;
	}
	for (y = 0; y < nh; y++)
		for (x = 0; x < nw; x++) {
			int sx = by == 1 ? y : by == 2 ? w - 1 - x : w - 1 - y, sy = by == 1 ? h - 1 - x : by == 2 ? h - 1 - y : x;
			const uint8_t *s = pic.pixels + (size_t)sy * (size_t)pic.stride + (size_t)sx * 3;
			uint8_t *d = to + (size_t)y * (size_t)stride + (size_t)x * 3;

			d[0] = s[0], d[1] = s[1], d[2] = s[2];
		}
	forget_pic();
	pic.width = nw, pic.height = nh, pic.channels = 3, pic.stride = stride, pic.pixels = to;
	pic_mine = 1, pic_turns = turns;
	return 1;
}

/* ---- the size it is shown at ---- */

static int
shown_w(void)
{
	return turns & 1 ? pic_h : pic_w;
}

static int
shown_h(void)
{
	return turns & 1 ? pic_w : pic_h;
}

/* (one too large to keep is only fitted: zoomed, its size in pixels would pass an int's) */
static int
can_zoom(void)
{
	return (kind == PPM || kind == CODED) && (long long)pic_w * pic_h <= PIC_MOST;
}

static float
fit_scale(void)
{
	float s, t;

	if (shown_w() < 1 || shown_h() < 1 || room_w < 1 || room_h < 1)
		return 1;
	s = (float)room_w / shown_w(), t = (float)room_h / shown_h();
	s = t < s ? t : s;
	return s > 1 ? 1 : s;
}

/* The picture kept in its room: in the middle where it is smaller, never leaving a gap where larger. */
static void
place(void)
{
	float dw, dh;

	if (fitted || !can_zoom())
		scale = fit_scale();
	dw = shown_w() * scale, dh = shown_h() * scale;
	if (dw <= room_w)
		ox = (room_w - dw) / 2;
	else if (ox > 0)
		ox = 0;
	else if (ox < room_w - dw)
		ox = room_w - dw;
	if (dh <= room_h)
		oy = (room_h - dh) / 2;
	else if (oy > 0)
		oy = 0;
	else if (oy < room_h - dh)
		oy = room_h - dh;
}

/* Zooms to `s` with the picture's point under px, py (the room's) kept there. */
static void
zoom_to(float s, float px, float py)
{
	float least = fit_scale(), old;

	if (!can_zoom()) {
		snprintf(notice, sizeof notice, kind == ANIM ? "an animation is shown as large as fits" : "this picture is not zoomed");
		return;
	}
	place();
	old = scale;
	s = s > MAXS ? MAXS : s;
	if (s <= least * 1.001f) {
		fitted = 1;
		return;
	}
	if ((old < 0.999f && s > 1.001f) || (old > 1.001f && s < 0.999f))
		s = 1;	/* (a step past the picture's own size stops at it) */
	fitted = 0;
	ox = px - (px - ox) * s / old;
	oy = py - (py - oy) * s / old;
	scale = s;
	place();
}

static void
zoom_by(float factor)
{
	place();
	zoom_to(scale * factor, room_w / 2.0f, room_h / 2.0f);
}

/* ---- the layout ---- */

enum { T_ZOOM_OUT, T_ZOOM_IN, T_ACTUAL, T_PREV, T_SLIDE, T_NEXT, T_TURN_LEFT, T_TURN_RIGHT, T_DELETE, TOOL_COUNT };
static int tool_x[TOOL_COUNT], tool_w[TOOL_COUNT], tools_y;
static const char *const commands[] = { "Open", "Edit", "Set as desktop" };
#define COMMANDS 3
static int command_x[COMMANDS], command_w[COMMANDS];

static void
layout(void)
{
	int i, x, w;

	if (full) {
		const GR_SCREEN_INFO *screen = ui_screen();

		room_x = room_y = 0, room_w = screen->cols, room_h = screen->rows;
		return;
	}
	room_x = 0, room_y = TOP, room_w = width, room_h = height - TOP - TOOLS - STATUS;
	room_h = room_h < 1 ? 1 : room_h;
	for (i = 0, x = 6; i < COMMANDS; i++) {
		command_x[i] = x, command_w[i] = ui_width(&ui_var_widths, ui_var, commands[i], -1) + 16;
		x += command_w[i] + 4;
	}
	/* the tools in the middle of their row, in three groups and Delete */
	for (i = 0, w = 0; i < TOOL_COUNT; i++) {
		tool_w[i] = i == T_SLIDE ? 44 : 32;
		w += tool_w[i] + (i == T_ACTUAL || i == T_NEXT || i == T_TURN_RIGHT ? 16 : 2);
	}
	for (i = 0, x = (width - w) / 2; i < TOOL_COUNT; i++) {
		tool_x[i] = x;
		x += tool_w[i] + (i == T_ACTUAL || i == T_NEXT || i == T_TURN_RIGHT ? 16 : 2);
	}
	tools_y = room_y + room_h;
}

static int
tool_greyed(int t)
{
	switch (t) {
	case T_ZOOM_OUT: case T_ZOOM_IN: case T_ACTUAL:
		return !can_zoom();
	case T_TURN_LEFT: case T_TURN_RIGHT:
		return kind != PPM && kind != CODED;
	case T_PREV: case T_NEXT: case T_SLIDE:
		return count < 1;
	case T_DELETE:
		return !path[0] || access(path, F_OK) != 0;
	}
	return 0;
}

/* ---- drawing ---- */

static uint32_t
rgb_of(GR_COLOR c)
{
	return (uint32_t)(c & 255) << 16 | (c & 0xff00) | (c >> 16 & 255);
}

static int
rnd(float v)
{
	return (int)floorf(v + 0.5f);
}

/* The room but the rectangle x, y, w, h (the room's), in the background colour. */
static void
around(int x, int y, int w, int h, GR_COLOR bg)
{
	int x1 = x + w, y1 = y + h;

	x = x < 0 ? 0 : x, y = y < 0 ? 0 : y;
	x1 = x1 > room_w ? room_w : x1, y1 = y1 > room_h ? room_h : y1;
	if (x1 <= x || y1 <= y) {
		ui_fill(view, room_x, room_y, room_w, room_h, bg);
		return;
	}
	ui_fill(view, room_x, room_y, room_w, y, bg);
	ui_fill(view, room_x, room_y + y1, room_w, room_h - y1, bg);
	ui_fill(view, room_x, room_y + y, x, y1 - y, bg);
	ui_fill(view, room_x + x1, room_y + y, room_w - x1, y1 - y, bg);
}

/* Lines of text in the middle of the room. */
static void
room_text(const char *first, const char *second, GR_COLOR bg)
{
	int w;

	ui_fill(view, room_x, room_y, room_w, room_h, bg);
	w = ui_width(&ui_var_widths, ui_var, first, -1);
	ui_text(view, room_x + (room_w - w) / 2, room_y + room_h / 2 - (second ? 16 : 7), first, -1, WHITE, 0);
	if (second) {
		w = ui_width(&ui_var_widths, ui_var, second, -1);
		ui_text(view, room_x + (room_w - w) / 2, room_y + room_h / 2 + 4, second, -1, MWRGB(200, 200, 200), 0);
	}
}

/* The animation's picture `anim_frame`, fitted, through a pixmap the GPU keeps. */
static void
draw_anim(int x, int y, int w, int h, GR_COLOR bg)
{
	struct media_image in, out;

	if (w < 1 || h < 1)
		return;
	if (anim_pixmap && (anim_w != w || anim_h != h)) {
		GrDestroyWindow(anim_pixmap);
		anim_pixmap = 0;
	}
	if (!anim_pixmap) {
		if (!(anim_pixmap = ui_gpu_pixmap(w, h, &out)))
			return;
		anim_w = w, anim_h = h;
	} else if (!ui_gpu_map(anim_pixmap, w, h, &out)) {
		return;
	}
	memset(&in, 0, sizeof in);
	in.width = pic_w, in.height = pic_h, in.channels = 4, in.stride = pic_w * 4;
	in.pixels = (uint8_t *)media_anim_frame(anim, anim_frame, &anim_delay);
	if (in.pixels) {
		out.background = rgb_of(bg);
		media_image_fit(&in, &out, 0, workers_asked());
	}
	ui_gpu_unmap(&out);
	/* (the room round it, not under it: the wait for the GPU above would show that) */
	around(x, y, w, h, bg);
	GrCopyArea(view, ui_gc, room_x + x, room_y + y, w, h, anim_pixmap, 0, 0, MWROP_COPY);
}

/* Zoomed or turned: the part of the kept pixels in view, made at the size it is shown. */
static void
draw_part(GR_COLOR bg)
{
	struct media_image in, out;
	int vx0, vy0, vx1, vy1, sx0, sy0, sx1, sy1, cx0, cy0, cw, ch, dw, dh;

	if (!need_pic() || !turn_pic()) {
		room_text("This picture cannot be zoomed or turned", notice, bg);
		return;
	}
	dw = rnd(shown_w() * scale), dh = rnd(shown_h() * scale);
	vx0 = rnd(ox) > 0 ? rnd(ox) : 0, vy0 = rnd(oy) > 0 ? rnd(oy) : 0;
	vx1 = rnd(ox) + dw < room_w ? rnd(ox) + dw : room_w, vy1 = rnd(oy) + dh < room_h ? rnd(oy) + dh : room_h;
	if (vx1 <= vx0 || vy1 <= vy0)
		return;
	/* the picture's pixels under that, whole ones, and where they lie */
	sx0 = (int)floorf((vx0 - ox) / scale), sy0 = (int)floorf((vy0 - oy) / scale);
	sx1 = (int)ceilf((vx1 - ox) / scale), sy1 = (int)ceilf((vy1 - oy) / scale);
	sx0 = sx0 < 0 ? 0 : sx0, sy0 = sy0 < 0 ? 0 : sy0;
	sx1 = sx1 > pic.width ? pic.width : sx1, sy1 = sy1 > pic.height ? pic.height : sy1;
	if (sx1 <= sx0 || sy1 <= sy0)
		return;
	cx0 = rnd(ox + sx0 * scale), cy0 = rnd(oy + sy0 * scale);
	cw = rnd(ox + sx1 * scale) - cx0, ch = rnd(oy + sy1 * scale) - cy0;
	cx0 = cx0 > vx0 ? vx0 : cx0, cy0 = cy0 > vy0 ? vy0 : cy0;
	cw = cx0 + cw < vx1 ? vx1 - cx0 : cw, ch = cy0 + ch < vy1 ? vy1 - cy0 : ch;
	if (zoom_pixmap && (zoom_pw < cw || zoom_ph < ch)) {
		GrDestroyWindow(zoom_pixmap);
		zoom_pixmap = 0;
	}
	if (!zoom_pixmap) {
		zoom_pw = cw > room_w + 40 ? cw : room_w + 40, zoom_ph = ch > room_h + 40 ? ch : room_h + 40;
		if (!(zoom_pixmap = ui_gpu_pixmap(zoom_pw, zoom_ph, &out))) {
			room_text("No memory of the GPU's to show it zoomed", NULL, bg);
			return;
		}
		ui_gpu_unmap(&out);
	}
	if (!ui_gpu_map(zoom_pixmap, cw, ch, &out))
		return;
	in = pic;
	in.pixels = pic.pixels + (size_t)sy0 * (size_t)pic.stride + (size_t)sx0 * 3;
	in.width = sx1 - sx0, in.height = sy1 - sy0;
	out.background = rgb_of(bg);
	media_image_fit(&in, &out, 1, workers_asked());	/* (cover: a part's rounding leaves no edge of background) */
	ui_gpu_unmap(&out);
	around(vx0, vy0, vx1 - vx0, vy1 - vy0, bg);
	GrCopyArea(view, ui_gc, room_x + vx0, room_y + vy0, vx1 - vx0, vy1 - vy0, zoom_pixmap, vx0 - cx0, vy0 - cy0, MWROP_COPY);
}

static void
draw_room(void)
{
	GR_COLOR bg = full ? BLACK : DARK;
	int x, y, w, h;

	place();
	x = rnd(ox), y = rnd(oy), w = rnd(shown_w() * scale), h = rnd(shown_h() * scale);
	w = w < 1 ? 1 : w, h = h < 1 ? 1 : h;
	switch (kind) {
	case NONE:
		if (wrong)
			room_text("Viewer cannot open this picture", wrong, bg);
		else if (path[0])
			room_text("Viewer cannot open this picture", "it is no picture this viewer reads", bg);
		else
			room_text("No picture", "Open has the pictures (the o key)", bg);
		return;
	case ANIM:
		draw_anim(x, y, w, h, bg);
		return;
	case XPM:
		around(x, y, w, h, bg);
		GrDrawImageToFit(view, ui_gc, room_x + x, room_y + y, w, h, image);
		return;
	}
	if (fitted && !turns) {
		if (kind == PPM) {
			around(x, y, w, h, bg);
			GrDrawImageFromFile(view, ui_gc, room_x + x, room_y + y, w, h, path, 0);
			return;
		}
		around(x, y, w, h, bg);
		if (ui_picture_decoded_at(view, path, room_x + x, room_y + y, w, h, bg, 0))
			return;
	}
	draw_part(bg);
}

/* A sign of the viewer's own, or one of ui_glyph.h's, with its middle at cx, cy. */
static void
sign(int t, int cx, int cy, GR_COLOR c)
{
	int k;

	GrSetGCForeground(ui_gc, c);
	switch (t) {
	case T_ZOOM_OUT:
	case T_ZOOM_IN:
		/* a lens with a handle, and in it the sign */
		GrLine(view, ui_gc, cx - 4, cy - 7, cx + 1, cy - 7);
		GrLine(view, ui_gc, cx - 4, cy + 2, cx + 1, cy + 2);
		GrLine(view, ui_gc, cx - 6, cy - 5, cx - 6, cy);
		GrLine(view, ui_gc, cx + 3, cy - 5, cx + 3, cy);
		GrPoint(view, ui_gc, cx - 5, cy - 6), GrPoint(view, ui_gc, cx + 2, cy - 6);
		GrPoint(view, ui_gc, cx - 5, cy + 1), GrPoint(view, ui_gc, cx + 2, cy + 1);
		for (k = 0; k < 3; k++)
			GrLine(view, ui_gc, cx + 3 + k, cy + 2 + k - 1, cx + 6 + k, cy + 5 + k - 1);
		GrFillRect(view, ui_gc, cx - 4, cy - 3, 6, 2);
		if (t == T_ZOOM_IN)
			GrFillRect(view, ui_gc, cx - 2, cy - 5, 2, 6);
		break;
	case T_ACTUAL:
		if (fitted && fit_scale() < 0.999f) {
			int tw = ui_width(&ui_var_widths, ui_var, "1:1", -1);

			ui_text(view, cx - tw / 2, cy - 6, "1:1", -1, c, 0);
		} else {
			/* four corners: back to as large as fits */
			for (k = 0; k < 4; k++) {
				int x = k & 1 ? cx + 6 : cx - 6, y = k & 2 ? cy + 5 : cy - 5, sx = k & 1 ? -1 : 1, sy = k & 2 ? -1 : 1;

				GrLine(view, ui_gc, x, y, x + 3 * sx, y);
				GrLine(view, ui_gc, x, y, x, y + 3 * sy);
			}
			GrFillRect(view, ui_gc, cx - 2, cy - 1, 5, 3);
		}
		break;
	case T_DELETE:
		GrSetGCForeground(ui_gc, c == UI_SHADOW ? c : MWRGB(200, 30, 30));
		for (k = -1; k <= 1; k++) {
			GrLine(view, ui_gc, cx - 5 + k, cy - 5, cx + 5 + k, cy + 5);
			GrLine(view, ui_gc, cx + 5 + k, cy - 5, cx - 5 + k, cy + 5);
		}
		break;
	default:
		ui_glyph(view, cx, cy, t == T_PREV ? UI_G_PREV : t == T_NEXT ? UI_G_NEXT : t == T_SLIDE ? UI_G_PLAY :
			 t == T_TURN_LEFT ? UI_G_TURN_LEFT : UI_G_TURN_RIGHT, c);
	}
}

static void
draw_tool(int t, int pressed)
{
	int x = tool_x[t], h = t == T_SLIDE ? 34 : 30, y = tools_y + (TOOLS - h) / 2, w = tool_w[t], greyed = tool_greyed(t);
	GR_COLOR c = greyed ? UI_SHADOW : BLACK;

	if (ui_seven && t == T_SLIDE && !greyed) {
		/* Windows 7's round blue key, its corners cut */
		ui_fill(view, x + 2, y, w - 4, h, pressed ? MWRGB(20, 100, 190) : MWRGB(40, 125, 215));
		ui_fill(view, x, y + 2, w, h - 4, pressed ? MWRGB(20, 100, 190) : MWRGB(40, 125, 215));
		ui_fill(view, x + 1, y + 1, w - 2, h - 2, pressed ? MWRGB(20, 100, 190) : MWRGB(40, 125, 215));
		ui_fill(view, x + 3, y + 2, w - 6, h / 2 - 2, pressed ? MWRGB(60, 130, 210) : MWRGB(100, 165, 235));
		c = WHITE;
	} else if (ui_seven) {
		/* a flat key, edged only while pressed */
		if (pressed)
			ui_button_seven(view, x, y, w, h, 1);
	} else {
		ui_fill(view, x, y, w, h, UI_FACE);
		ui_bevel(view, x, y, w, h, pressed);
	}
	sign(t, x + w / 2 + (pressed && !ui_seven), y + h / 2 + (pressed && !ui_seven), c);
}

static void
draw_bars(void)
{
	char text[700], size[32];
	int i, pct;

	/* the bar of commands */
	ui_fill(view, 0, 0, width, TOP, UI_FACE);
	if (ui_seven)
		ui_fill(view, 0, 0, width, TOP / 2, MWRGB(248, 248, 248));
	for (i = 0; i < COMMANDS; i++)
		ui_button(view, command_x[i], 3, command_w[i], 22, commands[i], 0);
	GrSetGCForeground(ui_gc, UI_SHADOW);
	GrLine(view, ui_gc, 0, TOP - 1, width - 1, TOP - 1);
	/* the row of tools */
	if (ui_seven) {
		ui_fill(view, 0, tools_y, width, TOOLS / 2, MWRGB(233, 238, 245));
		ui_fill(view, 0, tools_y + TOOLS / 2, width, TOOLS - TOOLS / 2, MWRGB(215, 223, 234));
		GrSetGCForeground(ui_gc, MWRGB(180, 190, 205));
		GrLine(view, ui_gc, 0, tools_y, width - 1, tools_y);
	} else {
		ui_fill(view, 0, tools_y, width, TOOLS, UI_FACE);
		ui_bevel(view, 0, tools_y, width, TOOLS, 0);
	}
	for (i = 0; i < TOOL_COUNT; i++)
		draw_tool(i, 0);
	/* the status line: the name, its size, the zoom, its place in the folder */
	ui_fill(view, 0, height - STATUS, width, STATUS, UI_FACE);
	if (ui_seven) {
		GrSetGCForeground(ui_gc, MWRGB(215, 215, 215));
		GrLine(view, ui_gc, 0, height - STATUS, width - 1, height - STATUS);
	} else
		ui_bevel(view, 2, height - STATUS + 2, width - 4, STATUS - 3, 1);
	place();
	pct = rnd(scale * 100);
	size[0] = 0;
	if (file_bytes >= 1 << 20)
		snprintf(size, sizeof size, "%ld.%ld MB", file_bytes >> 20, (file_bytes % (1 << 20)) * 10 >> 20);
	else if (file_bytes > 0)
		snprintf(size, sizeof size, "%ld KB", (file_bytes + 1023) / 1024);
	if (kind == NONE)
		snprintf(text, sizeof text, "%.300s   %s", path[0] ? leaf_of(path) : "No picture", notice);
	else if (kind == ANIM)
		snprintf(text, sizeof text, "%.300s   %d x %d   %d%%   picture %d of %d   %s   %s", leaf_of(path), shown_w(), shown_h(), pct,
			 anim_frame + 1, media_anim_frames(anim), size, notice);
	else
		snprintf(text, sizeof text, "%.300s   %d x %d   %d%%   %s   %s", leaf_of(path), shown_w(), shown_h(), pct, size, notice);
	ui_text_fit(view, 8, height - STATUS + 4, width - 100, text, BLACK);
	if (at >= 0 && count > 0) {
		snprintf(text, sizeof text, "%d of %d", at + 1, count);
		ui_text(view, width - 10 - ui_width(&ui_var_widths, ui_var, text, -1), height - STATUS + 4, text, -1, BLACK, 0);
	}
}

static void
print_state(const char *what)
{
	if (!getenv("NXVIEW_PRINT"))
		return;
	place();
	printf("nxview: %s: %s, %d x %d, %d%%%s, turned %d, %d of %d, full %d, slide %d, %s\n", what, path, shown_w(), shown_h(),
	       rnd(scale * 100), fitted ? " fitted" : "", turns, at + 1, count, full != 0, slide, notice);
	fflush(stdout);
}

static void
draw(void)
{
	layout();
	draw_room();
	if (!full)
		draw_bars();
}

/* ---- taking a picture ---- */

static void
anim_forget(void)
{
	if (anim)
		media_anim_close(anim);
	if (anim_file)
		munmap(anim_file, anim_size);
	if (anim_pixmap)
		GrDestroyWindow(anim_pixmap);
	anim = NULL, anim_file = NULL, anim_frame = 0, anim_pixmap = 0;
}

static int
is_picture(const char *name)
{
	static const char *const ends[] = { ".ppm", ".png", ".jpg", ".jpeg", ".jpe", ".gif", ".bmp", ".pgm", ".pbm", ".pnm", ".tga", ".xpm" };
	size_t n = strlen(name), i;

	for (i = 0; i < sizeof ends / sizeof ends[0]; i++)
		if (n > strlen(ends[i]) && !strcasecmp(name + n - strlen(ends[i]), ends[i]))
			return 1;
	return 0;
}

static int
picture_entry(const struct dirent *entry)
{
	return entry->d_name[0] != '.' && is_picture(entry->d_name);
}

static int
by_name(const struct dirent **a, const struct dirent **b)
{
	return strcasecmp((*a)->d_name, (*b)->d_name);
}

/* The folder's pictures read again; `at` is the shown one's place among them (-1: not there). */
static void
scan_folder(void)
{
	struct dirent **found;
	int n, i;

	for (i = 0; i < count; i++)
		free(names[i]);
	free(names);
	names = NULL, count = 0, at = -1;
	if (!folder[0] || (n = scandir(folder, &found, picture_entry, by_name)) < 0)
		return;
	names = malloc(sizeof *names * (size_t)(n ? n : 1));
	for (i = 0; i < n; i++) {
		if (names && (names[count] = strdup(found[i]->d_name)) != NULL)
			count++;
		free(found[i]);
	}
	free(found);
	for (i = 0; i < count && path[0]; i++)
		if (!strcmp(names[i], leaf_of(path)))
			at = i;
}

/* Takes a file as the picture to show. */
static void
take(const char *file)
{
	char title[600], where[512];
	const char *slash;
	int pw, ph, fd;
	struct stat st;
	struct media_info about;

	anim_forget();
	forget_pic();
	hold_workers(0);
	if (image)
		GrFreeImage(image);
	image = 0, kind = NONE, wrong = NULL, notice[0] = 0, file_bytes = 0;
	turns = 0, fitted = 1, pic_w = pic_h = 0, pic_failed = 0;
	snprintf(path, sizeof path, "%s", file);
	snprintf(title, sizeof title, "%s - Viewer", path[0] ? leaf_of(path) : "No picture");
	if (window)
		GrSetWindowTitle(window, title);
	slash = strrchr(path, '/');
	snprintf(where, sizeof where, "%.*s", slash ? (slash == path ? 1 : (int)(slash - path)) : 1, slash ? path : ".");
	if (path[0] && strcmp(where, folder)) {
		snprintf(folder, sizeof folder, "%s", where);
		scan_folder();
	} else {
		for (at = -1, pw = 0; pw < count && path[0]; pw++)
			if (!strcmp(names[pw], leaf_of(path)))
				at = pw;
	}
	if (!path[0])
		return;
	if (stat(path, &st) != 0) {
		wrong = errno == ENOENT ? "there is no such file" : "the file cannot be read";
		return;
	}
	file_bytes = (long)st.st_size;
	if (S_ISDIR(st.st_mode)) {
		wrong = "it is a folder";
	} else if (is_coded(path)) {
		/* its size from its headers; it is decoded where it is drawn */
		uint8_t *f = MAP_FAILED;

		if ((fd = open(path, O_RDONLY)) >= 0 && st.st_size > 16)
			f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
		if (fd >= 0)
			close(fd);
		if (f == MAP_FAILED) {
			wrong = st.st_size <= 16 ? "the file is empty or cut short" : "the file cannot be read";
			return;
		}
		if ((wrong = media_probe(f, (uint32_t)st.st_size, &about)) == NULL && !about.width)
			wrong = "it is not a picture";
		if (!wrong && about.frames > 1) {
			/* an animation: every picture of it decoded ahead, on the workers, kept while it plays */
			hold_workers(1);
			anim = media_anim_open(f, (uint32_t)st.st_size, workers_asked(), &wrong);
			anim_file = f, anim_size = (uint32_t)st.st_size;
			anim_due = now_ms();
			if (anim && media_anim_frames(anim) < 1)
				wrong = "it has no pictures";
		} else {
			munmap(f, (size_t)st.st_size);
		}
		if (!wrong) {
			kind = anim ? ANIM : CODED, pic_w = about.width, pic_h = about.height;
			if (anim)	/* (its frames are rows of the canvas's width) */
				pic_w = media_anim_width(anim), pic_h = media_anim_height(anim);
		} else
			anim_forget();
	} else if (ui_picture_size(path, &pw, &ph)) {
		kind = PPM, pic_w = pw, pic_h = ph;
	} else if ((image = GrLoadImageFromFile(path, 0)) != 0) {
		GR_IMAGE_INFO info;

		GrGetImageInfo(image, &info);
		kind = XPM, pic_w = info.width, pic_h = info.height;
	} else {
		wrong = "it is no picture this viewer reads";
	}
}

/* A clean message for a file it was asked to open and cannot. */
static void others(GR_EVENT *event);

static void
cannot_open(void)
{
	static const char *const ok[] = { "OK" };
	char text[400];

	snprintf(text, sizeof text, "Viewer cannot open %.200s:\n%s.", leaf_of(path), wrong ? wrong : "it is no picture this viewer reads");
	ui_ask_icon = "error";
	ui_ask("Viewer", text, ok, 1, others);
	ui_ask_icon = NULL;
}

/* The picture `by` after this one in its folder, round the end to the start. */
static void
next(int by)
{
	int to;

	scan_folder();
	if (count < 1) {
		snprintf(notice, sizeof notice, "no pictures in this folder");
		return;
	}
	if (count == 1 && at == 0) {
		snprintf(notice, sizeof notice, "the only picture here");
		return;
	}
	to = at < 0 ? (by > 0 ? 0 : count - 1) : ((at + by) % count + count) % count;
	{
		char file[1100];

		snprintf(file, sizeof file, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", names[to]);
		take(file);
	}
}

static void
go_to(int n)
{
	char file[1100];

	scan_folder();
	if (count < 1)
		return;
	n = n < 0 ? 0 : n >= count ? count - 1 : n;
	snprintf(file, sizeof file, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", names[n]);
	take(file);
}

/* ---- the full window and the slide show ---- */

static void
set_full(int on)
{
	if (on && !full) {
		const GR_SCREEN_INFO *screen = ui_screen();

		full = GrNewWindowEx(GR_WM_PROPS_NODECORATE, "Viewer", GR_ROOT_WINDOW_ID, 0, 0, screen->cols, screen->rows, BLACK);
		GrSelectEvents(full, EVENTS);
		GrMapWindow(full);
		GrRaiseWindow(full);
		GrSetFocus(full);
	} else if (!on && full) {
		ui_moves(full, EVENTS, 0);
		GrDestroyWindow(full);
		full = 0, slide = 0;
		GrSetFocus(window);
	}
	view = full ? full : window;
	fitted = 1;
}

static long
slide_ms(void)
{
	const char *ms = getenv("NXVIEW_SLIDE");

	return ms ? atol(ms) : 4000;
}

static void
slide_show(int on)
{
	if (on && count < 1) {
		snprintf(notice, sizeof notice, "no pictures for a slide show");
		return;
	}
	if (on) {
		set_full(1);
		slide = 1, slide_due = now_ms() + slide_ms();
	} else {
		set_full(0);
	}
}

/* The slide show's next: one it cannot show is passed over. */
static void
slide_next(void)
{
	int tries;

	for (tries = 0; tries < count; tries++) {
		next(1);
		if (kind != NONE)
			break;
	}
	slide_due = now_ms() + slide_ms();
}

/* ---- what the commands and keys do ---- */

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == view)
		draw();
	/* the viewer's own box while the file window is up: both go */
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		ui_files_stop = closing = 1;
}

/* The picture shown is the desktop's from now on: the file itself, kept in the registry with
 * the turns it is shown with (the desktop decodes it turned so). */
static void
as_desktop(void)
{
	char turn[600];

	if (kind != PPM && kind != CODED) {
		snprintf(notice, sizeof notice, "this picture cannot be the desktop's");
		return;
	}
	snprintf(turn, sizeof turn, "%d:%s", turns & 3, path);
	/* (the desktop sees the registry change and draws it; the turn first, the picture last) */
	if (reg_set(UI_WALLPAPER_TURN, turn) && reg_set(UI_WALLPAPER_CHOICE, path))
		snprintf(notice, sizeof notice, "it is the desktop's picture now");
	else
		snprintf(notice, sizeof notice, "the registry cannot be written");
}

/* Deletes the file shown, once the question is answered, and shows the folder's next. */
static void
delete_shown(void)
{
	static const char *const answers[] = { "Delete", "Cancel" };
	char text[400];
	int was = at;

	if (!path[0] || access(path, F_OK) != 0)
		return;
	snprintf(text, sizeof text, "Delete %.200s for good?\nIt is not kept anywhere after this.", leaf_of(path));
	ui_ask_icon = "warning";
	was = ui_ask("Delete picture", text, answers, 2, others) == 0 ? at : -2;
	ui_ask_icon = NULL;
	if (was == -2)
		return;
	if (unlink(path) != 0) {
		snprintf(notice, sizeof notice, "it cannot be deleted: %s", strerror(errno));
		return;
	}
	take("");
	scan_folder();
	if (count > 0)
		go_to(was < 0 ? 0 : was);
	snprintf(notice, sizeof notice, "deleted");
}

static void
open_file(void)
{
	char chosen[1024];

	if (ui_choose_file("Open a picture", folder[0] ? folder : "/root/Pictures", ".jpg .jpeg .png .gif .bmp .tga .ppm .pgm .pbm .pnm .xpm",
			   chosen, sizeof chosen, others)) {
		take(chosen);
		draw();
		if (kind == NONE)
			cannot_open();
	}
}

/* A key, or the tool or command that stands for it. False to leave. */
static int
key(int ch)
{
	notice[0] = 0;
	switch (ch) {
	case 'q':
		return 0;
	case MWKEY_ESCAPE:
		if (!full)
			return 0;
		set_full(0);
		break;
	case 'o': open_file(); if (closing) return 0; break;
	case 'e':
		/* the paint program with this file */
		if (path[0] && real_vfork() == 0) {
			signal(SIGCHLD, SIG_DFL);	/* (paint waits for the decoder it starts) */
			execlp("nxpaint", "nxpaint", path, (char *)NULL);
			_exit(127);
		}
		break;
	case 'w': as_desktop(); break;
	case ' ':
		if (slide) {
			slide = slide == 1 ? 2 : 1, slide_due = now_ms() + slide_ms();
			snprintf(notice, sizeof notice, slide == 2 ? "paused" : "playing");
			break;
		}
		/* fall through: the next */
	case MWKEY_RIGHT: case MWKEY_PAGEDOWN: case MWKEY_DOWN:
		next(1), slide_due = now_ms() + slide_ms();
		break;
	case MWKEY_LEFT: case MWKEY_PAGEUP: case MWKEY_UP: case MWKEY_BACKSPACE:
		next(-1), slide_due = now_ms() + slide_ms();
		break;
	case MWKEY_HOME: go_to(0); break;
	case MWKEY_END: go_to(count - 1); break;
	case '+': case '=': zoom_by(1.25f); break;
	case '-': zoom_by(1 / 1.25f); break;
	case '0': case 'f': fitted = 1; break;
	case '1': place(); zoom_to(1, room_w / 2.0f, room_h / 2.0f); break;
	case 'a':	/* (the tool: its own size, or back to as large as fits) */
		if (fitted && fit_scale() < 0.999f)
			zoom_to(1, room_w / 2.0f, room_h / 2.0f);
		else
			fitted = 1;
		break;
	case 'l': case ',': case 'r': case '.':
		if (kind != PPM && kind != CODED) {
			snprintf(notice, sizeof notice, "this picture is not turned");
			break;
		}
		turns = (turns + (ch == 'l' || ch == ',' ? 3 : 1)) & 3, fitted = 1;
		break;
	case MWKEY_DELETE: delete_shown(); break;
	case MWKEY_F11: case MWKEY_ENTER: set_full(!full); break;
	case MWKEY_F5: slide_show(!slide); break;
	default:
		return 1;
	}
	draw();
	print_state("key");
	return 1;
}

static const int tool_keys[TOOL_COUNT] = { '-', '+', 'a', MWKEY_LEFT, MWKEY_F5, MWKEY_RIGHT, 'l', 'r', MWKEY_DELETE };
static const int command_keys[COMMANDS] = { 'o', 'e', 'w' };

/* A press of the left button in the framed window's bars; false to leave. */
static int
press_bars(int x, int y)
{
	int i;

	if (y < TOP) {
		for (i = 0; i < COMMANDS; i++)
			if (ui_inside(x, y, command_x[i], 3, command_w[i], 22))
				return key(command_keys[i]);
	} else if (y >= tools_y && y < tools_y + TOOLS) {
		for (i = 0; i < TOOL_COUNT; i++)
			if (ui_inside(x, y, tool_x[i], tools_y, tool_w[i], TOOLS) && !tool_greyed(i)) {
				draw_tool(i, 1);
				GrFlush();
				return key(tool_keys[i]);
			}
	}
	return 1;
}

int
main(int argc, char **argv)
{
	GR_EVENT event, peek;
	const GR_SCREEN_INFO *screen;
	int pw, ph, i, held = 0, held_x = 0, held_y = 0;
	float held_ox = 0, held_oy = 0;

	if (argc > 3 && !strcmp(argv[1], "--decode")) {
		const char *why = decode(argv[2], argv[3], &pw, &ph);

		if (why)
			fprintf(stderr, "nxview: %s: %s\n", argv[2], why);
		return why != NULL;
	}
	if (GrOpen() < 0)
		return 1;
	signal(SIGCHLD, SIG_IGN);	/* (Edit's paint program is not waited for) */
	ui_init();
	screen = ui_screen();
	width = screen->cols - 60 < 640 ? screen->cols - 60 : 640;
	height = screen->rows - 110 < 500 ? screen->rows - 110 : 500;
	window = view = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Viewer", GR_ROOT_WINDOW_ID, -1, -1, width, height, DARK);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	if (argc > 1) {
		/* decoding: the window framed and saying so first (docs/system/nanox.md) */
		ui_framed();
		snprintf(path, sizeof path, "%s", argv[1]);
		layout();
		room_text("Opening", leaf_of(path), DARK);
		draw_bars();
		take(argv[1]);
		draw();
	}
	if (getenv("NXVIEW_PRINT")) {
		layout();
		for (i = 0; i < COMMANDS; i++)
			printf("nxview: command %s at %d,%d\n", commands[i], command_x[i] + command_w[i] / 2, 3 + 11);
		for (i = 0; i < TOOL_COUNT; i++)
			printf("nxview: tool %d at %d,%d\n", i, tool_x[i] + tool_w[i] / 2, tools_y + TOOLS / 2);
		printf("nxview: %s, %d x %d%s\n", path, pic_w, pic_h, kind == PPM || kind == CODED || kind == ANIM ? "" : kind == XPM ? " (not a PPM)" : " (not shown)");
		fflush(stdout);
	}
	if (argc > 1 && kind == NONE)
		cannot_open();
	for (;;) {
		long now = now_ms(), due = -1;

		if (anim && !hidden)
			due = anim_due;
		if (slide == 1 && (due < 0 || slide_due < due))
			due = slide_due;
		if (due >= 0) {
			GrGetNextEventTimeout(&event, due > now ? (GR_TIMEOUT)(due - now) : 1);
			if (event.type == GR_EVENT_TYPE_TIMEOUT) {
				now = now_ms();
				if (slide == 1 && now >= slide_due) {
					slide_next();
					draw();
					print_state("slide");
				} else if (anim && now >= anim_due) {
					anim_frame = (anim_frame + 1) % media_anim_frames(anim);
					anim_due = now + (anim_delay > 10 ? anim_delay : 100);
					layout();
					draw_room();
					if (!full)
						draw_bars();
				}
				continue;
			}
		} else {
			GrGetNextEvent(&event);
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window)
				hidden = 0;
			if (event.exposure.wid == view) {
				ui_restyle();	/* (Settings may have changed the look) */
				draw();
			}
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.wid == window && event.update.utype == GR_UPDATE_SIZE) {
				width = event.update.width;
				height = event.update.height;
				if (!full)
					draw();
			} else if (event.update.wid == window && (event.update.utype == GR_UPDATE_UNMAP || event.update.utype == GR_UPDATE_MAP))
				hidden = event.update.utype == GR_UPDATE_UNMAP;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid != view)
				break;
			layout();
			if (ui_wheel(&event)) {
				/* the wheel zooms round the pointer */
				int sum = ui_wheel_sum(&event);
				float s;

				notice[0] = 0;
				if (!ui_inside(event.button.x, event.button.y, room_x, room_y, room_w, room_h))
					break;
				for (i = 0; i < (sum < 0 ? -sum : sum); i++) {
					place();
					s = sum < 0 ? scale * 1.25f : scale / 1.25f;
					zoom_to(s, (float)(event.button.x - room_x), (float)(event.button.y - room_y));
				}
				draw();
				print_state("wheel");
				break;
			}
			if (!(event.button.changebuttons & GR_BUTTON_L))
				break;
			if (!full && !ui_inside(event.button.x, event.button.y, room_x, room_y, room_w, room_h)) {
				if (!press_bars(event.button.x, event.button.y))
					goto leave;
				if (getenv("NXVIEW_PRINT"))
					print_state("press");
				break;
			}
			/* two clicks: the full window and back; one: a drag moves a picture larger than its room */
			{
				long t = now_ms();

				if (t - last_click < 500 && abs(event.button.x - last_x) < 6 && abs(event.button.y - last_y) < 6) {
					last_click = 0;
					set_full(!full);
					draw();
					print_state("double click");
					break;
				}
				last_click = t, last_x = event.button.x, last_y = event.button.y;
			}
			place();
			if (shown_w() * scale > room_w + 0.5f || shown_h() * scale > room_h + 0.5f) {
				held = 1, held_x = event.button.x, held_y = event.button.y, held_ox = ox, held_oy = oy;
				ui_moves(view, EVENTS, 1);	/* (the pointer's moves, while the button is held) */
			}
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			/* (only the last of the moves waiting: a redraw answers them all) */
			while (GrPeekEvent(&peek) && peek.type == GR_EVENT_TYPE_MOUSE_MOTION)
				GrGetNextEvent(&event);
			if (held && (event.mouse.buttons & GR_BUTTON_L)) {
				ox = held_ox + (event.mouse.x - held_x), oy = held_oy + (event.mouse.y - held_y);
				layout();
				draw_room();
			} else
				held = 0;
			if (!event.mouse.buttons)
				ui_moves(view, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (held && !full)
				draw_bars();	/* (the place in the status line) */
			held = 0;
			ui_moves(view, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (!key(event.keystroke.ch))
				goto leave;
			/* (a key's repeats that came while a picture was decoded are dropped) */
			while (GrPeekEvent(&peek) && peek.type == GR_EVENT_TYPE_KEY_DOWN && peek.keystroke.ch == event.keystroke.ch)
				GrGetNextEvent(&peek);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			goto leave;
		}
	}
leave:
	forget_pic();
	hold_workers(0);
	GrClose();
	return 0;
}
