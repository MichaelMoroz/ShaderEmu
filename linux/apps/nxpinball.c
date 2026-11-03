/*
 * nxpinball: a pinball table: two flippers, a plunger, three pop bumpers, two slingshots, three
 * rollover lanes, a bank of drop targets and an orbit round the top; three balls a game, the best
 * score the registry's (nxpinball.high). PINBALL_DEMO=N plays N frames itself and prints a sum.
 * A controller plays it (docs/machine/input.md, "Game controls"; ui_pad.h): in a headset each hand's trigger
 * is its flipper and a grip or the jump button (A) the plunger; with a mouse the left and right
 * buttons on the table are the flippers and a side button the plunger. NXPINBALL_PRINT=1 says each.
 */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_pad.h"
#include <math.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#define TABLE_W 300			/* the table in its own units, y downwards; S pixels a unit */
#define TABLE_H 480
#define BALL_R 7.0f
#define LANE_X 278.0f			/* the plunger's lane, right of the wall at x 266 */
#define PLUNGER_Y 458.0f		/* the plunger's top at rest */
#define PULL_MOST 20.0f
#define PULL_SPEED 25.0f		/* units a second the plunger is drawn back while held */
#define LAUNCH_MIN 300.0f
#define LAUNCH_PER 40.0f		/* launch speed a unit of pull */
#define GRAVITY 520.0f
#define MOST_SPEED 1500.0f
#define STEP_HZ 120
#define STEP (1.0f / STEP_HZ)
#define STEP_US (1000000 / STEP_HZ)
#define FRAME_US 33333
#define FLIP_LEN 44.0f
#define FLIP_R0 7.0f
#define FLIP_R1 4.0f
#define FLIP_UP 26.0f			/* radians a second, up and back down */
#define FLIP_DOWN 16.0f
#define FLIP_E 0.3f
#define BUMPER_KICK 300.0f
#define SLING_KICK 260.0f
#define BALLS 3
#define NF 14				/* the flippers' angles drawn */
#define MENU_H 20
#define PAD 8
#define PANEL_W 150
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_FOCUS_OUT)
#define FONT_BIG "/usr/share/font-ui-b-17.sef"
#define PI 3.14159265f

enum { READY, PLAYING, PAUSED, OVER };
enum { S_WALL, S_SLING, S_TARGET, S_GATE };

/* ---- the table ---- */

struct seg { float ax, ay, bx, by, pad, e, nx, ny, il2, x0, y0, x1, y1; int kind, id; };
static struct seg segs[64];
static int nsegs;
static const float bumper_xy[3][2] = { { 104, 138 }, { 176, 138 }, { 140, 186 } };
#define BUMPER_R 16.0f
static const float lane_x[3] = { 106, 140, 174 };
static const float post_x[4] = { 89, 123, 157, 191 };
#define TARGET_X 261.0f
#define TARGET_Y(i) (196.0f + 24 * (i))
#define TARGET_H 18.0f
#define MULT_X(i) (99.0f + 27 * (i))
#define MULT_Y 304.0f

struct flipper { float px, py, rest, up, angle, omega, c, s; int held; };
static struct flipper flip[2] = {
	{ 82, 414, 0.50f, -0.45f },
	{ 194, 414, PI - 0.50f, PI + 0.45f },
};

/* the game */
static int state, score, high, ball_no, mult, bonus, ticks, games;
static float bx, by, bvx, bvy, pull;
static int on_plunger, charging, launch_at, orbit_at;
static int lane_lit[3], lane_in[3], target_up[3], targets_down_at, bumper_until[3];
static char note[2][40];
static int note_until, note_gen;
static int keys_held[3];		/* a bit for each key held: left flipper, right flipper, plunger */
static int demo;

/* the window */
static GR_WINDOW_ID window, pix;
static GR_FONT_ID big;
static struct ui_widths big_widths;
static float S = 1;			/* pixels a unit of the table */
static int TW, TH, TX, TY, PX, width, height, pix_w, pix_h;
static int menu_x[3], button_x, button_y, button_w = 96, button_h = 24;
static int overlay_shown = -2, ball_shown, ball_rect[4];
static int score_shown = -1, ball_no_shown = -1, bonus_shown = -1, mult_shown = -1, high_shown = -1, note_shown = -1;

/* What can change on the table, each a rectangle drawn in all its looks into the pixmap below
 * the table's picture: one copy shows one. */
enum { R_FLIP0, R_FLIP1, R_BUMP0, R_LANE0 = R_BUMP0 + 3, R_TARGET0 = R_LANE0 + 3, R_MULT0 = R_TARGET0 + 3,
	R_PLUNGER = R_MULT0 + 4, REGIONS };
struct region { int x, y, w, h, n, shown; short ax[NF], ay[NF]; };
static struct region regions[REGIONS];

static long long
now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void
seg_add(float ax, float ay, float bx_, float by_, float pad, float e, int kind, int id)
{
	struct seg *s = &segs[nsegs++];
	float dx = bx_ - ax, dy = by_ - ay, l = sqrtf(dx * dx + dy * dy), rr = pad + BALL_R;

	s->ax = ax, s->ay = ay, s->bx = bx_, s->by = by_, s->pad = pad, s->e = e, s->kind = kind, s->id = id;
	s->nx = dy / l, s->ny = -dx / l, s->il2 = 1 / (l * l);
	s->x0 = (ax < bx_ ? ax : bx_) - rr, s->x1 = (ax > bx_ ? ax : bx_) + rr;
	s->y0 = (ay < by_ ? ay : by_) - rr, s->y1 = (ay > by_ ? ay : by_) + rr;
}

static void
make_table(void)
{
	int i;

	nsegs = 0;
	for (i = 0; i < 24; i++) {
		float a0 = PI + PI * i / 24, a1 = PI + PI * (i + 1) / 24;

		seg_add(150 + 140 * cosf(a0), 150 + 140 * sinf(a0), 150 + 140 * cosf(a1), 150 + 140 * sinf(a1), 0, 0.5f, S_WALL, 0);
	}
	seg_add(10, 150, 10, 480, 0, 0.5f, S_WALL, 0);
	seg_add(290, 150, 290, 480, 0, 0.5f, S_WALL, 0);
	seg_add(266, 130, 266, 480, 2, 0.5f, S_WALL, 0);	/* the plunger's lane */
	seg_add(258, 132, 290, 108, 1, 0.4f, S_GATE, 0);	/* lets the ball out of the lane, not back in */
	seg_add(10, 232, 40, 268, 2, 0.5f, S_WALL, 0);		/* the two deflectors over the outlanes */
	seg_add(266, 272, 232, 300, 2, 0.5f, S_WALL, 0);
	seg_add(36, 320, 36, 480, 2, 0.5f, S_WALL, 0);		/* the guides between outlane and inlane */
	seg_add(36, 378, 74, 404, 2, 0.5f, S_WALL, 0);
	seg_add(240, 320, 240, 480, 2, 0.5f, S_WALL, 0);
	seg_add(240, 378, 202, 404, 2, 0.5f, S_WALL, 0);
	seg_add(56, 322, 80, 384, 2, 0.6f, S_SLING, 0);		/* the slingshots: their kicking side first */
	seg_add(56, 322, 56, 366, 2, 0.5f, S_WALL, 0);
	seg_add(56, 366, 80, 384, 2, 0.5f, S_WALL, 0);
	seg_add(196, 384, 220, 322, 2, 0.6f, S_SLING, 1);
	seg_add(220, 322, 220, 366, 2, 0.5f, S_WALL, 0);
	seg_add(220, 366, 196, 384, 2, 0.5f, S_WALL, 0);
	for (i = 0; i < 4; i++)
		seg_add(post_x[i], 56, post_x[i], 80, 2.5f, 0.5f, S_WALL, 0);
	for (i = 0; i < 3; i++)
		seg_add(TARGET_X, TARGET_Y(i), TARGET_X, TARGET_Y(i) + TARGET_H, 2, 0.4f, S_TARGET, i);
}

