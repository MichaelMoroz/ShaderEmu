/*
 * nxlife: Conway's Game of Life on a grid that wraps at its edges, a generation at a time on the
 * worker cores: each takes a band of rows, works out its cells' next state from the last one and
 * writes their pixels straight into the window's buffer (as nxmon does: docs/system/nanox.md, "A
 * window's buffer written by its program"). A living cell is coloured by its age. The left button
 * paints cells, the right one clears them; a few patterns to start from.
 *
 * Keys: Space runs and pauses, N (or S) makes one generation, R fills the grid at random, C clears
 * it, G puts a glider gun, + and - set the speed, F10 the menus. A controller (ui_pad.h): A runs
 * and pauses, B makes one generation, Y fills at random.
 *
 *   NXLIFE_SEED=N      the random fill's (a test's)
 *   NXLIFE_GENS=N      N generations at once from a random fill, then a line with the population and
 *                      a sum of the grid: "nxlife: N generations, population P, sum X, W workers, G/s"
 *   NXLIFE_CORES=0     no workers: the first core makes every generation (the same sum)
 *   NXLIFE_PRINT=1     the population every second while it runs
 * The workers are lent (mcw_lend): a program that asks for them has them, and this one goes on alone.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <ctype.h>
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_pad.h"
#include "mcwx.h"

#define MENU_H 20
#define STATUS_H 22
#define MARGIN 8		/* (at least 4: no two workers' rows share a texel of the window's buffer) */
#define WORKERS 63
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
	GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_CLOSE_REQ)

/* the grid: a cell's byte is 0 dead, else its age (1 born this generation, up to 255) */
static int COLS, ROWS, STRIDE, CELL;
static unsigned char *grid[2];	/* this generation and the next, rows of STRIDE bytes (16-byte aligned) */
static int now_grid;
static int *left_of, *right_of;	/* a column's neighbours, the grid wrapping */
static unsigned long long generation;
static unsigned population;
static int running, speed = 3;	/* speed: 0 slowest (2 a second) to 4 (as fast as it can) */
static const int speed_ms[5] = { 500, 200, 80, 30, 0 };
static unsigned seed = 1;

/* the window and its buffer */
static GR_WINDOW_ID window;
static int width, height, FX, FY, FW, FH;	/* the field: where the grid is drawn */
static int menu_x[3];
static uint32_t *surface;	/* the window's first pixel in GPU memory, or NULL: drawn through the window system */
static int surface_row;
static int server_drew;		/* the window system drew since the buffer was last written: settle first */
static int workers, no_workers, print;
static int painting;		/* 1 the left button paints, 2 the right one clears */
static struct ui_pad pad;

