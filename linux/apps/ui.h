/*
 * What the desktop's small programs share: the button look (classic grey, or Windows 7's as
 * the look says: ui_look.h), the two fonts, a list and a slider and their keys, starting
 * another program, and putting a picture or a colour on the desktop. ui_files.h adds a window
 * to choose a file in.
 */
#ifndef SHADEREMU_APPS_UI_H
#define SHADEREMU_APPS_UI_H

#include <signal.h>
#include "../userland/real_vfork.h"
#include "registry.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#define MWINCLUDECOLORS
#include "nano-X.h"
#include "ui_width.h"
#include "ui_state.h"
#include "ui_look.h"
#include "ui_exec.h"

#ifndef GR_WM_PROPS_NOMAXIMIZE
#define GR_WM_PROPS_NOMAXIMIZE 0x00004000L
#endif
/* A window whose size is its layout's: not resized by its corner, and no maximise box. */
#define UI_FIXED (GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE | GR_WM_PROPS_NOMAXIMIZE)
/* the look's colours: read once, at ui_init */
#define UI_FACE (ui_seven ? MWRGB(240, 240, 240) : MWRGB(192, 192, 192))
#define UI_SHADOW (ui_seven ? MWRGB(160, 160, 160) : MWRGB(128, 128, 128))
#define UI_SELECTED (ui_seven ? MWRGB(51, 153, 255) : MWRGB(0, 0, 128))
#define UI_KEPT "/persistence/desktop"	/* the desktop's own files that the host keeps (docs/system/persist.md) */
#define UI_WALLPAPER_CHOICE "desktop.wallpaper"	/* the registry's key for what the desktop shows: a picture's path, or #RRGGBB */
#define UI_WALLPAPER_DEFAULT "/usr/share/wallpaper-fox.jpg"
#define UI_WALLPAPER_TURN "desktop.wallpaper.turn"	/* "N:PATH": that picture shown N quarter turns to the right */

static GR_GC_ID ui_gc;
static GR_FONT_ID ui_fixed, ui_var;
static int ui_fw, ui_fh;	/* a character of the fixed font */
static struct ui_widths ui_var_widths;	/* the other font's: ui_width(&ui_var_widths, ui_var, ...) */

static void
ui_init(void)
{
	GR_FONT_INFO info;

	ui_look_read();
	ui_gc = GrNewGC();
	GrSetGCUseBackground(ui_gc, GR_FALSE);
	ui_var = GrCreateFontEx(ui_smooth ? UI_LOOK_FONT : GR_FONT_SYSTEM_VAR, 0, 0, NULL);
	ui_fixed = GrCreateFontEx(GR_FONT_SYSTEM_FIXED, 0, 0, NULL);
	GrGetFontInfo(ui_fixed, &info);
	ui_fw = info.maxwidth;
	ui_fh = info.height;
}

/* The look as Settings has it now; true when it changed, and the caller draws again. */
static int
ui_restyle(void)
{
	int smooth = ui_smooth;

	if (!ui_look_read())
		return 0;
	if (smooth != ui_smooth) {
		GrDestroyFont(ui_var);
		ui_var = GrCreateFontEx(ui_smooth ? UI_LOOK_FONT : GR_FONT_SYSTEM_VAR, 0, 0, NULL);
		ui_var_widths.known = 0;
	}
	return 1;
}

/* The screen, asked about once: its size does not change under a running program (another
 * size is the desktop started again, and every program with it). */
static const GR_SCREEN_INFO *
ui_screen(void)
{
	static GR_SCREEN_INFO screen;
	static int known;

	if (!known)
		GrGetScreenInfo(&screen);
	known = 1;
	return &screen;
}

static void
ui_fill(GR_DRAW_ID w, int x, int y, int width, int height, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	GrFillRect(w, ui_gc, x, y, width, height);
}

/* Text with its top left corner at x, y; `fixed` picks the font. (Selawik's ascent is a pixel
 * more than the bitmap font's: its text is a pixel higher, on the same baseline.) */
static void
ui_text(GR_DRAW_ID w, int x, int y, const char *text, int count, GR_COLOR colour, int fixed)
{
	GrSetGCFont(ui_gc, fixed ? ui_fixed : ui_var);
	GrSetGCForeground(ui_gc, colour);
	GrText(w, ui_gc, x, y - (ui_smooth && !fixed), (void *)text, count < 0 ? (int)strlen(text) : count, GR_TFASCII | GR_TFTOP);
}

/* The four sides of a rectangle, in two colours: top and left, bottom and right. */
static void
ui_edges(GR_DRAW_ID w, int x, int y, int width, int height, GR_COLOR lit, GR_COLOR shaded)
{
	GrSetGCForeground(ui_gc, lit);
	GrLine(w, ui_gc, x, y, x + width - 1, y);
	GrLine(w, ui_gc, x, y, x, y + height - 1);
	GrSetGCForeground(ui_gc, shaded);
	GrLine(w, ui_gc, x, y + height - 1, x + width - 1, y + height - 1);
	GrLine(w, ui_gc, x + width - 1, y, x + width - 1, y + height - 1);
}

