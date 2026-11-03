/*
 * Micropolis, the city simulator of 1989 (docs/games/micropolis.md): MicropolisCore's engine
 * (engine.cpp, mp.h) in a Nano-X window. The map, its sprites and the tools are drawn by the
 * GPU in a child window: the map is a layer of tiles whose cells are the engine's own tile
 * numbers, kept in a ring of 64 x 64 cells that the view scrolls through.
 *
 * Micropolis is a registered trademark of Micropolis Corporation (Micropolis GmbH) and is
 * licensed here as a courtesy of the owner under the Micropolis Public Name License.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <GLES/gl.h>
#include <GLES/segl.h>
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_pad.h"
#include "ui_files.h"
#include "ui_ask.h"
#include "shaderemu_sound.h"
#include "mp.h"

#define DATA "/usr/share/micropolis"
#define SAVED "/persistence/micropolis"	/* kept by the host when asked (docs/system/persist.md) */
#define BAR 22			/* the menus along the top */
#define STATUS 40		/* two lines at the bottom: the city's numbers and the last message */
#define PANEL 80		/* the tools, left of the map */
#define TILE 16
#define RING 64			/* cells of the map layer each way: 1,024 pixels, which wraps */
#define ONE 65536
#define MAP_MOST 960		/* the map area at most, each way: the ring (RING tiles, 1,024 pixels) wraps beyond */
#define EVENTS (GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)

static const char *const tool_names[MP_TOOLS] = {
	"Residential zone", "Commercial zone", "Industrial zone", "Fire station", "Police station", "Query", "Power line",
	"Bulldozer", "Rail", "Road", "Stadium", "Park", "Seaport", "Coal power plant", "Nuclear power plant", "Airport",
};
/* sprites: frames of each type (train, helicopter, aeroplane, ship, monster, tornado, explosion, bus), and their size */
static const int frames[8] = {5, 8, 11, 8, 16, 3, 6, 4}, frame_size[8] = {32, 32, 48, 48, 48, 48, 48, 32};
static const char *const scenario_names[8] = {"Dullsville, 1900", "San Francisco, 1906", "Hamburg, 1944", "Bern, 1965",
	"Tokyo, 1957", "Detroit, 1972", "Boston, 2010", "Rio de Janeiro, 2047"};

static GR_WINDOW_ID window, view;
static int screen_w, screen_h, wide, high, view_w, view_h, map_w, map_h;
static struct ui_pad pad;
static int scroll_x, scroll_y;		/* the map's pixel at the map area's top left */
static int tool = MP_ROAD;
static int hover_x = -1, hover_y = -1;	/* the tile under the pointer, or -1 */
static int pressed, dragging, drag_x, drag_y, drag_sx, drag_sy, last_tx, last_ty;
static int dirty = 1, report, ticks_done, frames_done;
static unsigned next_tick;
static GLuint t_map, t_sprites, t_tools, t_white;
static unsigned int *map_layer, *sprite_layer;
static unsigned short *ring;
static char city_path[512], message[640];
static struct mp_status shown;		/* what the status lines show */
static char *strings[3][64];		/* stri.301 (messages), stri.219 (what a tile is), stri.202 (how much) */
static unsigned overview_colour[960];	/* each tile on the overview map */

static unsigned
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

/* ---- the files ---- */

/* A file of DATA into GPU memory: its bytes, or NULL. */
static void *
load_gpu(const char *name, unsigned *size)
{
	char path[256];
	struct stat st;
	void *at;
	int fd;

	snprintf(path, sizeof path, DATA "/%s", name);
	if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st) < 0 || !(at = seglMemory((unsigned)st.st_size)) ||
	    read(fd, at, (size_t)st.st_size) != st.st_size) {
		fprintf(stderr, "nxmicropolis: %s: %s\n", path, strerror(errno));
		return NULL;
	}
	close(fd);
	*size = (unsigned)st.st_size;
	return at;
}

/* A text file's lines, the first being number 1. */
static void
load_strings(int which, const char *name)
{
	static char text[3][4096];
	char path[256];
	int fd, n, line = 1;
	char *p;

	snprintf(path, sizeof path, DATA "/data/%s", name);
	if ((fd = open(path, O_RDONLY)) < 0)
		return;
	n = (int)read(fd, text[which], sizeof text[which] - 1);
	close(fd);
	text[which][n < 0 ? 0 : n] = 0;
	for (p = text[which]; *p && line < 64; line++) {
		strings[which][line] = p;
		p += strcspn(p, "\r\n");
		if (*p == '\r')
			*p++ = 0;
		if (*p == '\n')
			*p++ = 0;
	}
}

static const char *
string(int which, int line)
{
	return line > 0 && line < 64 && strings[which][line] ? strings[which][line] : "";
}

static int
load_pictures(void)
{
	unsigned size, *tiles, *sprites, *tools, *white;
	int i;

	if (!(tiles = load_gpu("tiles.bin", &size)) || size != 64 + 960 * 256 || !(sprites = load_gpu("sprites.bin", &size)) ||
	    size < 1024 + 2048 + 16 || !(tools = load_gpu("tools.bin", &size)) || size != MP_TOOLS * 2 * 32 * 32 * 4)
		return -1;
	map_layer = seglMemory(16);
	sprite_layer = seglMemory(16);
	ring = seglMemory(RING * RING * 2);
	white = seglMemory(16 * 16 * 4);
	if (!map_layer || !sprite_layer || !ring || !white)
		return -1;
	memset(ring, 0, RING * RING * 2);
	for (i = 0; i < 256; i++)
		white[i] = 0xFFFFFF;
	/* a layer is four words: its cells, its tiles, its palette, and tile width | height << 8 | cells across << 16 */
	map_layer[0] = seglAddress(ring);
	map_layer[1] = seglAddress(tiles + 16);
	map_layer[2] = seglAddress(tiles);
	map_layer[3] = TILE | TILE << 8 | RING << 16;
	sprite_layer[0] = seglAddress(sprites + 256);
	sprite_layer[1] = seglAddress(sprites + 256 + 512 + 4);
	sprite_layer[2] = seglAddress(sprites);
	sprite_layer[3] = TILE | TILE << 8 | 64 << 16;
	glGenTextures(1, &t_map);
	glBindTexture(GL_TEXTURE_2D, t_map);
	seglTexturePointer(map_layer, RING * TILE, RING * TILE, SEGL_TILES);
	glGenTextures(1, &t_sprites);
	glBindTexture(GL_TEXTURE_2D, t_sprites);
	seglTexturePointer(sprite_layer, 1024, 256, SEGL_TILES);
	glGenTextures(1, &t_tools);
	glBindTexture(GL_TEXTURE_2D, t_tools);
	seglTexturePointer(tools, 32, MP_TOOLS * 2 * 32, GL_RGBA);
	glGenTextures(1, &t_white);
	glBindTexture(GL_TEXTURE_2D, t_white);
	seglTexturePointer(white, 16, 16, GL_RGBA);
	return 0;
}