/* ---- the game ---- */

static void
say(const char *first, const char *second)
{
	snprintf(note[0], sizeof note[0], "%.39s", first);
	snprintf(note[1], sizeof note[1], "%.39s", second ? second : "");
	note_until = ticks + 2 * STEP_HZ, note_gen++;
}

static void
number(char *to, int room, int value)
{
	char digits[16];
	int n = snprintf(digits, sizeof digits, "%d", value), i, at = 0;

	for (i = 0; i < n && at < room - 2; i++) {
		if (i && (n - i) % 3 == 0)
			to[at++] = ',';
		to[at++] = digits[i];
	}
	to[at] = 0;
}

static void
award(int points, const char *what)
{
	char text[24];

	score += points;
	if (what) {
		number(text, sizeof text, points);
		say(what, text);
	}
}

static void
ball_to_plunger(void)
{
	on_plunger = 1, bx = LANE_X, by = PLUNGER_Y + pull - BALL_R, bvx = bvy = 0;
}

static void
new_game(void)
{
	int i;

	score = 0, ball_no = 1, mult = 1, bonus = 0, orbit_at = 0, targets_down_at = 0;
	for (i = 0; i < 3; i++)
		lane_lit[i] = 0, target_up[i] = 1, bumper_until[i] = 0;
	for (i = 0; i < 2; i++)
		flip[i].held = 0;
	charging = 0, pull = 0;
	ball_to_plunger();
	state = PLAYING;
	say("Ball 1", "Hold Space, let go");
}

static int new_high;	/* this game's score beat the old one (a tie does not) */

static void
game_over(void)
{
	char text[24];

	state = OVER;
	charging = 0;
	new_high = score > high;
	if (score > high) {
		high = score;
		if (!demo)
			reg_set_int("nxpinball.high", high);
		number(text, sizeof text, score);
		say("A new high score", text);
	} else
		say("Game over", NULL);
}

static void
lose_ball(void)
{
	char a[24], b[24], text[64];

	number(a, sizeof a, bonus);
	number(b, sizeof b, bonus * mult);
	snprintf(text, sizeof text, "%s x %d = %s", a, mult, b);
	score += bonus * mult;
	bonus = 0, mult = 1;
	if (++ball_no > BALLS) {
		ball_no = BALLS;
		game_over();
		return;
	}
	pull = 0;
	ball_to_plunger();
	snprintf(a, sizeof a, "Ball %d  bonus", ball_no);
	say(a, text);
}

/* The ball pushed out of a surface along n and bounced off it, the surface moving at (sx, sy);
 * returns how fast it came in (0 when it was leaving). */
static float
bounce(float nx, float ny, float depth, float sx, float sy, float e)
{
	float rx = bvx - sx, ry = bvy - sy, vn = rx * nx + ry * ny, t;

	bx += nx * depth, by += ny * depth;
	if (vn >= 0)
		return 0;
	rx -= (1 + e) * vn * nx, ry -= (1 + e) * vn * ny;
	if (vn < -30) {		/* a bounce, not rolling: a little of the speed along it goes */
		t = 0.03f * (ry * nx - rx * ny);
		rx += t * ny, ry -= t * nx;
	}
	bvx = rx + sx, bvy = ry + sy;
	return -vn;
}

static void
hit_seg(struct seg *s)
{
	float rr = BALL_R + s->pad, dx, dy, t, d2, d, nx, ny, in;

	if (bx < s->x0 || bx > s->x1 || by < s->y0 || by > s->y1)
		return;
	t = ((bx - s->ax) * (s->bx - s->ax) + (by - s->ay) * (s->by - s->ay)) * s->il2;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	dx = bx - (s->ax + (s->bx - s->ax) * t), dy = by - (s->ay + (s->by - s->ay) * t);
	d2 = dx * dx + dy * dy;
	if (d2 >= rr * rr)
		return;
	if (s->kind == S_GATE && (bx - s->ax) * s->nx + (by - s->ay) * s->ny < 0)
		return;
	if (s->kind == S_TARGET && !target_up[s->id])
		return;
	d = sqrtf(d2);
	if (d < 0.001f)
		nx = s->nx, ny = s->ny;
	else
		nx = dx / d, ny = dy / d;
	in = bounce(nx, ny, rr - d, 0, 0, s->e);
	if (s->kind == S_SLING && in > 40 && nx * s->nx + ny * s->ny > 0.9f) {
		bvx += s->nx * SLING_KICK, bvy += s->ny * SLING_KICK;
		score += 10;
	} else if (s->kind == S_TARGET && in > 60) {
		target_up[s->id] = 0;
		bonus += 500;
		if (!target_up[0] && !target_up[1] && !target_up[2]) {
			targets_down_at = ticks;
			bonus += 2000;
			award(5000, "Targets down");
		} else
			award(750, NULL);
	}
}

static void
hit_bumper(int i)
{
	float dx = bx - bumper_xy[i][0], dy = by - bumper_xy[i][1], rr = BUMPER_R + BALL_R, d2 = dx * dx + dy * dy, d, nx, ny, vn;

	if (d2 >= rr * rr)
		return;
	d = sqrtf(d2);
	nx = d > 0.001f ? dx / d : 0, ny = d > 0.001f ? dy / d : -1;
	bounce(nx, ny, rr - d, 0, 0, 0.4f);
	vn = bvx * nx + bvy * ny;
	bvx += nx * (BUMPER_KICK - (vn < 0 ? vn : 0)), bvy += ny * (BUMPER_KICK - (vn < 0 ? vn : 0));
	if (bumper_until[i] < ticks)
		score += 100, bonus += 10;
	bumper_until[i] = ticks + STEP_HZ / 8;
}

/* A flipper is a capsule thinning from pivot to tip; where it touches it moves at omega x r. */
static void
hit_flipper(struct flipper *f)
{
	float dx = bx - f->px, dy = by - f->py, reach = FLIP_LEN + FLIP_R0 + BALL_R, t, ex, ey, rad, d2, d, nx, ny, rx, ry;

	if (dx * dx + dy * dy > reach * reach)
		return;
	t = (dx * f->c + dy * f->s) / FLIP_LEN;
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	ex = dx - f->c * FLIP_LEN * t, ey = dy - f->s * FLIP_LEN * t;
	rad = FLIP_R0 + (FLIP_R1 - FLIP_R0) * t + BALL_R;
	d2 = ex * ex + ey * ey;
	if (d2 >= rad * rad)
		return;
	d = sqrtf(d2);
	nx = d > 0.001f ? ex / d : -f->s, ny = d > 0.001f ? ey / d : f->c;
	rx = dx - nx * BALL_R, ry = dy - ny * BALL_R;
	bounce(nx, ny, rad - d, -f->omega * ry, f->omega * rx, FLIP_E);
}

