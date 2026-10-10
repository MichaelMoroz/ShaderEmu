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
	}
	if ((int)(now - due) > 50 || (int)(due - now) > 200)
		due = now;
	else if ((int)(due - now) > 0)
		usleep((due - now) * 1000);
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