/* ---- the sounds: samples played by the card where they lie in the ROM (docs/machine/sound.md) ---- */

#define VOICES 4
static struct { char name[24]; unsigned address, length; } sounds[24];
static int sound_count, sound_rate, voice[VOICES], voice_sound[VOICES], next_voice, sounds_played, sounds_asked;

/* Each tile's colour on the overview map: its picture's mean. */
static void
load_overview(void)
{
	unsigned rgb[960];
	int fd, i;

	if ((fd = open(DATA "/overview.bin", O_RDONLY)) < 0)
		return;
	if (read(fd, rgb, sizeof rgb) == (ssize_t)sizeof rgb)
		for (i = 0; i < 960; i++)
			overview_colour[i] = MWRGB(rgb[i] >> 16 & 255, rgb[i] >> 8 & 255, rgb[i] & 255);
	close(fd);
}

static void
load_sounds(void)
{
	unsigned char head[16 + 32 * 24];
	unsigned at;
	int fd, n, i;

	if (snd_open() < 0 || (fd = open(DATA "/sounds.bin", O_RDONLY)) < 0)
		return;
	n = (int)read(fd, head, sizeof head);
	at = snd_rom(fd, "MPSOUNDS", 8);
	close(fd);
	if (n < 16 || !at || memcmp(head, "MPSOUNDS", 8))
		return;
	memcpy(&sound_count, head + 8, 4);
	memcpy(&sound_rate, head + 12, 4);
	sound_count = sound_count > 24 || 16 + 32 * sound_count > n ? 0 : sound_count;
	for (i = 0; i < sound_count; i++) {
		memcpy(sounds[i].name, head + 16 + 32 * i, 24);
		sounds[i].name[23] = 0;
		memcpy(&sounds[i].address, head + 16 + 32 * i + 24, 4);
		memcpy(&sounds[i].length, head + 16 + 32 * i + 28, 4);
		sounds[i].address += at;
	}
	for (i = 0; i < VOICES; i++)
		if ((voice[i] = snd_claim()) < 0)
			sound_count = 0;
}

/* A sound by its name; a city's (x, y in the map's pixels) is heard only near the view, from its side. */
static void
play(const char *name, int x, int y)
{
	int s, v, left = 200, right = 200;

	for (s = 0; s < sound_count && strcmp(sounds[s].name, name); s++)
		continue;
	if (s == sound_count)
		return;
	if (x >= 0) {
		int across = x - scroll_x - map_w / 2, down = y - scroll_y - map_h / 2;

		/* far off, a disaster is heard quietly and the traffic not at all */
		if (across < -map_w || across > map_w || down < -map_h || down > map_h) {
			if (strncmp(name, "Explosion", 9) && strcmp(name, "Monster"))
				return;
			left = right = 90;
		} else {
			across = across * 128 / map_w;
			left = 200 - (across > 0 ? across : 0), right = 200 + (across < 0 ? across : 0);
		}
	}
	/* a sound still playing is not started again; else a voice that has ended, else the next */
	for (v = 0; v < VOICES; v++)
		if (voice_sound[v] == s + 1 && snd_playing(voice[v]))
			return;
	for (v = 0; v < VOICES && snd_playing(voice[v]); v++)
		continue;
	if (v == VOICES)
		v = next_voice, next_voice = (next_voice + 1) % VOICES;
	voice_sound[v] = s + 1;
	sounds_played++;
	snd_play(voice[v], SND_PCM8 | SND_SMOOTH, sounds[s].address, sounds[s].length, 0, snd_step((unsigned)sound_rate),
		 SND_VOLUME(left, right));
}

/* ---- the map ---- */

static void
clamp_scroll(void)
{
	int most_x = MP_W * TILE - map_w, most_y = MP_H * TILE - map_h;

	scroll_x = scroll_x > most_x ? most_x : scroll_x;
	scroll_y = scroll_y > most_y ? most_y : scroll_y;
	scroll_x = scroll_x < 0 ? 0 : scroll_x;
	scroll_y = scroll_y < 0 ? 0 : scroll_y;
}

/* The window's parts for its size (docs/games/micropolis.md, "The windows"): the map area grows with the window up to
 * MAP_MOST each way, the menus above it and the two lines under it as wide as the window. */
static void
lay_out(int w, int h)
{
	wide = w < 400 ? 400 : w, high = h < 300 ? 300 : h;
	view_w = wide, view_h = high - BAR - STATUS;
	view_w = view_w > PANEL + MAP_MOST ? PANEL + MAP_MOST : view_w;
	view_h = view_h > MAP_MOST ? MAP_MOST : view_h;
	map_w = view_w - PANEL, map_h = view_h;
}

/* The ring's cells under the view, from the engine's map; with `animate`, a step of its
 * animated tiles there first (off the view they wait: nobody sees them). */
static void
fill_ring(int animate)
{
	seglSettle();
	mp_view(scroll_x / TILE, scroll_y / TILE, (scroll_x + map_w - 1) / TILE, (scroll_y + map_h - 1) / TILE, ring, RING, animate);
}

/* A rectangle of the view in its pixels, as the projection has it (0 to 1 across the view). */
static void
box_of(int x0, int y0, int x1, int y1, GLfixed *box)
{
	box[0] = x0 * ONE / view_w;
	box[1] = y0 * ONE / view_h;
	box[2] = x1 * ONE / view_w;
	box[3] = y1 * ONE / view_h;
}

/* A plain rectangle in a colour (0xTTRRGGBB). */
static void
rect(int x0, int y0, int x1, int y1, unsigned colour)
{
	GLfixed box[4];
	static const int texels[4] = {0, 0, 1024, 1024};

	if (x1 <= x0 || y1 <= y0)
		return;
	box_of(x0, y0, x1, y1, box);
	seglSprite(box, texels, t_white, colour, 0);
}

static void
outline(int x0, int y0, int x1, int y1, unsigned colour)
{
	rect(x0, y0, x1, y0 + 2, colour);
	rect(x0, y1 - 2, x1, y1, colour);
	rect(x0, y0, x0 + 2, y1, colour);
	rect(x1 - 2, y0, x1, y1, colour);
}

/* The tiles a tool would change round the tile it is used on (the engine's footprints). */
static void
footprint(int t, int tx, int ty, int *x0, int *y0, int *x1, int *y1)
{
	int size = mp_tool_size(t), back = size >= 3 ? 1 : 0;

	*x0 = tx - back, *y0 = ty - back;
	*x1 = *x0 + size, *y1 = *y0 + size;
}