static void
flippers_move(float h)
{
	int i;

	for (i = 0; i < 2; i++) {
		struct flipper *f = &flip[i];
		float to = f->held ? f->up : f->rest, was = f->angle;

		if (f->angle == to) {
			f->omega = 0;
			continue;
		}
		f->omega = (to > f->angle ? 1 : -1) * (f->held ? FLIP_UP : FLIP_DOWN);
		f->angle += f->omega * h;
		if ((f->omega > 0 && f->angle > to) || (f->omega < 0 && f->angle < to))
			f->angle = to;
		if (f->angle != was)
			f->c = cosf(f->angle), f->s = sinf(f->angle);
	}
}

/* What the ball passes over: the lanes, the orbit, the drain, the plunger. */
static void
sensors(void)
{
	int i, inside;

	if (!on_plunger && bx > 268 && by >= PLUNGER_Y - BALL_R && bvy >= 0) {
		pull = 0;
		ball_to_plunger();
	}
	for (i = 0; i < 3; i++) {
		inside = bx > lane_x[i] - 12 && bx < lane_x[i] + 12 && by > 58 && by < 78;
		if (inside && !lane_in[i] && !lane_lit[i]) {
			lane_lit[i] = 1;
			score += 500, bonus += 250;
			if (lane_lit[0] && lane_lit[1] && lane_lit[2]) {
				char text[24];

				lane_lit[0] = lane_lit[1] = lane_lit[2] = 0;
				mult = mult < 5 ? mult + 1 : 5;
				score += 5000, bonus += 1000;
				snprintf(text, sizeof text, "Bonus x %d", mult);
				say("Lanes lit", text);
			}
		}
		lane_in[i] = inside;
	}
	if (bx > 210 && by < 95 && bvx < 0)
		orbit_at = ticks;
	if (orbit_at && bx < 70 && by < 150 && bvy > 0) {
		if (ticks - orbit_at < 2 * STEP_HZ && ticks - launch_at > 5 * STEP_HZ / 2) {
			bonus += 1000;
			award(2500, "Orbit");
		}
		orbit_at = 0;
	}
	if (targets_down_at && ticks - targets_down_at > 3 * STEP_HZ / 2)
		target_up[0] = target_up[1] = target_up[2] = 1, targets_down_at = 0;
	if (note_until && ticks >= note_until)
		note[0][0] = note[1][0] = 0, note_until = 0, note_gen++;
	if (by > TABLE_H - BALL_R && bx < 266)
		lose_ball();
}

/* A step of the table's time; the ball's moves are cut so that none is longer than 2.5 units. */
static void
tick(void)
{
	float speed = sqrtf(bvx * bvx + bvy * bvy), h;
	int i, k, n;

	ticks++;
	if (charging && pull < PULL_MOST)
		pull = pull + PULL_SPEED * STEP < PULL_MOST ? pull + PULL_SPEED * STEP : PULL_MOST;
	if (on_plunger)
		ball_to_plunger();
	for (i = 0; i < 2; i++)
		if (flip[i].angle != (flip[i].held ? flip[i].up : flip[i].rest))
			speed += FLIP_LEN * FLIP_UP;
	n = 1 + (int)(speed * STEP / 2.5f);
	n = n > 16 ? 16 : n;
	h = STEP / n;
	for (k = 0; k < n; k++) {
		flippers_move(h);
		if (on_plunger)
			continue;
		bvy += GRAVITY * h;
		bx += bvx * h, by += bvy * h;
		for (i = 0; i < nsegs; i++)
			hit_seg(&segs[i]);
		for (i = 0; i < 3; i++)
			hit_bumper(i);
		hit_flipper(&flip[0]);
		hit_flipper(&flip[1]);
		speed = bvx * bvx + bvy * bvy;
		if (speed > MOST_SPEED * MOST_SPEED) {
			speed = MOST_SPEED / sqrtf(speed);
			bvx *= speed, bvy *= speed;
		}
	}
	bvx *= 0.9995f, bvy *= 0.9995f;
	sensors();
}

static void
flipper_key(int side, int down)
{
	int t;

	if (state != PLAYING || flip[side].held == down)
		return;
	flip[side].held = down;
	if (down) {	/* the lit lanes move the way the flipper does */
		if (side)
			t = lane_lit[2], lane_lit[2] = lane_lit[1], lane_lit[1] = lane_lit[0], lane_lit[0] = t;
		else
			t = lane_lit[0], lane_lit[0] = lane_lit[1], lane_lit[1] = lane_lit[2], lane_lit[2] = t;
	}
}

static void
plunger_key(int down)
{
	if (state != PLAYING)
		return;
	if (down) {
		charging = on_plunger;
		return;
	}
	if (charging && on_plunger) {
		on_plunger = 0;
		bvx = 0, bvy = -(LAUNCH_MIN + LAUNCH_PER * pull);
		launch_at = ticks;
	}
	charging = 0, pull = 0;
}

/* ---- drawing ---- */

static GR_DRAW_ID dd;		/* where the table's art goes, the table's corner at (dox, doy) */
static int dox, doy;

static int
px(float v)
{
	return (int)(v * S + (v >= 0 ? 0.5f : -0.5f));
}

static void
d_rect(float x, float y, float w, float h, GR_COLOR colour)
{
	int x0 = px(x), y0 = px(y);

	ui_fill(dd, dox + x0, doy + y0, px(x + w) - x0, px(y + h) - y0, colour);
}

static void
d_circle(float x, float y, float r, GR_COLOR colour)
{
	int pr = px(r) > 0 ? px(r) : 1;

	GrSetGCForeground(ui_gc, colour);
	GrFillEllipse(dd, ui_gc, dox + px(x), doy + px(y), pr, pr);
}

static void
d_poly(const float *xy, int n, GR_COLOR colour)
{
	GR_POINT p[40];
	int i;

	for (i = 0; i < n; i++)
		p[i].x = dox + px(xy[2 * i]), p[i].y = doy + px(xy[2 * i + 1]);
	GrSetGCForeground(ui_gc, colour);
	GrFillPoly(dd, ui_gc, n, p);
}

/* A line `w` units wide with round ends. */
static void
d_line(float ax, float ay, float bx_, float by_, float w, GR_COLOR colour)
{
	float dx = bx_ - ax, dy = by_ - ay, l = sqrtf(dx * dx + dy * dy), nx = -dy / l * w / 2, ny = dx / l * w / 2;
	float q[8] = { ax + nx, ay + ny, bx_ + nx, by_ + ny, bx_ - nx, by_ - ny, ax - nx, ay - ny };

	d_poly(q, 4, colour);
	d_circle(ax, ay, w / 2, colour);
	d_circle(bx_, by_, w / 2, colour);
}

static int
text_width(const char *text, int is_big)
{
	return is_big ? ui_width(&big_widths, big, text, -1) : ui_width(&ui_var_widths, ui_var, text, -1);
}

static void
d_text(float x, float y, const char *text, GR_COLOR colour, int is_big)
{
	GrSetGCFont(ui_gc, is_big ? big : ui_var);
	GrSetGCForeground(ui_gc, colour);
	GrText(dd, ui_gc, dox + px(x) - text_width(text, is_big) / 2, doy + px(y) - (ui_smooth ? 1 : 0), (void *)text,
		(int)strlen(text), GR_TFASCII | GR_TFTOP);
}

