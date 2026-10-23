/*
 * Quake III Arena's renderer on the machine's GPU (docs/games/quake3.md), in place of the game's own:
 * nothing of a frame is worked out here that the GPU can read for itself. The level's
 * triangles are in GPU memory from when it loads, sorted by what they are drawn with, and
 * are commands that stay in every frame's list; a texture, a light map and a model's pose
 * are read where bake.py's file lies in the ROM; a model is lit by a table of colours, one
 * for each way a vertex can face. A frame says where the eye is and what moved.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "client/client.h"
#include "renderercommon/tr_public.h"
#include <GLES/segl.h>
#include "nano-X.h"
#include "gpu_shaderemu.h"
#include "mcwx.h"
#include "media.h"
#include "se_data.h"
#include "se_q3.h"

#define INLINE_MODEL	0x4000		/* a handle from here on is one of the level's own models */
#define MAX_SCENE	1024		/* entities of a scene */
#define MAX_POLYS	1024
#define MAX_POLY_VERTS	6000
#define Z_NEAR		4.0f
#define Z_FAR		16384.0f

static refimport_t	ri;
static glconfig_t	config;
static GR_WINDOW_ID	window;
static int		opened, width = 640, height = 480;

/* ---- the file ---- */

static const unsigned char *dat;
static unsigned		dat_rom;	/* its first byte as the GPU addresses it */
static const se_texture_t *textures;
static const se_shader_t *shaders, **shader_of;
static const se_stage_t	*stages;
static const se_model_t	*models;
static const se_surface_t *surfaces;
static const se_skin_t	*skins;
static const se_skin_entry_t *skin_entries;
static const se_world_t	*world;
static const se_run_t	*runs;
static const se_bmodel_t *bmodels;
static const float	*normals;
static int		shader_count, model_count, skin_count, stage_count, texture_count;

static void stages_open (void);
static void pictures_open (void);

static const void *section (const char *tag, unsigned each, int *count)
{
	const void	*at = SE_DataSection (tag, each, count);

	if (!at)
		ri.Error (ERR_FATAL, "%s has no %s", SE_DATA_FILE, tag);
	return at;
}

static void data_open (void)
{
	int	i, count;

	dat = SE_Data (&dat_rom);
	if (!dat)
		ri.Error (ERR_FATAL, "no %s of bake.py's", SE_DATA_FILE);
	if (!dat_rom)
		ri.Error (ERR_FATAL, "%s is not a file of the ROM: the GPU reads it there", SE_DATA_FILE);
	if (((const se_header_t *)dat)->version != SE_VERSION)
		ri.Error (ERR_FATAL, "%s is not of this build's bake.py", SE_DATA_FILE);
	textures = section ("TEXS", sizeof (se_texture_t), &texture_count);
	shaders = section ("SHAD", sizeof (se_shader_t), &shader_count);
	stages = section ("STAG", sizeof (se_stage_t), &stage_count);
	models = section ("MODL", sizeof (se_model_t), &model_count);
	surfaces = section ("SURF", sizeof (se_surface_t), NULL);
	skins = section ("SKIN", sizeof (se_skin_t), &skin_count);
	skin_entries = section ("SKNE", sizeof (se_skin_entry_t), NULL);
	world = section ("WHDR", sizeof (se_world_t), NULL);
	runs = section ("WRUN", sizeof (se_run_t), NULL);
	bmodels = section ("WBMD", sizeof (se_bmodel_t), NULL);
	normals = section ("NRML", 12, &count);
	shader_of = malloc (shader_count * sizeof *shader_of);
	for (i = 0; i < shader_count; i++)
		shader_of[shaders[i].number] = &shaders[i];
	pictures_open ();
	stages_open ();
}

/* A game path as the file's tables have it: lower case, forward slashes, and with `strip` no extension. */
static void clean_name (char *out, const char *name, int strip)
{
	int	i, dot = -1;

	for (i = 0; i < 63 && name[i]; i++) {
		char c = name[i];

		out[i] = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? c + 32 : c;
		if (c == '.')
			dot = i;
		else if (c == '/' || c == '\\')
			dot = -1;
	}
	out[strip && dot >= 0 ? dot : i] = 0;
}

/* The entry of that name in a table sorted by name (each begins with its name), or -1. */
static int find (const void *table, int count, int each, const char *name)
{
	int	low = 0, high = count - 1;

	while (low <= high) {
		int	mid = (low + high) >> 1;
		int	c = strcmp (name, (const char *)table + mid * each);

		if (!c)
			return mid;
		if (c < 0)
			high = mid - 1;
		else
			low = mid + 1;
	}
	return -1;
}

/* ---- the window and the input ---- */

unsigned SE_WindowClock (void)
{
	const volatile unsigned	*clock = opened ? gpuControl (REG_CLOCK) : NULL;

	return clock ? *clock : 0;
}

void SE_Settle (void)
{
	SE_SceneFlush ();
	if (opened)
		seglSettle ();
}

int SE_ClientPlaying (void)
{
	return clc.state == CA_ACTIVE && !clc.demoplaying;
}

static int		mouse_x, mouse_y, mouse_known, mouse_inside = 1;
static int		origin_x, origin_y, origin_known, mouse_menu;	/* the window's place on the display */
static int		dragging;	/* the left button is held on the game: the pointer's travel turns the view */
/* A sitter in a holodeck's seat (docs/world/holodeck.md, "Played from the seat"): 1 with tracked
   hands, 2 with a mouse. The room is turned by `body`; the aim is the hand's from there. */
static int		seated;
static float		body, aimed, seat_fov_x = 90, seat_fov_y = 73.74f;
static unsigned		mouse_count;
static int		window_w, window_h;	/* its size now: the game's picture is config's, stretched to it */

/* A rectangle of the game's picture (y down) as the window's pixels. */
static void picture_viewport (int x, int y, int w, int h)
{
	int	x0 = x * window_w / config.vidWidth, x1 = (x + w) * window_w / config.vidWidth;
	int	y0 = y * window_h / config.vidHeight, y1 = (y + h) * window_h / config.vidHeight;

	glViewport (x0, window_h - y1, x1 - x0, y1 - y0);
}

static int game_key (int ch)
{
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, K_LEFTARROW}, {MWKEY_RIGHT, K_RIGHTARROW}, {MWKEY_UP, K_UPARROW}, {MWKEY_DOWN, K_DOWNARROW},
		{MWKEY_ENTER, K_ENTER}, {'\n', K_ENTER}, {'\r', K_ENTER}, {MWKEY_KP_ENTER, K_KP_ENTER}, {MWKEY_ESCAPE, K_ESCAPE},
		{MWKEY_BACKSPACE, K_BACKSPACE}, {MWKEY_TAB, K_TAB}, {MWKEY_INSERT, K_INS}, {MWKEY_DELETE, K_DEL},
		{MWKEY_HOME, K_HOME}, {MWKEY_END, K_END}, {MWKEY_PAGEUP, K_PGUP}, {MWKEY_PAGEDOWN, K_PGDN},
		{MWKEY_LSHIFT, K_SHIFT}, {MWKEY_RSHIFT, K_SHIFT}, {MWKEY_LCTRL, K_CTRL}, {MWKEY_RCTRL, K_CTRL},
		{MWKEY_LALT, K_ALT}, {MWKEY_RALT, K_ALT}, {MWKEY_PAUSE, K_PAUSE}, {'`', K_CONSOLE}, {'~', K_CONSOLE},
	};
	unsigned	i;

	if (ch >= 'A' && ch <= 'Z')
		return ch - 'A' + 'a';
	if (ch >= MWKEY_F1 && ch <= MWKEY_F12)
		return K_F1 + ch - MWKEY_F1;
	for (i = 0; i < ARRAY_LEN (other); i++)
		if (other[i].ch == ch)
			return other[i].key;
	return ch >= 32 && ch < 127 ? ch : 0;
}

/*
 * The window's keys and buttons as the game's events, and the pointer's travel over the
 * window as its mouse. Asking the window system costs system calls: asked when the machine's
 * own input counters have moved, and a few times a second (linux/quake/vid_shaderemu.c).
 */
void SE_Input (void)
{
	static unsigned	seen[4], asked_ms;
	static int	ask = 8;
	const volatile unsigned *input, *host;
	GR_EVENT	e;
	int		i, key, down;

	if (!opened)
		return;
	input = gpuControl (REG_INPUT);
	host = seglHostState ();
	/* a menu, the console, a level loading: a picture for a screen, not a world for a room */
	/* (and a level's end, its scoreboard: PM_INTERMISSION and PM_SPINTERMISSION are 5 and 6) */
	seglWorldFlat (Key_GetCatcher () != 0 || clc.state != CA_ACTIVE || (cl.snap.valid && cl.snap.ps.pm_type >= 5));
	/* the eye 50 units over the floor (26 over a player's origin, 24 over its feet) */
	seglWorldEye (7, 50, Z_NEAR, Z_FAR, 1 / tan (seat_fov_x * (M_PI / 360)), 1 / tan (seat_fov_y * (M_PI / 360)));
	if (mouse_inside || dragging) {		/* (a drag goes on outside the window) */
		int x = (int)input[0], y = (int)input[1];

		int menu = (Key_GetCatcher () & KEYCATCH_UI) != 0;

		/* a menu's cursor is the pointer itself: it is told where in the window, not how far (ui_atoms.c) */
		if (menu && (!origin_known || !mouse_menu || x != mouse_x || y != mouse_y)) {
			if (!origin_known)
				gpuWindowOrigin (&origin_x, &origin_y), origin_known = 1;
			Com_QueueEvent (0, SE_MOUSE, (x - origin_x) * config.vidWidth / window_w, (y - origin_y) * config.vidHeight / window_h, 0, NULL);
		} else if (!menu && dragging && !seated && mouse_known && (x != mouse_x || y != mouse_y))
			Com_QueueEvent (0, SE_MOUSE, (x - mouse_x) * 2, (y - mouse_y) * 2, 0, NULL);	/* 0.22 degrees a pixel */
		mouse_x = x, mouse_y = y, mouse_known = 1, mouse_menu = menu;
	}
	for (i = 2; i < 4; i++)		/* the buttons, and the count of keys */
		if (seen[i] != input[i]) {
			seen[i] = input[i];
			ask = 8;
		}
	if (!ask && SE_WindowClock () - asked_ms >= 250)
		ask = 1;
	while (ask) {
		GrCheckNextEvent (&e);
		switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = SE_WindowClock ();
			return;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			mouse_inside = 1;
			mouse_known = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			mouse_inside = 0;
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.utype == GR_UPDATE_SIZE) {
				SE_SceneFlush ();
				seglWindowChanged ();
				seglSize (&window_w, &window_h);
				if (window_w < 1 || window_h < 1)
					window_w = config.vidWidth, window_h = config.vidHeight;
			}
			origin_known = 0;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			origin_x = e.button.rootx - e.button.x, origin_y = e.button.rooty - e.button.y, origin_known = 1;
			/* In the game the left button drags the view round, as a finger on glass does, and
			   the right one is what the left was (fire). A sitter's buttons are the game's own. */
			if (!Key_GetCatcher () && !seated) {
				if (e.button.changebuttons & GR_BUTTON_L)
					dragging = down, mouse_known = 0;
				if (e.button.changebuttons & GR_BUTTON_R)
					Com_QueueEvent (0, SE_KEY, K_MOUSE1, down, 0, NULL);
				break;
			}
			/* a sitter fires by the trigger: a click on the room's screen is no shot (their release still counts) */
			if (!Key_GetCatcher () && seated && down && !(cl.snap.valid && cl.snap.ps.pm_type >= 5))
				break;
			dragging = 0;
			if (e.button.changebuttons & GR_BUTTON_L)
				Com_QueueEvent (0, SE_KEY, K_MOUSE1, down, 0, NULL);
			if (e.button.changebuttons & GR_BUTTON_R)
				Com_QueueEvent (0, SE_KEY, K_MOUSE2, down, 0, NULL);
			if (e.button.changebuttons & GR_BUTTON_M)
				Com_QueueEvent (0, SE_KEY, K_MOUSE3, down, 0, NULL);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			key = game_key (e.keystroke.ch);
			if (key)
				Com_QueueEvent (0, SE_KEY, key, down, 0, NULL);
			if (down && key == K_BACKSPACE)
				Com_QueueEvent (0, SE_CHAR, 'h' - 'a' + 1, 0, 0, NULL);
			else if (down && key != K_CONSOLE && e.keystroke.ch >= 32 && e.keystroke.ch < 127)
				Com_QueueEvent (0, SE_CHAR, e.keystroke.ch, 0, 0, NULL);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			Cbuf_ExecuteText (EXEC_APPEND, "quit\n");
			break;
		}
	}
}

