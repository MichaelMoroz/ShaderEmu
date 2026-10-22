/*
 * Nofrendo's machine side on ShaderEmu (docs/games/nes.md): the NES's picture is bytes in GPU memory
 * that the GPU stretches over a Nano-X window through the display's palette, the pad is the
 * keyboard, and a frame is drawn for every few emulated (NES_SKIP), the machine being slow.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
#include <setjmp.h>
#include <GLES/segl.h>
#include "mcwx.h"
#define MWINCLUDECOLORS
#include "nano-X.h"

#define WIDTH NES_SCREEN_WIDTH
#define HEIGHT NES_VISIBLE_HEIGHT
#define ONE 65536

static const char *rom_path;
static char *rom_data;
static unsigned rom_bytes;
static GR_WINDOW_ID window;
static unsigned char *picture;		/* WIDTH x HEIGHT bytes of GPU memory */
static GLuint texture;
static void (*tick)(void);		/* nofrendo's 60 Hz timer: counts the frames owed */
static int skip = 2;			/* emulated frames not drawn for each one that is */
static int frames_most, frames_hold, frames_drawn, frames_emulated;
static int swap_waits;			/* NES_SWAP=wait: every list waited for, as it was */
static int hold_late;			/* NES_HOLD_LATE=1: NES_HOLD's frame stays while the next is made ready */
static int frames_mark;			/* NES_MARK=N: "nesmark" at frame N, for a profile to begin at */
static unsigned gd_spent[3];		/* instructions following the game's writes (the rest unused) */
/* what whoever makes the lists counts: their instructions, quads and runs of lines */
static unsigned gd_made[4] __attribute__((aligned(16)));
static int gd_missed;			/* frames not shown: the worker was at the one before */
static int gd_worker;			/* a worker core makes the GPU's lists (docs/games/nes.md) */
/* The frames on a worker core (docs/games/nes.md, "The emulation on a worker core"): what each side
 * says to the other, in 16 bytes or more that only it writes. */
static int emu_worker;			/* set on the worker that emulates */
static struct { volatile uint32_t seq, slot, written, frames; } emu_said __attribute__((aligned(16)));
static struct {
	volatile uint32_t held, quit, swapped, pad;	/* the keys down; the lists' worker to end; the list it made last is shown */
	volatile uint32_t downs[8];	/* key presses so far, a count a button */
	volatile uint32_t done;		/* the frame given out whose list is drawn, or needs none */
	volatile uint32_t drawn, swap_spent, pad2;	/* lists shown; the swaps' instructions */
} first_said __attribute__((aligned(16)));

static int gpu_every = 1;		/* the GPU draws a frame in so many: more than one when the game is late */
static const char *script;		/* NES_PRESS: frames and the buttons to press at them */
static unsigned cycles_then, ms_then;
extern void nes6502_recompile(uint8 *prg, int bytes, int fixed);
extern void nes6502_recompiled(unsigned *instructions, unsigned *bytes, unsigned *runs, unsigned *entries);
extern void nes6502_recompile_touch(void (*touch)(void *at, unsigned bytes), unsigned code);
extern void (*nes_frame_elsewhere)(int after);
extern int (*nes_run_elsewhere)(void (*frames)(void));
static int run_elsewhere(void (*frames)(void));
extern int nes_sum_on;			/* ours, in nofrendo's frame: NES_SUM, for tests */
static int cells_check, cells_checked, cells_wrong;	/* NES_CELLS_CHECK: frames checked, and with a difference */
extern uint32 nes_sum;	/* ours, in nofrendo's loop: see frame_elsewhere */
extern char __DATA_BEGIN__[], _end[];
static void window_draw(GLuint of, int wait);
static void emu_end(int how) __attribute__((noreturn));
static void frame_counted(int emulated, int step, const unsigned char *shown);
static void elsewhere_start(void);
static void frame_paced(void);
static int unlimited;			/* NES_FAST: as fast as it goes, for measuring */
extern int rc_plain_runs_off, rc_eager;
extern int ppu_lines_each;		/* ours, in nofrendo's PPU: a slice of the 6502 is one line */
extern int nes6502_plain;		/* ours, in nofrendo's 6502: idle loops run turn by turn */

static unsigned
cycles(void)
{
	unsigned n;

#ifdef __gpuvm__
	__asm__ volatile("mfsr %0, cycle" : "=r"(n));
#else
	__asm__ volatile("rdcycle %0" : "=r"(n));
#endif
	return n;
}

static unsigned
milliseconds(void)
{
	struct timeval now;

	if (picture)		/* (the machine's clock: a load, and no system call on a worker) */
		return *(const volatile unsigned *)((char *)seglPalette() - 0x400 + 0x34);
	gettimeofday(&now, NULL);
	return (unsigned)(now.tv_sec * 1000 + now.tv_usec / 1000);
}

/* ---- the cartridge: a file read whole ---- */

char *
osd_getromdata(void)
{
	FILE *file;
	long size;
	int fd;

	if (rom_data)
		return rom_data;
	/* mapped, so that only the pages a game comes to are read; read whole where that cannot be */
	if ((fd = open(rom_path, O_RDONLY)) >= 0) {
		struct stat is;
		void *at = fstat(fd, &is) ? MAP_FAILED : mmap(NULL, is.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);

		close(fd);
		if (at != MAP_FAILED) {
			rom_bytes = (unsigned)is.st_size;
			return rom_data = at;
		}
	}
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
	rom_bytes = (unsigned)size;
	return rom_data;
}

/* ---- the timer: no interrupt here, the frames owed are counted where a frame is shown ---- */

int
osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
	tick = (void (*)(void))func;
	return 0;
}

/* ---- sound: the card's voices, not nofrendo's samples (apu_shaderemu.c) ---- */

extern void se_apu_init(int workers);
extern void se_apu_look(void);
static int apu_worker;	/* recordings are decoded by a worker, if there is one */

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
	se_apu_init(apu_worker);
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
	window_draw(texture, 1);	/* (the next frame is copied into the picture it reads) */

	/* the frames to emulate before the next one is drawn */
	for (y = 0; y < skip + 1 && tick; y++)
		tick();
	frames_drawn++;
	frames_emulated = frames_drawn * (skip + 1);
	frame_counted(frames_emulated, skip + 1, picture);
}

/* The GPU draws a picture over the whole window. Without `wait` the list is only submitted:
 * for a picture that nobody writes while it is the one on show. */
static void
window_draw(GLuint of, int wait)
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
	if (wait || swap_waits)
		seglSwap();
	else
		seglSwapEarly();
}

