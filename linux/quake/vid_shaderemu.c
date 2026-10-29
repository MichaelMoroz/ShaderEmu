/*
 * GLQuake's window, frame and input on the ShaderEmu machine (docs/games/quake.md): a Nano-X
 * window the GPU draws into, in place of gl_vidlinuxglx.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include "quakedef.h"
#include "nano-X.h"
#include "mcwx.h"

#define CAPTION		22		/* the window manager's caption and frame (nanowm.h) */
#define FRAME		4
#define REG_INPUT	0x20		/* the machine's input counters (docs/machine/input.md) */
#define REG_CLOCK	0x34
#define WARP_WIDTH	320
#define WARP_HEIGHT	200

unsigned	d_8to24table[256];
unsigned char	d_15to8table[65536];	/* for mipmaps of paletted textures, which are not made */

int		texture_mode = GL_NEAREST;
int		texture_extension_number = 1;
float		gldepthmin, gldepthmax;
cvar_t		gl_ztrick = {"gl_ztrick", "0"};
const char	*gl_vendor, *gl_renderer, *gl_version, *gl_extensions;
qboolean	isPermedia = false;
qboolean	gl_mtexable = false;

static cvar_t	in_mouse = {"in_mouse", "0", true};	/* the pointer turns the view */
static GR_WINDOW_ID window;
static int	opened;
static int	mouse_x, mouse_y, mouse_dx, mouse_dy, mouse_known, mouse_inside = 1;
static int	dragging;	/* the left button is held on the game: the pointer's travel turns the view */
/* A sitter in a holodeck's seat (docs/world/holodeck.md, "Played from the seat"): 1 with tracked
   hands, 2 with a mouse. The room is turned by `body`; the aim is the hand's from there. */
static int	seated;
static float	body, aimed;
static unsigned	mouse_count;
static unsigned	frames, hold_frame, hold_late, stats_ms = 5000, stats_frames;

void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height) {}
void D_EndDirectRect (int x, int y, int width, int height) {}
void VID_HandlePause (qboolean pause) {}
void VID_ShiftPalette (unsigned char *palette) {}
qboolean VID_Is8bit (void) { return true; }

void VID_Shutdown (void)
{
	if (opened)
		GrClose ();
	opened = 0;
}

/* The machine's clock in milliseconds once the GPU is ours: a load, where the system's is a call. */
unsigned VID_Milliseconds (void)
{
	return opened ? *qglControl (REG_CLOCK) : 0;
}

void VID_SetPalette (unsigned char *palette)
{
	unsigned	*table = d_8to24table, *words;
	int		i;

	for (i = 0; i < 256; i++, palette += 3)
		table[i] = 255u << 24 | palette[0] | palette[1] << 8 | (unsigned)palette[2] << 16;
	d_8to24table[255] &= 0xffffff;	/* 255 is transparent */
	if (!opened)
		return;
	words = seglPalette ();
	seglSettle ();
	for (i = 0; i < 256; i++)
		words[i] = (table[i] & 0xff) << 16 | (table[i] & 0xff00) | (table[i] >> 16 & 0xff);
}

void GL_Init (void)
{
	gl_vendor = (const char *)glGetString (GL_VENDOR);
	gl_renderer = (const char *)glGetString (GL_RENDERER);
	gl_version = (const char *)glGetString (GL_VERSION);
	gl_extensions = (const char *)glGetString (GL_EXTENSIONS);
	Con_Printf ("GL_RENDERER: %s\n", gl_renderer);

	glClearColor (0, 0, 0, 0);
	glEnable (GL_TEXTURE_2D);
	glEnable (GL_ALPHA_TEST);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* the host shows the world round a seat: what is beside and behind the camera is drawn too (docs/world/holodeck.md) */
int se_all_round;

extern qboolean scr_drawloading;	/* gl_screen.c */

extern int se_scene_split;	/* scene_shaderemu.c */

void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	extern int sb_updates;
	const volatile unsigned *host = seglHostState ();

	*x = *y = 0;
	seglSize (width, height);
	se_all_round = host && SEGL_HOST_HOLODECK (host);
	/* a menu, the console, a level loading: a picture for a screen, not a world for a room
	 * (the loading plaque's frame too: it stays while the next level replaces what it drew) */
	seglWorldFlat (key_dest != key_game || cls.state != ca_connected || cls.signon != SIGNONS || scr_drawloading ||
		scr_disabled_for_loading || cl.intermission);	/* (a level's end: its totals, or the episode's text) */
	if (!se_scene_split)	/* (a frame drawn on a worker: the first core drew its bar, SE_ScreenRecord) */
		sb_updates = 0;		/* every frame is drawn whole: the status bar too */
}

unsigned se_started;	/* the instruction count at main (sys_shaderemu.c) */

