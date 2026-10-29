/* nxphys: rigid boxes, every worker core moving its share of them (docs/apps/physics.md).
 * Keys, and the bar's buttons: 1 2 3 a wall, towers, a heap; space the heavy box thrown; p stop;
 * c each box in its core's colour; 9 / 0 all the workers, none; r again; w a s d walk; q leave.
 * The pointer and a holodeck sitter's hands carry boxes and throw them ("The controls" there). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES/segl.h>
#include "ui.h"
#include "mcwx.h"
#include "physics.h"

#define MAX_BODIES 2048
#define ROOM 12.0f		/* the walls are this far from the middle */
#define ROOM_HIGH 8		/* cells of the grid upwards */
#define TILES 12		/* the floor's squares each way */
#define FLOOR_QUADS (TILES * TILES)
#define SHADES 64
#define COLOURS 16

/* A share's own two texels: the substep it is to make next, the picture it last made, what had
 * no room, how many shares a picture's share has found at its substep, and (PHYS_COUNT) its
 * dearest substep without a look. */
struct share { volatile uint32_t phase, shown; uint32_t trouble, ready; uint32_t most, pad2[3]; };
/* PHYS_COUNT's counters (mcwx.h): the instructions of all grids, substeps with a look and without */
enum { COUNT_GRID, COUNT_LOOK, COUNT_PLAIN };

/* Core 0's: the workers only read it. */
static struct {
	struct phys w;
	struct share *shares;
	uint32_t *quads[2];	/* a picture's quads, for the GPU: one drawn while the other is made */
	uint8_t *colours;
	/* the shares: `nbody` of them have the bodies (body i is share i mod nbody's), and the
	 * `nservice` after those the grid and the pictures; with none of those every share has
	 * its bodies, its cells of the grid and its bodies' part of a picture */
	int nshares, nbody, nservice;
	uint32_t *costs;	/* PHYS_COUNT: what each substep cost each share, `cost_row` words a share */
	uint32_t cost_row;
	uint32_t base;		/* substeps made before the shares' first */
	volatile uint32_t limit, show_step, show;	/* picture `show` is of substep show_step, or the first after */
	volatile int stop, owners;
	int count;
	int ahead;		/* PHYS_ORDER=ahead: the look for a pair of substeps is made after the pair before, not just before it */
	int spin;		/* PHYS_SPIN: a share with nothing to do turns in place, as it did */
} sim __attribute__((aligned(16)));
static char sim_end[16] __attribute__((aligned(16), used));	/* (nothing else in sim's last 16 bytes) */

static inline uint32_t
cycles(void)
{
	uint32_t count;

	__asm__ volatile("rdcycle %0" : "=r"(count));
	return count;
}

/* Share s's part of the picture being made: boxes as quads. */
static void
show_share(int s, int from, int into)
{
	uint32_t *quads = sim.quads[into] + 16 * FLOOR_QUADS;
	int owners = sim.owners, every = sim.nservice ? sim.nservice : sim.nshares, i;

	for (i = sim.nservice ? s - sim.nbody : s; i < sim.w.bodies; i += every)
		phys_quads(&sim.w, i, from, owners ? (uint32_t)(i % sim.nbody) % COLOURS : sim.colours[i], quads + PHYS_QUAD_WORDS * i);
}

/* The first share from `at` on that has not made the substep before `phase`, or n. (The share
 * that is last to make a substep looks at every other each time: four a turn.) */
static inline int
behind(int at, int n, uint32_t phase)
{
	const struct share *shares = sim.shares;

	for (; at + 4 <= n; at += 4) {
		if (shares[at].phase < phase) return at;
		if (shares[at + 1].phase < phase) return at + 1;
		if (shares[at + 2].phase < phase) return at + 2;
		if (shares[at + 3].phase < phase) return at + 3;
	}
	for (; at < n; at++)
		if (shares[at].phase < phase)
			break;
	return at;
}

/* Whether the shares of every body share s's bodies have a contact with have made the substep
 * before `phase`: a substep without a look reads no other body. */
static int
partners_ready(int s, uint32_t phase)
{
	const struct share *shares = sim.shares;
	uint32_t n = (uint32_t)sim.nbody, m, pairs;
	int i;

	for (i = s; i < sim.w.bodies; i += (int)n) {
		const struct manifold *list = &sim.w.manifolds[i * PHYS_MANIFOLDS];

		for (pairs = sim.w.info[i].manifolds, m = 0; m < pairs; m++)
			if (shares[list[m].partner % n].phase < phase)
				return 0;
	}
	return 1;
}

/* Share s's part of the picture asked for, or its next substep, now that it may. (Out of line:
 * a share that waits only asks share_advance(), and that is short.) */
static __attribute__((noinline)) int
share_work(int s, int *seen, uint32_t phase, int wanted, uint32_t show)
{
	struct share *st = &sim.shares[s];
	uint32_t kind = phase % PHYS_EVERY;
	int from = (int)((sim.base + phase) % PHYS_BUFFERS), trouble = 0, bodied = s < sim.nbody, serves = !sim.nservice || !bodied;
	/* (a look's number is that of the pair of substeps it is for. Every PHYS_EVERY-th substep
	 * looks for contacts first; or, looking ahead, the substep before does when it has said
	 * its boxes are made: the longest look then holds no share up. docs/apps/physics.md) */
	int turn = (int)((sim.base + phase) / PHYS_EVERY) + 1, ahead = sim.ahead;
	uint32_t began = 0, took = 0;

	if (wanted) {
		/* (the bodies are all at this substep, and none goes on until this share does) */
		show_share(s, from, (int)(show & 1));
		st->shown = show;
		return 1;
	}
	if (phase >= sim.limit)
		return 0;
	if (sim.count)
		began = cycles();
	/* (looking ahead, the first substep of all has its look first too: there was none before it) */
	if (bodied)
		trouble = phys_step(&sim.w, s, sim.nbody, from, (ahead ? phase == 0 : kind == 0) ? turn : 0, turn);
	if (sim.count) {
		took = cycles() - began;
		mcw_add(kind == 0 && !ahead ? COUNT_LOOK : COUNT_PLAIN, took);
		if ((kind || ahead) && took > st->most)
			st->most = took;
		began = cycles();
	}
	/* the grid the next look reads: made a substep before that look */
	if (kind == (ahead ? 0 : PHYS_EVERY - 1) && serves)
		trouble += phys_grid(&sim.w, s, from, ahead ? turn + 1 : (int)((sim.base + phase + 1) / PHYS_EVERY) + 1);
	if (sim.count) {
		mcw_add(COUNT_GRID, cycles() - began);
		took += cycles() - began;
	}
	*seen = 0;
	st->phase = phase + 1;
	if (ahead && kind == PHYS_EVERY - 1 && bodied) {
		/* the next pair's look, where the bodies were before this substep: nobody waits for it */
		if (sim.count)
			began = cycles();
		trouble += phys_look(&sim.w, s, sim.nbody, from, (int)((sim.base + phase + 1) / PHYS_EVERY) + 1);
		if (sim.count) {
			mcw_add(COUNT_LOOK, cycles() - began);
			took += cycles() - began;
		}
	}
	if (sim.costs && phase < sim.cost_row)
		sim.costs[(uint32_t)s * sim.cost_row + phase] = took;
	if (trouble)
		st->trouble += (uint32_t)trouble;
	return 1;
}

/* Whether a body of share s sleeps at `phase`: it looks round itself for fast bodies, and
 * needs every share to have made the substep before. */
static int
share_sleeps(int s, uint32_t phase)
{
	const struct body_state *st = sim.w.states[(sim.base + phase) % PHYS_BUFFERS];
	int i;

	for (i = s; i < sim.w.bodies; i += sim.nbody)
		if (st[i].quiet >= PHYS_SLEEP_AFTER)
			return 1;
	return 0;
}

/* What share s can do now, done, once every share has made the substep before (`seen` is how
 * far that was found so, the number of shares when it is): its part of the picture asked for,
 * or its next substep. A share of the grid and the pictures has no bodies. */
static inline int
share_advance(int s, int *seen)
{
	uint32_t phase = sim.shares[s].phase, show = sim.show;
	int n = sim.nshares, serves = !sim.nservice || s >= sim.nbody;
	int wanted = serves && sim.shares[s].shown != show && phase >= sim.show_step;

	if (sim.nservice && serves && sim.shares[s].shown != show) {
		/* a picture's share makes substep show_step's once every body is there, beside the
		 * substeps: it holds at the one before the look that would write that state again */
		struct share *st = &sim.shares[s];
		uint32_t at = sim.show_step;

		wanted = 0;
		if ((int)st->ready < sim.nbody && (st->ready = (uint32_t)behind((int)st->ready, sim.nbody, at)) < (uint32_t)sim.nbody) {
			if (phase >= (sim.ahead ? at + 1 : at | 1))
				return 0;
		} else {
			show_share(s, (int)((sim.base + at) % PHYS_BUFFERS), (int)(show & 1));
			st->shown = show, st->ready = 0;
			return 1;
		}
	}
	if (!wanted && phase >= sim.limit)
		return 0;
	if (phase % PHYS_EVERY == 1 && !sim.ahead && !wanted && !serves && !share_sleeps(s, phase)) {
		/* (the substep after a look, which every share began once all had made the one before) */
		if (!partners_ready(s, phase))
			return 0;
	} else if (sim.nservice && serves && !wanted && phase % PHYS_EVERY != PHYS_EVERY - 1 && !sim.ahead) {
		/* (a share of the grid and the pictures with neither to make reads nothing) */
	} else if (*seen < n && (*seen = behind(*seen, n, phase)) < n)
		return 0;
	return share_work(s, seen, phase, wanted, show);
}

/* A worker's job: its share, for as long as the boxes move. (Run by core 0, it does nothing:
 * that core has its own share and those of the workers that have no core, in its own loop.) */
static uint32_t
share_job(uint32_t s, uint32_t worker)
{
	int seen = 0;

	if (!mcw_on_worker())
		return 0;
	/* (with nothing to do its pass ends there and it looks again in the next: turning in
	 * place it cost the machine the pass's instructions, 8,192 of them, for nothing) */
	while (!sim.stop && !mcw_recalled((int)worker))
		if (!share_advance((int)s, &seen) && !sim.spin)
			mc_end_pass();
	return 1;
}

/* ---- core 0 ---- */

