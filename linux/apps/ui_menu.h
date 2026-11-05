/* ui_menu.h: a menu that pops up, with menus of its own opening to its side: the Start
 * menu's, and the one a right click brings. Needs nano-X.h and ui_icons.h, nothing of ui.h.
 *
 *	ui_menu_open(items, count, x, y, 0, 0);
 *	... and in the program's loop, before anything else looks at an event:
 *	switch (ui_menu_event(&event, &id)) { case UI_MENU_CHOSEN: ...act on id... }
 *
 * A level is a window of its own that goes when the menu closes. A press anywhere outside
 * closes it (GR_WM_PROPS_POPUP: the window system asks every menu to close), as Escape does. */
#ifndef UI_MENU_H
#define UI_MENU_H
#include <ctype.h>
#include <string.h>
#include "ui_width.h"
#include "ui_state.h"
#include "ui_look.h"

struct ui_menu_item {
	const char *label;
	int icon;				/* a cell of the icons' sheet, or -1 */
	int id;					/* what choosing it reports */
	const struct ui_menu_item *sub;		/* its own menu, or NULL */
	int sub_count;
	int flags;
	const char *icon_file;			/* or a file's own icon (a program's), which comes first: the cell if it has none */
};
#define UI_MENU_GREY 1		/* shown, not to be chosen */
#define UI_MENU_LINE 2		/* a line above it */
#define UI_MENU_CHECK 4		/* a tick where an icon would be: a choice that is on */

#define UI_MENU_NOT 0		/* the event was not a menu's */
#define UI_MENU_TOOK 1		/* it was, and that is all */
#define UI_MENU_CHOSEN 2	/* an item was chosen: the menu is closed */
#define UI_MENU_CLOSED 3	/* the menu closed with nothing chosen */

#define UI_MENU_LEVELS 4
#define UI_MENU_ROW 20
#define UI_MENU_BAR 26		/* the bar along the screen's bottom (nxbar.c): a menu stays above it */
#define UI_MENU_FACE (ui_seven ? MWRGB(240, 240, 240) : MWRGB(192, 192, 192))

static struct {
	GR_WINDOW_ID window;
	const struct ui_menu_item *items;
	int count, hover, x, y, width;
} ui_menu_level[UI_MENU_LEVELS];
static int ui_menu_depth;
static int ui_menu_button, ui_menu_root_x, ui_menu_root_y;	/* the button an item was chosen with (0: a key), and where */
static GR_GC_ID ui_menu_gc;
static GR_FONT_ID ui_menu_font;		/* the look's (0: the one a new GC has), which the menu and the bar draw in */
static struct ui_widths ui_menu_widths;	/* its widths */

/* The menu's context and font for the look as ui_look_read() last read it: at its first use,
 * and again by a program that follows a change of the look (the bar). */
static void
ui_menu_restyle(void)
{
	if (!ui_menu_gc) {
		ui_menu_gc = GrNewGC();
		GrSetGCUseBackground(ui_menu_gc, GR_FALSE);
	}
	if (ui_menu_font)
		GrDestroyFont(ui_menu_font);
	ui_menu_font = ui_smooth ? GrCreateFontEx(UI_LOOK_FONT, 0, 0, NULL) : 0;
	GrSetGCFont(ui_menu_gc, ui_menu_font);
	ui_menu_widths.known = 0;
}

static int
ui_menu_is_open(void)
{
	return ui_menu_depth > 0;
}