static unsigned cycles (void)
{
	unsigned v;

	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

/*
 * The part of a frame that begins here: 0 the rest, 1 the server, 2 finding what of the level
 * is in view, 3 drawing it, 4 models, 5 particles and the weapon, 6 the 2D screen and the swap.
 */
static unsigned phase, phase_since, phase_cycles[7];
extern unsigned se_drawn[2];	/* gl_rmain.c */
extern unsigned se_server_cycles, se_server_frames, se_server_passes, se_server_between, se_server_held, se_server_behind, se_server_ticks[4];	/* server_shaderemu.c */
extern unsigned se_ahead_used, se_ahead_late, se_ahead_own, se_ahead_wrong;	/* gl_rlight.c */
extern unsigned se_scene_frames, se_scene_waits, se_scene_instructions, se_scene_window, se_scene_waited, se_scene_unrecorded;
extern int se_scene_split;	/* scene_shaderemu.c */
#ifdef SE_COUNT
unsigned se_count[10];	/* what the server did, counted where it does it (quake.patch) */
#endif

void VID_Phase (int next)
{
	unsigned now;

	if (mcw_on_worker ())
		return;		/* (a frame drawn on a worker core: its parts are not counted) */
	now = cycles ();
	SE_ServerPoll ();	/* a worker core with the server's frame may want a page */
	seglSwapTake ();	/* and the drawing worker's frame its submit */
	phase_cycles[phase] += now - phase_since;
	phase = next;
	phase_since = now;
}

/*
 * Every few seconds: frames a second, and the machine's instructions a frame (quakestat:
 * lines; QUAKE_STATS_MS says how often, 0 never). QUAKE_HOLD=N keeps frame N on the screen
 * and stops there, which is how pictures are compared; with QUAKE_HOLD_LATE=1 it stops when
 * the frame after is ready to be shown, having written whatever that one writes.
 */
static void after_swap (void);

void GL_EndRendering (void)
{
	SE_FrameEnd ();
	if (hold_frame && hold_late && frames >= hold_frame) {
		hold_late = 0;
	} else {
		qglSwap ();
		VID_Phase (0);
		frames++;
	}
	after_swap ();
}

/* A frame drawn on a worker core ends there with the swap (scene_shaderemu.c); this core counts
 * it, holds it and says what the frames cost when it has joined it. */
void SE_SceneSwap (void)
{
	extern unsigned se_draw_ticks[3];
	unsigned at;

	SE_FrameEnd ();
	__asm__ volatile ("rdtime %0" : "=r"(at));
	qglSwapHand ();
	se_draw_ticks[1] -= at;
	__asm__ volatile ("rdtime %0" : "=r"(at));
	se_draw_ticks[1] += at;
}

void SE_AfterSwap (void)
{
	frames++;
	after_swap ();
}

int SE_HoldLate (void) { return hold_frame && hold_late; }
/* Whether the frame `ahead` swaps from now is one to hold (scene_shaderemu.c: none is drawn after it). */
int SE_HoldAt (unsigned ahead) { return hold_frame && frames + ahead >= hold_frame; }
int SE_Seated (void) { return seated; }

static void after_swap (void)
{
	static unsigned since_ms, since_cycles, since_frames;
	static int level_shown;
	unsigned now;

	if (hold_frame && frames >= hold_frame && !hold_late) {
		seglSettle ();	/* (the frame is on the screen when this says so) */
		printf ("quake: holding frame %u\n", frames);
		fflush (stdout);
		for (;;)
			__asm__ volatile (".word 0x0100000f");	/* pause: the machine's frame ends */
	}
	if (!level_shown && stats_ms && cls.signon == SIGNONS) {
		/* from the program's start to a level's first picture */
		level_shown = 1;
		printf ("quakestat: a level's first frame is frame %u, %u thousand instructions from the start\n", frames, (cycles () - se_started) / 1000);
		fflush (stdout);
	}
	now = VID_Milliseconds ();
	if (!since_ms)
		since_ms = now, since_cycles = cycles (), since_frames = frames;
	if (stats_frames ? frames - since_frames >= stats_frames : stats_ms && now - since_ms >= stats_ms) {
		unsigned n = frames - since_frames, kept, in_gpu, gpu_size, evicted, vertices;

		qglMemory (&kept, &in_gpu, &gpu_size, &evicted, &vertices);
		printf ("quakestat: %u frames in %u ms, %u instructions a frame, %u vertices; textures %u KB, %u of %u KB in the GPU, %u evicted\n",
			n, now - since_ms, n ? (cycles () - since_cycles) / n : 0, vertices, kept >> 10, in_gpu >> 10, gpu_size >> 10, evicted);
		if (n) {
			unsigned *c = phase_cycles;
			static unsigned drawn_was[2];	/* (the drawing worker counts on: read here, never written) */
			unsigned drawn[2] = {se_drawn[0] - drawn_was[0], se_drawn[1] - drawn_was[1]};

			printf ("quakestat: a frame in thousands: server %u, in view %u, level %u, models %u, particles and weapon %u, 2D and swap %u, rest %u; %u.%u models and %u.%u brush models drawn\n",
				c[1] / n / 1000, c[2] / n / 1000, c[3] / n / 1000, c[4] / n / 1000, c[5] / n / 1000, c[6] / n / 1000, c[0] / n / 1000,
				drawn[0] / n, drawn[0] * 10 / n % 10, drawn[1] / n, drawn[1] * 10 / n % 10);
			memset (phase_cycles, 0, sizeof phase_cycles);
			drawn_was[0] += drawn[0], drawn_was[1] += drawn[1];
			if (se_server_frames) {
				printf ("quakestat: the server on a worker core: %u frames of its own to these %u (%u drawn between two of them, %u standing at the newer, %u behind the older), %u thousand instructions each, %u.%u passes waited for one\n",
					se_server_frames, n, se_server_between, se_server_held, se_server_behind, se_server_cycles / se_server_frames / 1000, se_server_passes / se_server_frames, se_server_passes * 10 / se_server_frames % 10);
				printf ("quakestat: the server's frame in ms of the clock: %u.%u to begin, %u.%u running, %u.%u until taken, %u.%u until the next\n",
					se_server_ticks[0] / 5 / se_server_frames, se_server_ticks[0] * 2 / se_server_frames % 10,
					se_server_ticks[1] / 5 / se_server_frames, se_server_ticks[1] * 2 / se_server_frames % 10,
					se_server_ticks[2] / 5 / se_server_frames, se_server_ticks[2] * 2 / se_server_frames % 10,
					se_server_ticks[3] / 5 / se_server_frames, se_server_ticks[3] * 2 / se_server_frames % 10);
				memset (se_server_ticks, 0, sizeof se_server_ticks);
				{	/* (the server's own counts: read, never written here) */
					extern unsigned se_kept_hits, se_kept_asked;	/* world.c */
					static unsigned hits, asked;

					printf ("quakestat: the server's moves through a model's hull: %u of %u kept from before\n",
						se_kept_hits - hits, se_kept_asked - asked);
					hits = se_kept_hits, asked = se_kept_asked;
				}
				se_server_cycles = se_server_frames = se_server_passes = se_server_between = se_server_held = se_server_behind = 0;
			}
			if (se_scene_frames) {
				printf ("quakestat: %u of these frames drawn by a worker core, %u instructions each, %u waited for; here %u between two frames and %u waiting, a frame\n",
					se_scene_frames, se_scene_instructions / se_scene_frames, se_scene_waits, se_scene_window / se_scene_frames,
					se_scene_waited / se_scene_frames);
				{	/* (the drawing worker's clock: read, never written here) */
					extern unsigned se_draw_ticks[3], se_scene_wait_ticks, mcw_faults;
					static unsigned was[3], faults;
					unsigned d[3], k;

					for (k = 0; k < 3; k++)
						d[k] = (se_draw_ticks[k] - was[k]) * 2 / se_scene_frames, was[k] = se_draw_ticks[k];
					printf ("quakestat: a frame in tenths of a ms of the clock: drawn in %u, of them %u in the swap; the drawing worker idle %u, this core waiting for it %u; %u pages brought for workers\n",
						d[0], d[1], d[2], se_scene_wait_ticks * 2 / se_scene_frames, mcw_faults - faults);
					se_scene_wait_ticks = 0;
					faults = mcw_faults;
				}
				{	/* (the recording worker's count: read, never written here) */
					static unsigned was;

					if (se_scene_unrecorded != was)
						printf ("quakestat: %u frames without their 2D screen (not recorded)\n", se_scene_unrecorded - was);
					was = se_scene_unrecorded;
				}
				se_scene_frames = se_scene_waits = se_scene_instructions = se_scene_window = se_scene_waited = 0;
			}
			{	/* (counted where the models are drawn, a worker too: read, never written here) */
				static unsigned was[4];
				unsigned d[4] = {se_ahead_used - was[0], se_ahead_late - was[1], se_ahead_own - was[2], se_ahead_wrong - was[3]};

				if (d[0] || d[1] || d[2])
					printf ("quakestat: the light under models that moved: %u walks a worker made, %u made here while it had not, %u made here and not given out; %u not as made here (QUAKE_LIGHTPOINT=check)\n",
						d[0], d[1], d[2], d[3]);
				was[0] += d[0], was[1] += d[1], was[2] += d[2], was[3] += d[3];
			}
#ifdef SE_COUNT
			printf ("quakestat: a frame's server: %u point tests of %u nodes, %u moves, %u point contents, %u links, %u QuakeC calls of %u statements; %u water and %u sky vertices\n",
				se_count[0] / n, se_count[1] / n, se_count[2] / n, se_count[3] / n, se_count[4] / n, se_count[5] / n, se_count[6] / n,
				se_count[8] / n, se_count[9] / n);
			memset (se_count, 0, sizeof se_count);
#endif
		}
		fflush (stdout);
		since_ms = now, since_cycles = cycles (), since_frames = frames;
	}
}

/* The console command "hold": the frame after next stays, as with QUAKE_HOLD. */
static void hold_command (void)
{
	hold_frame = frames + 2;
}

/* GLQuake's brightening of the palette: 0.7 unless -gamma says (1 leaves it as it is). */
static void gamma_palette (unsigned char *palette)
{
	float	gamma = 0.7, f;
	int	i;

	if ((i = COM_CheckParm ("-gamma")) != 0)
		gamma = Q_atof (com_argv[i + 1]);
	if (gamma == 1)
		return;
	if (gamma == 0.7f) {
		/* what the sums below come to for 0.7, by byte: they are 768 powers in doubles, a
		   twentieth of the time from the command to the first picture */
		static const unsigned char brighter[256] = {
	5, 9, 11, 14, 16, 18, 21, 23, 24, 26, 28, 30, 32, 33, 35, 37, 38, 40, 41, 43,
	44, 46, 47, 49, 50, 51, 53, 54, 56, 57, 58, 59, 61, 62, 63, 65, 66, 67, 68, 70,
	71, 72, 73, 74, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88, 89, 90, 91, 92,
	93, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113,
	114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 126, 127, 128, 129, 130, 131, 132,
	133, 134, 135, 136, 137, 138, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 147, 148, 149, 150,
	151, 152, 153, 154, 154, 155, 156, 157, 158, 159, 160, 160, 161, 162, 163, 164, 165, 165, 166, 167,
	168, 169, 170, 170, 171, 172, 173, 174, 175, 175, 176, 177, 178, 179, 179, 180, 181, 182, 183, 184,
	184, 185, 186, 187, 188, 188, 189, 190, 191, 191, 192, 193, 194, 195, 195, 196, 197, 198, 199, 199,
	200, 201, 202, 202, 203, 204, 205, 205, 206, 207, 208, 208, 209, 210, 211, 212, 212, 213, 214, 215,
	215, 216, 217, 218, 218, 219, 220, 221, 221, 222, 223, 223, 224, 225, 226, 226, 227, 228, 229, 229,
	230, 231, 232, 232, 233, 234, 234, 235, 236, 237, 237, 238, 239, 239, 240, 241, 242, 242, 243, 244,
	244, 245, 246, 247, 247, 248, 249, 249, 250, 251, 252, 252, 253, 254, 254, 255,
		};

		for (i = 0; i < 768; i++)
			palette[i] = brighter[palette[i]];
		return;
	}
	for (i = 0; i < 768; i++) {
		f = pow ((palette[i] + 1) / 256.0, gamma) * 255 + 0.5;
		palette[i] = f < 0 ? 0 : f > 255 ? 255 : (int)f;
	}
}

void VID_Init (unsigned char *palette)
{
	GR_SCREEN_INFO	info;
	int		i, width = 640, height = 480, bare;
	const char	*s;

	Cvar_RegisterVariable (&in_mouse);
	Cvar_RegisterVariable (&gl_ztrick);
	Cmd_AddCommand ("hold", hold_command);

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));

	if (GrOpen () < 0)
		Sys_Error ("no Nano-X server (start one: nano-X -p &)");
	GrGetScreenInfo (&info);
	if ((i = COM_CheckParm ("-width")) != 0)
		width = atoi (com_argv[i + 1]);
	if ((i = COM_CheckParm ("-height")) != 0)
		height = atoi (com_argv[i + 1]);
	if (width > info.cols)
		width = info.cols;
	if (height > info.rows)
		height = info.rows;
	/* a window that leaves no room for a frame and caption has none, and sits in the corner */
	bare = width + 2 * FRAME > info.cols || height + CAPTION + FRAME > info.rows;
	window = GrNewWindowEx (bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		"Quake", GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width, height, 0);
	GrSelectEvents (window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE);
	GrMapWindow (window);
	GrSetFocus (window);
	if (qglOpen (window, palette) < 0)
		Sys_Error ("this machine has no GPU");
	opened = 1;

	/* the 2D screen is half the window's size from 640 across, so that its text can be read */
	if ((i = COM_CheckParm ("-conwidth")) != 0)
		vid.conwidth = Q_atoi (com_argv[i + 1]);
	else
		vid.conwidth = width >= 640 ? width / 2 : width;
	vid.conwidth &= 0xfff8;
	if (vid.conwidth < 320)
		vid.conwidth = 320;
	vid.conheight = vid.conwidth * height / width;
	if ((i = COM_CheckParm ("-conheight")) != 0)
		vid.conheight = Q_atoi (com_argv[i + 1]);
	if (vid.conheight < 200)
		vid.conheight = 200;
	vid.width = vid.conwidth;
	vid.height = vid.conheight;
	vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
	vid.numpages = 2;

	if ((s = getenv ("QUAKE_HOLD")) != NULL)
		hold_frame = atoi (s);
	hold_late = getenv ("QUAKE_HOLD_LATE") != NULL;
	if ((s = getenv ("QUAKE_STATS_MS")) != NULL)
		stats_ms = atoi (s);
	if ((s = getenv ("QUAKE_STATS_FRAMES")) != NULL)
		stats_frames = atoi (s);	/* a line every so many frames instead: the same frames each run */

	GL_Init ();
	gamma_palette (palette);
	VID_SetPalette (palette);
	Con_SafePrintf ("Video mode %dx%d initialized.\n", width, height);
	vid.recalc_refdef = 1;
}