static const char *const menu_names[] = { "Game", "Help" };
enum { C_RUN = 1, C_STEP, C_RANDOM, C_CLEAR, C_GUN, C_ACORN, C_RPENT, C_EXIT, C_KEYS, C_ABOUT, C_SPEED = 100 };
static struct ui_menu_item speed_items[] = {
	{ "Slowest", -1, C_SPEED + 0 }, { "Slow", -1, C_SPEED + 1 }, { "Normal", -1, C_SPEED + 2 }, { "Fast", -1, C_SPEED + 3 },
	{ "As fast as it can", -1, C_SPEED + 4 },
};
static struct ui_menu_item game_items[] = {
	{ "Run / pause   Space", -1, C_RUN }, { "One generation   N", -1, C_STEP }, { "Speed", -1, 0, speed_items, 5 },
	{ "Random   R", -1, C_RANDOM, NULL, 0, UI_MENU_LINE }, { "Clear   C", -1, C_CLEAR }, { "Glider gun   G", -1, C_GUN },
	{ "Acorn", -1, C_ACORN }, { "R-pentomino", -1, C_RPENT }, { "Exit", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item help_items[] = { { "Keys", -1, C_KEYS }, { "About Life", -1, C_ABOUT } };

static long long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static unsigned
roll(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 16;
}

/* A living cell's colour by its age: newborn pale yellow, then green, cyan and a deep blue. */
static uint32_t
colour_of(int age)
{
	static const unsigned char steps[5][3] = { { 255, 245, 150 }, { 130, 230, 110 }, { 60, 200, 210 }, { 50, 120, 230 }, { 70, 70, 190 } };
	int k;

	if (!age)
		return 0x10141c;
	k = age < 2 ? 0 : age < 6 ? 1 : age < 20 ? 2 : age < 60 ? 3 : 4;
	return (uint32_t)steps[k][0] << 16 | (uint32_t)steps[k][1] << 8 | steps[k][2];
}

/* ---- a generation: rows from..to-1 of the next grid from this one, and their pixels ---- */

struct step { const unsigned char *from; unsigned char *to; uint32_t *pixels; int pixel_row; unsigned live[1]; };
static struct step job __attribute__((aligned(16)));

static void
paint_row(uint32_t *pixels, int pixel_row, const unsigned char *row, int r)
{
	uint32_t *p = pixels + (FY + r * CELL) * pixel_row + FX;
	int c, k, y;

	for (c = 0; c < COLS; c++) {
		uint32_t colour = colour_of(row[c]);

		for (k = 0; k < CELL - (CELL > 2); k++)
			p[c * CELL + k] = colour;
		if (CELL > 2)
			p[c * CELL + CELL - 1] = 0x0c0f16;	/* (a line between cells) */
	}
	for (y = 1; y < CELL; y++)
		memcpy(p + y * pixel_row, p, (size_t)(COLS * CELL) * 4);
	if (CELL > 2)
		for (c = 0; c < COLS * CELL; c++)
			p[(CELL - 1) * pixel_row + c] = 0x0c0f16;
}

static void
step_rows(void *ctx, uint32_t from, uint32_t to)
{
	const struct step *s = ctx;
	uint32_t r;
	int c;

	for (r = from; r < to; r++) {
		const unsigned char *up = s->from + ((r + ROWS - 1) % ROWS) * STRIDE, *mid = s->from + r * STRIDE,
			*down = s->from + ((r + 1) % ROWS) * STRIDE;
		unsigned char *out = s->to + r * STRIDE;

		for (c = 0; c < COLS; c++) {
			int l = left_of[c], rr = right_of[c];
			int n = !!up[l] + !!up[c] + !!up[rr] + !!mid[l] + !!mid[rr] + !!down[l] + !!down[c] + !!down[rr];

			out[c] = mid[c] ? (n == 2 || n == 3 ? (mid[c] < 255 ? mid[c] + 1 : 255) : 0) : n == 3;
		}
		if (s->pixels)
			paint_row(s->pixels, s->pixel_row, out, (int)r);
	}
}

/* The window system's drawing done before the buffer is written (a pixel read is answered then). */
static void
settle(void)
{
	GR_PIXELVAL pixel;

	if (!server_drew)
		return;
	GrReadArea(window, 0, 0, 1, 1, &pixel);
	server_drew = 0;
}

static void
count_population(void)
{
	const unsigned char *g = grid[now_grid];
	int r, c;

	population = 0;
	for (r = 0; r < ROWS; r++)
		for (c = 0; c < COLS; c++)
			population += g[r * STRIDE + c] != 0;
}

/* Rows of the field drawn through the window system, where the buffer cannot be written. */
static void
draw_field_server(void)
{
	static uint32_t *line;
	int r;

	if (!line)
		line = malloc((size_t)FW * CELL * 4);
	for (r = 0; r < ROWS && line; r++) {
		const unsigned char *row = grid[now_grid] + r * STRIDE;
		int c, k, y;

		for (c = 0; c < COLS; c++)
			for (k = 0; k < CELL; k++)
				line[c * CELL + k] = CELL > 2 && k == CELL - 1 ? 0x0c0f16 : colour_of(row[c]);
		for (y = 1; y < CELL; y++)
			memcpy(line + y * FW, line, (size_t)FW * 4);
		if (CELL > 2)
			for (c = 0; c < FW; c++)
				line[(CELL - 1) * FW + c] = 0x0c0f16;
		GrArea(window, ui_gc, FX, FY + r * CELL, FW, CELL, line, MWPF_RGB);
	}
	server_drew = 1;
}

/* The whole field drawn again from the grid as it is. */
static void
draw_field(void)
{
	int r;

	if (!surface) {
		draw_field_server();
		return;
	}
	settle();
	for (r = 0; r < ROWS; r++)
		paint_row(surface, surface_row, grid[now_grid] + r * STRIDE, r);
}

static void
next_generation(void)
{
	mcw_for_opts o = { 0 };

	job.from = grid[now_grid], job.to = grid[!now_grid];
	job.pixels = surface, job.pixel_row = surface_row;
	if (surface)
		settle();
	if (workers > 0 && mcw_asked()) {	/* (another program wants them: it has them) */
		mcw_for_end();
		mcw_close();
		workers = 0;
	}
	o.grain = 2;
	o.out_bytes = (uint32_t)STRIDE;
	if (workers > 0)
		mcw_for_opt((uint32_t)ROWS, step_rows, &job, &o);
	else
		step_rows(&job, 0, (uint32_t)ROWS);
	now_grid = !now_grid;
	generation++;
	if (!surface)
		draw_field_server();
}

/* ---- the grid's contents ---- */

static void
clear_grid(void)
{
	memset(grid[now_grid], 0, (size_t)STRIDE * ROWS);
	generation = 0;
}

static void
random_fill(void)
{
	int r, c;

	clear_grid();
	for (r = 0; r < ROWS; r++)
		for (c = 0; c < COLS; c++)
			grid[now_grid][r * STRIDE + c] = roll() % 100 < 30 ? 1 : 0;
}

/* a pattern of rows ('O' living), its top left at (x, y) of the grid */
static void
put(const char *const *rows, int count, int x, int y)
{
	int r, c;

	for (r = 0; r < count; r++)
		for (c = 0; rows[r][c]; c++)
			if (rows[r][c] == 'O')
				grid[now_grid][((y + r) % ROWS) * STRIDE + (x + c) % COLS] = 1;
}

static void
pattern(int which)
{
	static const char *const gun[] = {
		"........................O...........",
		"......................O.O...........",
		"............OO......OO............OO",
		"...........O...O....OO............OO",
		"OO........O.....O...OO..............",
		"OO........O...O.OO....O.O...........",
		"..........O.....O.......O...........",
		"...........O...O....................",
		"............OO......................",
	};
	static const char *const acorn[] = { ".O.....", "...O...", "OO..OOO" };
	static const char *const rpent[] = { ".OO", "OO.", ".O." };

	clear_grid();
	if (which == C_GUN)
		put(gun, 9, 4, 4);
	else if (which == C_ACORN)
		put(acorn, 3, COLS / 2 - 3, ROWS / 2 - 1);
	else
		put(rpent, 3, COLS / 2 - 1, ROWS / 2 - 1);
}

static unsigned
grid_sum(void)
{
	unsigned h = 2166136261u;
	int r, c;

	for (r = 0; r < ROWS; r++)
		for (c = 0; c < COLS; c++)
			h = (h ^ (grid[now_grid][r * STRIDE + c] != 0)) * 16777619u;
	return h;
}

/* ---- drawing what is not the field ---- */

static void
draw_status(void)
{
	char text[96];

	count_population();
	snprintf(text, sizeof text, "Generation %llu     Population %u     %s     %d workers", generation, population,
		 running ? "running" : "paused (Space runs)", workers > 0 ? workers : 0);
	ui_fill(window, 0, height - STATUS_H, width, STATUS_H, UI_FACE);
	ui_fill(window, 0, height - STATUS_H, width, 1, UI_SHADOW);
	ui_text(window, 8, height - STATUS_H + 4, text, -1, BLACK, 0);
	server_drew = 1;
}

static void
draw_all(void)
{
	int i;

	ui_fill(window, 0, 0, width, FY, UI_FACE);
	ui_fill(window, 0, FY, FX, FH, UI_FACE);
	ui_fill(window, FX + FW, FY, width - FX - FW, FH, UI_FACE);
	ui_fill(window, 0, FY + FH, width, height - FY - FH, UI_FACE);
	for (i = 0; i < 2; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
	draw_status();
	draw_field();
}

/* Sizes from the screen's: cells of 3 pixels over as much of it as a window leaves. */
static void
layout(void)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	int x;

	CELL = 3;
	COLS = (screen->cols - 40 - 2 * MARGIN) / CELL;
	ROWS = (screen->rows - UI_MENU_BAR - 34 - MENU_H - STATUS_H - 2 * MARGIN) / CELL;
	COLS = COLS > 400 ? 400 : COLS < 40 ? 40 : COLS;
	ROWS = ROWS > 300 ? 300 : ROWS < 30 ? 30 : ROWS;
	STRIDE = (COLS + 15) & ~15;
	FW = COLS * CELL, FH = ROWS * CELL;
	FX = MARGIN, FY = MENU_H + MARGIN;
	width = FW + 2 * MARGIN, height = FY + FH + MARGIN + STATUS_H;
	x = 2;
	menu_x[0] = x;
	x += ui_width(&ui_var_widths, ui_var, menu_names[0], -1) + 16;
	menu_x[1] = x;
	menu_x[2] = x + ui_width(&ui_var_widths, ui_var, menu_names[1], -1) + 16;
}

static void
make_grid(void)
{
	int k, c;

	for (k = 0; k < 2; k++) {
		grid[k] = aligned_alloc(16, (size_t)STRIDE * ROWS);
		memset(grid[k], 0, (size_t)STRIDE * ROWS);
	}
	left_of = malloc(sizeof *left_of * (size_t)COLS);
	right_of = malloc(sizeof *right_of * (size_t)COLS);
	for (c = 0; c < COLS; c++)
		left_of[c] = (c + COLS - 1) % COLS, right_of[c] = (c + 1) % COLS;
}

/* The window's buffer mapped, and the workers lent: once the window is there. */
static void
locate(void)
{
	GR_WINDOW_INFO info;
	uint32_t offset, size;
	void *memory;
	int gpu = open("/dev/gpu", O_RDWR);

	if (!no_workers) {
		mcw_lend();
		workers = mcw_open(WORKERS);
		if (workers < 0)
			workers = 0;
	}
	if (gpu >= 0) {
		GrGetWindowInfo(window, &info);
		if (info.surface_address) {
			surface_row = info.surface_row ? (int)info.surface_row : (int)info.width;
			offset = info.surface_address - 0x80000000u;
			size = (offset & 4095) + (uint32_t)surface_row * (uint32_t)height * 4;
			memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, offset & ~4095u);
			if (memory != MAP_FAILED) {
				surface = (uint32_t *)((char *)memory + (offset & 4095));
				mcw_touch(memory, size);	/* (a worker's store to a page no core has touched would stop it) */
			}
		}
		close(gpu);
	}
	mcw_touch(grid[0], (unsigned)(STRIDE * ROWS));
	mcw_touch(grid[1], (unsigned)(STRIDE * ROWS));
	server_drew = 1;
}

/* ---- input ---- */

static void
paint_at(int x, int y)
{
	int c = (x - FX) / CELL, r = (y - FY) / CELL;
	unsigned char *cell;

	if (x < FX || y < FY || c >= COLS || r >= ROWS)
		return;
	cell = &grid[now_grid][r * STRIDE + c];
	if ((painting == 1) == (*cell != 0))
		return;
	*cell = painting == 1 ? 1 : 0;
	if (surface) {
		settle();
		paint_row(surface, surface_row, grid[now_grid] + r * STRIDE, r);
	} else
		draw_field_server();
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window) {
		GrClose();
		exit(0);
	}
}

