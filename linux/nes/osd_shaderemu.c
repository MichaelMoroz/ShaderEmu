/*
 * Nofrendo's machine side on ShaderEmu (docs/nes.md): the NES's picture is bytes in GPU memory
 * that the GPU stretches over a Nano-X window through the display's palette, the pad is the
 * keyboard, and a frame is drawn for every few emulated (NES_SKIP), the machine being slow.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <noftypes.h>
#include <nofconfig.h>
#include <bitmap.h>
#include <event.h>
#include <log.h>
#include <nes.h>
#include <nesinput.h>
#include <nofrendo.h>
#include <osd.h>
#include <nes_ppu.h>
#include <nes_rom.h>
#include <GLES/segl.h>
#include "mcw.h"
#define MWINCLUDECOLORS
#include "nano-X.h"

#define WIDTH NES_SCREEN_WIDTH
#define HEIGHT NES_VISIBLE_HEIGHT
#define ONE 65536

static const char *rom_path;
static char *rom_data;
static GR_WINDOW_ID window;
static unsigned char *picture;		/* WIDTH x HEIGHT bytes of GPU memory */
static GLuint texture;
static void (*tick)(void);		/* nofrendo's 60 Hz timer: counts the frames owed */
static int skip = 2;			/* emulated frames not drawn for each one that is */
static int frames_most, frames_hold, frames_drawn, frames_emulated;
static unsigned gd_spent[5], gd_swap_at;	/* instructions: following the game's writes, the quads, the swap */
static int gpu_every = 1;		/* the GPU draws a frame in so many: more than one when the game is late */
static const char *script;		/* NES_PRESS: frames and the buttons to press at them */
static unsigned cycles_then, ms_then;
extern void nes6502_recompile(uint8 *prg, int bytes, int fixed);
extern void nes6502_recompiled(unsigned *instructions, unsigned *bytes, unsigned *runs, unsigned *entries);
extern void (*nes_frame_elsewhere)(int after);
extern int nes_sum_on;			/* ours, in nofrendo's frame: NES_SUM, for tests */
extern uint32 nes_sum;	/* ours, in nofrendo's loop: see frame_elsewhere */
extern char __DATA_BEGIN__[], _end[];
static void window_draw(GLuint of);
static void frame_counted(int emulated, int step, const unsigned char *shown);
static void elsewhere_start(void);
static void frame_paced(void);
static int unlimited;			/* NES_FAST: as fast as it goes, for measuring */
extern int nes6502_plain;		/* ours, in nofrendo's 6502: idle loops run turn by turn */