static void pitch_to (float pitch)
{
	cl.viewangles[PITCH] = (pitch > 80 ? 80 : pitch < -80 ? -80 : pitch) - SHORT2ANGLE (cl.snap.ps.delta_angles[PITCH]);
}

/* A move as it is made (cl_input.c). From a holodeck's seat with tracked hands the right hand
 * aims and the walk stays the body's; with a mouse its travel turns, and the head's tilt is
 * the aim's rise. */
void SE_SeatMove (usercmd_t *cmd)
{
	const volatile unsigned *host = seglHostState ();
	int now = !host || !SEGL_HOST_HOLODECK (host) ? 0 : SEGL_HOST_HANDS (host) ? 1 : SEGL_HOST_KIND (host) == 2 ? 2 : 0;
	static int said = -1;

	if (said < 0)
		said = getenv ("QUAKE3_SEAT") != NULL;	/* a line every 20 moves: how it is seated, the aim, the body */
	if (said && ++said % 20 == 0)
		Com_Printf ("seat: %d aim %.1f %.1f body %.1f drag %d at %.0f %.0f %.0f eye %08x\n", seated, cl.viewangles[YAW] + SHORT2ANGLE (cl.snap.ps.delta_angles[YAW]),
			cl.viewangles[PITCH] + SHORT2ANGLE (cl.snap.ps.delta_angles[PITCH]), body + SHORT2ANGLE (cl.snap.ps.delta_angles[YAW]), dragging,
			cl.snap.ps.origin[0], cl.snap.ps.origin[1], cl.snap.ps.origin[2], host ? host[(0x6c - 0x360) / 4] : 0);
	if (now != seated) {
		seated = now;
		dragging = 0;
		body = aimed = cl.viewangles[YAW];
		mouse_count = host ? SEGL_HOST_MOUSE_X (host) : 0;
	}
	if (seated == 1) {
		float turn = SEGL_HOST_HAND_YAW (host, 1) * (360.0f / 65536.0f), forward = cmd->forwardmove, right = cmd->rightmove;
		float look = SEGL_HOST_STICK (host, 2) / 127.0f, walk = (turn - SEGL_HOST_HEAD_YAW (host) * (360.0f / 65536.0f)) * (M_PI / 180);
		float s = sin (walk), c = cos (walk);	/* the walk goes the way the head looks: so far to the left of the aim */

		/* (what the keys turned the aim by since, the body turned; and the right stick) */
		body += cl.viewangles[YAW] - aimed;
		if (!Key_GetCatcher ()) {
			body -= (fabs (look) < 0.15f ? 0 : look * fabs (look)) * 140 * cls.frametime * 0.001f;
			forward += SEGL_HOST_STICK (host, 1);
			right += SEGL_HOST_STICK (host, 0);
		}
		cl.viewangles[YAW] = aimed = body - turn;
		pitch_to (-SEGL_HOST_HAND_PITCH (host, 1) * (360.0f / 65536.0f));
		cmd->forwardmove = ClampChar ((int)(forward * c + right * s));
		cmd->rightmove = ClampChar ((int)(right * c - forward * s));
	} else if (seated == 2) {
		unsigned count = SEGL_HOST_MOUSE_X (host);

		if (!Key_GetCatcher ()) {
			cl.viewangles[YAW] -= 0.066f * (short)(count - mouse_count);
			/* (the keys they walk by come as the left stick: no window need have the keyboard) */
			cmd->forwardmove = ClampChar (cmd->forwardmove + SEGL_HOST_STICK (host, 1));
			cmd->rightmove = ClampChar (cmd->rightmove + SEGL_HOST_STICK (host, 0));
		}
		mouse_count = count;
		pitch_to (-SEGL_HOST_HEAD_PITCH (host) * (360.0f / 65536.0f));
	}
	/* a sitter's fire and jump: the right trigger and the jump button, or a desktop's keys for them */
	if (seated && !Key_GetCatcher ()) {
		if (SEGL_HOST_HAND_TRIGGER (host, 1))
			cmd->buttons |= BUTTON_ATTACK;
		if (SEGL_HOST_JUMP (host))
			cmd->upmove = 127;
	}
}

/* For the client's game: whether its player sits with tracked hands, and then the yaw of the
 * room's view and where the right hand is from the eye, in the game's units: right, up, forward. */
int SE_SeatView (float *yaw, float *hand)
{
	const volatile unsigned *host = seglHostState ();
	float at, x, y, metre;

	if (seated != 1 || !host)
		return 0;
	*yaw = body + SHORT2ANGLE (cl.snap.ps.delta_angles[YAW]);
	/* units a metre: the room's plane, 1.28 by 0.8 m, is `at` from the camera (GpuVolumePass.cginc) */
	at = Z_NEAR * pow (Z_FAR / Z_NEAR, SEGL_HOST_PLANE (host) / 255.0f);
	x = 1.28f / tan (seat_fov_x * (M_PI / 360)), y = 0.8f / tan (seat_fov_y * (M_PI / 360));
	metre = 2 * at / (x < y ? x : y) * 0.004f;
	hand[0] = SEGL_HOST_HAND_X (host, 1) * metre;
	hand[1] = SEGL_HOST_HAND_Y (host, 1) * metre;
	hand[2] = SEGL_HOST_HAND_Z (host, 1) * metre;
	return 1;
}

void IN_Init (void *windowData) {}
void IN_Frame (void) {}
void IN_Shutdown (void) {}
void IN_Restart (void) {}

static void window_open (void)
{
	GR_SCREEN_INFO	info;
	const char	*s;
	int		bare;

	if (GrOpen () < 0)
		ri.Error (ERR_FATAL, "no Nano-X server (start one: nano-X -p &)");
	GrGetScreenInfo (&info);
	if ((s = getenv ("QUAKE3_SIZE")) != NULL)
		sscanf (s, "%dx%d", &width, &height);
	if (width > info.cols)
		width = info.cols;
	if (height > info.rows)
		height = info.rows;
	/* a window that leaves no room for a frame and caption has none, and sits in the corner */
	bare = width + 8 > info.cols || height + 26 > info.rows;
	window = GrNewWindowEx (bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		"Quake III Arena", GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width, height, 0);
	GrSelectEvents (window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE);
	GrMapWindow (window);
	GrSetFocus (window);
	if (seglInit (window) < 0)
		ri.Error (ERR_FATAL, "this machine has no GPU");
	seglWorldUp (3);
	opened = 1;
}

/* ---- numbers ---- */

static inline unsigned float_bits (float f)
{
	union { float f; unsigned u; } c = {f};

	return c.u;
}

static inline float floor_of (float f)
{
	float	whole = (float)(int)f;

	return whole > f ? whole - 1 : whole;
}

static inline unsigned level (float f)
{
	return f <= 0 ? 0 : f >= 255 ? 255 : (unsigned)f;
}

/* A colour as the GPU has it: the top byte is how transparent. */
static inline unsigned colour_word (unsigned r, unsigned g, unsigned b, unsigned a)
{
	return (255 - a) << 24 | r << 16 | g << 8 | b;
}

static float	sines[256];	/* of 256ths of a turn */

static float wave (const se_wave_t *w, float time)
{
	float	x = w->phase + time * w->frequency, v;

	x -= floor_of (x);
	switch (w->function) {
	case SE_TRIANGLE:	v = x < 0.25f ? 4 * x : x < 0.75f ? 2 - 4 * x : 4 * x - 4; break;
	case SE_SQUARE:		v = x < 0.5f ? 1 : -1; break;
	case SE_SAWTOOTH:	v = x; break;
	case SE_INVERSE_SAWTOOTH: v = 1 - x; break;
	case SE_NOISE:		v = 0; break;
	default:		v = sines[(int)(x * 256) & 255]; break;
	}
	return w->base + w->amplitude * v;
}

/* ---- a stage: what the GPU is told of it ---- */

static unsigned	*picture_at;	/* each texture's address as the GPU reads it: GPU memory, the ROM; 0 not decoded */
static float		scene_time;	/* the scene's clock, in seconds */
static int		scene_mode = -1;	/* 1: a frame is drawn by a job on a worker core ("The scene on a worker core") */

/* Its texture now (one of several, by the clock), how its texels are read, and its pass. */
static void stage_state (gpu_state *s, const se_stage_t *st, int flat)
{
	const se_texture_t	*t;
	int			which = 0;

	memset (s, 0, sizeof *s);
	s->colour = 0x00ffffff;
	s->fragment = GPU_PASS (flat ? (st->pass == SE_ADD ? 6 : 5) : st->pass);
	if (st->texture == 0xffff)
		return;
	if (st->frames > 1)
		which = (int)(scene_time * st->fps) % st->frames;
	t = &textures[st->texture + which];
	s->texture = picture_at[st->texture + which];
	if (!s->texture)
		return;		/* (its sheet is not decoded: drawn in its colour) */
	s->width = t->width, s->height = t->height;
	s->fragment |= (t->format == SE_WORDS ? GPU_WORDS : GPU_BYTES) | GPU_SMOOTH;
	if (st->flags & SE_CLAMP)
		s->fragment |= GPU_CLAMPED;
	if (st->flags & SE_KEYED)
		s->fragment |= GPU_KEYED, s->key = t->key;
}

/* Its colour: `entity` is the colour of what is drawn, `vertex` of its first corner (either may be NULL). */
static unsigned stage_colour (const se_stage_t *st, const byte *entity, const byte *vertex)
{
	unsigned	r = 255, g = 255, b = 255, a = 255;

	switch (st->rgb) {
	case SE_RGB_WAVE:
		r = g = b = level (wave (&st->rgb_wave, scene_time) * 255.0f);
		break;
	case SE_RGB_CONST:
		r = st->constant[0], g = st->constant[1], b = st->constant[2];
		break;
	case SE_RGB_ENTITY:
		if (entity)
			r = entity[0], g = entity[1], b = entity[2];
		break;
	case SE_RGB_ONE_MINUS_ENTITY:
		if (entity)
			r = 255 - entity[0], g = 255 - entity[1], b = 255 - entity[2];
		break;
	case SE_RGB_VERTEX:
	case SE_RGB_DIFFUSE:
		if (vertex)
			r = vertex[0], g = vertex[1], b = vertex[2];
		break;
	}
	switch (st->alpha) {
	case SE_A_WAVE:		a = level (wave (&st->alpha_wave, scene_time) * 255.0f); break;
	case SE_A_CONST:	a = st->constant[3]; break;
	case SE_A_ENTITY:	if (entity) a = entity[3]; break;
	case SE_A_ONE_MINUS_ENTITY: if (entity) a = 255 - entity[3]; break;
	case SE_A_VERTEX:	if (vertex) a = vertex[3]; break;
	default:		if (vertex && (st->flags & SE_IMPLICIT)) a = vertex[3]; break;
	}
	return colour_word (r, g, b, a);
}

/* Every stage's state as stage_state makes it, in a scene and flat (for a quad), and its
   colour where that is the same whatever is drawn with it: made when the file is opened. A
   stage of several pictures is asked about each time. */
static gpu_state	*stage_kept[2];
static unsigned		*stage_colours;
static unsigned char	*stage_fixed;
typedef struct { unsigned frame, at; float time, m[6]; } moved_t;
static moved_t		*moved_kept;	/* how each one's texture is moved in the frame being made */
static unsigned		frame_number = 2;

/* Their states again, with the pictures decoded since (no frame being drawn reads them). */
static void stages_refresh (void)
{
	int	i;

	for (i = 0; i < stage_count; i++) {
		stage_state (&stage_kept[0][i], &stages[i], 0);
		stage_state (&stage_kept[1][i], &stages[i], 1);
		stage_kept[1][i].vertex = GPU_QUADS;
	}
}