static void
command(int id)
{
	static const char *const ok[] = { "OK" };

	if (id >= C_SPEED && id < C_SPEED + 5) {
		speed = id - C_SPEED;
		reg_set_int("nxlife.speed", speed);
		return;
	}
	switch (id) {
	case C_RUN: running = !running; break;
	case C_STEP: running = 0; next_generation(); break;
	case C_RANDOM: random_fill(); break;
	case C_CLEAR: running = 0; clear_grid(); break;
	case C_GUN: case C_ACORN: case C_RPENT: pattern(id); break;
	case C_EXIT: GrClose(); exit(0);
	case C_KEYS:
		ui_ask("Keys", "Space: run or pause     N: one generation\nR: random     C: clear     G: a glider gun\n"
			"+ and -: faster and slower     F10: the menus\nLeft button: paint cells     Right button: clear them", ok, 1, others);
		GrSetFocus(window);
		break;
	case C_ABOUT:
		ui_ask("About Life", "Conway's Game of Life: a cell with two or three\nliving neighbours lives on, an empty one with three\n"
			"is born. Each generation is made on the worker cores.", ok, 1, others);
		GrSetFocus(window);
		break;
	}
	draw_all();
}

static void
open_menu(int which, int keyboard)
{
	int x, y, i;

	for (i = 0; i < 5; i++)
		speed_items[i].flags = i == speed ? UI_MENU_CHECK : 0;
	ui_window_origin(window, &x, &y);
	if (which)
		ui_menu_open(help_items, 2, x + menu_x[1], y + MENU_H, 0, keyboard);
	else
		ui_menu_open(game_items, 9, x + menu_x[0], y + MENU_H, 0, keyboard);
}