/* The window's caption says the emulation's speed, a NES's 60 frames a second as 100%, about
 * once a second by the machine's clock. The first core's alone (its own 16 bytes): it is the
 * window system's. */
static struct { unsigned at_ms, at_frames, shown, pad; } caption __attribute__((aligned(16))) = {0, 0, ~0u, 0};

static void
speed_caption(unsigned frames)
{
	unsigned ms = milliseconds(), pct;
	char text[24];

	if (!caption.at_ms || frames < caption.at_frames) {
		caption.at_ms = ms | 1;
		caption.at_frames = frames;
		return;
	}
	if (ms - caption.at_ms < 1000)
		return;
	pct = (frames - caption.at_frames) * 100000u / 60 / (ms - caption.at_ms);
	caption.at_ms = ms | 1;
	caption.at_frames = frames;
	if (pct == caption.shown)
		return;
	caption.shown = pct;
	snprintf(text, sizeof text, "NES  %u%%", pct);
	GrSetWindowTitle(window, text);
	GrFlush();
}

/* After `step` more frames, `emulated` in all: the figures, and a test's end at `shown`. */
static void
frame_counted(int emulated, int step, const unsigned char *shown)
{
	static int since, drawn_then;
	static unsigned made_then[3], swaps_then;

	/* (lines by mcw_print: on the worker that emulates, written when it ends, or now) */
	if (frames_mark && emulated >= frames_mark) {
		frames_mark = 0;
		mcw_print("nesmark\n");
		mcw_print_now();
	}
	if (!emu_worker)
		speed_caption(emulated);
	since += step;
	if (since >= 60) {
		unsigned now = cycles(), ms = milliseconds(), drawn = frames_drawn + first_said.drawn, swaps = first_said.swap_spent;

		mcw_print("nesstat: %d frames of the NES in %u ms (%d drawn): %u thousand instructions each, %u%% of its speed\n",
			since, ms - ms_then, drawn - drawn_then, (now - cycles_then) / 1000 / since,
			ms > ms_then ? since * 100000 / 60 / (ms - ms_then) : 0);
		if (gd_made[0] != made_then[0])
			mcw_print("nesgpu: a frame's %u instructions to follow the game's writes, %u for %u quads in %u runs of lines%s, %u for the swap\n",
				gd_spent[0] / since, (gd_made[0] - made_then[0]) / since, (gd_made[1] - made_then[1]) / since,
				(gd_made[2] - made_then[2]) / since, gd_worker ? " (a worker core's)" : "", (swaps - swaps_then) / since);
		if (gd_missed)
			mcw_print("nesgpu: %d frames not shown, the worker being at the one before\n", gd_missed);
		/* (a few lines at a time: the console takes some 60 characters a pass, and a worker
		 * waits for its write) */
		mcw_print_now();
		gd_missed = 0;
		gd_spent[0] = 0;
		made_then[0] = gd_made[0], made_then[1] = gd_made[1], made_then[2] = gd_made[2];
		swaps_then = swaps;
		cycles_then = now;
		ms_then = ms;
		drawn_then = drawn;
		since = 0;
	}
	if (frames_hold && emulated >= frames_hold + hold_late) {
		GR_EVENT event;

		/* (for a test: the picture stays, and a snapshot has it in GPU memory) */
		if (emu_worker)
			while (first_said.done != emu_said.seq)
				mc_end_pass();
		else
			seglSettle();
		mcw_print("nes: holding, picture %d bytes past the palette\n", (int)(shown - (unsigned char *)seglPalette()));
		if (emu_worker)
			emu_end(2);
		do
			GrGetNextEvent(&event);
		while (event.type != GR_EVENT_TYPE_CLOSE_REQ);
		exit(0);
	}
	if (frames_most && emulated >= frames_most) {
		unsigned sum = 0;
		int i;

		/* (a sum of the last picture: what a change to the emulator must leave; four bytes a step) */
		for (i = 0; i < WIDTH * HEIGHT; i += 4)
			sum = sum * 923521 + shown[i] * 29791 + shown[i + 1] * 961 + shown[i + 2] * 31 + shown[i + 3];
		{
			unsigned instructions, bytes, runs, entries;

			nes6502_recompiled(&instructions, &bytes, &runs, &entries);
			mcw_print("nes: %u instructions of the 6502 translated in %u goes, into %u bytes; entered %u times\n",
				instructions, runs, bytes, entries);
		}
		if (nes_sum_on)
			mcw_print("nes: every frame's RAM %08x\n", nes_sum);
		if (cells_check)
			mcw_print("nes: cells and patterns as the PPU's memory: %d of %d frames differ\n", cells_wrong, cells_checked);
		mcw_print("nes: done, picture %08x\n", sum);
		if (emu_worker)
			emu_end(1);
		exit(0);
	}
}

/* ---- the picture drawn elsewhere (docs/games/nes.md): no frame is drawn by this core; of the
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
		window_draw(textures[front], 0);	/* (no team is given the picture on show) */
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

	if (2 == after)
		return;
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
		if ((int)(now - due) > 8 && gpu_every < 4 && !gd_worker)
			gpu_every++;
		else if ((int)(due - now) > 2 && gpu_every > 1)
			gpu_every--;
	}
	if ((int)(now - due) > 50 || (int)(due - now) > 200)
		due = now;
	else if (emu_worker) {
		while ((int)(due - *clock) > 0)
			mc_end_pass();
	} else {
		/* (the kernel's sleeps are whole hundredths of a second: the last of the wait is passes
		 * of the machine ended early, which the clock word counts) */
		if ((int)(due - now) > 20)
			usleep((due - now - 12) * 1000);
		while ((int)(due - *clock) > 0)
			mcw_rest();	/* (the kernel has the core meanwhile; with no worker open, the pause hint) */
	}
}

/* ---- the picture drawn by the GPU (docs/games/nes.md): the background is a layer of tiles
 * (docs/machine/gpu.md), the four name tables two by two as its cells; the sprites are another, a
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
	/* two of each: a list is made with the pair the list on show was not made with */
	unsigned int *sets_of[2];
	unsigned short *sprite_cells_of[2];
	int turn;
	int sets_used, set_pages, set_colours;
	GLuint name;
	unsigned int *layer;		/* the layer the library's texture is, at the moment */
	unsigned char shown[2][64];	/* the sprites on the picture in front of the background and behind it, the last first */
	int shown_count[2];
	unsigned short lines[64];	/* a sprite's lines that are drawn, a bit each from its top: all but where it is a ninth */
	/* the frame drawn last, to know one that is the same: its runs, their pages and colours, the sprites */
	int was_runs, alike;
	int set_w, set_h;		/* the window's size the library's matrices were set for */
	ppu_lineinfo_t was_run[8];
	uint8 was_end[8], was_colours[8][32], was_oam[256];
	uint8 *was_pages[8][16];
} __attribute__((aligned(16))) gd;

