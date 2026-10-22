/*
 * Doom's picture on the ShaderEmu GPU (docs/machine/gpu.md), through OpenGL (programs/linux/gles.c).
 * The 8-bit screen Doom draws on is a texture in GPU memory, shown as one rectangle looked up
 * in the palette. With the GPU also drawing the 3D view (doom_gl.c) that rectangle goes on top
 * of it, with the view's area keyed out.
 *
 *   DOOM_RENDER=soft   Doom's own renderer fills the view; the GPU only shows the screen
 *   DOOM_SOFTWARE=1    no GPU at all: the port converts each frame and sends it to the server
 *   DOOM_STOP_TIC=N    hold the picture of game tic N (for comparing pictures)
 *   DOOM_TOTALS=1      at exit: instructions of the start, the tics, the frames (doomtotal:)
 *
 * The port's i_video.c is compiled with its frame, palette and start-up functions renamed and
 * an accessor appended (linux/nanox/doom.sh). Also prints the frame rate.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <GLES/segl.h>
#include "nano-X.h"
#include "mcwx.h"

void I_FinishUpdate_port(void);
void I_SetPalette_port(unsigned char *palette);
void I_InitGraphics_port(void);
GR_WINDOW_ID doom_window(void);
extern unsigned char *screens[5];
extern unsigned char gammatable[5][256];
extern int usegamma;
extern int gametic, gameaction;
extern int doom_gl, doom_gl_scene, doom_gl_kept;
extern int doom_view_inline;	/* so many views more are this core's own to draw (doom_gl.c) */
extern char __DATA_BEGIN__[], _end[];
void doom_gl_again(void);
void doom_view_draw(int share);
int doom_view_crew(int workers, int as_told);
void doom_view_taken(unsigned *cost, unsigned *things);
int doom_view_asked(void);
void doom_view_again(void);
void doom_ahead_init(void);
int doom_ahead_wanted(int next);
void doom_ahead(void);
void doom_ahead_made(void);
unsigned doom_ahead_walked(void);
extern unsigned doom_ahead_count[2];
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
static int worker;		/* worker cores make the view's list while the next tic runs, */
static int crew = 1;		/* so many of them, each its share of the things */
static int view_out;		/* they have one, or have made one that is not shown yet */

/* Instructions the machine has run, for the figures printed with the frame rate. */
static inline uint32_t
cycles(void)
{
	uint32_t v;

	__asm__ volatile("rdcycle %0" : "=r"(v));
	return v;
}

static uint32_t in_tics, in_show, in_worker, stats_from;
static int tics_from;

/* DOOM_TOTALS=1: the start up to the first frame shown, and the sums of all after it. */
static uint32_t born, start_cost, all_from, all_tics, all_show, all_view, all_display, all_wait, all_worker;
static int all_frames, all_tic_count;

static void
totals(void)
{
	uint32_t whole = cycles() - all_from;

	all_view += doom_view[0];
	fprintf(stderr, "doomtotal: start %uk; %d tics %uk, %d frames %uk draw (the view %uk) + %uk show, the melt's waits %uk, between them %uk; all %uk; a worker core's %uk\n",
		start_cost / 1000, all_tic_count, all_tics / 1000, all_frames, (all_display - all_show - all_wait) / 1000, all_view / 1000,
		all_show / 1000, all_wait / 1000, (whole - all_tics - all_display) / 1000, whole / 1000, all_worker / 1000);
}

static void __attribute__((constructor))
begun(void)
{
	born = cycles();
	if (getenv("DOOM_TOTALS"))
		atexit(totals);
}

/*
 * The game's tic, as d_main.c and d_net.c call it (doom.sh): counted apart from drawing, and
 * with DOOM_HASH_TICS=N a sum of the game's state every N tics, to see a demo play the same.
 */
static void seat_tic(void);
static void view_poll(void);
static void view_shares(void *unused, uint32_t from, uint32_t to);
static void view_leave(void);
void doom_view_finish(void);

