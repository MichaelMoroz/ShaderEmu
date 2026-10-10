/*
 * What the desktop's small programs share: the grey button look, the two fonts, a list and a
 * slider and their keys, starting another program, and putting a picture or a colour on the
 * desktop. ui_files.h adds a window to choose a file in.
 */
#ifndef SHADEREMU_APPS_UI_H
#define SHADEREMU_APPS_UI_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"

#define UI_FACE MWRGB(192, 192, 192)
#define UI_SHADOW MWRGB(128, 128, 128)
#define UI_SELECTED MWRGB(0, 0, 128)
#define UI_WALLPAPER_CHOICE "/tmp/nxwallpaper"	/* what the desktop shows: a picture's path, or #RRGGBB */
#define UI_WALLPAPER_DEFAULT "/usr/share/wallpaper-fox.ppm"

static GR_GC_ID ui_gc;
static GR_FONT_ID ui_fixed, ui_var;
static int ui_fw, ui_fh;	/* a character of the fixed font */

static void
ui_init(void)
{
	GR_FONT_INFO info;

	ui_gc = GrNewGC();
	GrSetGCUseBackground(ui_gc, GR_FALSE);
	ui_var = GrCreateFontEx(GR_FONT_SYSTEM_VAR, 0, 0, NULL);
	ui_fixed = GrCreateFontEx(GR_FONT_SYSTEM_FIXED, 0, 0, NULL);
	GrGetFontInfo(ui_fixed, &info);
	ui_fw = info.maxwidth;
	ui_fh = info.height;
}

static void
ui_fill(GR_DRAW_ID w, int x, int y, int width, int height, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	GrFillRect(w, ui_gc, x, y, width, height);
}

/* Text with its top left corner at x, y; `fixed` picks the font. */
static void
ui_text(GR_DRAW_ID w, int x, int y, const char *text, int count, GR_COLOR colour, int fixed)
{
	GrSetGCFont(ui_gc, fixed ? ui_fixed : ui_var);
	GrSetGCForeground(ui_gc, colour);
	GrText(w, ui_gc, x, y, (void *)text, count < 0 ? (int)strlen(text) : count, GR_TFASCII | GR_TFTOP);
}

/* A rectangle with a lit top-left and a shaded bottom-right edge, or the reverse. */
static void
ui_bevel(GR_DRAW_ID w, int x, int y, int width, int height, int pressed)
{
	GrSetGCForeground(ui_gc, pressed ? UI_SHADOW : WHITE);
	GrLine(w, ui_gc, x, y, x + width - 1, y);
	GrLine(w, ui_gc, x, y, x, y + height - 1);
	GrSetGCForeground(ui_gc, pressed ? WHITE : UI_SHADOW);
	GrLine(w, ui_gc, x, y + height - 1, x + width - 1, y + height - 1);
	GrLine(w, ui_gc, x + width - 1, y, x + width - 1, y + height - 1);
}

static void
ui_button(GR_DRAW_ID w, int x, int y, int width, int height, const char *label, int pressed)
{
	ui_fill(w, x, y, width, height, UI_FACE);
	ui_bevel(w, x, y, width, height, pressed);
	ui_text(w, x + 6, y + (height - 12) / 2, label, -1, BLACK, 0);
}

/* A line round the control the keyboard works now, just outside the rectangle given. */
static void
ui_focus(GR_DRAW_ID w, int x, int y, int width, int height)
{
	GrSetGCForeground(ui_gc, BLACK);
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

	ui_fill(w, x, y, UI_SCROLL_W, height, MWRGB(226, 226, 226));
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
	int count = (int)strlen(text), tw, th, tb;

	GrSetGCFont(ui_gc, ui_var);
	/* asking the server how wide costs a round trip: only when it may not fit */
	while (count > 0 && count * 4 > width) {
		GrGetGCTextSize(ui_gc, (void *)text, count, GR_TFASCII, &tw, &th, &tb);
		if (tw <= width)
			break;
		count -= count > 8 ? 4 : 1;
	}
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

/* Starts a program with one argument (or none), without a shell. */
static void
ui_run(const char *program, const char *argument)
{
	if (fork() == 0) {
		execlp(program, program, argument, (char *)NULL);
		_exit(127);
	}
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
static int
ui_picture(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around)
{
	int pw, ph, dw = width, dh = height;

	if (!ui_picture_size(path, &pw, &ph))
		return 0;
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
	return 1;
}

/* Shows a picture file, or a colour written #RRGGBB, as the desktop, and remembers the choice. */
static void
ui_wallpaper(const char *choice, int remember)
{
	GR_SCREEN_INFO screen;
	FILE *file;

	GrGetScreenInfo(&screen);
	if (choice[0] == '#') {
		unsigned long rgb = strtoul(choice + 1, NULL, 16);

		ui_fill(GR_ROOT_WINDOW_ID, 0, 0, screen.cols, screen.rows, MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
	} else {
		ui_picture(GR_ROOT_WINDOW_ID, choice, screen.cols, screen.rows, BLACK);
	}
	if (remember && (file = fopen(UI_WALLPAPER_CHOICE, "w")) != NULL) {
		fputs(choice, file);
		fclose(file);
	}
}

/* The desktop as last chosen, or the default picture. */
static void
ui_wallpaper_restore(void)
{
	char choice[200] = UI_WALLPAPER_DEFAULT;
	FILE *file = fopen(UI_WALLPAPER_CHOICE, "r");

	if (file) {
		if (fgets(choice, sizeof choice, file))
			choice[strcspn(choice, "\r\n")] = 0;
		fclose(file);
	}
	if (choice[0] != '#' && access(choice, R_OK) != 0)
		strcpy(choice, "#008080");	/* the picture is not in this image */
	ui_wallpaper(choice, 0);
}

#endif
