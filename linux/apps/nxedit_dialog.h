/* nxedit_dialog.h: the editor's small windows (Find and Replace, Go to line): labels, fields
 * to type in, boxes to tick and buttons, in the look the desktop has. Tab and Shift+Tab go
 * round them, Enter presses the window's own button and Escape its last. Needs ui.h. */
#ifndef NXEDIT_DIALOG_H
#define NXEDIT_DIALOG_H

enum { DLG_LABEL, DLG_FIELD, DLG_CHECK, DLG_BUTTON };
#define DLG_ROW 22

struct dlg_control {
	int kind, x, y, w, id;
	const char *label;
	char *text;		/* a field's text, of `size` bytes */
	int size;
	int *on;		/* a box's state */
	int at, offered;	/* a field's caret; offered: all of it is picked out and a key replaces it */
};

struct dlg {
	GR_WINDOW_ID window;
	const char *title;
	int width, height, count, focus;
	int enter, escape;	/* the ids Enter and Escape press */
	struct dlg_control *c;
	char note[160];		/* a line along the bottom: what the last press found */
};

static int
dlg_takes(const struct dlg_control *c)
{
	return c->kind != DLG_LABEL;
}

static void
dlg_draw_control(struct dlg *g, int i)
{
	struct dlg_control *c = &g->c[i];
	GR_WINDOW_ID w = g->window;
	int focused = i == g->focus, tw, j, from;

	switch (c->kind) {
	case DLG_LABEL:
		ui_text(w, c->x, c->y + 4, c->label, -1, BLACK, 0);
		break;
	case DLG_FIELD:
		ui_fill(w, c->x, c->y, c->w, DLG_ROW - 2, WHITE);
		ui_bevel(w, c->x, c->y, c->w, DLG_ROW - 2, 1);
		/* the end of a long text, so the caret stays in the field */
		for (from = 0; from < c->at && ui_width(&ui_var_widths, ui_var, c->text + from, c->at - from) > c->w - 10; from++)
			continue;
		tw = ui_width(&ui_var_widths, ui_var, c->text + from, -1);
		if (c->offered && c->text[0] && focused)
			ui_fill(w, c->x + 3, c->y + 2, tw + 2 < c->w - 6 ? tw + 2 : c->w - 6, DLG_ROW - 6, UI_SELECTED);
		ui_text_fit(w, c->x + 4, c->y + 4, c->w - 8, c->text + from, c->offered && focused ? WHITE : BLACK);
		if (focused && !c->offered)
			ui_fill(w, c->x + 4 + ui_width(&ui_var_widths, ui_var, c->text + from, c->at - from), c->y + 3, 1, DLG_ROW - 8, BLACK);
		break;
	case DLG_CHECK:
		ui_fill(w, c->x, c->y, c->w, DLG_ROW - 2, UI_FACE);
		ui_fill(w, c->x, c->y + 3, 13, 13, WHITE);
		ui_bevel(w, c->x, c->y + 3, 13, 13, 1);
		GrSetGCForeground(ui_gc, BLACK);
		for (j = 0; *c->on && j < 7; j++)
			GrLine(w, ui_gc, c->x + 3 + j, c->y + 3 + (j < 3 ? 5 + j : 9 - j), c->x + 3 + j, c->y + 3 + (j < 3 ? 7 + j : 11 - j));
		ui_text(w, c->x + 19, c->y + 4, c->label, -1, BLACK, 0);
		if (focused)
			ui_focus(w, c->x + 18, c->y + 4, ui_width(&ui_var_widths, ui_var, c->label, -1) + 2, 12);
		break;
	case DLG_BUTTON:
		if (ui_seven)
			ui_button_seven(w, c->x, c->y, c->w, DLG_ROW + 1, 0);
		else {
			ui_fill(w, c->x, c->y, c->w, DLG_ROW + 1, UI_FACE);
			ui_bevel(w, c->x, c->y, c->w, DLG_ROW + 1, 0);
		}
		tw = ui_width(&ui_var_widths, ui_var, c->label, -1);
		ui_text(w, c->x + (c->w - tw) / 2, c->y + 6, c->label, -1, BLACK, 0);
		if (focused)
			ui_focus(w, c->x + 5, c->y + 5, c->w - 10, DLG_ROW - 9);
		break;
	}
}

static void
dlg_draw_note(struct dlg *g)
{
	ui_fill(g->window, 8, g->height - 22, g->width - 16, 18, UI_FACE);
	ui_text_fit(g->window, 10, g->height - 20, g->width - 20, g->note, BLACK);
}

static void
dlg_draw(struct dlg *g)
{
	int i;

	ui_fill(g->window, 0, 0, g->width, g->height, UI_FACE);
	for (i = 0; i < g->count; i++)
		dlg_draw_control(g, i);
	dlg_draw_note(g);
}

static void
dlg_say(struct dlg *g, const char *text)
{
	snprintf(g->note, sizeof g->note, "%s", text);
	if (g->window)
		dlg_draw_note(g);
}

/* Opens the window, the keyboard in control `focus`, at x, y on the screen (-1: the system's place). */
static void
dlg_open(struct dlg *g, int x, int y, int focus)
{
	g->focus = focus;
	g->window = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE, (char *)g->title, GR_ROOT_WINDOW_ID, x, y, g->width, g->height, UI_FACE);
	GrSelectEvents(g->window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(g->window);
	GrRaiseWindow(g->window);
	GrSetFocus(g->window);
}