/* ---- input ---- */

static int quake_key (int ch)
{
	static const char shifted[] = ")!@#$%^&*(";
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, K_LEFTARROW}, {MWKEY_RIGHT, K_RIGHTARROW}, {MWKEY_UP, K_UPARROW}, {MWKEY_DOWN, K_DOWNARROW},
		{MWKEY_ENTER, K_ENTER}, {'\n', K_ENTER}, {MWKEY_KP_ENTER, K_ENTER}, {MWKEY_ESCAPE, K_ESCAPE},
		{MWKEY_BACKSPACE, K_BACKSPACE}, {MWKEY_TAB, K_TAB}, {MWKEY_INSERT, K_INS}, {MWKEY_DELETE, K_DEL},
		{MWKEY_HOME, K_HOME}, {MWKEY_END, K_END}, {MWKEY_PAGEUP, K_PGUP}, {MWKEY_PAGEDOWN, K_PGDN},
		{MWKEY_LSHIFT, K_SHIFT}, {MWKEY_RSHIFT, K_SHIFT}, {MWKEY_LCTRL, K_CTRL}, {MWKEY_RCTRL, K_CTRL},
		{MWKEY_LALT, K_ALT}, {MWKEY_RALT, K_ALT}, {MWKEY_PAUSE, K_PAUSE},
	};
	const char *at;
	unsigned i;

	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A' + 'a';
	if (ch >= MWKEY_F1 && ch <= MWKEY_F12)
		return K_F1 + ch - MWKEY_F1;
	for (i = 0; i < sizeof other / sizeof other[0]; i++)
		if (other[i].ch == ch)
			return other[i].key;
	if (ch > 0 && ch < 128 && (at = strchr (shifted, ch)) != NULL)
		return '0' + (int)(at - shifted);
	return ch >= 32 && ch < 127 ? ch : 0;
}

