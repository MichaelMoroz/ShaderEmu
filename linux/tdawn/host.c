/*
 * Tiberian Dawn's window on the ShaderEmu GPU (docs/games/tdawn.md): the game's 8-bit screen is a
 * texture in GPU memory, shown as one rectangle looked up in the palette, with the pointer as
 * a second one. Input comes from the Nano-X server.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <GLES/segl.h>
#include "nano-X.h"
#include "host.h"
#include "mcwx.h"	/* programs/mc: the machine's worker cores, over the second layer (docs/machine/multicore.md) */

#define ONE		65536
#define CURSOR_MAX	128		/* the largest pointer picture, a side */
#define CAPTION		22		/* the window manager's caption and frame (nanowm.h) */
#define FRAME		4
#define REG_PALETTE	0x400		/* offsets in the machine's control words (docs/machine/gpu.md) */
#define REG_INPUT	0x20
#define REG_CLOCK	0x34
#define REG_SUBMIT	0x10

static GR_WINDOW_ID window;
static int screen_w, screen_h;
static unsigned char *screen, *cursor;	/* screen: two pages, one after the other */
static GLuint page_texture[2], cursor_texture;
static int cursor_w, cursor_h, cursor_hot_x, cursor_hot_y;
static int pointer_x, pointer_y;	/* in the game's pixels */
static int pointer_wanted = 1;		/* the game shows its pointer now */
static void pointer_publish(void);
static void pointer_read(void);
static void window_origin(void);
#define HW_SIDE 32			/* the display's cursor, a side */
static uint32_t *hw_image;		/* the pointer's picture for the display, when it is the display's cursor */
static int hw_on, hw_stale;
static int page_rows;			/* a page's texture is this high: a row is a whole number of 1,024ths */

/*
 * A scene's rectangles on a worker core (docs/games/tdawn.md, docs/machine/multicore.md). Making a frame's
 * two hundred rectangles into GPU commands is an eighth of a mission's frame, and nothing of
 * it is needed before the frame is shown. So host_sprite() and host_block() only note what
 * was asked, host_scene_end() gives the notes to a worker, and host_present() of that scene
 * is put off until the worker has done: the game goes on to its next frame's logic meanwhile,
 * and the frame is shown from host_event() or, at the latest, before the next scene begins.
 * The GPU's library has one state: nothing here calls it while the worker has the scene
 * (scene_finish() first). TDAWN_SCENE=inline is the way it was.
 */
#define SCENE_MOST 4096
#define SCENE_PARTS 3		/* what is under the objects, the objects, what is over them (host.h) */
struct scene_note { int kind, x, y, width, height, atlas_x, atlas_y; unsigned int colour; };	/* 32 bytes: two texels */
static struct scene_note scene_notes[SCENE_PARTS][SCENE_MOST] __attribute__((aligned(16)));
static int scene_noted[SCENE_PARTS + 1] __attribute__((aligned(16)));	/* (a texel of its own: the worker adds to it) */
/* What a worker stores here besides: in a texel of its own too (two cores never store to one 16 bytes). */
static struct { int fill_count, missed, batched; uint32_t run_quads; } own __attribute__((aligned(16)));
#define fill_count own.fill_count
#define missed own.missed
#define batched own.batched
#define run_quads own.run_quads
static int scene_part = 1, scene_out, scene_owed, scene_owed_page, scene_owed_cursor;
static int scene_at[2], scene_view[4];	/* the scene's origin on the screen, and the view its kept rectangles are cut to */
#define PATH_CORES 3
#ifdef SHADEREMU_RA
#define PRE_CORES 0
#else
#define PRE_CORES 32		/* workers that ask about the map's cells ahead of a long search (findpath.cpp) */
#endif
static int scene_cores = -1;	/* -1: not asked yet; 0: none */
static void frames_room(void);
static void workers_open(void);
unsigned int host_scene_jobs;	/* scenes a worker made, for the games' figures */
static void scene_finish(void);
static void behind_joined_now(void);
static mcw_handle scene_handle;
static int run_single = -1, list_check;	/* TDAWN_SPRITES=one, TDAWN_CHECK_LIST: put_sprite() */
static void list_report(void);
static uint32_t frames_turn(uint32_t what, uint32_t turn);

/* What each kind of job ran, on whatever core, in counters (TDAWN_MARK's figures): host_job_time(). */
enum { HOST_JOB_SCENE, HOST_JOB_FRAMES, HOST_JOB_PATH, HOST_JOB_PRE, HOST_JOB_FILLS, HOST_JOB_QUADS, HOST_JOB_BEHIND, HOST_JOBS };
static int jobs_counted;

static void
host_job_time(int kind, uint32_t began)
{
	if (jobs_counted)
		mcw_add(kind, host_cycles() - began);
}

static const volatile uint32_t *
control(int offset)
{
	return (const volatile uint32_t *)((const char *)seglPalette() - REG_PALETTE + offset);
}

/*
 * A frame is handed to the GPU and not waited for (docs/machine/gpu.md): host_settle() comes before
 * every store to memory that list reads. TDAWN_SWAP=wait waits at the swap, as it was.
 */
static int flight;	/* a list was handed over and may not be drawn yet */

void
host_settle(void)
{
	if (!flight)
		return;
	if (mcw_self()) {
		/* (the objects drawn behind write the page: a worker's pause is a sleep, and the flag the first core's) */
		while (*control(REG_SUBMIT))
			mc_end_pass();
		return;
	}
	while (*control(REG_SUBMIT))
#ifdef __gpuvm__
		__asm__ volatile("wait");	/* pause */
#else
		__asm__ volatile(".word 0x0100000f");	/* pause */
#endif
	flight = 0;
}

/* The list made since the last one is shown, or the last one once more as its memory is now. */
static void
show(int again)
{
	static int waits = -1;

	if (waits < 0) {
		const char *how = getenv("TDAWN_SWAP");

		waits = how && !strcmp(how, "wait");
	}
	if (waits) {
		if (again)
			seglSwapAgain();
		else
			seglSwap();
		return;
	}
	if (again)
		seglSwapAgainEarly();
	else
		seglSwapEarly();
	flight = 1;
}

/* The machine's clock word once the GPU is ours, going on from the system's clock before. */
unsigned int
host_ms(void)
{
	static unsigned int ahead;
	static int ours;
	struct timespec ts;
	unsigned int system_ms;

	if (ours)
		return *control(REG_CLOCK) + ahead;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	system_ms = (unsigned int)ts.tv_sec * 1000u + (unsigned int)ts.tv_nsec / 1000000u;
	if (!screen)
		return system_ms;
	ahead = system_ms - *control(REG_CLOCK);
	ours = 1;
	return system_ms;
}

/* Ends the machine's frame: the way to wait for anything that changes between frames. */
void
host_wait(void)
{
	scene_finish();	/* (a frame that waits for a worker core is shown first) */
#ifdef __gpuvm__
	__asm__ volatile("wait");	/* pause */
#else
	__asm__ volatile(".word 0x0100000f");	/* pause */
#endif
}

unsigned int
host_cycles(void)
{
	uint32_t v;

#ifdef __gpuvm__
	__asm__ volatile("mfsr %0, cycle" : "=r"(v));
#else
	__asm__ volatile("rdcycle %0" : "=r"(v));
#endif
	return v;
}

#ifdef SHADEREMU_RA
#define TITLE "Red Alert"	/* (Red Alert is built with this file too: linux/ralert) */
#else
#define TITLE "Tiberian Dawn"
#endif

/* GPU memory for the large pieces: the display's own framebuffer while nothing uses it (segl.h), else our share. */
static void *
big_memory(unsigned bytes)
{
	static unsigned char *spare;
	static unsigned left = ~0u;

	if (left == ~0u)
		spare = seglMemorySpare(&left);
	bytes = (bytes + 15) & ~15u;
	if (spare && left >= bytes) {
		spare += bytes;
		left -= bytes;
		return spare - bytes;
	}
	return seglMemory(bytes);
}

int
host_open(int width, int height)
{
	GR_SCREEN_INFO info;
	int page, bare, scale;

	if (GrOpen() < 0) {
		fprintf(stderr, "tdawn: no Nano-X server (start one: nano-X -p &)\n");
		return -1;
	}
	/*
	 * The GPU stretches the screen over the window: the largest whole scale that fits the
	 * display. Where a frame and caption would not fit as well (640x400 on a 640x480
	 * display) the window has none and sits in the corner.
	 */
	GrGetScreenInfo(&info);
	for (scale = 4; scale > 1 && (width * scale > info.cols || height * scale > info.rows); scale--)
		;
	bare = width * scale + 2 * FRAME > info.cols || height * scale + CAPTION + FRAME > info.rows;
	window = GrNewWindowEx(bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		TITLE, GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width * scale, height * scale, 0);
	/* (not the pointer's moves: pointer_read()) */
	GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE);
	GrMapWindow(window);
	GrSetFocus(window);
	if (seglInit(window) < 0) {
		fprintf(stderr, "tdawn: this machine has no GPU\n");
		return -1;
	}
	screen_w = width;
	screen_h = height;
	screen = big_memory(2 * width * height);
	cursor = seglMemory(CURSOR_MAX * CURSOR_MAX);
	hw_image = seglMemory(HW_SIDE * HW_SIDE * 4);
	if (!screen || !cursor) {
		fprintf(stderr, "tdawn: no GPU memory left\n");
		return -1;
	}
	memset(screen, 0, 2 * width * height);
	glGenTextures(2, page_texture);
	glGenTextures(1, &cursor_texture);
	for (page_rows = 256; page_rows < height; page_rows *= 2)
		;
	for (page = 0; page < 2; page++) {
		glBindTexture(GL_TEXTURE_2D, page_texture[page]);
		seglTexturePointer(screen + page * width * height, width, page_rows, GL_COLOR_INDEX8_EXT);
	}
	pointer_x = width / 2;
	pointer_y = height / 2;
	window_origin();
	frames_room();
	return 0;
}