static void
ui_menu_draw(int k)
{
	GR_WINDOW_ID w = ui_menu_level[k].window;
	int width = ui_menu_level[k].width, height = ui_menu_level[k].count * UI_MENU_ROW + 4, i, j;

	GrSetGCForeground(ui_menu_gc, UI_MENU_FACE);
	GrFillRect(w, ui_menu_gc, 0, 0, width, height);
	if (ui_seven) {
		/* a pale panel with an edge, the icons in a column of their own */
		GrSetGCForeground(ui_menu_gc, MWRGB(151, 151, 151));
		GrRect(w, ui_menu_gc, 0, 0, width, height);
		GrSetGCForeground(ui_menu_gc, MWRGB(226, 227, 227));
		GrLine(w, ui_menu_gc, 25, 2, 25, height - 3);
		GrSetGCForeground(ui_menu_gc, MWRGB(255, 255, 255));
		GrLine(w, ui_menu_gc, 26, 2, 26, height - 3);
	} else {
		GrSetGCForeground(ui_menu_gc, MWRGB(255, 255, 255));
		GrLine(w, ui_menu_gc, 0, 0, width - 1, 0);
		GrLine(w, ui_menu_gc, 0, 0, 0, height - 1);
		GrSetGCForeground(ui_menu_gc, MWRGB(64, 64, 64));
		GrLine(w, ui_menu_gc, 0, height - 1, width - 1, height - 1);
		GrLine(w, ui_menu_gc, width - 1, 0, width - 1, height - 1);
	}
	for (i = 0; i < ui_menu_level[k].count; i++) {
		const struct ui_menu_item *item = &ui_menu_level[k].items[i];
		int y = 2 + i * UI_MENU_ROW, lit = i == ui_menu_level[k].hover && !(item->flags & UI_MENU_GREY);

		if (item->flags & UI_MENU_LINE) {
			GrSetGCForeground(ui_menu_gc, ui_seven ? MWRGB(224, 224, 224) : MWRGB(128, 128, 128));
			GrLine(w, ui_menu_gc, ui_seven ? 28 : 3, y, width - 4, y);
		}
		if (lit && ui_seven) {
			/* a light blue box with rounded corners round the row */
			GrSetGCForeground(ui_menu_gc, ui_accent(MWRGB(232, 240, 251), 228));
			GrFillRect(w, ui_menu_gc, 3, y + 2, width - 6, UI_MENU_ROW / 2 - 1);
			GrSetGCForeground(ui_menu_gc, ui_accent(MWRGB(219, 232, 249), 208));
			GrFillRect(w, ui_menu_gc, 3, y + 1 + UI_MENU_ROW / 2, width - 6, UI_MENU_ROW / 2 - 2);
			GrSetGCForeground(ui_menu_gc, ui_accent(MWRGB(168, 202, 240), 150));
			GrLine(w, ui_menu_gc, 3, y + 1, width - 4, y + 1);
			GrLine(w, ui_menu_gc, 3, y + UI_MENU_ROW - 1, width - 4, y + UI_MENU_ROW - 1);
			GrLine(w, ui_menu_gc, 2, y + 2, 2, y + UI_MENU_ROW - 2);
			GrLine(w, ui_menu_gc, width - 3, y + 2, width - 3, y + UI_MENU_ROW - 2);
		} else if (lit) {
			GrSetGCForeground(ui_menu_gc, ui_accent(MWRGB(10, 36, 106), -64));
			GrFillRect(w, ui_menu_gc, 2, y + 1, width - 4, UI_MENU_ROW - 1);
		}
		if (item->icon_file && item->icon_file[0])
			GrDrawImageFromFile(w, ui_menu_gc, 5, y + 2, 16, 16, (char *)item->icon_file,
				item->icon >= 0 ? GR_IMAGE_ICON_OR(16, item->icon) : GR_IMAGE_ICON(16));
		else
			ui_icon(w, ui_menu_gc, 5, y + 2, item->icon, 16);
		GrSetGCForeground(ui_menu_gc, lit && !ui_seven ? MWRGB(255, 255, 255) : item->flags & UI_MENU_GREY ?
			(ui_seven ? MWRGB(109, 109, 109) : MWRGB(128, 128, 128)) : MWRGB(0, 0, 0));
		GrText(w, ui_menu_gc, ui_seven ? 32 : 27, y + (ui_smooth ? 2 : 3), (void *)item->label, strlen(item->label), GR_TFASCII | GR_TFTOP);
		for (j = 0; (item->flags & UI_MENU_CHECK) && j < 7; j++)
			GrLine(w, ui_menu_gc, 9 + j, y + (j < 3 ? 8 + j : 12 - j), 9 + j, y + (j < 3 ? 10 + j : 14 - j));
		for (j = 0; item->sub && j < 4; j++)	/* an arrow: its own menu opens to the side */
			GrLine(w, ui_menu_gc, width - 12 + j, y + 6 + j, width - 12 + j, y + 14 - j);
	}
}