/* A rectangle with a lit top-left and a shaded bottom-right edge, or the reverse; in the
 * Windows 7 look a raised one is a soft edge and a sunk one (a field, a list) a grey line. */
static void
ui_bevel(GR_DRAW_ID w, int x, int y, int width, int height, int pressed)
{
	if (ui_seven)
		ui_edges(w, x, y, width, height, pressed ? MWRGB(171, 173, 179) : WHITE, pressed ? MWRGB(171, 173, 179) : MWRGB(190, 190, 190));
	else
		ui_edges(w, x, y, width, height, pressed ? UI_SHADOW : WHITE, pressed ? WHITE : UI_SHADOW);
}

/* Windows 7's button: two halves of a pale gradient in a dark edge with its corners cut, blue
 * while pressed (or chosen, as a tab). */
static void
ui_button_seven(GR_DRAW_ID w, int x, int y, int width, int height, int pressed)
{
	int half = height / 2;

	ui_fill(w, x + 1, y + 1, width - 2, half - 1, pressed ? MWRGB(229, 244, 252) : MWRGB(242, 242, 242));
	ui_fill(w, x + 1, y + half, width - 2, height - half - 1, pressed ? MWRGB(196, 229, 246) : MWRGB(221, 221, 221));
	ui_edges(w, x + 1, y + 1, width - 2, height - 2, pressed ? MWRGB(240, 250, 255) : WHITE,
		pressed ? MWRGB(158, 202, 228) : MWRGB(205, 205, 205));
	GrSetGCForeground(ui_gc, pressed ? MWRGB(44, 98, 139) : MWRGB(112, 112, 112));
	GrLine(w, ui_gc, x + 1, y, x + width - 2, y);
	GrLine(w, ui_gc, x + 1, y + height - 1, x + width - 2, y + height - 1);
	GrLine(w, ui_gc, x, y + 1, x, y + height - 2);
	GrLine(w, ui_gc, x + width - 1, y + 1, x + width - 1, y + height - 2);
}

static void
ui_button(GR_DRAW_ID w, int x, int y, int width, int height, const char *label, int pressed)
{
	if (ui_seven)
		ui_button_seven(w, x, y, width, height, pressed);
	else {
		ui_fill(w, x, y, width, height, UI_FACE);
		ui_bevel(w, x, y, width, height, pressed);
	}
	ui_text(w, x + 6, y + (height - 12) / 2, label, -1, BLACK, 0);
}

/* A line round the control the keyboard works now, just outside the rectangle given. */
static void
ui_focus(GR_DRAW_ID w, int x, int y, int width, int height)
{
	GrSetGCForeground(ui_gc, ui_seven ? MWRGB(51, 153, 255) : BLACK);
	GrRect(w, ui_gc, x - 2, y - 2, width + 4, height + 4);
}

/* Tab and Shift+Tab move the keyboard among a window's `count` controls; true if it was one. */
static int
ui_tab(const GR_EVENT *event, int *focus, int count)
{
	if (event->keystroke.ch != MWKEY_TAB)
		return 0;
	*focus = (*focus + ((event->keystroke.modifiers & MWKMOD_SHIFT) ? count - 1 : 1)) % count;
	return 1;
}

/* The keys that press the button the keyboard is on. */
static int
ui_press_key(int ch)
{
	return ch == MWKEY_ENTER || ch == ' ';
}

/* True if a name starts with the letter typed, in either case. */
static int
ui_starts(const char *name, int ch)
{
	return ch > ' ' && ch < 127 && tolower((unsigned char)name[0]) == tolower(ch);
}

static int
ui_inside(int px, int py, int x, int y, int width, int height)
{
	return px >= x && px < x + width && py >= y && py < y + height;
}

/* Call after mapping a window that long work follows: the window manager frames a window as
 * its program takes events, so without this it stands bare until the work is done. The
 * events taken are dropped: draw after it. */
static void
ui_framed(void)
{
	GR_EVENT event;
	int k, none = 0;

	for (k = 0; k < 24 && none < 3; k++) {
		GrCheckNextEvent(&event);
		none = event.type == GR_EVENT_TYPE_NONE ? none + 1 : 0;
	}
}

/* Where a window's top left corner is on the screen (a menu is opened by the screen's
 * coordinates): a window's own place is in its frame, and the frame's on the screen. */
static struct { GR_WINDOW_ID window; int x, y; } ui_origin;	/* the last one found, until the window moves */