unsigned char *
host_page(int page)
{
	return screen ? screen + page * screen_w * screen_h : NULL;
}

unsigned int *
host_palette(void)
{
	return seglPalette();
}

/*
 * The pointer is the display's cursor where it can be (docs/machine/display.md, docs/games/tdawn.md): a
 * picture of 32 x 32 colours that whatever shows the display draws at the pointer, at the
 * display's own rate and not at the game's. The window system owns that cursor, so the
 * picture is given to it as a window's cursor is: one of 32 x 1 whose mask is a mark the
 * screen driver knows (linux/nanox/scr_shaderemu.c) and whose bits are the picture's address
 * in the GPU's memory. A hot spot is the cursor's, so there is one cursor for each the game
 * has used; the picture is changed where it lies. Where it cannot be (a pointer over 32
 * pixels, a window the picture is stretched over) the pointer is a rectangle of the scene, as
 * it was, under the window system's arrow. TDAWN_POINTER=drawn is that everywhere.
 */
static GR_CURSOR_ID hw_set = (GR_CURSOR_ID)-1, hw_none;
static struct { int hot_x, hot_y; GR_CURSOR_ID id; } hw_cursors[16];
static int hw_cursor_count;
static struct { unsigned char index; unsigned int colour; } hw_used[24];	/* the picture's first colours, as they were */
static int hw_used_count;

static GR_CURSOR_ID
hw_cursor(uint32_t address, int hot_x, int hot_y)
{
	GR_BITMAP bits[2] = {(GR_BITMAP)(address >> 16), (GR_BITMAP)address}, mark[2] = {0x5345, 0x4355};

	return GrNewCursor(HW_SIDE, 1, hot_x, hot_y, 0, 0, bits, mark);
}

static void
hw_picture(void)
{
	const unsigned int *palette = seglPalette();
	int r, c, i;

	hw_used_count = 0;
	for (r = 0; r < HW_SIDE; r++)
		for (c = 0; c < HW_SIDE; c++) {
			unsigned char index = r < cursor_h && c < cursor_w ? cursor[r * cursor_w + c] : 0;

			hw_image[r * HW_SIDE + c] = index ? 0xff000000u | (palette[index] & 0xffffff) : 0;
			if (!index || hw_used_count == (int)(sizeof hw_used / sizeof hw_used[0]))
				continue;
			for (i = 0; i < hw_used_count && hw_used[i].index != index; i++)
				;
			if (i == hw_used_count) {
				hw_used[i].index = index;
				hw_used[i].colour = palette[index];
				hw_used_count++;
			}
		}
	hw_stale = 0;
}

/* The pointer as it is now, where it is shown: after anything that changes either. */
static void
pointer_publish(void)
{
	static int drawn = -1;
	GR_CURSOR_ID id = 0;	/* the window system's own arrow */
	int width, height, fits, i;

	if (!hw_image)
		return;
	if (drawn < 0) {
		const char *how = getenv("TDAWN_POINTER");

		drawn = how && !strcmp(how, "drawn");
	}
	seglSize(&width, &height);
	fits = !drawn && width == screen_w && height == screen_h && cursor_w > 0 && cursor_w <= HW_SIDE && cursor_h <= HW_SIDE &&
		cursor_hot_x >= 0 && cursor_hot_x < HW_SIDE && cursor_hot_y >= 0 && cursor_hot_y < HW_SIDE;
	if (!pointer_wanted) {
		if (!hw_none)
			hw_none = hw_cursor(0, 0, 0);
		id = hw_none;
	} else if (fits) {
		for (i = 0; i < hw_cursor_count && (hw_cursors[i].hot_x != cursor_hot_x || hw_cursors[i].hot_y != cursor_hot_y); i++)
			;
		if (i == hw_cursor_count && i < (int)(sizeof hw_cursors / sizeof hw_cursors[0])) {
			hw_cursors[i].hot_x = cursor_hot_x;
			hw_cursors[i].hot_y = cursor_hot_y;
			hw_cursors[i].id = hw_cursor(seglAddress(hw_image), cursor_hot_x, cursor_hot_y);
			hw_cursor_count++;
		}
		fits = i < hw_cursor_count && hw_cursors[i].id;
		if (fits) {
			id = hw_cursors[i].id;
			if (hw_stale)
				hw_picture();
		}
	}
	hw_on = fits;
	if (id != hw_set) {
		hw_set = id;
		GrSetWindowCursor(window, id);
		GrFlush();
	}
}

/* The palette is not the one the display's picture of the pointer was made with: made again. */
static void
pointer_colours(void)
{
	const unsigned int *palette = seglPalette();
	int i;

	for (i = 0; i < hw_used_count; i++)
		if (palette[hw_used[i].index] != hw_used[i].colour) {
			hw_picture();
			return;
		}
}

void
host_cursor(const unsigned char *pixels, int width, int height, int hot_x, int hot_y)
{
	if (!cursor || width <= 0 || height <= 0 || width > CURSOR_MAX || height > CURSOR_MAX)
		return;
	scene_finish();
	host_settle();
	memcpy(cursor, pixels, width * height);
	cursor_w = width;
	cursor_h = height;
	cursor_hot_x = hot_x;
	cursor_hot_y = hot_y;
	glBindTexture(GL_TEXTURE_2D, cursor_texture);
	seglTexturePointer(cursor, width, height, GL_COLOR_INDEX8_EXT);
	hw_stale = 1;
	pointer_publish();
}

/* A rectangle of the screen's pixels showing the whole of a texture. */
static void
quad(GLuint texture, int x, int y, int width, int height, int keyed)
{
	static const GLfixed uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	/* in units of the whole window: a matrix of 16.16 numbers cannot hold 2/640 exactly */
	GLfixed x0 = (GLfixed)(((long long)x << 16) / screen_w), x1 = (GLfixed)(((long long)(x + width) << 16) / screen_w);
	GLfixed y0 = (GLfixed)(((long long)y << 16) / screen_h), y1 = (GLfixed)(((long long)(y + height) << 16) / screen_h);
	GLfixed xyz[12] = {x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0};

	seglQuad(xyz, uv, texture, 255, 255, keyed);
}

/* TDAWN_RESIZE=WIDTHxHEIGHT gives the window that size after its 60th frame, as a drag would. */
static void
resize_for_test(void)
{
	static int shown;
	const char *size, *x;

	if (++shown != 60 || !(size = getenv("TDAWN_RESIZE")) || !(x = strchr(size, 'x')))
		return;
	GrResizeWindow(window, atoi(size), atoi(x + 1));
}

/* The window's matrices: glOrthox(0, 1, 1, 0, -1, 1), and a place for the map's corner. */
static void
whole_window(int origin_x, int origin_y)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	int width, height;

	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	if (origin_x || origin_y)
		glTranslatex(origin_x * ONE / screen_w, origin_y * ONE / screen_h, 0);
	glDisable(GL_DEPTH_TEST);
}

/* ---- the map drawn by the GPU (docs/games/tdawn.md) ---- */

#define ATLAS_W		1024		/* texels across: a texture coordinate is a 1,024th */
#define ATLAS_ROWS	2048
#define VIEW_ROWS	1024		/* what one texture of it shows; the sprites' start at 512 and 1024 */
#define SHADE_W		256
#define SHADE_H		256

static unsigned char *atlas, *masks;
static unsigned int *shades, *tiles;
static GLuint icon_texture, sprite_texture[2], mask_texture[2], shade_texture;
static int tile_count;
static int scene_built, scene_shown;	/* a scene waits for its swap; the list shown last has one */
static int scenes_shown;
static int scene_rect[4], scene_key;	/* where the page lets the scene through, and by which index */
static unsigned int *page_commands[2], *cursor_command, *cursor_vertices;

int
host_scene_open(int cells)
{
	int i;

	if (!screen)
		return -1;
	if (tiles)
		return cells <= tile_count ? 0 : -1;
	atlas = big_memory(ATLAS_W * ATLAS_ROWS);
	masks = seglMemory(ATLAS_W / 8 * ATLAS_ROWS);
	shades = seglMemory(SHADE_W * SHADE_H * 4);
	tiles = seglMemory(cells * 64);
	if (!atlas || !masks || !shades || !tiles) {
		fprintf(stderr, "tdawn: no GPU memory for the map: the game draws it\n");
		tiles = NULL;
		return -1;
	}
	memset(tiles, 0, cells * 64);
	memset(masks, 0, ATLAS_W / 8 * ATLAS_ROWS);
	tile_count = cells;
	glGenTextures(1, &icon_texture);
	glGenTextures(2, sprite_texture);
	glGenTextures(2, mask_texture);
	glGenTextures(1, &shade_texture);
	glBindTexture(GL_TEXTURE_2D, icon_texture);
	seglTexturePointer(atlas, ATLAS_W, VIEW_ROWS, GL_COLOR_INDEX8_EXT);
	for (i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, sprite_texture[i]);
		seglTexturePointer(atlas + (i + 1) * 512 * ATLAS_W, ATLAS_W, VIEW_ROWS, GL_COLOR_INDEX8_EXT);
		glBindTexture(GL_TEXTURE_2D, mask_texture[i]);
		seglTexturePointer(masks + (i + 1) * 512 * (ATLAS_W / 8), ATLAS_W, VIEW_ROWS, SEGL_BITS);
	}
	glBindTexture(GL_TEXTURE_2D, shade_texture);
	seglTexturePointer(shades, SHADE_W, SHADE_H, GL_RGBA);
	seglKeep(1, cells);
	seglKeptQuads(0, tiles, 0, cells, icon_texture, 255, 0);
	return 0;
}

