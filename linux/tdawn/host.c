/*
 * Tiberian Dawn's window on the ShaderEmu GPU (docs/tdawn.md): the game's 8-bit screen is a
 * texture in GPU memory, shown as one rectangle looked up in the palette, with the pointer as
 * a second one. Input comes from the Nano-X server.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES/segl.h>
#include "nano-X.h"
#include "host.h"

#define ONE		65536
#define CURSOR_MAX	128		/* the largest pointer picture, a side */
#define CAPTION		22		/* the window manager's caption and frame (nanowm.h) */
#define FRAME		4
#define REG_PALETTE	0x400		/* offsets in the machine's control words (docs/gpu.md) */
#define REG_INPUT	0x20
#define REG_CLOCK	0x34

static GR_WINDOW_ID window;
static int screen_w, screen_h;
static unsigned char *screen, *cursor;	/* screen: two pages, one after the other */
static GLuint page_texture[2], cursor_texture;
static int cursor_w, cursor_h, cursor_hot_x, cursor_hot_y;
static int pointer_x, pointer_y;	/* in the game's pixels */
static int page_rows;			/* a page's texture is this high: a row is a whole number of 1,024ths */

static const volatile uint32_t *
control(int offset)
{
	return (const volatile uint32_t *)((const char *)seglPalette() - REG_PALETTE + offset);
}

/* The machine's clock word once the GPU is ours, going on from the system's clock before. */
unsigned int
host_ms(void)
{
	static unsigned int ahead;
	static int ours;
	struct timespec ts;
	unsigned int system_ms;

	if (ours)
		return *control(REG_CLOCK) + ahead;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	system_ms = (unsigned int)ts.tv_sec * 1000u + (unsigned int)ts.tv_nsec / 1000000u;
	if (!screen)
		return system_ms;
	ahead = system_ms - *control(REG_CLOCK);
	ours = 1;
	return system_ms;
}

/* Ends the machine's frame: the way to wait for anything that changes between frames. */
void
host_wait(void)
{
	__asm__ volatile(".word 0x0100000f");	/* pause */
}

unsigned int
host_cycles(void)
{
	uint32_t v;

	__asm__ volatile("rdcycle %0" : "=r"(v));
	return v;
}

int
host_open(int width, int height)
{
	GR_SCREEN_INFO info;
	int page, bare, scale;

	if (GrOpen() < 0) {
		fprintf(stderr, "tdawn: no Nano-X server (start one: nano-X -p &)\n");
		return -1;
	}
	/*
	 * The GPU stretches the screen over the window: the largest whole scale that fits the
	 * display. Where a frame and caption would not fit as well (640x400 on a 640x480
	 * display) the window has none and sits in the corner.
	 */
	GrGetScreenInfo(&info);
	for (scale = 4; scale > 1 && (width * scale > info.cols || height * scale > info.rows); scale--)
		;
	bare = width * scale + 2 * FRAME > info.cols || height * scale + CAPTION + FRAME > info.rows;
	window = GrNewWindowEx(bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		"Tiberian Dawn", GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width * scale, height * scale, 0);
	GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	GrMapWindow(window);
	GrSetFocus(window);
	if (seglInit(window) < 0) {
		fprintf(stderr, "tdawn: this machine has no GPU\n");
		return -1;
	}
	screen_w = width;
	screen_h = height;
	screen = seglMemory(2 * width * height);
	cursor = seglMemory(CURSOR_MAX * CURSOR_MAX);
	if (!screen || !cursor) {
		fprintf(stderr, "tdawn: no GPU memory left\n");
		return -1;
	}
	memset(screen, 0, 2 * width * height);
	glGenTextures(2, page_texture);
	glGenTextures(1, &cursor_texture);
	for (page_rows = 256; page_rows < height; page_rows *= 2)
		;
	for (page = 0; page < 2; page++) {
		glBindTexture(GL_TEXTURE_2D, page_texture[page]);
		seglTexturePointer(screen + page * width * height, width, page_rows, GL_COLOR_INDEX8_EXT);
	}
	pointer_x = width / 2;
	pointer_y = height / 2;
	return 0;
}

unsigned char *
host_page(int page)
{
	return screen ? screen + page * screen_w * screen_h : NULL;
}

unsigned int *
host_palette(void)
{
	return seglPalette();
}