static void
draw_sprites(void)
{
	struct mp_sprite list[64];
	int n = mp_sprites(list, 64), i, k, first[8];

	for (k = 0, i = 0; k < 8; k++)
		first[k] = i, i += frames[k];
	for (i = 0; i < n; i++) {
		int type = list[i].type - 1, frame = list[i].frame - 1, slot, size, x, y;
		int x0, y0, x1, y1, texels[4];
		GLfixed box[4];

		if (frame < 0 || frame >= frames[type])
			continue;
		slot = first[type] + frame, size = frame_size[type];
		x = PANEL + list[i].x - scroll_x, y = list[i].y - scroll_y;
		x0 = x < PANEL ? PANEL : x, y0 = y < 0 ? 0 : y;
		x1 = x + size > view_w ? view_w : x + size, y1 = y + size > view_h ? view_h : y + size;
		if (x1 <= x0 || y1 <= y0)
			continue;
		/* (the sprite layer is 1,024 x 256 pixels: a pixel is 1 across and 4 down in its 1,024ths) */
		texels[0] = slot % 21 * 48 + x0 - x, texels[2] = texels[0] + x1 - x0;
		texels[1] = (slot / 21 * 48 + y0 - y) * 4, texels[3] = texels[1] + (y1 - y0) * 4;
		box_of(x0, y0, x1, y1, box);
		seglSprite(box, texels, t_sprites, 0xFFFFFF, 1);
	}
}

/* The tools, two to a row, and the demand for zones under them. */
static int
tool_at(int x, int y)
{
	int column = (x - 6) / 36, row = (y - 6) / 36;

	if (x < 6 || y < 6 || column > 1 || row >= MP_TOOLS / 2 || (x - 6) % 36 >= 32 || (y - 6) % 36 >= 32)
		return -1;
	return row * 2 + column;
}

static void
draw_panel(void)
{
	static const unsigned colours[3] = {0x00C000, 0x4060FF, 0xE0C000};
	int demand[3] = {shown.residential, shown.commercial, shown.industrial}, t, k, middle = 6 + MP_TOOLS / 2 * 36 + 60;

	rect(0, 0, PANEL, view_h, 0xC0C0C0);
	for (t = 0; t < MP_TOOLS; t++) {
		int x = 6 + t % 2 * 36, y = 6 + t / 2 * 36, lit = t == tool, texels[4] = {0, (2 * t + lit) * 32, 1024, (2 * t + lit + 1) * 32};
		GLfixed box[4];

		box_of(x, y, x + 32, y + 32, box);
		seglSprite(box, texels, t_tools, 0xFFFFFF, 0);
		if (lit)
			outline(x - 2, y - 2, x + 34, y + 34, 0x000080);
	}
	/* residential, commercial and industrial demand: up from the line is wanted */
	rect(8, middle, PANEL - 8, middle + 1, 0x404040);
	for (k = 0; k < 3; k++) {
		int h = demand[k] * 50 / 2000, x = 14 + k * 20;

		h = h > 50 ? 50 : h < -50 ? -50 : h;
		if (h > 0)
			rect(x, middle - h, x + 12, middle, colours[k]);
		else if (h < 0)
			rect(x, middle + 1, x + 12, middle + 1 - h, colours[k]);
	}
}

static void
draw_view(void)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	GLfixed xyz[12], uv[8];
	int width, height;

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	/* the map: the ring repeats, so the view's texture coordinates are the scroll itself */
	xyz[0] = xyz[9] = PANEL * ONE / view_w, xyz[3] = xyz[6] = ONE;
	xyz[1] = xyz[4] = 0, xyz[7] = xyz[10] = ONE;
	xyz[2] = xyz[5] = xyz[8] = xyz[11] = 0;
	uv[0] = uv[6] = scroll_x * (ONE / 1024), uv[2] = uv[4] = (scroll_x + map_w) * (ONE / 1024);
	uv[1] = uv[3] = scroll_y * (ONE / 1024), uv[5] = uv[7] = (scroll_y + map_h) * (ONE / 1024);
	seglQuad(xyz, uv, t_map, 255, 255, 0);
	draw_sprites();
	if (hover_x >= 0) {
		int x0, y0, x1, y1;

		footprint(tool, hover_x, hover_y, &x0, &y0, &x1, &y1);
		outline(PANEL + x0 * TILE - scroll_x, y0 * TILE - scroll_y, PANEL + x1 * TILE - scroll_x, y1 * TILE - scroll_y,
			shown.funds >= mp_tool_cost(tool) ? 0xFFFF00 : 0xFF0000);
	}
	draw_panel();
	seglSwapEarly();
	dirty = 0;
	frames_done++;
}

/* ---- the window's own parts: the menus' bar and the status lines ---- */

static const struct { const char *label; int x, w; } bar_menus[] = {{"City", 4, 40}, {"Speed", 44, 50}, {"Options", 94, 64}, {"Disasters", 158, 76}, {"Windows", 234, 68}, {"Help", 302, 44}};
#define MENUS 6

static void
draw_bar(void)
{
	int i;

	ui_fill(window, 0, 0, wide, BAR, UI_FACE);
	for (i = 0; i < MENUS; i++)
		ui_text(window, bar_menus[i].x + 6, 4, bar_menus[i].label, -1, BLACK, 0);
}

static void
draw_status(void)
{
	static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	static const char *const speeds[4] = {"paused", "slow", "medium", "fast"};
	char line[200];

	snprintf(line, sizeof line, "%s   %s %d   Funds $%d   Population %d   %s   Tool: %s ($%d)", shown.name, months[shown.month % 12],
		 shown.year, shown.funds, shown.population, speeds[shown.paused ? 0 : shown.speed & 3], tool_names[tool], mp_tool_cost(tool));
	ui_fill(window, 0, high - STATUS, wide, STATUS, UI_FACE);
	ui_text_fit(window, 6, high - STATUS + 4, wide - 12, line, BLACK);
	ui_text_fit(window, 6, high - STATUS + 21, wide - 12, message, MWRGB(128, 0, 0));
	GrFlush();
}

static void
say(const char *text)
{
	snprintf(message, sizeof message, "%s", text);
	draw_status();
}

/* The city's numbers, shown again when one of them changed. */
static void
status_now(void)
{
	struct mp_status s;

	mp_status(&s);
	if (memcmp(&s, &shown, sizeof s)) {
		int panel = s.residential != shown.residential || s.commercial != shown.commercial || s.industrial != shown.industrial;

		shown = s;
		draw_status();
		dirty |= panel;
	}
}

/* ---- the other windows: the overview map, the budget, the evaluation and the graphs ---- */

enum { W_MAP, W_BUDGET, W_EVALUATION, W_GRAPHS, W_COUNT };
#define W_EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ)
static const char *const window_titles[W_COUNT] = {"Micropolis map", "Budget", "Evaluation", "Graphs"};
static const int window_sizes[W_COUNT][2] = {{372, 208}, {340, 236}, {300, 240}, {356, 206}};
static GR_WINDOW_ID extra[W_COUNT];
static int held_in = -1;			/* the window a button is held in, for its drags */
static int budget_paused;			/* the year's budget stopped the city until it is done */

static void draw_extra(int k);