static void
ui_window_origin(GR_WINDOW_ID window, int *x, int *y)
{
	GR_WINDOW_INFO info;
	GR_WINDOW_ID w = window;

	if (window && ui_origin.window == window) {
		*x = ui_origin.x, *y = ui_origin.y;
		return;
	}
	*x = *y = 0;
	while (w && w != GR_ROOT_WINDOW_ID) {
		GrGetWindowInfo(w, &info);
		*x += info.x, *y += info.y;
		w = info.parent;
	}
	ui_origin.window = window, ui_origin.x = *x, ui_origin.y = *y;
}

/* Every event of a program that uses ui_window_origin passes here: a click says where its
 * window is without a question (two round trips), and an update may have moved it. */
static void
ui_origin_note(const GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_BUTTON_DOWN) {
		ui_origin.window = event->button.wid;
		ui_origin.x = event->button.rootx - event->button.x;
		ui_origin.y = event->button.rooty - event->button.y;
	} else if (event->type == GR_EVENT_TYPE_UPDATE)
		ui_origin.window = 0;
}

/* The pointer's moves, for a window whose other events are `mask`: asked for while a button is
 * held in it (a drag) and not otherwise. Every move over a window that selects them is an
 * event to answer: 600 thousand instructions a second under a pointer that only passes. The
 * button's release is asked for with them: the window system sends no moves of a held button
 * to a window that would not hear it let go. */
static void
ui_moves(GR_WINDOW_ID window, GR_EVENT_MASK mask, int held)
{
	static GR_WINDOW_ID asked;

	if (held ? asked == window : asked != window)
		return;
	asked = held ? window : 0;
	GrSelectEvents(window, held ? mask | GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_BUTTON_UP : mask);
}

/* A notch of the wheel arrives as a button going down: -1 for up, 1 for down, 0 for a real button. */
static int
ui_wheel(const GR_EVENT *event)
{
	if (event->type != GR_EVENT_TYPE_BUTTON_DOWN)
		return 0;
	return (event->button.buttons & GR_BUTTON_SCROLLUP) ? -1 : (event->button.buttons & GR_BUTTON_SCROLLDN) ? 1 : 0;
}

/* The same, with every notch already waiting added in and taken off the queue: a slow redraw
 * then answers all of them at once and stops when the wheel does. */
static int
ui_wheel_sum(const GR_EVENT *event)
{
	GR_EVENT next;
	int sum = ui_wheel(event);

	while (sum && GrPeekEvent(&next)) {
		if (ui_wheel(&next))
			sum += ui_wheel(&next);
		else if (next.type != GR_EVENT_TYPE_BUTTON_UP ||
			 !(next.button.changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)))
			break;
		GrGetNextEvent(&next);
	}
	return sum;
}

#define UI_SCROLL_W 14

static int
ui_scroll_thumb(int height, int total, int shown)
{
	int thumb = total > shown ? height * shown / total : height;

	return thumb < 14 ? (height < 14 ? height : 14) : thumb;
}

/* A vertical scroll bar: `shown` of `total` rows are in view, from row `top`. */
static void
ui_scrollbar(GR_DRAW_ID w, int x, int y, int height, int total, int shown, int top)
{
	int thumb = ui_scroll_thumb(height, total, shown);
	int at = total > shown ? (height - thumb) * top / (total - shown) : 0;

	ui_fill(w, x, y, UI_SCROLL_W, height, ui_seven ? MWRGB(234, 234, 236) : MWRGB(226, 226, 226));
	if (ui_seven) {
		ui_fill(w, x + 2, y + at + 1, UI_SCROLL_W - 4, thumb - 2, MWRGB(222, 223, 226));
		ui_edges(w, x + 1, y + at, UI_SCROLL_W - 2, thumb, MWRGB(150, 150, 155), MWRGB(150, 150, 155));
		return;
	}
	ui_fill(w, x + 1, y + at, UI_SCROLL_W - 2, thumb, UI_FACE);
	ui_bevel(w, x + 1, y + at, UI_SCROLL_W - 2, thumb, 0);
}

/* The first row to show with the pointer at `py` on that bar: the thumb's middle follows it. */
static int
ui_scroll_to(int py, int y, int height, int total, int shown)
{
	int thumb = ui_scroll_thumb(height, total, shown), top;

	if (total <= shown || height <= thumb)
		return 0;
	top = (py - y - thumb / 2) * (total - shown) / (height - thumb);
	return top < 0 ? 0 : top > total - shown ? total - shown : top;
}

/* Text cut to what fits in `width` pixels. */
static void
ui_text_fit(GR_DRAW_ID w, int x, int y, int width, const char *text, GR_COLOR colour)
{
	int count = (int)strlen(text);

	while (count > 0 && ui_width(&ui_var_widths, ui_var, text, count) > width)
		count -= count > 8 ? 4 : 1;
	ui_text(w, x, y, text, count, colour, 0);
}