#define CHROME MWRGB(176, 184, 204)

static void
arc_points(float *xy)
{
	int i;

	for (i = 0; i <= 24; i++)
		xy[2 * i] = 150 + 140 * cosf(PI + PI * i / 24), xy[2 * i + 1] = 150 + 140 * sinf(PI + PI * i / 24);
}

/* The table with nothing on it that changes. */
static void
art_base(void)
{
	float xy[60], tri[6];
	unsigned seed = 7;
	int i, side;

	d_rect(0, 0, TABLE_W, TABLE_H, MWRGB(34, 26, 52));
	arc_points(xy);
	xy[50] = 290, xy[51] = 480, xy[52] = 10, xy[53] = 480;
	d_poly(xy, 27, MWRGB(14, 30, 78));
	d_circle(140, 262, 92, MWRGB(20, 40, 98));
	d_circle(140, 262, 62, MWRGB(28, 52, 118));
	for (i = 0; i < 70; i++) {	/* stars */
		float x, y;

		seed = seed * 1103515245 + 12345, x = 16 + (seed >> 16) % 244;
		seed = seed * 1103515245 + 12345, y = 30 + (seed >> 16) % 280;
		if ((x - 150) * (x - 150) + (y - 150) * (y - 150) < 130 * 130 || y > 150)
			d_rect(x, y, i % 5 ? 1 : 2, i % 5 ? 1 : 2, i % 3 ? MWRGB(150, 160, 210) : MWRGB(240, 240, 255));
	}
	d_rect(268, 130, 21, 350, MWRGB(10, 22, 58));
	for (side = 0; side < 2; side++) {	/* the aprons under the guides */
		float m = side ? 276 : 0, k = side ? -1 : 1;
		float apron[8] = { m + k * 36, 378, m + k * 74, 404, m + k * 80, 480, m + k * 36, 480 };

		d_poly(apron, 4, MWRGB(70, 22, 46));
	}
	d_rect(256, 190, 10, 78, MWRGB(36, 36, 58));
	for (i = 0; i < 24; i++)
		d_line(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], 4, CHROME);
	for (i = 24; i < nsegs; i++)
		if (segs[i].kind != S_TARGET)
			d_line(segs[i].ax, segs[i].ay, segs[i].bx, segs[i].by, segs[i].kind == S_GATE ? 2 : segs[i].pad ? 2 * segs[i].pad : 4,
				segs[i].kind == S_GATE ? MWRGB(230, 200, 60) : segs[i].kind == S_SLING ? WHITE : CHROME);
	for (side = 0; side < 2; side++) {	/* the slingshots' faces */
		float m = side ? 276 : 0, k = side ? -1 : 1;

		tri[0] = m + k * 59, tri[1] = 331, tri[2] = m + k * 59, tri[3] = 364, tri[4] = m + k * 74, tri[5] = 375;
		d_poly(tri, 3, MWRGB(200, 40, 60));
	}
	for (i = 0; i < 2; i++) {	/* arrows to the orbit and the top lanes */
		float ax = i ? 246 : 140, ay = i ? 150 : 106;
		float arrow[6] = { ax, ay - 8, ax + 6, ay + 4, ax - 6, ay + 4 };

		d_poly(arrow, 3, MWRGB(255, 196, 60));
	}
	d_text(140, 228, "PINBALL", MWRGB(130, 176, 255), 1);
	d_text(140, 280, "BONUS", MWRGB(160, 190, 240), 0);
	d_text(246, 158, "ORBIT", MWRGB(255, 196, 60), 0);
}

static void
art_flipper(int side, int v)
{
	struct flipper *f = &flip[side];
	float a = f->rest + (f->up - f->rest) * v / (NF - 1), xy[56];
	int i, pass;

	for (pass = 0; pass < 2; pass++) {
		float grow = pass ? 0 : 1.5f;

		for (i = 0; i < 7; i++) {
			float b = a + PI / 2 + PI * i / 6, c = a - PI / 2 + PI * i / 6;

			xy[2 * i] = f->px + (FLIP_R0 + grow) * cosf(b), xy[2 * i + 1] = f->py + (FLIP_R0 + grow) * sinf(b);
			xy[14 + 2 * i] = f->px + FLIP_LEN * cosf(a) + (FLIP_R1 + grow) * cosf(c);
			xy[15 + 2 * i] = f->py + FLIP_LEN * sinf(a) + (FLIP_R1 + grow) * sinf(c);
		}
		d_poly(xy, 14, pass ? MWRGB(240, 240, 250) : MWRGB(210, 36, 50));
	}
	d_circle(f->px, f->py, 2.5f, MWRGB(120, 120, 140));
}

static void
art_bumper(int i, int lit)
{
	float x = bumper_xy[i][0], y = bumper_xy[i][1];

	d_circle(x, y, BUMPER_R, lit ? MWRGB(255, 230, 90) : MWRGB(190, 30, 50));
	d_circle(x, y, BUMPER_R - 3, WHITE);
	d_circle(x, y, BUMPER_R - 6, lit ? MWRGB(255, 255, 200) : MWRGB(40, 90, 200));
	d_circle(x, y, 4, lit ? MWRGB(255, 150, 40) : WHITE);
}

static void
art_region(int r, int v)
{
	int i;

	if (r <= R_FLIP1)
		art_flipper(r - R_FLIP0, v);
	else if (r < R_LANE0)
		art_bumper(r - R_BUMP0, v);
	else if (r < R_TARGET0) {
		i = r - R_LANE0;
		d_circle(lane_x[i], 96, 6, MWRGB(20, 20, 30));
		d_circle(lane_x[i], 96, 5, v ? MWRGB(255, 210, 40) : MWRGB(90, 70, 20));
	} else if (r < R_MULT0) {
		i = r - R_TARGET0;
		if (v)
			d_rect(TARGET_X + 2, TARGET_Y(i), 2, TARGET_H, MWRGB(90, 50, 20));
		else {
			d_rect(TARGET_X - 3, TARGET_Y(i), 6, TARGET_H, MWRGB(255, 140, 30));
			d_rect(TARGET_X - 3, TARGET_Y(i), 2, TARGET_H, MWRGB(255, 210, 140));
		}
	} else if (r < R_PLUNGER) {
		char text[4];

		i = r - R_MULT0;
		snprintf(text, sizeof text, "%dx", i + 2);
		d_circle(MULT_X(i), MULT_Y, 11, MWRGB(20, 20, 40));
		d_circle(MULT_X(i), MULT_Y, 10, v ? MWRGB(255, 90, 200) : MWRGB(50, 40, 90));
		d_text(MULT_X(i), MULT_Y - 7, text, v ? WHITE : MWRGB(130, 130, 170), 0);
	} else {
		float top = PLUNGER_Y + PULL_MOST * v / 7;

		d_rect(276, top + 4, 4, TABLE_H - top - 4, CHROME);
		d_rect(270, top, 16, 5, MWRGB(210, 36, 50));
		d_rect(270, top, 16, 1.5f, MWRGB(255, 140, 140));
	}
}

