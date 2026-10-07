/*
 * Doom's picture on the ShaderEmu GPU (docs/gpu.md), through OpenGL (programs/linux/gles.c).
 * The 8-bit screen Doom draws on is a texture in GPU memory, shown as one rectangle looked up
 * in the palette. With the GPU also drawing the 3D view (doom_gl.c) that rectangle goes on top
 * of it, with the view's area keyed out.
 *
 *   DOOM_RENDER=soft   Doom's own renderer fills the view; the GPU only shows the screen
 *   DOOM_SOFTWARE=1    no GPU at all: the port converts each frame and sends it to the server
 *   DOOM_STOP_TIC=N    hold the picture of game tic N (for comparing pictures)
 *
 * The port's i_video.c is compiled with its frame, palette and start-up functions renamed and
 * an accessor appended (linux/nanox/doom.sh). Also prints the frame rate.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <GLES/segl.h>
#include "nano-X.h"

void I_FinishUpdate_port(void);
void I_SetPalette_port(unsigned char *palette);
void I_InitGraphics_port(void);
GR_WINDOW_ID doom_window(void);
extern unsigned char *screens[5];
extern unsigned char gammatable[5][256];
extern int usegamma;
extern int gametic;
extern int doom_gl, doom_gl_scene, doom_gl_kept;
void doom_gl_again(void);
int wipe_StartScreen(int x, int y, int width, int height);
int wipe_EndScreen(int x, int y, int width, int height);
int wipe_ScreenWipe(int wipeno, int x, int y, int width, int height, int ticks);
int M_Random(void);

#define SCREEN_W	320
#define SCREEN_H	200
#define KEY		247		/* as in doom_gl.c */
#define ONE		65536
#define REG_PALETTE	0x400		/* offsets in the machine's control words: what seglPalette() points at, */
#define REG_INPUT	0x20		/* and pointer x, y, buttons, key events so far */
#define REG_CLOCK	0x34		/* the host's clock in milliseconds, as of this machine frame */

void I_StartTic_port(void);
void G_Ticker(void);
unsigned doom_state_hash(void);
extern unsigned doom_view[4];	/* the view's instructions, walls, planes and things (doom_gl.c) */

static int stop_tic = -1;
static int gpu;			/* the GPU shows the screen */
static GLuint screen_texture;

/* Instructions the machine has run, for the figures printed with the frame rate. */
static inline uint32_t
cycles(void)
{
	uint32_t v;

	__asm__ volatile("rdcycle %0" : "=r"(v));
	return v;
}

static uint32_t in_tics, in_show, stats_from;
static int tics_from;

/*
 * The game's tic, as d_main.c and d_net.c call it (doom.sh): counted apart from drawing, and
 * with DOOM_HASH_TICS=N a sum of the game's state every N tics, to see a demo play the same.
 */
void
G_Ticker_counted(void)
{
	static int every = -1;
	uint32_t began = cycles();

	G_Ticker();
	in_tics += cycles() - began;
	if (every < 0) {
		const char *text = getenv("DOOM_HASH_TICS");
		every = text ? atoi(text) : 0;
	}
	if (every && (gametic + 1) % every == 0)
		fprintf(stderr, "doom: tic %d state %08x\n", gametic + 1, doom_state_hash());
}

/*
 * The window's scale when no option gave one: the largest of 1 to 3 the display has room for
 * beside a frame and the desktop's bar. The GPU does the stretching; the port's own path
 * (DOOM_SOFTWARE=1) copies every pixel and stays small.
 */
int
doom_window_scale(int asked, int given)
{
	const char *soft = getenv("DOOM_SOFTWARE");
	GR_SCREEN_INFO info;
	int scale;

	if (given || (soft && *soft == '1'))
		return asked;
	GrGetScreenInfo(&info);
	for (scale = 3; scale > 1 && (SCREEN_W * scale + 8 > info.cols || SCREEN_H * scale + 52 > info.rows); scale--)
		;
	return scale;
}

/*
 * The port allocates the screen here. Doom takes pointers into it when it first sets up its
 * view, which is after this, so this is where the screen becomes the texture.
 */
void
I_InitGraphics(void)
{
	const char *soft = getenv("DOOM_SOFTWARE"), *render = getenv("DOOM_RENDER");
	unsigned char *screen;

	I_InitGraphics_port();
	if ((soft && *soft == '1') || seglInit(doom_window()) < 0)
		return;
	screen = seglMemory(SCREEN_W * SCREEN_H);
	if (!screen)
		return;
	memset(screen, 0, SCREEN_W * SCREEN_H);
	free(screens[0]);
	screens[0] = screen;
	glGenTextures(1, &screen_texture);
	glBindTexture(GL_TEXTURE_2D, screen_texture);
	seglTexturePointer(screen, SCREEN_W, SCREEN_H, GL_COLOR_INDEX8_EXT);
	gpu = 1;
	doom_gl = !(render && !strcmp(render, "soft"));
}

int I_GetTime_port(void);