#define BAR 54			/* the bar under the picture: a row of buttons and a line of figures */
#define BUTTONS 7
#define LENS 0.6f		/* how far the view opens up and down at one unit ahead */
static const struct { const char *label; int key; } buttons[BUTTONS] = {
	{" Wall", '1'}, {" Towers", '2'}, {" Heap", '3'}, {" Throw", ' '}, {" Stop", 'p'}, {" Cores", 'c'}, {" Workers", 'W'},
};
#define REACH 60.0f		/* how far along a hand's or a sitter's aim a box is taken */
#define WALK 6.0f		/* what the keys move the view by, a second */
static GR_WINDOW_ID window, picture;
static int bar_y, wide, button_step = 90;	/* (the buttons share the bar's width) */
static char status[120] = "";
static float eye[3], ahead[3], right[3], up[3];
/* the button held in the picture: where the window is on the display, where the pointer was
 * last, and how far ahead of the eye the held box's point was taken */
static int talk;	/* PHYS_SAY: lines on the console as it runs (a line is a tenth of a second of this core) */
static int holding, held_for_good, origin_x, origin_y, last_x, last_y;	/* (holding: 1 the left button, 2 the right) */
static float held_far;
static int press_x, press_y, dragged;	/* a left press on no box, let go at once where it was: a box is fired */
static unsigned press_at;
static float centre[3] = {0.0f, 2.5f, 0.0f};	/* what the view looks at: the keys walk it about */
/* A holodeck's sitter (docs/world/holodeck.md): 1 with tracked hands, 2 with a mouse. The view is
 * then theirs, level, from `body_at`; `metre` is a metre of their room in the scene's units. */
static int seated, seat_say;
static float body_at[3], metre = 1.0f;
static unsigned mouse_count, moved_at;
static int keys_down;			/* the keys held that walk and turn: KEY_ bits */
enum { KEY_AHEAD = 1, KEY_BACK = 2, KEY_LEFT = 4, KEY_RIGHT = 8, KEY_TURN_LEFT = 16, KEY_TURN_RIGHT = 32, KEY_TAKE = 64 };
static int hand_down[PHYS_HOLDS], stick_was;	/* a hold's button as it was; the right stick pushed up or down */
static float hand_far[PHYS_HOLDS];	/* how far along its hand's way a hold's box was taken */
static int wanted_again;		/* what a hand or a click asks of the main loop: act()'s numbers, 4 a box fired */
static float fire_from[3], fire_way[3];
static int view_moved, shown_buffer = -1;	/* the view is not the shown picture's; which buffer that picture is */
static unsigned pictures;	/* pictures shown since the shares began */
/* A launch (act()'s 1, 3 or 4) is made when every share waits at substep launch_at, their jobs
 * kept; launch_old (PHYS_LAUNCH_OLD): by stopping and starting them, as it was. */
static int launch_kind, launch_old;
static uint32_t launch_at, launch_then;
static GLuint texture;
/* The workers asked for: all of them. With more than the bodies need, some have the grid and
 * the pictures (service_of); the wall's 240 substeps took 1,740 ms with 207 and 1,704 to 1,718
 * with 223 to 255, the heap's 120 took 1,181 and 1,133 (docs/apps/physics.md). */
#define WORKERS_ASKED 255
static int workers, wanted = WORKERS_ASKED, stopped, scene = 1, bodies_wanted = 400, total_quads;
#define OWN_UP_TO 7		/* this core has a share of its own with so many workers or fewer: with more it costs passes */
#define SERVICE_FROM 64		/* from so many shares on some of them have no bodies, but the grid and the pictures; */
#define SERVICE_LEAST 16	/* never fewer of those than so many: the grid is every body's key looked at by each */
static int own;			/* 1: share 0 is this core's and worker k has share k; 0: worker k has share k - 1 */
static uint8_t running[MC_MAX_CORES];	/* worker k has its share's job out */
static int seen_by[MC_MAX_CORES];
static int first_own, jobs_changed = 1;	/* its own apart, no share before first_own is this core's (found again as jobs begin and end) */
static int shown_so_far;	/* shares found to have made the picture asked for */
static float view_turn = 1.15f, view_high = 11.0f, view_far = 22.0f;	/* along the wall and from above: both its sides show */

static const volatile uint32_t *
control(int offset)
{
	return (const volatile uint32_t *)((const char *)seglPalette() - 0x400 + offset);
}

static unsigned now_ms(void) { return *control(0x34); }

static uint32_t waited, waited_before;	/* passes this core ended with nothing to do; and when the shares began */

static void
pass(void)
{
	waited++;
	mcw_rest();	/* the kernel has the core for what is left of the pass (the pause hint with no workers) */
}

static inline uint32_t bits(float f) { union { float f; uint32_t u; } c = {f}; return c.u; }

/* The colours' table: a row a colour, 64 shades of it across; and the floor's squares. */
static void
make_table(uint32_t *texels)
{
	static const uint8_t colours[COLOURS][3] = {
		{225, 80, 60}, {240, 150, 50}, {235, 205, 70}, {100, 195, 90}, {70, 185, 200}, {80, 125, 230}, {150, 100, 220}, {225, 110, 175},
		{150, 152, 158}, {105, 108, 116}, {250, 250, 250}, {140, 255, 60}, {60, 255, 190}, {255, 120, 120}, {120, 170, 255}, {255, 200, 140},
	};
	int c, s, x, z, corner;

	for (c = 0; c < COLOURS; c++)
		for (s = 0; s < SHADES; s++)
			texels[c * SHADES + s] = (uint32_t)(colours[c][0] * s / (SHADES - 1)) << 16 | (uint32_t)(colours[c][1] * s / (SHADES - 1)) << 8 |
				(uint32_t)(colours[c][2] * s / (SHADES - 1));
	for (z = 0; z < TILES; z++)
		for (x = 0; x < TILES; x++) {
			float side = 2.0f * ROOM / TILES, x0 = -ROOM + side * (float)x, z0 = -ROOM + side * (float)z;
			uint32_t texel = (56u * 16 + 8) | (uint32_t)(((x ^ z) & 1 ? 8 : 9) * 64 + 32) << 16, q[16];

			for (corner = 0; corner < 4; corner++) {
				q[4 * corner] = bits(x0 + (corner == 1 || corner == 2 ? side : 0.0f));
				q[4 * corner + 1] = 0;
				q[4 * corner + 2] = bits(z0 + (corner >= 2 ? side : 0.0f));
				q[4 * corner + 3] = texel;
			}
			memcpy(sim.quads[0] + 16 * (z * TILES + x), q, sizeof q);
			memcpy(sim.quads[1] + 16 * (z * TILES + x), q, sizeof q);
		}
}

static uint32_t chance = 12345;
static float
random_unit(void)
{
	chance = chance * 1103515245u + 12345u;
	return (float)(chance >> 8 & 0xffff) * (1.0f / 65536.0f);
}

static int scene_into;	/* the buffer set_scene() puts the boxes in */
static struct body_state *starts;	/* the scene's boxes as set_scene() put them, for the scene again */
static int starts_of;	/* which scene those are, 0 none */

/* A box put in the room at rest, upright; where the next goes. */
static int
put(int at, float hx, float hy, float hz, float density, float x, float y, float z, int colour)
{
	struct body_state *s = &sim.w.states[scene_into][at];

	if (at >= MAX_BODIES)
		return at;
	memset(s, 0, sizeof *s);
	phys_box(&sim.w, at, hx, hy, hz, density);
	s->pos[0] = x, s->pos[1] = y, s->pos[2] = z;
	s->q[3] = 1.0f;
	sim.colours[at] = (uint8_t)colour;
	return at + 1;
}

/* The heavy box, the last body, sent from one end of the room at the middle. */
static void
throw_box(int buffer)
{
	struct body_state *s = &sim.w.states[buffer][sim.w.bodies - 1];

	memset(s, 0, sizeof *s);
	s->pos[0] = 0.0f, s->pos[1] = 2.2f, s->pos[2] = ROOM - 2.0f;
	s->q[3] = 1.0f;
	s->vel[1] = 2.0f, s->vel[2] = -14.0f;
	s->spin[0] = 1.5f, s->spin[1] = 0.7f;
	s->key = phys_key(&sim.w, s->pos);
}

/* Memory nobody has written yet (zeros, from mcw_alloc()) is there for the workers: a store
 * of a zero into each of its pages. A worker stops at every page that is not. */
static void
there(void *memory, unsigned bytes)
{
	volatile uint32_t *at = memory, *end = (volatile uint32_t *)((char *)memory + bytes);

	for (; at < end; at = (volatile uint32_t *)(((uintptr_t)at | 4095) + 1))
		*at = 0;
}

/* What the workers read and write of the first n bodies is there (the rest is this core's to write). */
static void
bodies_there(int n)
{
	static int so_far;
	int k;

	if (n <= so_far)
		return;
	for (k = 0; k < PHYS_BUFFERS; k++)
		there(&sim.w.poses[k][so_far], (unsigned)(n - so_far) * sizeof(struct body_pose));
	for (k = 0; k < PHYS_BUFFERS; k++)
		there(&sim.w.nears[k][so_far], (unsigned)(n - so_far) * sizeof(struct body_near));
	for (k = 1; k < PHYS_BUFFERS; k++)
		there(&sim.w.states[k][so_far], (unsigned)(n - so_far) * sizeof(struct body_state));
	there(&sim.w.info[so_far], (unsigned)(n - so_far) * sizeof(struct body_info));
	there(&sim.w.manifolds[so_far * PHYS_MANIFOLDS], (unsigned)(n - so_far) * PHYS_MANIFOLDS * sizeof(struct manifold));
	/* (and their quads of both pictures: GPU memory is a worker's to write after a store of this core's) */
	for (k = 0; k < 2; k++)
		there(sim.quads[k] + 16 * FLOOR_QUADS + PHYS_QUAD_WORDS * so_far, (unsigned)(n - so_far) * PHYS_QUAD_WORDS * 4);
	so_far = n;
}