static int
region_want(int r)
{
	if (r <= R_FLIP1) {
		struct flipper *f = &flip[r - R_FLIP0];

		return (int)((f->angle - f->rest) / (f->up - f->rest) * (NF - 1) + 0.5f);
	}
	if (r < R_LANE0)
		return bumper_until[r - R_BUMP0] > ticks;
	if (r < R_TARGET0)
		return lane_lit[r - R_LANE0];
	if (r < R_MULT0)
		return !target_up[r - R_TARGET0];
	if (r < R_PLUNGER)
		return mult >= r - R_MULT0 + 2;
	return (int)(pull * 7 / PULL_MOST + 0.5f);
}

static void
region_box(int r, float x0, float y0, float x1, float y1, int n)
{
	struct region *g = &regions[r];

	g->x = px(x0) - 1, g->y = px(y0) - 1;
	g->w = px(x1) - px(x0) + 3, g->h = px(y1) - px(y0) + 3;
	g->x = g->x < 0 ? 0 : g->x, g->y = g->y < 0 ? 0 : g->y;
	g->w = g->x + g->w > TW ? TW - g->x : g->w, g->h = g->y + g->h > TH ? TH - g->y : g->h;
	g->n = n;
}

/* Where each region is and where its looks go in the pixmap, rows of them under the table. */
static void
regions_place(void)
{
	int r, v, i, x = 0, y = TH + 1, row = 0;

	for (i = 0; i < 2; i++) {
		struct flipper *f = &flip[i];
		float x0 = f->px, y0 = f->py, x1 = f->px, y1 = f->py, m = FLIP_R0 + 2;

		for (v = 0; v < NF; v++) {
			float a = f->rest + (f->up - f->rest) * v / (NF - 1), tx = f->px + FLIP_LEN * cosf(a), ty = f->py + FLIP_LEN * sinf(a);

			x0 = tx < x0 ? tx : x0, x1 = tx > x1 ? tx : x1, y0 = ty < y0 ? ty : y0, y1 = ty > y1 ? ty : y1;
		}
		region_box(R_FLIP0 + i, x0 - m, y0 - m, x1 + m, y1 + m, NF);
	}
	for (i = 0; i < 3; i++) {
		region_box(R_BUMP0 + i, bumper_xy[i][0] - BUMPER_R, bumper_xy[i][1] - BUMPER_R, bumper_xy[i][0] + BUMPER_R,
			bumper_xy[i][1] + BUMPER_R, 2);
		region_box(R_LANE0 + i, lane_x[i] - 6, 90, lane_x[i] + 6, 102, 2);
		region_box(R_TARGET0 + i, TARGET_X - 4, TARGET_Y(i), TARGET_X + 4, TARGET_Y(i) + TARGET_H, 2);
	}
	for (i = 0; i < 4; i++)
		region_box(R_MULT0 + i, MULT_X(i) - 11, MULT_Y - 11, MULT_X(i) + 11, MULT_Y + 11, 2);
	region_box(R_PLUNGER, 268, PLUNGER_Y - 1, 289, TABLE_H, 8);
	pix_w = 2 * TW;
	for (r = 0; r < REGIONS; r++)
		for (v = 0; v < regions[r].n; v++) {
			if (x + regions[r].w > pix_w)
				x = 0, y += row + 1, row = 0;
			regions[r].ax[v] = (short)x, regions[r].ay[v] = (short)y;
			x += regions[r].w + 1;
			row = regions[r].h > row ? regions[r].h : row;
		}
	pix_h = y + row;
}

/* The table's picture and every look of every region, drawn once into one pixmap. */
static void
art_make(void)
{
	int r, v;

	if (pix)
		GrDestroyWindow(pix);
	regions_place();
	pix = GrNewPixmap(pix_w, pix_h, NULL);
	dd = pix, dox = doy = 0;
	art_base();
	for (r = 0; r < REGIONS; r++)	/* (all the copies first: the table is drawn once before them) */
		for (v = 0; v < regions[r].n; v++)
			GrCopyArea(pix, ui_gc, regions[r].ax[v], regions[r].ay[v], regions[r].w, regions[r].h, pix, regions[r].x, regions[r].y, MWROP_COPY);
	for (r = 0; r < REGIONS; r++)
		for (v = 0; v < regions[r].n; v++) {
			dox = regions[r].ax[v] - regions[r].x, doy = regions[r].ay[v] - regions[r].y;
			art_region(r, v);
		}
	for (r = 0; r < REGIONS; r++)
		regions[r].shown = -1;
}

static void
show_region(int r, int v)
{
	struct region *g = &regions[r];

	GrCopyArea(window, ui_gc, TX + g->x, TY + g->y, g->w, g->h, pix, g->ax[v], g->ay[v], MWROP_COPY);
	g->shown = v;
}

static int
overlaps(const struct region *g, const int *b)
{
	return g->x < b[0] + b[2] && b[0] < g->x + g->w && g->y < b[1] + b[3] && b[1] < g->y + g->h;
}

static void
draw_ball(void)
{
	int cx = px(bx), cy = px(by), pr = px(BALL_R), hr = pr * 2 / 3, sr = pr / 3 > 1 ? pr / 3 : 1;

	GrSetGCForeground(ui_gc, MWRGB(110, 116, 134));
	GrFillEllipse(window, ui_gc, TX + cx, TY + cy, pr, pr);
	GrSetGCForeground(ui_gc, MWRGB(196, 200, 214));
	GrFillEllipse(window, ui_gc, TX + cx - pr / 4, TY + cy - pr / 4, hr, hr);
	GrSetGCForeground(ui_gc, WHITE);
	GrFillEllipse(window, ui_gc, TX + cx - pr / 3, TY + cy - pr / 3 - 1, sr, sr);
	ball_rect[0] = cx - pr - 1, ball_rect[1] = cy - pr - 1, ball_rect[2] = ball_rect[3] = 2 * pr + 3;
}

/* The table under where the ball was, the regions there in their looks of now. */
static void
ball_erase(void)
{
	int x = ball_rect[0] < 0 ? 0 : ball_rect[0], y = ball_rect[1] < 0 ? 0 : ball_rect[1], r;
	int x1 = ball_rect[0] + ball_rect[2] > TW ? TW : ball_rect[0] + ball_rect[2];
	int y1 = ball_rect[1] + ball_rect[3] > TH ? TH : ball_rect[1] + ball_rect[3];

	if (x1 > x && y1 > y)
		GrCopyArea(window, ui_gc, TX + x, TY + y, x1 - x, y1 - y, pix, x, y, MWROP_COPY);
	for (r = 0; r < REGIONS; r++)
		if (overlaps(&regions[r], ball_rect))
			show_region(r, regions[r].shown);
}

static int
ball_visible(void)
{
	return state != OVER;
}

static void
draw_table_all(void)
{
	int r;

	GrCopyArea(window, ui_gc, TX, TY, TW, TH, pix, 0, 0, MWROP_COPY);
	for (r = 0; r < REGIONS; r++)
		show_region(r, region_want(r));
	ball_shown = ball_visible();
	if (ball_shown)
		draw_ball();
}