/* A tile of the cartridge's (16 bytes, two planes) as 64, a pixel each, into `atlas`. */
static void
gd_tile(unsigned char *atlas, unsigned tile)
{
	static unsigned spread[256][2];	/* a byte's bits, a byte each, the leftmost pixel first: four and four */
	const unsigned char *from = gd.chr + tile * 16;
	unsigned int *to = (unsigned int *)(atlas + tile * 64);
	int row, n;

	if (!spread[1][1])
		for (n = 0; n < 256; n++) {
			spread[n][0] = (n >> 7 & 1) | (n >> 6 & 1) << 8 | (n >> 5 & 1) << 16 | (n >> 4 & 1) << 24;
			spread[n][1] = (n >> 3 & 1) | (n >> 2 & 1) << 8 | (n >> 1 & 1) << 16 | (n & 1) << 24;
		}
	for (row = 0; row < 8; row++) {
		const unsigned *a = spread[from[row]], *b = spread[from[row + 8]];

		to[2 * row] = a[0] | b[0] << 1;
		to[2 * row + 1] = a[1] | b[1] << 1;
	}
}

/* The cells of the PPU's own four kilobytes of name tables, by where a byte is in that memory:
 * a cartridge moves the picture's four tables between them (some at every write to their
 * register), and a table that moves is copied from here, not worked out again. Kept by the
 * core that emulates: not in gd. The layer's cells and pattern RAM's patterns are COPIES. */
#define COPIES 3		/* a list made with one, another drawn, one made up (docs/games/nes.md) */
static struct {
	unsigned short cells[4][1024];
	int made;
	int of[COPIES][4];	/* which of them each of the picture's tables is; -1 other memory, -2 not known yet */
	unsigned short *layer[COPIES];	/* the copies */
	unsigned char *atlas[COPIES];
	unsigned int bits[COPIES][128];	/* name table bytes written since */
	unsigned int tiles[COPIES][16];	/* tiles of pattern RAM written since */
	unsigned int rows[COPIES][4];	/* the words of bits that are not zero, a bit each */
	int other[COPIES];		/* name tables that are other memory written since */
} gd_tables;

/* A row of a table's cells from its bytes: each its tile, and its palette from the attribute
 * byte over it (rows 30 and 31 are the attribute bytes themselves, shown as tiles). */
static void
gd_row_of(const unsigned char *bytes, int cy, unsigned short *to)
{
	const unsigned char *attributes = bytes + 0x3C0 + (cy >> 2) * 8, *tiles = bytes + cy * 32;
	int shift = (cy & 2) << 1, cx;

	for (cx = 0; cx < 32; cx += 2) {
		unsigned bank = (attributes[cx >> 2] >> (shift | (cx & 2)) & 3) << 12;

		to[cx] = (unsigned short)(tiles[cx] | bank);
		to[cx + 1] = (unsigned short)(tiles[cx + 1] | bank);
	}
}

/* Where the picture's table `table` has its row `cy`. The layer has the tables two by two,
 * 30 rows each, and under them two rows a pair for a table's rows 30 and 31: its attribute
 * bytes, which the PPU shows as tiles when a game scrolls into them. */
static unsigned short *
gd_row_at(unsigned short *cells, int table, int cy)
{
	int row = cy < 30 ? (table >> 1) * 30 + cy : 60 + (table >> 1) * 2 + cy - 30;

	return &cells[row * 64 + (table & 1) * 32];
}

/* Bytes of name table memory `memory` were written, a bit each in `written` (a word a row of
 * 32): their cells, and the sixteen an attribute byte colours, in the memory's own copy and
 * in every table of the picture that is that memory. A row with many is worked out whole. */
static void
gd_cells_written(int copy, int memory, const unsigned int *written)
{
	static const unsigned char bit_of[32] = {
		0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
		31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
	};
	const unsigned char *bytes = ppu_nametables() + memory * 0x400;
	unsigned short *to[4];
	unsigned int rows[32];
	int tables[4], count = 0, cy, k;

	for (k = 0; k < 4; k++)
		if (gd_tables.of[copy][k] == memory)
			tables[count++] = k;
	for (cy = 0; cy < 32; cy++)
		rows[cy] = written[cy];
	for (cy = 30; cy < 32; cy++) {
		unsigned int set = written[cy];

		while (set) {
			unsigned int low = set & -set;
			int j = (cy - 30) * 32 + bit_of[low * 0x077CB531u >> 27], r;

			set ^= low;
			for (r = 0; r < 4; r++)
				rows[(j >> 3) * 4 + r] |= 0xFu << ((j & 7) * 4);
		}
	}
	for (cy = 0; cy < 32; cy++) {
		unsigned short *own = gd_tables.cells[memory] + cy * 32;
		unsigned int set = rows[cy];
		int shift = (cy & 2) << 1, done = 0;

		if (!set)
			continue;
		for (k = 0; k < count; k++)
			to[k] = gd_row_at(gd_tables.layer[copy], tables[k], cy);
		while (set && done++ < 8) {
			unsigned int low = set & -set;
			int cx = bit_of[low * 0x077CB531u >> 27];
			unsigned short cell = (unsigned short)(bytes[cy * 32 + cx]
			    | (bytes[0x3C0 + (cy >> 2) * 8 + (cx >> 2)] >> (shift | (cx & 2)) & 3) << 12);

			set ^= low;
			own[cx] = cell;
			for (k = 0; k < count; k++)
				to[k][cx] = cell;
		}
		if (set) {
			gd_row_of(bytes, cy, own);
			for (k = 0; k < count; k++)
				memcpy(to[k], own, 64);
		}
	}
}

/* What the game wrote since copy `copy` was last made up: the cells of those name table bytes,
 * the tables it moved, and the tiles of pattern RAM. Whether the copy changed. */