/* A scene's boxes, into states[buffer]: a wall of bricks, towers of cubes, or boxes let fall. */
static void
set_scene(int buffer)
{
	int n = 0, i, j, k;

	chance = 12345, scene_into = buffer;
	if (scene == 1) {
		/* every other row half a brick along, with a half brick at each end: nothing hangs over */
		for (j = 0; j < (getenv("PHYS_ROWS") ? atoi(getenv("PHYS_ROWS")) : 10); j++)
			for (i = 0; i < (j & 1 ? 21 : 20); i++) {
				int half = j & 1 && (i == 0 || i == 20);
				float x = j & 1 ? (i == 0 ? -10.0f : i == 20 ? 9.5195f : -9.75f + 0.5005f + 1.001f * (float)(i - 1)) : -9.75f + 1.001f * (float)i;

				n = put(n, half ? 0.25f : 0.5f, 0.25f, 0.25f, 1.0f, x, 0.25f + 0.501f * (float)j, 0.0f, (i + j / 2) % 8);
			}
	} else if (scene == 2) {
		for (k = 0; k < 9; k++)
			for (j = 0; j < 12; j++)
				n = put(n, 0.3f, 0.3f, 0.3f, 1.0f, -3.0f + 3.0f * (float)(k % 3), 0.3f + 0.601f * (float)j, -3.0f + 3.0f * (float)(k / 3), (k + j) % 8);
	} else if (scene == 4) {
		/* (a test's: one tower) */
		for (j = 0; j < bodies_wanted - 1; j++)
			n = put(n, 0.3f, 0.3f, 0.3f, 1.0f, 0.0f, 0.3f + 0.601f * (float)j, 0.0f, j % 8);
	} else {
		for (i = 0; i < bodies_wanted - 1; i++) {
			struct body_state *s = &sim.w.states[buffer][n];
			float q[4] = {random_unit() - 0.5f, random_unit() - 0.5f, random_unit() - 0.5f, random_unit() + 0.2f}, len;

			n = put(n, 0.25f + 0.25f * random_unit(), 0.2f + 0.15f * random_unit(), 0.25f + 0.2f * random_unit(), 1.0f,
				-5.2f + 1.3f * (float)(i % 9), 1.5f + 1.3f * (float)(i / 81), -5.2f + 1.3f * (float)(i / 9 % 9), i % 8);
			len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			for (k = 0; k < 4; k++)
				s->q[k] = q[k] / len;
		}
	}
	n = put(n, 0.4f, 0.4f, 0.4f, 6.0f, 0, 0, 0, 10);
	sim.w.bodies = n;
	bodies_there(n);
	if (scene < 3 && !getenv("PHYS_STILL"))
		throw_box(buffer);
	else
		sim.w.states[buffer][n - 1].pos[1] = 0.4f, sim.w.states[buffer][n - 1].pos[0] = ROOM - 1.0f;
	if (!starts)
		starts = malloc(MAX_BODIES * sizeof *starts);
	if (starts)
		memcpy(starts, sim.w.states[buffer], (size_t)n * sizeof *starts), starts_of = scene;
}

/* How many of n shares have no bodies, but the grid and the pictures: none of a few, three
 * in sixteen of many, and no more than leaves the fullest share with as many bodies as it
 * would have (a share is as long as its bodies: one more of them costs more than the grid). */
static int
service_of(int n)
{
	const char *text = getenv("PHYS_SERVICE");
	int most = n * 3 / 16, each = (sim.w.bodies + n - 1) / n, spare;

	if (text)
		return atoi(text) > 0 && atoi(text) < n ? atoi(text) : 0;
	if (n < SERVICE_FROM)
		return 0;
	spare = n - (sim.w.bodies + (each > 0 ? each : 1) - 1) / (each > 0 ? each : 1);
	if (spare < most)
		most = spare;
	return most >= SERVICE_LEAST ? most : 0;
}

/* The bodies of the scene shared out among the `n` shares there are. */
static void
share_bodies(int n)
{
	sim.nshares = n;
	sim.nservice = service_of(n);
	sim.nbody = n - sim.nservice;
	sim.w.cell_shares = sim.nservice ? sim.nservice : n, sim.w.cell_first = sim.nservice ? sim.nbody : 0;
}

/* What the look at substep `step` needs of states the program wrote there: every body's key,
 * which names its cell's share, its body_near and the grid. The shares make every one after. */
static void
written_at(uint32_t step)
{
	int i, buffer = (int)((sim.base + step) % PHYS_BUFFERS);

	for (i = 0; i < sim.w.bodies; i++)
		sim.w.states[buffer][i].key = phys_key(&sim.w, sim.w.states[buffer][i].pos);
	phys_nears(&sim.w, buffer);
	phys_grid_all(&sim.w, buffer, (int)((sim.base + step) / PHYS_EVERY) + 1);
}

/* The bodies shared out among `n` shares, all to begin a round of phases. */
static void
set_shares(int n)
{
	int s;

	share_bodies(n);
	jobs_changed = 1, shown_so_far = 0;
	for (s = 0; s < n; s++) {
		memset(&sim.shares[s], 0, sizeof sim.shares[s]);
		sim.shares[s].shown = sim.show;
		seen_by[s] = 0;
	}
	written_at(0);
}

static uint32_t
phase_least(void)
{
	uint32_t least = ~0u;
	int s;

	for (s = 0; s < sim.nshares; s++)
		if (sim.shares[s].phase < least)
			least = sim.shares[s].phase;
	return least;
}

static uint32_t
phase_most(void)
{
	uint32_t most = 0;
	int s;

	for (s = 0; s < sim.nshares; s++)
		if (sim.shares[s].phase > most)
			most = sim.shares[s].phase;
	return most;
}

/* PHYS_COUNT: how many of this core's passes each substep took, with a look and without (it
 * sees a substep made a pass late, every one alike). */
static uint32_t passes_of[2][8], noted_phase, noted_at;

static void
note_substeps(void)
{
	uint32_t least = phase_least();

	for (; noted_phase < least; noted_phase++, noted_at = waited) {
		uint32_t took = waited - noted_at;

		passes_of[noted_phase % PHYS_EVERY != 0][took < 7 ? took : 7]++;
	}
}

/* This core's own share, when it has one, and those of workers that have no core or no job out. */
static int
own_shares(void)
{
	int s, did = 0;

	/* (not every share looked at every turn: this core's turns are a share's time too) */
	if (jobs_changed) {
		for (first_own = own; first_own < sim.nshares && running[first_own + 1 - own]; first_own++)
			;
		jobs_changed = 0;
	}
	if (own)
		did = share_advance(0, &seen_by[0]);
	for (s = first_own; s < sim.nshares; s++)
		if (!running[s + 1 - own])
			did |= share_advance(s, &seen_by[s]);
	return did;
}

/* Which jobs have ended (a core the kernel wanted back, or a job this core was left to run). */
static void
look_at_jobs(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		if (running[k] && mcw_done(k))
			running[k] = 0, jobs_changed = 1;
}

static void
start_jobs(void)
{
	int k;

	sim.stop = 0;
	for (k = 1; k <= workers && k - 1 + own < sim.nshares; k++)
		if (!running[k]) {
			running[k] = 1, jobs_changed = 1;
			mcw_post(k, share_job, (uint32_t)(k - 1 + own), (uint32_t)k);
		}
}

/* Every share brought to one phase and the jobs ended: the bodies are then one state. */
static void
settle(void)
{
	uint32_t limit = sim.limit, most = phase_most();
	int k;

	if (most < limit)
		sim.limit = most;
	while (phase_least() < sim.limit) {
		look_at_jobs();
		if (!own_shares())
			pass();
	}
	sim.stop = 1;
	for (k = 1; k <= workers; k++)
		if (running[k]) {
			mcw_wait(k);
			running[k] = 0, jobs_changed = 1;
		}
	sim.limit = limit;
}

/* After settle(): the shares begin again from where the bodies are, with a look. */
static void
begin_again(void)
{
	sim.base += sim.nshares ? sim.shares[0].phase : 0;
}

static void share_out(void);
static void hold_drop(int slot);
static void throw_box(int buffer);
static void fire_box(int buffer);

/* A launch asked for: the shares stop at the first substep none can have passed, three past what
 * this core sees or the limit, which none passes. */
static void
launch_ask(int kind)
{
	if (!launch_kind) {
		uint32_t seen = phase_most() + 3;

		launch_then = sim.limit;
		launch_at = sim.limit < seen ? sim.limit : seen;
	}
	if (!launch_kind || kind == 1)
		launch_kind = kind;
	/* (a box at a substep without a look, which needs only the box's own: the scene at a look's) */
	if ((launch_kind == 1) == (launch_at & 1) && ++launch_at > sim.limit)
		sim.limit = launch_at;
	if (kind == 4 && talk)
		fprintf(stderr, "nxphys: a box fired from %d %d %d\n", (int)(fire_from[0] * 1000.0f), (int)(fire_from[1] * 1000.0f),
			(int)(fire_from[2] * 1000.0f));
	if (sim.limit > launch_at)
		sim.limit = launch_at;
}

/* Every share is at launch_at and waits: the boxes written there, and the shares let go on. */
static void
launch(void)
{
	int buffer = (int)((sim.base + launch_at) % PHYS_BUFFERS), k;

	for (k = 0; k < PHYS_HOLDS; k++)
		hold_drop(k);
	if (launch_kind == 1 && starts_of == scene) {
		/* (the same scene again: its boxes as they were put, a copy) */
		memcpy(sim.w.states[buffer], starts, (size_t)sim.w.bodies * sizeof *starts);
	} else if (launch_kind == 1) {
		shown_buffer = -1;
		set_scene(buffer);
		share_bodies(sim.nshares);
		jobs_changed = 1, shown_so_far = 0;
	} else if (launch_kind == 3)
		throw_box(buffer);
	else
		fire_box(buffer);
	/* (before a look, all a look needs; else the box's own: the grid the next look reads is made from it) */
	if (launch_at % PHYS_EVERY == 0)
		written_at(launch_at);
	else
		phys_near_of(&sim.w, buffer, sim.w.bodies - 1);
	/* (in real time the pace goes on from there; and that substep is the next picture's) */
	sim.limit = launch_then == ~0u || launch_then < launch_at ? launch_at : launch_then;
	sim.show_step = ~0u;
	launch_kind = 0, launch_at = 0;
}

/* The workers given back and as many taken as wanted, and a share for each. */
static void
take_workers(void)
{
	extern char __DATA_BEGIN__[], _end[];
	const char *owned = getenv("PHYS_OWN");

	mcw_close();
	workers = wanted > 0 ? mcw_open(wanted) : 0;
	if (sim.count)
		mcw_counters();
	if (workers) {
		extern char _start[], etext[];

		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
		/* and the program's code: a page of it that no core has run yet stops every worker that comes to it */
		/* (from _start: a flat program's __executable_start is 0) */
		mcw_touch_code(_start, (unsigned)(etext - _start));
	}
	/* (a share a worker, and with few of them one for this core, which would only wait:
	 * with many, every share waiting for this core's as well costs more passes than it saves) */
	own = workers > 0 && (owned ? atoi(owned) != 0 : workers <= OWN_UP_TO);
	share_out();
}

/* The scene's bodies shared out among the workers there are. */
static void
share_out(void)
{
	const char *text = getenv("PHYS_SHARES");
	int n = text && atoi(text) > workers ? atoi(text) : workers ? workers + own : 1;

	set_shares(n < MC_MAX_CORES - 1 + own ? n : MC_MAX_CORES - 1 + own);
}