static void stages_open (void)
{
	int	i;

	stage_kept[0] = malloc (stage_count * sizeof (gpu_state));
	stage_kept[1] = malloc (stage_count * sizeof (gpu_state));
	stage_colours = malloc (stage_count * sizeof *stage_colours);
	stage_fixed = malloc (stage_count);
	moved_kept = calloc (stage_count, sizeof *moved_kept);
	stages_refresh ();
	for (i = 0; i < stage_count; i++) {
		const se_stage_t *st = &stages[i];

		stage_colours[i] = stage_colour (st, NULL, NULL);
		stage_fixed[i] = (st->rgb == SE_RGB_IDENTITY || st->rgb == SE_RGB_CONST || st->rgb == SE_RGB_VERTEX || st->rgb == SE_RGB_DIFFUSE) &&
			(st->alpha == SE_A_IDENTITY || st->alpha == SE_A_CONST || st->alpha == SE_A_VERTEX);
	}
}

static inline const gpu_state *stage_base (gpu_state *own, const se_stage_t *st, int flat)
{
	if (st->frames > 1) {
		stage_state (own, st, flat);
		if (flat)
			own->vertex = GPU_QUADS;
		return own;
	}
	return &stage_kept[flat][st - stages];
}

/* How its texture slides, turns and grows now (tcMod): two rows of three, the same for everything a frame draws with it. */
static const float *stage_rows (const se_stage_t *st)
{
	moved_t	*k = &moved_kept[st - stages];
	float	*m = k->m, a[6], n[6];
	int	i;

	if (k->frame == frame_number && k->time == scene_time)
		return m;
	k->frame = frame_number, k->time = scene_time, k->at = 0;
	m[0] = 1, m[1] = 0, m[2] = 0, m[3] = 0, m[4] = 1, m[5] = 0;
	if (st->tcmods == 1 && st->tcmod[0].kind == SE_SCROLL) {	/* (most are only this) */
		m[2] = st->tcmod[0].p[0] * scene_time, m[5] = st->tcmod[0].p[1] * scene_time;
		m[2] -= floor_of (m[2]), m[5] -= floor_of (m[5]);
		return m;
	}
	for (i = 0; i < st->tcmods; i++) {
		const se_tcmod_t *t = &st->tcmod[i];
		float	p, sn, cs, turn;

		a[0] = 1, a[1] = 0, a[2] = 0, a[3] = 0, a[4] = 1, a[5] = 0;
		switch (t->kind) {
		case SE_SCROLL:
			a[2] = t->p[0] * scene_time, a[5] = t->p[1] * scene_time;
			a[2] -= floor_of (a[2]), a[5] -= floor_of (a[5]);
			break;
		case SE_SCALE:
			a[0] = t->p[0], a[4] = t->p[1];
			break;
		case SE_ROTATE:
			turn = -t->p[0] * scene_time * (1.0f / 360);
			turn -= floor_of (turn);
			sn = sines[(int)(turn * 256) & 255], cs = sines[((int)(turn * 256) + 64) & 255];
			a[0] = cs, a[1] = -sn, a[2] = 0.5f - 0.5f * cs + 0.5f * sn;
			a[3] = sn, a[4] = cs, a[5] = 0.5f - 0.5f * sn - 0.5f * cs;
			break;
		case SE_STRETCH: {
			se_wave_t w = {t->function, t->p[0], t->p[1], t->p[2], t->p[3]};

			p = wave (&w, scene_time);
			p = p ? 1.0f / p : 1.0f;
			a[0] = a[4] = p, a[2] = a[5] = 0.5f - 0.5f * p;
			break;
		}
		default:
			continue;
		}
		/* this one after those before it */
		n[0] = a[0] * m[0] + a[1] * m[3], n[1] = a[0] * m[1] + a[1] * m[4], n[2] = a[0] * m[2] + a[1] * m[5] + a[2];
		n[3] = a[3] * m[0] + a[4] * m[3], n[4] = a[3] * m[1] + a[4] * m[4], n[5] = a[3] * m[2] + a[4] * m[5] + a[5];
		memcpy (m, n, sizeof n);
	}
	return m;
}

/*
 * Those rows in the frame's memory for the GPU to move the texture by, and their address; 0
 * when the frame has no room. `whole` is what the vertices' own coordinates had taken off.
 */
static unsigned stage_moved (const se_stage_t *st, const float *whole)
{
	const float	*m = stage_rows (st);
	moved_t		*k = &moved_kept[st - stages];
	float		x = m[2], y = m[5], *out;

	if (!whole && k->at)
		return k->at;	/* (a model's, written for another this frame) */
	if (whole) {
		x += m[0] * whole[0] + m[1] * whole[1];
		y += m[3] * whole[0] + m[4] * whole[1];
	}
	if (!(st->flags & SE_CLAMP))
		x -= floor_of (x), y -= floor_of (y);
	out = gpuFrameMemory (32);
	if (!out)
		return 0;
	out[0] = m[0], out[1] = m[1], out[2] = x, out[3] = 0;
	out[4] = m[3], out[5] = m[4], out[6] = y, out[7] = 0;
	if (!whole)
		k->at = gpuAddress (out);
	return gpuAddress (out);
}

/* ---- the view ---- */

/* (of the scene's description: the whole of it is 8 KB, which a copy a scene is not worth) */
static struct { vec3_t vieworg, viewaxis[3]; int rdflags; } view;
static float		view_matrix[16], view_planes[4][4];
static int		matrix_is;	/* what the GPU's matrices are now: 0 unknown, 1 the scene's own, 2 the flat screen's */
unsigned		se_counts[6];	/* since the last q3stat: line: models, their draws, how many were lit, sprites and beams, polygons, flat pictures */

/* The tangent, sine and cosine of half a field of view: a frame's scenes have the same few. */
static const float *half_angle (float fov)
{
	static struct { float fov, of[3]; } kept[4];
	static int	next;
	float		half = fov * (M_PI / 360.0f);
	int		i;

	for (i = 0; i < 4; i++)
		if (kept[i].fov == fov && kept[i].of[2] != 0)
			return kept[i].of;
	i = next++ & 3;
	kept[i].fov = fov, kept[i].of[0] = tan (half), kept[i].of[1] = sin (half), kept[i].of[2] = cos (half);
	return kept[i].of;
}

static void view_set (const refdef_t *fd)
{
	float		p[16], xmax, ymax, sn, cs;
	const float	*half_x = half_angle (fd->fov_x), *half_y = half_angle (fd->fov_y);
	const float	*o = fd->vieworg, *f = fd->viewaxis[0], *l = fd->viewaxis[1], *u = fd->viewaxis[2];
	float		*m = view_matrix;
	int		i;

	VectorCopy (fd->vieworg, view.vieworg);
	AxisCopy (fd->viewaxis, view.viewaxis);
	view.rdflags = fd->rdflags;
	scene_time = fd->time * 0.001f;
	if (!(fd->rdflags & RDF_NOWORLDMODEL))
		seat_fov_x = fd->fov_x, seat_fov_y = fd->fov_y;
	xmax = Z_NEAR * half_x[0];
	ymax = Z_NEAR * half_y[0];
	memset (p, 0, sizeof p);
	p[0] = Z_NEAR / xmax, p[5] = Z_NEAR / ymax;
	p[10] = -(Z_FAR + Z_NEAR) / (Z_FAR - Z_NEAR), p[11] = -1, p[14] = -2 * Z_FAR * Z_NEAR / (Z_FAR - Z_NEAR);
	/* the game's forward, left and up are the GPU's -z, -x and y */
	m[0] = -l[0], m[4] = -l[1], m[8] = -l[2], m[12] = DotProduct (l, o);
	m[1] = u[0], m[5] = u[1], m[9] = u[2], m[13] = -DotProduct (u, o);
	m[2] = -f[0], m[6] = -f[1], m[10] = -f[2], m[14] = DotProduct (f, o);
	m[3] = m[7] = m[11] = 0, m[15] = 1;
	picture_viewport (fd->x, fd->y, fd->width, fd->height);
	gpuProjection (p);
	gpuModelview (m);
	matrix_is = 1;
	/* the view's four sides, facing in: what is wholly behind one is not drawn */
	for (i = 0; i < 4; i++) {
		const float	*side = i < 2 ? l : u;
		float		sign = i & 1 ? -1 : 1, *n = view_planes[i];

		sn = (i < 2 ? half_x : half_y)[1], cs = (i < 2 ? half_x : half_y)[2];
		n[0] = f[0] * sn + sign * side[0] * cs;
		n[1] = f[1] * sn + sign * side[1] * cs;
		n[2] = f[2] * sn + sign * side[2] * cs;
		n[3] = DotProduct (o, n);
	}
}

static int culled (const float *centre, float radius)
{
	int	i;

	for (i = 0; i < 4; i++)
		if (DotProduct (centre, view_planes[i]) - view_planes[i][3] < -radius)
			return 1;
	return 0;
}

/* The GPU's matrix for a thing at `origin` turned to `axis`, its own units `scale` and `move`d (or NULL). */
static void entity_matrix (const float *origin, const vec3_t *axis, const float *scale, const float *move)
{
	float		rows[12], *block = gpuModelviewBlock (), *to = block ? block : rows;
	float		x[3], y[3], z[3], at[3];
	const float	*v = view_matrix;
	int		i;

	VectorCopy (axis[0], x);
	VectorCopy (axis[1], y);
	VectorCopy (axis[2], z);
	VectorCopy (origin, at);
	if (move) {
		at[0] += x[0] * move[0] + y[0] * move[1] + z[0] * move[2];
		at[1] += x[1] * move[0] + y[1] * move[1] + z[1] * move[2];
		at[2] += x[2] * move[0] + y[2] * move[1] + z[2] * move[2];
	}
	if (scale) {
		VectorScale (x, scale[0], x);
		VectorScale (y, scale[1], y);
		VectorScale (z, scale[2], z);
	}
	for (i = 0; i < 3; i++, to += 4) {
		float	a = v[i], b = v[4 + i], c = v[8 + i];

		to[0] = a * x[0] + b * x[1] + c * x[2];
		to[1] = a * y[0] + b * y[1] + c * y[2];
		to[2] = a * z[0] + b * z[1] + c * z[2];
		to[3] = a * at[0] + b * at[1] + c * at[2] + v[12 + i];
	}
	if (!block)
		gpuModelviewRows (rows);
	matrix_is = 0;
}

static void view_matrix_again (void)
{
	if (matrix_is != 1) {
		gpuModelview (view_matrix);
		matrix_is = 1;
	}
}

/* ---- pictures: decoded into GPU memory when what draws with them is registered ---- */

/* Textures lie in sheets (bake.py), a sheet decoded whole on every core the game is not using.
   A sheet asked for on a worker core (the client's game) is decoded by this core at its next
   frame's end: each core's row of wants and its count are its own to write. */
#define WANT_ROW	((sheet_count + 15) & ~15)
static const se_sheet_t	*sheets;
static int		sheet_count;
static unsigned		*sheet_at;		/* where each sheet was decoded to; 0: not yet */
static unsigned char	*wanted;		/* SE_CORES rows of a byte a sheet */
static struct { unsigned asked, unused[3]; } wants[SE_CORES] __attribute__ ((aligned (16)));
static unsigned		wants_seen[SE_CORES];
static unsigned char	*spare;			/* the display's own memory (seglMemorySpare), given out first */
static unsigned		spare_bytes, spare_used, pictures_mark, picture_workers, picture_log;
static int		pictures_stale;		/* stage_kept was made before the latest sheets */
static mcw_handle	scene_out;		/* the job drawing the frame before, while it is out */
unsigned		se_pictures[5];		/* sheets decoded, their KB, not for want of memory, this core's instructions, JPEG checks failed */

static void pictures_open (void)
{
	int	i;

	sheets = section ("SHET", sizeof (se_sheet_t), &sheet_count);
	sheet_at = calloc (sheet_count + 1, sizeof *sheet_at);
	picture_at = calloc (texture_count + 1, sizeof *picture_at);
	wanted = aligned_alloc (16, SE_CORES * WANT_ROW + 16);
	memset (wanted, 0, SE_CORES * WANT_ROW);
	for (i = 0; i < texture_count; i++)
		if (textures[i].sheet == SE_NO_SHEET)
			picture_at[i] = dat_rom + textures[i].at;
}