/* The machine's clock word (docs/gpu.md): the time without asking the kernel. */
static uint32_t
clock_ms(void)
{
	return *(const volatile uint32_t *)((const char *)seglPalette() - REG_PALETTE + REG_CLOCK);
}

/*
 * The time in tics, which Doom asks for several times a frame: read from the machine's clock
 * once the GPU is ours, going on from what the port's clock said until then.
 */
int
I_GetTime(void)
{
	static uint32_t from;
	static int tics_then;

	if (!gpu)
		return I_GetTime_port();
	if (!from) {
		from = clock_ms() | 1;
		tics_then = I_GetTime_port();
	}
	return tics_then + (int)((clock_ms() - from) * 35 / 1000);
}

/*
 * Asking the server for events costs two system calls and two task switches, every tic. The
 * machine's own input words (pointer, buttons, key events so far; docs/input.md) say when
 * there can be any: ask when they have moved, for a few tics after, and twice a second.
 */
void
I_StartTic(void)
{
	static uint32_t seen[4];
	static int ask = 8, quiet;

	if (gpu) {
		const volatile uint32_t *input = (const volatile uint32_t *)((const char *)seglPalette() - REG_PALETTE + REG_INPUT);
		int i;

		for (i = 0; i < 4; i++)
			if (seen[i] != input[i]) {
				seen[i] = input[i];
				ask = 8;
			}
		if (++quiet >= 16)
			ask = ask ? ask : 1;
		if (!ask)
			return;
		ask--;
		quiet = 0;
	}
	I_StartTic_port();
}

void
I_SetPalette(unsigned char *palette)
{
	unsigned int *words;
	int i;

	if (!gpu) {
		I_SetPalette_port(palette);
		return;
	}
	words = seglPalette();
	for (i = 0; i < 256; i++, palette += 3)
		words[i] = (unsigned)gammatable[usegamma][palette[0]] << 16 | gammatable[usegamma][palette[1]] << 8 |
			gammatable[usegamma][palette[2]];
}

/* ---- the melt between two screens ---- */

/*
 * Doom slides the old screen down off the new one, 160 strips each at its own pace, by copying
 * the whole screen every frame; and its old screen has no 3D view in it here. On the GPU the
 * strips are quads of the picture the window showed last, each moved on by this frame's fall.
 */
#define STRIPS 160
static int melting;		/* 1: the first frame of a melt, 2: a later one, 3: its last */
static int melt_port;		/* this melt is the port's own (no GPU, or a view that cannot be drawn again) */
static int melt_y[2 * STRIPS];	/* how far each strip has fallen; below 0 it waits */
static int melt_fell[STRIPS];	/* and how far in this frame */

int
wipe_StartScreen_ours(int x, int y, int width, int height)
{
	melt_port = !gpu || !doom_gl || !doom_gl_kept;
	return melt_port ? wipe_StartScreen(x, y, width, height) : 0;
}

int
wipe_EndScreen_ours(int x, int y, int width, int height)
{
	return melt_port ? wipe_EndScreen(x, y, width, height) : 0;
}

/* One frame of it, `ticks` game tics on: Doom's own arithmetic, and its random numbers. */
int
wipe_ScreenWipe_ours(int wipeno, int x, int y, int width, int height, int ticks)
{
	int i, done = 1;

	if (melt_port)
		return wipe_ScreenWipe(wipeno, x, y, width, height, ticks);
	if (!melting || melting == 3) {
		melt_y[0] = -(M_Random() % 16);
		for (i = 1; i < 2 * STRIPS; i++) {
			melt_y[i] = melt_y[i - 1] + M_Random() % 3 - 1;
			if (melt_y[i] > 0)
				melt_y[i] = 0;
			else if (melt_y[i] == -16)
				melt_y[i] = -15;
		}
	}
	melting = melting == 1 || melting == 2 ? 2 : 1;
	memset(melt_fell, 0, sizeof melt_fell);
	while (ticks-- > 0)
		for (i = 0; i < STRIPS; i++) {
			if (melt_y[i] < 0) {
				melt_y[i]++;
				done = 0;
			} else if (melt_y[i] < SCREEN_H) {
				int dy = melt_y[i] < 16 ? melt_y[i] + 1 : 8;

				if (melt_y[i] + dy >= SCREEN_H)
					dy = SCREEN_H - melt_y[i];
				melt_y[i] += dy;
				melt_fell[i] += dy;
				done = 0;
			}
		}
	if (done)
		melting = 3;	/* one more frame, with nothing left to fall */
	return done;
}