/* The picture the shares have made, shown; or (again) the one shown last, from where the view
 * is now: turning the view asks nothing of the shares and waits for no substep. */
static void
show_picture(int again)
{
	if (!again)
		shown_buffer = (int)(sim.show & 1), pictures++;
	if (shown_buffer < 0)
		return;
	view_moved = 0;
	seglKeptQuads(0, sim.quads[shown_buffer], 0, FLOOR_QUADS + 6 * sim.w.bodies, texture, 255, 0);
	seglKeptBlend(0, 0, 1);
	seglKeptMatrices();
	glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	seglSwapEarly();	/* (no waiting for it: ask_picture() settles before that buffer is written again) */
}

/* The substep the next picture is of. A picture's share may have made three since the phases
 * this core sees; none goes past the limit. */
static uint32_t
picture_step(void)
{
	uint32_t at = phase_most() + (sim.nservice ? 3 : 1), last = sim.limit;

	return at < last ? at : last;
}

static void
ask_picture(void)
{
	/* (the list being drawn may read the buffer the shares are about to write) */
	seglSettle();
	sim.show_step = picture_step();
	sim.show++;
	shown_so_far = 0;
}

static int
picture_made(void)
{
	/* (with shares of the pictures, those: the ones before them make none) */
	if (shown_so_far < sim.nbody && sim.nservice)
		shown_so_far = sim.nbody;
	for (; shown_so_far < sim.nshares; shown_so_far++)
		if (sim.shares[shown_so_far].shown != sim.show)
			return 0;
	return 1;
}

/* The view: from a circle round the room's middle (or where the keys have walked), looking at it. */
static void
set_view(void)
{
	float to[3] = {centre[0], centre[1], centre[2]}, len, m[16];
	int width, height, k;

	eye[0] = centre[0] + view_far * sinf(view_turn), eye[1] = view_high * view_far * (1.0f / 22.0f), eye[2] = centre[2] + view_far * cosf(view_turn);
	if (seated) {
		/* a sitter's: from where they are, level (the room keeps its own horizon) */
		for (k = 0; k < 3; k++)
			eye[k] = to[k] = body_at[k];
		to[0] -= sinf(view_turn), to[2] -= cosf(view_turn);
	}
	for (len = 0.0f, k = 0; k < 3; k++)
		ahead[k] = to[k] - eye[k], len += ahead[k] * ahead[k];
	for (len = sqrtf(len), k = 0; k < 3; k++)
		ahead[k] /= len;
	right[0] = -ahead[2], right[1] = 0.0f, right[2] = ahead[0];	/* ahead x up */
	len = sqrtf(right[0] * right[0] + right[2] * right[2]);
	right[0] /= len, right[2] /= len;
	up[0] = right[1] * ahead[2] - right[2] * ahead[1], up[1] = right[2] * ahead[0] - right[0] * ahead[2], up[2] = right[0] * ahead[1] - right[1] * ahead[0];
	for (k = 0; k < 3; k++)
		m[4 * k] = right[k], m[4 * k + 1] = up[k], m[4 * k + 2] = -ahead[k], m[4 * k + 3] = 0.0f;
	m[12] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
	m[13] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
	m[14] = ahead[0] * eye[0] + ahead[1] * eye[1] + ahead[2] * eye[2];
	m[15] = 1.0f;
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustumf(-LENS * 0.5f * (float)width / (float)(height > 0 ? height : 1), LENS * 0.5f * (float)width / (float)(height > 0 ? height : 1), -LENS * 0.5f,
		LENS * 0.5f, 0.5f, 100.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(m);
}

/* The way from the eye through a pixel of the picture, at unit length. */
static void
ray(int px, int py, float *way)
{
	int width, height, k;
	float x, y, len = 0.0f;

	seglSize(&width, &height);
	x = (2.0f * ((float)px + 0.5f) / (float)(width > 0 ? width : 1) - 1.0f) * LENS * (float)width / (float)(height > 0 ? height : 1);
	y = (1.0f - 2.0f * ((float)py + 0.5f) / (float)(height > 0 ? height : 1)) * LENS;
	for (k = 0; k < 3; k++)
		way[k] = ahead[k] + right[k] * x + up[k] * y, len += way[k] * way[k];
	for (len = sqrtf(len), k = 0; k < 3; k++)
		way[k] /= len;
}

/* The window is so large now: the picture has all of it but the bar, the buttons its width. */
static void
set_size(int width, int height)
{
	if (width == wide && height == bar_y + BAR)
		return;
	wide = width;
	bar_y = height > BAR + 40 ? height - BAR : 40;
	button_step = wide > 10 + 20 * BUTTONS ? (wide - 10) / BUTTONS : 20;
	GrResizeWindow(picture, wide, bar_y);
	seglWindowChanged();
	set_view();
	view_moved = 1;
}

/* Nearer or further: a held box along the ray it was taken by, or with none held the eye. */
static void
zoom(int out)
{
	int slot, any = 0;

	if (!seated && sim.w.holds[0].body >= 0) {
		held_far *= out ? 1.1f : 0.9f;
		if (held_far < 2.0f) held_far = 2.0f;
		if (held_far > 60.0f) held_far = 60.0f;
		last_x = -1;	/* (the box's place is worked out again) */
		return;
	}
	for (slot = 0; slot < PHYS_HOLDS; slot++)
		if (sim.w.holds[slot].body >= 0 && hand_far[slot] > 0.0f) {
			hand_far[slot] *= out ? 1.1f : 0.9f;
			if (hand_far[slot] < 0.5f) hand_far[slot] = 0.5f;
			if (hand_far[slot] > REACH) hand_far[slot] = REACH;
			any = 1;
		}
	if (any || seated)
		return;
	view_far *= out ? 1.1f : 0.9f;
	if (view_far < 6.0f) view_far = 6.0f;
	if (view_far > 45.0f) view_far = 45.0f;
	set_view();
	view_moved = 1;
}

/* The bar under the picture: a button for what the keys do, and a line of figures. */
static void
draw_bar(void)
{
	int k;

	ui_fill(window, 0, bar_y, wide, BAR, UI_FACE);
	for (k = 0; k < BUTTONS; k++) {
		int key = buttons[k].key, down = (key == 'p' && stopped) || (key == 'c' && sim.owners) || (key == 'W' && workers > 0);

		ui_button(window, 5 + button_step * k, bar_y + 5, button_step - 4, 22, key == 'p' && stopped ? " Go" : buttons[k].label, down);
	}
	ui_text(window, 8, bar_y + 33, status, -1, BLACK, 0);
	GrFlush();
}

/* ---- boxes in the hand: the pointer's is hold 0, a sitter's hands' 1 and 2 ---- */

static int
state_now(void)
{
	return (int)((sim.base + phase_least()) % PHYS_BUFFERS);
}

/* Where a hold's point is wanted: inside the room and over the floor. */
static void
hold_to(int slot, const float *to)
{
	struct phys_hold *hold = &sim.w.holds[slot];
	int k;

	for (k = 0; k < 3; k++) {
		float most = k == 1 ? 2.0f * ROOM_HIGH - 1.0f : ROOM - 0.5f, least = k == 1 ? 0.3f : -most;
		float at = to[k] > most ? most : to[k] < least ? least : to[k];

		if (hold->to[k] != at)
			hold->to[k] = at;
	}
}

static void
hold_take(int slot, int body, const float *local, const float *to)
{
	struct phys_hold *hold = &sim.w.holds[slot];
	int k;

	/* (where it is wanted first, in its own texel: a share that sees the box sees that) */
	hold_to(slot, to);
	for (k = 0; k < 3; k++)
		hold->at[k] = local[k];
	hold->body = body;
	sim.w.held++;
}

static void
hold_drop(int slot)
{
	if (sim.w.holds[slot].body < 0)
		return;
	sim.w.holds[slot].body = -1;
	sim.w.held--;
}

static int
speed_of(int body)
{
	const float *vel = sim.w.states[state_now()][body].vel;

	return (int)(1000.0f * sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]));
}

/* Let go, a box keeps what speed the hand gave it: it is thrown. */
static void
let_go(int slot)
{
	if (sim.w.holds[slot].body >= 0 && talk)
		fprintf(stderr, "nxphys: box %d let go at %d mm/s\n", sim.w.holds[slot].body, speed_of(sim.w.holds[slot].body));
	hold_drop(slot);
}

/* A hold's hand: where it is, the way it points (or none), how near a box it must be to take
 * it without pointing, its button. The box is taken as the button goes down, wanted where
 * the hand is (or as far along its way as it was) while held, and let go as it comes up. */
static void
hand(int slot, const float *at, const float *way, float near, int down)
{
	struct phys_hold *hold = &sim.w.holds[slot];
	float local[3], to[3], distance = 0.0f;
	int was = hand_down[slot], body, k;

	hand_down[slot] = down;
	if (!down) {
		let_go(slot);
		return;
	}
	if (!was) {
		hand_far[slot] = 0.0f;
		body = near > 0.0f ? phys_near(&sim.w, state_now(), at, near, local) : -1;
		if (body < 0 && way && (body = phys_pick(&sim.w, state_now(), at, way, local, &distance)) >= 0) {
			if (distance > REACH)
				body = -1;
			else
				hand_far[slot] = distance;
		}
		if (body < 0)
			return;
		for (k = 0; k < 3; k++)
			to[k] = at[k] + (way ? way[k] * hand_far[slot] : 0.0f);
		hold_take(slot, body, local, to);
		if (talk)
			fprintf(stderr, "nxphys: box %d held by hand %d, %d mm away\n", body, slot, (int)(distance * 1000.0f));
		return;
	}
	if (hold->body < 0)
		return;
	for (k = 0; k < 3; k++)
		to[k] = at[k] + (way ? way[k] * hand_far[slot] : 0.0f);
	hold_to(slot, to);
}

static void
say_view(void)
{
	if (talk)
		fprintf(stderr, "nxphys: the view is from %d %d %d, looking at %d %d %d\n", (int)(eye[0] * 1000.0f), (int)(eye[1] * 1000.0f), (int)(eye[2] * 1000.0f),
			(int)(centre[0] * 1000.0f), (int)(centre[1] * 1000.0f), (int)(centre[2] * 1000.0f));
}

/* A box to be fired from `from` along `way` (unit), from where that way is inside the room:
 * 4 for the main loop, 0 when it never is. */
