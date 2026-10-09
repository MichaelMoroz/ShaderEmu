/*
 * nxpaint: a paint program. Pick a tool, a size and a colour on the left and draw on the
 * canvas; Save writes the picture as a PPM file (nxpaint FILE opens one and saves to it).
 * Keys: a tool's first letter picks it, 1 to 3 the size, [ and ] the colour, Ctrl+S saves.
 */
#include "ui.h"

#define TOOLS 60		/* width of the panel on the left */
#define CANVAS_W 440
#define CANVAS_H 340
#define SAVE_ROWS 20		/* rows asked for at a time when saving; divides CANVAS_H */

enum { PEN, LINE, BOX, FILLED, ERASER, TOOL_COUNT };
static const char *tool_names[TOOL_COUNT] = { "Pen", "Line", "Box", "Fill", "Erase" };
static const int sizes[3] = { 1, 3, 7 };
static const GR_COLOR palette[16] = {
	MWRGB(0, 0, 0), MWRGB(128, 128, 128), MWRGB(128, 0, 0), MWRGB(128, 128, 0),
	MWRGB(0, 128, 0), MWRGB(0, 128, 128), MWRGB(0, 0, 128), MWRGB(128, 0, 128),
	MWRGB(255, 255, 255), MWRGB(192, 192, 192), MWRGB(255, 0, 0), MWRGB(255, 255, 0),
	MWRGB(0, 255, 0), MWRGB(0, 255, 255), MWRGB(0, 0, 255), MWRGB(255, 0, 255),
};

static GR_WINDOW_ID window, canvas;
static int tool, size_index = 1, colour;
static int drawing, from_x, from_y, last_x, last_y;
static char path[256] = "/root/picture.ppm";
static const char *notice = "";

static void
draw_panel(void)
{
	int i;

	ui_fill(window, 0, 0, TOOLS, CANVAS_H, UI_FACE);
	for (i = 0; i < TOOL_COUNT; i++)
		ui_button(window, 3, 3 + i * 22, TOOLS - 6, 20, tool_names[i], i == tool);
	for (i = 0; i < 3; i++) {
		ui_button(window, 3 + i * 18, 118, 17, 17, "", i == size_index);
		ui_fill(window, 3 + i * 18 + 8 - sizes[i] / 2, 118 + 8 - sizes[i] / 2, sizes[i], sizes[i], BLACK);
	}
	for (i = 0; i < 16; i++) {
		ui_fill(window, 4 + (i % 4) * 13, 142 + (i / 4) * 13, 12, 12, palette[i]);
		ui_bevel(window, 4 + (i % 4) * 13, 142 + (i / 4) * 13, 12, 12, i == colour);
	}
	ui_fill(window, 4, 198, TOOLS - 8, 14, palette[colour]);
	ui_bevel(window, 4, 198, TOOLS - 8, 14, 1);
	ui_button(window, 3, 220, TOOLS - 6, 20, "Clear", 0);
	ui_button(window, 3, 243, TOOLS - 6, 20, "Save", 0);
	ui_text(window, 3, 270, notice, -1, BLACK, 0);
}

/* A square dab of the tool's size; a stroke is dabs close enough together to join. */
static void
dab(int x, int y)
{
	int size = sizes[size_index];

	GrFillRect(canvas, ui_gc, x - size / 2, y - size / 2, size, size);
}

static void
stroke(int x0, int y0, int x1, int y1)
{
	int dx = x1 - x0, dy = y1 - y0, steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy), i;

	GrSetGCForeground(ui_gc, tool == ERASER ? WHITE : palette[colour]);
	if (sizes[size_index] == 1) {
		GrLine(canvas, ui_gc, x0, y0, x1, y1);
		return;
	}
	for (i = 0; i <= steps; i++)
		dab(x0 + (steps ? dx * i / steps : 0), y0 + (steps ? dy * i / steps : 0));
}

static void
shape(int x0, int y0, int x1, int y1)
{
	int x = x0 < x1 ? x0 : x1, y = y0 < y1 ? y0 : y1, w = abs(x1 - x0) + 1, h = abs(y1 - y0) + 1, size = sizes[size_index];

	GrSetGCForeground(ui_gc, palette[colour]);
	if (tool == LINE) {
		stroke(x0, y0, x1, y1);
	} else if (tool == FILLED) {
		GrFillRect(canvas, ui_gc, x, y, w, h);
	} else {
		GrFillRect(canvas, ui_gc, x, y, w, size);
		GrFillRect(canvas, ui_gc, x, y + h - size, w, size);
		GrFillRect(canvas, ui_gc, x, y, size, h);
		GrFillRect(canvas, ui_gc, x + w - size, y, size, h);
	}
}