static int
gd_follow(int copy)
{
	unsigned int *bits = gd_tables.bits[copy], *rows = gd_tables.rows[copy], *tiles = gd_tables.tiles[copy];
	unsigned short *layer = gd_tables.layer[copy];
	unsigned int fresh[128], fresh_rows[4], fresh_tiles[16], set;
	const unsigned char *own = ppu_nametables();
	int any = ppu_written_rows(fresh, fresh_rows), n, table, cy, changed, k;

	if (!gd_tables.made) {
		gd_tables.made = 1;
		for (table = 0; table < 4; table++) {
			for (k = 0; k < COPIES; k++)
				gd_tables.of[k][table] = -2;
			for (cy = 0; cy < 32; cy++)
				gd_row_of(own + table * 0x400, cy, gd_tables.cells[table] + cy * 32);
		}
		any = 0;
	}
	for (table = 0; table < 4 && any; table++)
		for (set = fresh_rows[table]; set; set &= set - 1)
			for (n = table * 32 + __builtin_ctz(set), k = 0; k < COPIES; k++) {
				gd_tables.bits[k][n] |= fresh[n];
				gd_tables.rows[k][table] |= 1u << (n & 31);
			}
	/* (a write to name tables that are other memory: those are worked out again, in every copy) */
	for (k = 0; k < COPIES && any < 0; k++)
		gd_tables.other[k] = 1;
	changed = any = (rows[0] | rows[1] | rows[2] | rows[3]) != 0 || gd_tables.other[copy];
	gd_tables.other[copy] = 0;
	for (table = 0; table < 4; table++) {
		if (!rows[table])
			continue;
		gd_cells_written(copy, table, bits + table * 32);
		for (; rows[table]; rows[table] &= rows[table] - 1)
			bits[table * 32 + __builtin_ctz(rows[table])] = 0;
	}
	for (table = 0; table < 4; table++) {
		const unsigned char *bytes = ppu_getpage(8 + table) + 0x2000 + table * 0x400;
		unsigned from = (unsigned)(bytes - own);
		int memory = from < 0x1000 && !(from & 0x3FF) ? (int)(from >> 10) : -1;

		/* (a table that is other memory, a cartridge's own: worked out whenever anything was written) */
		if (memory == gd_tables.of[copy][table] && !(memory < 0 && any))
			continue;
		gd_tables.of[copy][table] = memory;
		changed = 1;
		for (cy = 0; cy < 32; cy++) {
			if (memory < 0)
				gd_row_of(bytes, cy, gd_row_at(layer, table, cy));
			else
				memcpy(gd_row_at(layer, table, cy), gd_tables.cells[memory] + cy * 32, 64);
		}
	}
	/* (tiles a bit each, 32 to a word: tile 32 n + b is word n's bit b) */
	if (gd.chr_ram) {
		if (ppu_written_tiles((unsigned char *)fresh_tiles))
			for (n = 0; n < 16; n++)
				for (k = 0; k < COPIES && fresh_tiles[n]; k++)
					gd_tables.tiles[k][n] |= fresh_tiles[n];
		for (n = 0; n < 16 && n * 32 < (int)(gd.chr_bytes / 16); n++) {
			if (!tiles[n])
				continue;
			changed = 1;
			for (set = tiles[n]; set; set &= set - 1)
				gd_tile(gd_tables.atlas[copy], n * 32 + __builtin_ctz(set));
			tiles[n] = 0;
		}
	}
	return changed;
}

/* NES_CELLS_CHECK=1: a copy just made up against the name tables and pattern RAM worked out
 * whole (cells_check, at the top). */

static void
gd_check(int copy)
{
	static unsigned char tile[64] __attribute__((aligned(16)));
	unsigned short row[32];
	int table, cy, n, wrong = 0;

	for (table = 0; table < 4; table++)
		for (cy = 0; cy < 32; cy++) {
			gd_row_of(ppu_getpage(8 + table) + 0x2000 + table * 0x400, cy, row);
			wrong |= memcmp(row, gd_row_at(gd_tables.layer[copy], table, cy), 64) != 0;
		}
	for (n = 0; gd.chr_ram && n < (int)(gd.chr_bytes / 16); n++) {
		gd_tile(tile - n * 64, (unsigned)n);
		wrong |= memcmp(tile, gd_tables.atlas[copy] + n * 64, 64) != 0;
	}
	cells_checked++;
	cells_wrong += wrong;
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
	gd_made[1]++;
}

/* The sprites of lines y0 to y1 that are behind the background, or those in front: the
 * later ones first, so that the earlier are on top. */