/* Over the main window again: the window manager raises that at every click in it. */
static void
raise_extras(void)
{
	GR_WINDOW_INFO info;
	int k;

	for (k = 0; k < W_COUNT; k++)
		if (extra[k]) {
			GrGetWindowInfo(extra[k], &info);	/* its frame, which is what is raised */
			GrRaiseWindow(info.parent && info.parent != GR_ROOT_WINDOW_ID ? info.parent : extra[k]);
		}
}

static void
open_extra(int k)
{
	int x, y;

	if (extra[k]) {
		GR_WINDOW_INFO info;

		/* (its frame, if it was minimised: nothing else brings it back from this menu) */
		GrGetWindowInfo(extra[k], &info);
		if (info.parent && info.parent != GR_ROOT_WINDOW_ID)
			GrMapWindow(info.parent);
		raise_extras();
		draw_extra(k);
		return;
	}
	/* one to a corner of the map (the frame is 6 wider and 29 higher), the menus and the tools left free */
	x = k == W_MAP || k == W_BUDGET ? screen_w - window_sizes[k][0] - 10 : PANEL + 8;
	y = k == W_MAP || k == W_EVALUATION ? screen_h - window_sizes[k][1] - 29 - 30 : BAR + 30;
	extra[k] = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE, window_titles[k], GR_ROOT_WINDOW_ID, x, y, window_sizes[k][0],
				 window_sizes[k][1], UI_FACE);
	GrSelectEvents(extra[k], W_EVENTS);
	GrMapWindow(extra[k]);
}

static void
close_extra(int k)
{
	if (!extra[k])
		return;
	GrDestroyWindow(extra[k]);
	extra[k] = 0;
	held_in = held_in == k ? -1 : held_in;
	if (k == W_BUDGET && budget_paused) {
		budget_paused = 0;
		mp_pause(0);
	}
}

static int
extra_of(GR_WINDOW_ID w)
{
	int k;

	for (k = 0; k < W_COUNT; k++)
		if (extra[k] && extra[k] == w)
			return k;
	return -1;
}

static const char *const show_names[MP_SHOWS] = {"All", "Residential", "Commercial", "Industrial", "Transport", "Power",
	"Population", "Growth", "Traffic", "Pollution", "Crime", "Land value", "Fire cover", "Police cover"};
static int show = MP_SHOW_ALL;

static void
draw_overview(void)
{
	static const unsigned levels[4] = {MWRGB(192, 192, 192), MWRGB(255, 255, 0), MWRGB(255, 128, 0), MWRGB(255, 0, 0)};
	static const unsigned growth[4] = {MWRGB(255, 0, 0), MWRGB(255, 128, 0), MWRGB(128, 255, 128), MWRGB(0, 192, 0)};
	static const unsigned power[3] = {MWRGB(255, 48, 48), MWRGB(64, 160, 255), MWRGB(176, 176, 176)};
	static unsigned short tiles[MP_W * MP_H];
	static unsigned char level[MP_W * MP_H];
	static MWCOLORVAL pixels[MP_W * 2 * MP_H * 2];
	GR_WINDOW_ID w = extra[W_MAP];
	int x, y, i;

	mp_overview(show, tiles, level);
	for (y = 0; y < MP_H; y++)
		for (x = 0; x < MP_W; x++) {
			int l = level[y * MP_W + x];
			MWCOLORVAL c = !l ? overview_colour[tiles[y * MP_W + x] % 960] : show == MP_SHOW_POWER ? power[(l - 1) % 3] :
				       show == MP_SHOW_GROWTH ? growth[(l - 1) & 3] : levels[(l - 1) & 3];
			MWCOLORVAL *p = pixels + y * 2 * MP_W * 2 + x * 2;

			p[0] = p[1] = p[MP_W * 2] = p[MP_W * 2 + 1] = c;
		}
	GrArea(w, ui_gc, 4, 4, MP_W * 2, MP_H * 2, pixels, MWPF_RGB);
	/* the main view's rectangle */
	GrSetGCForeground(ui_gc, WHITE);
	GrRect(w, ui_gc, 4 + scroll_x / 8, 4 + scroll_y / 8, map_w / 8, map_h / 8);
	for (i = 0; i < MP_SHOWS; i++) {
		int at = 4 + i * 14;

		ui_fill(w, 252, at - 1, 116, 14, i == show ? UI_SELECTED : UI_FACE);
		ui_text(w, 256, at, show_names[i], -1, i == show ? WHITE : BLACK, 0);
	}
}

static int shown_percent[3];

static void
draw_budget(void)
{
	static const char *const names[3] = {"Roads", "Fire", "Police"};
	GR_WINDOW_ID w = extra[W_BUDGET];
	struct mp_budget b;
	char line[120];
	int k;

	mp_budget(&b);
	ui_fill(w, 0, 0, window_sizes[W_BUDGET][0], window_sizes[W_BUDGET][1], UI_FACE);
	snprintf(line, sizeof line, "Tax rate %d%%, taxes collected $%d a year", b.tax_rate, b.taxes);
	ui_text(w, 8, 8, line, -1, BLACK, 0);
	ui_slider(w, 8, 26, 324, b.tax_rate, 20);
	for (k = 0; k < 3; k++) {
		shown_percent[k] = b.percent[k];
		snprintf(line, sizeof line, "%s: %d%% of $%d asked, $%d spent", names[k], b.percent[k], b.wanted[k], b.spent[k]);
		ui_text(w, 8, 54 + k * 40, line, -1, BLACK, 0);
		ui_slider(w, 8, 72 + k * 40, 324, b.percent[k], 100);
	}
	snprintf(line, sizeof line, "Cash flow $%d a year, funds $%d", b.cash_flow, b.funds);
	ui_text(w, 8, 176, line, -1, b.cash_flow < 0 ? MWRGB(160, 0, 0) : BLACK, 0);
	ui_fill(w, 8, 200, 14, 14, WHITE);
	ui_bevel(w, 8, 200, 14, 14, 1);
	if (shown.auto_budget)
		ui_text(w, 10, 201, "x", -1, BLACK, 0);
	ui_text(w, 28, 201, "Budget by itself", -1, BLACK, 0);
	ui_button(w, 256, 196, 76, 24, budget_paused ? "Go on" : "Done", 0);
}

/* A click or a drag in the budget window. */
static void
budget_at(int x, int y, int click)
{
	int percent[3] = {shown_percent[0], shown_percent[1], shown_percent[2]}, k;

	if (y >= 26 && y < 44) {
		mp_set_tax(ui_slider_value(x, 8, 324, 20));
		status_now();
	}
	for (k = 0; k < 3; k++)
		if (y >= 72 + k * 40 && y < 90 + k * 40) {
			percent[k] = ui_slider_value(x, 8, 324, 100);
			mp_set_budget(percent);
		}
	if (click && ui_inside(x, y, 8, 198, 160, 18)) {
		mp_set_auto_budget(!shown.auto_budget);
		reg_set_int("micropolis.autobudget", !shown.auto_budget);
		status_now();
	}
	if (click && ui_inside(x, y, 256, 196, 76, 24)) {
		close_extra(W_BUDGET);
		return;
	}
	draw_budget();
}