/* The canvas as a PPM file: three bytes a pixel, rows top to bottom. */
static void
save(void)
{
	static GR_PIXELVAL rows[SAVE_ROWS * CANVAS_W];
	static unsigned char bytes[SAVE_ROWS * CANVAS_W * 3];
	FILE *file = fopen(path, "wb");
	int x, y;

	if (!file) {
		notice = "cannot save";
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", CANVAS_W, CANVAS_H);
	for (y = 0; y < CANVAS_H; y += SAVE_ROWS) {
		GrReadArea(canvas, 0, y, CANVAS_W, SAVE_ROWS, rows);
		for (x = 0; x < SAVE_ROWS * CANVAS_W; x++) {
			/* a pixel of this screen is 0xAARRGGBB */
			bytes[3 * x] = rows[x] >> 16;
			bytes[3 * x + 1] = rows[x] >> 8;
			bytes[3 * x + 2] = rows[x];
		}
		fwrite(bytes, 3, SAVE_ROWS * CANVAS_W, file);
	}
	fclose(file);
	notice = "saved";
}

static void
panel_click(int x, int y)
{
	int i;

	notice = "";
	for (i = 0; i < TOOL_COUNT; i++)
		if (ui_inside(x, y, 3, 3 + i * 22, TOOLS - 6, 20))
			tool = i;
	for (i = 0; i < 3; i++)
		if (ui_inside(x, y, 3 + i * 18, 118, 17, 17))
			size_index = i;
	for (i = 0; i < 16; i++)
		if (ui_inside(x, y, 4 + (i % 4) * 13, 142 + (i / 4) * 13, 12, 12))
			colour = i;
	if (ui_inside(x, y, 3, 220, TOOLS - 6, 20))
		ui_fill(canvas, 0, 0, CANVAS_W, CANVAS_H, WHITE);
	if (ui_inside(x, y, 3, 243, TOOLS - 6, 20))
		save();
	draw_panel();
}

static void
key(int ch)
{
	int i;

	notice = "";
	for (i = 0; i < TOOL_COUNT; i++)
		if (ui_starts(tool_names[i], ch))
			tool = i;
	if (ch >= '1' && ch <= '3')
		size_index = ch - '1';
	else if (ch == '[' || ch == ']')
		colour = (colour + (ch == '[' ? 15 : 1)) % 16;
	else if (ch == ('s' & 0x1f))
		save();
	draw_panel();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int loaded = 0;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	if (argc > 1)
		snprintf(path, sizeof path, "%s", argv[1]);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Paint", GR_ROOT_WINDOW_ID, -1, -1, TOOLS + CANVAS_W, CANVAS_H, UI_FACE);
	canvas = GrNewWindow(window, TOOLS, 0, CANVAS_W, CANVAS_H, 0, WHITE, BLACK);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrSelectEvents(canvas, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP |
		GR_EVENT_MASK_MOUSE_POSITION);
	GrMapWindow(canvas);
	GrMapWindow(window);
	for (;;) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window)
				draw_panel();
			else if (!loaded) {
				/* the first time the canvas is shown: the file's picture, if there is one */
				loaded = 1;
				if (access(path, R_OK) == 0)
					GrDrawImageFromFile(canvas, ui_gc, 0, 0, CANVAS_W, CANVAS_H, path, 0);
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event))
				break;
			if (event.button.wid == window) {
				panel_click(event.button.x, event.button.y);
				break;
			}
			drawing = 1;
			from_x = last_x = event.button.x;
			from_y = last_y = event.button.y;
			if (tool == PEN || tool == ERASER)
				stroke(last_x, last_y, last_x, last_y);
			break;
		case GR_EVENT_TYPE_MOUSE_POSITION:
			if (drawing && (tool == PEN || tool == ERASER)) {
				stroke(last_x, last_y, event.mouse.x, event.mouse.y);
				last_x = event.mouse.x;
				last_y = event.mouse.y;
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (drawing && tool != PEN && tool != ERASER)
				shape(from_x, from_y, event.button.x, event.button.y);
			drawing = 0;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