void
G_Ticker_counted(void)
{
	static int every = -1;
	uint32_t began = cycles();

	if (gameaction)
		doom_view_finish();	/* (a level may be made anew: the workers read the one there is) */
	seat_tic();
	G_Ticker();
	view_poll();
	in_tics += cycles() - began;
	all_tics += cycles() - began;
	all_tic_count++;
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
	seglWorldUp(3);	/* z */
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
	/* DOOM_VIEW=N: so many worker cores for the view's list (4); inline: none; told: none, and
	 * the list made from what they would be told (docs/games/doom.md) */
	render = getenv("DOOM_VIEW");
	if (doom_gl && !(render && atoi(render) <= 0) && (crew = mcw_open((render ? atoi(render) : 4) + 1)) > 0) {
		atexit(mcw_close);
		mcw_touch(__DATA_BEGIN__, _end - __DATA_BEGIN__);	/* a worker stops at a page nobody has written yet */
		/* one more than the view's: the next tic's sight checks (doom_fast.c) */
		if (crew > 1 && crew > (render ? atoi(render) : 4))
			crew--, doom_ahead_init();
		crew = doom_view_crew(crew, 0);
		worker = 1;
		mcw_touch_code((const void *)view_shares, 64);
		atexit(view_leave);
	}
	if (!worker)
		crew = doom_view_crew(0, render && !strcmp(render, "told"));
}

/* ---- the view's list on worker cores (docs/games/doom.md, "The view on worker cores") ---- */

static void show_list(int scene);

/*
 * The shares of a view, one to a worker by mcw_for_begin(), made while this core runs the next
 * tic and taken when all are made. The first share shows the screen over the view; a piece
 * after the shares walks the sight checks of the tic after the next (doom_fast.c).
 */
static int ahead_out;		/* the round has the checks ahead of the tic after the next too */

static void
view_shares(void *unused, uint32_t from, uint32_t to)
{
	(void)unused;
	for (; from < to; from++)
		if (from == (uint32_t)crew) {
			doom_ahead();
		} else {
			doom_view_draw(from);
			if (!from)
				show_list(1);
		}
}

/* The program leaves: the view that is out is waited for. */
static void
view_leave(void)
{
	mcw_for_wait();
	view_out = worker = 0;
}

static void
view_cost(void)
{
	unsigned cost = 0;

	doom_view_taken(&cost, &doom_view[3]);
	in_worker += cost;
	all_worker += cost;
}

/* The list the workers made goes to the GPU, and what it cost is taken. */
static void
view_shown(void)
{
	mcw_for_wait();
	if (ahead_out)
		doom_ahead_made();
	ahead_out = 0;
	seglSwapEarly();
	view_out = 0;
	view_cost();
}

static void
view_poll(void)
{
	if (view_out && mcw_for_ready())
		view_shown();
}

/* Before anything is written that the worker's view reads, or another list is begun. */
void
doom_view_finish(void)
{
	if (!view_out)
		return;
	while (!mcw_for_ready())
		mcw_rest();
	view_shown();
}

/* The game waits for the next tic's time (doom.sh has d_net.c call this in its loop): the
 * clock moves between the machine's passes, so the pass ends here. */
void
doom_idle(void)
{
	view_poll();
	mcw_rest();
}

int I_GetTime_port(void);

/* The machine's clock word (docs/machine/gpu.md): the time without asking the kernel. */
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
	static int tics_then, began;

	if (!gpu)
		return I_GetTime_port();
	if (!began) {
		/* (`from` itself must be the clock's word: a later one would give a time before it) */
		from = clock_ms();
		tics_then = I_GetTime_port();
		began = 1;
	}
	return tics_then + (int)((clock_ms() - from) * 35 / 1000);
}

/* ---- the pointer, and a sitter in a holodeck's seat (docs/world/holodeck.md, "Played from the seat") ---- */

/* (Doom's own, which this file does not include) */
typedef struct { int type, data1, data2, data3; } doom_event;
typedef struct { signed char forwardmove, sidemove; short angleturn, consistancy; unsigned char chatchar, buttons; } doom_cmd;
enum { EV_MOUSE = 2, BACKUP_TICS = 12 };
void D_PostEvent(doom_event *event);
void I_GetEvent(GR_EVENT *event);
void G_BuildTiccmd(doom_cmd *cmd);
extern int maketic, viewangleoffset;