static void
draw_evaluation(void)
{
	static const char *const problems[7] = {"Crime", "Pollution", "Housing costs", "Taxes", "Traffic", "Unemployment", "Fire"};
	static const char *const classes[6] = {"Village", "Town", "City", "Capital", "Metropolis", "Megalopolis"};
	static const char *const levels[3] = {"Easy", "Medium", "Hard"};
	GR_WINDOW_ID w = extra[W_EVALUATION];
	struct mp_evaluation e;
	char line[120];
	int k, y = 8;

	mp_evaluation(&e);
	ui_fill(w, 0, 0, window_sizes[W_EVALUATION][0], window_sizes[W_EVALUATION][1], UI_FACE);
	ui_text(w, 8, y, "Is the mayor doing a good job?", -1, BLACK, 0);
	snprintf(line, sizeof line, "Yes %d%%    No %d%%", e.yes, 100 - e.yes);
	ui_text(w, 20, y += 18, line, -1, BLACK, 0);
	ui_text(w, 8, y += 26, "What are the worst problems?", -1, BLACK, 0);
	for (k = 0; k < 4; k++)
		if (e.problems[k] >= 0 && e.problems[k] < 7) {
			snprintf(line, sizeof line, "%s  %d%%", problems[e.problems[k]], e.votes[k]);
			ui_text(w, 20, y += 16, line, -1, BLACK, 0);
		}
	y = 156;
	snprintf(line, sizeof line, "Population %d, %+d last year", e.population, e.population_delta);
	ui_text(w, 8, y, line, -1, BLACK, 0);
	snprintf(line, sizeof line, "Assessed value $%d", e.assessed);
	ui_text(w, 8, y += 18, line, -1, BLACK, 0);
	snprintf(line, sizeof line, "A %s, game level %s", classes[e.city_class >= 0 && e.city_class < 6 ? e.city_class : 0],
		 levels[e.level >= 0 && e.level < 3 ? e.level : 0]);
	ui_text(w, 8, y += 18, line, -1, BLACK, 0);
	snprintf(line, sizeof line, "Overall score %d of 1000, %+d last year", e.score, e.score_delta);
	ui_text(w, 8, y += 18, line, -1, BLACK, 0);
}

static const char *const history_names[6] = {"Residential", "Commercial", "Industrial", "Cash flow", "Crime", "Pollution"};
static const unsigned history_colours[6] = {MWRGB(0, 160, 0), MWRGB(48, 80, 255), MWRGB(200, 160, 0), MWRGB(0, 160, 160),
	MWRGB(220, 0, 0), MWRGB(128, 96, 32)};
static int graph_on = 0x3f, graph_long;

static void
draw_graphs(void)
{
	GR_WINDOW_ID w = extra[W_GRAPHS];
	short history[6][120];
	int k, i, most = 1, top = 8, high_g = 120, left = 8, wide_g = 340;

	ui_fill(w, 0, 0, window_sizes[W_GRAPHS][0], window_sizes[W_GRAPHS][1], UI_FACE);
	for (k = 0; k < 6; k++)
		mp_history(k, graph_long, history[k]);
	for (k = 0; k < 3; k++)
		for (i = 0; i < 120; i++)
			most = history[k][i] > most ? history[k][i] : most;
	ui_fill(w, left, top, wide_g, high_g, WHITE);
	ui_bevel(w, left - 1, top - 1, wide_g + 2, high_g + 2, 1);
	for (k = 0; k < 6; k++) {
		int scale = k < 3 ? most : 256, px = 0, py = 0;

		if (!(graph_on >> k & 1))
			continue;
		GrSetGCForeground(ui_gc, history_colours[k]);
		/* the newest month on the right */
		for (i = 0; i < 120; i++) {
			int v = history[k][i] < 0 ? 0 : history[k][i] > scale ? scale : history[k][i];
			int x = left + wide_g - 1 - i * (wide_g - 1) / 119, y = top + high_g - 2 - v * (high_g - 4) / scale;

			if (i)
				GrLine(w, ui_gc, px, py, x, y);
			px = x, py = y;
		}
	}
	for (k = 0; k < 6; k++) {
		int x = 8 + k % 3 * 116, y = 136 + k / 3 * 20;

		ui_fill(w, x, y + 2, 12, 12, graph_on >> k & 1 ? history_colours[k] : WHITE);
		ui_bevel(w, x, y + 2, 12, 12, 1);
		ui_text(w, x + 16, y, history_names[k], -1, BLACK, 0);
	}
	ui_button(w, 8, 176, 80, 24, "10 years", !graph_long);
	ui_button(w, 94, 176, 80, 24, "120 years", graph_long);
}

static void
graphs_at(int x, int y)
{
	int k;

	for (k = 0; k < 6; k++)
		if (ui_inside(x, y, 8 + k % 3 * 116, 136 + k / 3 * 20, 112, 18))
			graph_on ^= 1 << k;
	if (ui_inside(x, y, 8, 176, 80, 24))
		graph_long = 0;
	if (ui_inside(x, y, 94, 176, 80, 24))
		graph_long = 1;
	draw_graphs();
}

static void
draw_extra(int k)
{
	static int placed;

	if (!extra[k])
		return;
	if (report && !(placed >> k & 1)) {
		int x, y;

		ui_window_origin(extra[k], &x, &y);
		printf("nxmicropolis: %s at %d,%d\n", window_titles[k], x, y);
		fflush(stdout);
		placed |= 1 << k;
	}
	switch (k) {
	case W_MAP: draw_overview(); break;
	case W_BUDGET: draw_budget(); break;
	case W_EVALUATION: draw_evaluation(); break;
	case W_GRAPHS: draw_graphs(); break;
	}
	GrFlush();
}

/* What the engine said: its messages, a query's answer, a scenario's end. */
static void
hear(void)
{
	struct mp_said s;
	char text[200];

	while (mp_next_said(&s))
		switch (s.kind) {
		case MP_SAID_SOUND:
			sounds_asked++;
			play(s.sound, s.x, s.y);
			break;
		case MP_SAID_BUDGET:
			if (!shown.paused) {
				mp_pause(1);
				budget_paused = 1;
			}
			open_extra(W_BUDGET);
			say("The year's budget: set it, then go on.");
			break;
		case MP_SAID_MESSAGE:
			if (s.number > 0)
				say(string(0, s.number));
			break;
		case MP_SAID_QUERY:
			snprintf(text, sizeof text, "%s: density %s, value %s, crime %s, pollution %s, growth %s", string(1, s.query[0]),
				 string(2, s.query[1]), string(2, s.query[2]), string(2, s.query[3]), string(2, s.query[4]), string(2, s.query[5]));
			say(text);
			break;
		case MP_SAID_WON:
			say("You have won the scenario: the city is a success.");
			break;
		case MP_SAID_LOST:
			say("You have lost the scenario: the citizens have voted you out.");
			break;
		}
}