/* Closes level k and those above it. */
static void
ui_menu_close_from(int k)
{
	while (ui_menu_depth > k) {
		ui_menu_depth--;
		GrDestroyWindow(ui_menu_level[ui_menu_depth].window);
		ui_menu_level[ui_menu_depth].window = 0;
	}
}

static void
ui_menu_close(void)
{
	ui_menu_close_from(0);
}

/* Opens level k at x, y: its top left, or with `up` its bottom left. `pick` is the row picked out. */
static void
ui_menu_show(int k, const struct ui_menu_item *items, int count, int x, int y, int up, int pick)
{
	static GR_SCREEN_INFO screen;	/* asked about once: a screen keeps its size under a running program */
	int width = 150, height = count * UI_MENU_ROW + 4, i, w;

	if (k >= UI_MENU_LEVELS || count < 1)
		return;
	if (!ui_menu_gc) {
		ui_look_read();
		ui_menu_restyle();
	}
	ui_menu_close_from(k);
	for (i = 0; i < count; i++) {
		w = ui_width(&ui_menu_widths, ui_menu_font, items[i].label, -1);
		if (w + (ui_seven ? 32 : 27) + 20 > width)
			width = w + (ui_seven ? 32 : 27) + 20;
	}
	if (!screen.cols)
		GrGetScreenInfo(&screen);
	if (up)
		y -= height;
	if (k > 0 && x + width > screen.cols)	/* no room to the side: on the other side of its parent */
		x = ui_menu_level[k - 1].x - width + 3;
	if (x + width > screen.cols)
		x = screen.cols - width;
	if (y + height > screen.rows - UI_MENU_BAR)
		y = screen.rows - UI_MENU_BAR - height;
	ui_menu_level[k].items = items;
	ui_menu_level[k].count = count;
	ui_menu_level[k].hover = pick;
	ui_menu_level[k].x = x < 0 ? 0 : x;
	ui_menu_level[k].y = y < 0 ? 0 : y;
	ui_menu_level[k].width = width;
	ui_menu_level[k].window = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_POPUP,
		"menu", GR_ROOT_WINDOW_ID, ui_menu_level[k].x, ui_menu_level[k].y, width, height, UI_MENU_FACE);
	GrSelectEvents(ui_menu_level[k].window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(ui_menu_level[k].window);
	GrRaiseWindow(ui_menu_level[k].window);
	GrSetFocus(ui_menu_level[k].window);
	ui_menu_depth = k + 1;
}

/* Opens a menu; `keyboard` picks out its first row, as a menu opened by a key has. */
static void
ui_menu_open(const struct ui_menu_item *items, int count, int x, int y, int up, int keyboard)
{
	ui_menu_show(0, items, count, x, y, up, keyboard ? 0 : -1);
}

/* Opens the menu of level k's row `row` beside it. */
static void
ui_menu_enter(int k, int row, int pick)
{
	const struct ui_menu_item *item = &ui_menu_level[k].items[row];

	ui_menu_show(k + 1, item->sub, item->sub_count, ui_menu_level[k].x + ui_menu_level[k].width - 3,
		ui_menu_level[k].y + row * UI_MENU_ROW, 0, pick);
}

/* Opens the items of the first level's row called `name`, for a test without a pointer. */
static void
ui_menu_enter_named(const char *name)
{
	int i;

	for (i = 0; ui_menu_depth == 1 && i < ui_menu_level[0].count; i++)
		if (ui_menu_level[0].items[i].sub && !strcmp(ui_menu_level[0].items[i].label, name)) {
			ui_menu_level[0].hover = i;
			ui_menu_enter(0, i, -1);
			return;
		}
}