static int
fire_asked(const float *from, const float *way)
{
	float in = 0.0f, out = 1e30f;
	int k;

	for (k = 0; k < 3; k++) {
		float most = k == 1 ? 2.0f * ROOM_HIGH - 1.0f : ROOM - 0.8f, least = k == 1 ? 0.6f : -most;

		if (fabsf(way[k]) < 1e-6f) {
			if (from[k] < least || from[k] > most)
				return 0;
		} else {
			float a = (least - from[k]) / way[k], b = (most - from[k]) / way[k];

			if (a > b) { float c = a; a = b; b = c; }
			if (a > in) in = a;
			if (b < out) out = b;
		}
	}
	in += 0.8f;	/* (clear of whoever fires it, or of the wall it came in by) */
	if (in > out)
		return 0;
	for (k = 0; k < 3; k++)
		fire_from[k] = from[k] + way[k] * in, fire_way[k] = way[k];
	return 4;
}

/* The heavy box, the last body, fired from there. */
static void
fire_box(int buffer)
{
	struct body_state *s = &sim.w.states[buffer][sim.w.bodies - 1];
	int k;

	memset(s, 0, sizeof *s);
	for (k = 0; k < 3; k++)
		s->pos[k] = fire_from[k], s->vel[k] = 16.0f * fire_way[k];
	s->q[3] = 1.0f;
	s->spin[0] = 1.5f, s->spin[1] = 0.7f;
	s->key = phys_key(&sim.w, s->pos);
}

/* The way that is `yaw` to the right of the view's and `pitch` up (65,536ths of a turn). */
static void
way_of(int yaw, int pitch, float *way)
{
	float a = (float)yaw * (6.2831853f / 65536.0f), p = (float)pitch * (6.2831853f / 65536.0f), c = cosf(p), ca = cosf(a) * c, sa = sinf(a) * c, sp = sinf(p);
	int k;

	for (k = 0; k < 3; k++)
		way[k] = ahead[k] * ca + right[k] * sa + up[k] * sp;
}

/* A place walked over the floor: so far ahead and to the right of the level way `f` (x and z). */
static void
walked(float *place, const float *f, float along, float aside, float most)
{
	place[0] += f[0] * along - f[1] * aside;
	place[2] += f[1] * along + f[0] * aside;
	if (place[0] > most) place[0] = most;
	if (place[0] < -most) place[0] = -most;
	if (place[2] > most) place[2] = most;
	if (place[2] < -most) place[2] = -most;
}

/* A holodeck's sitter (docs/world/holodeck.md, "Played from the seat"; docs/apps/physics.md, "The
 * controls"): the view is theirs, their hands take boxes, their sticks or keys walk and turn. */
static void
seat(float dt)
{
	static unsigned plane, said_at;
	static int placed;
	const volatile unsigned *host = seglHostState();
	unsigned w0 = host ? host[0] : 0;
	int kind = !(w0 & 16) ? 0 : (w0 & 32) ? 1 : (w0 & 15) == 2 ? 2 : 0, slot, h, k, moved = 0;
	float f[2], along, aside, way[3];

	if (kind != seated) {
		for (slot = 0; slot < PHYS_HOLDS; slot++) {
			hold_drop(slot);
			hand_down[slot] = 0;
		}
		holding = keys_down = placed = 0;
		seated = kind;
		if (!kind) {
			set_view();
			view_moved = 1;
			return;
		}
		mouse_count = SEGL_HOST_MOUSE_X(host);
	}
	if (!seated)
		return;
	if (!placed || SEGL_HOST_PLANE(host) != plane) {
		/* a metre of the room: its plane, 1.28 by 0.8 m, is `at` from the camera (GpuVolumePass.cginc) */
		float at = 0.5f * powf(100.0f / 0.5f, (float)(plane = SEGL_HOST_PLANE(host)) * (1.0f / 255.0f)), across;
		int width, height;

		seglSize(&width, &height);
		across = (float)width / (float)(height > 0 ? height : 1) * (1.0f / 1.28f);
		metre = 2.0f * at * LENS * (across > 1.25f ? across : 1.25f);
		if (!placed) {
			/* before what the view looked at, a step and a half away, facing it */
			body_at[0] = centre[0], body_at[2] = centre[2];
			f[0] = sinf(view_turn), f[1] = cosf(view_turn);
			walked(body_at, f, 1.0f + 1.5f * metre, 0.0f, ROOM - 0.5f);
			placed = 1;
		}
		body_at[1] = 0.93f * metre;	/* the seat's eye point over the room's floor: the floors are one */
		moved = 1;
	}
	along = (float)(!!(keys_down & KEY_AHEAD) - !!(keys_down & KEY_BACK)), aside = (float)(!!(keys_down & KEY_RIGHT) - !!(keys_down & KEY_LEFT));
	if (keys_down & (KEY_TURN_LEFT | KEY_TURN_RIGHT)) {
		view_turn += (keys_down & KEY_TURN_LEFT ? 1.6f : -1.6f) * dt;
		moved = 1;
	}
	f[0] = -sinf(view_turn), f[1] = -cosf(view_turn);
	if (seated == 1) {
		float look = (float)SEGL_HOST_STICK(host, 2) * (1.0f / 127.0f);

		/* the sticks as sticks: the right one turns, the left one walks the way the head looks */
		if (fabsf(look) >= 0.15f) {
			view_turn -= look * fabsf(look) * 2.44f * dt;
			moved = 1;
		}
		along += (float)SEGL_HOST_STICK(host, 1) * (1.0f / 127.0f), aside += (float)SEGL_HOST_STICK(host, 0) * (1.0f / 127.0f);
		way_of(SEGL_HOST_HEAD_YAW(host), 0, way);
		f[0] = way[0], f[1] = way[2];
	} else {
		unsigned count = SEGL_HOST_MOUSE_X(host);

		/* the mouse turns, as it turns the games: 0.066 degrees a count */
		if (count != mouse_count) {
			view_turn -= 0.001152f * (float)(short)(count - mouse_count);
			mouse_count = count;
			moved = 1;
		}
	}
	if (along != 0.0f || aside != 0.0f) {
		walked(body_at, f, along * 1.2f * metre * dt, aside * 1.2f * metre * dt, ROOM - 0.5f);
		moved = 1;
	}
	if (moved) {
		set_view();
		view_moved = 1;
	}
	if (seated == 1) {
		float at[2][3], unit = 0.004f * metre, len;
		int push = (int)SEGL_HOST_STICK(host, 3), now = abs((int)SEGL_HOST_STICK(host, 2)) > 50 ? 0 : push > 100 ? 1 : push < -100 ? -1 : 0;

		for (h = 1; h >= 0; h--) {
			for (k = 0; k < 3; k++)
				at[h][k] = eye[k] + (right[k] * (float)SEGL_HOST_HAND_X(host, h) + up[k] * (float)SEGL_HOST_HAND_Y(host, h) +
						     ahead[k] * (float)SEGL_HOST_HAND_Z(host, h)) * unit;
			/* the right hand points as its beam does; of the left one only the place is told
			 * (its second word is the sticks): it points as the eye sees it */
			if (h)
				way_of(SEGL_HOST_HAND_YAW(host, 1), SEGL_HOST_HAND_PITCH(host, 1), way);
			else {
				for (len = 0.0f, k = 0; k < 3; k++)
					way[k] = at[0][k] - eye[k], len += way[k] * way[k];
				for (len = sqrtf(len), k = 0; k < 3; k++)
					way[k] = len > 1e-3f ? way[k] / len : ahead[k];
			}
			hand(1 + h, at[h], way, 0.15f * metre, SEGL_HOST_HAND_TRIGGER(host, h) | SEGL_HOST_HAND_GRIP(host, h));
			/* the right stick pushed up fires a box from the right hand, pulled down is the scene again */
			if (h && now != stick_was && now > 0)
				wanted_again = fire_asked(at[1], way);
		}
		if (now != stick_was && now < 0)
			wanted_again = 1;
		stick_was = now;
		if (seat_say && now_ms() - said_at >= 1000) {
			said_at = now_ms();
			fprintf(stderr, "nxphys: seat 1 at %d %d %d turned %d, a metre %d, left %d %d %d right %d %d %d, boxes %d %d\n", (int)(eye[0] * 1000.0f),
				(int)(eye[1] * 1000.0f), (int)(eye[2] * 1000.0f), (int)(view_turn * 57.29578f), (int)(metre * 1000.0f), (int)(at[0][0] * 1000.0f),
				(int)(at[0][1] * 1000.0f), (int)(at[0][2] * 1000.0f), (int)(at[1][0] * 1000.0f), (int)(at[1][1] * 1000.0f), (int)(at[1][2] * 1000.0f),
				sim.w.holds[1].body, sim.w.holds[2].body);
		}
	} else {
		/* no hands: the aim is the view's way at the head's tilt, and Ctrl (their left button) takes */
		way_of(0, SEGL_HOST_HEAD_PITCH(host), way);
		hand(0, eye, way, 0.0f, !!(keys_down & KEY_TAKE));
		if (seat_say && now_ms() - said_at >= 1000) {
			said_at = now_ms();
			fprintf(stderr, "nxphys: seat 2 at %d %d %d turned %d, a metre %d, the aim %d %d %d, box %d\n", (int)(eye[0] * 1000.0f), (int)(eye[1] * 1000.0f),
				(int)(eye[2] * 1000.0f), (int)(view_turn * 57.29578f), (int)(metre * 1000.0f), (int)(way[0] * 1000.0f), (int)(way[1] * 1000.0f),
				(int)(way[2] * 1000.0f), sim.w.holds[0].body);
		}
	}
}

/* What moves with time: a sitter's view and hands, or with nobody seated the keys' walk. */
static void
motion(unsigned now)
{
	float dt = (float)(now - moved_at > 100 ? 100 : now - moved_at) * 0.001f, f[2];

	moved_at = now;
	seat(dt);
	if (seated || !(keys_down & (KEY_AHEAD | KEY_BACK | KEY_LEFT | KEY_RIGHT)))
		return;
	f[0] = -sinf(view_turn), f[1] = -cosf(view_turn);
	walked(centre, f, (float)(!!(keys_down & KEY_AHEAD) - !!(keys_down & KEY_BACK)) * WALK * dt,
		(float)(!!(keys_down & KEY_RIGHT) - !!(keys_down & KEY_LEFT)) * WALK * dt, ROOM);
	set_view();
	view_moved = 1;
}

/* The key's bit of those that are held to walk and turn, or 0. (A sitter's arrows walk and
 * turn: a headset's left stick is the arrow keys under the pointer's controls.) */