/*
 * A list of rows, one of them selected, with a scroll bar when they do not all fit. The
 * program keeps the struct and says what a row reads; the list keeps `top` and `selected`.
 */
#define UI_ROW 18

struct ui_list {
	int x, y, width, height;
	int count, top, selected;	/* selected: -1 for none */
};

static int
ui_list_rows(const struct ui_list *l)
{
	return (l->height - 4) / UI_ROW;
}

static void
ui_list_draw(GR_DRAW_ID w, const struct ui_list *l, const char *(*label)(int row))
{
	int rows = ui_list_rows(l), bar = l->count > rows ? UI_SCROLL_W : 0, i;

	ui_fill(w, l->x, l->y, l->width, l->height, WHITE);
	for (i = 0; i < rows && l->top + i < l->count; i++) {
		int n = l->top + i, y = l->y + 2 + i * UI_ROW;

		if (n == l->selected)
			ui_fill(w, l->x + 2, y, l->width - 4 - bar, UI_ROW, UI_SELECTED);
		ui_text_fit(w, l->x + 6, y + 3, l->width - 12 - bar, label(n), n == l->selected ? WHITE : BLACK);
	}
	if (bar)
		ui_scrollbar(w, l->x + l->width - 2 - UI_SCROLL_W, l->y + 2, l->height - 4, l->count, rows, l->top);
	ui_bevel(w, l->x, l->y, l->width, l->height, 1);
}

/* Keeps the selected row in view, after the program has moved the selection itself. */
static void
ui_list_show(struct ui_list *l)
{
	int rows = ui_list_rows(l);

	if (l->selected >= 0 && l->selected < l->top)
		l->top = l->selected;
	if (l->selected >= l->top + rows)
		l->top = l->selected - rows + 1;
}

/* What an event does to a list: 0 nothing, 1 it changed (draw it again), 2 the selected row
 * was clicked again (open it). With mouse motion events selected the scroll bar can be dragged. */
static int
ui_list_event(struct ui_list *l, const GR_EVENT *event)
{
	int rows = ui_list_rows(l), most = l->count > rows ? l->count - rows : 0, turn = ui_wheel(event);
	int down = event->type == GR_EVENT_TYPE_BUTTON_DOWN;
	int held = event->type == GR_EVENT_TYPE_MOUSE_MOTION && (event->mouse.buttons & GR_BUTTON_L);
	int px = down ? event->button.x : event->mouse.x, py = down ? event->button.y : event->mouse.y, top = l->top, n;

	if ((!down && !held) || !ui_inside(px, py, l->x, l->y, l->width, l->height))
		return 0;
	if (turn) {
		top += turn * 3;
	} else if (most && px >= l->x + l->width - 2 - UI_SCROLL_W) {
		top = ui_scroll_to(py, l->y + 2, l->height - 4, l->count, rows);
	} else if (down) {
		n = l->top + (py - l->y - 2) / UI_ROW;
		if (n < 0 || n >= l->count)
			return 0;
		if (n == l->selected)
			return 2;
		l->selected = n;
		return 1;
	}
	top = top < 0 ? 0 : top > most ? most : top;
	if (top == l->top)
		return 0;
	l->top = top;
	return 1;
}

/* What a key does to a list: true if it moved the selection (draw the list again). */
static int
ui_list_key(struct ui_list *l, int ch)
{
	int rows = ui_list_rows(l), to = l->selected;

	switch (ch) {
	case MWKEY_UP: to--; break;
	case MWKEY_DOWN: to++; break;
	case MWKEY_PAGEUP: to -= rows - 1; break;
	case MWKEY_PAGEDOWN: to += rows - 1; break;
	case MWKEY_HOME: to = 0; break;
	case MWKEY_END: to = l->count - 1; break;
	default: return 0;
	}
	to = to >= l->count ? l->count - 1 : to < 0 ? 0 : to;
	if (to == l->selected || l->count == 0)
		return 0;
	l->selected = to;
	ui_list_show(l);
	return 1;
}

/* A slider: a track with a knob at `value` of `most`. */
static void
ui_slider(GR_DRAW_ID w, int x, int y, int width, int value, int most)
{
	int at = x + 4 + (width - 16) * value / (most > 0 ? most : 1);

	ui_fill(w, x, y, width, 18, UI_FACE);
	if (ui_seven) {
		ui_fill(w, x + 4, y + 7, width - 8, 4, MWRGB(231, 234, 234));
		ui_edges(w, x + 4, y + 7, width - 8, 4, MWRGB(176, 176, 176), MWRGB(214, 214, 214));
		ui_button_seven(w, at, y + 1, 8, 16, 0);
		return;
	}
	ui_fill(w, x + 4, y + 7, width - 8, 4, UI_SHADOW);
	ui_bevel(w, x + 4, y + 7, width - 8, 4, 1);
	ui_fill(w, at, y + 1, 8, 16, UI_FACE);
	ui_bevel(w, at, y + 1, 8, 16, 0);
}