static unsigned
cycles(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static unsigned
milliseconds(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (unsigned)(now.tv_sec * 1000 + now.tv_usec / 1000);
}

/* ---- the cartridge: a file read whole ---- */

char *
osd_getromdata(void)
{
	FILE *file;
	long size;

	if (rom_data)
		return rom_data;
	if (!(file = fopen(rom_path, "rb"))) {
		fprintf(stderr, "nes: cannot open %s\n", rom_path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	rom_data = malloc(size);
	if (!rom_data || fread(rom_data, 1, size, file) != (size_t)size) {
		fprintf(stderr, "nes: cannot read %s\n", rom_path);
		exit(1);
	}
	fclose(file);
	return rom_data;
}

/* ---- the timer: no interrupt here, the frames owed are counted where a frame is shown ---- */

int
osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
	tick = (void (*)(void))func;
	return 0;
}

/* ---- sound: none yet ---- */

/* (also where nofrendo says the cartridge is in and its loop about to start) */
void
osd_setsound(void (*playfunc)(void *buffer, int length))
{
	const char *cpu = getenv("NES_CPU");
	rominfo_t *rom = nes_getcontextptr()->rominfo;

	/* the 6502 translated as it goes (rc6502.h), unless NES_CPU=interp */
	if (!cpu || strcmp(cpu, "interp"))
		nes6502_recompile(rom->rom, rom->rom_banks * 0x4000, rom->mapper_number == 0);
	elsewhere_start();
}

void
osd_getsoundinfo(sndinfo_t *info)
{
	info->sample_rate = 22050;
	info->bps = 16;
}

/* ---- video ---- */

static int
video_init(int width, int height)
{
	return 0;
}

static void
video_shutdown(void)
{
}

static int
video_mode(int width, int height)
{
	return 0;
}

/* The NES's colours (and nofrendo's own for its messages) are the display's palette. */
static void
video_palette(rgb_t *pal)
{
	unsigned int *words = seglPalette();
	int i;

	for (i = 0; i < 256; i++)
		words[i] = (unsigned)pal[i].r << 16 | (unsigned)pal[i].g << 8 | (unsigned)pal[i].b;
}

static void
video_clear(uint8 color)
{
	memset(picture, color, WIDTH * HEIGHT);
}

/* The screen as nofrendo's bitmap: made once and kept (it reads it after giving it back). */
static bitmap_t *
video_lock(void)
{
	static bitmap_t *screen;

	if (!screen)
		screen = bmp_createhw(picture, WIDTH, HEIGHT, WIDTH);
	return screen;
}

static void
video_free(int num_dirties, rect_t *dirty_rects)
{
}

/* A frame is ready: its rows go to GPU memory and the GPU draws them over the whole window. */
static void
video_show(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed xyz[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	int y;

	for (y = 0; y < HEIGHT && y < bmp->height; y++)
		memcpy(picture + y * WIDTH, bmp->line[y], WIDTH);
	window_draw(texture);

	/* the frames to emulate before the next one is drawn */
	for (y = 0; y < skip + 1 && tick; y++)
		tick();
	frames_drawn++;
	frames_emulated = frames_drawn * (skip + 1);
	frame_counted(frames_emulated, skip + 1, picture);
}

/* The GPU draws a picture over the whole window. */
static void
window_draw(GLuint of)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed xyz[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	int width, height;

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(xyz, uv, of, 255, 255, 0);
	seglSwap();
}

/* After `step` more frames, `emulated` in all: the figures, and a test's end at `shown`. */
static void
frame_counted(int emulated, int step, const unsigned char *shown)
{
	static int since, drawn_then;

	since += step;
	if (since >= 60) {
		unsigned now = cycles(), ms = milliseconds();

		fprintf(stderr, "nesstat: %d frames of the NES in %u ms (%d drawn): %u thousand instructions each, %u%% of its speed\n",
			since, ms - ms_then, frames_drawn - drawn_then, (now - cycles_then) / 1000 / since,
			ms > ms_then ? since * 100000 / 60 / (ms - ms_then) : 0);
		if (gd_spent[1])
			fprintf(stderr, "nesgpu: a frame's %u instructions to follow the game's writes, %u for %u quads in %u runs of lines, %u for the swap\n",
				gd_spent[0] / since, gd_spent[1] / since, gd_spent[4] / since, gd_spent[3] / since, gd_spent[2] / since);
		gd_spent[0] = gd_spent[1] = gd_spent[2] = gd_spent[3] = gd_spent[4] = 0;
		cycles_then = now;
		ms_then = ms;
		drawn_then = frames_drawn;
		since = 0;
	}
	if (frames_hold && emulated >= frames_hold) {
		GR_EVENT event;

		/* (for a test: the picture stays, and a snapshot has it in GPU memory) */
		fprintf(stderr, "nes: holding, picture %d bytes past the palette\n", (int)(shown - (unsigned char *)seglPalette()));
		do
			GrGetNextEvent(&event);
		while (event.type != GR_EVENT_TYPE_CLOSE_REQ);
		exit(0);
	}
	if (frames_most && emulated >= frames_most) {
		unsigned sum = 0;
		int i;

		/* (a sum of the last picture: what a change to the emulator must leave) */
		for (i = 0; i < WIDTH * HEIGHT; i++)
			sum = sum * 31 + shown[i];
		{
			unsigned instructions, bytes, runs, entries;

			nes6502_recompiled(&instructions, &bytes, &runs, &entries);
			fprintf(stderr, "nes: %u instructions of the 6502 translated in %u goes, into %u bytes; entered %u times\n",
				instructions, runs, bytes, entries);
		}
		if (nes_sum_on)
			fprintf(stderr, "nes: every frame's RAM %08x\n", nes_sum);
		fprintf(stderr, "nes: done, picture %08x\n", sum);
		exit(0);
	}
}

/* ---- the picture drawn elsewhere (docs/nes.md): no frame is drawn by this core; of the
 * frames it can, the PPU keeps a record, and worker cores draw the picture from it into a
 * texture not shown, while this core goes on with the next frames. The workers are in
 * teams, a frame a team: giving a frame out and hearing it is done cost two passes of the
 * machine whoever draws, so several frames under way at once show more of them ---- */

#define TEAMS_MOST 6

static struct team {
	int first, count;		/* its workers: jobs[first] on */
	int busy, record, picture;	/* what it draws from, and into */
} teams[TEAMS_MOST];
static int team_count, team_order[TEAMS_MOST], teams_busy;	/* the busy ones, the oldest first */
static int team_most, team_wanted = 1;	/* as many teams as leave the game its speed, when no team is busy */
static ppu_frame_t *records[TEAMS_MOST + 1];
static ppu_drawjob_t jobs[MC_MAX_CORES];
static unsigned char *pictures[TEAMS_MOST + 1];
static GLuint textures[TEAMS_MOST + 1];
static int drawers, inline_draw, front, recording;

/* A record, or a picture, that no busy team has (and that is not the one on show). */
static int
spare(int of_pictures)
{
	int n, t;

	for (n = 0; n <= team_most; n++) {
		if (of_pictures && n == front)
			continue;
		for (t = 0; t < team_count; t++)
			if (teams[t].busy && (of_pictures ? teams[t].picture : teams[t].record) == n)
				break;
		if (t == team_count)
			return n;
	}
	return -1;
}

static void
team_post(struct team *team, int record)
{
	int k;

	team->record = record;
	team->picture = spare(1);
	team->busy = 1;
	team_order[teams_busy++] = (int)(team - teams);
	for (k = 0; k < team->count; k++) {
		ppu_drawjob_t *job = &jobs[team->first + k];

		job->frame = records[record];
		job->rows = pictures[team->picture];
		job->first = HEIGHT * k / team->count;
		job->count = HEIGHT * (k + 1) / team->count - job->first;
		if (inline_draw)
			ppu_draw((uint32)job, 0);
		else
			mcw_post(team->first + k + 1, ppu_draw, (uint32_t)job, 0);
	}
}

/* Whether a team's picture is drawn; with `wait`, not before it is. */
static int
team_done(struct team *team, int wait)
{
	int k, done = 1;

	for (k = team->first + 1; k <= team->first + team->count && !inline_draw; k++) {
		if (wait)
			mcw_wait(k);
		else if (!mcw_done(k))
			done = 0;	/* (and the rest are still asked: a worker stopped at a page goes on) */
	}
	return done;
}

/* The pictures that are drawn, in the order their frames were: the newest of them is shown. */
static void
teams_show(int wait)
{
	int shown = -1, n;

	while (teams_busy && team_done(&teams[team_order[0]], wait)) {
		struct team *team = &teams[team_order[0]];

		team->busy = 0;
		shown = team->picture;
		for (n = 1; n < teams_busy; n++)
			team_order[n - 1] = team_order[n];
		teams_busy--;
	}
	/* (the others are asked too: a worker of theirs stopped at a page goes on) */
	for (n = 1; n < teams_busy; n++)
		team_done(&teams[team_order[n]], 0);
	if (shown >= 0) {
		front = shown;
		window_draw(textures[front]);
		frames_drawn++;
	}
}

/* The workers shared out between so many teams. */
static void
teams_of(int count)
{
	int k;

	team_count = count;
	for (k = 0; k < count; k++) {
		teams[k].first = drawers * k / count;
		teams[k].count = drawers * (k + 1) / count - teams[k].first;
		teams[k].busy = 0;
	}
}

/* Before and after each frame. A frame is recorded when a team is free to draw it. */
static void
frame_elsewhere(int after)
{
	int last = (frames_hold && frames_emulated + 1 >= frames_hold) || (frames_most && frames_emulated + 1 >= frames_most);
	int t;

	if (!after) {
		/* a test's last frame is the one it sees, whatever was being drawn */
		if (last)
			teams_show(1);
		recording = -1;
		if (!teams_busy && team_wanted != team_count)
			teams_of(team_wanted);
		/* (teams of another number wanted: no more is given out until these have done) */
		if (teams_busy < team_count && (team_wanted == team_count || last)) {
			recording = spare(0);
			if (!ppu_capture(records[recording]))
				recording = -1;
		}
		return;
	}
	frames_emulated++;
	teams_show(0);
	if (recording >= 0) {
		ppu_capture_end();
		for (t = 0; teams[t].busy; t++)
			;
		team_post(&teams[t], recording);
		if (last || inline_draw)
			teams_show(1);
	}
	/* (the pad every fourth frame: asking the window system is thousands of instructions) */
	if (!(frames_emulated & 3))
		osd_getinput();
	frame_counted(frames_emulated, 1, pictures[front]);
	frame_paced();
}

/* No faster than a NES: sixty frames a second by the machine's clock (a word of the GPU's
 * memory: reading it is a load). A game that is late is not owed the time back. */
static void
frame_paced(void)
{
	static unsigned due;
	const volatile unsigned *clock = (const volatile unsigned *)((char *)seglPalette() - 0x400 + 0x34);
	unsigned now = *clock;

	if (unlimited)
		return;
	due += 16 + (frames_emulated % 3 != 0);		/* 16.67 ms */
	/* busy workers make every pass of the machine longer: a team more while the game is ahead
	 * of its time, one fewer when it falls behind (looked at four times a second) */
	if (!(frames_emulated & 15)) {
		if ((int)(now - due) > 8 && team_wanted > 1)
			team_wanted--;
		else if ((int)(due - now) > 4 && team_wanted < team_most)
			team_wanted++;
		/* (and the GPU's frames: every one while the game keeps its time, one in two to four when it does not) */
		if ((int)(now - due) > 8 && gpu_every < 4)
			gpu_every++;
		else if ((int)(due - now) > 2 && gpu_every > 1)
			gpu_every--;
	}
	if ((int)(now - due) > 50 || (int)(due - now) > 200)
		due = now;
	else {
		/* (the kernel's sleeps are whole hundredths of a second: the last of the wait is passes
		 * of the machine ended early, which the clock word counts) */
		if ((int)(due - now) > 20)
			usleep((due - now - 12) * 1000);
		while ((int)(due - *clock) > 0)
			__asm__ volatile(".word 0x0100000f");
	}
}

/* ---- the picture drawn by the GPU (docs/nes.md): the background is a layer of tiles
 * (docs/gpu.md), the four name tables two by two as its cells; the sprites are another, a
 * cell or two a sprite; and a frame is a few quads of them for every run of lines the PPU's
 * registers stayed the same in. The pattern tables are kept as a byte a pixel in GPU
 * memory, and the cells follow what the game writes ---- */

#define SETS_MOST 24		/* different pages or colours in a frame; lines past that are drawn with the last */
#define SET_WORDS 176		/* a set: 144 colours, 12 words of pieces, four layers' four words */

static struct {
	ppu_frame_t *record;
	ppu_t *context;			/* for the picture a test compares with, drawn by this core */
	unsigned char *chr, *atlas;	/* the cartridge's patterns, and four bytes for each of theirs */
	unsigned chr_bytes;
	int chr_ram;
	unsigned short *cells, *sprite_cells, *plain_cell;
	unsigned char *plain_tile;	/* a tile of ones: the colour behind everything */
	unsigned int *sets;
	int sets_used, set_pages, set_colours;
	GLuint name;
	unsigned int *layer;		/* the layer the library's texture is, at the moment */
	unsigned char shown[2][64];	/* the sprites on the picture in front of the background and behind it, the last first */
	int shown_count[2];
	unsigned short lines[64];	/* a sprite's lines that are drawn, a bit each from its top: all but where it is a ninth */
	/* the frame drawn last, to know one that is the same: its runs, their pages and colours, the sprites */
	int was_runs, alike;
	ppu_lineinfo_t was_run[8];
	uint8 was_end[8], was_colours[8][32], was_oam[256];
	uint8 *was_pages[8][16];
} gd;

/* A tile of the cartridge's (16 bytes, two planes) as 64, a pixel each. */
static void
gd_tile(unsigned tile)
{
	static unsigned spread[16];	/* a nibble's bits, a byte each, the leftmost pixel first */
	const unsigned char *from = gd.chr + tile * 16;
	unsigned int *to = (unsigned int *)(gd.atlas + tile * 64);
	int row, n;

	if (!spread[1])
		for (n = 0; n < 16; n++)
			spread[n] = (n >> 3 & 1) | (n >> 2 & 1) << 8 | (n >> 1 & 1) << 16 | (n & 1) << 24;
	for (row = 0; row < 8; row++) {
		unsigned a = from[row], b = from[row + 8];

		to[2 * row] = spread[a >> 4] | spread[b >> 4] << 1;
		to[2 * row + 1] = spread[a & 15] | spread[b & 15] << 1;
	}
}

/* The cell of name table `table` at `at` from the table's bytes. The layer has the tables two
 * by two, 30 rows each, and under them two rows a pair for a table's rows 30 and 31: its
 * attribute bytes, which the PPU shows as tiles when a game scrolls into them. */
static void
gd_cell(int table, int at)
{
	const unsigned char *bytes = ppu_getpage(8 + table) + 0x2000 + table * 0x400;
	int cx = at & 31, cy = at >> 5;
	unsigned attribute = bytes[0x3C0 + (cy >> 2) * 8 + (cx >> 2)];
	unsigned bank = attribute >> ((cy & 2) << 1 | (cx & 2)) & 3;

	int row = cy < 30 ? (table >> 1) * 30 + cy : 60 + (table >> 1) * 2 + cy - 30;

	gd.cells[row * 64 + (table & 1) * 32 + cx] = (unsigned short)(bytes[at] | bank << 12);
}

/* What the game wrote since the last frame: the cells of those name table bytes (in every
 * table that is the same memory), and the tiles of pattern RAM. */
static int
gd_follow(void)
{
	uint16 written[256];
	unsigned char tiles[64];
	int count = ppu_written_cells(written, 256), n, table, at, changed = count != 0;

	if (count < 0) {
		for (table = 0; table < 4; table++)
			for (at = 0; at < 1024; at++)
				gd_cell(table, at);
	}
	for (n = 0; n < count; n++) {
		int home = (written[n] >> 10) & 3, offset = written[n] & 0x3FF;
		const unsigned char *memory = ppu_getpage(8 + home) + 0x2000 + home * 0x400;

		for (table = 0; table < 4; table++) {
			if (ppu_getpage(8 + table) + 0x2000 + table * 0x400 != memory)
				continue;
			gd_cell(table, offset);
			if (offset >= 0x3C0) {
				/* a byte of attributes: the sixteen cells it colours */
				int cx = (offset & 7) * 4, cy = ((offset - 0x3C0) >> 3) * 4, i;

				for (i = 0; i < 16; i++)
					gd_cell(table, (cy + (i >> 2)) * 32 + cx + (i & 3));
			}
		}
	}
	if (gd.chr_ram && ppu_written_tiles(tiles)) {
		changed = 1;
		for (n = 0; n < 512 && n < (int)(gd.chr_bytes / 16); n++)
			if (tiles[n >> 3] >> (n & 7) & 1)
				gd_tile(n);
	}
	return changed;
}

/* The set for a line's pages and colours: its palette (a bank of 16 a palette of the NES's
 * four colours, the background's then the sprites', and one for the colour behind), where
 * the tiles' eight pieces are, and the layers. The same as the last line's, mostly. */
static unsigned int *
gd_set(const ppu_lineinfo_t *line)
{
	unsigned int *set = gd.sets + SET_WORDS * (gd.sets_used ? gd.sets_used - 1 : 0);
	const unsigned char *colours;
	uint8 *const *pages;
	const unsigned int *rgb = seglPalette();	/* (the NES's 64 colours are the display palette's first) */
	unsigned sizes = 8 | 8 << 8 | 6u << 28;
	int n;

	if (gd.sets_used && ((line->pages == gd.set_pages && line->colours == gd.set_colours) || gd.sets_used == SETS_MOST))
		return set;
	set = gd.sets + SET_WORDS * gd.sets_used++;
	gd.set_pages = line->pages;
	gd.set_colours = line->colours;
	colours = ppu_frame_colours(gd.record, line->colours);
	pages = ppu_frame_pages(gd.record, line->pages);
	for (n = 0; n < 32; n++)
		set[(n >> 2) * 16 + (n & 3)] = rgb[colours[n] & 63];
	set[8 * 16 + 1] = rgb[colours[0] & 63];
	for (n = 0; n < 8; n++) {
		unsigned at = (unsigned)(pages[n] + n * 0x400 - gd.chr);

		set[144 + n] = seglAddress(at < gd.chr_bytes ? gd.atlas + at * 4 : gd.plain_tile);
	}
	set[144 + 8] = seglAddress(gd.plain_tile);
	/* the layers, four words each: the background with the first or the second 256 tiles, the sprites, the plain one */
	for (n = 0; n < 4; n++) {
		unsigned int *layer = set + 160 + 4 * n;

		layer[0] = seglAddress(n < 2 ? gd.cells : n == 2 ? gd.sprite_cells : gd.plain_cell);
		layer[1] = seglAddress(set + 144 + (n == 1 ? 4 : 0));
		layer[2] = seglAddress(set);
		layer[3] = sizes | (n == 3 ? 1u : 64u) << 16;
	}
	return set;
}

/* A rectangle of the picture (in the NES's pixels) from a layer, whose pixel (u, v) is at its top left. */
static void
gd_quad(unsigned int *layer, int layer_w, int layer_h, int x0, int y0, int x1, int y1, int u, int v, int keyed)
{
	GLfixed box[4];
	int texels[4];

	if (x1 <= x0 || y1 <= y0)
		return;
	if (layer != gd.layer) {
		glBindTexture(GL_TEXTURE_2D, gd.name);
		seglTexturePointer(layer, layer_w, layer_h, SEGL_TILES);
		gd.layer = layer;
	}
	box[0] = x0 * (ONE / WIDTH);
	box[1] = (y0 * ONE + HEIGHT / 2) / HEIGHT;
	box[2] = x1 * (ONE / WIDTH);
	box[3] = (y1 * ONE + HEIGHT / 2) / HEIGHT;
	/* (1,024ths of the layer: whole numbers, its sizes being powers of two) */
	texels[0] = u * (1024 / layer_w);
	texels[1] = v * (1024 / layer_h);
	texels[2] = (u + x1 - x0) * (1024 / layer_w);
	texels[3] = (v + y1 - y0) * (1024 / layer_h);
	seglSprite(box, texels, gd.name, 0xFFFFFF, keyed);
	gd_spent[4]++;
}

/* The sprites of lines y0 to y1 that are behind the background, or those in front: the
 * later ones first, so that the earlier are on top. */
static void
gd_sprites(unsigned int *set, const ppu_lineinfo_t *line, int y0, int y1, int behind)
{
	const unsigned char *oam = ppu_frame_oam(gd.record);
	int height = line->flags & PPU_LINE_TALL ? 16 : 8, left = line->flags & PPU_LINE_OBJMASK ? 8 : 0, k;

	for (k = 0; k < gd.shown_count[behind]; k++) {
		int n = gd.shown[behind][k], top = oam[4 * n] + 1, x = oam[4 * n + 3];
		int from = top > y0 ? top : y0, to = top + height < y1 ? top + height : y1;
		int x0 = x > left ? x : left, x1 = x + 8 < WIDTH ? x + 8 : WIDTH;

		/* (a quad for each stretch of its lines that are drawn: one, but where it is a ninth on a line) */
		while (from < to) {
			int until;

			while (from < to && !(gd.lines[n] >> (from - top) & 1))
				from++;
			for (until = from; until < to && (gd.lines[n] >> (until - top) & 1); until++)
				;
			gd_quad(set + 168, 512, 16, x0, from, x1, until, n * 8 + x0 - x, from - top, 1);
			from = until;
		}
	}
}

/* Whether so many words are the same in two places. */
static int
words_same(const void *a, const void *b, int words)
{
	const unsigned int *x = a, *y = b;

	while (words-- > 0)
		if (*x++ != *y++)
			return 0;
	return 1;
}

/* Whether the record is of a frame like the one drawn last (and keeps what it is, if not). */
static int
gd_same(const ppu_lineinfo_t *runs, const uint8 *ends, int count)
{
	const unsigned char *oam = ppu_frame_oam(gd.record);
	int same = count == gd.was_runs && count <= 8 && words_same(oam, gd.was_oam, 64), run;

	for (run = 0; run < count && same; run++)
		same = !memcmp(&runs[run], &gd.was_run[run], sizeof runs[run]) && ends[run] == gd.was_end[run]
		       && words_same(ppu_frame_colours(gd.record, runs[run].colours), gd.was_colours[run], 8)
		       && words_same(ppu_frame_pages(gd.record, runs[run].pages), gd.was_pages[run], 16);
	if (same)
		return 1;
	gd.was_runs = count;
	memcpy(gd.was_oam, oam, 256);
	for (run = 0; run < count && run < 8; run++) {
		gd.was_run[run] = runs[run];
		gd.was_end[run] = ends[run];
		memcpy(gd.was_colours[run], ppu_frame_colours(gd.record, runs[run].colours), 32);
		memcpy(gd.was_pages[run], ppu_frame_pages(gd.record, runs[run].pages), sizeof gd.was_pages[run]);
	}
	return 0;
}

/* The frame the record is of, as quads; and shown. `written`: the game wrote cells or tiles. */
static void
gd_draw(int written)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	const unsigned char *oam = ppu_frame_oam(gd.record);
	const ppu_lineinfo_t *runs;
	const uint8 *ends;
	int width, height, count = ppu_frame_runs(gd.record, &runs, &ends), run, start = 0, n, tall = 0, high = 0;

	/* a frame like the last is on the window already (but twice a second it is drawn all the
	 * same: the window may have been made, or uncovered, since) */
	gd_swap_at = cycles();
	if (gd_same(runs, ends, count) && !written && ++gd.alike < 30)
		return;
	gd.alike = 0;

	/* the sprites' cells: a tile each, or two for a tall one, mirrored as the sprite is */
	for (run = 0; run < count; run++) {
		if (runs[run].flags & PPU_LINE_OBJ) {
			tall = runs[run].flags & PPU_LINE_TALL;
			high = runs[run].flags & PPU_LINE_OBJHIGH;
			break;
		}
	}
	/* the PPU draws eight sprites a line, the first eight of them that are on it */
	{
		static unsigned char on_line[HEIGHT + 16];
		int sprites = 0, tall_by = tall ? 16 : 8, y;

		for (n = 0; n < 64; n++)
			sprites += oam[4 * n] < 239;
		for (n = 0; n < 64; n++)
			gd.lines[n] = 0xFFFF;
		if (sprites > 8) {
			memset(on_line, 0, sizeof on_line);
			for (n = 0; n < 64; n++) {
				if (oam[4 * n] >= 239)
					continue;
				for (y = 0; y < tall_by; y++)
					if (on_line[oam[4 * n] + 1 + y]++ >= 8)
						gd.lines[n] &= ~(1u << y);
			}
		}
	}
	gd.shown_count[0] = gd.shown_count[1] = 0;
	for (n = 63; n >= 0; n--) {
		unsigned tile = oam[4 * n + 1], how = oam[4 * n + 2];
		unsigned cell = (4 + (how & 3)) << 12 | (how & 0x40 ? 0x400 : 0) | (how & 0x80 ? 0x800 : 0);
		unsigned upper = tall ? (tile & 1) << 8 | (tile & 0xFE) : tile + (high ? 256 : 0);

		if (oam[4 * n] >= 239)
			continue;	/* (below the picture: where a game puts the ones it does not use) */
		gd.shown[how >> 5 & 1][gd.shown_count[how >> 5 & 1]++] = (unsigned char)n;
		gd.sprite_cells[n] = (unsigned short)(cell | (tall && (how & 0x80) ? upper + 1 : upper));
		gd.sprite_cells[64 + n] = (unsigned short)(cell | (how & 0x80 ? upper : upper + 1));
	}

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	gd.sets_used = 0;
	gd.layer = NULL;
	gd_spent[3] += count;
	for (run = 0; run < count; start = ends[run++]) {
		ppu_lineinfo_t first = runs[run];
		unsigned int *set = gd_set(&first);
		int y = ends[run];

		gd_quad(set + 172, 8, 8, 0, start, WIDTH, y, 0, 0, 0);
		if ((first.flags & (PPU_LINE_OBJ | PPU_LINE_SPRITES)) == (PPU_LINE_OBJ | PPU_LINE_SPRITES))
			gd_sprites(set, &first, start, y, 1);
		if (first.flags & PPU_LINE_BG) {
			int left = first.flags & PPU_LINE_BGMASK ? 8 : 0, lower = first.y >> 8, row = first.y & 255, line = start;
			unsigned int *layer = set + (first.flags & PPU_LINE_BGHIGH ? 164 : 160);

			/* down the tables as the PPU goes: from row 29 into the other pair's top, from 31 (the
			 * attribute bytes) to the same pair's */
			while (line < y) {
				int rows = (row < 240 ? 240 : 256) - row, from = row < 240 ? lower * 240 + row : 480 + lower * 16 + row - 240;

				if (rows > y - line)
					rows = y - line;
				gd_quad(layer, 512, 512, left, line, WIDTH, line + rows, first.x + left, from, 1);
				line += rows;
				if (row < 240)
					lower ^= 1;
				row = 0;
			}
		}
		if ((first.flags & (PPU_LINE_OBJ | PPU_LINE_SPRITES)) == (PPU_LINE_OBJ | PPU_LINE_SPRITES))
			gd_sprites(set, &first, start, y, 0);
	}
	gd_swap_at = cycles();
	seglSwap();
	frames_drawn++;
}

/* Before and after each frame: every frame is recorded and drawn. */
static void
frame_gpu(int after)
{
	int last = (frames_hold && frames_emulated + 1 >= frames_hold) || (frames_most && frames_emulated + 1 >= frames_most);

	static int drawn;

	if (!after) {
		/* (a test's last frame is drawn by this core too, which wants the record's copies of memory) */
		ppu_capture_copies = last;
		drawn = last || frames_emulated % gpu_every == 0;
		if (drawn)
			ppu_capture(gd.record);
		return;
	}
	frames_emulated++;
	if (drawn)
		ppu_capture_end();
	if (drawn) {
		unsigned t0 = cycles(), t1;
		int written = gd_follow();

		t1 = cycles();
		gd_draw(written);
		gd_spent[0] += t1 - t0;
		gd_spent[1] += gd_swap_at - t1;
		gd_spent[2] += cycles() - gd_swap_at;
	}
	if (last) {
		ppu_drawjob_t job = {gd.record, gd.context, picture, 0, HEIGHT};

		ppu_draw((uint32)&job, 0);
	}
	if (!(frames_emulated & 3))
		osd_getinput();
	frame_counted(frames_emulated, 1, picture);
	frame_paced();
}

/* Whether the GPU draws: the memory it takes (four bytes a byte of patterns), and a cartridge that can be recorded. */
static int
gpu_start(void)
{
	rominfo_t *rom = nes_getcontextptr()->rominfo;
	unsigned n;

	gd.chr_ram = rom->vram != NULL && rom->vrom_banks == 0;
	gd.chr = gd.chr_ram ? rom->vram : rom->vrom;
	gd.chr_bytes = gd.chr_ram ? 0x2000u * rom->vram_banks : 0x2000u * rom->vrom_banks;
	if (!gd.chr || !gd.chr_bytes || seglMemoryLeft() < gd.chr_bytes * 4 + 0x10000)
		return 0;
	gd.record = ppu_frame_create(rom->vram, rom->vram ? 0x2000 * rom->vram_banks : 0);
	gd.context = calloc(1, sizeof(ppu_t));
	gd.atlas = seglMemory(gd.chr_bytes * 4);
	gd.cells = seglMemory(64 * 64 * 2);
	gd.sprite_cells = seglMemory(64 * 2 * 2 + 16);
	gd.plain_tile = seglMemory(64 * 64);
	gd.sets = seglMemory(SETS_MOST * SET_WORDS * 4);
	if (!gd.record || !gd.context || !gd.sets || !ppu_capture(gd.record))
		return 0;
	ppu_capture_end();
	gd.plain_cell = gd.sprite_cells + 128;
	gd.plain_cell[0] = 8 << 12 | 512;	/* the ninth piece's first tile, in the ninth bank */
	memset(gd.plain_tile, 1, 64 * 64);
	memset(gd.cells, 0, 64 * 64 * 2);
	for (n = 0; n < gd.chr_bytes / 16; n++)
		gd_tile(n);
	glGenTextures(1, &gd.name);
	gd.was_runs = -1;
	if (getenv("NES_EVERY"))
		gpu_every = atoi(getenv("NES_EVERY")) > 0 ? atoi(getenv("NES_EVERY")) : 1;
	nes_frame_elsewhere = frame_gpu;
	fprintf(stderr, "nes: the picture is drawn by the GPU\n");
	return 1;
}

/* As many workers as the machine will give, of a size NES_SHAPE names (5: 3 KB of new stores
 * a pass; a line is 16 texels of the picture and a worker draws eight or so in a pass).
 * NES_WORKERS=N asks for fewer than all that fit. */
static int
drawers_open(void)
{
	static const int fit[7] = {0, 0, 0, 15, 15, 12, 7};	/* by size: how many the machine has room for */
	unsigned char sizes[MC_MAX_CORES];
	const char *most = getenv("NES_WORKERS"), *shape = getenv("NES_SHAPE");
	int size = shape ? atoi(shape) : 5, wanted, k;

	if (size < 3 || size > 6)
		size = 5;
	wanted = most ? atoi(most) : fit[size];
	if (wanted > fit[size])
		wanted = fit[size];
	for (k = 0; k < wanted; k++)
		sizes[k] = (unsigned char)size;
	/* (refused when another program has workers, or on a machine whose cores are as they are:
	 * then it is the workers there are) */
	if (wanted > 0)
		mcw_shape(sizes, wanted);
	return mcw_open(wanted);
}

/* Called once the cartridge is in: who draws, and their memory. */
static void
elsewhere_start(void)
{
	const char *how = getenv("NES_PPU");
	rominfo_t *rom = nes_getcontextptr()->rominfo;
	int k;

	if (how && !strcmp(how, "here"))
		return;
	/* the GPU, unless NES_PPU says who (workers, inline, here) or it cannot */
	if ((!how || !strcmp(how, "gpu")) && gpu_start())
		return;
	inline_draw = how && !strcmp(how, "inline");
	drawers = inline_draw ? 1 : drawers_open();
	if (drawers <= 0)
		return;
	if (!inline_draw)
		atexit(mcw_close);
	/* at most a team to two workers; a test has as many as NES_TEAMS says, or one */
	team_most = drawers / 2 > TEAMS_MOST ? TEAMS_MOST : drawers / 2 ? drawers / 2 : 1;
	if (getenv("NES_TEAMS") || unlimited) {
		team_wanted = getenv("NES_TEAMS") ? atoi(getenv("NES_TEAMS")) : 1;
		if (team_wanted < 1 || team_wanted > team_most)
			team_wanted = 1;
		team_most = team_wanted;
	}
	teams_of(team_wanted);
	/* a record and a picture a team, and one more: the frame being recorded, the picture on show */
	for (k = 0; k <= team_most; k++) {
		records[k] = ppu_frame_create(rom->vram, rom->vram ? 0x2000 * rom->vram_banks : 0);
		pictures[k] = k ? seglMemory(WIDTH * HEIGHT) : picture;
		if (!records[k] || !pictures[k]) {
			drawers = 0;
			return;
		}
		if (k) {
			memset(pictures[k], 0, WIDTH * HEIGHT);
			glGenTextures(1, &textures[k]);
			glBindTexture(GL_TEXTURE_2D, textures[k]);
			seglTexturePointer(pictures[k], WIDTH, HEIGHT, GL_COLOR_INDEX8_EXT);
		} else
			textures[0] = texture;
	}
	front = 0;
	for (k = 0; k < drawers; k++) {
		/* a drawer's own PPU: whole 16s of memory nobody else writes, there before it starts */
		jobs[k].context = inline_draw ? malloc(sizeof(ppu_t)) : mcw_alloc(sizeof(ppu_t));
		memset(jobs[k].context, 0, sizeof(ppu_t));
	}
	if (!inline_draw)
		mcw_touch(__DATA_BEGIN__, _end - __DATA_BEGIN__);
	if (!ppu_capture(records[0])) {
		drawers = 0;	/* (a cartridge whose pattern pages turn as they are drawn) */
		return;
	}
	ppu_capture_end();
	nes_frame_elsewhere = frame_elsewhere;
	if (inline_draw)
		fprintf(stderr, "nes: the picture is drawn by this core, from the frame's record\n");
	else
		fprintf(stderr, "nes: the picture is drawn by %d worker core%s, in up to %d team%s\n", drawers, drawers == 1 ? "" : "s", team_most, team_most == 1 ? "" : "s");
}

static viddriver_t driver = {
	"ShaderEmu", video_init, video_shutdown, video_mode, video_palette, video_clear,
	video_lock, video_free, video_show, false
};

void
osd_getvideoinfo(vidinfo_t *info)
{
	info->default_width = WIDTH;
	info->default_height = HEIGHT;
	info->driver = &driver;
}

/* ---- the pad: arrows, X for A, Z for B, Enter for Start, Tab for Select; Escape ends ---- */

static const int pad[8] = {
	event_joypad1_up, event_joypad1_down, event_joypad1_left, event_joypad1_right,
	event_joypad1_a, event_joypad1_b, event_joypad1_start, event_joypad1_select
};

static void
press(int button, int down)
{
	event_t handler = event_get(pad[button]);

	if (handler)
		handler(down ? INP_STATE_MAKE : INP_STATE_BREAK);
}

static int
button_of(int key)
{
	switch (key) {
	case MWKEY_UP: return 0;
	case MWKEY_DOWN: return 1;
	case MWKEY_LEFT: return 2;
	case MWKEY_RIGHT: return 3;
	case 'x': case 'X': return 4;
	case 'z': case 'Z': return 5;
	case MWKEY_ENTER: return 6;
	case MWKEY_TAB: return 7;
	}
	return -1;
}

/* Asked once a drawn frame. A key let go in the call it went down in stays down until the
 * next call: the game would never have seen it. */
void
osd_getinput(void)
{
	static unsigned let_go;
	unsigned went_down = 0;
	GR_EVENT event;
	int button;

	for (button = 0; button < 8; button++)
		if (let_go & 1u << button)
			press(button, 0);
	let_go = 0;
	/* a test's presses (NES_PRESS="300 start 420 a"): each at its frame, until the next call */
	while (script && *script) {
		static const char *const names[8] = {"up", "down", "left", "right", "a", "b", "start", "select"};
		char name[8];
		int frame, used = 0;

		if (sscanf(script, "%d %7s%n", &frame, name, &used) < 2 || frame > frames_emulated)
			break;
		script += used;
		for (button = 0; button < 8; button++)
			if (!strcmp(name, names[button])) {
				press(button, 1);
				let_go |= 1u << button;
			}
	}
	for (;;) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_NONE)
			return;
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			exit(0);
		if (event.type == GR_EVENT_TYPE_UPDATE && event.update.utype == GR_UPDATE_SIZE)
			seglWindowChanged();
		if (event.type != GR_EVENT_TYPE_KEY_DOWN && event.type != GR_EVENT_TYPE_KEY_UP)
			continue;
		if (event.keystroke.ch == MWKEY_ESCAPE)
			exit(0);
		if ((button = button_of(event.keystroke.ch)) < 0)
			continue;
		if (event.type == GR_EVENT_TYPE_KEY_DOWN) {
			press(button, 1);
			went_down |= 1u << button;
		} else if (went_down & 1u << button)
			let_go |= 1u << button;
		else
			press(button, 0);
	}
}