static int
key_bit(int key)
{
	switch (key) {
	case 'w': return KEY_AHEAD;
	case 's': return KEY_BACK;
	case 'a': return KEY_LEFT;
	case 'd': return KEY_RIGHT;
	case MWKEY_LCTRL: case MWKEY_RCTRL: return KEY_TAKE;
	case MWKEY_UP: return seated ? KEY_AHEAD : 0;
	case MWKEY_DOWN: return seated ? KEY_BACK : 0;
	case MWKEY_LEFT: return seated ? KEY_TURN_LEFT : 0;
	case MWKEY_RIGHT: return seated ? KEY_TURN_RIGHT : 0;
	}
	return 0;
}

/* What a key or its button does. 1: the scene again; 2: other workers; 3: the heavy box
 * thrown; 4: fired the way a sitter looks. */
static int
act(int key)
{
	float way[3];

	switch (key) {
	case MWKEY_ESCAPE:
	case 'q':
		/* (a sitter's left grip is Esc, and a desktop's Q is beside W and A: they stand up by End) */
		if (seated)
			break;
		sim.stop = 1; exit(0);
	case 'p': stopped = !stopped; break;
	case 'c': sim.owners = !sim.owners; break;
	case 'r': return 1;
	case '1': case '2': case '3': scene = key - '0'; return 1;
	case MWKEY_ENTER: scene = scene % 3 + 1; return 1;	/* the next scene: a headset's jump button */
	case '9': wanted = MC_MAX_CORES; return 2;
	case '0': wanted = 0; return 2;
	case 'W': wanted = workers ? 0 : WORKERS_ASKED; return 2;
	case ' ':
		if (!seated)
			return 3;
		way_of(0, seated == 2 ? SEGL_HOST_HEAD_PITCH(seglHostState()) : 0, way);
		return fire_asked(eye, way);
	case '+': case '=': zoom(0); break;
	case '-': zoom(1); break;
	case MWKEY_LEFT: view_turn -= 0.15f; set_view(); view_moved = 1; break;
	case MWKEY_RIGHT: view_turn += 0.15f; set_view(); view_moved = 1; break;
	case MWKEY_UP: if (view_high < 20.0f) view_high += 1.0f; set_view(); view_moved = 1; break;
	case MWKEY_DOWN: if (view_high > 2.0f) view_high -= 1.0f; set_view(); view_moved = 1; break;
	}
	return 0;
}

/* A button went down at that pixel of the picture (1 the left, 2 the right). The left one
 * holds the box under it by that point; with no box there, and the right one, the view. */
static void
press(int px, int py, int button)
{
	float way[3], local[3], to[3], distance;
	int body, k;

	last_x = press_x = px, last_y = press_y = py;
	holding = button, dragged = 0, press_at = now_ms();
	if (button != 1)
		return;
	ray(px, py, way);
	body = phys_pick(&sim.w, state_now(), eye, way, local, &distance);
	if (body < 0) {
		if (talk)
			fprintf(stderr, "nxphys: no box at %d, %d: the view is held\n", px, py);
		return;
	}
	held_far = distance * (way[0] * ahead[0] + way[1] * ahead[1] + way[2] * ahead[2]);
	for (k = 0; k < 3; k++)
		to[k] = eye[k] + way[k] * distance;
	hold_take(0, body, local, to);
	if (talk)
		fprintf(stderr, "nxphys: box %d held, %d mm away\n", body, (int)(distance * 1000.0f));
}

/* While the button is down: the held box's point goes where the pointer is, as far from the
 * eye as it was taken; or the view turns. The pointer is the machine's words (docs/machine/input.md).
 * A left press on no box that comes up at once where it went down fires a box that way. */
static void
pointer(void)
{
	int px = (int)*control(0x20) - origin_x, py = (int)*control(0x24) - origin_y, k;
	float way[3], to[3], far;

	if (!holding || held_for_good)
		return;
	if (!(*control(0x28) & (unsigned)holding)) {
		if (sim.w.holds[0].body >= 0) {
			let_go(0);
		} else if (holding == 1 && !dragged && now_ms() - press_at < 350) {
			ray(press_x, press_y, way);
			wanted_again = fire_asked(eye, way);
		} else {
			say_view();
		}
		holding = 0;
		return;
	}
	if (px == last_x && py == last_y)
		return;
	if (abs(px - press_x) + abs(py - press_y) > 3)
		dragged = 1;
	if (sim.w.holds[0].body >= 0) {
		ray(px, py, way);
		far = held_far / (way[0] * ahead[0] + way[1] * ahead[1] + way[2] * ahead[2]);
		for (k = 0; k < 3; k++)
			to[k] = eye[k] + way[k] * far;
		hold_to(0, to);
	} else {
		view_turn -= 0.01f * (float)(px - last_x);
		view_high += 0.05f * (float)(py - last_y);
		if (view_high > 20.0f) view_high = 20.0f;
		if (view_high < 2.0f) view_high = 2.0f;
		set_view();
		view_moved = 1;
	}
	last_x = px, last_y = py;
}

/* Keys and the buttons. What act() said, of the last thing that said anything. */
static int
events(void)
{
	GR_EVENT e;
	int again = 0, k;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return again;
		ui_origin_note(&e);	/* (the window's place is remembered: a click says it, an update forgets it) */
		if (e.type == GR_EVENT_TYPE_CLOSE_REQ) {
			sim.stop = 1;
			exit(0);
		}
		if (e.type == GR_EVENT_TYPE_EXPOSURE && e.exposure.wid == window)
			draw_bar();
		if (e.type == GR_EVENT_TYPE_KEY_DOWN) {
			if (key_bit(e.keystroke.ch))
				keys_down |= key_bit(e.keystroke.ch);
			else if ((k = act(e.keystroke.ch)) != 0)
				again = k;
		}
		if (e.type == GR_EVENT_TYPE_KEY_UP && key_bit(e.keystroke.ch)) {
			keys_down &= ~key_bit(e.keystroke.ch);
			say_view();
		}
		if (e.type == GR_EVENT_TYPE_FOCUS_OUT)
			keys_down = 0;
		if (e.type == GR_EVENT_TYPE_UPDATE && e.update.utype == GR_UPDATE_SIZE && e.update.wid == window) {
			set_size(e.update.width, e.update.height);
			draw_bar();
		}
		if (ui_wheel(&e)) {
			zoom(ui_wheel(&e) > 0);
			continue;
		}
		if (e.type != GR_EVENT_TYPE_BUTTON_DOWN || !(e.button.changebuttons & (GR_BUTTON_L | GR_BUTTON_R)))
			continue;
		if (e.button.y < bar_y) {
			/* (a sitter's trigger is the pointer's button too, under the pointer's controls) */
			if (seated)
				continue;
			/* (where the pointer is now, not where the event says it was: the box is taken
			 * under the hand; and the window where this press says it is: the picture is its
			 * top left corner) */
			ui_window_origin(window, &origin_x, &origin_y);
			press((int)*control(0x20) - origin_x, (int)*control(0x24) - origin_y, e.button.changebuttons & GR_BUTTON_L ? 1 : 2);
		} else if ((e.button.changebuttons & GR_BUTTON_L) && e.button.y >= bar_y + 5 && e.button.y < bar_y + 27 && e.button.x >= 5 &&
			   (e.button.x - 5) % button_step < button_step - 4 && (e.button.x - 5) / button_step < BUTTONS) {
			if ((k = act(buttons[(e.button.x - 5) / button_step].key)) != 0)
				again = k;
		}
	}
}

/* ---- PHYS_HANDS: a test's hands, moved between substeps so that every run of it is the same ---- */

#define SCRIPT_KEYS 32
static struct {
	int n;
	struct { uint32_t step; float at[3]; int grip; } key[SCRIPT_KEYS];
	uint32_t taken_at, let_go_at;
	int box, box_speed, hand_speed, worst;
} scripted[2];
static uint32_t script_first = ~0u, script_last;
static int script_on, script_deepest;

/* "SUBSTEP:HAND,X,Y,Z,GRIP ...": hand 0 or 1 is at that place of the room (millimetres) after
 * so many substeps, its grip closed or open from then; between two lines it is on the way. */
static void
script_read(const char *text)
{
	for (;;) {
		unsigned step;
		int h, x, y, z, grip, used = 0;

		if (sscanf(text, " %u:%d,%d,%d,%d,%d%n", &step, &h, &x, &y, &z, &grip, &used) < 6 || !used)
			return;
		text += used;
		if (h < 0 || h > 1 || scripted[h].n == SCRIPT_KEYS)
			continue;
		scripted[h].key[scripted[h].n].step = step, scripted[h].key[scripted[h].n].grip = grip;
		scripted[h].key[scripted[h].n].at[0] = (float)x * 0.001f, scripted[h].key[scripted[h].n].at[1] = (float)y * 0.001f;
		scripted[h].key[scripted[h].n++].at[2] = (float)z * 0.001f;
		scripted[h].box = -1;
		if (step < script_first) script_first = step;
		if (step > script_last) script_last = step;
		script_on = 1;
	}
}

/* Where a test's hand is after `step` substeps, and its grip (-1: it is not there yet). */
static int
script_at(int h, uint32_t step, float *at)
{
	int n = scripted[h].n, i, k;

	if (!n || step < scripted[h].key[0].step)
		return -1;
	for (i = 0; i + 1 < n && scripted[h].key[i + 1].step <= step; i++)
		;
	for (k = 0; k < 3; k++) {
		at[k] = scripted[h].key[i].at[k];
		if (i + 1 < n)
			at[k] += (scripted[h].key[i + 1].at[k] - at[k]) * (float)(step - scripted[h].key[i].step) /
				 (float)(scripted[h].key[i + 1].step - scripted[h].key[i].step);
	}
	return scripted[h].key[i].grip;
}

/* A test's hands after `step` substeps, every share being there: how far the held boxes are
 * from where they were wanted and how fast one let go is, then where they are wanted next. */
static void
script_apply(uint32_t step)
{
	float at[3], was[3], point[3], deepest, fastest;
	int h, k;

	for (h = 0; h < 2; h++) {
		struct phys_hold *hold = &sim.w.holds[1 + h];
		int grip = script_at(h, step, at), body = hold->body;

		if (grip < 0)
			continue;
		if (body >= 0) {
			float off = 0.0f, went = 0.0f;

			phys_point(&sim.w, state_now(), body, hold->at, point);
			for (k = 0; k < 3; k++)
				off += (point[k] - hold->to[k]) * (point[k] - hold->to[k]);
			if (step >= scripted[h].taken_at + 20 && (int)(1000.0f * sqrtf(off)) > scripted[h].worst)
				scripted[h].worst = (int)(1000.0f * sqrtf(off));
			if (!grip && step && script_at(h, step - 1, was) >= 0) {
				for (k = 0; k < 3; k++)
					went += (at[k] - was[k]) * (at[k] - was[k]);
				scripted[h].let_go_at = step, scripted[h].box = body;
				scripted[h].box_speed = speed_of(body), scripted[h].hand_speed = (int)(1000.0f * PHYS_HZ * sqrtf(went));
			}
		}
		hand(1 + h, at, NULL, 0.3f, grip);
		if (body < 0 && hold->body >= 0)
			scripted[h].taken_at = step, scripted[h].box = hold->body;
	}
	phys_figures(&sim.w, state_now(), &deepest, &fastest);
	if ((int)(deepest * 1000.0f) > script_deepest)
		script_deepest = (int)(deepest * 1000.0f);
}