/* The value a click or a drag at px on that slider asks for. */
static int
ui_slider_value(int px, int x, int width, int most)
{
	int value = (px - x - 8) * most / (width - 16 > 0 ? width - 16 : 1);

	return value < 0 ? 0 : value > most ? most : value;
}

/*
 * The desktop's clipboard: one piece of text every program can leave and take, a file in
 * memory. (The window system's own selections are a conversation between two programs that
 * both have to be running and listening; a file is there after its program has gone.) The
 * terminal uses the same file (linux/nanox/microwindows.patch).
 */
#define UI_CLIPBOARD "/tmp/clipboard"

static void
ui_clip_set(const char *text, int length)
{
	FILE *file = fopen(UI_CLIPBOARD ".new", "wb");

	if (!file)
		return;
	fwrite(text, 1, length, file);
	fclose(file);
	rename(UI_CLIPBOARD ".new", UI_CLIPBOARD);
}

/* The clipboard's text, for the caller to free, or NULL when it has none. */
static char *
ui_clip_get(int *length)
{
	FILE *file = fopen(UI_CLIPBOARD, "rb");
	char *text;
	long size;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	if (size <= 0 || size > 1 << 20 || !(text = malloc(size + 1))) {
		fclose(file);
		return NULL;
	}
	size = fread(text, 1, size, file);
	fclose(file);
	text[size] = 0;
	*length = (int)size;
	return text;
}

/* Starts a program with one argument (or none), without a shell. vfork: the child runs in
 * this program's memory until it is another program, and no page is copied for it. It may do
 * nothing there but what is below: no variable of ours written, nothing allocated. The child's
 * number, or -1. */
static pid_t
ui_run(const char *program, const char *argument)
{
	pid_t child = real_vfork();

	if (child == 0) {
		signal(SIGCHLD, SIG_DFL);	/* (a program that ignores it must not hand that on: the started one's children would go unseen) */
		execlp(program, program, argument, (char *)NULL);
		ui_exec_failed();
	}
	return child;
}

/* The size a PPM file says it has; 0 when it is not one. */
static int
ui_picture_size(const char *path, int *pw, int *ph)
{
	FILE *file = fopen(path, "rb");
	int most = 0, ok;

	if (!file)
		return 0;
	ok = fscanf(file, "P6 %d %d %d", pw, ph, &most) == 3 && *pw > 0 && *ph > 0;
	fclose(file);
	return ok;
}

/*
 * Draws a PPM file over a whole window of that size. A picture near the window's shape is
 * cropped to it (drawn larger, the window cutting it); another is shown whole, on `around`.
 */
static void ui_picture_sized(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around, int pw, int ph);

static int
ui_picture(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around)
{
	int pw, ph;

	if (!ui_picture_size(path, &pw, &ph))
		return 0;
	ui_picture_sized(w, path, width, height, around, pw, ph);
	return 1;
}

/* The same, for a picture whose size (pw x ph) the caller has read already. */
static void
ui_picture_sized(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around, int pw, int ph)
{
	int dw = width, dh = height;

	if (pw * height * 10 > ph * width * 13 || ph * width * 10 > pw * height * 13) {
		if (pw * height > ph * width)
			dh = ph * width / pw;
		else
			dw = pw * height / ph;
		ui_fill(w, 0, 0, width, height, around);
	} else if (pw * height > ph * width) {
		dw = pw * height / ph;
	} else {
		dh = ph * width / pw;
	}
	GrDrawImageFromFile(w, ui_gc, (width - dw) / 2, (height - dh) / 2, dw, dh, (char *)path, 0);
}

#ifdef UI_PICTURES
/* Any other picture (media.h) decoded once on the worker cores into a pixmap the GPU keeps, at
 * the size it is drawn, cropped or fitted as above: drawing it, or a part behind an icon, is one
 * copy. A twin made for that size ("NAME@WxH.jpg") is decoded as it is. 0: it could not be. */
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "media.h"
#include "mcw.h"

static struct { char path[200]; int w, h, cover, turns; GR_COLOR around; GR_WINDOW_ID pixmap; } ui_decoded;
static int ui_picture_turns;	/* quarter turns to the right the next pictures decoded here are shown with */