void
host_cursor(const unsigned char *pixels, int width, int height, int hot_x, int hot_y)
{
	if (!cursor || width <= 0 || height <= 0 || width > CURSOR_MAX || height > CURSOR_MAX)
		return;
	memcpy(cursor, pixels, width * height);
	cursor_w = width;
	cursor_h = height;
	cursor_hot_x = hot_x;
	cursor_hot_y = hot_y;
	glBindTexture(GL_TEXTURE_2D, cursor_texture);
	seglTexturePointer(cursor, width, height, GL_COLOR_INDEX8_EXT);
}

/* A rectangle of the screen's pixels showing the whole of a texture. */
static void
quad(GLuint texture, int x, int y, int width, int height, int keyed)
{
	static const GLfixed uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	/* in units of the whole window: a matrix of 16.16 numbers cannot hold 2/640 exactly */
	GLfixed x0 = (GLfixed)(((long long)x << 16) / screen_w), x1 = (GLfixed)(((long long)(x + width) << 16) / screen_w);
	GLfixed y0 = (GLfixed)(((long long)y << 16) / screen_h), y1 = (GLfixed)(((long long)(y + height) << 16) / screen_h);
	GLfixed xyz[12] = {x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0};

	seglQuad(xyz, uv, texture, 255, 255, keyed);
}

/* TDAWN_RESIZE=WIDTHxHEIGHT gives the window that size after its 60th frame, as a drag would. */
static void
resize_for_test(void)
{
	static int shown;
	const char *size, *x;

	if (++shown != 60 || !(size = getenv("TDAWN_RESIZE")) || !(x = strchr(size, 'x')))
		return;
	GrResizeWindow(window, atoi(size), atoi(x + 1));
}

/* The window's matrices: glOrthox(0, 1, 1, 0, -1, 1), and a place for the map's corner. */
static void
whole_window(int origin_x, int origin_y)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	int width, height;

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	if (origin_x || origin_y)
		glTranslatex(origin_x * ONE / screen_w, origin_y * ONE / screen_h, 0);
	glDisable(GL_DEPTH_TEST);
}

/* ---- the map drawn by the GPU (docs/tdawn.md) ---- */

#define ATLAS_W		1024		/* texels across: a texture coordinate is a 1,024th */
#define ATLAS_ROWS	2048
#define VIEW_ROWS	1024		/* what one texture of it shows; the sprites' start at 512 and 1024 */
#define SHADE_W		128
#define SHADE_H		256

static unsigned char *atlas, *masks;
static unsigned int *shades, *tiles;
static GLuint icon_texture, sprite_texture[2], mask_texture[2], shade_texture;
static int tile_count;
static int scene_built, scene_shown;	/* a scene waits for its swap; the list shown last has one */
static int scene_rect[4], scene_key;	/* where the page lets the scene through, and by which index */
static unsigned int *page_commands[2], *cursor_command, *cursor_vertices;

int
host_scene_open(int cells)
{
	int i;

	if (!screen)
		return -1;
	if (tiles)
		return cells <= tile_count ? 0 : -1;
	atlas = seglMemory(ATLAS_W * ATLAS_ROWS);
	masks = seglMemory(ATLAS_W / 8 * ATLAS_ROWS);
	shades = seglMemory(SHADE_W * SHADE_H * 4);
	tiles = seglMemory(cells * 64);
	if (!atlas || !masks || !shades || !tiles) {
		fprintf(stderr, "tdawn: no GPU memory for the map: the game draws it\n");
		tiles = NULL;
		return -1;
	}
	memset(tiles, 0, cells * 64);
	memset(masks, 0, ATLAS_W / 8 * ATLAS_ROWS);
	tile_count = cells;
	glGenTextures(1, &icon_texture);
	glGenTextures(2, sprite_texture);
	glGenTextures(2, mask_texture);
	glGenTextures(1, &shade_texture);
	glBindTexture(GL_TEXTURE_2D, icon_texture);
	seglTexturePointer(atlas, ATLAS_W, VIEW_ROWS, GL_COLOR_INDEX8_EXT);
	for (i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, sprite_texture[i]);
		seglTexturePointer(atlas + (i + 1) * 512 * ATLAS_W, ATLAS_W, VIEW_ROWS, GL_COLOR_INDEX8_EXT);
		glBindTexture(GL_TEXTURE_2D, mask_texture[i]);
		seglTexturePointer(masks + (i + 1) * 512 * (ATLAS_W / 8), ATLAS_W, VIEW_ROWS, SEGL_BITS);
	}
	glBindTexture(GL_TEXTURE_2D, shade_texture);
	seglTexturePointer(shades, SHADE_W, SHADE_H, GL_RGBA);
	seglKeep(1, cells);
	seglKeptQuads(0, tiles, 0, cells, icon_texture, 255, 0);
	return 0;
}

