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
extern int doom_gl, doom_gl_scene;

#define SCREEN_W	320
#define SCREEN_H	200
#define KEY		247		/* as in doom_gl.c */
#define ONE		65536
#define REG_PALETTE	0x400		/* offsets in the machine's control words: what seglPalette() points at, */
#define REG_INPUT	0x20		/* and pointer x, y, buttons, key events so far */

void I_StartTic_port(void);

static int stop_tic = -1;
static int gpu;			/* the GPU shows the screen */
static GLuint screen_texture;

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

/* The screen as one rectangle over the whole window, then the frame goes to the GPU. */
static void
show(void)
{
	/* in units of the whole window: a matrix of 16.16 numbers cannot hold 2/320 exactly */
	static const GLfixed corners[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	int width, height;

	seglSize(&width, &height);
	if (!doom_gl_scene)
		glClear(GL_COLOR_BUFFER_BIT);	/* nothing under it this frame */
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthox(0, ONE, ONE, 0, -ONE, ONE);
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
	seglSwap();
	doom_gl_scene = 0;
}

void
I_FinishUpdate(void)
{
	static int frames;
	static long start;
	struct timeval now;
	long ms;

	if (gpu)
		show();
	else
		I_FinishUpdate_port();
	gettimeofday(&now, NULL);
	ms = now.tv_sec * 1000L + now.tv_usec / 1000;
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
	if (ms - start >= 5000) {
		fprintf(stderr, "doom: %d frames in %ld.%ld seconds = %ld.%ld FPS\n", frames, (ms - start) / 1000,
			(ms - start) % 1000 / 100, frames * 1000L / (ms - start), frames * 10000L / (ms - start) % 10);
		frames = 0;
		start = ms;
	}
}