/*
 * Asking the server costs two system calls and two task switches. The machine's own input
 * counters say when there can be anything: ask when they have moved, for a few calls after,
 * and a few times a second. The pointer's travel is read from the machine's input words
 * themselves (docs/machine/input.md): the server's word of every move was a question and an answer
 * each, in every frame the view was turned in. The server says when the pointer comes over
 * the window and leaves it.
 */
void Sys_SendKeyEvents (void)
{
	static unsigned seen[4], asked_ms;
	static int ask = 8;
	const volatile unsigned *input;
	GR_EVENT e;
	int i, key;

	if (!opened)
		return;
	input = qglControl (REG_INPUT);
	if (mouse_inside || dragging) {		/* (a drag goes on outside the window) */
		int x = (int)input[0], y = (int)input[1];

		if (mouse_known)
			mouse_dx += x - mouse_x, mouse_dy += y - mouse_y;
		mouse_x = x, mouse_y = y, mouse_known = 1;
	}
	for (i = 2; i < 4; i++)		/* the buttons, and the count of keys */
		if (seen[i] != input[i]) {
			seen[i] = input[i];
			ask = 8;
		}
	if (!ask && VID_Milliseconds () - asked_ms >= 250)
		ask = 1;
	while (ask) {
		GrCheckNextEvent (&e);
		switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = VID_Milliseconds ();
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_inside = 1;
			mouse_known = 0;	/* from where it is now */
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			mouse_inside = 0;
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.utype == GR_UPDATE_SIZE) {
				SE_SceneOwn ();
				seglWindowChanged ();
				vid.recalc_refdef = 1;
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			/* In the game the left button drags the view round, as a finger on glass does, and
			   the right one is what the left was (fire). A sitter's buttons are the game's own. */
			if (key_dest == key_game && !seated) {
				if (e.button.changebuttons & GR_BUTTON_L) {
					dragging = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
					mouse_dx = mouse_dy = 0;
				}
				if (e.button.changebuttons & GR_BUTTON_R)
					Key_Event (K_MOUSE1, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
				break;
			}
			/* a sitter fires by the trigger: a click on the room's screen is no shot (their release still counts) */
			if (key_dest == key_game && seated && !cl.intermission && e.type == GR_EVENT_TYPE_BUTTON_DOWN)
				break;
			dragging = 0;
			if (e.button.changebuttons & GR_BUTTON_L)
				Key_Event (K_MOUSE1, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
			if (e.button.changebuttons & GR_BUTTON_R)
				Key_Event (K_MOUSE2, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
			if (e.button.changebuttons & GR_BUTTON_M)
				Key_Event (K_MOUSE3, e.type == GR_EVENT_TYPE_BUTTON_DOWN);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			key = quake_key (e.keystroke.ch);
			if (key)
				Key_Event (key, e.type == GR_EVENT_TYPE_KEY_DOWN);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Sys_Quit ();
		}
	}
}

void IN_Init (void) {}
void IN_Shutdown (void) {}
void IN_Commands (void) {}

static void pitch_to (float pitch)
{
	V_StopPitchDrift ();
	cl.viewangles[PITCH] = pitch > 80 ? 80 : pitch < -70 ? -70 : pitch;
}

/* A sitter's button from the host's words, on the game's own: down and up as its key would be. */
static void word_button (kbutton_t *button, int *was, int now)
{
	if (now == *was)
		return;
	*was = now;
	if (now)
		button->state |= 1 + 2;
	else if (!button->down[0] && !button->down[1])
		button->state = (button->state & ~1) | 4;
}

/* The view's turn. From a holodeck's seat with tracked hands the right hand aims, and the walk
 * stays the body's; with a mouse its travel turns and the head's tilt is the aim's rise. On a
 * flat screen the pointer dragged with the left button turns the view (in_mouse 1: its travel). */
void IN_Move (usercmd_t *cmd)
{
	const volatile unsigned *host = seglHostState ();
	float mx = mouse_dx * sensitivity.value, my = mouse_dy * sensitivity.value;
	int now = !host || !SEGL_HOST_HOLODECK (host) ? 0 : SEGL_HOST_HANDS (host) ? 1 : SEGL_HOST_KIND (host) == 2 ? 2 : 0;

	mouse_dx = mouse_dy = 0;
	V_StopPitchDrift ();	/* where the view was left it stays: nothing levels it */
	if (now != seated) {
		seated = now;
		dragging = 0;
		body = aimed = cl.viewangles[YAW];
		mouse_count = host ? SEGL_HOST_MOUSE_X (host) : 0;
	}
	/* the eye 46 units over the floor (22 over a player's origin, 24 over its feet); near 4, far 4096 */
	if (host && r_refdef.fov_x > 0 && r_refdef.fov_y > 0)
		seglWorldEye (2, 46, 4, 4096, 1 / tan (r_refdef.fov_x * (M_PI / 360)), 1 / tan (r_refdef.fov_y * (M_PI / 360)));
	{	/* a sitter's fire and jump: the right trigger and the jump button, or a desktop's keys for them */
		extern kbutton_t in_attack, in_jump;
		static int fire, jump;
		int play = seated && key_dest == key_game;

		word_button (&in_attack, &fire, play && SEGL_HOST_HAND_TRIGGER (host, 1));
		word_button (&in_jump, &jump, play && SEGL_HOST_JUMP (host));
	}
	if (seated == 1) {
		float turn = SEGL_HOST_HAND_YAW (host, 1) * (360.0f / 65536.0f), forward, s, c;
		float look = SEGL_HOST_STICK (host, 2) / 127.0f, walk = (turn - SEGL_HOST_HEAD_YAW (host) * (360.0f / 65536.0f)) * (M_PI / 180);

		/* (what the keys or the game turned the aim by since, the body turned; and the right stick) */
		body += cl.viewangles[YAW] - aimed;
		if (key_dest == key_game) {
			body -= (fabs (look) < 0.15f ? 0 : look * fabs (look)) * 140 * host_frametime;
			cmd->forwardmove += SEGL_HOST_STICK (host, 1) * (400 / 127.0f);
			cmd->sidemove += SEGL_HOST_STICK (host, 0) * (350 / 127.0f);
		}
		cl.viewangles[YAW] = aimed = body - turn;
		pitch_to (-SEGL_HOST_HAND_PITCH (host, 1) * (360.0f / 65536.0f));
		/* the walk goes the way the head looks: so far to the left of the aim */
		forward = cmd->forwardmove, s = sin (walk), c = cos (walk);
		cmd->forwardmove = forward * c + cmd->sidemove * s;
		cmd->sidemove = cmd->sidemove * c - forward * s;
		return;
	}
	if (seated == 2) {
		unsigned count = SEGL_HOST_MOUSE_X (host);

		if (key_dest == key_game) {
			cl.viewangles[YAW] -= m_yaw.value * sensitivity.value * (short)(count - mouse_count);
			/* (the keys they walk by come as the left stick: no window need have the keyboard) */
			cmd->forwardmove += SEGL_HOST_STICK (host, 1) * (400 / 127.0f);
			cmd->sidemove += SEGL_HOST_STICK (host, 0) * (350 / 127.0f);
		}
		mouse_count = count;
		pitch_to (-SEGL_HOST_HEAD_PITCH (host) * (360.0f / 65536.0f));
		return;
	}
	if (dragging && key_dest == key_game) {
		/* a quarter of a degree a pixel: the window's width is a hundred and sixty degrees */
		cl.viewangles[YAW] -= 0.25f * mx / sensitivity.value;
		pitch_to (cl.viewangles[PITCH] + 0.25f * my / sensitivity.value);
		return;
	}
	if (!in_mouse.value || key_dest != key_game)
		return;
	if ((in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1)))
		cmd->sidemove += m_side.value * mx;
	else
		cl.viewangles[YAW] -= m_yaw.value * mx;
	if (in_mlook.state & 1)
		V_StopPitchDrift ();
	if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
		cl.viewangles[PITCH] += m_pitch.value * my;
		if (cl.viewangles[PITCH] > 80)
			cl.viewangles[PITCH] = 80;
		if (cl.viewangles[PITCH] < -70)
			cl.viewangles[PITCH] = -70;
	} else {
		cmd->forwardmove -= m_forward.value * my;
	}
}