unsigned char *
host_atlas(void)
{
	host_settle();
	return atlas;
}

unsigned char *
host_masks(void)
{
	host_settle();
	return masks;
}

unsigned int *
host_shades(void)
{
	host_settle();
	return shades;
}

/* A cell of the map: 24 pixels square at a place of the map, from the atlas. */
void
host_tile(int index, int map_x, int map_y, int atlas_x, int atlas_y)
{
	unsigned int *to = tiles + 16 * index;
	unsigned int x0, y0, x1, y1, u0 = atlas_x, u1 = atlas_x + 24, v0 = atlas_y << 16, v1 = (atlas_y + 24) << 16;

	if (!tiles || index < 0 || index >= tile_count)
		return;
	host_settle();
	if (atlas_x < 0) {
		memset(to, 0, 64);
		return;
	}
	x0 = map_x * ONE / screen_w, x1 = (map_x + 24) * ONE / screen_w;
	y0 = map_y * ONE / screen_h, y1 = (map_y + 24) * ONE / screen_h;
	to[0] = x0, to[1] = y0, to[2] = 0, to[3] = u0 | v0;
	to[4] = x1, to[5] = y0, to[6] = 0, to[7] = u1 | v0;
	to[8] = x1, to[9] = y1, to[10] = 0, to[11] = u1 | v1;
	to[12] = x0, to[13] = y1, to[14] = 0, to[15] = u0 | v1;
}

void
host_tiles_clear(void)
{
	host_settle();
	if (tiles)
		memset(tiles, 0, tile_count * 64);
}

/*
 * Shape files' frames on worker cores (docs/games/tdawn.md, "Frames made ready"): a file a frame
 * was asked of is unpacked from that frame on, a run of frames a turn, into memory that is
 * the worker's to write. TDAWN_FRAMES_CORE=0 leaves every frame to the game, =N has N cores.
 */
int ShaderEmu_Frame_Size(const void *file, int *width, int *height);	/* keyframe.cpp's */
int ShaderEmu_Frame_Step(const void *file, int frame, void *buffer, int have);
enum { FRAME_FILES = 240, FRAME_SLOTS = 40960, FRAMES_ARENA = 8 << 20, FRAMES_PIECE = 128 << 10, FRAME_CORES = 6 };
#ifdef SHADEREMU_RA
enum { FRAMES_FIRST = 3 << 20 };	/* the arena's pages that are there from the program's start */
#else
enum { FRAMES_FIRST = 2 << 20 };
#endif
enum { FRAMES_MORE = 8 << 10, FRAMES_SPARE = 2 * FRAMES_PIECE };
enum { FILE_WAITS, FILE_OUT, FILE_RESTS, FILE_MADE, FILE_LEFT };
enum { FRAMES_TURN = 64, FRAMES_BYTES = 128 << 10, FRAMES_NEAR = 12 };
/* (asks: counted up when `from` is set while a core has the file, which then goes on from there) */
struct frame_file { const void *file; struct host_frame *frames; int count, width, height, state; volatile int from, asks; };
static struct frame_file frame_files[FRAME_FILES];
static unsigned char frame_hash[256];	/* a file's place in those, + 1 */
static int frame_file_count, frame_waiting, frame_slots_used;
static int frames_wait;	/* how many files wait for a turn */
static struct host_frame *frame_slots;	/* a frame's slot is its worker's to write */
static unsigned char *frames_arena;
static unsigned int frames_given, frames_limit;	/* the arena is shared out this far, and its pages are there this far */
/* A core that unpacks: its piece of the arena and its buffer (this core stores here while that one has no job). */
static struct frames_core { unsigned char *at, *end, *scratch; int job, handle, unused[3];	/* job: its file + 1 */ }
	frames_cores[FRAME_CORES] __attribute__((aligned(16)));
static int frames_most = -1;
static volatile int frames_stop;
unsigned int host_frames_bytes;

/*
 * A file's frames, each cut to the rectangle that has pixels: so many of them a turn (or so
 * many bytes: large ones are few), from the one asked for last. The answer's low bits: 1 the file is done, 2 out of room, 3 more of it
 * another turn, 0 stopped; above them the bytes of the piece used.
 */
static uint32_t
frames_job(uint32_t what, uint32_t turn)
{
	uint32_t began = host_cycles(), how = frames_turn(what, turn);

	host_job_time(HOST_JOB_FRAMES, began);
	return how;
}

static uint32_t
frames_turn(uint32_t what, uint32_t turn)
{
	struct frame_file *f = &frame_files[what & 0xffff];
	const struct frames_core *core = &frames_cores[what >> 16];
	unsigned char *at = core->at, *scratch = core->scratch;
	int y, width = f->width, size = f->width * f->height, have = -1, how = 1, bytes = FRAMES_BYTES;
	int frame = f->from, asks = f->asks, passed = 0;

	for (;; frame = frame + 1 < f->count ? frame + 1 : 0) {
		struct host_frame *r;
		int x0 = width, x1 = -1, y0 = -1, y1 = -1, w, h, spans;
		unsigned char *to;
		unsigned int *seen;

		if (f->asks != asks)	/* (a frame was missed elsewhere in the file) */
			asks = f->asks, frame = f->from, passed = 0;
		r = &f->frames[frame];
		if (r->state) {
			if (++passed >= f->count)
				break;
			continue;
		}
		passed = 0;
		if (frames_stop || mcw_recalled(mcw_self())) {
			how = 0;
			break;
		}
		if (turn-- == 0 || bytes <= 0) {
			how = 3;
			break;
		}
		bytes -= size;
		if (at + size + 4 * f->height + 64 > core->end) {
			how = 2;
			break;
		}
		if (!ShaderEmu_Frame_Step(f->file, frame, scratch, have)) {
			r->state = -1;
			have = -1;
			continue;
		}
		have = frame;
		for (y = 0; y < f->height; y++) {
			const unsigned char *row = scratch + y * width;
			int a = host_first_pixel(row, width), b;

			if (a == width)
				continue;
			b = host_last_pixel(row, width);
			y0 = y0 < 0 ? y : y0, y1 = y;
			x0 = a < x0 ? a : x0, x1 = b > x1 ? b : x1;
		}
		if (y0 < 0) {
			r->state = HOST_FRAME_EMPTY;
			continue;
		}
		w = x1 - x0 + 1, h = y1 - y0 + 1;
		seen = (unsigned int *)at;
		memset(seen, 0, 32);
		to = at + 32;
		r->pixels = to;
		r->left = (short)x0, r->top = (short)y0;
		r->w = (short)w, r->h = (short)h;
		for (y = y0; y <= y1; y++, to += w) {
			const unsigned char *from = scratch + y * width + x0;
			int x;

			for (x = 0; x < w; x++)
				seen[from[x] >> 5] |= 1u << (from[x] & 31);
			memcpy(to, from, w);
		}
		spans = 1;
		if (spans) {
			/* (for the game's own drawing: a row's first pixel and last, and whether a hole lies between) */
			struct host_span *s = (struct host_span *)(r->pixels + ((w * h + 3) & ~3));

			for (y = 0; y < h; y++, s++) {
				const unsigned char *row = r->pixels + y * w;
				int a = host_first_pixel(row, w), b = a == w ? -1 : host_last_pixel(row, w), k;

				if (b < 0) {
					s->first = 1, s->last = 0;
					continue;
				}
				for (k = a; k <= b && row[k]; k++)
					;
				s->first = (unsigned short)(a | (k <= b ? 0x8000 : 0)), s->last = (unsigned short)b;
			}
			to = (unsigned char *)s;
		}
		at += (unsigned int)(to - at + 15) & ~15u;
		r->state = spans ? HOST_FRAME_READY | HOST_FRAME_SPANS : HOST_FRAME_READY;	/* (last: the other core looks at this) */
	}
	return (uint32_t)how | (uint32_t)(at - core->at) << 2;
}

/* A core's job has ended: its file waits again, rests until a frame of it is missed, or is done. */
static void
frames_took(struct frames_core *core)
{
	struct frame_file *f = &frame_files[core->job - 1];
	uint32_t how = mcw_join(core->handle);

	core->at += how >> 2;
	host_frames_bytes += how >> 2;
	f->state = (how & 3) == 1 ? FILE_MADE : (how & 3) == 3 ? FILE_RESTS : FILE_WAITS;
	frames_wait += f->state == FILE_WAITS;
	core->job = 0;
}

/*
 * The cores' files are seen done, and the files that wait given to cores with none. more 1: once
 * a frame, with the arena's next pages; -1: only cores that have no file are looked at.
 */