static void *picture_room (unsigned bytes)
{
	void	*to;

	bytes = (bytes + 15) & ~15u;
	if (spare && spare_used + bytes <= spare_bytes)
		to = spare + spare_used, spare_used += bytes;
	else
		to = seglMemory (bytes);
	if (to)
		mcw_touch (to, bytes);	/* (the workers store there) */
	return to;
}

/* The room picture_room() gave last, given back. */
static void picture_room_back (void *to, unsigned bytes)
{
	bytes = (bytes + 15) & ~15u;
	if ((unsigned char *)to == spare + spare_used - bytes)
		spare_used -= bytes;
	else
		gpuMemoryRelease (gpuMemoryMark () - bytes);
}

/* The decoding workers parked in a game: a call each, not joined until the next sheet, so that
   the game's calls (which go round the workers with none out) keep to as many as it asked for.
   A parked worker sleeps; going round all of them woke a cold one for every job. */
#define PARKED_MOST	64
static mcw_handle	parked[PARKED_MOST];
static int		parked_count;

static uint32_t park_job (uint32_t unused0, uint32_t unused1)
{
	return 0;
}

static void pictures_park (void)
{
	int	extra = mcw_count () - se_game_workers;

	while (parked_count < extra && parked_count < PARKED_MOST && (parked[parked_count] = mcw_try_call (park_job, 0, 0)) != 0)
		parked_count++;
}

static void pictures_unpark (void)
{
	while (parked_count > 0)
		mcw_join (parked[--parked_count]);
}

/* Sheet s decoded where the GPU reads it, and its textures' states made again. */
static void sheet_decode (int s)
{
	const se_sheet_t	*x = &sheets[s];
	struct media_image	out;
	unsigned		i, got, began = SE_Cycles ();
	volatile unsigned char	sum = 0;
	const char		*why;

	if (sheet_at[s])
		return;
	memset (&out, 0, sizeof out);
	out.width = x->width, out.height = x->height, out.channels = x->channels, out.stride = x->width * x->channels, out.bgr = 1;
	if ((out.pixels = picture_room ((unsigned)out.stride * x->height)) == NULL) {
		se_pictures[2]++, sheet_at[s] = ~0u;	/* (not tried again) */
		ri.Printf (PRINT_WARNING, "no GPU memory for sheet %d of the pictures (%u KB)\n", s, ((unsigned)out.stride * x->height) >> 10);
		return;
	}
	for (i = x->at & ~4095u; i < x->at + x->bytes; i += 4096)
		sum += dat[i];		/* (the file's pages there before a worker reads them) */
	pictures_unpark ();
	/* (a small one is this core's: two strips of 16 rows at least) */
	got = x->height < 32 ? 0 : (x->height + 15) / 16u < picture_workers ? (x->height + 15) / 16u : picture_workers;
	why = media_image_decode (dat + x->at, x->bytes, &out, (int)got);
	if (why) {
		ri.Printf (PRINT_WARNING, "sheet %d of the pictures of %s: %s\n", s, SE_DATA_FILE, why);
		picture_room_back (out.pixels, (unsigned)out.stride * x->height);
		sheet_at[s] = ~0u;
		return;
	}
	sheet_at[s] = gpuAddress (out.pixels);
	for (i = 0; (int)i < texture_count; i++)
		if (textures[i].sheet == s)
			picture_at[i] = sheet_at[s] + textures[i].at;
	if (picture_log)
		printf ("q3pic: sheet %d %ux%u %s of %u bytes, %u workers, %u instructions\n", s, x->width, x->height,
			x->packing == SE_JPEG ? "JPEG" : "PNG", x->bytes, got, SE_Cycles () - began);
	se_pictures[0]++, se_pictures[1] += ((unsigned)out.stride * x->height) >> 10, se_pictures[3] += SE_Cycles () - began;
	if (scene_out) {
		pictures_stale = 1;
		return;
	}
	for (i = 0; (int)i < stage_count; i++)
		if (stages[i].texture != 0xffff && textures[stages[i].texture].sheet == s) {
			stage_state (&stage_kept[0][i], &stages[i], 0);
			stage_state (&stage_kept[1][i], &stages[i], 1);
			stage_kept[1][i].vertex = GPU_QUADS;
		}
}

/* A stage's pictures: their sheets decoded now on this core, asked for on a worker. */
static void want_stage (const se_stage_t *st)
{
	int	core = SE_OnWorker (), f, s;

	if (st->texture == 0xffff)
		return;
	for (f = 0; f < st->frames; f++)
		if (!picture_at[st->texture + f] && (s = textures[st->texture + f].sheet) != SE_NO_SHEET) {
			if (!core)
				sheet_decode (s);
			else if (core < SE_CORES)
				wanted[core * WANT_ROW + s] = 1, wants[core].asked++;
		}
}

static void want_shader (const se_shader_t *sh)
{
	int	k;

	for (k = 0; sh && k < sh->stages; k++)
		want_stage (&stages[sh->first_stage + k]);
}

/* Whether the workers asked for any, or stages are to be made again. */
static int pictures_pending (void)
{
	int	core;

	for (core = 1; core < SE_CORES; core++)
		if (wants[core].asked != wants_seen[core])
			return 1;
	return pictures_stale;
}

/* What the workers asked for, decoded (with no frame out on the scene's worker). */
static void pictures_take (void)
{
	int	core, s;

	for (core = 1; core < SE_CORES; core++)
		if (wants[core].asked != wants_seen[core]) {
			wants_seen[core] = wants[core].asked;
			for (s = 0; s < sheet_count; s++)
				if (wanted[core * WANT_ROW + s])
					sheet_decode (s);
		}
	if (pictures_stale && !scene_out)
		pictures_stale = 0, stages_refresh ();
}

/* All of them given back (the window is closed). */
static void pictures_free (void)
{
	int	i;

	if (!picture_at)
		return;
	seglSettle ();		/* (the list being drawn may read them) */
	spare_used = 0;
	gpuMemoryRelease (pictures_mark);
	memset (sheet_at, 0, sheet_count * sizeof *sheet_at);
	for (i = 0; i < texture_count; i++)
		if (textures[i].sheet != SE_NO_SHEET)
			picture_at[i] = 0;
	memset (wanted, 0, SE_CORES * WANT_ROW);
	for (i = 0; i < SE_CORES; i++)
		wants_seen[i] = wants[i].asked;
	stages_refresh ();
}

/* ---- the level ---- */

/* the light on things, as it was last asked for (light_at) */
#define LIGHT_KEPT	64
static struct { int at[3], valid; float light[9]; } light_kept[LIGHT_KEPT];	/* by where, in steps of 16 units */

typedef struct {
	gpu_state	state;
	unsigned	vertices, first, count;
	int		changes;	/* a frame says something of it: its colour, its picture, how it moves */
} run_t;

static run_t		*world_runs;
static int		*world_changing, world_changes;	/* the kept ones a frame says something of */
static int		world_count, world_kept, world_hidden[2], world_loaded;
static unsigned char	*world_room;	/* GPU memory the level is copied into, as much as it needs */
static unsigned		world_room_bytes, world_used;
static int		world_seen;	/* this frame drew a scene with the level in it */

static void *gpu_copy (unsigned at, unsigned bytes)
{
	void	*to = world_room + world_used;

	if (!world_room || world_used + bytes > world_room_bytes)
		ri.Error (ERR_DROP, "no GPU memory for the level (%u bytes more)", bytes);
	world_used += (bytes + 15) & ~15u;
	memcpy (to, dat + at, bytes);
	return to;
}

/* The level's room and the pictures' place in GPU memory, after what stays (the facings). */
static void gpu_rooms (void)
{
	unsigned	bytes = (world->vertices * 16 + 15) & ~15u;
	int		i;

	for (i = 0; i < (int)world->runs; i++)
		bytes += (runs[i].flags & SE_RUN_LIT ? (runs[i].count * 4 + 15) & ~15u : 0) + (runs[i].flags & SE_RUN_TAGGED ? 1024 : 0);
	world_room = seglMemory (bytes);
	world_room_bytes = world_room ? bytes : 0;
	pictures_mark = gpuMemoryMark ();
	spare = seglMemorySpare (&spare_bytes);
	picture_workers = SE_Workers () > 0 ? mcw_count () : 0;
	if (getenv ("QUAKE3_PICTURE_WORKERS"))
		picture_workers = (unsigned)atoi (getenv ("QUAKE3_PICTURE_WORKERS"));
	picture_log = getenv ("QUAKE3_PICTURES_LOG") != NULL;
	printf ("quake3: pictures are decoded on %u worker cores into %u KB of GPU memory and %u KB of the display's\n",
		picture_workers, seglMemoryLeft () >> 10, spare ? spare_bytes >> 10 : 0);
}

static void world_free (void)
{
	SE_SceneFlush ();
	if (!world_loaded)
		return;
	gpuKeep (0, 0);
	seglSettle ();		/* (the list being drawn may read it) */
	world_used = 0;
	free (world_runs);
	free (world_changing);
	world_runs = NULL, world_changing = NULL, world_changes = 0;
	world_loaded = world_count = world_kept = 0;
}

static void RE_LoadWorld (const char *name)
{
	const unsigned	*vertices;
	int		i, set, mesh = 0;

	world_free ();	/* (and the frame out on the scene's worker shown) */
	memset (light_kept, 0, sizeof light_kept);
	world_count = (int)world->runs;
	for (i = 0; i < world_count; i++)
		want_stage (&stages[runs[i].stage]);
	world_runs = calloc (world_count ? world_count : 1, sizeof (run_t));
	vertices = gpu_copy (world->vertices_at, world->vertices * 16);
	for (i = 0; i < world_count; i++) {
		const se_run_t		*r = &runs[i];
		const se_stage_t	*st = &stages[r->stage];
		run_t			*w = &world_runs[i];

		stage_state (&w->state, st, 0);
		w->state.fragment = (w->state.fragment & 0xffff) | GPU_PASS (r->pass);
		w->state.colour = stage_colour (st, NULL, NULL);
		w->vertices = gpuAddress (vertices + 4 * r->first);
		w->first = r->first, w->count = r->count;
		if (r->flags & SE_RUN_LIT) {
			const se_texture_t *page = &textures[r->light];

			w->state.fragment |= GPU_LIT;
			w->state.more[0] = gpuAddress (gpu_copy (r->light_at, r->count * 4));
			w->state.more[1] = dat_rom + page->at;
			w->state.more[2] = page->width | (unsigned)page->height << 16;
		}
		if (r->flags & SE_RUN_TAGGED) {
			w->state.vertex |= GPU_TAGGED | GPU_TABLE;
			w->state.colour = gpuAddress (gpu_copy (r->table_at, 1024));
		}
		if (r->flags & SE_RUN_SKY)
			w->state.fragment |= GPU_LAID;
		if (r->flags & SE_RUN_MOVING)
			w->state.vertex |= GPU_MOVED;
		w->changes = (r->flags & (SE_RUN_SKY | SE_RUN_MOVING)) || st->frames > 1 || ((st->rgb == SE_RGB_WAVE || st->alpha == SE_A_WAVE) && !(r->flags & SE_RUN_TAGGED));
		if (r->model == 0)
			world_kept = i + 1, mesh += r->count;
	}
	if (gpuKeep (world_kept, mesh) < 0)
		ri.Error (ERR_DROP, "the level is too large to keep in GPU memory (%i vertices in %i draws)", mesh, world_kept);
	for (set = 0; set < 2; set++)
		for (i = 0; i < world_kept; i++)
			gpuKept (set, i, &world_runs[i].state, world_runs[i].vertices, world_runs[i].first, world_runs[i].count);
	world_changing = malloc ((world_kept + 1) * sizeof (int));
	for (i = 0; i < world_kept; i++)
		if (world_runs[i].changes)
			world_changing[world_changes++] = i;
	world_hidden[0] = world_hidden[1] = 0;
	world_loaded = 1;
	ri.Printf (PRINT_ALL, "the level is kept in GPU memory: %i vertices in %i draws, %u KB\n", mesh, world_kept,
		world_used >> 10);
}