/* Units a metre of the room's: its plane, 1.28 by 0.8 m, is `at` from the camera (GpuVolumePass.cginc). */
static float seat_metre (const volatile unsigned *host)
{
	float at = 4 * pow (4096 / 4, SEGL_HOST_PLANE (host) / 255.0f);
	float x = 1.28f / tan (r_refdef.fov_x * (M_PI / 360)), y = 0.8f / tan (r_refdef.fov_y * (M_PI / 360));

	return 2 * at / (x < y ? x : y);
}

/* Where a hand is in the world (0 left, 1 right): from the eye, in the body's frame. */
static void seat_hand (const volatile unsigned *host, int h, vec3_t hand)
{
	vec3_t	level = {0, body, 0}, forward, right, up;
	float	unit = 0.004f * seat_metre (host);
	int	i;

	AngleVectors (level, forward, right, up);
	for (i = 0; i < 3; i++)
		hand[i] = r_refdef.vieworg[i] + (forward[i] * SEGL_HOST_HAND_Z (host, h) + right[i] * SEGL_HOST_HAND_X (host, h)
			+ up[i] * SEGL_HOST_HAND_Y (host, h)) * unit;
}

/* ---- the status in the room (docs/world/holodeck.md, "The status in the room") ---- */

typedef struct { int texnum; float sl, tl, sh, th; } hud_pic;	/* gl_draw.c's glpic_t */
extern qpic_t	*sb_nums[2][11];
extern int	char_texture, con_current, con_linewidth, con_totallines;
extern char	*con_text, scr_centerstring[];
extern unsigned	con_times[];
extern float	scr_centertime_off;
extern cvar_t	con_notifytime;