static void
gd_sprites(unsigned int *set, const ppu_lineinfo_t *line, int y0, int y1, int behind)
{
	/* a pixel's place as the library's number (a worker core's to write: whole 16 bytes, docs/machine/multicore.md) */
	static unsigned across[WIDTH + 4] __attribute__((aligned(16))), down[HEIGHT + 4] __attribute__((aligned(16)));
	const unsigned char *restrict oam = ppu_frame_oam(gd.record), *restrict shown = gd.shown[behind];
	const unsigned short *restrict all_lines = gd.lines;
	int height = line->flags & PPU_LINE_TALL ? 16 : 8, left = line->flags & PPU_LINE_OBJMASK ? 8 : 0, k, room, quads = 0;
	int count = gd.shown_count[behind];
	unsigned int *restrict to = seglSpriteSpace(&room);

	if (!count || !to)
		return;
	if (!down[HEIGHT]) {
		for (k = 0; k <= WIDTH; k++)
			across[k] = seglNumber(k * (ONE / WIDTH));
		for (k = 0; k <= HEIGHT; k++)
			down[k] = seglNumber((k * ONE + HEIGHT / 2) / HEIGHT);
	}
	/* (the counts and tables in registers: through gd each store of a quad was taken to change them) */
	for (k = 0; k < count; k++) {
		int n = shown[k], top = oam[4 * n] + 1, x = oam[4 * n + 3];
		int from = top > y0 ? top : y0, end = top + height < y1 ? top + height : y1;
		int x0 = x > left ? x : left, x1 = x + 8 < WIDTH ? x + 8 : WIDTH;
		unsigned u0 = (unsigned)(n * 8 + x0 - x) * 2, u1 = u0 + (unsigned)(x1 - x0) * 2, lines = all_lines[n];

		/* (a quad for each stretch of its lines that are drawn: one, but where it is a ninth on a line) */
		while (from < end && x0 < x1 && quads < room) {
			int until = end;
			unsigned v0, v1;

			if (lines != 0xFFFF) {
				while (from < end && !(lines >> (from - top) & 1))
					from++;
				for (until = from; until < end && (lines >> (until - top) & 1); until++)
					;
				if (from == until)
					break;
			}
			v0 = (unsigned)(from - top) << 22;
			v1 = (unsigned)(until - top) << 22;
			to[0] = across[x0], to[1] = down[from], to[2] = 0, to[3] = u0 | v0;
			to[4] = across[x1], to[5] = down[from], to[6] = 0, to[7] = u1 | v0;
			to[8] = across[x1], to[9] = down[until], to[10] = 0, to[11] = u1 | v1;
			to[12] = across[x0], to[13] = down[until], to[14] = 0, to[15] = u0 | v1;
			to += 16;
			quads++;
			from = until;
		}
	}
	if (!quads)
		return;
	if (set + 168 != gd.layer) {
		glBindTexture(GL_TEXTURE_2D, gd.name);
		seglTexturePointer(set + 168, 512, 16, SEGL_TILES);
		gd.layer = set + 168;
	}
	seglSpritesUsed(quads, gd.name, 0xFFFFFF, 1);
	gd_made[1] += quads;
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

/* The frame `record` is of, as quads: whether there is a list to show (none for a frame
 * like the last). `written`: the game wrote cells or tiles. No system call: a worker's job. */
static uint32_t
gd_draw(uint32_t written, uint32_t record)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	const unsigned char *oam = ppu_frame_oam(gd.record = (ppu_frame_t *)record);
	const ppu_lineinfo_t *runs;
	const uint8 *ends;
	int width, height, count = ppu_frame_runs(gd.record, &runs, &ends), run, start = 0, n, tall = 0, high = 0, crowded = 0;
	unsigned char rows[36];		/* how many sprites begin in each eight lines */
	unsigned char busy[36];		/* whether a line of each eight may have more than eight sprites */

	/* a frame like the last is on the window already (but twice a second it is drawn all the
	 * same: the window may have been made, or uncovered, since) */
	unsigned began = cycles();

	if (gd_same(runs, ends, count) && !written && ++gd.alike < 30)
		return 0;
	gd.alike = 0;
	gd.turn ^= 1;
	gd.sets = gd.sets_of[gd.turn];
	gd.sprite_cells = gd.sprite_cells_of[gd.turn];

	for (run = 0; run < count; run++) {
		if (runs[run].flags & PPU_LINE_OBJ) {
			tall = runs[run].flags & PPU_LINE_TALL;
			high = runs[run].flags & PPU_LINE_OBJHIGH;
			break;
		}
	}
	/* the sprites' cells: a tile each, or two for a tall one, mirrored as the sprite is */
	memset(rows, 0, sizeof rows);
	memset(busy, 0, sizeof busy);
	{
		/* (counts and tables in registers: through gd each store was taken to change the others) */
		unsigned short *restrict cells = gd.sprite_cells, *restrict lines = gd.lines;
		unsigned char *restrict shown[2] = {gd.shown[0], gd.shown[1]};
		int counts[2] = {0, 0};

		for (n = 63; n >= 0; n--) {
			unsigned y = oam[4 * n], tile = oam[4 * n + 1], how = oam[4 * n + 2], upper, cell, side;

			if (y >= 239)
				continue;	/* (below the picture: where a game puts the ones it does not use) */
			cell = (4 + (how & 3)) << 12 | (how & 0x40 ? 0x400 : 0) | (how & 0x80 ? 0x800 : 0);
			upper = tall ? (tile & 1) << 8 | (tile & 0xFE) : tile + (high ? 256 : 0);
			side = how >> 5 & 1;
			shown[side][counts[side]++] = (unsigned char)n;
			cells[n] = (unsigned short)(cell | (tall && (how & 0x80) ? upper + 1 : upper));
			cells[64 + n] = (unsigned short)(cell | (how & 0x80 ? upper : upper + 1));
			lines[n] = 0xFFFF;
			rows[(y + 1) >> 3]++;
		}
		gd.shown_count[0] = counts[0];
		gd.shown_count[1] = counts[1];
	}
	/* the PPU draws eight sprites a line, the first eight of them that are on it: looked for
	 * only where more than eight begin within a sprite's height of each other */
	for (n = 0; n < 32; n++) {
		busy[n] = rows[n] + (n ? rows[n - 1] : 0) + (tall && n > 1 ? rows[n - 2] : 0) > 8;
		crowded |= busy[n];
	}
	if (crowded) {
		/* (the sprites in a band that may have more than eight counted a line at a time as they
		 * begin and end; only a line with more is walked in their order, and the ninth on loses it) */
		signed char change[HEIGHT + 20];
		int tall_by = tall ? 16 : 8, y, on = 0, lowest = HEIGHT + 20, highest = 0;

		memset(change, 0, sizeof change);
		for (n = 0; n < 64; n++) {
			int top = oam[4 * n] + 1, band = top >> 3;

			if (oam[4 * n] >= 239 || !(busy[band] | busy[band + 1] | (tall ? busy[band + 2] : 0)))
				continue;
			change[top]++;
			change[top + tall_by]--;
			lowest = top < lowest ? top : lowest;
			highest = top + tall_by > highest ? top + tall_by : highest;
		}
		for (y = lowest; y < highest; y++) {
			int seen = 0;

			if ((on += change[y]) <= 8)
				continue;
			for (n = 0; n < 64; n++) {
				int top = oam[4 * n] + 1;

				if (oam[4 * n] < 239 && y >= top && y < top + tall_by && seen++ >= 8)
					gd.lines[n] &= ~(1u << (y - top));
			}
		}
	}

	seglSize(&width, &height);
	if (width != gd.set_w || height != gd.set_h) {
		gd.set_w = width;
		gd.set_h = height;
		glViewport(0, 0, width, height);
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixx(whole);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glDisable(GL_DEPTH_TEST);
	}
	gd.sets_used = 0;
	gd.layer = NULL;
	gd_made[2] += count;
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
	gd_made[0] += cycles() - began;
	return 1;
}

/* The lists are a worker core's to make when there is one: this core goes on with the next
 * frame, and shows a list when it finds it made (in the middle of the next frame, mostly).
 * Two records, the worker's and the one the PPU writes (COPIES while a worker emulates). */
static ppu_frame_t *gd_records[COPIES];
static int gd_with, gd_taking, gd_written;
static mcw_handle gd_call;		/* the list being made (mcw_call) */

static void
gd_swap(void)
{
	unsigned t = cycles();

	/* not waited for: the next frame's emulation runs while it is drawn. The next list has
	 * sets and sprite cells of its own, and the cells and tiles settle first (frame_gpu) */
	if (swap_waits)
		seglSwap();
	else
		seglSwapEarly();
	first_said.swap_spent += cycles() - t;
	first_said.drawn++;
}