static int dragging;		/* the left button is held on the game: the pointer's travel turns the view */
static int seated;		/* 1 with tracked hands, 2 with a mouse */
static unsigned mouse_count;
static short hand_turn, turn_of[BACKUP_TICS];	/* the right hand's turn, and as each tic's move was made */

static const volatile unsigned *
sitter(void)
{
	const volatile unsigned *host = gpu ? seglHostState() : NULL;
	int now = !host || !SEGL_HOST_HOLODECK(host) ? 0 : SEGL_HOST_HANDS(host) ? 1 : SEGL_HOST_KIND(host) == 2 ? 2 : 0;

	if (now != seated) {
		seated = now;
		dragging = 0;
		hand_turn = now == 1 ? SEGL_HOST_HAND_YAW(host, 1) : 0;
		mouse_count = host ? SEGL_HOST_MOUSE_X(host) : 0;
	}
	return host;
}

static void
turn_by(int buttons, int by)
{
	doom_event event = { EV_MOUSE, buttons, by, 0 };

	D_PostEvent(&event);
}

/* The window's events. On a flat screen the left button drags the view round, as a finger on
 * glass does, and the right one fires; a sitter's buttons are the game's own, and the pointer
 * turns nothing (a mouse's travel does: 0.066 degrees a count, as 0.044 a unit of Doom's). */
static void
events(void)
{
	extern int gamestate, menuactive;
	static int last_x, fire;
	const volatile unsigned *host = sitter();
	int playing = gamestate == 0 && !menuactive;
	GR_EVENT e;

	if (seated == 2) {
		unsigned count = SEGL_HOST_MOUSE_X(host);
		int gone = (short)(count - mouse_count);

		/* (a menu's would take a look round for its arrows: only a level is turned) */
		mouse_count = count;
		if (gone && playing)
			turn_by(fire, gone * 3 / 2);
	}
	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return;
		if (e.type == GR_EVENT_TYPE_MOUSE_MOTION) {
			if (dragging && !seated)
				turn_by(fire, (e.mouse.x - last_x) * 6);	/* a quarter of a degree a pixel */
			last_x = e.mouse.x;
		} else if ((e.type == GR_EVENT_TYPE_BUTTON_DOWN || e.type == GR_EVENT_TYPE_BUTTON_UP) && !seated) {
			dragging = (e.button.buttons & GR_BUTTON_L) != 0;
			fire = (e.button.buttons & GR_BUTTON_R) != 0;
			last_x = e.button.x;
			turn_by(fire, 0);
		} else if (e.type == GR_EVENT_TYPE_BUTTON_DOWN || e.type == GR_EVENT_TYPE_BUTTON_UP) {
			/* a sitter's click on the room's screen is a menu's, never a shot: they fire by the trigger */
			fire = 0;
			if (!playing || e.type == GR_EVENT_TYPE_BUTTON_UP)
				I_GetEvent(&e);
		} else {
			I_GetEvent(&e);
		}
	}
}

/* A tic's move as it is made (doom.sh has d_net.c call this). With tracked hands the right
 * hand aims: the player turns as it turns, the walk stays the body's, and the room's view is
 * the player's angle less the hand's turn of the tic it shows (seat_tic). */