/* A picture's part standing at `at`, facing the sitter: `wide` by `high` units, its middle `x` to the right. */
static void hud_quad (int texture, float sl, float tl, float sh, float th, vec3_t at, float x, float wide, float high)
{
	vec3_t	level = {0, body, 0}, forward, right, up, point;
	int	i, k;

	AngleVectors (level, forward, right, up);
	GL_Bind (texture);
	glBegin (GL_QUADS);
	for (k = 0; k < 4; k++) {
		float across = k < 2 ? x - wide / 2 : x + wide / 2, rise = k == 1 || k == 2 ? high / 2 : -high / 2;

		glTexCoord2f (k < 2 ? sl : sh, k == 1 || k == 2 ? tl : th);
		for (i = 0; i < 3; i++)
			point[i] = at[i] + right[i] * across + up[i] * rise;
		glVertex3fv (point);
	}
	glEnd ();
}

/* A number in the status bar's digits, its middle at `at`, a digit `high` units tall; red when `red`. */
static void hud_number (vec3_t at, int value, float high, int red)
{
	char	digits[12];
	int	i, n = sprintf (digits, "%d", value < 0 ? 0 : value);
	float	wide = high * 0.9f;

	for (i = 0; i < n; i++) {
		hud_pic *pic = (hud_pic *)sb_nums[red ? 1 : 0][digits[i] - '0']->data;

		hud_quad (pic->texnum, pic->sl, pic->tl, pic->sh, pic->th, at, (i - (n - 1) * 0.5f) * wide, wide, high);
	}
}