static void
frames_pump(int more)
{
	int c, tries;

	if (scene_cores < 2 || !frames_arena)
		return;
	for (c = 0; c < frames_most && c + 1 < scene_cores; c++) {
		struct frames_core *core = &frames_cores[c];
		struct frame_file *f;
		unsigned int need;

		if (core->job) {
			if (more < 0 || !mcw_ready(core->handle))
				continue;
			frames_took(core);
		}
		if (!frames_wait)
			continue;
		/* (the files in turns: one that is new has its first frames before an old one its last) */
		for (tries = 0; tries < frame_file_count; tries++) {
			frame_waiting = frame_waiting + 1 < frame_file_count ? frame_waiting + 1 : 0;
			if (frame_files[frame_waiting].state == FILE_WAITS)
				break;
		}
		if (tries == frame_file_count) {
			frames_wait = 0;
			break;
		}
		f = &frame_files[frame_waiting];
		need = (unsigned int)(f->width * f->height + 4 * f->height + 64);
		if (core->at + need > core->end) {
			/* another piece of the arena, of pages that are there */
			if (frames_given + FRAMES_PIECE > frames_limit) {
				if (frames_given + FRAMES_PIECE > FRAMES_ARENA) {
					f->state = FILE_LEFT;
					frames_wait--;
				}
				continue;
			}
			core->at = frames_arena + frames_given;
			core->end = core->at + FRAMES_PIECE;
			frames_given += FRAMES_PIECE;
		}
		/* (on a worker with nothing to do, or the file waits on) */
		core->handle = mcw_try_call(frames_job, (uint32_t)(frame_waiting | c << 16), FRAMES_TURN);
		if (!core->handle)
			break;
		frames_wait--;
		core->job = frame_waiting + 1;
		f->state = FILE_OUT;
	}
	/* (the arena's pages a few at a time, ahead of their use: 32 at once were a long frame) */
	if (more > 0 && frames_limit < FRAMES_ARENA && frames_limit < frames_given + FRAMES_SPARE) {
		mcw_touch(frames_arena + frames_limit, FRAMES_MORE);
		frames_limit += FRAMES_MORE;
	}
}

/* The file's place among those known, made if it is new (NULL: no room for more). */
static struct frame_file *
frames_file(const void *file, int frame, int *is_new)
{
	unsigned int at = (unsigned int)(((uintptr_t)file >> 4) * 2654435761u) >> 24;
	struct frame_file *f;

	*is_new = 0;
	for (; frame_hash[at]; at = (at + 1) & 255)
		if (frame_files[frame_hash[at] - 1].file == file)
			return &frame_files[frame_hash[at] - 1];
	if (frame_file_count == FRAME_FILES)
		return NULL;
	f = &frame_files[frame_file_count++];
	frame_hash[at] = (unsigned char)frame_file_count;
	f->file = file;
	f->count = ShaderEmu_Frame_Size(file, &f->width, &f->height);
	f->from = frame > 0 && frame < f->count ? frame : 0;
	f->state = FILE_LEFT;
	if (f->count > 0 && f->width > 0 && f->height > 0 && f->width * f->height < 65536
	    && frame_slots_used + f->count <= FRAME_SLOTS) {
		f->frames = frame_slots + frame_slots_used;
		frame_slots_used += f->count;
		f->state = FILE_WAITS;
		frames_wait++;
	}
	*is_new = 1;
	return f;
}

/*
 * On the worker that draws the objects behind: a frame as it is, asked for nothing; one that is
 * not ready is noted, and asked for by the first core when it joins (frames_missed()).
 */
#define MISSES 64
static struct { const void *file; int frame, unused[2]; } misses[MISSES] __attribute__((aligned(16)));

static const struct host_frame *
frame_seen(const void *file, int frame)
{
	unsigned int at = (unsigned int)(((uintptr_t)file >> 4) * 2654435761u) >> 24;
	const struct frame_file *f = NULL;

	for (; frame_hash[at]; at = (at + 1) & 255)
		if (frame_files[frame_hash[at] - 1].file == file) {
			f = &frame_files[frame_hash[at] - 1];
			break;
		}
	if (f && f->state != FILE_LEFT && (unsigned int)frame < (unsigned int)f->count && f->frames[frame].state > 0)
		return &f->frames[frame];
	if (missed < MISSES)
		misses[missed].file = file, misses[missed++].frame = frame;
	return NULL;
}

static void
frames_missed(void)
{
	int i;

	for (i = 0; i < missed; i++)
		host_frame(misses[i].file, misses[i].frame);
	missed = 0;
}

const struct host_frame *
host_frame(const void *file, int frame)
{
	struct frame_file *f;
	const struct host_frame *r;
	int is_new, near;

	if (scene_cores < 2 || !frames_arena)
		return NULL;
	if (mcw_self())
		return frame_seen(file, frame);
	f = frames_file(file, frame, &is_new);
	if (f && is_new)
		frames_pump(-1);
	if (!f || is_new || f->state == FILE_LEFT || (unsigned int)frame >= (unsigned int)f->count)
		return NULL;
	r = &f->frames[frame];
	near = (frame + FRAMES_NEAR) % f->count;
	if (f->state != FILE_MADE && (!r->state || (r->state > 0 && !f->frames[near].state))) {
		/* its turn goes on from here: at this frame, or after it where what is ready ends soon */
		f->from = r->state ? (frame + 1) % f->count : frame;
		if (f->state == FILE_OUT) {
			f->asks++;
		} else if (f->state == FILE_RESTS) {
			f->state = FILE_WAITS;
			frames_wait++;
			frames_pump(-1);	/* (a core now: the frames after this one are wanted in the next frame) */
		}
	}
	if (r->state <= 0)
		frames_pump(0);	/* (a score screen has no scene to give the files their turns) */
	return r->state > 0 ? r : NULL;
}

void
host_frames_ahead(const void *file, int frame)
{
	int is_new;

	workers_open();
	if (file && scene_cores >= 2 && frames_arena)
		frames_file(file, frame, &is_new);
}

/* The workers are told to end their files, and have by the time this returns. */
static void
frames_halt(void)
{
	int c;

	frames_stop = 1;
	for (c = 0; c < FRAME_CORES; c++)
		if (frames_cores[c].job) {
			frames_took(&frames_cores[c]);
		}
	frames_stop = 0;
}

/* Work put off to the core that makes the scene (host.h). */
#define FILLS_MOST 1024
static struct { host_fill_fn fn; void *what; const void *from; int unused; } fills[FILLS_MOST] __attribute__((aligned(16)));

static void
fills_now(int count)
{
	int i;

	for (i = 0; i < count; i++)
		fills[i].fn(fills[i].what, fills[i].from);
}

void
host_fill(host_fill_fn fn, void *what, const void *from)
{
	if (!mcw_self())
		scene_finish();		/* (the list is the worker's while it has a scene) */
	if (scene_cores <= 0 || fill_count == FILLS_MOST) {
		fn(what, from);
		return;
	}
	fills[fill_count].fn = fn, fills[fill_count].what = what, fills[fill_count].from = from;
	fill_count++;
}

void
host_frames_settle(void)
{
	scene_finish();
	fills_now(fill_count);	/* (what they were given goes now) */
	fill_count = 0;
}

void
host_frames_forget(void)
{
	int c;

	host_frames_settle();
	frames_halt();
	if (frame_slots && frame_slots_used)
		memset(frame_slots, 0, (unsigned)frame_slots_used * sizeof *frame_slots);
	memset(frame_hash, 0, sizeof frame_hash);
	frame_file_count = frame_waiting = frame_slots_used = frames_wait = 0;
	frames_given = 0;	/* (the arena from its start again: its pages stay) */
	for (c = 0; c < FRAME_CORES; c++)
		frames_cores[c].at = frames_cores[c].end = NULL;
}

/* How many cores unpack shape files (TDAWN_FRAMES_CORE=N; 0: the game builds every frame). */
static int
frames_wanted(void)
{
	const char *how = getenv("TDAWN_SCENE");

	if (frames_most < 0) {
		frames_most = how && !strcmp(how, "inline") ? 0 : FRAME_CORES;
		how = getenv("TDAWN_FRAMES_CORE");
		if (how && atoi(how) < frames_most)
			frames_most = atoi(how) < 0 ? 0 : atoi(how);
	}
	return frames_most;
}

/* The frames' arena, its first megabyte there: asked for as the program starts, not in a mission's frame. */
static void
frames_room(void)
{
	int c;

	if (frames_arena || !frames_wanted() || mcw_most() < 2)
		return;
	frames_arena = mcw_alloc(FRAMES_ARENA);
	frame_slots = mcw_alloc(FRAME_SLOTS * sizeof *frame_slots);
	for (c = 0; c < FRAME_CORES; c++) {
		frames_cores[c].scratch = mcw_alloc(4096 + 65536 + 4096);
		if (!frames_cores[c].scratch) {
			frames_arena = NULL;
			break;
		}
		mcw_touch(frames_cores[c].scratch, 4096 + 65536 + 4096);
		frames_cores[c].scratch += 4096;	/* (a bad file's copies reach back) */
	}
	if (!frames_arena || !frame_slots) {
		frames_arena = NULL;
		frames_most = 0;
		return;
	}
	mcw_touch(frame_slots, FRAME_SLOTS * sizeof *frame_slots);
	mcw_touch(frames_arena, FRAMES_FIRST);
	frames_limit = FRAMES_FIRST;
}

/* The worker cores are asked for: the first time, and after they were given back. */
static void
workers_open(void)
{
	if (run_single < 0) {
		run_single = getenv("TDAWN_SPRITES") && !strcmp(getenv("TDAWN_SPRITES"), "one");
		list_check = getenv("TDAWN_CHECK_LIST") != NULL;
		if (list_check)
			atexit(list_report);
	}
	if (scene_cores < 0 || (scene_cores > 0 && mcw_count() == 0)) {
		const char *how = getenv("TDAWN_SCENE");
		static int at_exit;

		/* the scene's, the frames', the paths', those that ask about cells and the frames' others */
		/* (a worker for the scene, the frames', the paths' and those that ask about cells: any of them for any) */
		scene_cores = how && !strcmp(how, "inline") ? 0 : mcw_open(1 + PATH_CORES + PRE_CORES + FRAME_CORES);
		if (scene_cores > 0 && !at_exit) {
			extern char __DATA_BEGIN__[], _end[];

			at_exit = 1;
			atexit(host_workers_close);
			mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
			if (getenv("TDAWN_MARK") || getenv("RALERT_MARK"))
				jobs_counted = mcw_counters() >= HOST_JOBS;
		}
		if (scene_cores > 1)
			frames_room();
	}
}