/* A test's last lines: a sum of every body's state, and where and how fast the bodies are. */
static void
say_done(unsigned took, int print)
{
	int buffer = (int)((sim.base + sim.limit) % PHYS_BUFFERS), trouble = 0, i, k, s;
	const uint32_t *words = (const uint32_t *)sim.w.states[buffer];
	uint32_t sum = 0;
	float deepest, fastest, top = 0.0f;

	for (i = 0; i < sim.w.bodies; i++) {
		/* (not the key: it names a share, and the sum is the same for any number of them) */
		for (k = 0; k < 16; k++)
			if (k != 3)
				sum = sum * 31 + words[16 * i + k];
		if (sim.w.states[buffer][i].pos[1] > top)
			top = sim.w.states[buffer][i].pos[1];
		if (print) {
			const struct body_state *b = &sim.w.states[buffer][i];

			/* (its turn in ten-thousandths, its half sides in millimetres, its spin in thousandths a second) */
			fprintf(stderr, "nxphys| %d: at %d %d %d going %d %d %d turned %d %d %d %d half %d %d %d spinning %d %d %d\n", i, (int)(b->pos[0] * 1000.0f),
				(int)(b->pos[1] * 1000.0f), (int)(b->pos[2] * 1000.0f), (int)(b->vel[0] * 1000.0f), (int)(b->vel[1] * 1000.0f), (int)(b->vel[2] * 1000.0f),
				(int)(b->q[0] * 10000.0f), (int)(b->q[1] * 10000.0f), (int)(b->q[2] * 10000.0f), (int)(b->q[3] * 10000.0f),
				(int)(sim.w.shapes[i].half[0] * 1000.0f), (int)(sim.w.shapes[i].half[1] * 1000.0f), (int)(sim.w.shapes[i].half[2] * 1000.0f),
				(int)(b->spin[0] * 1000.0f), (int)(b->spin[1] * 1000.0f), (int)(b->spin[2] * 1000.0f));
		}
	}
	for (s = 0; s < sim.nshares; s++)
		trouble += (int)sim.shares[s].trouble;
	phys_figures(&sim.w, buffer, &deepest, &fastest);
	if (getenv("PHYS_QSUM") && shown_buffer >= 0) {
		/* (a test's: the last picture's quads as the GPU has them, summed) */
		uint32_t quads = 0;

		for (i = 0; i < 16 * (FLOOR_QUADS + 6 * sim.w.bodies); i++)
			quads = quads * 31 + sim.quads[shown_buffer][i];
		fprintf(stderr, "physstat: quads %08x\n", (unsigned)quads);
	}
	for (k = 0; k < PHYS_HOLDS; k++) {
		const struct phys_hold *hold = &sim.w.holds[k];
		float at[3];

		if (hold->body < 0)
			continue;
		phys_point(&sim.w, buffer, hold->body, hold->at, at);
		fprintf(stderr, "nxphys: box %d's held point is %d mm from where it is wanted\n", hold->body, (int)(1000.0f * sqrtf(
			(at[0] - hold->to[0]) * (at[0] - hold->to[0]) + (at[1] - hold->to[1]) * (at[1] - hold->to[1]) +
			(at[2] - hold->to[2]) * (at[2] - hold->to[2]))));
	}
	for (k = 0; k < 2 && script_on; k++)
		if (scripted[k].n && scripted[k].box >= 0)
			fprintf(stderr, "nxphys: hand %d took box %d after %u substeps: its held point at most %d mm from where the hand wanted it (from 20 substeps on); "
				"let go after %u going %d mm/s, the hand %d mm/s\n", k, scripted[k].box, (unsigned)scripted[k].taken_at, scripted[k].worst,
				(unsigned)scripted[k].let_go_at, scripted[k].box_speed, scripted[k].hand_speed);
	if (script_on)
		fprintf(stderr, "nxphys: the deepest corner while the hands moved: %d mm\n", script_deepest);
	if (sim.count) {
		extern unsigned mcw_faults;	/* (pages a worker stopped at, each brought by this core) */

		fprintf(stderr, "physstat: the first core ended %u passes with nothing to do, in %u substeps; %u pages brought for workers\n",
			(unsigned)(waited - waited_before), (unsigned)sim.limit, mcw_faults);
	}
	if (sim.count && sim.costs) {
		/* what the time is made of: a substep is as long as its dearest share */
		uint32_t dearest[2] = {0, 0}, step;

		for (step = 0; step < sim.limit && step < sim.cost_row; step++) {
			uint32_t most = 0;

			for (s = 0; s < sim.nshares; s++)
				if (sim.costs[(uint32_t)s * sim.cost_row + step] > most)
					most = sim.costs[(uint32_t)s * sim.cost_row + step];
			dearest[step % PHYS_EVERY != 0] += most;
		}
		fprintf(stderr, "physstat: the dearest share's instructions, substep by substep: %u in all with a look, %u without; %d shares have the bodies, %d the grid and the pictures\n",
			(unsigned)dearest[0], (unsigned)dearest[1], sim.nbody, sim.nservice);
		for (k = 0; k < 2; k++)
			fprintf(stderr, "physstat: substeps %s a look by the passes they took, 0 to 7 and over: %u %u %u %u %u %u %u %u\n", k ? "without" : "with",
				(unsigned)passes_of[k][0], (unsigned)passes_of[k][1], (unsigned)passes_of[k][2], (unsigned)passes_of[k][3], (unsigned)passes_of[k][4],
				(unsigned)passes_of[k][5], (unsigned)passes_of[k][6], (unsigned)passes_of[k][7]);
	}
	if (sim.count) {
		uint32_t spent[3] = {0, 0, 0}, most = 0, looks = (sim.limit + PHYS_EVERY - 1) / PHYS_EVERY, plain = sim.limit - looks;

		mcw_counters_settle();
		for (k = 0; k < 3; k++)
			spent[k] = mcw_get(k);
		for (s = 0; s < sim.nshares; s++)
			if (sim.shares[s].most > most)
				most = sim.shares[s].most;
		fprintf(stderr, "physstat: a box costs %u instructions a grid, %u a substep with a look for contacts, %u a substep without (a share's longest: %u)\n",
			spent[0] / (looks ? looks : 1) / (uint32_t)sim.w.bodies, spent[1] / (looks ? looks : 1) / (uint32_t)sim.w.bodies,
			spent[2] / (plain ? plain : 1) / (uint32_t)sim.w.bodies, most);
	}
	fprintf(stderr, "physstat: done: %d boxes, %u substeps in %u ms, %d shares on %d workers: highest %d mm, deepest %d mm, fastest %d mm/s, "
		"%d without room, sum %08x; %u pictures\n", sim.w.bodies, (unsigned)sim.limit, took, sim.nshares, workers, (int)(top * 1000.0f),
		(int)(deepest * 1000.0f), (int)(fastest * 1000.0f), trouble, (unsigned)sum, pictures);
}