void
osd_getmouse(int *x, int *y, int *button)
{
}

/* ---- the rest of what nofrendo asks of a machine ---- */

static int
log_line(const char *text)
{
	return fputs(text, stderr);
}

int
osd_init(void)
{
	GR_SCREEN_INFO screen;
	int scale;

	log_chain_logfunc(log_line);
	if (GrOpen() < 0) {
		fprintf(stderr, "nes: no Nano-X server (start one: nano-X -p &)\n");
		return -1;
	}
	GrGetScreenInfo(&screen);
	for (scale = 3; scale > 1 && (WIDTH * scale + 8 > screen.cols || HEIGHT * scale + 60 > screen.rows); scale--)
		;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "NES", GR_ROOT_WINDOW_ID, -1, -1, WIDTH * scale, HEIGHT * scale, 0);
	GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	GrMapWindow(window);
	GrSetFocus(window);
	if (seglInit(window) < 0 || !(picture = seglMemory(WIDTH * HEIGHT))) {
		fprintf(stderr, "nes: this machine has no GPU\n");
		return -1;
	}
	memset(picture, 0, WIDTH * HEIGHT);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(picture, WIDTH, HEIGHT, GL_COLOR_INDEX8_EXT);
	cycles_then = cycles();
	ms_then = milliseconds();
	return 0;
}