/* A picture decoded whole, turned, then fitted into `out` (the pixmap's pixels). */
static const char *
ui_picture_turned(const char *file, struct media_image *out, int cover, int turns)
{
	struct media_image in, turned;
	const char *why;
	int w, h, x, y;

	memset(&in, 0, sizeof in);
	if ((why = media_image_load(file, &in, 3, mcw_most())) != NULL)
		return why;
	w = turns & 1 ? in.height : in.width, h = turns & 1 ? in.width : in.height;
	turned = in;
	turned.width = w, turned.height = h, turned.stride = (w * 3 + 15) & ~15, turned.owned = 0;
	if (!(turned.pixels = malloc((size_t)turned.stride * (size_t)h))) {
		media_image_free(&in);
		return "not enough memory to turn it";
	}
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int sx = turns == 1 ? y : turns == 2 ? in.width - 1 - x : in.width - 1 - y;
			int sy = turns == 1 ? in.height - 1 - x : turns == 2 ? in.height - 1 - y : x;

			memcpy(turned.pixels + (size_t)y * turned.stride + (size_t)x * 3, in.pixels + (size_t)sy * in.stride + (size_t)sx * 3, 3);
		}
	media_image_free(&in);
	media_image_fit(&turned, out, cover, mcw_most());
	free(turned.pixels);
	return NULL;
}

/* A slow decode's progress (media_progress) in a panel over the middle of where the picture will
 * be drawn: after 0.4 s, at most every 0.25 s, a bar where the decoder says how far, and the time. */
static struct { GR_DRAW_ID on; int x, y, w, h; long began, said; unsigned most; const char *name; } ui_decoding;

static long
ui_decoding_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void
ui_decoding_told(uint32_t done, uint32_t total)
{
	long now = ui_decoding_ms(), tenths = (now - ui_decoding.began) / 100;
	int w = ui_decoding.w - 16 < 260 ? ui_decoding.w - 16 : 260, h = 62, x, y, bar = w - 24;
	char text[64];
	const char *slash = strrchr(ui_decoding.name, '/');

	if (total && (unsigned)((uint64_t)done * 100 / total) > ui_decoding.most)
		ui_decoding.most = (unsigned)((uint64_t)done * 100 / total);
	if (now - ui_decoding.began < 400 || now - ui_decoding.said < 250 || w < 140 || ui_decoding.h < h + 8)
		return;
	ui_decoding.said = now;
	x = ui_decoding.x + (ui_decoding.w - w) / 2, y = ui_decoding.y + (ui_decoding.h - h) / 2;
	ui_fill(ui_decoding.on, x, y, w, h, MWRGB(36, 38, 44));
	ui_edges(ui_decoding.on, x, y, w, h, MWRGB(96, 100, 112), MWRGB(16, 16, 20));
	snprintf(text, sizeof text, "Opening %s", ui_decoding.on == GR_ROOT_WINDOW_ID ? "the desktop's picture" :
		 slash ? slash + 1 : ui_decoding.name);
	ui_text_fit(ui_decoding.on, x + 12, y + 8, w - 24, text, MWRGB(240, 240, 240));
	ui_fill(ui_decoding.on, x + 12, y + 28, bar, 10, MWRGB(20, 20, 24));
	ui_fill(ui_decoding.on, x + 12, y + 28, bar * (int)ui_decoding.most / 100, 10, MWRGB(70, 140, 230));
	if (ui_decoding.most)
		snprintf(text, sizeof text, "%u%%   %ld.%ld s", ui_decoding.most, tenths / 10, tenths % 10);
	else
		snprintf(text, sizeof text, "%ld.%ld s", tenths / 10, tenths % 10);
	ui_text(ui_decoding.on, x + 12, y + 42, text, -1, MWRGB(200, 200, 200), 0);
	GrFlush();
	if (getenv("UI_PICTURES_PRINT"))
		fprintf(stderr, "ui: %s: %s\n", ui_decoding.name, text);
}

/* The file a picture is decoded from for a size: its twin made for the size, when there is one. */
static void
ui_picture_for(const char *path, int width, int height, char *to, int room)
{
	const char *dot = strrchr(path, '.');

	if (dot && strchr(dot, '/') == NULL) {
		snprintf(to, room, "%.*s@%dx%d%s", (int)(dot - path), path, width, height, dot);
		if (access(to, R_OK) == 0)
			return;
	}
	snprintf(to, room, "%s", path);
}

/* A pixmap the GPU keeps (of width x height), its pixels mapped here as `out` (0x00RRGGBB
 * words) once the GPU has done what it was asked of it: 0 where they are not the GPU's. */
static int
ui_gpu_map(GR_WINDOW_ID pixmap, int width, int height, struct media_image *out)
{
	GR_WINDOW_INFO info;
	GR_PIXELVAL pixel;
	uint32_t offset, bytes;
	uint8_t *mapped;
	int gpu;

	GrGetWindowInfo(pixmap, &info);
	if (!info.surface_address || (gpu = open("/dev/gpu", O_RDWR)) < 0) {
		if (getenv("UI_PICTURES_PRINT"))
			fprintf(stderr, "ui: no pixmap of the GPU's for %d x %d (address %08x)\n", width, height, (unsigned)info.surface_address);
		return 0;
	}
	/* (a pixel read first: the GPU has drawn what it was asked, a copy from it among them) */
	GrReadArea(pixmap, 0, 0, 1, 1, &pixel);
	offset = info.surface_address - 0x80000000u;
	bytes = (offset & 4095) + info.surface_row * (uint32_t)height * 4;
	mapped = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, offset & ~4095u);
	close(gpu);
	if (mapped == MAP_FAILED)
		return 0;
	memset(out, 0, sizeof *out);
	out->width = width, out->height = height, out->channels = 4, out->bgr = 1;
	out->stride = (int)info.surface_row * 4;
	out->pixels = mapped + (offset & 4095);
	return 1;
}