static void
key_down(int ch)
{
	if (ch < 128)
		ch = tolower(ch);
	switch (ch) {
	case ' ': command(C_RUN); break;
	case 'n': case 's': command(C_STEP); break;
	case 'r': command(C_RANDOM); break;
	case 'c': command(C_CLEAR); break;
	case 'g': command(C_GUN); break;
	case '+': case '=': if (speed < 4) command(C_SPEED + speed + 1); draw_status(); break;
	case '-': if (speed > 0) command(C_SPEED + speed - 1); draw_status(); break;
	case MWKEY_F10: open_menu(0, 1); break;
	}
}

/* NXLIFE_GENS: generations at once from a random fill, and what they came to */
static void
test_run(int gens)
{
	long long began;
	int g;

	random_fill();
	began = now_ms();
	for (g = 0; g < gens; g++)
		next_generation();
	began = now_ms() - began;
	count_population();
	printf("nxlife: %d generations, population %u, sum %08x, %d workers, %.1f/s\n", gens, population, grid_sum(), workers,
	       began > 0 ? gens * 1000.0 / (double)began : 0.0);
	fflush(stdout);
}

int
main(void)
{
	GR_EVENT event = { 0 };
	const char *a;
	long long next_at = 0, said_at = 0, status_at = 0;
	int id;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	seed = getenv("NXLIFE_SEED") ? (unsigned)atoi(getenv("NXLIFE_SEED")) : (unsigned)time(NULL);
	no_workers = (a = getenv("NXLIFE_CORES")) && atoi(a) == 0;
	print = getenv("NXLIFE_PRINT") != NULL;
	speed = reg_int("nxlife.speed", 3);
	speed = speed < 0 || speed > 4 ? 3 : speed;
	layout();
	make_grid();
	window = GrNewWindowEx(UI_FIXED, "Life", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(window, EVENTS);
	ui_pad_ask(window);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	locate();
	if ((a = getenv("NXLIFE_GENS")) && atoi(a) > 0)
		test_run(atoi(a));
	else {
		pattern(C_GUN);
		running = 1;
	}
	draw_all();
	for (;;) {
		long long now = now_ms();
		int wait = -1;

		ui_pad_poll(&pad);
		if (pad.down & INPUT_JUMP)
			command(C_RUN);
		if (pad.down & INPUT_RIGHT_UPPER)
			command(C_STEP);
		if (pad.down & INPUT_LEFT_UPPER)
			command(C_RANDOM);
		if (running && now >= next_at) {
			next_generation();
			next_at = now + speed_ms[speed];
			if (now >= status_at)	/* (the window system asked to draw four times a second, not every generation) */
				draw_status(), status_at = now + 250;
			if (print && now >= said_at) {
				printf("nxlife: generation %llu, population %u\n", generation, population);
				fflush(stdout);
				said_at = now + 1000;
			}
		}
		if (running)
			wait = next_at > now ? (int)(next_at - now) : 1;
		wait = ui_pad_wait(&pad, wait);
		GrFlush();
		if (wait > 0 || !running) {
			GrGetNextEventTimeout(&event, wait < 0 ? 0 : wait);
		} else if (!GrPeekEvent(&event))
			continue;
		else
			GrGetNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_TIMEOUT || event.type == GR_EVENT_TYPE_NONE)
			continue;
		if (ui_restyle()) {
			ui_menu_restyle();
			draw_all();
		}
		ui_origin_note(&event);
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id);
			continue;
		case UI_MENU_CLOSED:
			GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window)
				draw_all();
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key_down(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid != window || ui_wheel(&event))
				break;
			if (event.button.y < MENU_H) {
				int i;

				for (i = 0; i < 2; i++)
					if (event.button.x >= menu_x[i] && event.button.x < menu_x[i + 1])
						open_menu(i, 0);
				break;
			}
			painting = event.button.changebuttons & GR_BUTTON_R ? 2 : 1;
			paint_at(event.button.x, event.button.y);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (painting && event.mouse.wid == window)
				paint_at(event.mouse.x, event.mouse.y);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (painting) {
				painting = 0;
				draw_status();
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (workers > 0) {
				mcw_for_end();
				mcw_close();
			}
			GrClose();
			return 0;
		}
	}
}