/* What a frame says of a run that changes, in the words of a command of it. */
static void run_now (const se_run_t *r, const run_t *w, unsigned *c)
{
	const se_stage_t	*st = &stages[r->stage];
	gpu_state		s;

	if (st->frames > 1) {
		stage_state (&s, st, 0);
		c[7] = s.texture, c[8] = s.width, c[9] = s.height;
	}
	if (!(r->flags & SE_RUN_TAGGED) && (st->rgb == SE_RGB_WAVE || st->alpha == SE_A_WAVE))
		c[11] = stage_colour (st, NULL, NULL);
	if (r->flags & SE_RUN_MOVING)
		c[15] = stage_moved (st, r->whole);
	if (r->flags & SE_RUN_SKY) {
		/* the sky lies on the picture, four repeats to 1,024 pixels, and slides as the eye turns */
		const float	*f = view.viewaxis[0];
		float		yaw = atan2 (f[1], f[0]) * (1.0f / (2 * M_PI)), pitch = f[2];
		float		u = -yaw * 4 * config.vidWidth * (4.0f / 1024), v = pitch * config.vidHeight * (4.0f / 1024);

		c[12] = float_bits (u - floor_of (u)), c[13] = float_bits (v - floor_of (v));
		c[14] = float_bits (4.0f * config.vidWidth / window_w), c[15] = float_bits (4.0f * config.vidHeight / window_h);
	}
}

/* The level in this frame: the kept draws get the scene's matrices, and what changes is said. */
static void world_frame (void)
{
	int	i, set = seglSet ();

	if (world_seen)
		return;		/* (a frame's second scene of the level: the kept draws have the first's matrices) */
	seglKeptMatrices ();
	if (world_hidden[set])
		for (i = 0; i < world_kept; i++)
			gpuKeptCommand (i)[2] = world_runs[i].count;
	for (i = 0; i < world_changes; i++) {
		int	n = world_changing[i];

		run_now (&runs[n], &world_runs[n], gpuKeptCommand (n));
	}
	world_hidden[set] = 0;
	world_seen = 1;
}

/* A frame with no scene of the level (a menu, the console over everything) must not show the kept one. */
static void world_hide (void)
{
	int	i, set = seglSet ();

	if (!world_kept || world_hidden[set])
		return;
	for (i = 0; i < world_kept; i++)
		gpuKeptCommand (i)[2] = 0;
	world_hidden[set] = 1;
}

/* One of the level's own models that moves (a door, a platform): its runs, where it is now. */
static void draw_inline (const refEntity_t *e)
{
	const se_bmodel_t	*b;
	unsigned		i, n = (unsigned)e->hModel - INLINE_MODEL;

	if (!world_loaded || n >= world->models)
		return;
	b = &bmodels[n];
	if (!b->runs)
		return;
	entity_matrix (e->origin, e->axis, NULL, NULL);
	for (i = b->first_run; i < b->first_run + b->runs && i < (unsigned)world_count; i++) {
		run_t		*w = &world_runs[i];
		gpu_state	s = w->state;
		unsigned	c[16];

		if (w->changes) {
			c[7] = s.texture, c[8] = s.width, c[9] = s.height, c[11] = s.colour;
			c[12] = s.more[0], c[13] = s.more[1], c[14] = s.more[2], c[15] = s.more[3];
			run_now (&runs[i], w, c);
			s.texture = c[7], s.width = c[8], s.height = c[9], s.colour = c[11];
			s.more[0] = c[12], s.more[1] = c[13], s.more[2] = c[14], s.more[3] = c[15];
		}
		gpuDrawAt (&s, w->vertices, (int)w->count);
	}
}

/* ---- the light on what moves ---- */

static float		*facings;	/* in GPU memory: the 256 ways a model's vertex can face */
static unsigned		facings_at;	/* as a command names it */

/* The light at a place, from the level's grid of it: ambient, directed (0 to 255) and from where. */
static int light_at (const float *place, float *ambient, float *directed, float *from, int keep)
{
	const unsigned char	*grid;
	float			frac[3], total = 0;
	int			pos[3], step[3], i, j, at[3], kept;

	VectorClear (ambient);
	VectorClear (directed);
	VectorClear (from);
	if (!world_loaded || !world->grid_points || (keep && (view.rdflags & RDF_NOWORLDMODEL)))
		return 0;
	/* (a thing at rest, or bobbing where it lies, was asked about last frame: the grid's points are 64 units apart) */
	for (i = 0; i < 3; i++)
		at[i] = (int)floor_of (place[i] * (1.0f / 16));
	kept = (int)(((unsigned)at[0] * 73856093u ^ (unsigned)at[1] * 19349663u ^ (unsigned)at[2] * 83492791u) >> 8) & (LIGHT_KEPT - 1);
	if (keep && light_kept[kept].valid && light_kept[kept].at[0] == at[0] && light_kept[kept].at[1] == at[1] && light_kept[kept].at[2] == at[2]) {
		const float *l = light_kept[kept].light;

		VectorCopy (l, ambient);
		VectorCopy (l + 3, directed);
		VectorCopy (l + 6, from);
		return 1;
	}
	for (i = 0; i < 3; i++) {
		float v = (place[i] - world->grid_origin[i]) / world->grid_size[i];

		pos[i] = (int)floor_of (v);
		frac[i] = v - pos[i];
		if (pos[i] < 0)
			pos[i] = 0;
		else if (pos[i] > world->grid_bounds[i] - 1)
			pos[i] = world->grid_bounds[i] - 1;
	}
	step[0] = 8, step[1] = 8 * world->grid_bounds[0], step[2] = 8 * world->grid_bounds[0] * world->grid_bounds[1];
	grid = dat + world->grid_at + pos[0] * step[0] + pos[1] * step[1] + pos[2] * step[2];
	for (i = 0; i < 8; i++) {
		const unsigned char	*d = grid;
		float			factor = 1, sl, cl, sg, cg;

		for (j = 0; j < 3; j++) {
			if (i & (1 << j)) {
				if (pos[j] + 1 > world->grid_bounds[j] - 1)
					break;
				factor *= frac[j];
				d += step[j];
			} else {
				factor *= 1 - frac[j];
			}
		}
		if (j != 3 || !(d[0] + d[1] + d[2]))
			continue;	/* outside the level, or in a wall */
		total += factor;
		ambient[0] += factor * d[0], ambient[1] += factor * d[1], ambient[2] += factor * d[2];
		directed[0] += factor * d[3], directed[1] += factor * d[4], directed[2] += factor * d[5];
		sl = sines[d[7]], cl = sines[(d[7] + 64) & 255], sg = sines[d[6]], cg = sines[(d[6] + 64) & 255];
		from[0] += factor * cl * sg, from[1] += factor * sl * sg, from[2] += factor * cg;
	}
	if (total > 0 && total < 0.99f) {
		total = 1 / total;
		VectorScale (ambient, total, ambient);
		VectorScale (directed, total, directed);
	}
	VectorNormalize (from);
	if (!keep)
		return 1;
	VectorCopy (at, light_kept[kept].at);
	VectorCopy (ambient, light_kept[kept].light);
	VectorCopy (directed, light_kept[kept].light + 3);
	VectorCopy (from, light_kept[kept].light + 6);
	light_kept[kept].valid = 1;
	return 1;
}

/*
 * The light on a thing, for the GPU to light its vertices by (docs/machine/gpu.md, vertices that face):
 * from where in the thing's own space, its colour, and the colour of the light from
 * everywhere, three texels in the frame's memory. 0 when the frame has no room.
 */
static unsigned entity_light (const refEntity_t *e)
{
	float	ambient[3], directed[3], from[3], *out = gpuFrameMemory (48);
	int	i;

	if (!out)
		return 0;
	if (!light_at (e->renderfx & RF_LIGHTING_ORIGIN ? e->lightingOrigin : e->origin, ambient, directed, from, 1)) {
		ambient[0] = ambient[1] = ambient[2] = directed[0] = directed[1] = directed[2] = 150;
		from[0] = 0.57735f, from[1] = 0.57735f, from[2] = 0.57735f;
	}
	for (i = 0; i < 3; i++) {
		out[i] = DotProduct (from, e->axis[i]);
		out[4 + i] = directed[i] * (1.0f / 255);
		out[8 + i] = (ambient[i] + 32) * (1.0f / 255);	/* (the game's own least light on everything) */
	}
	out[3] = out[7] = out[11] = 0;
	se_counts[2]++;
	return gpuAddress (out);
}

/* ---- models ---- */

static const se_shader_t *shader_handle (qhandle_t h)
{
	return h > 0 && h <= shader_count ? shader_of[h - 1] : NULL;
}

/* What a model is drawn as with the skin or the shader it is given: its surfaces' stages, found once. */
typedef struct { const se_surface_t *surface; const se_stage_t *stage; } way_draw_t;
typedef struct { int model, skin, shader, first, count; } way_t;
#define WAYS		256
#define WAY_DRAWS	4096
static way_t		ways[WAYS];
static way_draw_t	way_draws[WAY_DRAWS];
static int		way_draw_count;

static const way_t *model_way (const refEntity_t *e, const se_model_t *m)
{
	way_t			*w = &ways[((unsigned)e->hModel * 31 + (unsigned)e->customSkin * 17 + (unsigned)e->customShader) & (WAYS - 1)];
	const se_surface_t	*surf;
	const se_shader_t	*custom;
	const se_skin_t		*skin;
	int			i, j, k;

	if (w->model == e->hModel && w->skin == e->customSkin && w->shader == e->customShader)
		return w;
	custom = shader_handle (e->customShader);
	skin = e->customSkin > 0 && e->customSkin <= skin_count ? &skins[e->customSkin - 1] : NULL;
	if (way_draw_count + m->surfaces * 8 > WAY_DRAWS) {
		memset (ways, 0, sizeof ways);
		way_draw_count = 0;
	}
	w->model = e->hModel, w->skin = e->customSkin, w->shader = e->customShader;
	w->first = way_draw_count;
	for (i = 0, surf = &surfaces[m->first_surface]; i < m->surfaces; i++, surf++) {
		const se_shader_t	*sh = custom;

		if (!sh && skin) {
			/* (which of a skin's shaders a surface has, by its name) */
			for (j = 0; j < skin->count; j++)
				if (!strcmp (skin_entries[skin->first + j].surface, surf->name)) {
					int n = skin_entries[skin->first + j].shader;

					sh = n >= 0 ? shader_of[n] : NULL;
					break;
				}
		} else if (!sh && surf->shader >= 0) {
			sh = shader_of[surf->shader];
		}
		if (!sh || (sh->flags & SE_NODRAW))
			continue;
		for (k = 0; k < sh->stages && way_draw_count < WAY_DRAWS; k++) {
			const se_stage_t	*st = &stages[sh->first_stage + k];

			if (st->flags & SE_LIGHTMAP)
				continue;
			way_draws[way_draw_count].surface = surf, way_draws[way_draw_count].stage = st;
			way_draw_count++;
		}
	}
	w->count = way_draw_count - w->first;
	return w;
}

static void draw_model (const refEntity_t *e)
{
	const se_model_t	*m;
	const way_t		*way;
	const way_draw_t	*d, *end;
	unsigned		light = 0, moved, *c;
	const float		*info;
	float			centre[3], radius;
	int			frame, i, lit = 0;

	if (e->hModel >= INLINE_MODEL) {
		draw_inline (e);
		return;
	}
	if (e->hModel <= 0 || e->hModel > model_count)
		return;
	m = &models[e->hModel - 1];
	frame = e->backlerp > 0.5f ? e->oldframe : e->frame;
	if (frame < 0 || frame >= m->frames)
		frame = 0;
	info = (const float *)(dat + m->frames_at) + 4 * frame;
	for (i = 0; i < 3; i++)
		centre[i] = e->origin[i] + e->axis[0][i] * info[0] + e->axis[1][i] * info[1] + e->axis[2][i] * info[2];
	radius = e->nonNormalizedAxes ? info[3] * 2 : info[3];
	if (!(e->renderfx & RF_FIRST_PERSON) && culled (centre, radius))
		return;
	entity_matrix (e->origin, e->axis, m->scale, m->translate);
	se_counts[0]++;
	way = model_way (e, m);
	for (d = &way_draws[way->first], end = d + way->count; d < end; d++) {
		const se_stage_t	*st = d->stage;
		gpu_state		own;

		c = gpuDrawAt (stage_base (&own, st, 0), dat_rom + d->surface->vertices_at + (unsigned)frame * d->surface->vertices * 4,
			(int)d->surface->vertices);
		if (!c)
			return;
		c[4] |= GPU_PACKED | GPU_TAGGED;
		c[12] = dat_rom + d->surface->coords_at;
		if (st->rgb == SE_RGB_DIFFUSE) {
			if (!lit)
				light = entity_light (e), lit = 1;
			if (light)
				c[4] |= GPU_FACING, c[11] = facings_at, c[13] = light;
		} else {
			c[11] = stage_fixed[st - stages] ? stage_colours[st - stages] : stage_colour (st, e->shaderRGBA, NULL);
		}
		if (st->tcmods && (moved = stage_moved (st, NULL)) != 0)
			c[4] |= GPU_MOVED, c[15] = moved;
		se_counts[1]++;
	}
}