unsigned char *
host_atlas(void)
{
	return atlas;
}

unsigned char *
host_masks(void)
{
	return masks;
}

unsigned int *
host_shades(void)
{
	return shades;
}

/* A cell of the map: 24 pixels square at a place of the map, from the atlas. */
void
host_tile(int index, int map_x, int map_y, int atlas_x, int atlas_y)
{
	unsigned int *to = tiles + 16 * index;
	unsigned int x0, y0, x1, y1, u0 = atlas_x, u1 = atlas_x + 24, v0 = atlas_y << 16, v1 = (atlas_y + 24) << 16;

	if (!tiles || index < 0 || index >= tile_count)
		return;
	if (atlas_x < 0) {
		memset(to, 0, 64);
		return;
	}
	x0 = map_x * ONE / screen_w, x1 = (map_x + 24) * ONE / screen_w;
	y0 = map_y * ONE / screen_h, y1 = (map_y + 24) * ONE / screen_h;
	to[0] = x0, to[1] = y0, to[2] = 0, to[3] = u0 | v0;
	to[4] = x1, to[5] = y0, to[6] = 0, to[7] = u1 | v0;
	to[8] = x1, to[9] = y1, to[10] = 0, to[11] = u1 | v1;
	to[12] = x0, to[13] = y1, to[14] = 0, to[15] = u0 | v1;
}

void
host_tiles_clear(void)
{
	if (tiles)
		memset(tiles, 0, tile_count * 64);
}

/* A frame's scene begins: the map's pixel (0, 0) is at this pixel of the screen. */
void
host_scene_begin(int origin_x, int origin_y)
{
	if (scene_built)
		seglDiscard();		/* one nobody showed */
	whole_window(origin_x, origin_y);
	seglKeptMatrices();
	glLoadIdentity();
	glClearColorx(0, 0, 0, ONE);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorKeySE(0);
	scene_built = 1;
}

/* A rectangle of the screen from an atlas, over what the scene has so far. */
void
host_sprite(int kind, int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour)
{
	GLfixed box[4] = {x * ONE / screen_w, y * ONE / screen_h, (x + width) * ONE / screen_w, (y + height) * ONE / screen_h};
	int texels[4];
	GLuint name;

	if (kind == HOST_SHADE) {
		texels[0] = atlas_x * (1024 / SHADE_W), texels[1] = atlas_y * (1024 / SHADE_H);
		texels[2] = (atlas_x + width) * (1024 / SHADE_W), texels[3] = (atlas_y + height) * (1024 / SHADE_H);
		name = shade_texture;
	} else {
		/* the texture that shows these rows: the first for the icons' rows, then two a half apart */
		int view = atlas_y + height <= VIEW_ROWS && kind != HOST_MASK ? 0 : atlas_y + height <= VIEW_ROWS + 512 ? 1 : 2;

		atlas_y -= view * 512;
		texels[0] = atlas_x, texels[1] = atlas_y, texels[2] = atlas_x + width, texels[3] = atlas_y + height;
		name = kind == HOST_MASK ? mask_texture[view ? view - 1 : 0] : view ? sprite_texture[view - 1] : icon_texture;
	}
	seglSprite(box, texels, name, colour, kind == HOST_KEYED);
}

/* A rectangle of one colour: the middle of 24 x 24 set bits in the masks, as large as wanted. */
void
host_block(int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour)
{
	GLfixed box[4] = {x * ONE / screen_w, y * ONE / screen_h, (x + width) * ONE / screen_w, (y + height) * ONE / screen_h};
	int view = atlas_y + 24 <= VIEW_ROWS + 512 ? 1 : 2;
	int texels[4] = {atlas_x + 8, atlas_y - view * 512 + 8, atlas_x + 16, atlas_y - view * 512 + 16};

	seglSprite(box, texels, mask_texture[view - 1], colour, 0);
}

/* The scene is whole. The page goes over it, with holes of this index inside this rectangle. */
void
host_scene_end(int x, int y, int width, int height, int key)
{
	scene_rect[0] = x, scene_rect[1] = y, scene_rect[2] = width, scene_rect[3] = height;
	scene_key = key;
}

