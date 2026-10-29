/* nxpaint: a paint program (docs/system/nanox.md, "Paint"). The picture is the program's own memory,
 * shown by writing the part in view into the window's buffer; every change is kept as the
 * pixels it replaced, to be taken back. nxpaint FILE opens any picture libmedia reads and
 * saves as the name asks: PNG, JPEG, GIF, BMP or PPM, encoded on the worker cores (media.h). */
#define _GNU_SOURCE
#include "ui.h"
#include "ui_bulk.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "media.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>

#define MENU_H 20
#define TOOLS_W 72
#define BOTTOM_H 62
#define STATUS 18
#define MOST_W 1600
#define MOST_H 1200
#define FIELD_W 120
#define FIELD_H 44
#define GREY 0xff808080u
#define PICTURES "/root/Pictures"
#define KINDS ".png .jpg .jpeg .gif .bmp .ppm"	/* what is written, by the name's ending (another: .png added) */
#define OPENED ".jpg .jpeg .png .gif .bmp .tga .ppm .pgm .pbm"

enum { T_SELECT, T_PENCIL, T_BRUSH, T_ERASER, T_FILL, T_PICK, T_LINE, T_RECT, T_OVAL, T_SPRAY, TOOL_COUNT };
static const char *const tool_names[TOOL_COUNT] = { "Select", "Pencil", "Brush", "Eraser", "Fill", "Pick", "Line", "Rect", "Oval", "Spray" };
/* the tools' pictures, 16 x 16: a letter a colour of tool_inks, '.' none */
static const char *const tool_pictures[TOOL_COUNT][16] = {
	{ /* Select */
		"................",
		"................",
		".kk..kk..kk..kk.",
		".k............k.",
		"................",
		"................",
		".k............k.",
		".k............k.",
		"................",
		"................",
		".k............k.",
		".k............k.",
		"................",
		".kk..kk..kk..kk.",
		"................",
		"................",
	},
	{ /* Pencil */
		"................",
		"............Ppp.",
		"...........Ppp..",
		"..........gss...",
		".........Yyy....",
		"........Yyy.....",
		".......Yyy......",
		"......Yyy.......",
		".....Yyy........",
		"....Yyy.........",
		"...ooo..........",
		"..ooo...........",
		"..ok............",
		".kk.............",
		"................",
		"................",
	},
	{ /* Brush */
		"..............nn",
		".............nnn",
		"............nnn.",
		"...........nnn..",
		"..........nnn...",
		".........nnn....",
		"........nnn.....",
		".......sss......",
		"......sss.......",
		".....kkk........",
		"....kkkk........",
		"...kkkk.........",
		"..bkkk..........",
		".bbkk...........",
		".bbb............",
		"................",
	},
	{ /* Eraser */
		"................",
		"................",
		"................",
		"........kkkkkkk.",
		".......kppppppk.",
		"......kppppppkk.",
		".....kppppppkPk.",
		"....kppppppkPPk.",
		"...kwwwwwwkPPk..",
		"..kwwwwwwkPPk...",
		".kwwwwwwkPPk....",
		".kwwwwwwkPk.....",
		".kwwwwwwkk......",
		".kkkkkkkk.......",
		"................",
		"................",
	},
	{ /* Fill */
		"................",
		"......kk........",
		".....k..k.......",
		".....k..kk......",
		"....kkkkkkk.....",
		"...kwwwwwwwk....",
		"..kwwwwwwwwwk...",
		".kwwwwwwwwwwbk..",
		"..kwwwwwwwwbbk..",
		"...kwwwwwwkbbk..",
		"....kwwwwk.bbk..",
		".....kwwk..bbk..",
		"......kk...bk...",
		"...........bk...",
		"..........bbb...",
		"................",
	},
	{ /* Pick */
		"................",
		"............kk..",
		"...........kkkk.",
		"..........kkkkk.",
		"........kkkkkk..",
		".........kkkk...",
		"........kwkkk...",
		".......kwwk.k...",
		"......kwwk......",
		".....kwwk.......",
		"....kbbk........",
		"...kbbk.........",
		"..kbbk..........",
		"..kbk...........",
		".kk.............",
		"................",
	},
	{ /* Line */
		"................",
		"..............k.",
		".............kk.",
		"............kk..",
		"...........kk...",
		"..........kk....",
		".........kk.....",
		"........kk......",
		".......kk.......",
		"......kk........",
		".....kk.........",
		"....kk..........",
		"...kk...........",
		"..kk............",
		".k..............",
		"................",
	},
	{ /* Rect */
		"................",
		"................",
		"................",
		".kkkkkkkkkkkkkk.",
		".k............k.",
		".k............k.",
		".k............k.",
		".k............k.",
		".k............k.",
		".k............k.",
		".k............k.",
		".k............k.",
		".kkkkkkkkkkkkkk.",
		"................",
		"................",
		"................",
	},
	{ /* Oval */
		"................",
		"................",
		"................",
		".....kkkkkk.....",
		"...kk......kk...",
		"..k..........k..",
		".k............k.",
		".k............k.",
		".k............k.",
		".k............k.",
		"..k..........k..",
		"...kk......kk...",
		".....kkkkkk.....",
		"................",
		"................",
		"................",
	},
	{ /* Spray */
		"................",
		"..b..b..........",
		"b...b..b........",
		"..b...b.........",
		".b..b...........",
		"......kkk.......",
		".....kGGGk......",
		"....kssssk......",
		"....kbbbbk......",
		"....kbBbbk......",
		"....kbBbbk......",
		"....kbBbbk......",
		"....kbbbbk......",
		"....kssssk......",
		"....kkkkkk......",
		"................",
	},
};
static const struct { char letter; GR_COLOR colour; } tool_inks[] = {
	{ 'k', MWRGB(24, 24, 28) }, { 'w', MWRGB(255, 255, 255) }, { 'g', MWRGB(128, 128, 128) }, { 'G', MWRGB(196, 196, 196) },
	{ 'y', MWRGB(246, 206, 60) }, { 'Y', MWRGB(196, 146, 24) }, { 'o', MWRGB(214, 160, 100) }, { 'n', MWRGB(120, 70, 30) },
	{ 'b', MWRGB(50, 100, 210) }, { 'B', MWRGB(130, 170, 240) }, { 'p', MWRGB(236, 128, 150) }, { 'P', MWRGB(200, 80, 110) },
	{ 's', MWRGB(170, 180, 190) },
};
static const int sizes[5] = { 1, 2, 4, 8, 16 };
static const char *const mode_names[3] = { "Outline", "Filled", "Both" };
static const uint32_t palette[28] = {
	0xff000000, 0xff404040, 0xff808080, 0xff800000, 0xff808000, 0xff008000, 0xff008080, 0xff000080, 0xff800080, 0xff804000,
	0xff004040, 0xff0040ff, 0xff4000ff, 0xff804020,
	0xffffffff, 0xffc0c0c0, 0xffe0e0e0, 0xffff0000, 0xffffff00, 0xff00ff00, 0xff00ffff, 0xff0000ff, 0xffff00ff, 0xffff8000,
	0xff00c080, 0xff80c0ff, 0xffc080ff, 0xffffc0a0,
};
enum {
	C_NONE, C_NEW, C_OPEN, C_SAVE, C_SAVE_AS, C_EXIT, C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_DELETE, C_ALL, C_DESELECT,
	C_RESIZE, C_SCALE, C_CROP, C_FLIP_H, C_FLIP_V, C_ROTATE_R, C_ROTATE_L, C_INVERT, C_CLEAR, C_ZOOM_IN, C_ZOOM_OUT, C_ZOOM_1
};
enum { ASK_NONE, ASK_RESIZE, ASK_SCALE };

static GR_WINDOW_ID window;
static int width, height;		/* the window's */
static int view_w, view_h;		/* the part of it the picture shows in, from (TOOLS_W, MENU_H) */
static uint32_t *surface;		/* the window's own buffer, mapped: 0xAARRGGBB a pixel */
static uint32_t surface_at, surface_bytes, buffers_seen;
static volatile uint32_t *buffers;	/* the screen driver's count of buffers made (0x81000064) */
static int surface_row, gpu = -1;

static uint32_t *pix, *back;		/* the picture, and the picture as of the last change that was kept */
static int pw, ph;
static int zoom = 1, ox, oy;		/* the picture's pixel at the view's corner */
static int tool = T_BRUSH, size_index = 2, mode;
static uint32_t fg = 0xff000000, bg = 0xffffffff;
static int hue, sat = 255, value = 0;	/* the foreground as the field and the slider have it */
static char path[300], notice[160];
static int named;			/* the picture has a file: Save writes it without asking */
static int modified, leaving;
static int asking;
static char answer[200];

/* what is being done with the pointer held */
static int held, held_button, from_x, from_y, last_x, last_y;
static int d_x0, d_y0, d_x1, d_y1;	/* the picture's pixels this change has touched */
static int sliding, sliding_button, picking_field;

/* the selection, and a piece of picture that floats in it until it is put down */
static int sel_on, s_x0, s_y0, s_x1, s_y1;
static struct { int on, x, y, w, h, hole, hx0, hy0, hx1, hy1, moving, grab_x, grab_y; uint32_t *buf; } fl;

struct change { int whole, x, y, w, h, pw, ph; uint32_t *data; };
#define HISTORY 48
static struct change done[HISTORY], undone[HISTORY];
static int done_count, undone_count;

static char script[2000];

/* ---- the window's buffer, and the picture shown in it ---- */

static int asked;	/* the window system has been asked to draw since settle() last waited for it */
static int showing;	/* the workers may be writing the window's buffer still (ui_bulk.h) */
static int field_on;	/* the buffer has the field of hues as `field` has it */

static int relaid;	/* the window is another size: all of it is to be drawn, which nobody asks for */
static int following;	/* the pointer's place is asked for: a button is held */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ)

/* Maps the window's buffer, again when the driver has made one since or with `ask` (the window
 * made larger has another, of another size, and the old one is some other window's by then). */
static void
locate(int ask)
{
	GR_WINDOW_INFO info;
	uint32_t offset, count = buffers ? *buffers : 0;
	void *memory;

	if (gpu < 0 || (surface && count == buffers_seen && !ask))
		return;
	buffers_seen = count;
	GrGetWindowInfo(window, &info);
	if (!info.width || !info.height)
		return;
	if ((int)info.width != width || (int)info.height != height)
		relaid = 1;
	else if (surface && info.surface_address == surface_at)
		return;
	if (surface) {
		bulk_wait();
		munmap((void *)((uintptr_t)surface & ~(uintptr_t)4095), surface_bytes);
	}
	surface = NULL, surface_at = 0, field_on = 0;
	width = (int)info.width, height = (int)info.height;
	view_w = width - TOOLS_W, view_h = height - MENU_H - BOTTOM_H - STATUS;
	if (!info.surface_address)
		return;
	surface_row = info.surface_row ? (int)info.surface_row : width;
	offset = info.surface_address - 0x80000000u;
	surface_bytes = (offset & 4095) + (uint32_t)surface_row * (uint32_t)height * 4;
	memory = mmap(NULL, surface_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, offset & ~4095u);
	if (memory == MAP_FAILED)
		return;
	surface = (uint32_t *)((char *)memory + (offset & 4095));
	surface_at = info.surface_address;
}

/* Waits until the window system has drawn what it was asked: the GPU draws on the buffer and
 * copies it back, and what is written into it meanwhile is lost. A pixel read is the request
 * it answers only then (asking where the window is comes back before the GPU is done). */
static void
settle(void)
{
	GR_PIXELVAL pixel;

	if (asked) {
		GrReadArea(window, 0, 0, 1, 1, &pixel);
		asked = 0;
	}
	locate(0);
}