/* Only what moved: the regions whose look changed and the ball. */
static void
draw_table(void)
{
	int r, v, again = 0, cx = px(bx), cy = px(by), pr = px(BALL_R), now[4];

	now[0] = cx - pr - 1, now[1] = cy - pr - 1, now[2] = now[3] = 2 * pr + 3;
	for (r = 0; r < REGIONS; r++)
		if ((v = region_want(r)) != regions[r].shown) {
			show_region(r, v);
			again |= (ball_shown && overlaps(&regions[r], ball_rect)) || overlaps(&regions[r], now);
		}
	if (!again && ball_shown == ball_visible() && (!ball_shown || (now[0] == ball_rect[0] && now[1] == ball_rect[1])))
		return;
	if (ball_shown)
		ball_erase();
	ball_shown = ball_visible();
	if (ball_shown)
		draw_ball();
}

#define LABEL (ui_seven ? MWRGB(0, 51, 153) : BLACK)

static void
text_at(int x, int y, const char *text, GR_COLOR colour, GR_FONT_ID font)
{
	GrSetGCFont(ui_gc, font);
	GrSetGCForeground(ui_gc, colour);
	GrText(window, ui_gc, x, y - (ui_smooth ? 1 : 0), (void *)text, (int)strlen(text), GR_TFASCII | GR_TFTOP);
}

/* The box over the table when no ball is in play: what to do, and a button that does it. */
static void
draw_overlay(void)
{
	const char *title = state == READY ? "Pinball" : state == PAUSED ? "Paused" : "Game over";
	const char *button = state == READY ? "Play" : state == PAUSED ? "Resume" : "Play again";
	char line[2][40], value[24];
	int w = TW - 60 < 200 ? TW - 60 : 200, h = 120, x = TX + (TW - w) / 2, y = TY + TH / 2 - h, i;

	if (state == READY)
		snprintf(line[0], 40, "Three balls a game"), snprintf(line[1], 40, "Press Space to start");
	else if (state == PAUSED)
		snprintf(line[0], 40, "P or Space goes on"), snprintf(line[1], 40, "F2 starts again");
	else {
		number(value, sizeof value, score);
		snprintf(line[0], 40, "Score %s", value);
		snprintf(line[1], 40, "%s", new_high ? "A new high score!" : "Press Space to play");
	}
	ui_fill(window, x, y, w, h, ui_seven ? WHITE : UI_FACE);
	if (ui_seven)
		ui_edges(window, x, y, w, h, MWRGB(120, 120, 120), MWRGB(120, 120, 120));
	else
		ui_bevel(window, x, y, w, h, 0);
	text_at(x + (w - text_width(title, 1)) / 2, y + 12, title, LABEL, big);
	for (i = 0; i < 2; i++)
		text_at(x + (w - text_width(line[i], 0)) / 2, y + 40 + i * 16, line[i], BLACK, ui_var);
	button_x = x + (w - button_w) / 2, button_y = y + h - button_h - 12;
	ui_button(window, button_x, button_y, button_w, button_h, "", 0);
	text_at(button_x + (button_w - text_width(button, 0)) / 2, button_y + (button_h - 12) / 2, button, BLACK, ui_var);
}

static void
draw_value(int y, const char *text)
{
	ui_fill(window, PX, y, PANEL_W, 22, UI_FACE);
	text_at(PX + PANEL_W - 4 - text_width(text, 1), y + 2, text, BLACK, big);
}

static void
draw_panel(void)
{
	static const char *const keys[] = { "Z, Left", "left flipper", "/, Right", "right flipper", "Space, Down", "plunger",
		"P, Esc", "pause", "F2", "new game" };
	int i, y = TY + TH - 5 * 15;

	text_at(PX, TY, "Score", LABEL, ui_var);
	text_at(PX, TY + 48, "Ball", LABEL, ui_var);
	text_at(PX, TY + 96, "Bonus", LABEL, ui_var);
	text_at(PX, TY + 144, "High score", LABEL, ui_var);
	ui_fill(window, PX, y - 8, PANEL_W, 1, UI_SHADOW);
	for (i = 0; i < 10; i += 2, y += 15) {
		text_at(PX, y, keys[i], BLACK, ui_var);
		text_at(PX + PANEL_W - text_width(keys[i + 1], 0), y, keys[i + 1], UI_SHADOW, ui_var);
	}
}

static void
draw_panel_changes(void)
{
	char text[40], value[24];
	int i;

	if (score != score_shown)
		number(text, sizeof text, score), draw_value(TY + 14, text), score_shown = score;
	if ((state == READY ? 0 : ball_no) != ball_no_shown) {
		snprintf(text, sizeof text, "%d of %d", ball_no, BALLS);
		draw_value(TY + 62, state == READY ? "" : text), ball_no_shown = state == READY ? 0 : ball_no;
	}
	if (bonus != bonus_shown || mult != mult_shown) {
		number(value, sizeof value, bonus);
		snprintf(text, sizeof text, "%s x %d", value, mult);
		draw_value(TY + 110, text), bonus_shown = bonus, mult_shown = mult;
	}
	if (high != high_shown)
		number(text, sizeof text, high), draw_value(TY + 158, text), high_shown = high;
	if (note_gen != note_shown) {
		ui_fill(window, PX, TY + 196, PANEL_W, 38, UI_FACE);
		for (i = 0; i < 2; i++)
			if (note[i][0])
				text_at(PX, TY + 196 + i * 19, note[i], i ? BLACK : LABEL, i ? ui_var : big);
		note_shown = note_gen;
	}
}

static void
draw_changes(void)
{
	int overlay = state == PLAYING ? -1 : state;

	if (overlay != overlay_shown) {
		draw_table_all();
		if (overlay >= 0)
			draw_overlay();
		overlay_shown = overlay;
	} else if (overlay < 0)
		draw_table();
	draw_panel_changes();
}

/* A sunk frame round the table. */
static void
frame(int x, int y, int w, int h)
{
	ui_fill(window, x - 1, y - 1, w + 2, h + 2, MWRGB(0, 0, 0));
	ui_bevel(window, x - 2, y - 2, w + 4, h + 4, 1);
}

static void
draw_all(void)
{
	int i;

	ui_fill(window, 0, 0, width, height, UI_FACE);
	for (i = 0; i < 2; i++)
		text_at(menu_x[i] + 8, 3, i ? "Help" : "Game", BLACK, ui_var);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
	frame(TX, TY, TW, TH);
	draw_panel();
	overlay_shown = -2;
	score_shown = ball_no_shown = bonus_shown = mult_shown = high_shown = note_shown = -1;
	draw_changes();
}

/* Sizes from the screen's: the table as tall as the screen leaves room for. */
static void
layout(void)
{
	const GR_SCREEN_INFO *screen = ui_screen();

	S = (float)(screen->rows - UI_MENU_BAR - 34 - MENU_H - 2 * PAD - 4) / TABLE_H;
	S = S > 1.5f ? 1.5f : S < 0.6f ? 0.6f : S;
	TW = px(TABLE_W), TH = px(TABLE_H);
	TX = PAD + 2, TY = MENU_H + PAD + 2;
	PX = TX + TW + PAD + 6;
	width = PX + PANEL_W + PAD, height = TY + TH + PAD + 2;
	menu_x[0] = 2;
	menu_x[1] = menu_x[0] + ui_width(&ui_var_widths, ui_var, "Game", -1) + 16;
	menu_x[2] = menu_x[1] + ui_width(&ui_var_widths, ui_var, "Help", -1) + 16;
}