/* ---- sprites, beams and the game's own polygons ---- */

static inline void corner (unsigned *v, const float *p, float s, float t)
{
	v[0] = float_bits (p[0]), v[1] = float_bits (p[1]), v[2] = float_bits (p[2]);
	v[3] = ((unsigned)(int)(s * 1024.0f) & 0xffff) | (unsigned)(int)(t * 1024.0f) << 16;
}

/* Four corners in order round a quad, with the whole of each stage's texture on it (`along` times, the long way). */
static void draw_quad (const se_shader_t *sh, const float corners[4][3], const byte *colour, float along)
{
	int	k;

	if (!sh)
		return;
	view_matrix_again ();
	se_counts[3]++;
	for (k = 0; k < sh->stages; k++) {
		const se_stage_t	*st = &stages[sh->first_stage + k];
		gpu_state		s;
		unsigned		*v;

		if (st->flags & SE_LIGHTMAP)
			continue;
		s = *stage_base (&s, st, 0);
		s.vertex = GPU_QUADS;
		s.colour = stage_colour (st, colour, colour);
		if (st->tcmods && (s.more[3] = stage_moved (st, NULL)) != 0)
			s.vertex |= GPU_MOVED;
		if ((v = gpuDrawRoom (&s, 4, 6)) == NULL)
			return;
		corner (v, corners[0], 0, 0), corner (v + 4, corners[1], along, 0);
		corner (v + 8, corners[2], along, 1), corner (v + 12, corners[3], 0, 1);
	}
}

static void draw_sprite (const refEntity_t *e)
{
	float	left[3], up[3], c[4][3], r = e->radius;
	int	i;

	if (culled (e->origin, r * 1.5f))
		return;
	if (e->rotation == 0) {
		VectorScale (view.viewaxis[1], r, left);
		VectorScale (view.viewaxis[2], r, up);
	} else {
		float	turn = e->rotation * (1.0f / 360), sn, cs;

		turn -= floor_of (turn);
		sn = sines[(int)(turn * 256) & 255] * r, cs = sines[((int)(turn * 256) + 64) & 255] * r;
		for (i = 0; i < 3; i++) {
			left[i] = cs * view.viewaxis[1][i] - sn * view.viewaxis[2][i];
			up[i] = cs * view.viewaxis[2][i] + sn * view.viewaxis[1][i];
		}
	}
	for (i = 0; i < 3; i++) {
		c[0][i] = e->origin[i] + left[i] + up[i], c[1][i] = e->origin[i] - left[i] + up[i];
		c[2][i] = e->origin[i] - left[i] - up[i], c[3][i] = e->origin[i] + left[i] - up[i];
	}
	draw_quad (shader_handle (e->customShader), c, e->shaderRGBA, 1);
}

/* A beam from one place to another (a rail's core, a bolt of lightning): a strip turned to face the eye. */
static void draw_beam (const refEntity_t *e, float half, int crossed)
{
	float	along[3], to_eye[3], right[3], c[4][3], length;
	int	i, turn;

	VectorSubtract (e->oldorigin, e->origin, along);
	length = VectorNormalize (along);
	VectorSubtract (view.vieworg, e->origin, to_eye);
	CrossProduct (along, to_eye, right);
	if (VectorNormalize (right) == 0)
		return;
	for (turn = 0; turn <= crossed; turn++) {
		for (i = 0; i < 3; i++) {
			c[0][i] = e->origin[i] + right[i] * half, c[1][i] = e->oldorigin[i] + right[i] * half;
			c[2][i] = e->oldorigin[i] - right[i] * half, c[3][i] = e->origin[i] - right[i] * half;
		}
		draw_quad (shader_handle (e->customShader), c, e->shaderRGBA, length * (1.0f / 256));
		CrossProduct (along, right, to_eye);
		VectorCopy (to_eye, right);
	}
}

typedef struct { const se_shader_t *shader; int first, count; } scene_poly_t;
#define LIST_WORDS	8192
#define VIEW_WORDS	22		/* of a refdef_t: where its picture is, the eye, the clock, its flags */
enum { REC_PICTURE = 1, REC_SCENE, REC_LIST, REC_CLEAR, REC_SUM };
#define CORE_LIST	(1 + SE_LISTS)	/* this core's frames when the scene is a worker's: three in turn */
#define CORE_LISTS	3
/* What a frame adds and draws. The first is this core's and drawn as it comes. Then the
   client's game's on its worker core (client_shaderemu.c), written there in turn and drawn a
   frame later, and this core's own in turn when the scene's worker draws a frame (CORE_LIST):
   `words` has the scenes, flat pictures, clears and other lists in their order. */
typedef struct {
	refEntity_t	entities[MAX_SCENE];
	scene_poly_t	polys[MAX_POLYS];
	polyVert_t	verts[MAX_POLY_VERTS];
	unsigned	words[LIST_WORDS];
	int		entity_count, poly_count, vert_count, word_count;
	int		scene_entity, scene_poly, scene_vert;	/* where the scene being added to begins */
	unsigned	colour;				/* of the flat pictures that follow */
} frame_list_t;
static frame_list_t	lists[CORE_LIST + CORE_LISTS] __attribute__ ((aligned (16)));
static int		recording;	/* which of them the client's game on its worker writes */
static int		core_list = CORE_LIST;	/* which this core writes, when the scene is a worker's */

/* What the client's game's helper core adds for the level's items (client_shaderemu.c), apart for
   each of the client's lists: a placeholder in the list says whose come in its place. */
#define SIDE_ENTITIES	512
#define SIDE_ITEMS	256
#define SIDE_PLACE	0x5e		/* a placeholder's reType; its frame is the item's */
typedef struct {
	refEntity_t	entities[SIDE_ENTITIES];
	struct { int first, count; } items[SIDE_ITEMS];
	int		count, unused[3];
} side_list_t;
static side_list_t	sides[1 + SE_LISTS] __attribute__ ((aligned (16)));
static int		side_list;	/* the helper's */

static inline frame_list_t *list_now (void)
{
	return &lists[SE_OnWorker () == 2 ? recording : scene_mode > 0 ? core_list : 0];
}

/* Two words of a recorded list's own: a clear, another list, the client's game's sum on or off. */
static void list_word (frame_list_t *l, unsigned kind, unsigned value)
{
	if (l->word_count + 2 <= LIST_WORDS)
		l->words[l->word_count] = kind, l->words[l->word_count + 1] = value, l->word_count += 2;
}

static void draw_poly (const scene_poly_t *p, const polyVert_t *pv)
{
	const se_shader_t	*sh = p->shader;
	float			nudge[3] = {0, 0, 0};
	int			k, i, n = p->count, quad = n == 4;

	if (sh->flags & SE_OFFSET)	/* a mark on a wall: a little off it, towards the eye */
		VectorScale (view.viewaxis[0], -0.5f, nudge);
	se_counts[4]++;
	for (k = 0; k < sh->stages; k++) {
		const se_stage_t	*st = &stages[sh->first_stage + k];
		gpu_state		s;
		unsigned		*v;
		float			at[3];

		if (st->flags & SE_LIGHTMAP)
			continue;
		s = *stage_base (&s, st, 0);
		s.vertex = quad ? GPU_QUADS : 0;
		s.colour = stage_colour (st, pv[0].modulate, pv[0].modulate);
		if (st->tcmods && (s.more[3] = stage_moved (st, NULL)) != 0)
			s.vertex |= GPU_MOVED;
		if ((v = gpuDrawRoom (&s, quad ? 4 : 3 * (n - 2), quad ? 6 : 3 * (n - 2))) == NULL)
			return;
		if (quad) {
			for (i = 0; i < 4; i++, v += 4) {
				VectorAdd (pv[i].xyz, nudge, at);
				corner (v, at, pv[i].st[0], pv[i].st[1]);
			}
		} else {
			for (i = 1; i + 1 < n; i++, v += 12) {
				VectorAdd (pv[0].xyz, nudge, at), corner (v, at, pv[0].st[0], pv[0].st[1]);
				VectorAdd (pv[i].xyz, nudge, at), corner (v + 4, at, pv[i].st[0], pv[i].st[1]);
				VectorAdd (pv[i + 1].xyz, nudge, at), corner (v + 8, at, pv[i + 1].st[0], pv[i + 1].st[1]);
			}
		}
	}
}

/* ---- a scene ---- */

static void RE_ClearScene (void)
{
	frame_list_t	*l = list_now ();

	l->entity_count = l->scene_entity, l->poly_count = l->scene_poly, l->vert_count = l->scene_vert;
}

static void RE_AddRefEntityToScene (const refEntity_t *e)
{
	frame_list_t	*l;

	if (SE_OnWorker () == SE_SIDE) {
		side_list_t *s = &sides[side_list];

		if (s->count < SIDE_ENTITIES && e->reType >= 0 && e->reType < RT_MAX_REF_ENTITY_TYPE)
			s->entities[s->count++] = *e;
		return;
	}
	l = list_now ();

	if (l->entity_count < MAX_SCENE && e->reType >= 0 && e->reType < RT_MAX_REF_ENTITY_TYPE)
		l->entities[l->entity_count++] = *e;
}

static void RE_AddPolyToScene (qhandle_t hShader, int numVerts, const polyVert_t *verts, int num)
{
	const se_shader_t	*sh = shader_handle (hShader);
	frame_list_t		*l = list_now ();
	int			i;

	if (!sh)
		return;
	for (i = 0; i < num; i++, verts += numVerts) {
		scene_poly_t	*p = &l->polys[l->poly_count];

		if (numVerts < 3 || l->poly_count == MAX_POLYS || l->vert_count + numVerts > MAX_POLY_VERTS)
			return;
		p->shader = sh, p->first = l->vert_count, p->count = numVerts;
		memcpy (&l->verts[l->vert_count], verts, numVerts * sizeof (polyVert_t));
		l->vert_count += numVerts, l->poly_count++;
	}
}

static int RE_LightForPoint (vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir)
{
	/* (what is kept is the drawing core's: this one's unless the scene is a worker's) */
	return light_at (point, ambientLight, directedLight, lightDir, !SE_OnWorker () && scene_mode <= 0);
}

static void RE_AddLightToScene (const vec3_t org, float intensity, float r, float g, float b) {}

void SE_SceneSideBegin (int list)
{
	side_list = list;
	sides[list].count = 0;
}

void SE_SceneSideItem (int item)
{
	side_list_t	*s = &sides[side_list];

	if (item > 0 && item <= SIDE_ITEMS)
		s->items[item - 1].count = s->count - s->items[item - 1].first;
	if (item < SIDE_ITEMS)
		s->items[item].first = s->count;
}

void SE_SceneSidePlace (int item)
{
	frame_list_t	*l = list_now ();

	if (l->entity_count < MAX_SCENE)
		l->entities[l->entity_count].reType = SIDE_PLACE, l->entities[l->entity_count].frame = item, l->entity_count++;
}

/* A placeholder's entities: n of them, or NULL when it is none. */
static const refEntity_t *side_of (const frame_list_t *l, const refEntity_t *e, int *n)
{
	int			i = (int)(l - lists);
	const side_list_t	*s = &sides[i];

	if (e->reType != SIDE_PLACE || i < 1 || i > SE_LISTS || e->frame < 0 || e->frame >= SIDE_ITEMS)
		return NULL;
	*n = s->items[e->frame].count;
	return &s->entities[s->items[e->frame].first];
}

/* QUAKE3_SUM=1: a sum over what each frame of the client's game asked to have drawn (its
   scenes' views, entities and polygons, its flat pictures), printed with the frame's time.
   The same game gives the same sums whichever core made the frame. */
static unsigned	sum, sum_time;
static int	sums, summing, summed;