/* How many of a unit's path searches other cores can make now (they are workers 3 and up). */
int
host_path_cores(void)
{
	static int most = -1;
	int k;

	if (most < 0) {
		const char *how = getenv("TDAWN_PATH_CORES");

		most = how ? atoi(how) : PATH_CORES;
		if (most > PATH_CORES)
			most = PATH_CORES;
	}
	if (most <= 0)
		return 0;
	workers_open();
	k = scene_cores - 1;
	return k < most ? (k < 0 ? 0 : k) : most;
}

static unsigned int (*path_search)(unsigned int, unsigned int), (*pre_ask)(unsigned int, unsigned int);
static mcw_handle path_handle[PATH_CORES + 1], pre_handle[PRE_CORES + 1];

static uint32_t
path_job(uint32_t object, uint32_t what)
{
	uint32_t began = host_cycles(), done = path_search(object, what);

	host_job_time(HOST_JOB_PATH, began);
	return done;
}

/* On a worker with nothing to do: 0 if none has. */
int
host_path_post(int k, unsigned int (*search)(unsigned int, unsigned int), unsigned int object, unsigned int what)
{
	path_search = search;
	path_handle[k] = mcw_try_call(path_job, object, what);
	return path_handle[k] != 0;
}

void
host_path_wait(int k)
{
	mcw_join(path_handle[k]);
	path_handle[k] = 0;
}

/* How many cores can ask about cells for a search now (TDAWN_PRE_CORES=N: no more than N), from 0. */
int
host_pre_cores(void)
{
	static int most = -1;
	int k;

	if (most < 0) {
		const char *how = getenv("TDAWN_PRE_CORES");

		most = how ? atoi(how) : PRE_CORES;
		if (most > PRE_CORES)
			most = PRE_CORES;
	}
	if (most <= 0 || host_path_cores() <= 0)
		return 0;
	k = scene_cores - 1 - PATH_CORES;
	return k < most ? (k < 0 ? 0 : k) : most;
}

static uint32_t
pre_job(uint32_t object, uint32_t rows)
{
	uint32_t began = host_cycles(), done = pre_ask(object, rows);

	host_job_time(HOST_JOB_PRE, began);
	return done;
}

int
host_pre_post(int k, unsigned int (*ask)(unsigned int, unsigned int), unsigned int object, unsigned int what)
{
	pre_ask = ask;
	pre_handle[k] = mcw_try_call(pre_job, object, what);
	return pre_handle[k] != 0;
}

void
host_pre_wait(int k)
{
	mcw_join(pre_handle[k]);
	pre_handle[k] = 0;
}

/* A frame's scene begins: the map's pixel (0, 0) is at this pixel of the screen. */
void
host_scene_begin(int origin_x, int origin_y)
{
	scene_finish();		/* the frame before is shown by now */
	host_settle();		/* and drawn: the worker's fills and the kept rectangles are stored from here */
	workers_open();
	frames_pump(0);
	scene_part = 1;
	scene_noted[1] = 0;
	if (scene_built)
		seglDiscard();		/* one nobody showed */
	scene_at[0] = origin_x, scene_at[1] = origin_y;
	whole_window(origin_x, origin_y);
	seglKeptMatrices();
	glLoadIdentity();
	glClearColorx(0, 0, 0, ONE);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorKeySE(0);
	scene_built = 1;
}

/*
 * The scene's rectangles go to the list a batch at a time (seglSprites: what seglSprite()
 * makes of them one by one, for a fifth of the instructions). TDAWN_SPRITES=one is one by
 * one, for TDAWN_CHECK_LIST.
 */
#define BATCH 128
static SEGLsprite batch[BATCH] __attribute__((aligned(16)));	/* (the scene's core's alone) */

static void
run_end(void)
{
	seglSprites(batch, batched);
	batched = 0;
}

static inline __attribute__((always_inline)) void
put_sprite(GLuint name, unsigned int colour, int keyed, int x, int y, int width, int height, const int *texels)
{
	SEGLsprite *s = &batch[batched];

	run_quads++;
	s->box[0] = x * ONE / screen_w, s->box[1] = y * ONE / screen_h;
	s->box[2] = (x + width) * ONE / screen_w, s->box[3] = (y + height) * ONE / screen_h;
	s->texels[0] = texels[0], s->texels[1] = texels[1], s->texels[2] = texels[2], s->texels[3] = texels[3];
	s->name = name, s->colour = colour, s->keyed = keyed;
	if (run_single)
		seglSprite(s->box, s->texels, name, colour, keyed);
	else if (++batched == BATCH)
		run_end();
}

/* A rectangle of the screen from an atlas, over what the scene has so far. */
static inline __attribute__((always_inline)) void
sprite_now(int kind, int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour)
{
	int texels[4];
	GLuint name;

	if (kind == HOST_SHADE) {
		texels[0] = atlas_x * (1024 / SHADE_W), texels[1] = atlas_y * (1024 / SHADE_H);
		texels[2] = (atlas_x + width) * (1024 / SHADE_W), texels[3] = (atlas_y + height) * (1024 / SHADE_H);
		name = shade_texture;
	} else {
		/* the texture that shows these rows: the first for the icons' rows, then two a half apart */
		int view = atlas_y + height <= VIEW_ROWS && kind != HOST_MASK ? 0 : atlas_y + height <= VIEW_ROWS + 512 ? 1 : 2;

		atlas_y -= view * 512;
		texels[0] = atlas_x, texels[1] = atlas_y, texels[2] = atlas_x + width, texels[3] = atlas_y + height;
		name = kind == HOST_MASK ? mask_texture[view ? view - 1 : 0] : view ? sprite_texture[view - 1] : icon_texture;
	}
	put_sprite(name, colour, kind == HOST_KEYED, x, y, width, height, texels);
}

/* A rectangle of one colour: the middle of 24 x 24 set bits in the masks, as large as wanted. */
static inline __attribute__((always_inline)) void
block_now(int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour)
{
	int view = atlas_y + 24 <= VIEW_ROWS + 512 ? 1 : 2;
	int texels[4] = {atlas_x + 8, atlas_y - view * 512 + 8, atlas_x + 16, atlas_y - view * 512 + 16};

	put_sprite(mask_texture[view - 1], colour, 0, x, y, width, height, texels);
}

/* TDAWN_CHECK_LIST=1: a sum of every scene's vertices and draw commands as made, said at the end. */
static uint32_t list_sum, list_words;

static void
list_report(void)
{
	fprintf(stderr, "tdawn: the scenes' lists: sum %08x of %u words\n", list_sum, list_words);
}

static void
list_add(const unsigned int *from, const unsigned int *to)
{
	for (; from < to; from++, list_words++)
		list_sum = (list_sum ^ *from) * 16777619u;
}

/* ---- the same on a worker core ---- */

#define NOTE_BLOCK 3
#define NOTE_QUADS 4	/* x: where the caller's rectangles are; y: how many */

/* The notes of every part made into the GPU's commands, in the parts' order. */
static uint32_t
scene_job(uint32_t counts, uint32_t last)
{
	uint32_t count[SCENE_PARTS] = {counts & 0xffff, counts >> 16, last & 0xffff}, began = host_cycles();
	int part;
	unsigned int *vertices = NULL, *commands = NULL;
	GLsizei room;

	fills_now((int)(last >> 16));
	host_job_time(HOST_JOB_FILLS, began);
	if (list_check) {
		vertices = seglSpriteSpace(&room);
		commands = seglLastCommand();
	}
	for (part = 0; part < SCENE_PARTS; part++) {
		const struct scene_note *n = scene_notes[part];
		uint32_t i;

		for (i = 0; i < count[part]; i++, n++)
			if (n->kind == NOTE_QUADS) {
				const struct host_quad *q = (const struct host_quad *)(uintptr_t)n->x;
				int k;

				for (k = 0; k < n->y; k++, q++) {
					int x = q->x + scene_at[0], y = q->y + scene_at[1];

					if (x < scene_view[2] && y < scene_view[3] && x + q->w > scene_view[0] && y + q->h > scene_view[1])
						sprite_now(q->kind, x, y, q->w, q->h, q->ax, q->ay, q->colour);
				}
			} else if (n->kind == NOTE_BLOCK)
				block_now(n->x, n->y, n->width, n->height, n->atlas_x, n->atlas_y, n->colour);
			else
				sprite_now(n->kind, n->x, n->y, n->width, n->height, n->atlas_x, n->atlas_y, n->colour);
	}
	run_end();
	if (jobs_counted)
		mcw_add(HOST_JOB_QUADS, run_quads);
	run_quads = 0;
	if (list_check && vertices) {
		unsigned int *last_one = seglLastCommand();

		list_add(vertices, seglSpriteSpace(&room));
		if (commands && last_one && commands <= last_one && last_one - commands < (1 << 20))
			list_add(commands, last_one + 16);
	}
	host_job_time(HOST_JOB_SCENE, began);
	return counts;
}

/* One rectangle into host_scene_picture()'s two pictures, as the GPU's pass would lay it. */
static void
picture_rect(unsigned char *index, unsigned int *dark, int kind, int x, int y, int w, int h, int ax, int ay, unsigned int colour)
{
	int r, c;

	for (r = y < 0 ? -y : 0; r < h && y + r < screen_h; r++)
		for (c = x < 0 ? -x : 0; c < w && x + c < screen_w; c++) {
			int at = (y + r) * screen_w + x + c, u = ax + c, v = ay + r;

			if (kind == NOTE_BLOCK) {
				index[at] = 0, dark[at] = 1;
			} else if (kind == HOST_KEYED || kind < 0) {
				if (atlas[v * ATLAS_W + u] || kind < 0)
					index[at] = atlas[v * ATLAS_W + u], dark[at] = 0;
			} else if (kind == HOST_MASK) {
				if (masks[v * (ATLAS_W / 8) + (u >> 3)] & (0x80 >> (u & 7)))
					dark[at] = dark[at] * 31 + (colour >> 24) + 2;
			} else {
				dark[at] = dark[at] * 31 + shades[v * SHADE_W + u] + 1;
			}
		}
}