/* The list being made, shown if it is made (with `wait`, once it is). */
static void
gd_show(int wait)
{
	if (!gd_call || (!wait && !mcw_ready(gd_call)))
		return;
	if (mcw_join(gd_call))
		gd_swap();
	gd_call = 0;
}

static void pad_away(void);
static uint32_t given[COPIES];	/* the frame last given out with each copy (emulating worker's) */
static int gives;

/* Before a frame on the worker that emulates: it is recorded when the copy it would have is
 * free, that is when the list made with it last is drawn (the last frame waits for that). */
static void
frame_before_away(int last, int *drawn)
{
	int c = gives % COPIES;

	while (last && (int)(first_said.done - given[c]) < 0)
		mc_end_pass();
	if (!*drawn || (int)(first_said.done - given[c]) < 0) {
		gd_missed += *drawn;
		*drawn = 0;
		return;
	}
	gd_taking = c;
	ppu_capture(gd_records[c]);
}

/* After it: the copy made up to the frame and the frame given out (lists_job, lists_step). */
static void
frame_after_away(int last, int drawn)
{
	if (drawn) {
		unsigned t0 = cycles();

		ppu_capture_end();
		gd_written += gd_follow(gd_taking);
		if (cells_check)
			gd_check(gd_taking);
		if (hold_late && frames_hold && frames_emulated > frames_hold)
			frame_counted(frames_emulated, 0, picture);
		gd_with = gd_taking;
		given[gd_with] = (uint32_t)frames_emulated;
		gives++;
		emu_said.slot = (uint32_t)gd_with;
		emu_said.written = (uint32_t)gd_written;
		emu_said.seq = (uint32_t)frames_emulated;
		gd_spent[0] += cycles() - t0;
	}
	emu_said.frames = (uint32_t)frames_emulated;
	if (last) {
		ppu_drawjob_t job = {gd_records[gd_with], gd.context, picture, 0, HEIGHT};

		while (first_said.done != emu_said.seq)
			mc_end_pass();
		ppu_draw((uint32)&job, 0);
	}
	if (!(frames_emulated & 3))
		pad_away();
	frame_counted(frames_emulated, 1, picture);
	frame_paced();
	/* (the kernel wants this core: the frames go on in another job, run_elsewhere) */
	if (mcw_recalled(mcw_self()))
		emu_end(3);
}

/* Before and after each frame: every frame is recorded and drawn. */
static void
frame_gpu(int after)
{
	int last = (frames_hold && frames_emulated + 1 >= frames_hold) || (frames_most && frames_emulated + 1 >= frames_most);

	static int drawn;

	if (2 == after) {
		if (!emu_worker)
			gd_show(0);
		return;
	}
	if (!after) {
		/* (a test's last frame is drawn by this core too, which wants the record's copies of memory) */
		ppu_capture_copies = last;
		drawn = last || frames_emulated % gpu_every == 0;
		if (emu_worker)
			frame_before_away(last, &drawn);
		else if (drawn) {
			gd_taking = 1 - gd_with;
			ppu_capture(gd_records[gd_taking]);
		}
		return;
	}
	frames_emulated++;
	if (emu_worker) {
		frame_after_away(last, drawn);
		return;
	}
	if (drawn) {
		unsigned t0 = cycles();

		ppu_capture_end();
		seglSettle();
		gd_written |= gd_follow(0);
		if (cells_check)
			gd_check(0);
		gd_spent[0] += cycles() - t0;
		gd_show(last || !gd_worker);
		/* (a frame whose turn comes while the worker is at the one before is not shown) */
		gd_missed += gd_call != 0;
		if (!gd_call) {
			if (hold_late && frames_hold && frames_emulated > frames_hold)
				frame_counted(frames_emulated, 0, picture);
			gd_with = gd_taking;
			if (gd_worker && !last)
				gd_call = mcw_call(gd_draw, (uint32_t)gd_written, (uint32_t)gd_records[gd_with]);
			else if (gd_draw((uint32_t)gd_written, (uint32_t)gd_records[gd_with]))
				gd_swap();
			gd_written = 0;
		}
	}
	if (last) {
		ppu_drawjob_t job = {gd_records[gd_with], gd.context, picture, 0, HEIGHT};

		ppu_draw((uint32)&job, 0);
	}
	if (!(frames_emulated & 3))
		osd_getinput();
	frame_counted(frames_emulated, 1, picture);
	frame_paced();
}

/* ---- the emulation on a worker core (docs/games/nes.md): another makes the lists of the frames it
 * gives out, this core shows them, reads the window's events for the pad, and helps the two on
 * (pages, system calls) ---- */

static jmp_buf emu_jump;
static int emu_how;		/* how the frames ended: 1 done, 2 holding, 3 the core asked back */
extern unsigned mcw_faults, mcw_calls;

static void
emu_end(int how)
{
	emu_how = how;
	longjmp(emu_jump, 1);
}

/* The job: the frames, on a worker. */
static uint32_t
emulate(uint32_t frames, uint32_t unused)
{
	(void)unused;
	emu_worker = 1;
	if (!setjmp(emu_jump))
		((void (*)(void))frames)();
	return 0;
}

/* The lists of the frames given out: made by another worker (lists_job) or, without one, by
 * this core (lists_step); shown by this core, which alone uses the window system, and the frame
 * said done when its list is drawn (or it needs none). */
static struct { uint32_t made, written, showing, shown, waits, swapped, pad[2]; } listing __attribute__((aligned(16)));
/* what the lists' worker says, its own 16 bytes: the frame whose list it made last, whether
 * there is one to show, the count of written frames it was made at; passes it waited */
static struct { volatile uint32_t made, result, written, waits; } lists_said __attribute__((aligned(16)));

static uint32_t
lists_job(uint32_t unused0, uint32_t unused1)
{
	(void)unused0, (void)unused1;
	for (;;) {
		uint32_t seq = emu_said.seq, copy = emu_said.slot, now = emu_said.written;

		/* (a list made is shown by the first core before the library makes the next) */
		if (lists_said.made == first_said.swapped && seq != lists_said.made) {
			gd.cells = gd_tables.layer[copy];
			gd.atlas = gd_tables.atlas[copy];
			lists_said.result = gd_draw(now != lists_said.written, (uint32_t)gd_records[copy]);
			lists_said.written = now;
			lists_said.made = seq;
		} else if (lists_said.made == first_said.swapped) {
			if (first_said.quit)
				return 0;
			if (mcw_recalled(mcw_self()))
				return 1;
		}
		lists_said.waits++;
		mc_end_pass();
	}
}