static void sum_words (const void *words, int count)
{
	const unsigned	*w = words;

	for (; count > 0; count--)
		sum = (sum ^ *w++) * 0x01000193u;
}

void SE_SceneSum (int on)
{
	static int	asked;

	if (!asked)
		asked = 1, sums = getenv ("QUAKE3_SUM") != NULL;
	if (scene_mode > 0) {
		if (sums)
			list_word (list_now (), REC_SUM, (unsigned)on);
		return;
	}
	summing = sums && on;
}

static void entity_draw (const refEntity_t *e)
{
	switch (e->reType) {
	case RT_MODEL:
		if (!(e->renderfx & RF_THIRD_PERSON))
			draw_model (e);
		break;
	case RT_SPRITE:
		draw_sprite (e);
		break;
	case RT_RAIL_CORE:
		draw_beam (e, 3, 0);
		break;
	case RT_LIGHTNING:
		draw_beam (e, 8, 1);
		break;
	default:
		break;
	}
}

/* A scene drawn: `fd` may be only a refdef_t's first VIEW_WORDS words. */
static void scene_draw (const refdef_t *fd, const frame_list_t *l, int entity, int entities, int poly, int polys)
{
	int	i, k, n, was = scene_mode > 0 ? 0 : SE_Phase (3);

	if (summing) {
		sum_words (fd, VIEW_WORDS);
		for (i = entity; i < entities; i++) {
			const refEntity_t	*e = &l->entities[i], *side = side_of (l, e, &n);

			if (side)
				sum_words (side, n * (int)(sizeof (refEntity_t) / 4));
			else
				sum_words (e, (int)(sizeof (refEntity_t) / 4));
		}
		for (i = poly; i < polys; i++)
			sum_words (&l->verts[l->polys[i].first], l->polys[i].count * (int)(sizeof (polyVert_t) / 4));
		if (!(fd->rdflags & RDF_NOWORLDMODEL))
			sum_time = (unsigned)fd->time, summed = 1;
	}
	view_set (fd);
	if (!(fd->rdflags & RDF_NOWORLDMODEL) && world_loaded)
		world_frame ();
	for (i = entity; i < entities; i++) {
		const refEntity_t	*e = &l->entities[i], *side = side_of (l, e, &n);

		if (side)
			for (k = 0; k < n; k++)
				entity_draw (&side[k]);
		else
			entity_draw (e);
	}
	view_matrix_again ();
	for (i = poly; i < polys; i++)
		draw_poly (&l->polys[i], &l->verts[l->polys[i].first]);
	if (scene_mode <= 0)
		SE_Phase (was);
}

static void RE_RenderScene (const refdef_t *fd)
{
	frame_list_t	*l = list_now ();

	if (l == &lists[0]) {
		scene_draw (fd, l, 0, l->entity_count, 0, l->poly_count);
		/* (as the game's own renderer: a scene's entities and polygons are drawn once) */
		l->entity_count = l->poly_count = l->vert_count = 0;
		return;
	}
	if (l->word_count + 5 + VIEW_WORDS <= LIST_WORDS) {
		unsigned *w = &l->words[l->word_count];

		w[0] = REC_SCENE, w[1] = l->scene_entity, w[2] = l->entity_count, w[3] = l->scene_poly, w[4] = l->poly_count;
		memcpy (w + 5, fd, VIEW_WORDS * 4);
		l->word_count += 5 + VIEW_WORDS;
	}
	l->scene_entity = l->entity_count, l->scene_poly = l->poly_count, l->scene_vert = l->vert_count;
}

/* ---- the flat screen: the status bar, the console, the menus ---- */

static void RE_SetColor (const float *rgba)
{
	list_now ()->colour = rgba ? colour_word (level (rgba[0] * 255.0f), level (rgba[1] * 255.0f), level (rgba[2] * 255.0f), level (rgba[3] * 255.0f)) : 0x00ffffff;
}

#define PICTURE_WORDS	8
/* A flat picture as eight words: what it is and its shader, its colour, two corners, and the texture's two. */
static inline void picture_words (unsigned *w, unsigned colour, float x, float y, float wide, float high, float s1, float t1, float s2, float t2,
	qhandle_t hShader)
{
	w[0] = REC_PICTURE | (unsigned)hShader << 8, w[1] = colour;
	w[2] = float_bits (x), w[3] = float_bits (y), w[4] = float_bits (x + wide), w[5] = float_bits (y + high);
	w[6] = ((unsigned)(int)(s1 * 1024.0f) & 0xffff) | (unsigned)(int)(s2 * 1024.0f) << 16;
	w[7] = ((unsigned)(int)(t1 * 1024.0f) & 0xffff) | (unsigned)(int)(t2 * 1024.0f) << 16;
}