void
G_BuildTiccmd_seat(doom_cmd *cmd)
{
	const volatile unsigned *host = sitter();

	G_BuildTiccmd(cmd);
	if (seated) {
		extern int menuactive;

		/* a sitter's fire: the right trigger; use: the left one or the jump button (Space) */
		if (!menuactive && SEGL_HOST_HAND_TRIGGER(host, 1))
			cmd->buttons |= 1;
		if (!menuactive && (SEGL_HOST_HAND_TRIGGER(host, 0) || SEGL_HOST_JUMP(host)))
			cmd->buttons |= 2;
	}
	if (seated == 2) {
		/* (the keys they walk by come as the left stick: no window need have the keyboard) */
		int forward = cmd->forwardmove + SEGL_HOST_STICK(host, 1) * 50 / 127, side = cmd->sidemove + SEGL_HOST_STICK(host, 0) * 40 / 127;

		cmd->forwardmove = (signed char)(forward > 50 ? 50 : forward < -50 ? -50 : forward);
		cmd->sidemove = (signed char)(side > 40 ? 40 : side < -40 ? -40 : side);
	}
	if (seated == 1) {
		short turn = SEGL_HOST_HAND_YAW(host, 1);
		/* the walk goes the way the head looks: so far to the left of the aim */
		float angle = (short)(turn - SEGL_HOST_HEAD_YAW(host)) * (6.2831853f / 65536.0f), s = sinf(angle), c = cosf(angle);
		float look = SEGL_HOST_STICK(host, 2) / 127.0f;
		int forward = cmd->forwardmove + SEGL_HOST_STICK(host, 1) * 50 / 127, side = cmd->sidemove + SEGL_HOST_STICK(host, 0) * 40 / 127;

		forward = forward > 50 ? 50 : forward < -50 ? -50 : forward;
		side = side > 40 ? 40 : side < -40 ? -40 : side;
		/* (the right stick: 140 degrees a second pushed home, 35 tics a second) */
		cmd->angleturn -= (short)((fabsf(look) < 0.15f ? 0 : look * fabsf(look)) * 728);
		cmd->angleturn -= (short)(turn - hand_turn);
		hand_turn = turn;
		cmd->forwardmove = (signed char)(forward * c + side * s);
		cmd->sidemove = (signed char)(side * c - forward * s);
	}
	turn_of[maketic % BACKUP_TICS] = seated == 1 ? hand_turn : 0;
}

static void
seat_tic(void)
{
	extern unsigned viewangle;
	extern int viewx, viewy;
	static int said = -1;

	if (said < 0)
		said = getenv("DOOM_SEAT") != NULL;	/* a line every 20 tics: how it is seated, the aim, the room's view */
	if (said && ++said % 20 == 0)
		fprintf(stderr, "seat: %d aim %.1f body %.1f drag %d at %d %d eye %08x\n", seated, (viewangle - (unsigned)viewangleoffset) * (360.0 / 4294967296.0),
			viewangle * (360.0 / 4294967296.0), dragging, viewx >> 16, viewy >> 16, gpu ? seglHostState()[(0x6c - 0x360) / 4] : 0);
	viewangleoffset = (int)((unsigned)(unsigned short)turn_of[gametic % BACKUP_TICS] << 16);
}

/*
 * Asking the server for events costs two system calls and two task switches, every tic. The
 * machine's own input words (pointer, buttons, key events so far; docs/machine/input.md) say when
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

		if (sitter() && seated == 2 && SEGL_HOST_MOUSE_X(seglHostState()) != mouse_count)
			ask = ask ? ask : 1;
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
	events();
}

/* The melt waits for the clock to move on by asking the time again and again (doom.sh has
 * it call this between): the machine's pass ends instead, and the clock moves between passes. */
void
doom_wait(void)
{
	uint32_t began = cycles();

	if (gpu)
		__asm__ volatile(".word 0x0100000f");
	all_wait += cycles() - began;
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
	if (!view_out)
		seglSettle();	/* (a level's start sets the palette from a tic) */
	for (i = 0; i < 256; i++, palette += 3)
		words[i] = (unsigned)gammatable[usegamma][palette[0]] << 16 | gammatable[usegamma][palette[1]] << 8 |
			gammatable[usegamma][palette[2]];
}

/* A frame's drawing writes the screen, the kept level and the palette, which the list of the
 * frame before reads: not before the GPU has drawn that one (doom.sh names the game's own). */
void D_Display_game(void);