/* A line of the game's letters, its middle at `at`, a letter `high` units tall. */
static void hud_line (vec3_t at, const char *text, int count, float high)
{
	int	i, last = count;

	while (last > 0 && (text[last - 1] & 127) <= ' ')
		last--;
	for (i = 0; i < last; i++) {
		int c = text[i] & 255;

		if ((c & 127) == ' ')
			continue;
		hud_quad (char_texture, (c & 15) * 0.0625f, (c >> 4) * 0.0625f, (c & 15) * 0.0625f + 0.0625f, (c >> 4) * 0.0625f + 0.0625f,
			at, (i - (last - 1) * 0.5f) * high, high, high);
	}
}

/* A sitter with tracked hands has no flat screen: the ammunition stands over the weapon, health
 * and armour over the left hand, and what the game says hangs before them, a little under the eyes. */
void SE_Hud (void)
{
	const volatile unsigned *host = seglHostState ();
	vec3_t	level = {0, body, 0}, forward, right, up, at;
	float	metre;
	int	i, line, lines = 0;
	char	*text;

	if (seated != 1 || !host || cl.intermission || cl.stats[STAT_HEALTH] <= 0)
		return;
	metre = seat_metre (host);
	AngleVectors (level, forward, right, up);
	glEnable (GL_ALPHA_TEST);
	glColor3f (1, 1, 1);
	if (!(cl.stats[STAT_ACTIVEWEAPON] & IT_AXE)) {
		seat_hand (host, 1, at);
		for (i = 0; i < 3; i++)
			at[i] += up[i] * 0.12f * metre;
		hud_number (at, cl.stats[STAT_AMMO], 0.035f * metre, cl.stats[STAT_AMMO] <= 10);
	}
	seat_hand (host, 0, at);
	for (i = 0; i < 3; i++)
		at[i] += up[i] * 0.10f * metre;
	hud_number (at, cl.stats[STAT_HEALTH], 0.04f * metre, cl.stats[STAT_HEALTH] <= 25);
	if (cl.stats[STAT_ARMOR] > 0) {
		for (i = 0; i < 3; i++)
			at[i] += up[i] * 0.05f * metre;
		hud_number (at, cl.stats[STAT_ARMOR], 0.028f * metre, 0);
	}
	/* the last lines said, newest lowest, 1.2 m ahead; under them what is printed mid-screen */
	for (line = con_current; line > con_current - 4 && line >= 0; line--) {
		unsigned made = con_times[line % 4];

		if (!made || REALTIME_SINCE (made) > con_notifytime.value)
			continue;
		text = con_text + (line % con_totallines) * con_linewidth;
		for (i = 0; i < 3; i++)
			at[i] = r_refdef.vieworg[i] + (forward[i] * 1.2f + up[i] * (-0.12f + 0.04f * lines)) * metre;
		hud_line (at, text, con_linewidth, 0.03f * metre);
		lines++;
	}
	if (scr_centertime_off > 0) {
		for (text = scr_centerstring, line = 0; *text && line < 6; line++) {
			int count = strcspn (text, "\n");

			for (i = 0; i < 3; i++)
				at[i] = r_refdef.vieworg[i] + (forward[i] * 1.2f + up[i] * (-0.20f - 0.04f * line)) * metre;
			hud_line (at, text, count, 0.03f * metre);
			text += count + (text[count] != 0);
		}
	}
	glDisable (GL_ALPHA_TEST);
}