static void
fonts(void)
{
	if (big)
		GrDestroyFont(big);
	big = GrCreateFontEx(ui_smooth ? FONT_BIG : GR_FONT_SYSTEM_VAR, 0, 0, NULL);
	big_widths.known = 0;
}

/* ---- PINBALL_DEMO: flippers pressed when the ball comes near them, the plunger held a while ---- */

static int demo_hold[3], demo_rest[2];

static void
demo_inputs(void)
{
	int side;

	if (demo_hold[2]) {
		if (--demo_hold[2] == 0)
			plunger_key(0);
	} else if (on_plunger) {
		plunger_key(1);
		demo_hold[2] = 10 + 5 * (ball_no % 3) + games % 4;
	}
	for (side = 0; side < 2; side++) {
		struct flipper *f = &flip[side];
		float dx = side ? f->px - bx : bx - f->px, dy = by - f->py;

		if (demo_hold[side]) {
			if (--demo_hold[side] == 0)
				flipper_key(side, 0), demo_rest[side] = 6;
		} else if (demo_rest[side])
			demo_rest[side]--;
		else if (!on_plunger && dx > -4 && dx < 50 && dy > -34 && dy < 18 && bvy > -40)
			flipper_key(side, 1), demo_hold[side] = 7;
	}
}

static unsigned
state_sum(void)
{
	unsigned h = 2166136261u, w[16];
	int i;

	memcpy(&w[0], &bx, 4), memcpy(&w[1], &by, 4), memcpy(&w[2], &bvx, 4), memcpy(&w[3], &bvy, 4);
	memcpy(&w[4], &flip[0].angle, 4), memcpy(&w[5], &flip[1].angle, 4);
	w[6] = (unsigned)score, w[7] = (unsigned)ball_no, w[8] = (unsigned)ticks, w[9] = (unsigned)games;
	w[10] = (unsigned)(lane_lit[0] | lane_lit[1] << 1 | lane_lit[2] << 2 | target_up[0] << 3 | target_up[1] << 4 | target_up[2] << 5);
	w[11] = (unsigned)mult, w[12] = (unsigned)bonus, w[13] = (unsigned)high, w[14] = (unsigned)on_plunger, w[15] = (unsigned)state;
	for (i = 0; i < 16; i++)
		h = (h ^ w[i]) * 16777619u;
	return h;
}

static void
demo_frame(void)
{
	int k;

	demo_inputs();
	for (k = 0; k < STEP_HZ * FRAME_US / 1000000 + 1 && state == PLAYING; k++)
		tick();
	draw_changes();
	GrFlush();
}

static void next_event(GR_EVENT *event, int ms);

/* PINBALL_HOLD=near or over: then on until the ball is near the flippers or the game is over,
 * and the window stays as it is (for a picture of it). */
static void
demo_run(int frames, const char *hold)
{
	GR_PIXELVAL pixel;
	GR_EVENT event;
	int f;

	draw_all();
	GrReadArea(window, 0, 0, 1, 1, &pixel);
	new_game();
	games = 1;
	printf("pinball: go\n");
	fflush(stdout);
	for (f = 0; f < frames; f++) {
		if (state == OVER)
			new_game(), games++;
		demo_frame();
	}
	GrReadArea(window, 0, 0, 1, 1, &pixel);
	printf("pinball: %d frames, %d games, ball %d, high %d, score %d state %08x\n", frames, games, ball_no, high, score, state_sum());
	fflush(stdout);
	if (hold) {
		while (state == PLAYING && (strcmp(hold, "near") || on_plunger || by < 380 || bvy < 0))
			demo_frame();
		printf("pinball: held at frame %d\n", f);
		fflush(stdout);
		for (;;) {
			next_event(&event, -1);
			if (event.type == GR_EVENT_TYPE_EXPOSURE && event.exposure.wid == window)
				draw_all();
			if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
				break;
		}
	}
	GrClose();
	exit(0);
}

/* ---- events ---- */

/* The next event, or a timeout: the window system is asked once and the wait is this
 * program's, on its socket (as nxtetris does). */
static GR_EVENT taken;
static int took;

static void
take(GR_EVENT *event)
{
	taken = *event;
	took = 1;
}

static void
next_event(GR_EVENT *event, int ms)
{
	static int asked, fd = -1;
	struct timeval limit;
	fd_set set;
	int most = 0;

	GrFlush();
	FD_ZERO(&set);
	took = 0;
	GrServiceSelect(&set, take);
	if (took)
		asked = 0;
	if (!took && !asked) {
		GrPrepareSelect(&most, &set);
		for (fd = most; fd > 0 && !FD_ISSET(fd, &set); fd--)
			continue;
		asked = 1;
	}
	if (!took) {
		FD_ZERO(&set);
		FD_SET(fd, &set);
		limit.tv_sec = ms / 1000, limit.tv_usec = ms % 1000 * 1000;
		if (select(fd + 1, &set, NULL, NULL, ms < 0 ? NULL : &limit) > 0) {
			GrServiceSelect(&set, take);
			asked = 0;
		}
	}
	if (took)
		*event = taken;
	else
		event->type = GR_EVENT_TYPE_TIMEOUT;
}

static long long tick_due, frame_due;

static void
pause_game(void)
{
	if (state == PLAYING)
		state = PAUSED;
	flip[0].held = flip[1].held = 0;
	charging = 0, pull = 0;
	memset(keys_held, 0, sizeof keys_held);
}

/* Play, go on, or play again. */
static void
go(void)
{
	if (state == READY || state == OVER)
		new_game();
	else if (state == PAUSED)
		state = PLAYING;
	tick_due = frame_due = now_us();
}