/* A new such pixmap, mapped; 0 where the GPU has no room for it. ui_gpu_unmap() when written. */
static GR_WINDOW_ID
ui_gpu_pixmap(int width, int height, struct media_image *out)
{
	GR_WINDOW_ID pixmap = GrNewPixmap(width, height, NULL);

	if (!ui_gpu_map(pixmap, width, height, out)) {
		GrDestroyWindow(pixmap);
		return 0;
	}
	return pixmap;
}

static void
ui_gpu_unmap(struct media_image *out)
{
	uintptr_t at = (uintptr_t)out->pixels;

	munmap((void *)(at & ~(uintptr_t)4095), (at & 4095) + (uint32_t)out->stride * (uint32_t)out->height);
	out->pixels = NULL;
}

/* A picture file decoded into a pixmap of width x height, drawn at x, y: `cover` as the desktop's. */
static int
ui_picture_decoded_at(GR_DRAW_ID w, const char *path, int x, int y, int width, int height, GR_COLOR around, int cover)
{
	struct media_image out;
	struct stat st;
	char file[220];
	const char *why = "no memory of the GPU's for it";
	uint8_t *data;
	int fd;

	if (ui_decoded.pixmap && ui_decoded.w == width && ui_decoded.h == height && ui_decoded.around == around &&
	    ui_decoded.cover == cover && ui_decoded.turns == (ui_picture_turns & 3) && !strcmp(ui_decoded.path, path)) {
		GrCopyArea(w, ui_gc, x, y, width, height, ui_decoded.pixmap, 0, 0, MWROP_COPY);
		return 1;
	}
	if (ui_decoded.pixmap)
		GrDestroyWindow(ui_decoded.pixmap);
	ui_decoded.pixmap = 0;
	ui_picture_for(path, width, height, file, sizeof file);
	if ((fd = open(file, O_RDONLY)) < 0)
		return 0;
	if (fstat(fd, &st) || st.st_size < 16 || (data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0)) == MAP_FAILED) {
		close(fd);
		return 0;
	}
	close(fd);
	if ((ui_decoded.pixmap = ui_gpu_pixmap(width, height, &out)) != 0) {
		struct timespec t0, t1;

		out.background = (uint32_t)(around & 255) << 16 | (around & 0xff00) | (around >> 16 & 255);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		ui_decoding.on = w, ui_decoding.x = x, ui_decoding.y = y, ui_decoding.w = width, ui_decoding.h = height;
		ui_decoding.began = ui_decoding_ms(), ui_decoding.said = 0, ui_decoding.most = 0, ui_decoding.name = path;
		media_progress = ui_decoding_told;
		why = ui_picture_turns & 3 ? ui_picture_turned(file, &out, cover, ui_picture_turns & 3) :
		      media_image_decode_fit(data, (uint32_t)st.st_size, &out, cover, mcw_most());
		clock_gettime(CLOCK_MONOTONIC, &t1);
		media_progress = NULL;
		ui_gpu_unmap(&out);
		if (getenv("UI_PICTURES_PRINT"))
			fprintf(stderr, "ui: %s at %d x %d: %s in %ld ms\n", file, width, height, why ? why : "decoded",
				(long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
	}
	munmap(data, (size_t)st.st_size);
	if (why) {
		if (ui_decoded.pixmap)
			GrDestroyWindow(ui_decoded.pixmap);
		ui_decoded.pixmap = 0;
		return 0;
	}
	snprintf(ui_decoded.path, sizeof ui_decoded.path, "%s", path);
	ui_decoded.w = width, ui_decoded.h = height, ui_decoded.around = around, ui_decoded.cover = cover;
	ui_decoded.turns = ui_picture_turns & 3;
	GrCopyArea(w, ui_gc, x, y, width, height, ui_decoded.pixmap, 0, 0, MWROP_COPY);
	return 1;
}

static int
ui_picture_decoded(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around)
{
	return ui_picture_decoded_at(w, path, 0, 0, width, height, around, 1);
}
#endif

/* The registry keeps the picture file chosen, wherever it is; one that is no PPM is decoded
 * (above), or where the GPU has no room shown from a copy decoded into /tmp, named by its path. */
static void
ui_wallpaper_copy(const char *choice, char *to, int room)
{
	unsigned hash = 2166136261u;
	const char *c;

	for (c = choice; *c; c++)
		hash = (hash ^ (unsigned char)*c) * 16777619u;
	snprintf(to, room, "/tmp/nxwallpaper-%08x.ppm", hash);
}

/* The PPM file that shows a choice: itself, or its decoded copy. 0 while that is not made. */
static int
ui_wallpaper_file(const char *choice, char *to, int room)
{
	int pw, ph;

	if (ui_picture_size(choice, &pw, &ph)) {
		snprintf(to, room, "%s", choice);
		return 1;
	}
	ui_wallpaper_copy(choice, to, room);
	return access(to, R_OK) == 0;
}

/* The copy made, in the background unless `wait`: written beside and renamed, never seen half done. */
static void
ui_wallpaper_decode(const char *choice, int wait)
{
	char to[64], part[72];
	pid_t child;
	int status;

	if (choice[0] == '#' || access(choice, R_OK) != 0 || ui_wallpaper_file(choice, to, sizeof to))
		return;
	snprintf(part, sizeof part, "%s.part", to);
	if (access(part, F_OK) == 0 && !wait)
		return;	/* (being made already) */
	if ((child = fork()) == 0) {
		execl("/bin/sh", "sh", "-c", "nxview --decode \"$1\" \"$2.part\" && mv \"$2.part\" \"$2\"", "sh", choice, to, (char *)NULL);
		_exit(127);
	}
	if (wait && child > 0)
		waitpid(child, &status, 0);
}

/* A picture file of any kind over a whole window: a PPM as it lies, another decoded (above). */
static int
ui_picture_any(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around)
{
	char file[200];

#ifdef UI_PICTURES
	if (!ui_picture_turns && ui_picture(w, path, width, height, around))
		return 1;
	if (ui_picture_decoded(w, path, width, height, around))
		return 1;
#else
	if (ui_picture(w, path, width, height, around))
		return 1;
#endif
	if (ui_wallpaper_file(path, file, sizeof file))
		return ui_picture(w, file, width, height, around);
	ui_wallpaper_decode(path, 0);
	return 0;
}

/* The quarter turns the registry keeps for the desktop's picture `choice` (0 for another). */
static int
ui_wallpaper_turns(const char *choice)
{
	const char *kept = reg_get(UI_WALLPAPER_TURN, "");

	return kept[0] >= '1' && kept[0] <= '3' && kept[1] == ':' && !strcmp(kept + 2, choice) ? kept[0] - '0' : 0;
}

/* Shows a picture file, or a colour written #RRGGBB, as the desktop, and remembers the choice:
 * a picture with `turns` quarter turns to the right (shown so where it is decoded). */
static void
ui_wallpaper_turned(const char *choice, int turns, int remember)
{
	GR_SCREEN_INFO screen = *ui_screen();
	char text[220];

	if (remember) {
		snprintf(text, sizeof text, "%d:%s", turns & 3, choice);
		reg_set(UI_WALLPAPER_TURN, text);
	}
#ifdef UI_PICTURES
	ui_picture_turns = turns;
#endif

	if (choice[0] == '#') {
		unsigned long rgb = strtoul(choice + 1, NULL, 16);

		ui_fill(GR_ROOT_WINDOW_ID, 0, 0, screen.cols, screen.rows, MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
	} else if (!ui_picture_any(GR_ROOT_WINDOW_ID, choice, screen.cols, screen.rows, BLACK)) {
		ui_fill(GR_ROOT_WINDOW_ID, 0, 0, screen.cols, screen.rows, MWRGB(0, 128, 128));
	}
#ifdef UI_PICTURES
	ui_picture_turns = 0;
#endif
	if (remember)
		reg_set(UI_WALLPAPER_CHOICE, choice);
}

static void
ui_wallpaper(const char *choice, int remember)
{
	ui_wallpaper_turned(choice, 0, remember);
}

/* What the desktop shows as last chosen, or the default picture: a picture of the image's that
 * was a PPM is its JPEG now; one this image does not have is a colour. */
static void
ui_wallpaper_choice(char *choice, int room)
{
	size_t n;

	snprintf(choice, room, "%s", reg_get(UI_WALLPAPER_CHOICE, UI_WALLPAPER_DEFAULT));
	n = strlen(choice);
	if (choice[0] != '#' && access(choice, R_OK) != 0 && n > 4 && !strcmp(choice + n - 4, ".ppm") && (int)n < room) {
		strcpy(choice + n - 4, ".jpg");
		if (access(choice, R_OK) != 0)
			choice[n - 4] = 0, strcat(choice, ".ppm");
	}
	if (choice[0] != '#' && access(choice, R_OK) != 0)
		snprintf(choice, room, "#008080");
}

static void
ui_wallpaper_restore(void)
{
	char choice[200];

	ui_wallpaper_choice(choice, sizeof choice);
	ui_wallpaper(choice, 0);
}

#endif