/* The window system is about to be asked to draw: nothing of ours may be on its way into the
 * buffer then, or the GPU's copy back loses it. */
static void
ask_to_draw(void)
{
	if (showing)
		bulk_wait();
	showing = 0;
	asked = 1;
}

/* The selection's edge over what was just shown: every other pair of pixels turned over. */
static void
edge(int sx0, int sy0, int sx1, int sy1)
{
	int ex0 = TOOLS_W + (s_x0 - ox) * zoom, ey0 = MENU_H + (s_y0 - oy) * zoom, ex1 = TOOLS_W + (s_x1 - ox) * zoom - 1,
	    ey1 = MENU_H + (s_y1 - oy) * zoom - 1, x, y;

	if (!sel_on || !surface)
		return;
	for (x = ex0 > sx0 ? ex0 : sx0; x <= ex1 && x < sx1; x++) {
		if (ey0 >= sy0 && ey0 < sy1 && (x >> 2 & 1))
			surface[ey0 * surface_row + x] ^= 0xffffff;
		if (ey1 >= sy0 && ey1 < sy1 && ey1 != ey0 && (x >> 2 & 1))
			surface[ey1 * surface_row + x] ^= 0xffffff;
	}
	for (y = ey0 > sy0 ? ey0 : sy0; y <= ey1 && y < sy1; y++) {
		if (ex0 >= sx0 && ex0 < sx1 && (y >> 2 & 1) && y != ey0 && y != ey1)
			surface[y * surface_row + ex0] ^= 0xffffff;
		if (ex1 >= sx0 && ex1 < sx1 && ex1 != ex0 && (y >> 2 & 1) && y != ey0 && y != ey1)
			surface[y * surface_row + ex1] ^= 0xffffff;
	}
}

static int view_beside;	/* for the next present(): so many pixels right of each row are grey */

/* Shows the picture's pixels from (x0, y0) to before (x1, y1), where they are in view. */
static void
present(int x0, int y0, int x1, int y1)
{
	static uint32_t line[2048];
	int sx0, sy0, sx1, sy1, sy, sx, iy, beside = view_beside;

	view_beside = 0;
	if (!bulk_held)
		bulk_wait();
	settle();	/* (first: the view's size is the buffer's) */
	if (!surface)
		return;
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > pw ? pw : x1, y1 = y1 > ph ? ph : y1;
	sx0 = TOOLS_W + (x0 - ox) * zoom, sy0 = MENU_H + (y0 - oy) * zoom;
	sx1 = TOOLS_W + (x1 - ox) * zoom, sy1 = MENU_H + (y1 - oy) * zoom;
	sx0 = sx0 < TOOLS_W ? TOOLS_W : sx0, sy0 = sy0 < MENU_H ? MENU_H : sy0;
	sx1 = sx1 > TOOLS_W + view_w ? TOOLS_W + view_w : sx1, sy1 = sy1 > MENU_H + view_h ? MENU_H + view_h : sy1;
	if (sx0 >= sx1 || sy0 >= sy1)
		return;
	for (sy = sy0; sy < sy1; sy++) {
		iy = oy + (sy - MENU_H) / zoom;
		if (zoom == 1) {
			/* (the rows as they are: shared out among the cores when they are many) */
			showing = 1;
			bulk_rows_tail(surface + sy * surface_row + sx0, (uint32_t)surface_row * 4, pix + iy * pw + ox + sx0 - TOOLS_W, (uint32_t)pw * 4,
				(uint32_t)(sx1 - sx0) * 4, (uint32_t)(sy1 - sy), (uint32_t)beside * 4, GREY);
			/* (the edge is drawn over the rows: they are there first) */
			if (sel_on)
				bulk_wait();
			break;
		}
		/* a row of the picture is as many rows of the screen as the zoom: made once */
		if (sy == sy0 || (sy - MENU_H) % zoom == 0)
			for (sx = sx0; sx < sx1; sx++)
				line[sx - sx0] = pix[iy * pw + ox + (sx - TOOLS_W) / zoom];
		memcpy(surface + sy * surface_row + sx0, line, (size_t)(sx1 - sx0) * 4);
	}
	edge(sx0, sy0, sx1, sy1);
}

/* The whole view: the picture, and grey where it ends. */
static void
present_view(void)
{
	int right, bottom;

	locate(0);
	if (ox > pw - view_w / zoom)
		ox = pw - view_w / zoom;
	if (oy > ph - view_h / zoom)
		oy = ph - view_h / zoom;
	ox = ox < 0 ? 0 : ox, oy = oy < 0 ? 0 : oy;
	right = TOOLS_W + (pw - ox) * zoom, bottom = MENU_H + (ph - oy) * zoom;
	right = right < TOOLS_W ? TOOLS_W : right > TOOLS_W + view_w ? TOOLS_W + view_w : right;
	bottom = bottom < MENU_H ? MENU_H : bottom > MENU_H + view_h ? MENU_H + view_h : bottom;
	/* the picture, the grey beside it and the grey below it are one hand-over to the workers;
	 * beside is the same rows' own end (two jobs would meet in 16 bytes there) */
	bulk_hold();
	if (zoom == 1 && right < TOOLS_W + view_w)
		view_beside = TOOLS_W + view_w - right;
	present(ox, oy, ox + view_w / zoom + 1, oy + view_h / zoom + 1);
	if (surface) {
		showing = 1;
		if (zoom != 1 && right < TOOLS_W + view_w && bottom > MENU_H)
			bulk_fill_rows(surface + MENU_H * surface_row + right, (uint32_t)surface_row * 4, (uint32_t)(TOOLS_W + view_w - right) * 4,
				(uint32_t)(bottom - MENU_H), GREY);
		if (bottom < MENU_H + view_h)
			bulk_fill_rows(surface + bottom * surface_row + TOOLS_W, (uint32_t)surface_row * 4, (uint32_t)view_w * 4,
				(uint32_t)(MENU_H + view_h - bottom), GREY);
	}
	bulk_go(1);
}

/* ---- changes, kept to be taken back ---- */

/* Room in `back` for a picture of so many bytes: the largest picture's, taken once (its pages
 * are there only as far as they are used). Made anew for each size, it was copied each time. */
static void
back_room(size_t bytes)
{
	static size_t room;

	if (bytes < (size_t)MOST_W * MOST_H * 4)
		bytes = (size_t)MOST_W * MOST_H * 4;
	if (bytes > room) {
		free(back);
		back = malloc(bytes);
		room = bytes;
	}
}

static void
forget(struct change *stack, int *count, int from)
{
	int i;

	for (i = from; i < *count; i++)
		free(stack[i].data);
	*count = from;
}

static void
push(struct change *stack, int *count, struct change c)
{
	if (*count == HISTORY) {
		free(stack[0].data);
		memmove(stack, stack + 1, (HISTORY - 1) * sizeof stack[0]);
		(*count)--;
	}
	stack[(*count)++] = c;
}

/* The pixels from (x0, y0) to before (x1, y1) have changed, and the change is to stay. */
static void
keep(int x0, int y0, int x1, int y1)
{
	struct change c = { 0 };

	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > pw ? pw : x1, y1 = y1 > ph ? ph : y1;
	if (x0 >= x1 || y0 >= y1)
		return;
	c.x = x0, c.y = y0, c.w = x1 - x0, c.h = y1 - y0;
	c.data = malloc((size_t)c.w * (size_t)c.h * 4);
	bulk_rows_kept(c.data, back + y0 * pw + x0, pix + y0 * pw + x0, (uint32_t)pw * 4, (uint32_t)c.w * 4, (uint32_t)c.h);
	forget(undone, &undone_count, 0);
	push(done, &done_count, c);
	modified = 1;
}

/* The picture is another one altogether (another size): `old` is what it was. */
static void
keep_whole(uint32_t *old, int old_w, int old_h)
{
	struct change c = { 1, 0, 0, 0, 0, old_w, old_h, old };

	forget(undone, &undone_count, 0);
	push(done, &done_count, c);
	back_room((size_t)pw * (size_t)ph * 4);
	bulk_flat(back, NULL, pix, (uint32_t)pw * (uint32_t)ph * 4, 0);
	sel_on = 0;
	modified = 1;
}

/* A change taken back, or made again: what it holds and what the picture has change places. */
static void
turn(struct change *c)
{
	uint32_t word;
	int y, x, n;

	if (c->whole) {
		uint32_t *was = pix;

		pix = c->data, c->data = was;
		n = pw, pw = c->pw, c->pw = n;
		n = ph, ph = c->ph, c->ph = n;
		back_room((size_t)pw * (size_t)ph * 4);
		bulk_hold();
		bulk_flat(back, NULL, pix, (uint32_t)pw * (uint32_t)ph * 4, 0);
		sel_on = 0;
		present_view();
		bulk_go(1);
		return;
	}
	for (y = 0; y < c->h; y++) {
		uint32_t *seen = pix + (c->y + y) * pw + c->x, *kept = back + (c->y + y) * pw + c->x, *held = c->data + y * c->w;

		for (x = c->w; x > 0; x--) {
			word = *seen;
			*seen++ = *kept++ = *held;
			*held++ = word;
		}
	}
	bulk_hold();
	present(c->x, c->y, c->x + c->w, c->y + c->h);
	bulk_go(1);
}

/* ---- drawing into the picture ---- */

static void
touch(int x0, int y0, int x1, int y1)
{
	if (d_x1 <= d_x0)
		d_x0 = x0, d_y0 = y0, d_x1 = x1, d_y1 = y1;
	else {
		d_x0 = x0 < d_x0 ? x0 : d_x0, d_y0 = y0 < d_y0 ? y0 : d_y0;
		d_x1 = x1 > d_x1 ? x1 : d_x1, d_y1 = y1 > d_y1 ? y1 : d_y1;
	}
}

static int undrawing;	/* span() puts back what the picture had before this change, not a colour */

static void
span(int x0, int x1, int y, uint32_t colour)
{
	if (y < 0 || y >= ph)
		return;
	x0 = x0 < 0 ? 0 : x0, x1 = x1 > pw ? pw : x1;
	if (x0 >= x1)
		return;
	if (undrawing) {
		memcpy(pix + y * pw + x0, back + y * pw + x0, (size_t)(x1 - x0) * 4);
	} else {
		/* (the row's place in a register: a store here could be one to pw, for all the compiler knows) */
		uint32_t *at = pix + y * pw + x0, *end = at + (x1 - x0);

		for (; at + 4 <= end; at += 4)
			at[0] = at[1] = at[2] = at[3] = colour;
		for (; at < end; at++)
			*at = colour;
	}
}

static void
box(int x0, int y0, int x1, int y1, uint32_t colour)
{
	for (; y0 < y1; y0++)
		span(x0, x1, y0, colour);
}

/* A round dab `size` across. */
static void
dab(int x, int y, int size, uint32_t colour)
{
	int r = size / 2, dy, dx;

	if (size <= 2) {
		box(x, y, x + size, y + size, colour);
	} else
		for (dy = -r; dy <= r; dy++) {
			for (dx = 0; dx * dx + dy * dy <= r * r + r / 2; dx++)
				continue;
			span(x - dx + 1, x + dx, y + dy, colour);
		}
	touch(x - r - 1, y - r - 1, x + r + 2, y + r + 2);
}

static int stroke_dabs;	/* NXPAINT_DABS=1: a stroke is its dabs one after another, as it was */

/* A line of dabs. Dabs a step apart cover one run of a row between them, so each row the line
 * touches is found from where to where and written once (the same pixels as the dabs'). */