static void picture_draw (const unsigned *w)
{
	const se_shader_t	*sh = shader_handle ((qhandle_t)(w[0] >> 8));
	gpu_state		s;
	unsigned		*v, u0 = w[6] & 0xffff, u1 = w[6] >> 16, v0 = w[7] << 16, v1 = w[7] & 0xffff0000u;

	if (summing)
		sum_words (w, PICTURE_WORDS);
	if (!sh || !sh->stages)
		return;
	se_counts[5]++;
	if (matrix_is != 2) {
		static const float	one[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		float			m[16];

		memset (m, 0, sizeof m);
		m[0] = 2.0f / config.vidWidth, m[5] = -2.0f / config.vidHeight, m[10] = -1, m[12] = -1, m[13] = 1, m[15] = 1;
		picture_viewport (0, 0, config.vidWidth, config.vidHeight);
		gpuProjection (m);
		gpuModelview (one);
		matrix_is = 2;
	}
	/* (drawn in the order they come, in the one pass that blends without depth: the first stage of each) */
	s = *stage_base (&s, &stages[sh->first_stage], 1);
	s.colour = w[1];
	if ((v = gpuDrawRoom (&s, 4, 6)) == NULL)
		return;
	v[0] = w[2], v[1] = w[3], v[2] = 0, v[3] = u0 | v0;
	v[4] = w[4], v[5] = w[3], v[6] = 0, v[7] = u1 | v0;
	v[8] = w[4], v[9] = w[5], v[10] = 0, v[11] = u1 | v1;
	v[12] = w[2], v[13] = w[5], v[14] = 0, v[15] = u0 | v1;
}

static void RE_StretchPic (float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader)
{
	frame_list_t	*l = list_now ();
	unsigned	words[PICTURE_WORDS];

	if (l == &lists[0]) {
		picture_words (words, l->colour, x, y, w, h, s1, t1, s2, t2, hShader);
		picture_draw (words);
	} else if (l->word_count + PICTURE_WORDS <= LIST_WORDS) {
		picture_words (&l->words[l->word_count], l->colour, x, y, w, h, s1, t1, s2, t2, hShader);
		l->word_count += PICTURE_WORDS;
	}
}

static void list_clear (frame_list_t *l, unsigned colour)
{
	l->entity_count = l->poly_count = l->vert_count = l->word_count = 0;
	l->scene_entity = l->scene_poly = l->scene_vert = 0;
	l->colour = colour;
}

/* The client's game on its worker core begins a frame: its scenes and pictures go to list `which` (1 to SE_LISTS). */
void SE_SceneRecord (int which)
{
	recording = which;
	list_clear (&lists[which], 0x00ffffff);
}

/* A list drawn, by the core that draws. */
static void list_draw (const frame_list_t *l)
{
	int	at = 0;

	while (at < l->word_count) {
		const unsigned *w = &l->words[at];

		switch (w[0] & 0xff) {
		case REC_PICTURE:
			picture_draw (w);
			at += PICTURE_WORDS;
			break;
		case REC_SCENE:
			scene_draw ((const refdef_t *)(w + 5), l, (int)w[1], (int)w[2], (int)w[3], (int)w[4]);
			at += 5 + VIEW_WORDS;
			break;
		case REC_LIST:
			list_draw (&lists[w[1]]);
			at += 2;
			break;
		case REC_CLEAR:
			glClear (GL_COLOR_BUFFER_BIT);
			at += 2;
			break;
		default:
			summing = sums && w[1];
			at += 2;
			break;
		}
	}
}

/* That frame drawn: by this core, or by the scene's worker with the rest of this core's frame. */
void SE_SceneReplay (int which)
{
	if (scene_mode > 0)
		list_word (list_now (), REC_LIST, (unsigned)which);
	else
		list_draw (&lists[which]);
}

/* The lists' pages are there before a worker writes them (docs/machine/multicore.md). */
void SE_SceneTouch (void (*touch) (void *, unsigned))
{
	touch (lists, sizeof lists);
	touch (sides, sizeof sides);
}

/* ---- the scene on a worker core ---- */

unsigned		se_scene_jobs, se_scene_cycles, se_scene_passes;	/* for the q3stat: lines */

/* Frames this core recorded (frame n in list CORE_LIST + (n - 1) % 3) and showed: this core's.
   Drawn, and how each ended (gpuEnd, and its instructions): the job's. */
static struct { unsigned recorded, shown, unused[2]; } scene_asked __attribute__ ((aligned (16)));
static struct { unsigned drawn, unused[3]; } scene_made __attribute__ ((aligned (16)));
static struct { unsigned ended[4]; } scene_ended[2] __attribute__ ((aligned (16)));

#define SCENE_LINGER	8	/* passes the job waits for the next frame before it ends */

/* The frames recorded made the GPU's lists, one after another: all a frame draws, but the showing. */
static uint32_t scene_job (uint32_t unused0, uint32_t unused1)
{
	for (;;) {
		unsigned	n = scene_made.drawn + 1, began, idle = 0;

		while (scene_made.drawn >= scene_asked.recorded) {
			if (++idle > SCENE_LINGER)
				return 0;
			mc_end_pass ();
		}
		/* (its set is free once frame n - 2, drawn from it, is shown and drawn: showing n - 1 waits for that) */
		while (scene_asked.shown + 2 < n || (scene_asked.shown + 1 < n && !gpuIdle ()))
			mc_end_pass ();
		began = SE_Cycles ();
		world_seen = 0;
		matrix_is = 0;
		list_draw (&lists[CORE_LIST + (n - 1) % CORE_LISTS]);
		if (!world_seen)
			world_hide ();
		if (summed)
			mcw_print ("q3sum: %u %08x list %08x\n", sum_time, sum, gpuListSum (dat_rom));
		sum = 0, summed = 0, summing = 0;
		frame_number++;
		gpuEnd (scene_ended[n & 1].ended);
		scene_ended[n & 1].ended[3] = SE_Cycles () - began;
		scene_made.drawn = n;
	}
}

/* The job given out again when it ended with frames left to draw. */
static void scene_start (void)
{
	if (scene_out && mcw_ready (scene_out))
		mcw_join (scene_out), scene_out = 0;
	if (!scene_out && scene_made.drawn < scene_asked.recorded)
		scene_out = mcw_call (scene_job, 0, 0);
}

int SE_SceneOut (int *on)
{
	on[0] = scene_out;
	return 1;
}

/* Rests until the next pass: the scene's worker has the whole pass, and so have the client's game and its items (gpu_vm),
   which are the frame's length in turn with it */
static void scene_rest (void)
{
	mcw_handle	on[3] = { scene_out };

	mcw_rest_on (on, 1 + SE_ClientOut (on + 1));
}

/* The frames recorded up to `upto` shown, each when it has been drawn; or those drawn so far. */
static void scene_show (unsigned upto, int wait)
{
	while (scene_asked.shown < upto) {
		unsigned	n = scene_asked.shown + 1;
		int		was;

		while (scene_made.drawn < n) {
			scene_start ();
			if (scene_made.drawn >= n)
				break;
			if (!wait)
				return;
			scene_rest ();
			se_scene_passes++;
			SE_ServerPoll ();
		}
		was = SE_Phase (4);
		gpuShow (scene_ended[n & 1].ended);
		SE_Phase (was);
		se_scene_cycles += scene_ended[n & 1].ended[3];
		se_scene_jobs++;
		scene_asked.shown = n;
	}
}

void SE_ScenePoll (void)
{
	if (scene_mode > 0)
		scene_show (scene_asked.recorded, 0);
}

static void scene_setup (void)
{
	const char	*how = getenv ("QUAKE3_SCENE");

	scene_mode = !(how && !strcmp (how, "inline")) && SE_Workers () >= 3;
	if (scene_mode) {
		extern const char _start[], etext[];

		/* (what the job runs and writes is there before it does: the program's code, the lists,
		   the GPU's two sets, the renderer's own) */
		mcw_touch_code (_start, (unsigned)(etext - _start));	/* (a flat program's __executable_start is 0) */
		mcw_touch (lists, sizeof lists);
		mcw_touch (sides, sizeof sides);
		gpuTouchSets (mcw_touch);
		mcw_touch (moved_kept, stage_count * sizeof *moved_kept);
		mcw_touch (light_kept, sizeof light_kept);
		mcw_touch (way_draws, sizeof way_draws);
		mcw_touch (ways, sizeof ways);
		mcw_touch (se_counts, sizeof se_counts);
		mcw_touch (scene_ended, sizeof scene_ended);
		mcw_touch (&scene_made, sizeof scene_made);
	}
	printf ("quake3: the scene is drawn by %s\n", scene_mode ? "a worker core, while this core goes on" : "this core");
}

void SE_SceneFlush (void)
{
	scene_show (scene_asked.recorded, 1);
	while (scene_out && !mcw_ready (scene_out)) {
		scene_rest ();
		SE_ServerPoll ();
	}
	if (scene_out)
		mcw_join (scene_out), scene_out = 0;
}

static void RE_StretchRaw (int x, int y, int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {}
static void RE_UploadCinematic (int w, int h, int cols, int rows, const byte *data, int client, qboolean dirty) {}

/* ---- a frame ---- */

static void RE_BeginFrame (stereoFrame_t stereoFrame)
{
	if (scene_mode < 0)
		scene_setup ();
	if (scene_mode > 0) {
		/* (recorded here, and drawn by a job at its end while this core goes on to the next) */
		SE_ScenePoll ();
		core_list = CORE_LIST + (int)(scene_asked.recorded % CORE_LISTS);
		list_clear (&lists[core_list], lists[CORE_LIST + (scene_asked.recorded + CORE_LISTS - 1) % CORE_LISTS].colour);
		list_word (&lists[core_list], REC_CLEAR, 0);
		return;
	}
	pictures_take ();
	world_seen = 0;
	matrix_is = 0;
	glClear (GL_COLOR_BUFFER_BIT);
}

static void RE_EndFrame (int *frontEndMsec, int *backEndMsec)
{
	int	was;

	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	if (world_loaded == 1 && clc.state == CA_ACTIVE) {	/* (where a level's loading is measured to) */
		world_loaded = 2;
		printf ("quake3: pictures decoded so far %u (%u KB), not for want of GPU memory %u, this core's instructions in them "
			"%u thousand\nquake3: the level's first frame\n", se_pictures[0], se_pictures[1], se_pictures[2],
			se_pictures[3] / 1000);
		fflush (stdout);
	}
	if (!parked_count && clc.state == CA_ACTIVE)
		pictures_park ();
	if (scene_mode > 0) {
		if (pictures_pending ()) {
			SE_SceneFlush ();
			pictures_take ();
		}
		/* (drawn by the job after the frame before, or when it is next given out; each shown once drawn,
		   the one before last waited for here: the next frame is recorded into its list) */
		scene_asked.recorded++;
		scene_start ();
		if (scene_asked.recorded > 2)
			scene_show (scene_asked.recorded - 2, 1);
		scene_show (scene_asked.recorded, 0);
		SE_FrameDone ();
		return;
	}
	if (!world_seen)
		world_hide ();
	if (summed)
		printf ("q3sum: %u %08x list %08x\n", sum_time, sum, gpuListSum (dat_rom));
	sum = 0, summed = 0;
	was = SE_Phase (4);
	SE_Part (12);
	gpuSwap (0);
	SE_Part (10);
	SE_Phase (was);
	frame_number++;
	SE_FrameDone ();
}

/* ---- what the game registers ---- */

static qhandle_t register_shader (const char *name, const char *kind)
{
	char	clean[64];
	int	i;

	SE_Wanted (kind, name);
	clean_name (clean, name, 1);
	i = find (shaders, shader_count, sizeof (se_shader_t), clean);
	if (i >= 0)
		want_shader (&shaders[i]);
	/* (a run that lists what the game asks for has it go on asking: nothing is missing) */
	return i < 0 ? se_wanted : shaders[i].number + 1;
}

static qhandle_t RE_RegisterShader (const char *name) { return register_shader (name, "shader"); }
static qhandle_t RE_RegisterShaderNoMip (const char *name) { return register_shader (name, "shader2d"); }

static qhandle_t RE_RegisterModel (const char *name)
{
	char	clean[64];
	int	i;

	SE_Wanted ("model", name);
	if (name[0] == '*')
		return INLINE_MODEL + atoi (name + 1);
	clean_name (clean, name, 0);
	i = find (models, model_count, sizeof (se_model_t), clean) + 1;
	if (i) {
		const se_surface_t	*surf = &surfaces[models[i - 1].first_surface];
		int			k;

		for (k = 0; k < models[i - 1].surfaces; k++)
			if (surf[k].shader >= 0)
				want_shader (shader_of[surf[k].shader]);
	}
	return i ? i : se_wanted;
}

static qhandle_t RE_RegisterSkin (const char *name)
{
	char	clean[64];
	int	i;

	SE_Wanted ("skin", name);
	clean_name (clean, name, 0);
	i = find (skins, skin_count, sizeof (se_skin_t), clean) + 1;
	if (i) {
		const se_skin_t	*skin = &skins[i - 1];
		int		k;

		for (k = 0; k < skin->count; k++)
			if (skin_entries[skin->first + k].shader >= 0)
				want_shader (shader_of[skin_entries[skin->first + k].shader]);
	}
	return i ? i : se_wanted;
}

static int RE_LerpTag (orientation_t *tag, qhandle_t handle, int startFrame, int endFrame, float frac, const char *tagName)
{
	const se_model_t	*m;
	const char		*names;
	const float		*t;
	int			i, frame;

	if (handle > 0 && handle <= model_count) {
		m = &models[handle - 1];
		names = (const char *)dat + m->tags_at;
		/* (the frame the model is drawn in: draw_model takes the nearer of the two as well) */
		frame = frac >= 0.5f ? endFrame : startFrame;
		if (frame < 0 || frame >= m->frames)
			frame = 0;
		for (i = 0; i < m->tags; i++)
			if (!strcmp (names + 64 * i, tagName)) {
				t = (const float *)(names + 64 * m->tags) + 12 * (frame * m->tags + i);
				VectorCopy (t, tag->origin);
				VectorCopy (t + 3, tag->axis[0]);
				VectorCopy (t + 6, tag->axis[1]);
				VectorCopy (t + 9, tag->axis[2]);
				return qtrue;
			}
	}
	AxisClear (tag->axis);
	VectorClear (tag->origin);
	return qfalse;
}

static void RE_ModelBounds (qhandle_t handle, vec3_t mins, vec3_t maxs)
{
	VectorClear (mins);
	VectorClear (maxs);
	if (handle >= INLINE_MODEL && world && (unsigned)(handle - INLINE_MODEL) < world->models) {
		VectorCopy (bmodels[handle - INLINE_MODEL].mins, mins);
		VectorCopy (bmodels[handle - INLINE_MODEL].maxs, maxs);
	} else if (handle > 0 && handle <= model_count) {
		VectorCopy (models[handle - 1].mins, mins);
		VectorCopy (models[handle - 1].maxs, maxs);
	}
}

static void RE_BeginRegistration (glconfig_t *out)
{
	int	i;

	SE_SceneFlush ();
	if (!opened) {
		window_open ();
		data_open ();
		for (i = 0; i < 256; i++)
			sines[i] = sin (i * (2 * M_PI / 256));
		facings = seglMemory (SE_NORMALS * 16);
		if (!facings)
			ri.Error (ERR_FATAL, "no GPU memory");
		for (i = 0; i < SE_NORMALS; i++) {
			memcpy (facings + 4 * i, normals + 3 * i, 12);
			facings[4 * i + 3] = 0;
		}
		facings_at = gpuAddress (facings);
		gpu_rooms ();
		glClearColor (0, 0, 0, 0);
	}
	memset (&config, 0, sizeof config);
	Q_strncpyz (config.renderer_string, "the machine's GPU device", sizeof config.renderer_string);
	Q_strncpyz (config.vendor_string, "ShaderEmu", sizeof config.vendor_string);
	Q_strncpyz (config.version_string, "1", sizeof config.version_string);
	config.maxTextureSize = 256;
	config.numTextureUnits = 1;
	config.colorBits = 32, config.depthBits = 24;
	seglSize (&config.vidWidth, &config.vidHeight);
	window_w = config.vidWidth, window_h = config.vidHeight;
	config.windowAspect = (float)config.vidWidth / config.vidHeight;
	config.displayFrequency = 60;
	*out = config;
}

static void RE_Shutdown (qboolean destroyWindow)
{
	SE_SceneFlush ();
	if (opened && destroyWindow) {
		world_free ();
		pictures_free ();
	}
}

static void RE_SetWorldVisData (const byte *vis) {}
static void RE_EndRegistration (void) {}
static int RE_MarkFragments (int numPoints, const vec3_t *points, const vec3_t projection, int maxPoints, vec3_t pointBuffer,
	int maxFragments, markFragment_t *fragmentBuffer) { return 0; }
static void RE_RegisterFont (const char *fontName, int pointSize, fontInfo_t *font) {}
static void RE_RemapShader (const char *oldShader, const char *newShader, const char *offsetTime) {}
static qboolean RE_GetEntityToken (char *buffer, int size) { return qfalse; }
static qboolean RE_inPVS (const vec3_t p1, const vec3_t p2) { return qtrue; }
static void RE_TakeVideoFrame (int h, int w, byte *captureBuffer, byte *encodeBuffer, qboolean motionJpeg) {}

/* What the client's game asks most often, for it to call (quake3.patch, cg_syscalls.c): the modules are the same program. */
void SE_AddRefEntity (const refEntity_t *e) { RE_AddRefEntityToScene (e); }
void SE_SetColor (const float *rgba) { RE_SetColor (rgba); }
void SE_StretchPic (float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader)
{
	RE_StretchPic (x, y, w, h, s1, t1, s2, t2, hShader);
}
int SE_LerpTag (orientation_t *tag, qhandle_t model, int startFrame, int endFrame, float frac, const char *tagName)
{
	return RE_LerpTag (tag, model, startFrame, endFrame, frac, tagName);
}

refexport_t *GetRefAPI (int apiVersion, refimport_t *rimp)
{
	static refexport_t re;

	ri = *rimp;
	memset (&re, 0, sizeof re);
	if (apiVersion != REF_API_VERSION)
		return NULL;
	re.Shutdown = RE_Shutdown;
	re.BeginRegistration = RE_BeginRegistration;
	re.RegisterModel = RE_RegisterModel;
	re.RegisterSkin = RE_RegisterSkin;
	re.RegisterShader = RE_RegisterShader;
	re.RegisterShaderNoMip = RE_RegisterShaderNoMip;
	re.LoadWorld = RE_LoadWorld;
	re.SetWorldVisData = RE_SetWorldVisData;
	re.EndRegistration = RE_EndRegistration;
	re.BeginFrame = RE_BeginFrame;
	re.EndFrame = RE_EndFrame;
	re.MarkFragments = RE_MarkFragments;
	re.LerpTag = RE_LerpTag;
	re.ModelBounds = RE_ModelBounds;
	re.ClearScene = RE_ClearScene;
	re.AddRefEntityToScene = RE_AddRefEntityToScene;
	re.AddPolyToScene = RE_AddPolyToScene;
	re.LightForPoint = RE_LightForPoint;
	re.AddLightToScene = RE_AddLightToScene;
	re.AddAdditiveLightToScene = RE_AddLightToScene;
	re.RenderScene = RE_RenderScene;
	re.SetColor = RE_SetColor;
	re.DrawStretchPic = RE_StretchPic;
	re.DrawStretchRaw = RE_StretchRaw;
	re.UploadCinematic = RE_UploadCinematic;
	re.RegisterFont = RE_RegisterFont;
	re.RemapShader = RE_RemapShader;
	re.GetEntityToken = RE_GetEntityToken;
	re.inPVS = RE_inPVS;
	re.TakeVideoFrame = RE_TakeVideoFrame;
	return &re;
}