/*
 * For a test (TDAWN_CHECK_REDRAW): the scene as its notes and the kept cells have it now, drawn
 * here: a pixel's index from the atlas, and a sum of what darkens it. Without the page.
 */
void
host_scene_picture(unsigned char *index, unsigned int *dark)
{
	int part, i, k;

	scene_finish();
	memset(index, 0, screen_w * screen_h);
	memset(dark, 0, screen_w * screen_h * sizeof *dark);
	if (!tiles)
		return;
	for (i = 0; i < tile_count; i++) {
		const unsigned int *t = tiles + 16 * i;

		if (t[8] | t[9])
			picture_rect(index, dark, -1, (int)((t[0] * screen_w + ONE - 1) >> 16) + scene_at[0],
				(int)((t[1] * screen_h + ONE - 1) >> 16) + scene_at[1], 24, 24, (int)(t[3] & 0xffff), (int)(t[3] >> 16), 0);
	}
	for (part = 0; part < SCENE_PARTS; part++) {
		const struct scene_note *n = scene_notes[part];

		for (i = 0; i < scene_noted[part]; i++, n++) {
			const struct host_quad *q = (const struct host_quad *)(uintptr_t)n->x;

			if (n->kind != NOTE_QUADS) {
				picture_rect(index, dark, n->kind, n->x, n->y, n->width, n->height, n->atlas_x, n->atlas_y, n->colour);
				continue;
			}
			for (k = 0; k < n->y; k++, q++) {
				int x = q->x + scene_at[0], y = q->y + scene_at[1];

				if (x < scene_view[2] && y < scene_view[3] && x + q->w > scene_view[0] && y + q->h > scene_view[1])
					picture_rect(index, dark, q->kind, x, y, q->w, q->h, q->ax, q->ay, q->colour);
			}
		}
	}
}

/*
 * For a test (TDAWN_CHECK_GPU): the window's picture as the GPU left it in the window's own
 * buffer (docs/machine/gpu.md), a word a pixel in rows of *row. NULL where the window has no buffer.
 */
const unsigned int *
host_window_picture(int *width, int *height, int *row)
{
	static int fd = -1;
	static unsigned char *map;
	static uint32_t mapped = ~0u;
	GR_WINDOW_INFO info;
	uint32_t at;

	scene_finish();
	host_settle();
	GrGetWindowInfo(window, &info);
	if (!info.surface_address || info.width <= 0)
		return NULL;
#ifdef __gpuvm__
	at = info.surface_address;	/* (the device's memory, from its start: 0 on gpu_vm) */
#else
	at = info.surface_address - 0x86000000u;	/* (the device's memory, from its start) */
#endif
	if (mapped != (at & ~0xfffu)) {
		if (map)
			munmap(map, 4 << 20);
		if (fd < 0)
			fd = open("/dev/gpu", O_RDWR);
		map = mmap(0, 4 << 20, PROT_READ, MAP_SHARED, fd, at & ~0xfffu);
		if (map == MAP_FAILED) {
			map = NULL;
			return NULL;
		}
		mapped = at & ~0xfffu;
	}
	*width = info.width, *height = info.height;
	*row = info.surface_row ? (int)info.surface_row : info.width;
	return (const unsigned int *)(map + (at & 0xfffu));
}

/* A rectangle more than a part has room to note is left out (a part is 4,096 of them). */
void
host_sprite(int kind, int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour)
{
	struct scene_note *n;

	if (scene_noted[scene_part] == SCENE_MOST)
		return;
	n = &scene_notes[scene_part][scene_noted[scene_part]++];
	n->kind = kind, n->x = x, n->y = y, n->width = width, n->height = height;
	n->atlas_x = atlas_x, n->atlas_y = atlas_y, n->colour = colour;
}

void
host_block(int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour)
{
	host_sprite(NOTE_BLOCK, x, y, width, height, atlas_x, atlas_y, colour);
}

void
host_scene_view(int x0, int y0, int x1, int y1)
{
	scene_view[0] = x0, scene_view[1] = y0, scene_view[2] = x1, scene_view[3] = y1;
}

void
host_quads(const struct host_quad *quads, int count)
{
	struct scene_note *n;

	if (scene_noted[scene_part] == SCENE_MOST || count <= 0)
		return;
	n = &scene_notes[scene_part][scene_noted[scene_part]++];
	n->kind = NOTE_QUADS, n->x = (int)(uintptr_t)quads, n->y = count;
}

void
host_scene_settle(void)
{
	scene_finish();
}

/* The rectangles that follow are this part's: after those it has, or in their place. */
void
host_scene_part(int part, int keep)
{
	scene_part = part;
	if (!keep)
		scene_noted[part] = 0;
}

/* The worker's scene is waited for, and the frame that was put off for it is shown. */
static void
scene_finish(void)
{
	if (!scene_out)
		return;
	mcw_join(scene_handle);
	scene_out = 0;
	behind_joined_now();
	frames_missed();
	if (scene_owed) {
		scene_owed = 0;
		host_present(scene_owed_page, scene_owed_cursor);
	}
}

/* TDAWN_MARK's figures: what each kind of job ran, on all cores, from the first mark (ends 0) to the second. */
void
host_workers_mark(int ends, int frames)
{
	static uint32_t at[HOST_JOBS];
	uint32_t ran[HOST_JOBS];
	int k;

	if (!jobs_counted)
		return;
	mcw_counters_settle();
	for (k = 0; k < HOST_JOBS; k++)
		ran[k] = mcw_get(k) - at[k], at[k] += ran[k];
	if (ends && frames > 0)
		fprintf(stderr, "tdworkers: the jobs' own instructions a frame: objects behind %u, scene %u (fills %u, %u quads), frames %u, paths %u, cells %u\n",
			ran[6] / frames, ran[0] / frames, ran[4] / frames, ran[5] / frames, ran[1] / frames, ran[2] / frames, ran[3] / frames);
}

void
host_for(unsigned int n, void (*fn)(void *ctx, unsigned int from, unsigned int to), void *ctx, void *out,
	 unsigned int out_bytes)
{
	mcw_for_opts o = {0};

	workers_open();
	scene_finish();
	frames_halt();	/* (their files go on after) */
	o.out_bytes = out_bytes;
	o.first_idle = 1;	/* (shares are fixed: this core waits on the last one either way) */
	o.touch = out, o.touch_bytes = n * out_bytes;	/* (stored to here first, or a worker stops at its pages) */
	mcw_for_opt(n, (mcw_body)fn, ctx, &o);
	mcw_for_end();	/* (the scene's and the frames' calls come after) */
}

/* Gives the worker cores back (another use of them in this program: Red Alert's movies). */
void
host_workers_close(void)
{
	scene_finish();
	frames_halt();
	if (scene_cores > 0) {
		mcw_for_end();
		mcw_close();
	}
	scene_cores = -1;
}

/* The scene is whole. The page goes over it, with holes of this index inside this rectangle. */
void
host_scene_end(int x, int y, int width, int height, int key)
{
	uint32_t also;

	scene_rect[0] = x, scene_rect[1] = y, scene_rect[2] = width, scene_rect[3] = height;
	scene_key = key;
	also = (uint32_t)(scene_noted[2] | fill_count << 16);
	fill_count = 0;	/* (before the job: its worker stores this texel, `own`) */
	if (scene_cores > 0 && scene_noted[0] + scene_noted[1] + scene_noted[2] >= 20) {
		scene_handle = mcw_call(scene_job, (uint32_t)(scene_noted[0] | scene_noted[1] << 16), also);
		scene_out = 1;
		host_scene_jobs++;
	} else {
		/* (few of them are not worth a pass or two of waiting) */
		scene_job((uint32_t)(scene_noted[0] | scene_noted[1] << 16), also);
	}
}

/* Whether host_scene_behind() would give its drawing to a worker now. */
int
host_scene_behind_ready(void)
{
	return scene_cores > 0 && mcw_count() > 0;
}

static void (*behind_draw)(void), (*behind_joined)(void);

static void
behind_joined_now(void)
{
	void (*joined)(void) = behind_joined;

	behind_joined = NULL;
	if (joined)
		joined();
}

static uint32_t
behind_job(uint32_t unused, uint32_t also)
{
	uint32_t began = host_cycles();

	(void)unused, (void)also;
	behind_draw();
	host_job_time(HOST_JOB_BEHIND, began);
	began = scene_job((uint32_t)(scene_noted[0] | scene_noted[1] << 16), (uint32_t)(scene_noted[2] | fill_count << 16));
	fill_count = 0;
	return began;
}

/* host_scene_end() after draw(), which notes the objects' part, both on a worker (gl.cpp). */
void
host_scene_behind(void (*draw)(void), int x, int y, int width, int height, int key, void (*joined)(void))
{
	if (!host_scene_behind_ready()) {
		draw();
		host_scene_end(x, y, width, height, key);
		if (joined)
			joined();
		return;
	}
	scene_rect[0] = x, scene_rect[1] = y, scene_rect[2] = width, scene_rect[3] = height;
	scene_key = key;
	behind_draw = draw;
	behind_joined = joined;
	scene_handle = mcw_call(behind_job, 0, 0);
	scene_out = 1;
	host_scene_jobs++;
}