static void
stroke(int x0, int y0, int x1, int y1, int size, uint32_t colour)
{
	static int from[MOST_H], to[MOST_H];
	int dx = abs(x1 - x0), dy = abs(y1 - y0), steps = dx > dy ? dx : dy, i, r = size / 2, x, y, row, n, top, bottom;
	int reach[17], rows = size <= 2 ? size : 2 * r + 1;

	if (stroke_dabs || r > 8 || ph > MOST_H) {
		for (i = 0; i <= steps; i++)
			dab(x0 + (steps ? (x1 - x0) * i / steps : 0), y0 + (steps ? (y1 - y0) * i / steps : 0), size, colour);
		return;
	}
	/* a dab's rows: from x - before to x + after, as dab() has them */
	for (i = 0; i < rows; i++) {
		for (n = 0; size > 2 && n * n + (i - r) * (i - r) <= r * r + r / 2; n++)
			continue;
		reach[i] = size <= 2 ? size : n;
	}
	top = (y0 < y1 ? y0 : y1) - (size <= 2 ? 0 : r), bottom = (y0 < y1 ? y1 : y0) + (size <= 2 ? size : r + 1);
	top = top < 0 ? 0 : top, bottom = bottom > ph ? ph : bottom;
	for (row = top; row < bottom; row++)
		from[row] = 1 << 30, to[row] = -(1 << 30);
	for (i = 0; i <= steps; i++) {
		x = x0 + (steps ? (x1 - x0) * i / steps : 0), y = y0 + (steps ? (y1 - y0) * i / steps : 0);
		for (n = 0, row = y - (size <= 2 ? 0 : r); n < rows; n++, row++) {
			int left = size <= 2 ? x : x - reach[n] + 1, right = x + reach[n];

			if (row < 0 || row >= ph)
				continue;
			if (left < from[row])
				from[row] = left;
			if (right > to[row])
				to[row] = right;
		}
	}
	for (row = top; row < bottom; row++)
		span(from[row], to[row], row, colour);
	touch((x0 < x1 ? x0 : x1) - r - 1, (y0 < y1 ? y0 : y1) - r - 1, (x0 < x1 ? x1 : x0) + r + 2, (y0 < y1 ? y1 : y0) + r + 2);
}

/* A rectangle or an oval between two corners: its edge `size` thick, its inside, or both. */
static void
shape(int oval, int x0, int y0, int x1, int y1, int size, uint32_t line, uint32_t inside)
{
	int y, n, left, right, in_left, in_right;
	float cx, cy, rx, ry, t, half;

	if (x0 > x1)
		n = x0, x0 = x1, x1 = n;
	if (y0 > y1)
		n = y0, y0 = y1, y1 = n;
	x1++, y1++;
	touch(x0, y0, x1, y1);
	if (mode == 1)
		line = inside, size = 1 << 20;
	cx = (float)(x0 + x1) * 0.5f, cy = (float)(y0 + y1) * 0.5f, rx = (float)(x1 - x0) * 0.5f, ry = (float)(y1 - y0) * 0.5f;
	for (y = y0; y < y1; y++) {
		if (!oval) {
			if (mode == 2)
				span(x0, x1, y, inside);
			if (y - y0 < size || y1 - 1 - y < size)
				span(x0, x1, y, line);
			else
				span(x0, x0 + size, y, line), span(x1 - size, x1, y, line);
			continue;
		}
		/* the row's pixels whose middles are in the oval: none of them outside the corners' box */
		t = ((float)y + 0.5f - cy) / ry;
		half = rx * sqrtf(1.0f - t * t);
		left = (int)floorf(cx - half + 0.5f), right = (int)floorf(cx + half + 0.5f);
		in_left = in_right = 0;
		if (ry - (float)size > 0.5f && rx - (float)size > 0.5f) {
			t = ((float)y + 0.5f - cy) / (ry - (float)size);
			if (t > -1.0f && t < 1.0f) {
				half = (rx - (float)size) * sqrtf(1.0f - t * t);
				in_left = (int)floorf(cx - half + 0.5f), in_right = (int)floorf(cx + half + 0.5f);
			}
		}
		if (in_left >= in_right)
			span(left, right, y, line);
		else {
			span(left, in_left, y, line), span(in_right, right, y, line);
			if (mode == 2)
				span(in_left, in_right, y, inside);
		}
	}
}

/* Every pixel that can be reached from (x, y) through pixels of its colour takes `colour`. */
static int flood_plain;	/* NXPAINT_FLOOD=plain: the flood as it was, every row looked at from both sides */

/* The same by runs that remember which way they came: a pixel is looked at once where it is
 * filled, and the row a run came from only where the run is wider than its parent. */
static void
flood_runs(int x, int y, uint32_t colour, uint32_t old)
{
	uint32_t *all = pix, *row;
	int w = pw, h = ph, room = 1024, top = 0, x1, x2, dy, lx0 = x, ly0 = y, lx1 = x + 1, ly1 = y + 1;
	int *stack = malloc((size_t)room * 4 * sizeof *stack);

#define FLOOD_PUSH(a, b, c, d) do { \
		if ((c) >= 0 && (c) < h) { \
			if (top == room) \
				room *= 2, stack = realloc(stack, (size_t)room * 4 * sizeof *stack); \
			stack[top * 4] = (a), stack[top * 4 + 1] = (b), stack[top * 4 + 2] = (c), stack[top * 4 + 3] = (d), top++; \
		} \
	} while (0)
	FLOOD_PUSH(x, x, y, 1);
	FLOOD_PUSH(x, x, y - 1, -1);
	while (top) {
		top--;
		x1 = stack[top * 4], x2 = stack[top * 4 + 1], y = stack[top * 4 + 2], dy = stack[top * 4 + 3];
		row = all + y * w;
		x = x1;
		if (row[x] == old) {
			while (x > 0 && row[x - 1] == old)
				row[--x] = colour;
			if (x < x1)
				FLOOD_PUSH(x, x1 - 1, y - dy, -dy);
		}
		while (x1 <= x2) {
			while (x1 < w && row[x1] == old)
				row[x1++] = colour;
			if (x1 > x) {
				FLOOD_PUSH(x, x1 - 1, y + dy, dy);
				lx0 = x < lx0 ? x : lx0, lx1 = x1 > lx1 ? x1 : lx1, ly0 = y < ly0 ? y : ly0, ly1 = y + 1 > ly1 ? y + 1 : ly1;
			}
			if (x1 - 1 > x2)
				FLOOD_PUSH(x2 + 1, x1 - 1, y - dy, -dy);
			x1++;
			while (x1 < x2 && row[x1] != old)
				x1++;
			x = x1;
		}
	}
#undef FLOOD_PUSH
	free(stack);
	touch(lx0, ly0, lx1, ly1);
}

static void
flood(int x, int y, uint32_t colour)
{
	uint32_t old;
	int *stack, room = 1024, top = 0, left, right, i, dy;

	if (x < 0 || y < 0 || x >= pw || y >= ph || (old = pix[y * pw + x]) == colour)
		return;
	if (!flood_plain) {
		flood_runs(x, y, colour, old);
		return;
	}
	stack = malloc((size_t)room * 2 * sizeof *stack);
	stack[0] = x, stack[1] = y, top = 1;
	while (top) {
		top--;
		x = stack[top * 2], y = stack[top * 2 + 1];
		if (pix[y * pw + x] != old)
			continue;
		for (left = x; left > 0 && pix[y * pw + left - 1] == old; left--)
			continue;
		for (right = x; right < pw - 1 && pix[y * pw + right + 1] == old; right++)
			continue;
		for (i = left; i <= right; i++)
			pix[y * pw + i] = colour;
		touch(left, y, right + 1, y + 1);
		/* the rows above and below: one place to go on from for every run of the old colour */
		for (dy = -1; dy <= 1; dy += 2) {
			if (y + dy < 0 || y + dy >= ph)
				continue;
			for (i = left; i <= right; i++)
				if (pix[(y + dy) * pw + i] == old && (i == left || pix[(y + dy) * pw + i - 1] != old)) {
					if (top == room) {
						room *= 2;
						stack = realloc(stack, (size_t)room * 2 * sizeof *stack);
					}
					stack[top * 2] = i, stack[top * 2 + 1] = y + dy, top++;
				}
		}
	}
	free(stack);
}

static void
spray(int x, int y, int size, uint32_t colour)
{
	static unsigned chance = 2463534242u;
	int reach = size * 2 + 3, i, dx, dy;

	for (i = 0; i < size * 3 + 6; i++) {
		chance ^= chance << 13, chance ^= chance >> 17, chance ^= chance << 5;
		dx = (int)(chance % (unsigned)(2 * reach + 1)) - reach, dy = (int)(chance / 4096 % (unsigned)(2 * reach + 1)) - reach;
		if (dx * dx + dy * dy <= reach * reach)
			span(x + dx, x + dx + 1, y + dy, colour);
	}
	touch(x - reach, y - reach, x + reach + 1, y + reach + 1);
}

/* The picture's pixels as they were before this change began. */
static void
restore(int x0, int y0, int x1, int y1)
{
	x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > pw ? pw : x1, y1 = y1 > ph ? ph : y1;
	if (y0 < y1 && x0 < x1)
		bulk_rows(pix + y0 * pw + x0, (uint32_t)pw * 4, back + y0 * pw + x0, (uint32_t)pw * 4, (uint32_t)(x1 - x0) * 4, (uint32_t)(y1 - y0));
}

/* ---- the selection, and what floats in it ---- */