/* After the view is worked out, for a sitter with tracked hands: the room's picture is level
 * and turned as the body is, and the weapon is in the right hand, pointing as it points. */
void SE_View (void)
{
	const volatile unsigned *host = seglHostState ();
	entity_t	*view = &cl.viewent;
	vec3_t		level = {0, body, 0}, forward, right, up, hand;
	float		at, metre, x, y, z;
	int		i;

	static int	said = -1;

	if (said < 0)
		said = getenv ("QUAKE_SEAT") != NULL;	/* a line every 20 frames: how it is seated, the aim, the body */
	if (said && ++said % 20 == 0)
		Sys_Printf ("seat: %d aim %.1f %.1f body %.1f drag %d at %.0f %.0f %.0f eye %08x\n", seated, cl.viewangles[YAW], cl.viewangles[PITCH], body, dragging,
			cl_entities[cl.viewentity].origin[0], cl_entities[cl.viewentity].origin[1], cl_entities[cl.viewentity].origin[2], host ? host[(0x6c - 0x360) / 4] : 0);
	if (seated != 1 || !host)
		return;
	VectorCopy (level, r_refdef.viewangles);
	metre = seat_metre (host);
	seat_hand (host, 1, hand);
	view->angles[PITCH] = -cl.viewangles[PITCH];
	view->angles[YAW] = cl.viewangles[YAW];
	view->angles[ROLL] = 0;
	VectorCopy (hand, view->origin);
	if (view->model) {
		/* the hand holds it a third of its length from its back end, at its middle */
		aliashdr_t *hdr = (aliashdr_t *)Mod_Extradata (view->model);
		float gx = hdr->scale_origin[0] + 255 * hdr->scale[0] * 0.33f, gy = hdr->scale_origin[1] + 127.5f * hdr->scale[1];
		float gz = hdr->scale_origin[2] + 127.5f * hdr->scale[2];

		AngleVectors (cl.viewangles, forward, right, up);
		for (i = 0; i < 3; i++)
			view->origin[i] -= forward[i] * gx - right[i] * gy + up[i] * gz;
		/* the axe is swung by the arm that holds it, not by the game */
		if (!strcmp (view->model->name, "progs/v_axe.mdl"))
			view->frame = 0;
		if (said && said % 20 == 0)
			Sys_Printf ("seat: gun %.1f %.1f %.1f from the eye, a metre %.1f\n", view->origin[0] - r_refdef.vieworg[0],
				view->origin[1] - r_refdef.vieworg[1], view->origin[2] - r_refdef.vieworg[2], metre);
	}
}