/* The speed kept: one a menu has (0 would be a pause no menu shows). */
static int
reg_speed(void)
{
	int speed = reg_int("micropolis.speed", 2);

	return speed < 1 || speed > 3 ? 2 : speed;
}

static void
new_city(void)
{
	scroll_x = (MP_W * TILE - map_w) / 2, scroll_y = (MP_H * TILE - map_h) / 2;
	clamp_scroll();
	fill_ring(0);
	/* the player's, not the city file's */
	mp_set_speed(reg_speed());
	mp_set_auto_budget(reg_int("micropolis.autobudget", 1));
	memset(&shown, -1, sizeof shown);
	status_now();
	dirty = 1;
}

static void
open_city(const char *path)
{
	char text[600];

	if (mp_load(path) < 0) {
		snprintf(text, sizeof text, "%s could not be read as a city.", path);
		say(text);
		return;
	}
	snprintf(city_path, sizeof city_path, "%s", path);
	new_city();
	say("");
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window) {
		draw_bar();
		draw_status();
	}
	if (event->type == GR_EVENT_TYPE_EXPOSURE && extra_of(event->exposure.wid) >= 0)
		draw_extra(extra_of(event->exposure.wid));
	/* (the X under a question: not forgotten) */
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		exit(0);
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && extra_of(event->general.wid) >= 0)
		close_extra(extra_of(event->general.wid));
}

static void
save_city(int ask)
{
	char path[512], name[512];
	const char *slash;

	mkdir(SAVED, 0755);
	if (ask || !city_path[0] || strncmp(city_path, SAVED "/", sizeof SAVED)) {
		slash = strrchr(city_path, '/');
		snprintf(name, sizeof name, "%s", city_path[0] ? (slash ? slash + 1 : city_path) : "city.cty");
		if (!ui_save_file("Save the city", SAVED, ".cty", name, path, sizeof path, others))
			return;
		ui_files_ending(path, sizeof path, ".cty");
		snprintf(city_path, sizeof city_path, "%s", path);
	}
	say(mp_save(city_path) < 0 ? "The city could not be saved." : "The city is saved.");
}

enum { A_NEW = 1, A_OPEN, A_SAVE, A_SAVE_AS, A_QUIT, A_PAUSE, A_SLOW, A_MEDIUM, A_FAST, A_AUTO_BUDGET, A_DISASTERS,
       A_ABOUT, A_WINDOW, A_TAX = 40, A_SCENARIO = 70, A_DISASTER = 90 };

static void
act(int id)
{
	static const char *const ok[] = {"OK"};

	if (id >= A_SCENARIO && id < A_SCENARIO + 8) {
		mp_scenario(id - A_SCENARIO + 1);
		city_path[0] = 0;
		new_city();
		say(scenario_names[id - A_SCENARIO]);
	} else if (id >= A_TAX && id <= A_TAX + 20) {
		mp_set_tax(id - A_TAX);
		status_now();
	} else if (id >= A_DISASTER && id < A_DISASTER + 6)
		mp_disaster(id - A_DISASTER);
	else if (id >= A_WINDOW && id < A_WINDOW + W_COUNT)
		open_extra(id - A_WINDOW);
	switch (id) {
	case A_NEW:
		mp_generate((int)now_ms());
		city_path[0] = 0;
		new_city();
		say("A new city: build roads, power and zones.");
		break;
	case A_OPEN: {
		char path[512];

		mkdir(SAVED, 0777);
		if (ui_choose_file("Open a city", city_path[0] ? SAVED : DATA "/cities", ".cty", path, sizeof path, others))
			open_city(path);
		break;
	}
	case A_SAVE:
	case A_SAVE_AS:
		save_city(id == A_SAVE_AS);
		break;
	case A_QUIT:
		exit(0);
	case A_PAUSE:
		mp_pause(!shown.paused);
		break;
	case A_SLOW:
	case A_MEDIUM:
	case A_FAST:
		mp_pause(0);
		mp_set_speed(id - A_SLOW + 1);
		reg_set_int("micropolis.speed", id - A_SLOW + 1);
		break;
	case A_AUTO_BUDGET:
		mp_set_auto_budget(!shown.auto_budget);
		reg_set_int("micropolis.autobudget", !shown.auto_budget);
		break;
	case A_DISASTERS:
		mp_set_disasters(!shown.disasters);
		reg_set_int("micropolis.disasters", !shown.disasters);
		break;
	case A_ABOUT:
		ui_ask("About Micropolis",
		       "Micropolis, the city simulator of 1989 (GPLv3, EA's terms: COPYING in " DATA ").\n"
		       "Copyright (C) 1989 - 2007 Electronic Arts Inc. This is a modified version.\n"
		       "Micropolis is a registered trademark of Micropolis Corporation (Micropolis GmbH)\n"
		       "and is licensed here as a courtesy of the owner (micropolis.com).",
		       ok, 1, others);
		break;
	}
	status_now();
	dirty = 1;
}

static void
open_menu(int which)
{
	static struct ui_menu_item menu[12], scenarios[9], taxes[21];
	static char tax_labels[21][8];
	static const struct { const char *label; int id; } rows[MENUS][8] = {
		{{"New city", A_NEW}, {"Scenarios", A_SCENARIO}, {"Open...", A_OPEN}, {"Save", A_SAVE}, {"Save as...", A_SAVE_AS}, {"Quit", A_QUIT}},
		{{"Pause    Space", A_PAUSE}, {"Slow", A_SLOW}, {"Medium", A_MEDIUM}, {"Fast", A_FAST}},
		{{"Budget by itself", A_AUTO_BUDGET}, {"Disasters", A_DISASTERS}, {"Tax rate", A_TAX}},
		{{"Fire", A_DISASTER}, {"Flood", A_DISASTER + 1}, {"Tornado", A_DISASTER + 2}, {"Earthquake", A_DISASTER + 3},
		 {"Monster", A_DISASTER + 4}, {"Meltdown", A_DISASTER + 5}},
		{{"Map", A_WINDOW + W_MAP}, {"Budget", A_WINDOW + W_BUDGET}, {"Evaluation", A_WINDOW + W_EVALUATION},
		 {"Graphs", A_WINDOW + W_GRAPHS}},
		{{"About Micropolis", A_ABOUT}},
	};
	int i, n, x, y;

	for (n = 0; n < 8 && rows[which][n].label; n++) {
		int id = rows[which][n].id;

		memset(&menu[n], 0, sizeof menu[n]);
		menu[n].label = rows[which][n].label, menu[n].id = id, menu[n].icon = -1;
		if (id == A_SCENARIO)
			menu[n].sub = scenarios, menu[n].sub_count = 8, menu[n].id = 0;
		if (id == A_TAX)
			menu[n].sub = taxes, menu[n].sub_count = 21, menu[n].id = 0;
		if (id == A_OPEN || id == A_QUIT || id == A_SLOW)
			menu[n].flags |= UI_MENU_LINE;
		if ((id == A_PAUSE && shown.paused) || (id >= A_SLOW && id <= A_FAST && !shown.paused && shown.speed == id - A_SLOW + 1) ||
		    (id == A_AUTO_BUDGET && shown.auto_budget) || (id == A_DISASTERS && shown.disasters))
			menu[n].flags |= UI_MENU_CHECK;
	}
	for (i = 0; i < 8; i++)
		memset(&scenarios[i], 0, sizeof scenarios[i]), scenarios[i].label = scenario_names[i], scenarios[i].id = A_SCENARIO + i, scenarios[i].icon = -1;
	for (i = 0; i <= 20; i++) {
		memset(&taxes[i], 0, sizeof taxes[i]);
		snprintf(tax_labels[i], sizeof tax_labels[i], "%d%%", i);
		taxes[i].label = tax_labels[i], taxes[i].id = A_TAX + i, taxes[i].icon = -1;
		taxes[i].flags = i == shown.tax ? UI_MENU_CHECK : 0;
	}
	ui_window_origin(window, &x, &y);
	ui_menu_open(menu, n, x + bar_menus[which].x, y + BAR, 0, 0);
}