static void
dlg_close(struct dlg *g)
{
	if (g->window)
		GrDestroyWindow(g->window);
	g->window = 0;
}

/* A field's text picked out whole, with the keyboard in it. */
static void
dlg_offer(struct dlg_control *c)
{
	c->at = (int)strlen(c->text);
	c->offered = 1;
}

static void
dlg_focus(struct dlg *g, int i)
{
	int was = g->focus;

	g->focus = i;
	if (g->c[i].kind == DLG_FIELD)
		dlg_offer(&g->c[i]);
	if (was >= 0 && was < g->count)
		dlg_draw_control(g, was);
	dlg_draw_control(g, i);
}

/* A key typed in a field; true when it was the field's. */
static int
dlg_field_key(struct dlg_control *c, int ch, int ctrl)
{
	int n = (int)strlen(c->text), length;
	char *clip;

	if (ctrl && (ch == 'a' || ch == 1)) {
		dlg_offer(c);
		return 1;
	}
	if (ctrl && (ch == 'v' || ch == 22)) {
		if ((clip = ui_clip_get(&length)) == NULL)
			return 1;
		length = (int)strcspn(clip, "\r\n");
		if (c->offered)
			c->text[0] = 0, n = c->at = 0, c->offered = 0;
		if (length > c->size - 1 - n)
			length = c->size - 1 - n;
		memmove(c->text + c->at + length, c->text + c->at, n - c->at + 1);
		memcpy(c->text + c->at, clip, length);
		c->at += length;
		free(clip);
		return 1;
	}
	if (c->at > n)
		c->at = n;	/* (the text can be written from outside: Ctrl+F3) */
	switch (ch) {
	case MWKEY_LEFT: c->at -= c->at > 0 && !c->offered; break;
	case MWKEY_RIGHT: c->at += c->at < n && !c->offered; break;
	case MWKEY_HOME: c->at = 0; break;
	case MWKEY_END: c->at = n; break;
	case MWKEY_BACKSPACE:
		if (c->offered)
			c->text[0] = 0, c->at = 0;
		else if (c->at > 0)
			memmove(c->text + c->at - 1, c->text + c->at, n - c->at + 1), c->at--;
		break;
	case MWKEY_DELETE:
		if (c->offered)
			c->text[0] = 0, c->at = 0;
		else if (c->at < n)
			memmove(c->text + c->at, c->text + c->at + 1, n - c->at);
		break;
	default:
		if (ch < 32 || ch > 126 || ctrl)
			return 0;
		if (c->offered)
			c->text[0] = 0, n = c->at = 0;
		if (n >= c->size - 1)
			break;
		memmove(c->text + c->at + 1, c->text + c->at, n - c->at + 1);
		c->text[c->at++] = (char)ch;
	}
	c->offered = 0;
	return 1;
}

/* What an event of the window does: the id of a button pressed (the Escape id when it is
 * closed), or 0. A box ticked or a field typed in is drawn here. */
static int
dlg_event(struct dlg *g, const GR_EVENT *event)
{
	struct dlg_control *c;
	int i, ch, ctrl;

	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		dlg_draw(g);
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		return g->escape;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if (ui_wheel(event))
			break;
		for (i = 0; i < g->count; i++) {
			c = &g->c[i];
			if (!dlg_takes(c) || !ui_inside(event->button.x, event->button.y, c->x, c->y, c->w, DLG_ROW))
				continue;
			if (c->kind == DLG_BUTTON)
				return c->id;
			if (c->kind == DLG_CHECK)
				*c->on = !*c->on;
			dlg_focus(g, i);
			if (c->kind == DLG_FIELD) {
				int at = 0, px = event->button.x - c->x - 4;

				while (c->text[at] && ui_width(&ui_var_widths, ui_var, c->text, at + 1) <= px)
					at++;
				c->offered = 0, c->at = at;
				dlg_draw_control(g, i);
			}
			break;
		}
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		ch = event->keystroke.ch;
		ctrl = (event->keystroke.modifiers & MWKMOD_CTRL) != 0;
		c = &g->c[g->focus];
		if (ch == MWKEY_ESCAPE)
			return g->escape;
		if (ch == MWKEY_TAB && event->keystroke.scancode == 15) {
			int step = (event->keystroke.modifiers & MWKMOD_SHIFT) ? g->count - 1 : 1;

			i = g->focus;
			do
				i = (i + step) % g->count;
			while (!dlg_takes(&g->c[i]));
			dlg_focus(g, i);
			break;
		}
		if (ch == MWKEY_ENTER)
			return c->kind == DLG_BUTTON ? c->id : g->enter;
		if (c->kind == DLG_FIELD && dlg_field_key(c, ch, ctrl)) {
			dlg_draw_control(g, g->focus);
			break;
		}
		if (ch == ' ' && c->kind == DLG_CHECK) {
			*c->on = !*c->on;
			dlg_draw_control(g, g->focus);
		} else if (ch == ' ' && c->kind == DLG_BUTTON)
			return c->id;
		break;
	}
	return 0;
}

#endif
