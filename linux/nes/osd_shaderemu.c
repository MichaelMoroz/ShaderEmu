/*
 * Nofrendo's machine side on ShaderEmu (docs/nes.md): the NES's picture is bytes in GPU memory
 * that the GPU stretches over a Nano-X window through the display's palette, the pad is the
 * keyboard, and a frame is drawn for every few emulated (NES_SKIP), the machine being slow.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <noftypes.h>
#include <nofconfig.h>
#include <bitmap.h>
#include <event.h>
#include <log.h>
#include <nes.h>
#include <nesinput.h>
#include <nofrendo.h>
#include <osd.h>
#include <GLES/segl.h>
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
static int frames_most, frames_hold, frames_drawn;
static const char *script;		/* NES_PRESS: frames and the buttons to press at them */
static unsigned cycles_then, ms_then;
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

void
osd_setsound(void (*playfunc)(void *buffer, int length))
{
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
	int y, width, height, emulated;

	for (y = 0; y < HEIGHT && y < bmp->height; y++)
		memcpy(picture + y * WIDTH, bmp->line[y], WIDTH);
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(xyz, uv, texture, 255, 255, 0);
	seglSwap();

	/* the frames to emulate before the next one is drawn */
	for (y = 0; y < skip + 1 && tick; y++)
		tick();
	frames_drawn++;
	emulated = frames_drawn * (skip + 1);
	if (frames_drawn % 20 == 0) {
		unsigned now = cycles(), ms = milliseconds();

		fprintf(stderr, "nesstat: %d frames of the NES in %u ms (%d drawn): %u thousand instructions each, %u%% of its speed\n",
			20 * (skip + 1), ms - ms_then, 20, (now - cycles_then) / 1000 / (20 * (skip + 1)),
			ms > ms_then ? 20 * (skip + 1) * 100000 / 60 / (ms - ms_then) : 0);
		cycles_then = now;
		ms_then = ms;
	}
	if (frames_hold && emulated >= frames_hold) {
		GR_EVENT event;

		/* (for a test: the picture stays, and a snapshot has it in GPU memory) */
		fprintf(stderr, "nes: holding, picture %d bytes past the palette\n", (int)(picture - (unsigned char *)seglPalette()));
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
			sum = sum * 31 + picture[i];
		fprintf(stderr, "nes: done, picture %08x\n", sum);
		exit(0);
	}
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

		if (sscanf(script, "%d %7s%n", &frame, name, &used) < 2 || frame > frames_drawn * (skip + 1))
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
	if (getenv("NES_PLAIN"))
		nes6502_plain = 1;
	return nofrendo_main(argc, argv);
}