void
osd_shutdown(void)
{
}

static char no_config[] = "/tmp/nes.cfg";

int
osd_main(int argc, char *argv[])
{
	config.filename = no_config;
	return main_loop("rom", system_autodetect);
}

void
osd_fullname(char *fullname, const char *shortname)
{
	strncpy(fullname, shortname, PATH_MAX);
}

char *
osd_newextension(char *string, char *ext)
{
	return string;
}

int
osd_makesnapname(char *filename, int len)
{
	return -1;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "nes FILE.nes   (NES_SKIP=N: draw one frame in N + 1; NES_FRAMES=N: end after N; NES_HOLD=N: stop there)\n");
		return 1;
	}
	rom_path = argv[1];
	if (getenv("NES_SKIP"))
		skip = atoi(getenv("NES_SKIP"));
	if (getenv("NES_FRAMES"))
		frames_most = atoi(getenv("NES_FRAMES"));
	script = getenv("NES_PRESS");
	if (getenv("NES_HOLD"))
		frames_hold = atoi(getenv("NES_HOLD"));
	unlimited = getenv("NES_FAST") != NULL || getenv("NES_FRAMES") != NULL;
	nes_sum_on = getenv("NES_SUM") != NULL;
	if (getenv("NES_PLAIN"))
		nes6502_plain = atoi(getenv("NES_PLAIN"));
	return nofrendo_main(argc, argv);
}