void
D_Display(void)
{
	uint32_t began;

	if (gpu) {
		extern int gamestate, menuactive, automapactive, dirtybox[4];

		/* the worker's view is shown first. What this core then adds to the screen may come
		 * a frame early; what it clears and draws again (a menu, the map, a page under
		 * what moves on it: all that drew last frame is in dirtybox) waits for the GPU */
		doom_view_finish();
		if (!worker || gamestate != 0 || menuactive || automapactive || dirtybox[0] >= dirtybox[1])
			seglSettle();
		/* a menu, a screen between levels: a picture for a screen, not a world for a room */
		seglWorldFlat(gamestate != 0 || menuactive);
		/* the eye 41 units over the floor; near 4, far 8192, a quarter turn wide (doom_gl.c) */
		seglWorldEye(1, 41, 4, 8192, 1, 1.6f);
	}
	began = all_from ? cycles() : 0;
	D_Display_game();
	if (began)
		all_display += cycles() - began;
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

/* The screen as one rectangle over the whole window, after the view if there is one (`scene`). */
static void
show_list(int scene)
{
	/* in units of the whole window: a matrix of 16.16 numbers cannot hold 2/320 exactly */
	static const GLfixed corners[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	static const GLfixed flat[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0};
	/* glOrthox(0, 1, 1, 0, -1, 1) column by column: working it out anew costs a frame 800 instructions */
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	int width, height;

	seglSize(&width, &height);
	if (!scene)
		glClear(GL_COLOR_BUFFER_BIT);	/* nothing under it this frame */
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glColorKeySE(KEY);
	/* keyed over a view: it shows through where the screen holds the key */
	seglQuad(flat, corners, screen_texture, 255, 255, scene);
	if (melting == 1 || melting == 2)
		melt_strips();
}

/* A frame is drawn: its list is the worker's to make from what the view was told, or made
 * here and now (no view in it, a level's first frames, a melt, a level that is not kept). */
static void
show(void)
{
	static int melt_scene;
	int scene = doom_gl_scene;

	doom_gl_scene = 0;
	if (scene && worker && doom_gl_kept && !melting && doom_view_inline <= 0) {
		/* (R_RenderPlayerView saw the worker's last view shown before it told this one) */
		const int pieces = crew + (ahead_out = doom_ahead_wanted(gametic));
		const mcw_for_opts one_each = { 1, 0, 0, 0, pieces, 1 };	/* a piece a worker, none here */

		mcw_for_begin(pieces, view_shares, NULL, &one_each);
		view_out = 1;
		return;
	}
	doom_view_finish();
	if (scene) {
		int k;

		doom_view_inline -= doom_view_inline > 0;
		for (k = 0; k < crew; k++)
			doom_view_draw(k);
		view_cost();
	}
	/* a melt's later frames have the view of its first under them, which Doom does not draw again */
	if (melting == 1)
		melt_scene = scene;
	else if (melting && melt_scene) {
		doom_gl_again();
		doom_gl_scene = 0;
		scene = 1;
	}
	show_list(scene);
	if (melting == 3)
		melting = 0;
	/* the game's tics come next and write no memory the list reads: D_Display() settles */
	seglSwapEarly();
}

/* ---- DOOM_SUM=1: at a held tic, a sum of all the GPU was given for the picture on show ---- */

#ifdef __gpuvm__
#define GPU_FROM	0x01000000u	/* gpu_vm: the device window moved down by 0x86000000 */
#else
#define GPU_FROM	0x87000000u
#endif
#define GPU_BYTES	0x00b00000u
#define REG_SHOWN	0x300		/* the last whole frame: its list's address, how many commands */

static unsigned
sum_of(unsigned h, const unsigned char *gpu_at, unsigned address, unsigned bytes)
{
	const unsigned *p;

	if (address < GPU_FROM || address - GPU_FROM > GPU_BYTES - bytes || bytes > GPU_BYTES)
		return h * 16777619u ^ 0xbadbad;
	for (p = (const unsigned *)(gpu_at + (address - GPU_FROM)); bytes >= 4; bytes -= 4)
		h = (h ^ *p++) * 16777619u;
	while (bytes--)
		h = (h ^ ((const unsigned char *)p)[bytes]) * 16777619u;
	return h;
}

/* Each command drawn, its matrices, its vertices and its texture, and the palette. The second
 * sum is the same whatever the commands' order: a thing's place among them goes by where the
 * game keeps the thing, which is not the same when pictures are made at other times. */
static void
list_sum(void)
{
	const unsigned char *at = (const unsigned char *)seglPalette() - REG_PALETTE;
	const unsigned *regs = (const unsigned *)at, list = regs[REG_SHOWN / 4], count = regs[REG_SHOWN / 4 + 1];
	unsigned h = 2166136261u, any = 0, i, k, drawn = 0, texture = 0;
	const int each = atoi(getenv("DOOM_SUM")) > 1;

	for (i = 0; i < count; i++) {
		const unsigned *c = (const unsigned *)(at + (list - GPU_FROM)) + 16 * i;

		if (c[0] == 3 && !c[2])
			continue;	/* a kept command that shows nothing */
		drawn++;
		/* (not where its vertices, matrices and texture lie: what is in them) */
		for (k = 0; k < 16; k++)
			if (c[0] != 3 || (k != 1 && k != 6 && k != 7))
				h = (h ^ c[k]) * 16777619u;
		if (c[0] != 3)
			continue;
		h = sum_of(h, at, c[6], 128);
		h = sum_of(h, at, c[1], c[4] & 0x200 ? c[2] / 6 * 64 : c[2] * 16);
		if ((c[5] & 0xff) == 2 && c[7] != texture)
			h = sum_of(h, at, texture = c[7], c[8] * c[9]);
		{
			unsigned one = 2166136261u;

			for (k = 0; k < 16; k++)
				if (k != 1 && k != 3 && k != 6 && k != 7)
					one = (one ^ c[k]) * 16777619u;
			one = sum_of(one, at, c[6], 128);
			one = sum_of(one, at, c[1], c[4] & 0x200 ? c[2] / 6 * 64 : c[2] * 16);
			if ((c[5] & 0xff) == 2)
				one = sum_of(one, at, c[7], c[8] * c[9]);
			any += one * 2654435761u;
		}
		if (each)	/* DOOM_SUM=2: where two lists part */
			fprintf(stderr, "doomlist: %u: %u %x %x %ux%u %x, vertices %08x, picture %08x, so far %08x\n", i, c[2], c[4], c[5], c[8], c[9],
				c[11], sum_of(0, at, c[1], c[4] & 0x200 ? c[2] / 6 * 64 : c[2] * 16), sum_of(0, at, c[7], c[8] * c[9]), h);
	}
	h = sum_of(h, at, GPU_FROM + REG_PALETTE, 1024);
	fprintf(stderr, "doom: tic %d list of %u commands, %u drawn, sum %08x, in any order %08x\n", gametic, count, drawn, h, any);
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
	all_show += cycles() - began;
	if (!all_from) {
		/* the first frame is shown: what follows is counted apart from the start */
		start_cost = cycles() - born;
		all_from = cycles();
		all_tics = all_show = all_display = all_wait = 0;
		all_view = -doom_view[0];
		all_frames = all_tic_count = 0;
	} else {
		all_frames++;
	}
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

		if (view_out) {
			/* the held picture is the one this core would have made: with every
			 * picture the worker asked for and went without */
			while (!mcw_for_ready())
				mcw_rest();
			while (doom_view_asked()) {
				doom_view_again();
				show_list(1);
			}
			view_shown();
		}
		/* the picture reaches the window a frame after it is drawn; the port's path has
		 * it in a buffer the server has not read yet, and an answer from the server
		 * means it has drawn everything sent before the question */
		for (wait = 0; gpu && wait < 4; wait++)
			__asm__ volatile(".word 0x0100000f");
		GrGetWindowInfo(doom_window(), &info);
		if (gpu && getenv("DOOM_SUM"))
			list_sum();
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
			fprintf(stderr, "doomstat: the view %uk a frame and %uk its list (%s), %u things\n", doom_view[0] / frames / 1000,
				in_worker / frames / 1000, worker ? "a worker core's" : "this core's", doom_view[3] / frames);
		if (doom_ahead_count[0])
			fprintf(stderr, "doomstat: %u sight checks, %u answered by a worker's walks (%u of them)\n",
				doom_ahead_count[0], doom_ahead_count[1], doom_ahead_walked());
		all_view += doom_view[0];
		memset(doom_view, 0, sizeof doom_view);
		in_tics = in_show = in_worker = 0;
		stats_from = cycles();
		tics_from = gametic;
		frames = 0;
		start = ms;
	}
}