/* ---- the pointer and the keys ---- */

/* A tool used on a tile, saying why not when it was not. */
static void
use_tool(int tx, int ty)
{
	int result = mp_tool(tool, tx, ty);

	if (result == -2)
		say("Not enough funds for that.");
	else if (result == -1)
		say("Bulldoze the area first.");
	else if (result == 1 && tool != MP_QUERY)
		play(tool == MP_BULLDOZER ? "bulldozer" : "build", -1, -1);
	fill_ring(0);
	hear();
	status_now();
	dirty = 1;
}

/* The line of tiles from the last one used to this one, for the tools that are drawn (roads, wires). */
static void
use_along(int tx, int ty)
{
	while (last_tx != tx || last_ty != ty) {
		if (last_tx != tx)
			last_tx += tx > last_tx ? 1 : -1;
		else
			last_ty += ty > last_ty ? 1 : -1;
		use_tool(last_tx, last_ty);
	}
}

static void
pointer_moved(int x, int y)
{
	int tx = -1, ty = -1;

	if (x >= PANEL && x < view_w && y >= 0 && y < view_h)
		tx = (x - PANEL + scroll_x) / TILE, ty = (y + scroll_y) / TILE;
	if (dragging) {
		scroll_x = drag_sx - (x - drag_x), scroll_y = drag_sy - (y - drag_y);
		clamp_scroll();
		fill_ring(0);
		dirty = 1;
	} else if (pressed && tx >= 0 && (tx != last_tx || ty != last_ty)) {
		if (mp_tool_size(tool) == 1 && tool != MP_QUERY)
			use_along(tx, ty);
	}
	if (tx != hover_x || ty != hover_y)
		hover_x = tx, hover_y = ty, dirty = 1;
}

static void
press(int x, int y, int buttons)
{
	int t;

	if (x < PANEL) {
		if ((t = tool_at(x, y)) >= 0) {
			tool = t;
			draw_status();
			dirty = 1;
		}
		return;
	}
	if (buttons & GR_BUTTON_R) {
		dragging = 1, drag_x = x, drag_y = y, drag_sx = scroll_x, drag_sy = scroll_y;
		return;
	}
	pointer_moved(x, y);
	if (hover_x < 0)
		return;
	pressed = 1, last_tx = hover_x, last_ty = hover_y;
	use_tool(hover_x, hover_y);
}

static void
scroll_by(int dx, int dy)
{
	scroll_x += dx, scroll_y += dy;
	clamp_scroll();
	fill_ring(0);
	dirty = 1;
}

/* A click (or, with the button held, a drag) in one of the other windows. */
static void
extra_press(int k, int x, int y, int click)
{
	switch (k) {
	case W_MAP:
		if (x < 4 + MP_W * 2 && y < 4 + MP_H * 2) {
			scroll_x = (x - 4) * 8 - map_w / 2, scroll_y = (y - 4) * 8 - map_h / 2;
			clamp_scroll();
			fill_ring(0);
			dirty = 1;
		} else if (click && x >= 252 && (y - 3) / 14 < MP_SHOWS)
			show = (y - 3) / 14;
		draw_overview();
		break;
	case W_BUDGET:
		budget_at(x, y, click);
		break;
	case W_GRAPHS:
		if (click)
			graphs_at(x, y);
		break;
	}
	GrFlush();
}

static int
extra_event(GR_EVENT *e)
{
	int k;

	switch (e->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		if ((k = extra_of(e->exposure.wid)) < 0)
			return 0;
		draw_extra(k);
		return 1;
	case GR_EVENT_TYPE_CLOSE_REQ:
		if ((k = extra_of(e->general.wid)) < 0)
			return 0;
		close_extra(k);
		return 1;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if ((k = extra_of(e->button.wid)) < 0)
			return 0;
		held_in = k;
		ui_moves(extra[k], W_EVENTS, 1);
		extra_press(k, e->button.x, e->button.y, 1);
		return 1;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		if ((k = extra_of(e->mouse.wid)) < 0)
			return 0;
		if (held_in == k)
			extra_press(k, e->mouse.x, e->mouse.y, 0);
		return 1;
	case GR_EVENT_TYPE_BUTTON_UP:
		if ((k = extra_of(e->button.wid)) < 0)
			return 0;
		ui_moves(extra[k], W_EVENTS, 0);
		held_in = -1;
		return 1;
	}
	return 0;
}