void
host_scene_drop(void)
{
	scene_finish();
	host_settle();
	if (scene_built)
		seglDiscard();
	scene_built = scene_shown = 0;
}

/* A part of a page, in its own place on the screen. */
static void
page_part(int page, int x, int y, int width, int height, int keyed)
{
	GLfixed box[4] = {x * ONE / screen_w, y * ONE / screen_h, (x + width) * ONE / screen_w, (y + height) * ONE / screen_h};
	int texels[4] = {x * 1024 / screen_w, y * 1024 / page_rows, (x + width) * 1024 / screen_w, (y + height) * 1024 / page_rows};

	if (width > 0 && height > 0)
		seglSprite(box, texels, page_texture[page & 1], 0x00ffffff, keyed);
}

/* ---- a picture stretched over the window ---- */

static unsigned char *picture;
static int picture_w, picture_h;
static GLuint picture_texture;

unsigned char *
host_picture(int width, int height, unsigned int *physical)
{
	if (!screen || width <= 0 || height <= 0)
		return NULL;
	scene_finish();
	if (!picture || width * height > picture_w * picture_h) {
		if (!(picture = big_memory(width * height)))
			return NULL;
		if (!picture_texture)
			glGenTextures(1, &picture_texture);
	}
	picture_w = width;
	picture_h = height;
	glBindTexture(GL_TEXTURE_2D, picture_texture);
	seglTexturePointer(picture, width, height, GL_COLOR_INDEX8_EXT);
	if (physical)
		*physical = seglAddress(picture);
	return picture;
}

void
host_show_picture(void)
{
	if (!picture)
		return;
	host_scene_drop();
	whole_window(0, 0);
	glColorKeySE(0);
	quad(picture_texture, 0, 0, screen_w, screen_h, 0);
	seglSwap();
}

/* The pointer's rectangle as four corners, or nothing at all. */
static void
cursor_corners(unsigned int *to, int shown)
{
	int x = pointer_x - cursor_hot_x, y = pointer_y - cursor_hot_y;
	unsigned int x0 = x * ONE / screen_w, y0 = y * ONE / screen_h;
	unsigned int x1 = (x + cursor_w) * ONE / screen_w, y1 = (y + cursor_h) * ONE / screen_h;

	if (!shown || !cursor_w || hw_on)
		x0 = x1 = y0 = y1 = 0;
	to[0] = x0, to[1] = y0, to[4] = x1, to[5] = y0, to[8] = x1, to[9] = y1, to[12] = x0, to[13] = y1;
}

/*
 * A new scene is shown with the page over it: its parts around the map as they are, the part
 * over the map with holes. The commands and the pointer's corners are remembered, so that the
 * same list can be drawn again when only the page or the pointer has changed.
 */
static void
present_scene(int page, int with_cursor)
{
	static const int all[4] = {0, 0, 1024, 1024};
	GLfixed box[4] = {0, 0, 0, 0};
	int x = scene_rect[0], y = scene_rect[1], w = scene_rect[2], h = scene_rect[3];

	glColorKeySE(scene_key);
	page_part(page, 0, 0, screen_w, y, 0);
	page_part(page, 0, y + h, screen_w, screen_h - y - h, 0);
	page_part(page, 0, y, x, h, 0);
	page_part(page, x + w, y, screen_w - x - w, h, 0);
	page_commands[0] = y > 0 || x > 0 || x + w < screen_w || y + h < screen_h ? seglLastCommand() : NULL;
	page_part(page, x, y, w, h, 1);
	page_commands[1] = seglLastCommand();
	glColorKeySE(0);
	seglSprite(box, all, cursor_texture, 0x00ffffff, 1);
	cursor_command = seglLastCommand();
	cursor_vertices = seglLastVertices();
	cursor_corners(cursor_vertices, with_cursor);
	show(0);
	scene_built = 0;
	scene_shown = 1;
	scenes_shown++;
}

/* TDAWN_HOLD_LATE=N, for a test: the Nth scene stays while the next frame is made ready, then the game stops. */
static int
held_late(void)
{
	static int at = -1;

	if (at < 0) {
		const char *text = getenv("TDAWN_HOLD_LATE");

		at = text ? atoi(text) : 0;
	}
	if (!at || scenes_shown < at)
		return 0;
	if (scene_built) {
		scene_owed = 0;
		scene_finish();
		host_settle();
		fprintf(stderr, "tdawn: held late after %d scenes\n", scenes_shown);
		for (;;)
#ifdef __gpuvm__
			__asm__ volatile("wait");	/* pause */
#else
			__asm__ volatile(".word 0x0100000f");	/* pause */
#endif
	}
	return 1;
}

void
host_present(int page, int with_cursor)
{
	if (!screen)
		return;
	pointer_read();
	if (pointer_wanted != (with_cursor != 0)) {
		pointer_wanted = with_cursor != 0;
		pointer_publish();
	}
	if (hw_on)
		pointer_colours();
	if (held_late())
		return;
	if (scene_out) {
		if (scene_owed || !scene_built) {
			scene_finish();		/* a second frame before the first is shown: in their order */
		} else if (!mcw_ready(scene_handle)) {
			/* this scene's frame, and the worker still has it: shown when it has done */
			scene_owed = 1;
			scene_owed_page = page;
			scene_owed_cursor = with_cursor;
			return;
		} else {
			mcw_join(scene_handle);
			scene_out = 0;
			behind_joined_now();
		}
	}
	if (scene_built) {
		present_scene(page, with_cursor);
	} else if (scene_shown) {
		/* the same scene: the page it shows may be the other one now, the pointer elsewhere */
		host_settle();
		page_commands[1][7] = seglAddress(screen + (page & 1) * screen_w * screen_h);
		if (page_commands[0])
			page_commands[0][7] = page_commands[1][7];
		cursor_command[8] = cursor_w;
		cursor_command[9] = cursor_h;
		cursor_corners(cursor_vertices, with_cursor);
		show(1);
	} else {
		whole_window(0, 0);
		glColorKeySE(0);
		page_part(page, 0, 0, screen_w, screen_h, 0);
		if (with_cursor && cursor_w && !hw_on)
			quad(cursor_texture, pointer_x - cursor_hot_x, pointer_y - cursor_hot_y, cursor_w, cursor_h, 1);
		show(0);
	}
	resize_for_test();
}

/*
 * Where the pointer is comes from the machine's own input words (docs/machine/input.md), not from
 * the window system: a move it tells of is a message to read and, before that, a question to
 * ask it, two system calls and two task switches, at every place the game looks for input.
 * With a pointer that never rests that was a third of a mission's frame and more. The window
 * system still says when the pointer comes into the window and leaves it (another window may
 * lie over this one), and a button's own event says where it was pressed.
 */
static int origin_x, origin_y;		/* the window's corner on the display */
static int pointer_inside = 1, pointer_held;

static void
window_origin(void)
{
	GR_WINDOW_INFO info;
	GR_WINDOW_ID id = window;
	int x = 0, y = 0, depth;

	for (depth = 0; depth < 8 && id && id != GR_ROOT_WINDOW_ID; depth++) {
		GrGetWindowInfo(id, &info);
		x += info.x;
		y += info.y;
		id = info.parent;
	}
	origin_x = x;
	origin_y = y;
}

static void
pointer_read(void)
{
	const volatile uint32_t *input = control(REG_INPUT);
	int width, height;

	if (!screen || (!pointer_inside && !pointer_held))
		return;
	seglSize(&width, &height);
	if (width > 0 && height > 0)
		seglPicturePoint((int)input[0] - origin_x, (int)input[1] - origin_y, screen_w, screen_h, &pointer_x, &pointer_y);
}

int
host_buttons(void)
{
	return screen ? (int)control(REG_INPUT)[2] : 0;
}

void
host_pointer(int *x, int *y)
{
	pointer_read();
	*x = pointer_x;
	*y = pointer_y;
}

/* A Nano-X key as the Windows virtual key the game's tables hold, or 0. */
static int
virtual_key(int ch)
{
	static const char shifted[] = ")!@#$%^&*(";
	static const struct { int ch, key; } other[] = {
		{MWKEY_LEFT, 0x25}, {MWKEY_UP, 0x26}, {MWKEY_RIGHT, 0x27}, {MWKEY_DOWN, 0x28},
		{MWKEY_INSERT, 0x2d}, {MWKEY_DELETE, 0x2e}, {MWKEY_HOME, 0x24}, {MWKEY_END, 0x23},
		{MWKEY_PAGEUP, 0x21}, {MWKEY_PAGEDOWN, 0x22}, {MWKEY_KP_ENTER, 0x0d},
		{MWKEY_LSHIFT, 0x10}, {MWKEY_RSHIFT, 0x10}, {MWKEY_LCTRL, 0x11}, {MWKEY_RCTRL, 0x11},
		{MWKEY_LALT, 0x12}, {MWKEY_RALT, 0x12}, {MWKEY_F11, 0x7a}, {MWKEY_F12, 0x7b},
		{';', 0xba}, {'=', 0xbb}, {',', 0xbc}, {'-', 0xbd}, {'.', 0xbe}, {'/', 0xbf}, {'`', 0xc0},
		{'[', 0xdb}, {'\\', 0xdc}, {']', 0xdd}, {'\'', 0xde}, {'\n', 0x0d},
	};
	const char *at;
	unsigned i;

	if (ch >= 'a' && ch <= 'z')
		return ch - 'a' + 'A';
	if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == MWKEY_ENTER || ch == MWKEY_ESCAPE ||
	    ch == MWKEY_BACKSPACE || ch == MWKEY_TAB)
		return ch;
	if (ch >= MWKEY_F1 && ch <= MWKEY_F10)
		return 0x70 + ch - MWKEY_F1;
	if (ch > 0 && ch < 128 && (at = strchr(shifted, ch)) != NULL)
		return '0' + (int)(at - shifted);
	for (i = 0; i < sizeof other / sizeof other[0]; i++)
		if (other[i].ch == ch)
			return other[i].key;
	return 0;
}