/* The strips, over everything else of the frame. */
static void
melt_strips(void)
{
	static GLuint last;
	GLfixed across, xyz[12], uv[8];
	int i;

	if (!last)
		glGenTextures(1, &last);
	if (seglWindowTexture(last, &across) < 0)
		return;
	for (i = 0; i < STRIPS; i++) {
		int top = melt_y[i] < 0 ? 0 : melt_y[i], from = top - melt_fell[i];
		GLfixed x0 = (i << 16) / STRIPS, x1 = ((i + 1) << 16) / STRIPS;

		if (top >= SCREEN_H)
			continue;
		xyz[0] = xyz[9] = x0;
		xyz[3] = xyz[6] = x1;
		xyz[1] = xyz[4] = (top << 16) / SCREEN_H;
		xyz[7] = xyz[10] = ONE;
		xyz[2] = xyz[5] = xyz[8] = xyz[11] = 0;
		uv[0] = uv[6] = (GLfixed)(((long long)x0 * across) >> 16);
		uv[2] = uv[4] = (GLfixed)(((long long)x1 * across) >> 16);
		uv[1] = uv[3] = (from << 16) / SCREEN_H;
		uv[5] = uv[7] = ((SCREEN_H - melt_fell[i]) << 16) / SCREEN_H;
		seglQuad(xyz, uv, last, 255, 255, 0);
	}
}

/* The screen as one rectangle over the whole window, then the frame goes to the GPU. */
static void
show(void)
{
	/* in units of the whole window: a matrix of 16.16 numbers cannot hold 2/320 exactly */
	static const GLfixed corners[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	/* glOrthox(0, 1, 1, 0, -1, 1) column by column: working it out anew costs a frame 800 instructions */
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	int width, height;

	static int melt_scene;

	/* a melt's later frames have the view of its first under them, which Doom does not draw again */
	if (melting == 1)
		melt_scene = doom_gl_scene;
	else if (melting && melt_scene)
		doom_gl_again();
	seglSize(&width, &height);
	if (!doom_gl_scene)
		glClear(GL_COLOR_BUFFER_BIT);	/* nothing under it this frame */
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	if (doom_gl_scene)
		glEnable(GL_ALPHA_TEST);	/* the view shows through where the screen holds the key */
	else
		glDisable(GL_ALPHA_TEST);
	glColorKeySE(KEY);
	glColor4ub(255, 255, 255, 255);
	glBindTexture(GL_TEXTURE_2D, screen_texture);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FIXED, 0, corners);
	glTexCoordPointer(2, GL_FIXED, 0, corners);
	glDrawArrays(GL_QUADS, 0, 4);
	glDisable(GL_ALPHA_TEST);
	if (melting == 1 || melting == 2)
		melt_strips();
	if (melting == 3)
		melting = 0;
	seglSwap();
	doom_gl_scene = 0;
}

void
I_FinishUpdate(void)
{
	static int frames, every = -1;
	static long start;
	struct timeval now;
	long ms;

	uint32_t began = cycles();

	if (gpu)
		show();
	else
		I_FinishUpdate_port();
	in_show += cycles() - began;
	if (gpu) {
		ms = (long)(clock_ms() & 0x7fffffff);
	} else {
		gettimeofday(&now, NULL);
		ms = now.tv_sec * 1000L + now.tv_usec / 1000;
	}
	if (start == 0)
		start = ms;
	frames++;
	if (stop_tic < 0) {
		const char *text = getenv("DOOM_STOP_TIC");

		stop_tic = text ? atoi(text) : 0;
	}
	if (stop_tic > 0 && gametic >= stop_tic) {
		GR_WINDOW_INFO info;
		int wait;

		/* the picture reaches the window a frame after it is drawn; the port's path has
		 * it in a buffer the server has not read yet, and an answer from the server
		 * means it has drawn everything sent before the question */
		for (wait = 0; gpu && wait < 4; wait++)
			__asm__ volatile(".word 0x0100000f");
		GrGetWindowInfo(doom_window(), &info);
		fprintf(stderr, "doom: stopped at tic %d\n", gametic);
		for (;;)
			select(0, NULL, NULL, NULL, NULL);
	}
	if (every < 0) {
		const char *text = getenv("DOOM_STATS_MS");	/* how often the figures are printed */

		every = text && atoi(text) > 0 ? atoi(text) : 5000;
	}
	if (ms - start >= every) {
		fprintf(stderr, "doom: %d frames in %ld.%ld seconds = %ld.%ld FPS\n", frames, (ms - start) / 1000,
			(ms - start) % 1000 / 100, frames * 1000L / (ms - start), frames * 10000L / (ms - start) % 10);
		/* thousands of instructions: all, and of those the tics, drawing a frame, showing it */
		if (frames && gametic > tics_from)
			fprintf(stderr, "doomstat: %uk/s; a tic %uk (%d tics); a frame %uk draw + %uk show\n",
				(cycles() - stats_from) / (unsigned)(ms - start), in_tics / (gametic - tics_from) / 1000,
				gametic - tics_from, (cycles() - stats_from - in_tics - in_show) / frames / 1000, in_show / frames / 1000);
		if (frames)
		if (frames)
			fprintf(stderr, "doomstat: the view %uk a frame, %u things\n", doom_view[0] / frames / 1000, doom_view[3] / frames);
		memset(doom_view, 0, sizeof doom_view);
		in_tics = in_show = 0;
		stats_from = cycles();
		tics_from = gametic;
		frames = 0;
		start = ms;
	}
}