/* The floating piece where it is now: the picture under it and where it was is the kept one. */
static void
float_show(int was_x, int was_y)
{
	int x0 = was_x < fl.x ? was_x : fl.x, y0 = was_y < fl.y ? was_y : fl.y, x1 = (was_x > fl.x ? was_x : fl.x) + fl.w,
	    y1 = (was_y > fl.y ? was_y : fl.y) + fl.h, y, x, n;

	if (fl.hole)
		x0 = fl.hx0 < x0 ? fl.hx0 : x0, y0 = fl.hy0 < y0 ? fl.hy0 : y0, x1 = fl.hx1 > x1 ? fl.hx1 : x1, y1 = fl.hy1 > y1 ? fl.hy1 : y1;
	restore(x0, y0, x1, y1);
	if (fl.hole)
		box(fl.hx0, fl.hy0, fl.hx1, fl.hy1, bg);
	for (y = 0; y < fl.h; y++) {
		if (fl.y + y < 0 || fl.y + y >= ph)
			continue;
		x = fl.x < 0 ? -fl.x : 0;
		n = fl.w - x - (fl.x + fl.w > pw ? fl.x + fl.w - pw : 0);
		if (n > 0)
			memcpy(pix + (fl.y + y) * pw + fl.x + x, fl.buf + y * fl.w + x, (size_t)n * 4);
	}
	touch(x0, y0, x1, y1);
	s_x0 = fl.x, s_y0 = fl.y, s_x1 = fl.x + fl.w, s_y1 = fl.y + fl.h, sel_on = 1;
	present(x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

/* Puts the floating piece down for good, or with `drop` takes it away again. */
static void
float_end(int drop)
{
	if (!fl.on)
		return;
	fl.on = 0;
	if (drop) {
		restore(d_x0, d_y0, d_x1, d_y1);
		sel_on = 0;
		present(d_x0 - 1, d_y0 - 1, d_x1 + 1, d_y1 + 1);
	} else
		keep(d_x0, d_y0, d_x1, d_y1);
	free(fl.buf);
	fl.buf = NULL;
	d_x1 = d_x0 = 0;
}

static uint32_t *
copy_of(int x0, int y0, int x1, int y1)
{
	uint32_t *copy = malloc((size_t)(x1 - x0) * (size_t)(y1 - y0) * 4);
	int y;

	for (y = y0; y < y1; y++)
		memcpy(copy + (y - y0) * (x1 - x0), pix + y * pw + x0, (size_t)(x1 - x0) * 4);
	return copy;
}

/* The selected piece comes loose, leaving the background's colour where it was. */
static void
lift(void)
{
	if (fl.on || !sel_on)
		return;
	memset(&fl, 0, sizeof fl);
	fl.on = 1, fl.x = s_x0, fl.y = s_y0, fl.w = s_x1 - s_x0, fl.h = s_y1 - s_y0;
	fl.buf = copy_of(s_x0, s_y0, s_x1, s_y1);
	fl.hole = 1, fl.hx0 = s_x0, fl.hy0 = s_y0, fl.hx1 = s_x1, fl.hy1 = s_y1;
	d_x1 = d_x0 = 0;
}

static void
select_none(void)
{
	int x0 = s_x0, y0 = s_y0, x1 = s_x1, y1 = s_y1;

	float_end(0);
	if (!sel_on)
		return;
	sel_on = 0;
	present(x0 - 1, y0 - 1, x1 + 1, y1 + 1);
}

/* ---- files: PPM, three bytes a pixel ---- */

/* The next number of a PPM's header, past blanks and # lines, and the one character after it. */
static int
ppm_number(FILE *file)
{
	int ch, n = 0, digits = 0;

	while ((ch = fgetc(file)) != EOF && (isspace(ch) || ch == '#'))
		while (ch == '#' && (ch = fgetc(file)) != EOF && ch != '\n')
			ch = '#';
	for (; ch >= '0' && ch <= '9' && n < 100000; ch = fgetc(file), digits++)
		n = n * 10 + ch - '0';
	return digits ? n : -1;
}

static uint32_t *
read_ppm(const char *file_path, int *w, int *h)
{
	FILE *file = fopen(file_path, "rb");
	uint32_t *pixels = NULL;
	unsigned char *row = NULL;
	int x, y, good;

	if (!file)
		return NULL;
	good = fgetc(file) == 'P' && fgetc(file) == '6';
	*w = good ? ppm_number(file) : 0, *h = good ? ppm_number(file) : 0;
	good = good && *w >= 1 && *h >= 1 && *w <= MOST_W && *h <= MOST_H && ppm_number(file) == 255;
	if (good)
		pixels = malloc((size_t)*w * (size_t)*h * 4), row = malloc((size_t)*w * 3);
	if (!pixels || !row) {
		free(pixels);
		free(row);
		fclose(file);
		return NULL;
	}
	for (y = 0; y < *h; y++) {
		if (fread(row, 3, (size_t)*w, file) != (size_t)*w)
			memset(row, 255, (size_t)*w * 3);
		for (x = 0; x < *w; x++)
			pixels[y * *w + x] = 0xff000000u | (uint32_t)row[3 * x] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x + 2];
	}
	free(row);
	fclose(file);
	return pixels;
}

/* Written beside the file and then put in its place: a write that fails (errno says why)
 * leaves the file that was there. The kind is the name's: PNG, JPEG, GIF, BMP or PPM. */
static int
write_picture(const char *file_path, const uint32_t *pixels, int w, int h)
{
	struct media_image image;
	char beside[320];
	const char *failed = NULL;	/* the step that did not go, for the log */
	uint32_t size = 0, done = 0;
	uint8_t *data = NULL;
	int fd = -1, why;

	memset(&image, 0, sizeof image);
	image.width = w, image.height = h, image.channels = 4, image.bgr = 1, image.stride = w * 4;
	image.pixels = (uint8_t *)pixels;
	snprintf(beside, sizeof beside, "%.300s.new", file_path);
	bulk_wait();	/* (the workers are the encoder's now) */
	switch (media_kind_of_name(file_path)) {
	case MEDIA_JPEG: data = media_jpeg_encode(&image, 92, &size, mcw_most()); break;
	case MEDIA_GIF: data = media_gif_encode(&image, &size, mcw_most()); break;
	case MEDIA_BMP: data = media_bmp_encode(&image, &size); break;
	case MEDIA_PNM: data = media_ppm_encode(&image, &size); break;
	default: data = media_png_encode(&image, 1, &size, mcw_most()); break;
	}
	if (!data) {
		failed = "no memory to encode", errno = ENOMEM;
	} else if ((fd = open(beside, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		failed = "cannot make";
	} else {
		while (done < size && !failed) {
			ssize_t n = write(fd, data + done, size - done);

			if (n <= 0)
				failed = "cannot write";
			else
				done += (uint32_t)n;
		}
		why = errno;
		if (close(fd) != 0 && !failed)
			failed = "cannot finish", why = errno;
		errno = why;
		if (!failed && rename(beside, file_path) != 0)
			failed = "cannot put in the file's place";
	}
	if (data)
		media_free(data, size);
	if (failed) {
		why = errno;
		fprintf(stderr, "nxpaint: save: %s %s (%u of %u bytes): %s\n", failed, beside, done, size, strerror(why));
		if (fd >= 0)
			unlink(beside);
		errno = why;
	}
	return !failed;
}

/* The clipboard between paint windows: a PPM. */
static int
write_ppm(const char *file_path, const uint32_t *pixels, int w, int h)
{
	return write_picture(file_path, pixels, w, h);
}

/* A new picture in place of this one, its history forgotten; `white`: its pixels are still to
 * be made so. The workers are left at it: whatever uses the picture next waits for them. */
static void
begin(uint32_t *pixels, int w, int h, int white)
{
	bulk_wait();
	forget(done, &done_count, 0);
	forget(undone, &undone_count, 0);
	free(fl.buf);
	memset(&fl, 0, sizeof fl);
	free(pix);
	pix = pixels, pw = w, ph = h;
	back_room((size_t)w * (size_t)h * 4);
	bulk_hold();
	if (white)
		bulk_flat(pix, back, NULL, (uint32_t)w * (uint32_t)h * 4, 0xffffffffu);
	else
		bulk_flat(back, NULL, pix, (uint32_t)w * (uint32_t)h * 4, 0);
	bulk_go(1);
	sel_on = modified = ox = oy = 0;
	d_x1 = d_x0 = 0;
	zoom = 1;
}

static void
blank(int w, int h)
{
	begin(malloc((size_t)w * (size_t)h * 4), w, h, 1);
}

static void
titled(void)
{
	char title[340];

	snprintf(title, sizeof title, "%s - Paint", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	if (window)
		GrSetWindowTitle(window, title);
}

/* Where pictures are opened from and saved to, unless another folder is walked to. */
static const char *
folder(void)
{
	return access(PICTURES, R_OK) == 0 ? PICTURES : "/root";
}

/* The picture has no file yet: Save will ask for one. */
static void
unnamed(void)
{
	snprintf(path, sizeof path, "%s/picture.png", folder());
	named = 0;
	titled();
}

/* Opens a picture of any kind libmedia reads, on the worker cores; one larger than Paint's most
 * is made smaller to fit. See-through pixels are laid on white. */
static int
open_file(const char *file_path)
{
	struct media_image image;
	struct media_info info;
	struct stat st;
	uint8_t *data;
	uint32_t *pixels;
	const char *why;
	int fd = open(file_path, O_RDONLY), w, h;

	if (fd < 0)
		return 0;
	if (fstat(fd, &st) || st.st_size < 8 || (data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0)) == MAP_FAILED) {
		close(fd);
		return 0;
	}
	close(fd);
	if (media_probe(data, (uint32_t)st.st_size, &info) || info.width < 1 || info.height < 1) {
		munmap(data, (size_t)st.st_size);
		return 0;
	}
	w = info.width, h = info.height;
	if (w > MOST_W || h > MOST_H) {
		if ((int64_t)w * MOST_H > (int64_t)h * MOST_W)
			h = (int)((int64_t)h * MOST_W / w), w = MOST_W;
		else
			w = (int)((int64_t)w * MOST_H / h), h = MOST_H;
	}
	bulk_wait();
	if (!(pixels = malloc((size_t)w * (size_t)h * 4))) {
		munmap(data, (size_t)st.st_size);
		return 0;
	}
	/* (its pages there before a worker writes them) */
	mcw_touch(pixels, (unsigned)w * (unsigned)h * 4);
	memset(&image, 0, sizeof image);
	image.width = w, image.height = h, image.channels = 4, image.bgr = 1, image.opaque = 1, image.stride = w * 4;
	image.pixels = (uint8_t *)pixels, image.background = 0xffffff;
	why = media_image_decode_fit(data, (uint32_t)st.st_size, &image, 0, mcw_most());
	munmap(data, (size_t)st.st_size);
	if (why) {
		fprintf(stderr, "nxpaint: %s: %s\n", file_path, why);
		free(pixels);
		return 0;
	}
	begin(pixels, w, h, 0);
	snprintf(path, sizeof path, "%s", file_path);
	named = 1;
	titled();
	return 1;
}

/* Saves to `wanted`, of the kind its ending asks for; a name with none of them gets ".png". */
static void
save_to(const char *wanted)
{
	const char *base = strrchr(wanted, '/') ? strrchr(wanted, '/') + 1 : wanted;
	char to[300];
	int added = 0;

	/* (what was asked for, what it became and how it went are said on the console too) */
	fprintf(stderr, "nxpaint: save: asked for \"%s\", %d x %d\n", wanted, pw, ph);
	if (strlen(wanted) + 5 >= sizeof to) {
		snprintf(notice, sizeof notice, "NOT saved: the name is too long (%d characters)", (int)strlen(wanted));
	} else if (!base[0] || base[0] == '.') {
		/* (no folder shows a name that starts with a dot: the picture would seem lost) */
		snprintf(notice, sizeof notice, "NOT saved: \"%.60s\" is not a name (it starts with a dot or is empty)", base);
	} else {
		snprintf(to, sizeof to, "%s", wanted);
		if (!ui_files_wanted(base, KINDS)) {
			strcat(to, ".png");
			added = 1;
		}
		if (!write_picture(to, pix, pw, ph)) {
			snprintf(notice, sizeof notice, "NOT saved: cannot write %.90s (%s)", to, strerror(errno));
		} else {
			snprintf(path, sizeof path, "%s", to);
			named = 1, modified = 0;
			titled();
			snprintf(notice, sizeof notice, added ? "saved as %.90s (a PNG: the name asked for no kind Paint writes)" : "saved as %.120s", path);
		}
	}
	fprintf(stderr, "nxpaint: save: %s\n", notice);
}

/* ---- colours ---- */

static uint32_t
from_hsv(int h, int s, int v)
{
	int sector = h * 6 / 256, f = h * 6 % 256, p = v * (255 - s) / 255, q = v * (255 - s * f / 255) / 255,
	    t = v * (255 - s * (255 - f) / 255) / 255, r, g, b;

	switch (sector) {
	case 0: r = v, g = t, b = p; break;
	case 1: r = q, g = v, b = p; break;
	case 2: r = p, g = v, b = t; break;
	case 3: r = p, g = q, b = v; break;
	case 4: r = t, g = p, b = v; break;
	default: r = v, g = p, b = q; break;
	}
	return 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

/* The foreground is another colour: where it is in the field, and on the slider. */
static void
foreground(uint32_t colour)
{
	int r = (int)(colour >> 16 & 255), g = (int)(colour >> 8 & 255), b = (int)(colour & 255);
	int most = r > g ? (r > b ? r : b) : (g > b ? g : b), least = r < g ? (r < b ? r : b) : (g < b ? g : b), range = most - least;

	fg = colour | 0xff000000u;
	value = most;
	sat = most ? range * 255 / most : 0;
	if (range)
		hue = (most == r ? (g - b) * 256 / range / 6 : most == g ? 256 / 3 + (b - r) * 256 / range / 6 : 512 / 3 + (r - g) * 256 / range / 6) & 255;
}

/* ---- the panels ---- */

static int
bottom_y(void)
{
	return height - STATUS - BOTTOM_H;
}

static char status_text[420];	/* what the status line and the tools show now: they are drawn */
static int status_on, tools_on;	/* again only when they are to show something else */

static void
draw_status(void)
{
	static const char *const asks[] = { "", "Canvas size, WIDTHxHEIGHT: ", "Scale to percent: " };
	char text[420];

	if (asking)
		snprintf(text, sizeof text, "%s%s_   (Enter, or Escape to leave)", asks[asking], answer);
	else
		snprintf(text, sizeof text, "%s%s   %d x %d, %dx   %s", tool_names[tool], modified ? " *" : "", pw, ph, zoom, notice);
	/* (a line that says what it said is left alone: asking for it holds up what is shown next) */
	if (status_on && !strcmp(text, status_text))
		return;
	snprintf(status_text, sizeof status_text, "%s", text);
	status_on = 1;
	ask_to_draw();
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_bevel(window, 0, height - STATUS, width, STATUS, 1);
	ui_text_fit(window, 6, height - STATUS + 3, width - 12, text, BLACK);
}

/* Tool t's picture at x, y: a one-bit mask a colour, each a rectangle of the GPU's. */
static void
tool_picture(int t, int x, int y)
{
	GR_BITMAP bits[16];
	unsigned k;
	int row, col, any;

	for (k = 0; k < sizeof tool_inks / sizeof tool_inks[0]; k++) {
		for (row = 0, any = 0; row < 16; row++)
			for (col = 0, bits[row] = 0; col < 16; col++)
				if (tool_pictures[t][row][col] == tool_inks[k].letter)
					bits[row] |= (GR_BITMAP)(0x8000 >> col), any = 1;
		if (!any)
			continue;
		GrSetGCForeground(ui_gc, tool_inks[k].colour);
		GrBitmap(window, ui_gc, x, y, 16, 16, bits);
	}
}

static void
draw_tools(void)
{
	int i, y = MENU_H + 4 + 5 * 26 + 6, now = 1 + tool + size_index * 16 + mode * 256 + zoom * 4096;
	char text[16];

	if (tools_on == now)
		return;
	tools_on = now;
	ask_to_draw();
	ui_fill(window, 0, MENU_H, TOOLS_W, height - MENU_H - BOTTOM_H - STATUS, UI_FACE);
	for (i = 0; i < TOOL_COUNT; i++) {
		int x = 3 + (i & 1) * 33, ty = MENU_H + 4 + (i / 2) * 26, in = i == tool && !ui_seven;

		ui_button(window, x, ty, 32, 24, "", i == tool);
		tool_picture(i, x + 8 + in, ty + 4 + in);
	}
	ui_text(window, 4, y, "Size", -1, BLACK, 0);
	for (i = 0; i < 5; i++) {
		int dot = sizes[i] > 9 ? 9 : sizes[i];

		ui_button(window, 3 + i * 13, y + 16, 13, 18, "", i == size_index);
		ui_fill(window, 3 + i * 13 + 6 - dot / 2, y + 25 - dot / 2, dot, dot, BLACK);
	}
	ui_text(window, 4, y + 42, "Shapes", -1, BLACK, 0);
	for (i = 0; i < 3; i++)
		ui_button(window, 3, y + 58 + i * 22, TOOLS_W - 6, 20, mode_names[i], i == mode);
	ui_text(window, 4, y + 130, "Zoom", -1, BLACK, 0);
	ui_button(window, 3, y + 146, 20, 20, " -", 0);
	snprintf(text, sizeof text, "%dx", zoom);
	ui_text(window, 30, y + 149, text, -1, BLACK, 0);
	ui_button(window, TOOLS_W - 23, y + 146, 20, 20, " +", 0);
}

/* A picture's colour (red highest) as the window system is told one (red lowest). */
static GR_COLOR
shown(uint32_t colour)
{
	return MWRGB(colour >> 16 & 255, colour >> 8 & 255, colour & 255);
}

static uint32_t field[FIELD_H][FIELD_W];	/* the field of hues at lightness field_value, its mark in it */
static int field_value = -1, mark_x = -1, mark_y;

/* The mark where the foreground is in the field, turned over at (x, y): in `field`, and with
 * `shown` in the window's buffer too. */
static void
field_mark(int x, int y, int shown)
{
	uint32_t *there = surface + (bottom_y() + 9) * surface_row + 60 + 14 * 18 + 12;
	int n;

	for (n = -3; n <= 3; n++) {
		if (x + n >= 0 && x + n < FIELD_W) {
			field[y][x + n] ^= 0xffffff;
			if (shown)
				there[y * surface_row + x + n] = field[y][x + n];
		}
		if (n && y + n >= 0 && y + n < FIELD_H) {
			field[y + n][x] ^= 0xffffff;
			if (shown)
				there[(y + n) * surface_row + x] = field[y + n][x];
		}
	}
}

/* The field: every hue across, paler downwards, at the slider's lightness. Copied into the
 * buffer when that lacks it; otherwise only the mark moves. */
static void
field_show(void)
{
	int y = bottom_y(), fx = 60 + 14 * 18 + 12, x = hue * (FIELD_W - 1) / 255, at = (255 - sat) * (FIELD_H - 1) / 255, row, column;

	if (!surface || width < fx + FIELD_W + 10 + 130)
		return;
	if (field_value != (value ? value : 255)) {
		field_value = value ? value : 255;
		for (row = 0; row < FIELD_H; row++)
			for (column = 0; column < FIELD_W; column++)
				field[row][column] = from_hsv(column * 255 / (FIELD_W - 1), 255 - row * 255 / (FIELD_H - 1), field_value);
		mark_x = -1, field_on = 0;
	}
	if (field_on && mark_x == x && mark_y == at)
		return;
	if (mark_x >= 0)
		field_mark(mark_x, mark_y, field_on);
	field_mark(mark_x = x, mark_y = at, field_on);
	if (!field_on) {
		showing = 1;
		bulk_rows(surface + (y + 9) * surface_row + fx, (uint32_t)surface_row * 4, field, sizeof field[0], sizeof field[0], FIELD_H);
		field_on = 1;
	}
}

/* The colours along the bottom, as the window system draws them: the two in hand, the
 * palette, the slider. The field's place is left as it is when the buffer has the field. */
static void
ask_colours(void)
{
	int y, i, fx = 60 + 14 * 18 + 12, sx = fx + FIELD_W + 10;
	char text[40];

	locate(0);
	y = bottom_y();
	ask_to_draw();
	if (field_on && width >= sx + 130) {
		ui_fill(window, 0, y, fx, BOTTOM_H, UI_FACE);
		ui_fill(window, fx + FIELD_W, y, width - fx - FIELD_W, BOTTOM_H, UI_FACE);
		ui_fill(window, fx, y, FIELD_W, 9, UI_FACE);
		ui_fill(window, fx, y + 9 + FIELD_H, FIELD_W, BOTTOM_H - 9 - FIELD_H, UI_FACE);
	} else {
		ui_fill(window, 0, y, width, BOTTOM_H, UI_FACE);
		field_on = 0;
	}
	ui_fill(window, 0, y, width, 1, UI_SHADOW);
	ui_fill(window, 22, y + 22, 28, 28, shown(bg));
	ui_bevel(window, 22, y + 22, 28, 28, 1);
	ui_fill(window, 6, y + 8, 28, 28, shown(fg));
	ui_bevel(window, 6, y + 8, 28, 28, 1);
	for (i = 0; i < 28; i++) {
		ui_fill(window, 60 + (i % 14) * 18, y + 10 + (i / 14) * 20, 17, 17, shown(palette[i]));
		ui_bevel(window, 60 + (i % 14) * 18, y + 10 + (i / 14) * 20, 17, 17, 1);
	}
	if (width < sx + 130)
		return;
	ui_slider(window, sx, y + 6, 124, value, 255);
	snprintf(text, sizeof text, "#%06X  light %d%%", (unsigned)(fg & 0xffffff), value * 100 / 255);
	ui_text(window, sx + 4, y + 30, text, -1, BLACK, 0);
	ui_text(window, sx + 4, y + 44, "right button: the other", -1, UI_SHADOW, 0);
}

static void
draw_colours(void)
{
	ask_colours();
	settle();
	bulk_hold();
	field_show();
	bulk_go(1);
}

static const char *const menu_names[] = { "File", "Edit", "Image", "View" };
static int menu_x[5];
static const struct ui_menu_item file_items[] = {
	{ "New   Ctrl+N", -1, C_NEW }, { "Open...   Ctrl+O", -1, C_OPEN }, { "Save   Ctrl+S", -1, C_SAVE }, { "Save as...", -1, C_SAVE_AS },
	{ "Exit   Ctrl+Q", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item edit_items[] = {
	{ "Undo   Ctrl+Z", -1, C_UNDO }, { "Redo   Ctrl+Y", -1, C_REDO }, { "Cut   Ctrl+X", -1, C_CUT, NULL, 0, UI_MENU_LINE },
	{ "Copy   Ctrl+C", -1, C_COPY }, { "Paste   Ctrl+V", -1, C_PASTE }, { "Delete", -1, C_DELETE },
	{ "Select all   Ctrl+A", -1, C_ALL, NULL, 0, UI_MENU_LINE }, { "Select nothing   Escape", -1, C_DESELECT },
};
static const struct ui_menu_item image_items[] = {
	{ "Canvas size...", -1, C_RESIZE }, { "Scale...", -1, C_SCALE }, { "Crop to the selection", -1, C_CROP },
	{ "Flip left to right", -1, C_FLIP_H, NULL, 0, UI_MENU_LINE }, { "Flip top to bottom", -1, C_FLIP_V },
	{ "Turn right", -1, C_ROTATE_R }, { "Turn left", -1, C_ROTATE_L },
	{ "Invert the colours", -1, C_INVERT, NULL, 0, UI_MENU_LINE }, { "Clear", -1, C_CLEAR },
};
static const struct ui_menu_item view_items[] = {
	{ "Zoom in   +", -1, C_ZOOM_IN }, { "Zoom out   -", -1, C_ZOOM_OUT }, { "Actual size   1", -1, C_ZOOM_1 },
};
static const struct { const struct ui_menu_item *items; int count; } menus[4] = { { file_items, 5 }, { edit_items, 8 }, { image_items, 9 }, { view_items, 3 } };

static void
draw_menu_bar(void)
{
	int i;

	ask_to_draw();
	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 4; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

static void
draw_all(void)
{
	locate(0);
	relaid = 0;
	status_on = tools_on = field_on = 0;
	/* what the window system draws first, and waited for once; then what is written into
	 * the buffer, all of it one hand-over to the workers, who are left at it */
	draw_menu_bar();
	draw_tools();
	draw_status();
	ask_colours();
	settle();
	bulk_hold();
	field_show();
	present_view();
	bulk_go(1);
}

/* ---- commands ---- */

#define CLIP "/tmp/nxpaint-clip.ppm"

/* The whole picture as another one of another size: `how` says what goes where. */
static void
remade(int w, int h, int how)
{
	uint32_t *old = pix, *made;
	int x, y, old_w = pw, old_h = ph;

	if (w < 1 || h < 1 || w > MOST_W || h > MOST_H) {
		snprintf(notice, sizeof notice, "a picture is from 1 x 1 to %d x %d", MOST_W, MOST_H);
		return;
	}
	made = malloc((size_t)w * (size_t)h * 4);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int sx = x, sy = y, inside;

			if (how == C_SCALE)
				sx = x * old_w / w, sy = y * old_h / h;
			else if (how == C_CROP)
				sx = x + s_x0, sy = y + s_y0;
			else if (how == C_ROTATE_R)
				sx = y, sy = old_h - 1 - x;
			else if (how == C_ROTATE_L)
				sx = old_w - 1 - y, sy = x;
			inside = sx >= 0 && sy >= 0 && sx < old_w && sy < old_h;
			made[y * w + x] = inside ? old[sy * old_w + sx] : bg;
		}
	pix = made, pw = w, ph = h;
	bulk_hold();
	keep_whole(old, old_w, old_h);
	ox = oy = 0;
	present_view();
	bulk_go(1);
}

static void
ask(int what, const char *start)
{
	asking = what;
	snprintf(answer, sizeof answer, "%s", start);
	draw_status();
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
}

static void command(int id);

/* True if the picture may be let go: it has no changes, or the answer was to save them or not
 * to. A test's script is asked nothing: its second "quit" leaves, as it did. */
static int
may_drop(void)
{
	static const char *const answers[] = { "Save", "Don't save", "Cancel" };
	char text[400];
	int answer;

	if (!modified)
		return 1;
	if (getenv("NXPAINT_SCRIPT")) {
		if (leaving)
			return 1;
		leaving = 1;
		snprintf(notice, sizeof notice, "not saved: Ctrl+S saves, Exit once more leaves without");
		return 0;
	}
	snprintf(text, sizeof text, "%.200s has changes that are not saved.\nSave them?", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	answer = ui_ask("Paint", text, answers, 3, others);
	if (answer == 0)
		command(C_SAVE);
	draw_all();
	return answer == 1 || (answer == 0 && !modified);
}

static void
command(int id)
{
	char chosen[300], text[40], where[300], *slash;
	uint32_t *piece;
	int x, y, w, h, x0, y0, x1, y1;

	bulk_wait();
	notice[0] = 0;
	if (id != C_EXIT)
		leaving = 0;
	if (id != C_COPY && id != C_CUT && id != C_DELETE && id != C_CROP && id != C_INVERT && id != C_ZOOM_IN && id != C_ZOOM_OUT && id != C_ZOOM_1)
		float_end(0);
	x0 = sel_on ? s_x0 : 0, y0 = sel_on ? s_y0 : 0, x1 = sel_on ? s_x1 : pw, y1 = sel_on ? s_y1 : ph;
	switch (id) {
	case C_NEW:
		/* a white picture of this one's size at once: "Canvas size" is where another is asked for */
		if (!may_drop())
			break;
		blank(pw, ph);
		unnamed();
		draw_all();
		return;
	case C_OPEN:
		if (!may_drop())
			break;
		if (ui_choose_file("Open a picture", folder(), OPENED, chosen, sizeof chosen, others) && !open_file(chosen))
			snprintf(notice, sizeof notice, "that is not a picture this can open");
		draw_all();
		return;
	case C_SAVE:
		/* (a picture opened from a kind Paint does not write is asked what to be saved as) */
		if (named && ui_files_wanted(path, KINDS)) {
			save_to(path);
			break;
		}
		/* fall through: a picture with no file is asked what to be saved as */
	case C_SAVE_AS:
		/* in the picture's own folder with its own name, or where pictures are opened from */
		snprintf(where, sizeof where, "%s", path);
		if ((slash = strrchr(where, '/')) != NULL)
			*slash = 0;
		if (ui_save_file("Save the picture as", !slash ? folder() : slash == where ? "/" : where, KINDS, slash ? slash + 1 : where, chosen,
				 sizeof chosen, others))
			save_to(chosen);
		else
			fprintf(stderr, "nxpaint: save: the window to name it in was closed with no name (it stood in %s, at \"%s\")\n", ui_files_where,
				ui_files_name);
		draw_all();
		return;
	case C_EXIT:
		if (!may_drop())
			break;
		GrClose();
		exit(0);
	case C_UNDO:
	case C_REDO:
		if (id == C_UNDO ? !done_count : !undone_count) {
			snprintf(notice, sizeof notice, id == C_UNDO ? "nothing to take back" : "nothing to make again");
			break;
		}
		if (id == C_UNDO) {
			turn(&done[done_count - 1]);
			push(undone, &undone_count, done[--done_count]);
		} else {
			turn(&undone[undone_count - 1]);
			push(done, &done_count, undone[--undone_count]);
		}
		modified = 1;
		break;
	case C_COPY:
	case C_CUT:
		if (id == C_CUT && !sel_on) {
			snprintf(notice, sizeof notice, "nothing is selected");
			break;
		}
		piece = fl.on ? NULL : copy_of(x0, y0, x1, y1);
		if (write_ppm(CLIP, fl.on ? fl.buf : piece, fl.on ? fl.w : x1 - x0, fl.on ? fl.h : y1 - y0))
			snprintf(notice, sizeof notice, "copied %d x %d", fl.on ? fl.w : x1 - x0, fl.on ? fl.h : y1 - y0);
		else
			snprintf(notice, sizeof notice, "NOT copied: cannot write " CLIP " (%s)", strerror(errno));
		free(piece);
		if (id == C_COPY)
			break;
		/* fall through */
	case C_DELETE:
		if (!sel_on) {
			snprintf(notice, sizeof notice, "nothing is selected");
			break;
		}
		if (fl.on) {
			/* the piece goes from wherever it floats; a lifted one leaves its hole, to be taken back */
			x0 = fl.hx0, y0 = fl.hy0, x1 = fl.hx1, y1 = fl.hy1, w = fl.hole;
			float_end(1);
			if (!w)
				break;
		}
		box(x0, y0, x1, y1, bg);
		bulk_hold();
		keep(x0, y0, x1, y1);
		present(x0, y0, x1, y1);
		bulk_go(1);
		break;
	case C_PASTE:
		if (!(piece = read_ppm(CLIP, &w, &h))) {
			snprintf(notice, sizeof notice, "nothing was copied");
			break;
		}
		select_none();
		memset(&fl, 0, sizeof fl);
		fl.on = 1, fl.buf = piece, fl.w = w, fl.h = h, fl.x = ox, fl.y = oy;
		d_x1 = d_x0 = 0;
		tool = T_SELECT;
		float_show(fl.x, fl.y);
		draw_tools();
		break;
	case C_ALL:
		select_none();
		s_x0 = s_y0 = 0, s_x1 = pw, s_y1 = ph, sel_on = 1;
		tool = T_SELECT;
		draw_tools();
		present_view();
		break;
	case C_DESELECT:
		select_none();
		break;
	case C_RESIZE:
		snprintf(text, sizeof text, "%dx%d", pw, ph);
		ask(ASK_RESIZE, text);
		return;
	case C_SCALE:
		ask(ASK_SCALE, "200");
		return;
	case C_CROP:
		float_end(0);
		if (!sel_on)
			snprintf(notice, sizeof notice, "select the part to keep first");
		else
			remade(s_x1 - s_x0, s_y1 - s_y0, C_CROP);
		break;
	case C_ROTATE_R:
	case C_ROTATE_L:
		remade(ph, pw, id);
		break;
	case C_FLIP_H:
	case C_FLIP_V:
		for (y = 0; y < (id == C_FLIP_V ? ph / 2 : ph); y++)
			for (x = 0; x < (id == C_FLIP_V ? pw : pw / 2); x++) {
				uint32_t *a = &pix[y * pw + x], *b = id == C_FLIP_V ? &pix[(ph - 1 - y) * pw + x] : &pix[y * pw + pw - 1 - x], word = *a;

				*a = *b, *b = word;
			}
		bulk_hold();
		keep(0, 0, pw, ph);
		present_view();
		bulk_go(1);
		break;
	case C_INVERT:
		float_end(0);
		for (y = y0; y < y1; y++)
			for (x = x0; x < x1; x++)
				pix[y * pw + x] ^= 0xffffff;
		bulk_hold();
		keep(x0, y0, x1, y1);
		present(x0, y0, x1, y1);
		bulk_go(1);
		break;
	case C_CLEAR:
		box(0, 0, pw, ph, bg);
		bulk_hold();
		keep(0, 0, pw, ph);
		present_view();
		bulk_go(1);
		break;
	case C_ZOOM_IN:
	case C_ZOOM_OUT:
	case C_ZOOM_1:
		/* what is at the view's middle stays there */
		x = ox + view_w / zoom / 2, y = oy + view_h / zoom / 2;
		zoom = id == C_ZOOM_1 ? 1 : id == C_ZOOM_IN ? (zoom < 8 ? zoom * 2 : 8) : (zoom > 1 ? zoom / 2 : 1);
		ox = x - view_w / zoom / 2, oy = y - view_h / zoom / 2;
		draw_tools();
		present_view();
		break;
	}
	draw_status();
}

/* ---- the pointer in the picture ---- */

static uint32_t
colour_for(int button)
{
	return tool == T_ERASER ? bg : button == GR_BUTTON_R ? bg : fg;
}

/* The line or shape this drag has drawn so far, to be taken out before the next one. */
static struct { int tool, x, y, size, mode; } drawn;

/* Takes it out. An outline is drawn again with the pixels from before it, which is its own
 * pixels and no hand-over; a filled shape is most of its rectangle, which is put back whole. */
static void
undraw(void)
{
	int now = mode;

	if (d_x1 <= d_x0)
		return;
	if (drawn.tool != T_LINE && drawn.mode) {
		restore(d_x0, d_y0, d_x1, d_y1);
		return;
	}
	undrawing = 1, mode = drawn.mode;
	if (drawn.tool == T_LINE)
		stroke(from_x, from_y, drawn.x, drawn.y, drawn.size, 0);
	else
		shape(drawn.tool == T_OVAL, from_x, from_y, drawn.x, drawn.y, drawn.size, 0, 0);
	undrawing = 0, mode = now;
}

/* The pointer went down in the picture at (x, y), or moved there held, or came up. */
static void
draw_at(int x, int y, int what)
{
	uint32_t colour = colour_for(held_button), other = held_button == GR_BUTTON_R ? fg : bg;
	int size = sizes[size_index], was_x0 = d_x0, was_y0 = d_y0, was_x1 = d_x1, was_y1 = d_y1, n;
	int together = tool != T_SELECT && tool != T_PICK;

	/* a shape is drawn over the picture as it was: the one before it goes first */
	if (tool == T_LINE || tool == T_RECT || tool == T_OVAL)
		undraw();
	/* what is shown and what is kept go to the workers together, and are left with them */
	if (together)
		bulk_hold();
	switch (tool) {
	case T_PENCIL:
	case T_BRUSH:
	case T_ERASER:
	case T_SPRAY:
		d_x1 = d_x0 = 0;
		if (tool == T_SPRAY)
			spray(x, y, size, colour);
		else
			stroke(what ? last_x : x, what ? last_y : y, x, y, tool == T_PENCIL ? 1 : size, colour);
		present(d_x0, d_y0, d_x1, d_y1);
		touch(was_x0 < was_x1 ? was_x0 : d_x0, was_y0 < was_y1 ? was_y0 : d_y0, was_x1 > was_x0 ? was_x1 : d_x1, was_y1 > was_y0 ? was_y1 : d_y1);
		break;
	case T_FILL:
		if (what == 0) {
			flood(x, y, colour);
			present(d_x0, d_y0, d_x1, d_y1);
		}
		break;
	case T_PICK:
		if (x >= 0 && y >= 0 && x < pw && y < ph) {
			if (held_button == GR_BUTTON_R)
				bg = pix[y * pw + x];
			else
				foreground(pix[y * pw + x]);
			draw_colours();
		}
		break;
	case T_LINE:
	case T_RECT:
	case T_OVAL:
		d_x1 = d_x0 = 0;
		if (tool == T_LINE)
			stroke(from_x, from_y, x, y, size, colour);
		else
			shape(tool == T_OVAL, from_x, from_y, x, y, size, colour, mode == 1 ? colour : other);
		drawn.tool = tool, drawn.x = x, drawn.y = y, drawn.size = size, drawn.mode = mode;
		/* what the shape covered before and what it covers now */
		n = d_x0, d_x0 = was_x1 > was_x0 && was_x0 < n ? was_x0 : n;
		n = d_y0, d_y0 = was_x1 > was_x0 && was_y0 < n ? was_y0 : n;
		n = d_x1, d_x1 = was_x1 > was_x0 && was_x1 > n ? was_x1 : n;
		n = d_y1, d_y1 = was_x1 > was_x0 && was_y1 > n ? was_y1 : n;
		present(d_x0, d_y0, d_x1, d_y1);
		break;
	case T_SELECT:
		if (fl.moving) {
			int was_x = fl.x, was_y = fl.y;

			fl.x = x - fl.grab_x, fl.y = y - fl.grab_y;
			if (fl.x != was_x || fl.y != was_y)
				float_show(was_x, was_y);
			break;
		}
		n = sel_on;
		was_x0 = s_x0, was_y0 = s_y0, was_x1 = s_x1, was_y1 = s_y1;
		s_x0 = from_x < x ? from_x : x, s_y0 = from_y < y ? from_y : y;
		s_x1 = (from_x < x ? x : from_x) + 1, s_y1 = (from_y < y ? y : from_y) + 1;
		s_x0 = s_x0 < 0 ? 0 : s_x0, s_y0 = s_y0 < 0 ? 0 : s_y0, s_x1 = s_x1 > pw ? pw : s_x1, s_y1 = s_y1 > ph ? ph : s_y1;
		sel_on = s_x1 > s_x0 && s_y1 > s_y0 && (s_x1 - s_x0 > 1 || s_y1 - s_y0 > 1);	/* (dragged beside the picture: none) */
		if (n)
			present(was_x0 - 1, was_y0 - 1, was_x1 + 1, was_y1 + 1);
		present(s_x0 - 1, s_y0 - 1, s_x1 + 1, s_y1 + 1);
		break;
	}
	last_x = x, last_y = y;
	if (what == 2) {
		/* the pointer is up: the change stays (a floating piece stays afloat) */
		if (tool == T_SELECT)
			fl.moving = 0;
		else {
			keep(d_x0, d_y0, d_x1, d_y1);
			d_x1 = d_x0 = 0;
		}
	}
	if (together)
		bulk_go(1);
}

static void
press(int x, int y, int button)
{
	int inside = sel_on && x >= s_x0 && y >= s_y0 && x < s_x1 && y < s_y1;

	held = 1, held_button = button;
	from_x = last_x = x, from_y = last_y = y;
	if (tool == T_SELECT && inside) {
		lift();
		fl.moving = 1, fl.grab_x = x - fl.x, fl.grab_y = y - fl.y;
		return;
	}
	select_none();
	d_x1 = d_x0 = 0;
	draw_at(x, y, 0);
}

/* ---- the panels' clicks ---- */

static int
field_x(void)
{
	return 60 + 14 * 18 + 12;
}

/* A press or a drag in the field or on the slider. */
static void
colour_at(int x, int y)
{
	int fx = field_x(), sx = fx + FIELD_W + 10, by = bottom_y(), h, s;

	if (picking_field) {
		h = (x - fx) * 255 / (FIELD_W - 1), s = 255 - (y - by - 9) * 255 / (FIELD_H - 1);
		h = h < 0 ? 0 : h > 255 ? 255 : h, s = s < 0 ? 0 : s > 255 ? 255 : s;
		if (sliding_button == GR_BUTTON_R) {
			/* the right button: the background, as the field shows it */
			bg = from_hsv(h, s, value ? value : 255);
			draw_colours();
			return;
		}
		hue = h, sat = s;
		if (!value)
			value = 255;
	} else
		value = ui_slider_value(x, sx, 124, 255);
	fg = from_hsv(hue, sat, value);
	draw_colours();
}

static void
panel_press(int x, int y, int button)
{
	int i, ty = MENU_H + 4 + 5 * 26 + 6, by = bottom_y(), fx = field_x();

	if (y < MENU_H) {
		for (i = 0; i < 4; i++)
			if (x >= menu_x[i] && x < menu_x[i + 1]) {
				int wx, wy;

				ui_window_origin(window, &wx, &wy);
				ui_menu_open(menus[i].items, menus[i].count, wx + menu_x[i], wy + MENU_H, 0, 0);
			}
		return;
	}
	if (y >= by) {
		for (i = 0; i < 28; i++)
			if (ui_inside(x, y, 60 + (i % 14) * 18, by + 10 + (i / 14) * 20, 17, 17)) {
				if (button == GR_BUTTON_R)
					bg = palette[i];
				else
					foreground(palette[i]);
			}
		if (ui_inside(x, y, 6, by + 8, 44, 42) && !ui_inside(x, y, 60, by, 400, BOTTOM_H)) {
			uint32_t was = fg;	/* the two in hand change places */

			foreground(bg);
			bg = was;
		}
		sliding_button = button;
		if (ui_inside(x, y, fx, by + 9, FIELD_W, FIELD_H))
			picking_field = 1, sliding = 1, colour_at(x, y);
		else if (ui_inside(x, y, fx + FIELD_W + 10, by + 6, 124, 18))
			picking_field = 0, sliding = 1, colour_at(x, y);
		else
			draw_colours();
		return;
	}
	if (x >= TOOLS_W)
		return;
	for (i = 0; i < TOOL_COUNT; i++)
		if (ui_inside(x, y, 3 + (i & 1) * 33, MENU_H + 4 + (i / 2) * 26, 32, 24)) {
			if (i != T_SELECT)
				select_none();
			tool = i;
		}
	for (i = 0; i < 5; i++)
		if (ui_inside(x, y, 3 + i * 13, ty + 16, 13, 18))
			size_index = i;
	for (i = 0; i < 3; i++)
		if (ui_inside(x, y, 3, ty + 58 + i * 22, TOOLS_W - 6, 20))
			mode = i;
	if (ui_inside(x, y, 3, ty + 146, 20, 20))
		command(C_ZOOM_OUT);
	else if (ui_inside(x, y, TOOLS_W - 23, ty + 146, 20, 20))
		command(C_ZOOM_IN);
	draw_tools();
	draw_status();
}

/* ---- keys ---- */

static void
ask_key(int ch)
{
	int n = (int)strlen(answer), what = asking, w = 0, h = 0;

	if (ch == MWKEY_ESCAPE) {
		asking = ASK_NONE;
	} else if (ch == MWKEY_ENTER || ch == '\n') {
		asking = ASK_NONE;
		sscanf(answer, "%d%*[xX ,]%d", &w, &h);
		if (what == ASK_RESIZE) {
			remade(w, h, C_RESIZE);
		} else if (what == ASK_SCALE && w > 0) {
			remade(pw * w / 100, ph * w / 100, C_SCALE);
		} else
			snprintf(notice, sizeof notice, "that is not a size");
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			answer[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < (int)sizeof answer - 1) {
		answer[n] = (char)ch, answer[n + 1] = 0;
	}
	draw_status();
}

static void
key(int ch, int modifiers)
{
	static const struct { int ch, id; } controls[] = { { 'n', C_NEW }, { 'o', C_OPEN }, { 's', C_SAVE }, { 'q', C_EXIT }, { 'z', C_UNDO },
		{ 'y', C_REDO }, { 'x', C_CUT }, { 'c', C_COPY }, { 'v', C_PASTE }, { 'a', C_ALL } };
	static const char letters[] = "spbefklroy";	/* a tool each, in their order */
	int i, step = 32 / zoom > 0 ? 32 / zoom : 1;

	if (asking) {
		ask_key(ch);
		return;
	}
	if ((modifiers & MWKMOD_CTRL) && ch < 128 && isalpha(ch))
		ch = tolower(ch) - 'a' + 1;
	for (i = 0; i < (int)(sizeof controls / sizeof controls[0]); i++)
		if (ch == controls[i].ch - 'a' + 1) {
			command(controls[i].id);
			return;
		}
	switch (ch) {
	case MWKEY_DELETE: command(C_DELETE); return;
	case MWKEY_ESCAPE:
		if (fl.on && !fl.hole)
			float_end(1);
		command(C_DESELECT);
		return;
	case '+': case '=': command(C_ZOOM_IN); return;
	case '-': command(C_ZOOM_OUT); return;
	case '1': command(C_ZOOM_1); return;
	case '[': size_index = size_index > 0 ? size_index - 1 : 0; break;
	case ']': size_index = size_index < 4 ? size_index + 1 : 4; break;
	case MWKEY_LEFT: ox -= step; present_view(); return;
	case MWKEY_RIGHT: ox += step; present_view(); return;
	case MWKEY_UP: oy -= step; present_view(); return;
	case MWKEY_DOWN: oy += step; present_view(); return;
	default:
		for (i = 0; i < TOOL_COUNT; i++)
			if (ch == letters[i]) {
				if (i != T_SELECT)
					select_none();
				tool = i;
			}
	}
	draw_tools();
	draw_status();
}

/* ---- events ---- */

static void
handle(GR_EVENT *event)
{
	int x, y, chosen, button;

	/* what the event before left with the workers is waited for by what uses the picture */
	if (event->type == GR_EVENT_TYPE_BUTTON_DOWN || event->type == GR_EVENT_TYPE_BUTTON_UP || event->type == GR_EVENT_TYPE_KEY_DOWN ||
	    (event->type == GR_EVENT_TYPE_MOUSE_POSITION && (held || sliding)))
		bulk_wait();
	ui_origin_note(event);
	switch (ui_menu_event(event, &chosen)) {
	case UI_MENU_CHOSEN:
		command(chosen);
		return;
	case UI_MENU_TOOK:
	case UI_MENU_CLOSED:
		return;
	}
	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		draw_all();
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		x = event->button.x, y = event->button.y;
		button = event->button.changebuttons & GR_BUTTON_R ? GR_BUTTON_R : GR_BUTTON_L;
		if (!ui_wheel(event) && !following) {
			/* where the pointer goes is asked for while a button is held: every move of it
			 * anywhere on the screen was an event to answer, drawing or not */
			following = 1;
			GrSelectEvents(window, EVENTS | GR_EVENT_MASK_MOUSE_POSITION);
		}
		if (ui_wheel(event)) {
			oy += ui_wheel_sum(event) * (48 / zoom > 0 ? 48 / zoom : 1);
			present_view();
		} else if (held) {
			/* (another button while one draws: the stroke goes on as it was) */
		} else if (x >= TOOLS_W && y >= MENU_H && y < MENU_H + view_h) {
			notice[0] = 0;
			press(ox + (x - TOOLS_W) / zoom, oy + (y - MENU_H) / zoom, button);
		} else
			panel_press(x, y, button);
		break;
	case GR_EVENT_TYPE_MOUSE_POSITION:
		if (sliding && event->mouse.buttons)
			colour_at(event->mouse.x, event->mouse.y);
		else if (held && event->mouse.buttons)
			draw_at(ox + (event->mouse.x - TOOLS_W) / zoom, oy + (event->mouse.y - MENU_H) / zoom, 1);
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		if (event->button.changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN))
			break;	/* (a notch of the wheel ending, not the button that draws) */
		if (held) {
			draw_at(ox + (event->button.x - TOOLS_W) / zoom, oy + (event->button.y - MENU_H) / zoom, 2);
			draw_status();
		}
		held = sliding = 0;
		if (following && !event->button.buttons) {
			following = 0;
			GrSelectEvents(window, EVENTS);
		}
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		key(event->keystroke.ch, event->keystroke.modifiers);
		break;
	case GR_EVENT_TYPE_UPDATE:
		if (event->update.wid == window)
			locate(1);
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		command(C_EXIT);
		break;
	}
	if (relaid)
		draw_all();
}

/* ---- a test's hands: NXPAINT_SCRIPT="tool:Rect drag:10,10:60,40 undo sum quit" ---- */

/* A pointer's event at the window's x, y, as the window system sends one. */
static void
pointer(int type, int x, int y, int button)
{
	GR_EVENT event;

	memset(&event, 0, sizeof event);
	event.type = type;
	if (type == GR_EVENT_TYPE_MOUSE_POSITION)
		event.mouse.wid = window, event.mouse.x = x, event.mouse.y = y, event.mouse.buttons = button;
	else
		event.button.wid = window, event.button.x = x, event.button.y = y, event.button.changebuttons = button,
		event.button.buttons = type == GR_EVENT_TYPE_BUTTON_DOWN ? button : 0;
	handle(&event);
}

static long
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* What is in hand, after the script's `word`. */
static void
hand(const char *word)
{
	printf("nxpaint: %s: %s, size %d, %s, %dx, fg %06x, bg %06x\n", word, tool_names[tool], sizes[size_index], mode_names[mode], zoom,
	       (unsigned)(fg & 0xffffff), (unsigned)(bg & 0xffffff));
}

static void
scripted(void)
{
	static const struct { const char *word; int id; } words[] = {
		{ "undo", C_UNDO }, { "redo", C_REDO }, { "copy", C_COPY }, { "cut", C_CUT }, { "paste", C_PASTE }, { "delete", C_DELETE },
		{ "all", C_ALL }, { "none", C_DESELECT }, { "crop", C_CROP }, { "fliph", C_FLIP_H }, { "flipv", C_FLIP_V }, { "rotr", C_ROTATE_R },
		{ "rotl", C_ROTATE_L }, { "invert", C_INVERT }, { "clear", C_CLEAR }, { "zoomin", C_ZOOM_IN }, { "zoomout", C_ZOOM_OUT },
		{ "save", C_SAVE }, { "saveas", C_SAVE_AS }, { "new", C_NEW }, { "quit", C_EXIT },
	};
	char word[320], to[300];
	GR_PIXELVAL seen;
	GR_EVENT event;
	uint32_t is;
	long until, left;
	int n, i, a, b, c, d, right;
	unsigned sum = 2166136261u;

	while (script[0]) {
		bulk_wait();
		n = (int)strcspn(script, " ");
		snprintf(word, sizeof word, "%.*s", n, script);
		memmove(script, script + n + (script[n] == ' '), strlen(script + n + (script[n] == ' ')) + 1);
		notice[0] = 0;
		right = word[0] == 'r';
		if (!strncmp(word, "tool:", 5)) {
			for (i = 0; i < TOOL_COUNT; i++)
				if (!strcmp(word + 5, tool_names[i]))
					key("spbefklroy"[i], 0);
		} else if (sscanf(word, "size:%d", &a) == 1) {
			for (i = 0; i < 5; i++)
				if (sizes[i] == a)
					size_index = i;
		} else if (sscanf(word, "mode:%d", &a) == 1) {
			mode = a < 0 ? 0 : a > 2 ? 2 : a;
		} else if (sscanf(word, "fg:%x", (unsigned *)&a) == 1) {
			foreground((uint32_t)a);
		} else if (sscanf(word, "bg:%x", (unsigned *)&a) == 1) {
			bg = 0xff000000u | (uint32_t)a;
		} else if (sscanf(word, "drag:%d,%d:%d,%d", &a, &b, &c, &d) == 4 || sscanf(word, "rdrag:%d,%d:%d,%d", &a, &b, &c, &d) == 4) {
			press(a, b, right ? GR_BUTTON_R : GR_BUTTON_L);
			for (i = 1; i <= 4; i++)
				draw_at(a + (c - a) * i / 4, b + (d - b) * i / 4, 1);
			draw_at(c, d, 2);
			held = 0;
		} else if (sscanf(word, "press:%d,%d", &a, &b) == 2) {
			press(a, b, GR_BUTTON_L);
			draw_at(a, b, 2);
			held = 0;
		} else if (sscanf(word, "click:%d,%d", &a, &b) == 2 || sscanf(word, "rclick:%d,%d", &a, &b) == 2 ||
			   sscanf(word, "pull:%d,%d:%d,%d", &a, &b, &c, &d) == 4 || sscanf(word, "rpull:%d,%d:%d,%d", &a, &b, &c, &d) == 4) {
			/* by the window's own coordinates, panels and all, through what the pointer's events go through */
			pointer(GR_EVENT_TYPE_BUTTON_DOWN, a, b, right ? GR_BUTTON_R : GR_BUTTON_L);
			if (strchr(word, 'p')) {
				for (i = 1; i <= 4; i++)
					pointer(GR_EVENT_TYPE_MOUSE_POSITION, a + (c - a) * i / 4, b + (d - b) * i / 4, right ? GR_BUTTON_R : GR_BUTTON_L);
				a = c, b = d;
			}
			pointer(GR_EVENT_TYPE_BUTTON_UP, a, b, right ? GR_BUTTON_R : GR_BUTTON_L);
			hand(word);
		} else if (!strcmp(word, "hand")) {
			hand(word);
		} else if (sscanf(word, "swatch:%d", &a) == 1 && a >= 0 && a < 30) {
			/* what the window shows in a swatch (28: the foreground's box, 29: the background's) */
			draw_colours();
			settle();
			is = a < 28 ? palette[a] : a == 28 ? fg : bg;
			seen = 0;
			GrReadArea(window, a < 28 ? 68 + (a % 14) * 18 : a == 28 ? 12 : 44,
				   bottom_y() + (a < 28 ? 18 + (a / 14) * 20 : a == 28 ? 14 : 44), 1, 1, &seen);
			printf("nxpaint: swatch %d shows %06x and is %06x%s\n", a, (unsigned)(seen & 0xffffff), (unsigned)(is & 0xffffff),
			       (seen & 0xffffff) == (is & 0xffffff) ? "" : ": NOT the same");
		} else if (sscanf(word, "events:%d", &a) == 1) {
			/* the pointer and the keys have the window for so many milliseconds (nxkey, from a shell) */
			for (until = now() + a; (left = until - now()) > 0;) {
				GrGetNextEventTimeout(&event, (GR_TIMEOUT)left);
				handle(&event);
			}
		} else if (!strcmp(word, "where")) {
			locate(0);
			ui_window_origin(window, &a, &b);
			printf("nxpaint: the window is at %d,%d on the screen, %d x %d\n", a, b, width, height);
		} else if (sscanf(word, "new:%dx%d", &a, &b) == 2) {
			blank(a, b);
			unnamed();
		} else if (sscanf(word, "resize:%dx%d", &a, &b) == 2) {
			float_end(0), remade(a, b, C_RESIZE);
		} else if (sscanf(word, "scale:%d", &a) == 1) {
			float_end(0), remade(pw * a / 100, ph * a / 100, C_SCALE);
		} else if (!strncmp(word, "open:", 5)) {
			printf("nxpaint: open %s: %s\n", word + 5, open_file(word + 5) ? "opened" : "NOT opened");
		} else if (!strncmp(word, "saveas:", 7)) {
			/* a name with no folder: in the folder the window to save in starts in */
			snprintf(to, sizeof to, "%s%s%.250s", word[7] == '/' ? "" : folder(), word[7] == '/' ? "" : "/", word + 7);
			float_end(0);
			save_to(to);
		} else if (sscanf(word, "pixel:%d,%d", &a, &b) == 2) {
			printf("nxpaint: pixel %d,%d is %06x\n", a, b, (unsigned)(pix[b * pw + a] & 0xffffff));
		} else if (!strcmp(word, "probe")) {
			settle();
			seen = 0;
			GrReadArea(window, TOOLS_W + 5, MENU_H + 5, 1, 1, &seen);
			if (surface)
				printf("nxpaint: on the screen: field %06x, view %06x (the window system's: %06x)\n",
				       (unsigned)(surface[(bottom_y() + 20) * surface_row + field_x() + 30] & 0xffffff),
				       (unsigned)(surface[(MENU_H + 5) * surface_row + TOOLS_W + 5] & 0xffffff), (unsigned)(seen & 0xffffff));
		} else if (!strcmp(word, "status")) {
			draw_status();
		} else if (!strcmp(word, "colours")) {
			draw_colours();
		} else if (!strcmp(word, "sum")) {
			sum = 2166136261u;
			for (i = 0; i < pw * ph; i++)
				sum = (sum ^ (pix[i] & 0xffffff)) * 16777619u;
			printf("nxpaint: %d x %d, sum %08x, %d to take back, %d to make again%s\n", pw, ph, sum, done_count, undone_count,
			       sel_on ? ", a selection" : "");
		} else
			for (i = 0; i < (int)(sizeof words / sizeof words[0]); i++)
				if (!strcmp(word, words[i].word))
					command(words[i].id);
		if (notice[0])
			printf("nxpaint: %s: %s\n", word, notice);
		fflush(stdout);
	}
}

int
main(int argc, char **argv)
{
	GR_SCREEN_INFO screen;
	GR_EVENT event;
	void *words;
	int i, x;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	signal(SIGCHLD, SIG_DFL);	/* (the decoder is waited for: see ui_run in ui.h) */
	if (getenv("NXPAINT_SCRIPT"))
		snprintf(script, sizeof script, "%s", getenv("NXPAINT_SCRIPT"));
	/* (the ways before these, for a test to compare pictures with) */
	stroke_dabs = getenv("NXPAINT_DABS") != NULL;
	flood_plain = getenv("NXPAINT_FLOOD") != NULL;
	screen = *ui_screen();
	width = screen.cols - 60 < 700 ? screen.cols - 60 : 700;
	height = screen.rows - 90 < 480 ? screen.rows - 90 : 480;
	view_w = width - TOOLS_W, view_h = height - MENU_H - BOTTOM_H - STATUS;
	for (i = 0, x = 4; i < 4; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[4] = x;
	gpu = open("/dev/gpu", O_RDWR);
	/* the control words, which the device maps a megabyte in: only the count of buffers is read */
	if (gpu >= 0 && (words = mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000)) != MAP_FAILED)
		buffers = (volatile uint32_t *)words + 0x64 / 4;
	/* the window first: a large picture is seconds to decode */
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE, "Paint", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	if (argc < 2 || !open_file(argv[1])) {
		blank(view_w < 480 ? view_w : 480, view_h < 340 ? view_h : 340);
		unnamed();
		if (argc > 1 && access(argv[1], F_OK) != 0) {
			/* a picture to be: Save makes the file */
			snprintf(path, sizeof path, "%s", argv[1]);
			named = 1;
			titled();
		} else if (argc > 1)
			snprintf(notice, sizeof notice, "%.80s is not a picture this can open", argv[1]);
	}
	foreground(fg);
	draw_all();
	scripted();
	for (;;) {
		GrGetNextEvent(&event);
		handle(&event);
	}
}