/*
 * Asking the server costs two system calls and two task switches. The machine's own input
 * words (pointer, buttons, key events so far; docs/machine/input.md) say when there can be anything:
 * ask when they have moved, for a few calls after, and every 10 ms while an event they told of
 * is owed (a tenth of a second at most: the kernel's tick is later). Otherwise once a second,
 * or four times while the pointer moves outside the picture (it may be coming in or going out).
 */
int
host_event(struct host_event *event)
{
	static uint32_t seen[4], asked_ms, placed_ms, changed_ms, roaming_ms;
	static int ask = 8, log = -1, owed;
	const volatile uint32_t *input;
	GR_EVENT e;
	int i;

	if (!screen)
		return 0;
	if (scene_out && mcw_ready(scene_handle))
		scene_finish();		/* the worker has made the scene: its frame is shown */
	frames_pump(1);
	if (log < 0)
		log = getenv("TDAWN_INPUT_LOG") != NULL;	/* say what arrives, for tests */
	input = control(REG_INPUT);
	for (i = 2; i < 4; i++)		/* the buttons, and the count of keys */
		if (seen[i] != input[i]) {
			owed += i == 3 ? (int)(input[i] - seen[i]) : 1;
			seen[i] = input[i];
			ask = 8;
			changed_ms = host_ms();
		}
	if (!ask && owed > 0) {
		if (host_ms() - changed_ms >= 100)
			owed = 0;
		else if (host_ms() - asked_ms >= 10)
			ask = 1;
	}
	if (seen[0] != input[0] || seen[1] != input[1]) {
		seen[0] = input[0], seen[1] = input[1];
		pointer_read();
		if (!pointer_inside || pointer_x < 0 || pointer_y < 0 || pointer_x >= screen_w || pointer_y >= screen_h)
			roaming_ms = host_ms();
	}
	if (!ask && host_ms() - asked_ms >= (host_ms() - roaming_ms < 500 ? 250u : 1000u)) {
		ask = 1;
		if (host_ms() - placed_ms >= 3000) {
			/* (a window's frame can be moved without a word to the window) */
			placed_ms = host_ms();
			scene_finish();
			window_origin();
		}
	}
	if (ask)
		scene_finish();		/* an event may be the window's: the GPU's library is asked about it */
	while (ask) {
		GrCheckNextEvent(&e);
		switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			ask--;
			asked_ms = host_ms();
			return 0;
		case GR_EVENT_TYPE_MOUSE_ENTER:
			pointer_inside = 1;
			window_origin();
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			pointer_inside = 0;
			break;
		case GR_EVENT_TYPE_UPDATE:
			/* the picture is stretched over the window, whatever size it is given */
			if (e.update.utype == GR_UPDATE_SIZE) {
				seglWindowChanged();
				pointer_publish();
				if (log)
					fprintf(stderr, "tdinput: the window is %dx%d now\n", e.update.width, e.update.height);
			}
			window_origin();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_BUTTON_UP:
			owed -= owed > 0;
			seglPicturePoint(e.button.x, e.button.y, screen_w, screen_h, &pointer_x, &pointer_y);
			if (!(e.button.changebuttons & (GR_BUTTON_L | GR_BUTTON_R)))
				break;
			event->type = HOST_BUTTON;
			event->key = e.button.changebuttons & GR_BUTTON_L ? 0x01 : 0x02;
			event->down = e.type == GR_EVENT_TYPE_BUTTON_DOWN;
			pointer_held = event->down ? pointer_held | event->key : pointer_held & ~event->key;			event->x = pointer_x;
			event->y = pointer_y;
			if (log) {
				int width, height;

				seglSize(&width, &height);
				fprintf(stderr, "tdinput: button %d %s at %d,%d (%d,%d in the %dx%d window), %u ms after its word\n", event->key,
					event->down ? "down" : "up", pointer_x, pointer_y, e.button.x, e.button.y, width, height, host_ms() - changed_ms);
			}
			return 1;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			owed -= owed > 0;
			event->key = virtual_key(e.keystroke.ch);
			if (!event->key)
				break;
			event->type = HOST_KEY;
			event->down = e.type == GR_EVENT_TYPE_KEY_DOWN;
			if (log)
				fprintf(stderr, "tdinput: key %d %s\n", event->key, event->down ? "down" : "up");
			return 1;
		case GR_EVENT_TYPE_CLOSE_REQ:
			event->type = HOST_QUIT;
			return 1;
		}
	}
	return 0;
}

#ifdef SHADEREMU_PGO
/*
 * The compiler's counters, written to the console when the program ends (a build with
 * TDAWN_PROFILE=count; the toolchain's own libgcov is of another float ABI). Branches, and
 * for each indirect call and such the values it met; linux/tdawn/profile.py reads the lines.
 */
struct gcov_ctr { unsigned int num; long long *values; };
struct gcov_fn { const void *key; unsigned int ident, lineno_checksum, cfg_checksum; struct gcov_ctr ctrs[1]; };
struct gcov_info {
	unsigned int version; struct gcov_info *next; unsigned int stamp, checksum; const char *filename;
	void (*merge[8])(long long *, unsigned int); unsigned int n_functions; const struct gcov_fn *const *functions;
};
struct gcov_kvp { long long value, count; struct gcov_kvp *next; };
static struct gcov_info *gcov_list;
__thread struct { void *callee; long long *counters; } __gcov_indirect_call;
long long __gcov_time_profiler_counter;

void __gcov_init(struct gcov_info *info) { info->next = gcov_list, gcov_list = info; }
void __gcov_merge_add(long long *counters, unsigned int count) { (void)counters, (void)count; }
void __gcov_merge_topn(long long *counters, unsigned int count) { (void)counters, (void)count; }
void __gcov_merge_time_profile(long long *counters, unsigned int count) { (void)counters, (void)count; }

/* A value met: counted among the 32 kept for its place, as libgcov keeps them. */
static void
gcov_met(long long *counters, long long value)
{
	struct gcov_kvp *n, *last = NULL, *least = NULL;

	counters[0]++;
	for (n = (struct gcov_kvp *)(uintptr_t)counters[2]; n; last = n, n = n->next) {
		if (n->value == value) {
			n->count++;
			return;
		}
		if (!least || n->count < least->count)
			least = n;
	}
	if (counters[1] == 32) {
		if (--least->count < 1)
			least->value = value, least->count = 1;
		return;
	}
	if (!(n = malloc(sizeof *n)))
		return;
	n->value = value, n->count = 1, n->next = NULL;
	if (last)
		last->next = n;
	else
		counters[2] = (uintptr_t)n;
	counters[1]++;
}

void __gcov_topn_values_profiler(long long *counters, long long value) { gcov_met(counters, value); }
void __gcov_merge_ior(long long *counters, unsigned int count) { (void)counters, (void)count; }
void __gcov_ior_profiler(long long *counters, long long value) { *counters |= value; }
void __gcov_average_profiler(long long *counters, long long value) { counters[0] += value, counters[1]++; }
void __gcov_pow2_profiler(long long *counters, long long value) { counters[value && !(value & (value - 1))]++; }

void
__gcov_interval_profiler(long long *counters, long long value, int start, unsigned int steps)
{
	long long delta = value - start;

	counters[delta < 0 ? steps + 1 : delta >= steps ? steps : delta]++;
}

void
__gcov_indirect_call_profiler_v4(long long value, void *function)
{
	if (function == __gcov_indirect_call.callee)
		gcov_met(__gcov_indirect_call.counters, value);
	__gcov_indirect_call.callee = NULL;
}

void
__gcov_exit(void)
{
	static int done;
	struct gcov_info *i;
	unsigned int f, c, kind, kinds;

	if (done)
		return;
	done = 1;
	for (i = gcov_list; i; i = i->next) {
		for (kinds = 0, kind = 0; kind < 8; kind++)
			kinds |= (i->merge[kind] != NULL) << kind;
		fprintf(stderr, "GC F %s %x %x %x %x\n", i->filename, i->version, i->stamp, i->checksum, kinds);
		for (f = 0; f < i->n_functions; f++) {
			const struct gcov_fn *fn = i->functions[f];
			const struct gcov_ctr *ctr;

			if (!fn || fn->key != i)
				continue;
			fprintf(stderr, "GC f %x %x %x\n", fn->ident, fn->lineno_checksum, fn->cfg_checksum);
			for (ctr = fn->ctrs, kind = 0; kind < 8; kind++) {
				if (!i->merge[kind])
					continue;
				fprintf(stderr, "GC k %u %u\n", kind, ctr->num);
				for (c = 0; c < ctr->num; c++) {
					if (kind == 3 || kind == 4) {
						/* (a place is three words: how often, how many values, the values) */
						const struct gcov_kvp *n = (const struct gcov_kvp *)(uintptr_t)ctr->values[c + 2];

						if (ctr->values[c])
							fprintf(stderr, "GC t %x %llx\n", c / 3, (unsigned long long)ctr->values[c]);
						for (; n; n = n->next)
							fprintf(stderr, "GC v %llx %llx\n", (unsigned long long)n->value, (unsigned long long)n->count);
						c += 2;
					} else if (ctr->values[c])
						fprintf(stderr, "GC c %x %llx\n", c, (unsigned long long)ctr->values[c]);
				}
				ctr++;
			}
		}
	}
	fprintf(stderr, "GC end\n");
}
#endif