void
host_scene_drop(void)
{
	if (scene_built)
		seglDiscard();
	scene_built = scene_shown = 0;
}

/* A part of a page, in its own place on the screen. */
static void
page_part(int page, int x, int y, int width, int height, int keyed)
{
	GLfixed box[4] = {x * ONE / screen_w, y * ONE / screen_h, (x + width) * ONE / screen_w, (y + height) * ONE / screen_h};
	int texels[4] = {x * 1024 / screen_w, y * 1024 / page_rows, (x + width) * 1024 / screen_w, (y + height) * 1024 / page_rows};

	if (width > 0 && height > 0)
		seglSprite(box, texels, page_texture[page & 1], 0x00ffffff, keyed);
}

/* The pointer's rectangle as four corners, or nothing at all. */
static void
cursor_corners(unsigned int *to, int shown)
{
	int x = pointer_x - cursor_hot_x, y = pointer_y - cursor_hot_y;
	unsigned int x0 = x * ONE / screen_w, y0 = y * ONE / screen_h;
	unsigned int x1 = (x + cursor_w) * ONE / screen_w, y1 = (y + cursor_h) * ONE / screen_h;

	if (!shown || !cursor_w)
		x0 = x1 = y0 = y1 = 0;
	to[0] = x0, to[1] = y0, to[4] = x1, to[5] = y0, to[8] = x1, to[9] = y1, to[12] = x0, to[13] = y1;
}

/*
 * A new scene is shown with the page over it: its parts around the map as they are, the part
 * over the map with holes. The commands and the pointer's corners are remembered, so that the
 * same list can be drawn again when only the page or the pointer has changed.
 */
static void
present_scene(int page, int with_cursor)
{
	static const int all[4] = {0, 0, 1024, 1024};
	GLfixed box[4] = {0, 0, 0, 0};
	int x = scene_rect[0], y = scene_rect[1], w = scene_rect[2], h = scene_rect[3];

	glColorKeySE(scene_key);
	page_part(page, 0, 0, screen_w, y, 0);
	page_part(page, 0, y + h, screen_w, screen_h - y - h, 0);
	page_part(page, 0, y, x, h, 0);
	page_part(page, x + w, y, screen_w - x - w, h, 0);
	page_commands[0] = y > 0 || x > 0 || x + w < screen_w || y + h < screen_h ? seglLastCommand() : NULL;
	page_part(page, x, y, w, h, 1);
	page_commands[1] = seglLastCommand();
	glColorKeySE(0);
	seglSprite(box, all, cursor_texture, 0x00ffffff, 1);
	cursor_command = seglLastCommand();
	cursor_vertices = seglLastVertices();
	cursor_corners(cursor_vertices, with_cursor);
	seglSwap();
	scene_built = 0;
	scene_shown = 1;
}

void
host_present(int page, int with_cursor)
{
	if (!screen)
		return;
	if (scene_built) {
		present_scene(page, with_cursor);
	} else if (scene_shown) {
		/* the same scene: the page it shows may be the other one now, the pointer elsewhere */
		page_commands[1][7] = seglAddress(screen + (page & 1) * screen_w * screen_h);
		if (page_commands[0])
			page_commands[0][7] = page_commands[1][7];
		cursor_command[8] = cursor_w;
		cursor_command[9] = cursor_h;
		cursor_corners(cursor_vertices, with_cursor);
		seglSwapAgain();
	} else {
		whole_window(0, 0);
		glColorKeySE(0);
		page_part(page, 0, 0, screen_w, screen_h, 0);
		if (with_cursor && cursor_w)
			quad(cursor_texture, pointer_x - cursor_hot_x, pointer_y - cursor_hot_y, cursor_w, cursor_h, 1);
		seglSwap();
	}
	resize_for_test();
}

void
host_pointer(int *x, int *y)
{
	*x = pointer_x;
	*y = pointer_y;
}