int
main(void)
{
	const char *text;
	GR_SCREEN_INFO info;
	uint32_t *table, steps = 0;
	int width = 640, height = 400, hold, print, frames = 0, asked = 0, cells, k;
	int pick_x = -1, pick_y = -1;
	uint32_t input = 0, test_at = 0;
	int test_kind = 0;
	unsigned hot_at = 0, drawn_at = 0;
	unsigned began, said_at, keys_at = 0, jobs_at = 0, asked_at = 0, retry_at = 0, made_said = 0, paced_at = 0;

	if ((text = getenv("PHYS_BODIES")) != NULL)
		bodies_wanted = atoi(text);
	if (bodies_wanted < 2 || bodies_wanted > MAX_BODIES)
		bodies_wanted = 400;
	if ((text = getenv("PHYS_SCENE")) != NULL && atoi(text) >= 1 && atoi(text) <= 4)
		scene = atoi(text);
	if ((text = getenv("PHYS_WORKERS")) != NULL)
		wanted = atoi(text);
	if ((text = getenv("PHYS_STEPS")) != NULL)
		steps = (uint32_t)atoi(text);
	sim.limit = steps ? steps : ~0u;
	hold = getenv("PHYS_HOLD") != NULL;
	print = getenv("PHYS_PRINT") != NULL;
	sim.count = getenv("PHYS_COUNT") != NULL;
	sim.spin = getenv("PHYS_SPIN") != NULL;
	sim.ahead = (text = getenv("PHYS_ORDER")) != NULL && !strcmp(text, "ahead");
	/* a contact found by a look has to do until the last substep before the next look's is
	 * made: two substeps and one to spare, or with the look one ahead three and none */
	sim.w.look_ahead = (PHYS_EVERY + 1) * PHYS_DT;
	sim.owners = getenv("PHYS_OWNERS") != NULL;
	if ((text = getenv("PHYS_PICK")) != NULL)
		sscanf(text, "%d,%d", &pick_x, &pick_y);
	talk = getenv("PHYS_SAY") != NULL || pick_x >= 0;
	seat_say = getenv("PHYS_SEAT") != NULL;
	for (k = 0; k < PHYS_HOLDS; k++)
		sim.w.holds[k].body = -1;
	if ((text = getenv("PHYS_HANDS")) != NULL && steps)
		script_read(text);
	if (script_on && script_first < steps)
		sim.limit = script_first;
	launch_old = getenv("PHYS_LAUNCH_OLD") != NULL;
	/* PHYS_LAUNCH=N:K (with PHYS_STEPS): act()'s K at substep N (the scene again at a look's) */
	if ((text = getenv("PHYS_LAUNCH")) != NULL && steps && sscanf(text, "%u:%d", &test_at, &test_kind) == 2 && test_at > 0 && test_at < steps) {
		if (test_kind == 1)
			test_at = (test_at + 1) & ~1u;
		launch_kind = launch_old ? 0 : test_kind, launch_at = test_at, launch_then = sim.limit;
		sim.limit = test_at;
	}
	if (GrOpen() < 0) {
		fprintf(stderr, "nxphys: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	if (width > info.cols - 20) width = (info.cols - 20) & ~3;
	if ((text = getenv("PHYS_SIZE")) != NULL)
		sscanf(text, "%dx%d", &width, &height);
	if (width < 160 || height < 100) width = 640, height = 400;
	if (height > info.rows - 60 - BAR) height = info.rows - 60 - BAR;
	ui_init();
	bar_y = height, wide = width;
	button_step = (wide - 10) / BUTTONS;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Physics", GR_ROOT_WINDOW_ID, -1, -1, width, height + BAR, UI_FACE);
	/* (not the pointer's moves: while the button is down its place is read from the machine's words) */
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_FOCUS_OUT | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	picture = GrNewWindow(window, 0, 0, width, height, 0, BLACK, BLACK);
	GrMapWindow(picture);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	draw_bar();
	if (seglInit(picture) < 0) {
		fprintf(stderr, "nxphys: this machine has no GPU\n");
		return 1;
	}
	sim.w.half_x = sim.w.half_z = ROOM;
	sim.w.gravity = 9.81f;
	sim.w.gx = sim.w.gz = (int)(2.0f * ROOM / PHYS_CELL);
	sim.w.gy = ROOM_HIGH;
	sim.w.cell_shares = 1;
	cells = sim.w.gx * sim.w.gy * sim.w.gz;
	total_quads = FLOOR_QUADS + 6 * MAX_BODIES;
	for (k = 0; k < PHYS_BUFFERS; k++) {
		sim.w.states[k] = mcw_alloc(MAX_BODIES * sizeof(struct body_state));
		sim.w.poses[k] = mcw_alloc(MAX_BODIES * sizeof(struct body_pose));
		sim.w.nears[k] = mcw_alloc(MAX_BODIES * sizeof(struct body_near));
	}
	sim.w.shapes = mcw_alloc(MAX_BODIES * sizeof(struct body_shape));
	sim.w.info = mcw_alloc(MAX_BODIES * sizeof(struct body_info));
	sim.w.manifolds = mcw_alloc(MAX_BODIES * PHYS_MANIFOLDS * sizeof(struct manifold));
	sim.w.grids[0] = mcw_alloc((unsigned)cells * sizeof(struct gcell));
	sim.w.grids[1] = mcw_alloc((unsigned)cells * sizeof(struct gcell));
	sim.shares = mcw_alloc(MC_MAX_CORES * sizeof(struct share));
	sim.colours = mcw_alloc(MAX_BODIES);
	sim.quads[0] = seglMemory((unsigned)total_quads * 64);
	sim.quads[1] = seglMemory((unsigned)total_quads * 64);
	table = seglMemory(COLOURS * SHADES * 4);
	if (!sim.w.states[0] || !sim.w.states[1] || !sim.w.states[2] || !sim.w.poses[0] || !sim.w.poses[1] || !sim.w.poses[2] || !sim.w.nears[0] || !sim.w.nears[1] || !sim.w.nears[2] || !sim.w.shapes || !sim.w.info || !sim.w.manifolds ||
	    !sim.w.grids[0] || !sim.w.grids[1] || !sim.shares || !sim.colours ||
	    !sim.quads[0] || !sim.quads[1] || !table || seglKeep(1, total_quads) < 0) {
		fprintf(stderr, "nxphys: no memory\n");
		return 1;
	}
	/* (the bodies' own memory when a scene says how many: bodies_there(); a picture's quads
	 * are written before they are drawn, and none past the bodies is) */
	there(sim.w.grids[0], (unsigned)cells * sizeof(struct gcell));
	there(sim.w.grids[1], (unsigned)cells * sizeof(struct gcell));
	there(sim.shares, MC_MAX_CORES * sizeof(struct share));
	if (sim.count && steps && steps <= 4096) {
		/* (a test's: every share's every substep, a row of whole texels a share) */
		sim.cost_row = (steps + 3) & ~3u;
		sim.costs = mcw_alloc(MC_MAX_CORES * sim.cost_row * 4);
		if (sim.costs)
			there(sim.costs, MC_MAX_CORES * sim.cost_row * 4);
	}
	make_table(table);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(table, SHADES, COLOURS, GL_RGBA);
	seglWorldUp(2);
	set_view();

	atexit(mcw_close);
	set_scene(0);
	take_workers();
	fprintf(stderr, "nxphys: %d boxes, %d shares on %d workers\n", sim.w.bodies, sim.nshares, workers);
	start_jobs();
	began = said_at = now_ms();
	pictures = 0;
	waited_before = noted_at = waited;
	if (pick_x >= 0) {
		/* a test: the box at that pixel held from the start, its point wanted three metres up */
		press(pick_x, pick_y, 1);
		sim.w.holds[0].to[1] += 3.0f;
		held_for_good = 1;
	}
	for (;;) {
		unsigned now = now_ms();
		int did = stopped ? 0 : own_shares(), again = 0;

		if (sim.costs)
			note_substeps();
		/* a test's hands: the shares stop at each of its substeps, and go on when the hands are told */
		if (script_on && sim.limit < steps && phase_least() >= sim.limit) {
			script_apply(sim.limit);
			sim.limit = sim.limit < script_last && sim.limit + 1 < steps ? sim.limit + 1 : steps;
		}

		/* as fast as time goes and no faster; what was not made in its time is not owed */
		if (!steps && !stopped) {
			unsigned due = (now - paced_at) * PHYS_HZ / 1000u;

			if (sim.limit == ~0u) {
				sim.limit = phase_least();
				paced_at = now;
			} else if (due) {
				uint32_t least = phase_least(), limit = sim.limit + due;

				sim.limit = limit > least + 4 ? least + 4 : limit;
				paced_at += due * 1000u / PHYS_HZ;
			}
			if (launch_kind && sim.limit > launch_at)
				sim.limit = launch_at;
		}
		/* a launch once every share waits at its substep and no picture is being made */
		if (launch_kind && !asked && phase_least() >= launch_at) {
			if (talk)
				fprintf(stderr, "nxphys: launched at substep %u\n", (unsigned)launch_at);
			launch();
			asked_at = now - 1000;	/* (its picture at once) */
		}
		if (asked && picture_made()) {
			show_picture(0);
			frames++;
			asked = 0;
		}
		/* a picture is asked for when the last has been shown, of a newer substep: with shares
		 * of the pictures as often as the boxes' time goes, else 25 a second */
		if (!asked && !stopped && (sim.nservice ? now - asked_at >= 1000u / PHYS_HZ && picture_step() != sim.show_step :
		    now - asked_at >= 40 && phase_least() < sim.limit)) {
			ask_picture();
			asked = 1;
			asked_at = now;
		}
		/* (the window system is asked when the button or key words change and for a while
		 * after, the event being on its way; else a few times a second: docs/machine/input.md) */
		if (input != (*control(0x28) ^ *control(0x2c) << 8)) {
			input = *control(0x28) ^ *control(0x2c) << 8;
			hot_at = now;
		}
		if (now - keys_at >= 150 || now - hot_at < 120) {
			int was = stopped;

			keys_at = now;
			again = events();
			if (stopped != was && !again) {
				if (stopped)
					settle();
				else
					start_jobs(), paced_at = now;
			}
			if (stopped != was || again == 2)	/* (a launch changes nothing of the bar) */
				draw_bar();
		}
		pointer();
		motion(now);
		if (wanted_again) {
			if (!again)
				again = wanted_again;
			wanted_again = 0;
		}
		if (view_moved && now - drawn_at >= 30) {
			show_picture(1);
			drawn_at = now;
		}
		if (now - jobs_at >= 50) {
			jobs_at = now;
			look_at_jobs();
		}
		/* a worker that lost its core is asked for again now and then */
		if (!stopped && now - retry_at >= 1000) {
			retry_at = now;
			start_jobs();
		}
		if (test_kind && launch_old && !asked && phase_least() >= test_at)
			again = test_kind, test_kind = 0;
		if (again && again != 2 && !stopped && !launch_old && !sim.ahead) {
			/* (the shares keep their jobs: a box thrown, fired or the scene again at a substep) */
			launch_ask(again);
			again = 0;
		}
		if (again) {
			settle();
			begin_again();
			launch_kind = 0, launch_at = 0;
			for (k = 0; k < PHYS_HOLDS; k++)
				hold_drop(k);
			if (again == 1)
				shown_buffer = -1;
			if (again == 1)
				sim.base = 0, set_scene(0);
			if (again == 3)
				throw_box((int)(sim.base % PHYS_BUFFERS));
			if (again == 4) {
				fire_box((int)(sim.base % PHYS_BUFFERS));
				if (talk)
					fprintf(stderr, "nxphys: a box fired from %d %d %d\n", (int)(fire_from[0] * 1000.0f), (int)(fire_from[1] * 1000.0f),
						(int)(fire_from[2] * 1000.0f));
			}
			if (again == 2)
				take_workers();
			else if (again == 1)
				share_out();
			else
				set_shares(sim.nshares);
			draw_bar();
			asked = 0;
			stopped = 0;
			sim.limit = !steps ? ~0u : steps > sim.base ? steps - sim.base : 1;
			start_jobs();
			began = said_at = now_ms();
			made_said = 0;
			frames = 0;
			continue;
		}
		if (steps && !asked && sim.base + sim.limit >= steps && sim.shares[0].phase >= sim.limit && phase_least() >= sim.limit) {
			/* a test: so many substeps, the last picture, and what the boxes come to */
			unsigned took = now_ms() - began;

			ask_picture();
			while (!picture_made()) {
				look_at_jobs();
				if (!own_shares())
					pass();
			}
			show_picture(0);
			settle();
			say_done(took, print);
			if (!hold)
				return 0;
			fprintf(stderr, "nxphys: holding\n");
			steps = 0;
			stopped = 1;
		}
		if (now - said_at >= 1000 && !stopped) {
			uint32_t made = phase_least();
			unsigned span = now - said_at, rate = (made - made_said) * 1000u / span;

			snprintf(status, sizeof status, "%d boxes on %d workers: %u substeps a second, %u.%02u of real time, %u pictures a second", sim.w.bodies,
				workers, rate, rate / PHYS_HZ, rate % PHYS_HZ * 100 / PHYS_HZ, (unsigned)frames * 1000u / span);
			draw_bar();
			if (talk)
				fprintf(stderr, "physstat: %d boxes, %d shares on %d workers: %u substeps a second, %u pictures a second\n", sim.w.bodies, sim.nshares,
				workers, rate, (unsigned)frames * 1000u / span);
			said_at = now;
			made_said = made;
			frames = 0;
		}
		if (!did)
			pass();
	}
}