static const struct ui_menu_item game_items[] = {
	{ "New game   F2", -1, 1 }, { "Pause   P", -1, 2 }, { "Exit", -1, 3, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item help_items[] = { { "Keys", -1, 4 }, { "About Pinball", -1, 5 } };

static int menu_paused;	/* the menu open now paused the game */

static void
open_menu(int which, int keyboard)
{
	int x, y;

	menu_paused = state != PAUSED;
	pause_game();
	ui_window_origin(window, &x, &y);
	if (which)
		ui_menu_open(help_items, 2, x + menu_x[1], y + MENU_H, 0, keyboard);
	else
		ui_menu_open(game_items, 3, x + menu_x[0], y + MENU_H, 0, keyboard);
}

/* The window's own events while a question is asked. */
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

	switch (id) {
	case 1:
		new_game();
		tick_due = frame_due = now_us();
		break;
	case 2:
		/* (the menu paused it to show itself: Pause from there is to stay so) */
		if (state == PAUSED && !menu_paused)
			go();
		else
			pause_game();
		break;
	case 3:
		GrClose();
		exit(0);
	case 4:
		ui_ask("Keys", "Z, Left or left Ctrl: the left flipper\n/, Right or right Ctrl: the right flipper\n"
			"Space or Down: hold to draw the plunger back, let go to launch\nP or Escape: pause     F2: a new game     F10: the menus", ok, 1, others);
		break;
	case 5:
		ui_ask("About Pinball", "Pinball for the desktop: three balls a game.\nLight the three lanes for a bonus multiplier,\n"
			"drop the three targets, and send the ball\nround the orbit; the bonus counts at each drain.", ok, 1, others);
		break;
	}
	if (id >= 4)
		GrSetFocus(window);	/* (the question had the keyboard) */
}

/* Which control a key is: 0 the left flipper, 1 the right one, 2 the plunger; -1 none. */
static int
control_of(int ch)
{
	switch (ch) {
	case 'z': case MWKEY_LEFT: case MWKEY_LCTRL: case MWKEY_KP4: return 0;
	case '/': case MWKEY_RIGHT: case MWKEY_RCTRL: case MWKEY_KP6: return 1;
	case ' ': case MWKEY_DOWN: case MWKEY_KP2: return 2;
	}
	return -1;
}

static int
key_bit(int ch)
{
	return ch == 'z' || ch == '/' || ch == ' ' ? 1 : ch == MWKEY_LEFT || ch == MWKEY_RIGHT || ch == MWKEY_DOWN ? 2 : 4;
}

static void
key_down(int ch)
{
	int c;

	if (ch < 128)
		ch = tolower(ch);
	if (ch == MWKEY_F10) {
		open_menu(0, 1);
		return;
	}
	if (ch == MWKEY_F2) {
		command(1);
		return;
	}
	if (state != PLAYING) {
		if (ch == ' ' || ch == MWKEY_ENTER || ch == '\r' || ch == '\n' || (state == PAUSED && ch == 'p'))
			go();
		return;
	}
	if (ch == 'p' || ch == MWKEY_ESCAPE || ch == MWKEY_PAUSE) {
		pause_game();
		return;
	}
	if ((c = control_of(ch)) < 0 || keys_held[c] & key_bit(ch))
		return;	/* (the kernel's own repeat) */
	keys_held[c] |= key_bit(ch);
	if (c == 2)
		plunger_key(1);
	else
		flipper_key(c, 1);
}

static void
key_up(int ch)
{
	int c;

	if (ch < 128)
		ch = tolower(ch);
	if ((c = control_of(ch)) < 0 || !(keys_held[c] & key_bit(ch)))
		return;
	keys_held[c] &= ~key_bit(ch);
	if (keys_held[c])
		return;
	if (c == 2)
		plunger_key(0);
	else
		flipper_key(c, 0);
}

/* What holds a control besides the keys (key_bit's 1, 2, 4): the hands, a mouse's button. */
#define BY_HANDS 8
#define BY_MOUSE 16
static struct ui_pad pad;
static int pad_print;

static void
control_set(int c, int by, int down)
{
	int was = keys_held[c] != 0;

	keys_held[c] = down ? keys_held[c] | by : keys_held[c] & ~by;
	if ((keys_held[c] != 0) == was)
		return;
	if (pad_print) {
		printf("nxpinball: %s %s\n", c == 0 ? "left flipper" : c == 1 ? "right flipper" : "plunger", down ? "held" : "let go");
		fflush(stdout);
	}
	if (c == 2)
		plunger_key(keys_held[c] != 0);
	else
		flipper_key(c, keys_held[c] != 0);
}

/* The hands' buttons, from the machine's word each frame of play: the triggers are the flippers. */
static void
pad_buttons(void)
{
	ui_pad_poll(&pad);
	if (pad.held & INPUT_HANDS || pad.up & INPUT_HANDS) {
		int hands = (pad.held & INPUT_HANDS) != 0;

		control_set(0, BY_HANDS, hands && (pad.held & INPUT_LEFT_TRIGGER));
		control_set(1, BY_HANDS, hands && (pad.held & INPUT_RIGHT_TRIGGER));
		control_set(2, BY_HANDS, hands && (pad.held & (INPUT_LEFT_GRIP | INPUT_RIGHT_GRIP | INPUT_JUMP)));
	}
	if (state != PLAYING && (pad.down & INPUT_JUMP))
		go();
	else if (state == PLAYING && (pad.down & INPUT_LEFT_UPPER))
		pause_game();
}

/* A mouse's buttons on the table (not in a headset, where the triggers are the pointer's buttons too). */
static int
pad_mouse(const GR_EVENT *event)
{
	GR_BUTTON b = event->button.changebuttons;
	int down = event->type == GR_EVENT_TYPE_BUTTON_DOWN;

	if (pad.held & INPUT_HANDS || event->button.y < MENU_H || (state != PLAYING && down))
		return 0;
	if (b & GR_BUTTON_L)
		control_set(0, BY_MOUSE, down);
	if (b & GR_BUTTON_R)
		control_set(1, BY_MOUSE, down);
	if (b & (GR_BUTTON_BACK | GR_BUTTON_FORWARD | GR_BUTTON_M))
		control_set(2, BY_MOUSE, down);
	return (b & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M | GR_BUTTON_BACK | GR_BUTTON_FORWARD)) != 0;
}

int
main(void)
{
	GR_EVENT event = { 0 };
	const char *a;
	int i, id;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	fonts();
	make_table();
	for (i = 0; i < 2; i++)
		flip[i].angle = flip[i].rest, flip[i].c = cosf(flip[i].angle), flip[i].s = sinf(flip[i].angle);
	demo = (a = getenv("PINBALL_DEMO")) && atoi(a) > 0;
	high = demo ? 0 : reg_int("nxpinball.high", 0);
	ball_to_plunger();
	ball_no = 1, mult = 1;
	for (i = 0; i < 3; i++)
		target_up[i] = 1;
	layout();
	art_make();
	window = GrNewWindowEx(UI_FIXED, "Pinball", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(window, EVENTS);
	ui_pad_ask(window);
	pad_print = getenv("NXPINBALL_PRINT") != NULL;
	GrMapWindow(window);
	GrSetFocus(window);
	if (demo)
		demo_run(atoi(a), getenv("PINBALL_HOLD"));
	for (;;) {
		long long now = now_us();
		int steps = 0, wait;

		pad_buttons();
		if (state == PLAYING && now >= frame_due) {
			while (state == PLAYING && tick_due <= now && steps++ < 8)
				tick(), tick_due += STEP_US;
			if (tick_due <= now)	/* (too far behind: the table's time waits) */
				tick_due = now + STEP_US;
			frame_due += FRAME_US;
			if (frame_due <= now)
				frame_due = now + FRAME_US;
		}
		draw_changes();
		now = now_us();
		wait = state != PLAYING ? -1 : frame_due <= now ? 0 : (int)((frame_due - now + 999) / 1000);
		if (pad.held & INPUT_HANDS && (wait < 0 || wait > 100))
			wait = 100;	/* (a headset's buttons are no event: looked at ten times a second) */
		next_event(&event, wait);
		if (event.type != GR_EVENT_TYPE_TIMEOUT && ui_restyle()) {	/* (Settings changed the look) */
			ui_menu_restyle();
			fonts();
			layout();
			art_make();
			GrResizeWindow(window, width, height);
			GrSetWindowBackgroundColor(window, UI_FACE);
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
		case GR_EVENT_TYPE_KEY_UP:
			key_up(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_FOCUS_OUT:
			pause_game();
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (event.button.wid == window)
				pad_mouse(&event);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid != window || ui_wheel(&event))
				break;
			if (overlay_shown < 0 && pad_mouse(&event))
				break;
			if (event.button.y < MENU_H) {
				for (i = 0; i < 2; i++)
					if (event.button.x >= menu_x[i] && event.button.x < menu_x[i + 1])
						open_menu(i, 0);
			} else if (overlay_shown >= 0 && ui_inside(event.button.x, event.button.y, button_x, button_y, button_w, button_h))
				go();
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