static void
events(int wait_ms)
{
	GR_EVENT e;
	int id, i;

	for (;;) {
		if (wait_ms > 0) {
			GrGetNextEventTimeout(&e, wait_ms);
			wait_ms = 0;
		} else
			GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE || e.type == GR_EVENT_TYPE_TIMEOUT)
			return;
		ui_origin_note(&e);
		if (extra_event(&e))
			continue;
		switch (ui_menu_event(&e, &id)) {
		case UI_MENU_CHOSEN:
			act(id);
			/* fall through */
		case UI_MENU_CLOSED:
		case UI_MENU_TOOK:
			continue;
		}
		switch (e.type) {
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (e.general.wid == window)
				exit(0);
			break;
		case GR_EVENT_TYPE_EXPOSURE:
			if (e.exposure.wid == window)
				draw_bar(), draw_status();
			dirty = 1;
			break;
		case GR_EVENT_TYPE_UPDATE:
			/* a new size: the map area as large as the window gives, the same city's place in its middle */
			if (e.update.wid == window && e.update.utype == GR_UPDATE_SIZE && (e.update.width != wide || e.update.height != high)) {
				int mx = scroll_x + map_w / 2, my = scroll_y + map_h / 2;

				seglSettle();
				lay_out(e.update.width, e.update.height);
				GrResizeWindow(view, view_w, view_h);
				seglWindowChanged();
				scroll_x = mx - map_w / 2, scroll_y = my - map_h / 2;
				clamp_scroll();
				fill_ring(0);
				draw_bar(), draw_status();
				dirty = 1;
				if (report) {
					printf("nxmicropolis: the window is %dx%d, the map %dx%d at %d,%d\n", wide, high, map_w, map_h, scroll_x, scroll_y);
					fflush(stdout);
				}
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (e.button.wid != window)
				break;
			raise_extras();
			if (e.button.y < BAR) {
				for (i = 0; i < MENUS; i++)
					if (e.button.x >= bar_menus[i].x && e.button.x < bar_menus[i].x + bar_menus[i].w)
						open_menu(i);
			} else if (e.button.y < BAR + view_h) {
				int k = ui_pad_arrow(&e);

				if (k) {	/* (a notch of the left hand's stick: the map a step that way) */
					scroll_by(k == MWKEY_LEFT ? -64 : k == MWKEY_RIGHT ? 64 : 0, k == MWKEY_UP ? -64 : k == MWKEY_DOWN ? 64 : 0);
					if (report) {
						printf("nxmicropolis: pad scrolls the map to %d,%d\n", scroll_x, scroll_y);
						fflush(stdout);
					}
				}
				else if (ui_wheel(&e))
					scroll_by(0, ui_wheel(&e) > 0 ? 64 : -64);
				else
					press(e.button.x, e.button.y - BAR, e.button.changebuttons);
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			pressed = dragging = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (e.mouse.wid == window)
				pointer_moved(e.mouse.x, e.mouse.y - BAR);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			switch (e.keystroke.ch) {
			case ' ': act(A_PAUSE); break;
			case MWKEY_LEFT: scroll_by(-64, 0); break;
			case MWKEY_RIGHT: scroll_by(64, 0); break;
			case MWKEY_UP: scroll_by(0, -64); break;
			case MWKEY_DOWN: scroll_by(0, 64); break;
			}
			break;
		}
	}
}

int
main(int argc, char **argv)
{
	GR_SCREEN_INFO info;
	const char *text;
	unsigned now, report_at;
	int refresh = 0;
	static char first_city[PATH_MAX];

	/* (a city named on the command line is where it is from here) */
	if (argc > 1 && argv[1][0] != '/' && realpath(argv[1], first_city))
		argv[1] = first_city;
	if (chdir(DATA) < 0) {
		fprintf(stderr, "nxmicropolis: no %s\n", DATA);
		return 1;
	}
	if ((text = getenv("MICROPOLIS_SUM")) != NULL)
		return mp_sum_test(text) < 0;
	if (GrOpen() < 0) {
		fprintf(stderr, "nxmicropolis: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	screen_w = info.cols, screen_h = info.rows;
	wide = info.cols - 20 < 880 ? info.cols - 20 : 880;
	high = info.rows - 70 < 640 ? info.rows - 70 : 640;
	lay_out(wide, high);
	ui_init();
	/* (resizable: a larger window shows more of the map, lay_out) */
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Micropolis", GR_ROOT_WINDOW_ID, -1, -1, wide, high, UI_FACE);
	ui_pad_ask(window);
	view = GrNewWindow(window, 0, BAR, view_w, view_h, 0, BLACK, BLACK);
	GrMapWindow(view);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	GrSelectEvents(window, EVENTS);	/* (before the seconds it loads: an X with nobody listening was lost, and the bar called it hung) */
	if (seglInit(view) < 0) {
		fprintf(stderr, "nxmicropolis: this machine has no GPU\n");
		return 1;
	}
	GrSelectEvents(view, 0);
	GrSelectEvents(window, EVENTS);
	if (load_pictures() < 0 || mp_init() < 0) {
		fprintf(stderr, "nxmicropolis: the pictures in %s did not load\n", DATA);
		return 1;
	}
	load_strings(0, "stri.301.txt");
	load_strings(1, "stri.219.txt");
	load_strings(2, "stri.202.txt");
	load_overview();
	load_sounds();
	mp_set_speed(reg_speed());
	mp_set_auto_budget(reg_int("micropolis.autobudget", 1));
	mp_set_disasters(reg_int("micropolis.disasters", 1));
	if (argc > 1)
		open_city(argv[1]);
	if (argc <= 1 || !city_path[0]) {
		mp_generate((int)now_ms());
		new_city();
		if (argc <= 1)
			say("Micropolis is a registered trademark of Micropolis GmbH, licensed here as a courtesy of the owner.");
		else {	/* (what the city could not be, not over-written: the city here is a new one) */
			char text[600];

			snprintf(text, sizeof text, "%s could not be read as a city: this is a new one.", argv[1]);
			say(text);
		}
	}
	draw_bar();
	/* (MICROPOLIS_REPORT=1, for a test: where the window is, then what was done every five seconds) */
	if ((report = getenv("MICROPOLIS_REPORT") != NULL)) {
		int x, y;

		ui_window_origin(window, &x, &y);
		printf("nxmicropolis: window at %d,%d, the map at %d,%d; %d sounds\n", x, y, x + PANEL, y + BAR, sound_count);
		fflush(stdout);
	}
	next_tick = report_at = now_ms();
	for (;;) {
		now = now_ms();
		if (report && (int)(now - report_at) >= 5000) {
			printf("nxmicropolis: %d ticks, %d frames, %d of %d sounds in %u ms\n", ticks_done, frames_done, sounds_played,
			       sounds_asked, now - report_at);
			fflush(stdout);
			ticks_done = frames_done = sounds_played = sounds_asked = 0, report_at = now;
		}
		if ((int)(now - next_tick) >= 0) {
			/* a step of the city ten times a second; the engine's speed says how many of them simulate */
			next_tick += 100;
			if ((int)(now - next_tick) > 500)
				next_tick = now + 100;
			if (!shown.paused) {
				mp_tick();
				ticks_done++;
				fill_ring(1);
				dirty = 1;
			}
			hear();
			status_now();
			/* a controller's buttons (docs/machine/input.md, "Game controls"): A pauses and goes on */
			ui_pad_poll(&pad);
			if (pad.down & INPUT_JUMP) {
				act(A_PAUSE);
				if (report) {
					printf("nxmicropolis: pad %s\n", shown.paused ? "pauses" : "goes on");
					fflush(stdout);
				}
			}
			/* the other windows once a second, the map's every other */
			if (++refresh % 10 == 0) {
				if (refresh % 20 == 0)
					draw_extra(W_MAP);
				if (held_in != W_BUDGET)
					draw_extra(W_BUDGET);
				draw_extra(W_EVALUATION);
				if (refresh % 50 == 0)
					draw_extra(W_GRAPHS);
			}
		}
		if (dirty)
			draw_view();
		now = now_ms();
		events((int)(next_tick - now) > 0 ? (int)(next_tick - now) : 1);
	}
}