/* At every look of this core: the list on show said drawn, a list the worker made shown, or
 * (`here`, without one) a list made here. */
static void
lists_step(int here)
{
	const volatile unsigned *regs = (const volatile unsigned *)((char *)seglPalette() - 0x400);
	uint32_t seq = emu_said.seq, copy = emu_said.slot, now = emu_said.written;

	if (listing.showing) {
		if (regs[0x10 / 4])
			return;
		listing.showing = 0;
		first_said.done = listing.shown;
	}
	if (!here) {
		if (lists_said.made == listing.swapped)
			return;
		listing.swapped = lists_said.made;
		if (lists_said.result) {
			gd_swap();
			listing.showing = 1;
			listing.shown = listing.swapped;
		} else
			first_said.done = listing.swapped;
		first_said.swapped = listing.swapped;
		return;
	}
	if (seq == listing.swapped)
		return;
	listing.swapped = first_said.swapped = seq;
	gd.cells = gd_tables.layer[copy];
	gd.atlas = gd_tables.atlas[copy];
	if (gd_draw(now != lists_said.written, (uint32_t)gd_records[copy])) {
		gd_swap();
		listing.showing = 1;
		listing.shown = seq;
	} else
		first_said.done = seq;
	lists_said.written = now;
}

static void pad_here(void);

/* The frames on this core after all (no worker for them), as without workers. */
static void
emulate_here(mcw_handle lists, void (*frames)(void))
{
	const volatile unsigned *regs = (const volatile unsigned *)((char *)seglPalette() - 0x400);

	if (lists) {
		first_said.quit = 1;
		mcw_join(lists);
		lists_step(0);
	}
	while (regs[0x10 / 4])
		mcw_rest();
	gd_worker = 0;
	emu_worker = 0;
	gd_with = 0;
	gd.cells = gd_tables.layer[0];
	gd.atlas = gd_tables.atlas[0];
	gd_written = 1;
	frames();
	exit(0);
}

static int
run_elsewhere(void (*frames)(void))
{
	const char *where = getenv("NES_EMU");
	mcw_handle emu, lists;
	uint32_t pads = 0;

	if (!gd_worker || nes_frame_elsewhere != frame_gpu || (where && !strcmp(where, "here")))
		return 0;
	/* (the cartridge read here first, and the translator's memory written: a worker stops at a
	 * page nobody has had) */
	mcw_touch_code(rom_data, rom_bytes);
	nes6502_recompile_touch(mcw_touch, 0x40000);
	if (!(emu = mcw_try_call(emulate, (uint32_t)frames, 0)))
		return 0;
	lists = where && !strcmp(where, "lists-here") ? 0 : mcw_try_call(lists_job, 0, 0);
	for (;;) {
		if (mcw_ready(emu)) {
			mcw_join(emu);
			if (3 != emu_how)
				break;
			/* (the kernel wanted its core back: the frames go on on another, or here) */
			emu_how = 0;
			emu = mcw_try_call(emulate, (uint32_t)frames, 0);
			mcw_print("nes: the kernel asked for the core that emulates: %s\n", emu ? "on another" : "on this one from here");
			if (!emu)
				emulate_here(lists, frames);
			continue;
		}
		/* (the lists' worker, helped on at a page; its job ends when the kernel wants its core) */
		if (lists && mcw_ready(lists)) {
			mcw_join(lists);
			lists_step(0);
			lists = mcw_try_call(lists_job, 0, 0);
			mcw_print("nes: the kernel asked for the core that makes the lists: %s\n", lists ? "on another" : "made here");
		}
		lists_step(!lists);
		/* (the window's events as often as the emulation asked for them) */
		if (emu_said.frames >> 2 != pads) {
			pads = emu_said.frames >> 2;
			pad_here();
		}
		se_apu_look();
		speed_caption(emu_said.frames);
		mcw_rest();
		listing.waits++;
	}
	first_said.quit = 1;
	if (lists)
		mcw_join(lists);
	fprintf(stderr, "nes: the frames on a worker core%s: %u pages brought for workers, %u system calls made for them; this core looked on in %u passes, the lists' worker waited in %u\n",
		lists ? ", the lists on another" : "", mcw_faults, mcw_calls, listing.waits, lists_said.waits);
	if (2 == emu_how) {
		GR_EVENT event;

		do
			GrGetNextEvent(&event);
		while (event.type != GR_EVENT_TYPE_CLOSE_REQ);
	}
	exit(0);
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
	if (!gd.chr || !gd.chr_bytes || seglMemoryLeft() < gd.chr_bytes * (gd.chr_ram ? 4 * COPIES : 4) + 0x10000)
		return 0;
	gd.record = gd_records[0] = ppu_frame_create(rom->vram, rom->vram ? 0x2000 * rom->vram_banks : 0);
	gd.context = calloc(1, sizeof(ppu_t));
	/* (copies of the cells, and of pattern RAM's patterns: gd_tables) */
	for (n = 0; n < COPIES; n++) {
		if (n)
			gd_records[n] = ppu_frame_create(rom->vram, rom->vram ? 0x2000 * rom->vram_banks : 0);
		gd_tables.atlas[n] = n && !gd.chr_ram ? gd_tables.atlas[0] : seglMemory(gd.chr_bytes * 4);
		gd_tables.layer[n] = seglMemory(64 * 64 * 2);
		if (!gd_records[n] || !gd_tables.atlas[n] || !gd_tables.layer[n])
			return 0;
		memset(gd_tables.layer[n], 0, 64 * 64 * 2);
	}
	gd.atlas = gd_tables.atlas[0];
	gd.cells = gd_tables.layer[0];
	gd.sprite_cells = gd.sprite_cells_of[0] = seglMemory(64 * 2 * 2 + 16);
	gd.sprite_cells_of[1] = seglMemory(64 * 2 * 2);
	gd.plain_tile = seglMemory(64 * 64);
	gd.sets = gd.sets_of[0] = seglMemory(SETS_MOST * SET_WORDS * 4);
	gd.sets_of[1] = seglMemory(SETS_MOST * SET_WORDS * 4);
	if (!gd.context || !gd.sets_of[1] || !gd.sprite_cells_of[1] || !ppu_capture(gd.record))
		return 0;
	ppu_capture_end();
	gd.plain_cell = gd.sprite_cells + 128;
	gd.plain_cell[0] = 8 << 12 | 512;	/* the ninth piece's first tile, in the ninth bank */
	memset(gd.plain_tile, 1, 64 * 64);
	for (n = 0; n < gd.chr_bytes / 16; n++) {
		int k;

		for (k = 0; k < (gd.chr_ram ? COPIES : 1); k++)
			gd_tile(gd_tables.atlas[k], n);
	}
	glGenTextures(1, &gd.name);
	gd.was_runs = -1;
	if (getenv("NES_EVERY"))
		gpu_every = atoi(getenv("NES_EVERY")) > 0 ? atoi(getenv("NES_EVERY")) : 1;
	/* the lists from a worker core, if there is one: NES_LIST=inline for none, and a test
	 * (which runs as fast as it goes) has one only with NES_LIST=worker */
	{
		const char *list = getenv("NES_LIST");

		if (list ? !strcmp(list, "worker") : !unlimited) {
			/* the emulation, the lists and the sound's recordings (apu_shaderemu.c), as calls */
			gd_worker = mcw_open(3) > 0;
			apu_worker = gd_worker;
			if (gd_worker) {
				atexit(mcw_close);
				mcw_touch(__DATA_BEGIN__, _end - __DATA_BEGIN__);
			}
		}
	}
	nes_frame_elsewhere = frame_gpu;
	nes_run_elsewhere = run_elsewhere;
	fprintf(stderr, "nes: the picture is drawn by the GPU%s\n", gd_worker ? ", from lists a worker core makes" : "");
	return 1;
}