static int
ui_menu_key(int ch, int *chosen)
{
	int k = ui_menu_depth - 1, count = ui_menu_level[k].count, hover = ui_menu_level[k].hover, to = hover, i;
	const struct ui_menu_item *items = ui_menu_level[k].items;

	switch (ch) {
	case MWKEY_UP: to = hover <= 0 ? count - 1 : hover - 1; break;
	case MWKEY_DOWN: to = hover + 1 >= count ? 0 : hover + 1; break;
	case MWKEY_HOME: case MWKEY_PAGEUP: to = 0; break;
	case MWKEY_END: case MWKEY_PAGEDOWN: to = count - 1; break;
	case MWKEY_RIGHT:
		if (hover >= 0 && items[hover].sub)
			ui_menu_enter(k, hover, 0);
		return UI_MENU_TOOK;
	case MWKEY_ENTER: case ' ':
		if (hover < 0 || (items[hover].flags & UI_MENU_GREY))
			return UI_MENU_TOOK;
		if (items[hover].sub) {
			ui_menu_enter(k, hover, 0);
			return UI_MENU_TOOK;
		}
		*chosen = items[hover].id;
		ui_menu_close();
		return UI_MENU_CHOSEN;
	case MWKEY_LEFT: case MWKEY_BACKSPACE: case MWKEY_ESCAPE:
		if (k > 0) {
			ui_menu_close_from(k);
			GrSetFocus(ui_menu_level[k - 1].window);
			return UI_MENU_TOOK;
		}
		if (ch != MWKEY_ESCAPE)
			return UI_MENU_TOOK;
		ui_menu_close();
		return UI_MENU_CLOSED;
	default:
		/* a letter: the next row that starts with it */
		for (i = 1; i <= count && ch > ' ' && ch < 127; i++)
			if (tolower((unsigned char)items[(hover + i + count) % count].label[0]) == tolower(ch)) {
				to = (hover + i + count) % count;
				break;
			}
	}
	if (to != hover) {
		ui_menu_level[k].hover = to;
		ui_menu_draw(k);
	}
	return UI_MENU_TOOK;
}

/* Takes an event if it is a menu's. *chosen is the item's id when UI_MENU_CHOSEN comes back. */
static int
ui_menu_event(GR_EVENT *event, int *chosen)
{
	GR_WINDOW_ID wid = event->type == GR_EVENT_TYPE_EXPOSURE ? event->exposure.wid :
		event->type == GR_EVENT_TYPE_BUTTON_DOWN ? event->button.wid :
		event->type == GR_EVENT_TYPE_MOUSE_MOTION ? event->mouse.wid :
		event->type == GR_EVENT_TYPE_KEY_DOWN ? event->keystroke.wid :
		event->type == GR_EVENT_TYPE_CLOSE_REQ ? event->general.wid : 0;
	int k, row;

	/* a key while a menu is open is the menu's, wherever the pointer is: a press of Escape over the bar went on to
	 * the program's window, whose Escape (nxplay's, nxvideo's) ends it */
	if (event->type == GR_EVENT_TYPE_KEY_DOWN && ui_menu_depth > 0)
		wid = ui_menu_level[ui_menu_depth - 1].window;
	for (k = 0; k < ui_menu_depth && ui_menu_level[k].window != wid; k++)
		;
	if (!wid || k == ui_menu_depth)
		return UI_MENU_NOT;
	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		ui_menu_draw(k);
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		row = (event->mouse.y - 2) / UI_MENU_ROW;
		if (row < 0 || row >= ui_menu_level[k].count || row == ui_menu_level[k].hover)
			break;
		ui_menu_level[k].hover = row;
		ui_menu_close_from(k + 1);
		ui_menu_draw(k);
		if (ui_menu_level[k].items[row].sub && !(ui_menu_level[k].items[row].flags & UI_MENU_GREY))
			ui_menu_enter(k, row, -1);
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		row = (event->button.y - 2) / UI_MENU_ROW;
		if ((event->button.buttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) || row < 0 || row >= ui_menu_level[k].count ||
		    (ui_menu_level[k].items[row].flags & UI_MENU_GREY))
			break;
		if (ui_menu_level[k].items[row].sub) {
			ui_menu_level[k].hover = row;
			ui_menu_draw(k);
			if (ui_menu_depth == k + 1)
				ui_menu_enter(k, row, -1);
			break;
		}
		*chosen = ui_menu_level[k].items[row].id;
		ui_menu_button = event->button.changebuttons ? event->button.changebuttons : event->button.buttons;
		ui_menu_root_x = event->button.rootx, ui_menu_root_y = event->button.rooty;
		ui_menu_close();
		return UI_MENU_CHOSEN;
	case GR_EVENT_TYPE_KEY_DOWN:
		ui_menu_button = 0;
		return ui_menu_key(event->keystroke.ch, chosen);
	case GR_EVENT_TYPE_CLOSE_REQ:
		ui_menu_close();
		return UI_MENU_CLOSED;
	}
	return UI_MENU_TOOK;
}
#endif