/* A Nano-X key as the Windows virtual key the game's tables hold, or 0. */
static int
virtual_key(int ch)
{
	static const char shifted[] = ")!@#$%^&*(";
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, 0x25}, {MWKEY_UP, 0x26}, {MWKEY_RIGHT, 0x27}, {MWKEY_DOWN, 0x28},
		{MWKEY_INSERT, 0x2d}, {MWKEY_DELETE, 0x2e}, {MWKEY_HOME, 0x24}, {MWKEY_END, 0x23},
		{MWKEY_PAGEUP, 0x21}, {MWKEY_PAGEDOWN, 0x22}, {MWKEY_KP_ENTER, 0x0d},
		{MWKEY_LSHIFT, 0x10}, {MWKEY_RSHIFT, 0x10}, {MWKEY_LCTRL, 0x11}, {MWKEY_RCTRL, 0x11},
		{MWKEY_LALT, 0x12}, {MWKEY_RALT, 0x12}, {MWKEY_F11, 0x7a}, {MWKEY_F12, 0x7b},
		{';', 0xba}, {'=', 0xbb}, {',', 0xbc}, {'-', 0xbd}, {'.', 0xbe}, {'/', 0xbf}, {'`', 0xc0},
		{'[', 0xdb}, {'\\', 0xdc}, {']', 0xdd}, {'\'', 0xde}, {'\n', 0x0d},
	};
	const char *at;
	unsigned i;

	if (ch >= 'a' && ch <= 'z')
		return ch - 'a' + 'A';
	if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == MWKEY_ENTER || ch == MWKEY_ESCAPE ||
	    ch == MWKEY_BACKSPACE || ch == MWKEY_TAB)
		return ch;
	if (ch >= MWKEY_F1 && ch <= MWKEY_F10)
		return 0x70 + ch - MWKEY_F1;
	if (ch > 0 && ch < 128 && (at = strchr(shifted, ch)) != NULL)
		return '0' + (int)(at - shifted);
	for (i = 0; i < sizeof other / sizeof other[0]; i++)
		if (other[i].ch == ch)
			return other[i].key;
	return 0;
}

/*
 * Asking the server costs two system calls and two task switches. The machine's own input
 * words (pointer, buttons, key events so far; docs/input.md) say when there can be anything:
 * ask when they have moved, for a few calls after, and a few times a second.
 */
int
host_event(struct host_event *event)
{
	static uint32_t seen[4], asked_ms;
	static int ask = 8, log = -1;
	const volatile uint32_t *input;
	GR_EVENT e;
	int i;

	if (!screen)
		return 0;
	if (log < 0)
		log = getenv("TDAWN_INPUT_LOG") != NULL;	/* say what arrives, for tests */
	input = control(REG_INPUT);
	for (i = 0; i < 4; i++)
		if (seen[i] != input[i]) {
			seen[i] = input[i];
			ask = 8;
		}
	if (!ask && host_ms() - asked_ms >= 250)
		ask = 1;
	while (ask) {
		GrCheckNextEvent(&e);
		switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = host_ms();
			return 0;
		case GR_EVENT_TYPE_MOUSE_POSITION:
			seglPicturePoint(e.mouse.x, e.mouse.y, screen_w, screen_h, &pointer_x, &pointer_y);
			break;
		case GR_EVENT_TYPE_UPDATE:
			/* the picture is stretched over the window, whatever size it is given */
			if (e.update.utype == GR_UPDATE_SIZE) {
				seglWindowChanged();
				if (log)
					fprintf(stderr, "tdinput: the window is %dx%d now\n", e.update.width, e.update.height);
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			seglPicturePoint(e.button.x, e.button.y, screen_w, screen_h, &pointer_x, &pointer_y);
			if (!(e.button.changebuttons & (GR_BUTTON_L | GR_BUTTON_R)))
				break;
			event->type = HOST_BUTTON;
			event->key = e.button.changebuttons & GR_BUTTON_L ? 0x01 : 0x02;
			event->down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			event->x = pointer_x;
			event->y = pointer_y;
			if (log) {
				int width, height;

				seglSize(&width, &height);
				fprintf(stderr, "tdinput: button %d %s at %d,%d (%d,%d in the %dx%d window)\n", event->key,
					event->down ? "down" : "up", pointer_x, pointer_y, e.button.x, e.button.y, width, height);
			}
			return 1;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			event->key = virtual_key(e.keystroke.ch);
			if (!event->key)
				break;
			event->type = HOST_KEY;
			event->down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			if (log)
				fprintf(stderr, "tdinput: key %d %s\n", event->key, event->down ? "down" : "up");
			return 1;
		case GR_EVENT_TYPE_CLOSE_REQ:
			event->type = HOST_QUIT;
			return 1;
		}
	}
	return 0;
}