/* Twelve workers for the picture, or NES_WORKERS of them: this program's share of the machine's
 * (a line is 16 texels of the picture, and a worker draws some six of them in a pass). */
static int
drawers_open(void)
{
	const char *most = getenv("NES_WORKERS");
	int wanted = most ? atoi(most) : 12;

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

/*
 * Asking the window system for events is two system calls and two task switches. The machine's
 * own input words (docs/machine/input.md: pointer, buttons, key events so far) say when there can be
 * any: it is asked when they have moved, a few times after, and twice a second.
 */
static struct { unsigned seen[4]; int ask, quiet, pad[2]; } inputs __attribute__((aligned(16))) = {{0}, 8, 0, {0}};

static int
input_due(void)
{
	const volatile unsigned *input = (const volatile unsigned *)((char *)seglPalette() - 0x400 + 0x20);
	int i;

	for (i = 0; i < 4; i++)
		if (inputs.seen[i] != input[i]) {
			inputs.seen[i] = input[i];
			inputs.ask = 8;
		}
	if (++inputs.quiet >= 8)
		inputs.ask = inputs.ask ? inputs.ask : 1;
	if (!inputs.ask)
		return 0;
	inputs.ask--;
	inputs.quiet = 0;
	return 1;
}

/* A test's presses (NES_PRESS="300 start 420 a"): each at its frame, until the next look. */
static void
pad_script(unsigned *let_go)
{
	static const char *const names[8] = {"up", "down", "left", "right", "a", "b", "start", "select"};
	static char name[8];
	static int frame = -1, used;	/* the next press, read once */
	int button;

	while (script && *script) {
		if (frame < 0 && sscanf(script, "%d %7s%n", &frame, name, &used) < 2) {
			script = NULL;
			break;
		}
		if (frame > frames_emulated)
			break;
		script += used;
		frame = -1;
		/* ("+right" holds it down from there, "-right" lets it go) */
		for (button = 0; button < 8; button++)
			if (!strcmp(name + ('+' == *name || '-' == *name), names[button])) {
				press(button, '-' != *name);
				if ('+' != *name && '-' != *name)
					*let_go |= 1u << button;
			}
	}
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
	pad_script(&let_go);
	if (!input_due())
		return;
	for (;;) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_NONE)
			return;
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			exit(0);
		if (event.type == GR_EVENT_TYPE_UPDATE && event.update.utype == GR_UPDATE_SIZE) {
			gd_show(1);	/* (not while a worker core is in the library) */
			seglWindowChanged();
		}
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

/* The window's events while a worker emulates: the keys down, and the presses counted, for it. */
static void
pad_here(void)
{
	GR_EVENT event;
	int button;

	if (!input_due())
		return;
	for (;;) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_NONE)
			return;
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			exit(0);
		/* (a window resized has another buffer: the swap asks the window system then) */
		if (event.type != GR_EVENT_TYPE_KEY_DOWN && event.type != GR_EVENT_TYPE_KEY_UP)
			continue;
		if (event.keystroke.ch == MWKEY_ESCAPE)
			exit(0);
		if ((button = button_of(event.keystroke.ch)) < 0)
			continue;
		if (event.type == GR_EVENT_TYPE_KEY_DOWN) {
			first_said.downs[button]++;
			first_said.held |= 1u << button;
		} else
			first_said.held &= ~(1u << button);
	}
}

/* osd_getinput on the worker that emulates, from what pad_here saw: a key pressed and let go
 * since the last look is down until the next. */
static void
pad_away(void)
{
	static unsigned let_go, down, downs[8];
	unsigned held = first_said.held;
	int button;

	for (button = 0; button < 8; button++)
		if (let_go & 1u << button) {
			press(button, 0);
			down &= ~(1u << button);
		}
	let_go = 0;
	pad_script(&let_go);
	for (button = 0; button < 8; button++) {
		unsigned bit = 1u << button;

		if (first_said.downs[button] != downs[button]) {
			downs[button] = first_said.downs[button];
			press(button, 1);
			down |= bit;
			if (!(held & bit))
				let_go |= bit;
		} else if ((down & bit) && !(held & bit)) {
			press(button, 0);
			down &= ~bit;
		}
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
	if (getenv("NES_MARK"))
		frames_mark = atoi(getenv("NES_MARK"));
	swap_waits = getenv("NES_SWAP") != NULL;
	hold_late = getenv("NES_HOLD_LATE") != NULL;
	unlimited = getenv("NES_FAST") != NULL || getenv("NES_FRAMES") != NULL;
	nes_sum_on = getenv("NES_SUM") != NULL;
	cells_check = getenv("NES_CELLS_CHECK") != NULL;
	if (getenv("NES_PLAIN"))
		nes6502_plain = atoi(getenv("NES_PLAIN"));
	/* NES_RC=plain: translated code with every instruction's own test of the cycles (rc6502.h) */
	rc_plain_runs_off = getenv("NES_RC") != NULL && !strcmp(getenv("NES_RC"), "plain");
	rc_eager = getenv("NES_RC") != NULL && !strcmp(getenv("NES_RC"), "eager");
	ppu_lines_each = getenv("NES_LINES") != NULL && !strcmp(getenv("NES_LINES"), "each");
	return nofrendo_main(argc, argv);
}
