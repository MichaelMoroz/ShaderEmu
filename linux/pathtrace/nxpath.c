/*
 * nxpath [MESH | SCENE]: a mesh viewer with a path tracer (docs/apps/raytrace.md, with its switches for
 * tests). The view is OpenGL's while it changes and the path tracer's when it is still; the
 * menus, the panel, the line of figures and the handles are Dear ImGui's (panel.cpp, gizmo.cpp).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <GLES/segl.h>
#include "ui.h"
#include "ui_icons.h"
#include "ui_files.h"
#include "mcw.h"
#include "scene.h"
#include "panel.h"

#define MOST_W 640		/* the largest picture traced */
#define MOST_H 480
#define ONE 65536
#define SMALL 32768		/* pixels of the small picture's memory */
#define DETAILS 4
#define ENDLESS (1 << 20)	/* samples of a picture that has no last one */
#define STILL 300		/* ms the view is left alone before it is traced */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)

static const int sample_steps[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 0};	/* 0: no last one */
#define SAMPLE_STEPS 11
static const char *const detail_names[DETAILS] = {" Full view", " Half", " A third", " A quarter"};
static const char *const kind_names[SC_KINDS] = {" Matt", " Plastic", " Metal", " Mirror", " Glass", " Glow"};
#define TURNS 16		/* the most turns a path may be given */
/* The example scenes: files of EXAMPLES, `nxpath NAME` or File, Examples (docs/apps/raytrace.md). */
#define EXAMPLES "/usr/share/pathtrace"
static const struct { const char *file, *label; } examples[] = {
	{"cornell", "The Cornell box"}, {"cornell-balls", "The Cornell box with balls"}, {"balls", "Glass and metal balls"},
	{"materials", "Matt, plastic, metal, glass"}, {"beer", "Coloured glass, thin and thick"}, {"mirrors", "Two mirrors face to face"},
	{"glow", "Lit by a glowing thing"},
};
#define EXAMPLE_COUNT (int)(sizeof examples / sizeof examples[0])

static GR_WINDOW_ID window;
static int wide = 770, high = 530, view_w, view_h;	/* the window, and the view in it under the menus */
static int bar = 19;		/* the menus' height (ImGui's) */
static int ui_dirty = 1;	/* a frame is to be drawn for the panel or the line of figures (at most every UI_MS) */
static unsigned ui_at;
#define UI_MS 100
static struct ui_list tree = {0, 30, PANEL - 8, ROWS * UI_ROW + 4, 0, 0, -1};
static int tab;			/* the panel's lower part: 0 what is selected, 1 the picture's settings */
static int what = GIZMO_MOVE;	/* what the handles do */
static int detail_at = 1, samples_at = 6, bounces = 8;
static int live = 1, wanted;	/* traced when the view is still; Render was asked for */
static char status[160] = "";
static int view_dirty = 1;
static int on_handle;		/* the pointer is over a handle, or holds one: the frame said so (panel_draw) */
static int drag, drag_x, drag_y;	/* a button held in the picture (1 turns the view, 2 moves it), and where the pointer was */
static int view_press, view_x, view_y, view_button;	/* a press in the view, its number (panel_button): the view's once ImGui had it and nothing of its own was under it */
static void press_view(int x, int y, int button);
static int have_gizmo;

/* the two kinds of thing a row of the list is: the objects first, then the lights */
static int chosen_object(void) { return tree.selected >= 0 && tree.selected < sc.object_count ? tree.selected : -1; }
static int chosen_light(void) { return tree.selected >= sc.object_count ? tree.selected - sc.object_count : -1; }

/* ---- the worker cores ---- */

static int workers;
static uint8_t busy[MC_MAX_CORES];
/* The lanes' field (docs/machine/lanes.md) where the machine has one: its jobs are the shown picture smoothed, a
 * texel a lane, a fixed and small amount of work (pt_smooth_texel; docs/apps/raytrace.md, "The picture smoothed on
 * the lanes"), and the workers trace. NXPATH_FIELD=1: the paths on the field in steps, the way before (use_field). */
static int use_field, field_wanted;
#define SMOOTH_MOST 32		/* the view shows the picture smoothed until it has this many samples */
static int smooth = 1;		/* (pathtrace.smooth) */
static int smoothing, smooth_ready, smoothed;	/* a job out; the smoothed picture is there, of `smoothed` samples */
static unsigned smooth_at;
static uint32_t *smooth_pixels;
static struct pt_smooth smooth_job __attribute__((aligned(16)));
static void smooth_check(void);

static unsigned
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

static void
take_workers(int most)
{
	extern char __DATA_BEGIN__[], _end[];
	extern void mcw_entry(void);

	mcw_close();
	workers = 0;
	use_field = most > 0 && field_wanted && mcw_field_lanes() > 0;
	if (most <= 0)
		return;
	/* (where a worker begins is read first: on a page of code this core has not run, every one stopped there) */
	(void)*(const volatile char *)mcw_entry;
	workers = mcw_open(most);
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
}

/* ---- the scene as OpenGL draws it ---- */

/* A mesh for the GPU: three vertices a triangle where they stay (seglTagged), a vertex's tag
 * the one of 256 directions its normal is nearest. An object's table has a colour for each. */
struct shown_mesh { unsigned int *vertices; int count; };
static unsigned int *tables;	/* 256 words an object, then a light: SC_OBJECTS + SC_LIGHTS of them */
static int ball_mesh = -1;	/* what a light is drawn as */

/* The direction a tag stands for: sixteen by sixteen places of an octahedron unfolded. */
static void
tag_way(int tag, float *n)
{
	float u = ((float)(tag & 15) + 0.5f) / 8.0f - 1.0f, w = ((float)(tag >> 4) + 0.5f) / 8.0f - 1.0f;
	float y = 1.0f - fabsf(u) - fabsf(w), len;

	if (y < 0.0f) {
		float fu = u, fw = w;

		u = fu >= 0.0f ? 1.0f - fabsf(fw) : fabsf(fw) - 1.0f, w = fw >= 0.0f ? 1.0f - fabsf(fu) : fabsf(fu) - 1.0f;
	}
	len = sqrtf(u * u + y * y + w * w);
	n[0] = u / len, n[1] = y / len, n[2] = w / len;
}

static int
way_tag(const float *n)
{
	float sum = fabsf(n[0]) + fabsf(n[1]) + fabsf(n[2]), u, w;
	int iu, iw;

	if (sum < 1e-12f)
		return 0;
	u = n[0] / sum, w = n[2] / sum;
	if (n[1] < 0.0f) {
		float fu = u, fw = w;

		u = fu >= 0.0f ? 1.0f - fabsf(fw) : fabsf(fw) - 1.0f, w = fw >= 0.0f ? 1.0f - fabsf(fu) : fabsf(fu) - 1.0f;
	}
	iu = (int)((u + 1.0f) * 8.0f), iw = (int)((w + 1.0f) * 8.0f);
	return (iu > 15 ? 15 : iu < 0 ? 0 : iu) | (iw > 15 ? 15 : iw < 0 ? 0 : iw) << 4;
}

/* A mesh's vertices for the GPU, made once; false when the GPU's memory is used up. */
static int
show_mesh(int m)
{
	struct sc_mesh *mesh = &sc.meshes[m];
	struct shown_mesh *shown;
	int i, count = mesh->triangles * 3;

	if (mesh->shown)
		return 1;
	shown = malloc(sizeof *shown);
	shown->count = count;
	shown->vertices = seglMemory((unsigned)count * 16);
	if (!shown->vertices) {
		free(shown);
		return 0;
	}
	for (i = 0; i < count; i++) {
		memcpy(&shown->vertices[i * 4], &mesh->positions[i * 3], 12);
		shown->vertices[i * 4 + 3] = (unsigned)way_tag(&mesh->normals[i * 3]) << 24;
	}
	mesh->shown = shown;
	return 1;
}

/* A table's colours: the colour lit from above and a little from the eye's side, by each
 * direction as the matrix turns it. `glow`: its own light (a light, or what is selected). */
static void
fill_table(unsigned int *table, const float *matrix, const float *colour, float glow)
{
	static const float light[3] = {0.35f, 0.85f, -0.4f};
	int tag, k;

	seglSettle();	/* (the list in flight reads the table) */
	for (tag = 0; tag < 256; tag++) {
		float n[3], w[3], len, lit, c[3];

		tag_way(tag, n);
		for (k = 0; k < 3; k++)
			w[k] = matrix[k] * n[0] + matrix[4 + k] * n[1] + matrix[8 + k] * n[2];
		len = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
		lit = len > 0.0f ? (w[0] * light[0] + w[1] * light[1] + w[2] * light[2]) / len : 0.0f;
		lit = 0.35f + 0.65f * (lit > 0.0f ? lit : -0.25f * lit) + glow;
		for (k = 0; k < 3; k++)
			c[k] = colour[k] * lit > 1.0f ? 1.0f : colour[k] * lit;
		table[tag] = (unsigned)(c[0] * 255.0f) << 16 | (unsigned)(c[1] * 255.0f) << 8 | (unsigned)(c[2] * 255.0f);
	}
}

static void
light_matrix(const struct sc_light *l, float *m)
{
	memset(m, 0, 16 * sizeof *m);
	m[0] = m[5] = m[10] = l->radius * 2.0f, m[15] = 1.0f;	/* (the ball is of radius one half) */
	m[12] = l->place[0], m[13] = l->place[1], m[14] = l->place[2];
}

/* Every table again: an object turned, coloured or picked out otherwise than it was. */
static void
fill_tables(void)
{
	int i;

	for (i = 0; i < sc.object_count; i++) {
		struct sc_object *o = &sc.objects[i];
		float c[3] = {o->colour[0], o->colour[1], o->colour[2]};

		if (o->metal > 0.5f)
			c[0] = 0.5f + 0.5f * c[0], c[1] = 0.5f + 0.5f * c[1], c[2] = 0.55f + 0.45f * c[2];
		if (o->glass > 0.5f)
			c[0] = 0.7f * c[0], c[1] = 0.85f * c[1], c[2] = 0.6f + 0.4f * c[2];
		fill_table(tables + 256 * i, o->matrix, c, (i == chosen_object() ? 0.25f : 0.0f) + (o->glow > 0.0f ? 0.6f : 0.0f));
	}
	for (i = 0; i < sc.light_count; i++) {
		float m[16];

		light_matrix(&sc.lights[i], m);
		fill_table(tables + 256 * (SC_OBJECTS + i), m, sc.lights[i].colour, i == chosen_light() ? 1.2f : 0.9f);
	}
	view_dirty = 1;
}

static void
view_matrices(float *view, float *projection)
{
	const struct sc_camera *c = &sc.camera;
	float x = c->lens * 0.5f * (float)view_w / (float)(view_h > 0 ? view_h : 1), y = c->lens * 0.5f, near = 0.5f, far = 100.0f;
	int k;

	for (k = 0; k < 3; k++)
		view[4 * k] = c->right[k], view[4 * k + 1] = c->up[k], view[4 * k + 2] = -c->ahead[k], view[4 * k + 3] = 0.0f;
	view[12] = -(c->right[0] * c->eye[0] + c->right[1] * c->eye[1] + c->right[2] * c->eye[2]);
	view[13] = -(c->up[0] * c->eye[0] + c->up[1] * c->eye[1] + c->up[2] * c->eye[2]);
	view[14] = c->ahead[0] * c->eye[0] + c->ahead[1] * c->eye[1] + c->ahead[2] * c->eye[2];
	view[15] = 1.0f;
	memset(projection, 0, 16 * sizeof *projection);
	projection[0] = near / x, projection[5] = near / y;
	projection[10] = -(far + near) / (far - near), projection[11] = -1.0f, projection[14] = -2.0f * far * near / (far - near);
}

/* ---- the path tracer's picture ---- */

/* The picture is the view's own shape, a part of its size: what OpenGL draws and what is
 * traced are one camera's. It is traced small first, then whole, sample after sample. */
static GLuint texture, small_texture;
static uint32_t *full_pixels, *small_pixels;
static int tracing;		/* shares are out, or to be handed out */
static int phase;		/* what is traced: 0 the small picture, 1 the picture */
static int traced;		/* the picture has a sample of every pixel */
static int showing;		/* what the view shows: 0 OpenGL's scene, 1 the small picture, 2 the picture */
static int stale = 1;		/* the scene or the view is no longer what was traced */
static int next_tile, sample, out;	/* this core alone: where it is; with workers: how many jobs are out */
/* On the field: a picture's samples in batches, each one job of the field in steps (a pixel's sample
 * an index, into the batch's values: docs/machine/lanes.md, "Steps"), then folded into the sums in order
 * by the workers, and shown. Two batches, so that the field makes one while the workers fold the other.
 * A batch's state: 0 free, 1 the field's, 2 made by it, 4 its fold on the workers. */
#define BATCH_MOST 4			/* samples a batch at most, */
#define BATCH_VALUES (MOST_W * MOST_H)	/* and its values at most (a pixel's sample each) */
static struct pt_batch batch[2] __attribute__((aligned(16)));
static int batch_state[2], field_on = -1, workers_on = -1, batch_next;
static uint32_t batch_from;		/* the first sample in no batch yet */
static unsigned field_at, field_ms, field_rounds, field_jobs, field_left;	/* (NXPATH_PRINT: the field's time, its rounds and jobs, those that left paths) */
static unsigned too_long;		/* steps no lane could make in a pass, made by this core: said with the picture */
static int rounds_set;			/* (NXPATH_ROUNDS: the field's rounds a batch, fixed; -1: as the workers are free) */
static struct mcw_steps_probe probed;	/* NXPATH_PROBE: the self-test, on a worker, */
static uint32_t probe_wrong;		/* and how many of its paths came out other than traced whole */
static uint32_t probe_budget;		/* (NXPATH_PROBE_BUDGET: a step's instructions; 0: a pass's) */

static uint32_t
probe_share(uint32_t first, uint32_t count)
{
	probed = mcw_steps_probe(pt_batch_step, &batch[0], first, count, probe_budget);
	probe_wrong = pt_batch_check(&batch[0], first, count);
	return 0;
}

/* worker w of n: every n-th texel's pixels of the batch folded (how: the batch, and 2 when it is shown) */
static uint32_t
fold_share(uint32_t which, uint32_t how)
{
	uint32_t n = which >> 16, w = which & 0xffff, texels = batch[how & 1].pixels / 4, t;

	/* (every n-th texel: the paths the field left going lie together, where the scene is hard) */
	for (t = w; t < texels; t += n)
		pt_batch_fold(&batch[how & 1], 4 * t, 4 * t + 4, (how & 2) != 0);
	return 0;
}

/* whether every worker given a share is through */
static int
workers_through(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		if (busy[k]) {
			if (!mcw_done(k))
				return 0;
			busy[k] = 0;
		}
	return 1;
}

static void
workers_share(mcw_fn fn, uint32_t how)
{
	int k;

	for (k = 1; k <= workers; k++)
		busy[k] = 1, mcw_post(k, fn, (uint32_t)(k - 1) | (uint32_t)workers << 16, how);
}

static unsigned began, took, shown_at, still_since, rays;
static int fixed_w, fixed_h;	/* a test's picture, of its own shape */
static int leave_when_done;
static int world_built;
static uint32_t world_of;

static void draw_panel(void);

/* The picture's bar along the bottom (panel.h): its words, and how much of it is filled. */
void
path_bar(char *text, int size, float *fill)
{
	int of = pt.samples > 0 ? pt.samples : 1, done = sample > of ? of : sample;

	snprintf(text, (size_t)size, "the scene, by OpenGL");
	*fill = 0.0f;
	if (tracing && pt.stop < 2 && phase == 0)
		snprintf(text, (size_t)size, "a small picture first");
	else if ((tracing && pt.stop < 2) || (traced && !stale)) {
		*fill = of == ENDLESS ? 0.0f : (float)done / (float)of;
		if (of == ENDLESS)
			snprintf(text, (size_t)size, tracing ? "%d samples, and on" : "%d samples", done);
		else
			snprintf(text, (size_t)size, tracing ? "sample %d of %d" : done < of ? "stopped at %d of %d" : "%d samples", tracing && done < of ? done + 1 : done, of);
	}
}

const char *
path_status(void)
{
	return status;
}

/* (the panel and the line of figures are drawn with the next frame) */
static void
draw_bar(void)
{
	ui_dirty = 1;
}

static void
draw_status(void)
{
	ui_dirty = 1;
}

static void
say(const char *text)
{
	snprintf(status, sizeof status, "%s", text);
	draw_status();
}

/* The scene or the view changed: what is traced is given up, and OpenGL shows it. */
static void
invalidate(void)
{
	stale = 1, showing = 0, view_dirty = 1;
	still_since = now_ms();
	if (tracing)
		pt.stop = 2;
}

/* How large the picture is traced: the view's size over what Detail says, in the memory there is. */
static void
picture_size(int *w, int *h)
{
	int by = detail_at + 1;

	if (fixed_w) {
		*w = fixed_w, *h = fixed_h;
		return;
	}
	do
		*w = view_w / by & ~3, *h = view_h / by, by++;
	while (*w * *h > MOST_W * MOST_H);
	*w = *w < 8 ? 8 : *w, *h = *h < 8 ? 8 : *h;
}

/* What the tree of boxes is made of: each object's mesh and where it stands. */
static uint32_t
world_sum(void)
{
	uint32_t sum = (uint32_t)sc.object_count, bits;
	int i, k;

	for (i = 0; i < sc.object_count; i++)
		for (sum = sum * 31 + (uint32_t)sc.objects[i].mesh, k = 0; k < 16; k++) {
			memcpy(&bits, &sc.objects[i].matrix[k], 4);
			sum = sum * 31 + bits;
		}
	return sum;
}

/* For a test: the object each of `count` of the eye's rays meets by the tree, and by trying
 * every triangle. A ray that meets a light first is not compared. */
static void
check_tree(int count)
{
	const struct sc_camera *c = &sc.camera;
	int i, k, wrong = 0, w, h;

	picture_size(&w, &h);
	for (i = 0; i < count; i++) {
		int x = (int)((unsigned)i * 7919u % (unsigned)w), y = (int)((unsigned)i * 104729u % (unsigned)h), by_tree = pt_pick((float)x + 0.5f, (float)y + 0.5f, w, h), slow;
		float sx = (2.0f * ((float)x + 0.5f) / (float)w - 1.0f) * c->lens * (c->aspect > 0.0f ? c->aspect : (float)w / (float)h);
		float sy = (1.0f - 2.0f * ((float)y + 0.5f) / (float)h) * c->lens, way[3], len = 0.0f, far;

		for (k = 0; k < 3; k++)
			way[k] = c->ahead[k] + c->right[k] * sx + c->up[k] * sy, len += way[k] * way[k];
		for (len = sqrtf(len), k = 0; k < 3; k++)
			way[k] /= len;
		slow = sc_world_nearest_slow(c->eye, way, &far);
		wrong += by_tree >= 0 && by_tree != (slow < 0 ? SC_OBJECTS : (int)sc_world.triangles[slow].object);
	}
	fprintf(stderr, "nxpath: %d of %d rays met another object by the tree than without it\n", wrong, count);
}

/* For a test: a point three units along each of `count` of the tracer's rays, put through
 * OpenGL's matrices: how far from the ray's own place in the view it comes, at most. */
static void
check_cameras(int count)
{
	const struct sc_camera *c = &sc.camera;
	float view[16], projection[16], worst = 0.0f;
	int i, k, w, h;

	picture_size(&w, &h);
	view_matrices(view, projection);
	for (i = 0; i < count; i++) {
		float x = (float)((unsigned)i * 7919u % (unsigned)w) + 0.5f, y = (float)((unsigned)i * 104729u % (unsigned)h) + 0.5f;
		float way[3], p[4], e[4], q[4], dx, dy;

		pt_way(x, y, w, h, way);
		for (k = 0; k < 3; k++)
			p[k] = c->eye[k] + 3.0f * way[k];
		for (k = 0; k < 4; k++)
			e[k] = view[k] * p[0] + view[4 + k] * p[1] + view[8 + k] * p[2] + view[12 + k];
		for (k = 0; k < 4; k++)
			q[k] = projection[k] * e[0] + projection[4 + k] * e[1] + projection[8 + k] * e[2] + projection[12 + k] * e[3];
		dx = fabsf((q[0] / q[3] * 0.5f + 0.5f) * (float)view_w - x * (float)view_w / (float)w);
		dy = fabsf((0.5f - q[1] / q[3] * 0.5f) * (float)view_h - y * (float)view_h / (float)h);
		worst = dx > worst ? dx : worst, worst = dy > worst ? dy : worst;
	}
	fprintf(stderr, "nxpath: OpenGL's camera and the tracer's are at most %d thousandths of a pixel of the view apart over %d rays\n",
		(int)(worst * 1000.0f + 0.5f), count);
}

/* One of the two pictures begun: the small one has a sample, the other as many as are asked. */
static void
start_phase(int p)
{
	int w, h, s, tiles;

	picture_size(&w, &h);
	if (p == 0)
		w = (w / 4 + 3) & ~3, h = (h + 3) / 4, too_long = 0;
	seglSettle();
	phase = p;
	pt.width = w, pt.height = h;
	pt.pixels = p ? full_pixels : small_pixels;
	glBindTexture(GL_TEXTURE_2D, p ? texture : small_texture);
	seglTexturePointer(pt.pixels, w, h, GL_RGBA);
	/* (as the squares it is: the device smooths no texture of this kind) */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	if (workers)
		mcw_touch(pt.sums, (unsigned)(w * h) * 4 * sizeof(float));
	for (s = 32; s > 1 && ((w + (s < 8 ? 8 : s) - 1) / (s < 8 ? 8 : s)) * ((h + s - 1) / s) < 24 * workers; s >>= 1)
		continue;
	pt.tile_h = s;
	pt.tile_w = s < 8 ? 8 : s;
	pt.tiles = tiles = ((w + pt.tile_w - 1) / pt.tile_w) * ((h + pt.tile_h - 1) / pt.tile_h);
	/* a stride with no factor in common with the number of tiles visits every one of them */
	for (pt.stride = (uint32_t)tiles * 5 / 8 + 1;; pt.stride++) {
		uint32_t a = pt.stride, b = (uint32_t)tiles;

		while (b) { uint32_t r = a % b; a = b; b = r; }
		if (a == 1)
			break;
	}
	pt.step = (uint32_t)(workers ? workers : 1) * pt.stride % (uint32_t)tiles;
	pt.samples = !p ? 1 : sample_steps[samples_at] ? sample_steps[samples_at] : ENDLESS;
	pt.stop = 0;
	memset(pt.slots, 0, MC_MAX_CORES * sizeof(struct pt_slot));
	if (field_on >= 0 || smoothing)
		mcw_field_wait();	/* (a batch of the picture given up: what it writes is the next one's values) */
	smoothing = smooth_ready = smoothed = 0;
	if (workers_on >= 0)
		for (s = 1; s <= workers; s++)
			if (busy[s])
				mcw_wait(s);
	memset(busy, 0, sizeof busy);
	next_tile = sample = out = 0, rays = 0;
	field_on = workers_on = -1, batch_state[0] = batch_state[1] = 0, batch_next = 0, batch_from = 0;
	/* (the batches once, when the field or the self-test first wants them: 59 MB; touched, as the workers store to them) */
	for (s = 0; (use_field || getenv("NXPATH_PROBE")) && s < 2; s++) {
		if (!batch[s].vals && !(batch[s].vals = mcw_alloc(BATCH_VALUES * 16 * PT_STEP_TEXELS)))
			use_field = 0;	/* (no memory for them: the workers' shares) */
		else if (!batch[s].pixels)
			mcw_touch(batch[s].vals, BATCH_VALUES * 16 * PT_STEP_TEXELS), batch[s].pixels = 1;
	}
	if (p && workers && getenv("NXPATH_PROBE") && batch[0].pixels) {
		/* the self-test: as many of the picture's pixels as asked, from its middle row on (sample 0) */
		uint32_t first = (uint32_t)(h / 2 * w), count = (uint32_t)atoi(getenv("NXPATH_PROBE"));

		probe_budget = getenv("NXPATH_PROBE_BUDGET") ? (uint32_t)atoi(getenv("NXPATH_PROBE_BUDGET")) : 0;
		count = count > (uint32_t)(w * h) - first ? (uint32_t)(w * h) - first : count;
		batch[0].s0 = 0, batch[0].count = 1, batch[0].pixels = (uint32_t)(w * h);
		mcw_post(1, probe_share, first, count);
		mcw_wait(1);
		fprintf(stderr, "nxpath: the self-test: %u steps of %u paths, %u rounds at most, %u instructions a step on average, the longest %u, %u over a pass; %u paths not as traced whole\n",
			probed.steps, count, probed.rounds, probed.mean, probed.longest, probed.over, probe_wrong);
	}
	if (p)
		traced = 0;
	tracing = 1;
}

/* The scene as it is and the view as it is, traced from the start: the tree again only when
 * an object is not where the tree has it. */
static void
begin_trace(void)
{
	uint32_t of = world_sum();
	char text[120];
	int w, h;

	if (tracing)
		return;
	if (!world_built || of != world_of) {
		say("Building the tree of boxes...");
		if (!sc_world_build(workers)) {
			stale = 0;
			say("No memory for the scene's triangles.");
			return;
		}
		world_built = 1, world_of = of;
	}
	if (!pt_glowing())
		say("No memory for the glowing triangles: they light nothing.");
	sc.camera.aspect = fixed_w ? 0.0f : (float)view_w / (float)view_h;
	sc_camera_set(&sc.camera);
	pt.bounces = bounces;
	pt.seed = 0x9e3779b9u;
	pt.owners = 0;
	pt_read_all();
	if (getenv("NXPATH_CHECK"))
		check_tree(atoi(getenv("NXPATH_CHECK")));
	if (getenv("NXPATH_CHECK") && !fixed_w)
		check_cameras(atoi(getenv("NXPATH_CHECK")) * 50);
	stale = 0;
	began = now_ms();
	wanted = 0;
	start_phase(fixed_w ? 1 : 0);
	picture_size(&w, &h);
	snprintf(text, sizeof text, "%d triangles under %d boxes (the tree %u ms by %u workers). Tracing %d x %d...", sc_world.triangle_count,
		 sc_world.node_count, (unsigned)sc_world.build_ms, (unsigned)sc_world.build_workers, w, h);
	say(text);
	draw_panel();
}

/* More samples of the picture that is there: after Stop, or when more are asked for. */
static int
trace_more(void)
{
	int most = sample_steps[samples_at] ? sample_steps[samples_at] : ENDLESS;

	if (stale || phase != 1 || (tracing ? pt.stop : !traced))
		return 0;
	pt.samples = most;
	if (!tracing && sample < most) {
		pt.stop = 0, tracing = 1;
		began = now_ms() - took;
		showing = 2, view_dirty = 1;
	}
	draw_bar();
	return 1;
}

static int
write_picture(const char *path)
{
	FILE *file = fopen(path, "wb");
	int i;

	if (!file) {
		say("The picture could not be written there.");
		return 0;
	}
	fprintf(file, "P6\n%d %d\n255\n", pt.width, pt.height);
	for (i = 0; i < pt.width * pt.height; i++) {
		fputc(pt.pixels[i] >> 16 & 255, file);
		fputc(pt.pixels[i] >> 8 & 255, file);
		fputc(pt.pixels[i] & 255, file);
	}
	i = ferror(file);
	if (fclose(file) != 0 || i) {
		say("The picture could not be written whole.");
		return 0;
	}
	return 1;
}

/* The picture has its samples, or was stopped: its figures, and what a test asked of it. */
static void
end_render(void)
{
	char text[160];
	int i;

	tracing = 0;
	took = now_ms() - began;
	traced = sample >= 1;
	if (use_field) {
		/* (the rays, every pixel's from its sum's fourth word) */
		for (rays = 0, i = 0; i < pt.width * pt.height; i++)
			rays += (unsigned)pt.sums[4 * i + 3];
	}
	if (use_field && getenv("NXPATH_PRINT"))
		fprintf(stderr, "nxpath: on the field: %u jobs in %u ms, %u rounds (%u left paths to the workers), %u steps too long for a lane\n", field_jobs, field_ms, field_rounds, field_left, too_long);
	field_ms = field_rounds = field_jobs = field_left = 0;
	if (leave_when_done || getenv("NXPATH_PRINT")) {
		unsigned sum = 0;
		float light = 0.0f;

		for (i = 0; i < pt.width * pt.height; i++)
			sum = sum * 31 + pt.pixels[i], light += pt.sums[i * 4] + pt.sums[i * 4 + 1] + pt.sums[i * 4 + 2];
		fprintf(stderr, "pathstat: %d x %d, %d samples, %d turns, %d triangles: tree %u ms by %u workers, picture %u ms with %d workers, %u rays, light %d, sum %08x\n",
			pt.width, pt.height, sample, pt.bounces, sc_world.triangle_count, (unsigned)sc_world.build_ms, (unsigned)sc_world.build_workers, took,
			workers, rays, sample ? (int)(light * 1000.0f / (float)(pt.width * pt.height * 3 * sample)) : 0, sum);
	}
	snprintf(text, sizeof text, "%d x %d, %d samples: %u.%u s with %d workers, %u thousand rays a second.", pt.width, pt.height, sample, took / 1000,
		 took % 1000 / 100, workers, took ? rays / took : 0);
	/* (a step longer than a pass: the lanes cannot make this picture as it is set, a core made those) */
	if (use_field && too_long)
		snprintf(text + strlen(text), sizeof text - strlen(text), " %u steps too long for the lanes.", too_long);
	if (traced)
		showing = 2;
	view_dirty = 1;
	say(text);
	if (getenv("NXPATH_SAVE"))
		write_picture(getenv("NXPATH_SAVE"));
	if (getenv("NXPATH_SMOOTH") && traced)
		smooth_check();
	draw_panel();
	if (leave_when_done)
		exit(0);
}

/* The shares: every worker has all of its own at once and is asked how far it is; with no
 * workers this core traces a tile a call. */
static void
trace_on(void)
{
	static unsigned polls;
	int k, least = pt.samples, before = sample, done;
	unsigned all = 0;

	if (use_field) {
		uint32_t pixels = (uint32_t)(pt.width * pt.height), m, most;

		/* the field's batch made: it goes on round after round while the workers fold, and when they
		 * have nothing to do ends at the next round and leaves them the paths still going */
		mcw_field_steps_rounds(rounds_set >= 0 ? (uint32_t)rounds_set : workers_on < 0 ? 1u : 0u);
		if (field_on >= 0 && mcw_field_ready()) {
			struct mcw_steps_report r = mcw_field_steps_report();

			field_ms += now_ms() - field_at, field_rounds += r.rounds, field_jobs++, too_long += r.too_long, field_left += r.left;
			batch_state[field_on] = 2, field_on = -1;
		}
		/* its fold on the workers, one batch at a time, in order */
		if (workers_on >= 0 && workers_through()) {
			struct pt_batch *b = &batch[workers_on];
			uint32_t i, few = 0;

			/* (its rays: those of a 64th of the pixels, times 64) */
			for (i = 0; i < pixels; i += 64)
				few += (uint32_t)pt.sums[4 * i + 3];
			rays = 64 * few;
			sample = (int)(b->s0 + b->count);
			batch_state[workers_on] = 0, workers_on = -1;
		}
		if (workers_on < 0) {
			int a = batch_state[0] == 2, c = batch_state[1] == 2;

			if (a && c)
				workers_on = batch[0].s0 < batch[1].s0 ? 0 : 1;
			else if (a || c)
				workers_on = a ? 0 : 1;
			if (workers_on >= 0) {
				struct pt_batch *b = &batch[workers_on];

				batch_state[workers_on] = 4;
				workers_share(fold_share, (uint32_t)workers_on | (pt_shown_after(b->s0 + b->count - 1) ? 2u : 0u));
			}
		}
		/* the next batch on the field: as many samples as fit, up to the next that is shown */
		if (field_on < 0 && !pt.stop && batch_from < (uint32_t)pt.samples && batch_state[batch_next] == 0) {
			most = BATCH_VALUES / pixels;
			most = most > BATCH_MOST ? BATCH_MOST : most < 1 ? 1 : most;
			for (m = 1; m < most && batch_from + m < (uint32_t)pt.samples && !pt_shown_after(batch_from + m - 1); m++)
				continue;
			batch[batch_next].s0 = batch_from, batch[batch_next].count = m, batch[batch_next].pixels = pixels;
			field_at = now_ms();
			mcw_field_steps(pt_batch_step, &batch[batch_next], 0, m * pixels, batch[batch_next].vals, PT_STEP_TEXELS);
			batch_state[batch_next] = 1, field_on = batch_next;
			batch_next ^= 1, batch_from += m;
		}
		done = field_on < 0 && workers_on < 0 && batch_state[0] == 0 && batch_state[1] == 0 && (pt.stop || batch_from >= (uint32_t)pt.samples);
	} else if (!workers) {
		done = pt.stop || sample >= pt.samples;
		if (!done) {
			rays += pt_tile((uint32_t)next_tile * pt.stride % (uint32_t)pt.tiles, (uint32_t)sample);
			if (++next_tile >= pt.tiles)
				next_tile = 0, sample++;
		}
	} else {
		for (k = 1; k <= workers; k++) {
			int at = (int)pt.slots[k].samples;

			/* (asked when its slot says it is through, and now and then: its core may have been wanted back) */
			if (busy[k] && ((polls & 3) == 0 || at >= pt.samples || pt.stop) && mcw_done(k))
				busy[k] = 0, out--;
			if (!busy[k] && at < pt.samples && !pt.stop) {
				busy[k] = 1, out++;
				mcw_post(k, pt_share, (uint32_t)(k - 1) | (uint32_t)workers << 16, (uint32_t)k << 24 | (mcw_bound(k) ? 0u : 1u));
			}
			all += pt.slots[k].rays;
			if (at < least)
				least = at;
		}
		polls++;
		rays = all, sample = least;
		done = out == 0 && (least >= pt.samples || pt.stop);
	}
	if (!done) {
		if (phase == 1 && !traced && sample >= 1) {
			traced = 1, showing = 2, view_dirty = 1;
			if (getenv("NXPATH_PRINT"))
				fprintf(stderr, "nxpath: the picture %d x %d has a sample after %u ms\n", pt.width, pt.height, now_ms() - began);
		}
		if (sample != before)
			draw_bar();
		return;
	}
	if (pt.stop == 2 || (phase == 0 && pt.stop)) {
		tracing = 0;
		draw_bar();
		return;
	}
	if (phase == 0) {
		showing = 1, view_dirty = 1;
		if (getenv("NXPATH_PRINT"))
			fprintf(stderr, "nxpath: the small picture %d x %d after %u ms\n", pt.width, pt.height, now_ms() - began);
		start_phase(1);
		return;
	}
	end_render();
}

/* The picture's settings are the registry's (registry.h): the next start has them. */
static void
keep_settings(void)
{
	reg_set_int("pathtrace.detail", detail_at);
	reg_set_int("pathtrace.samples", samples_at);
	reg_set_int("pathtrace.turns", bounces);
	reg_set_int("pathtrace.background", (int)(sc.sky_power * 100.0f + 0.5f));
	reg_set_int("pathtrace.background.colour", (int)(sc.sky[0] * 255.0f + 0.5f) << 16 | (int)(sc.sky[1] * 255.0f + 0.5f) << 8 | (int)(sc.sky[2] * 255.0f + 0.5f));
	reg_set_int("pathtrace.live", live);
	reg_set_int("pathtrace.smooth", smooth);
}

/* ---- the scene's things ---- */

static void
name_free(char *name, const char *wanted)
{
	int n, i, taken;

	for (n = 1;; n++) {
		snprintf(name, SC_NAME, n == 1 ? "%.24s" : "%.24s %d", wanted, n);
		for (taken = 0, i = 0; i < sc.object_count; i++)
			taken |= !strcmp(sc.objects[i].name, name);
		for (i = 0; i < sc.light_count; i++)
			taken |= !strcmp(sc.lights[i].name, name);
		if (!taken)
			return;
	}
}

/* An object sized so that its longest side is `across`, standing on the ground at the middle. */
static void
fit_object(struct sc_object *o, float across)
{
	const struct sc_mesh *m = &sc.meshes[o->mesh];
	float longest = m->high[0] - m->low[0];
	int k;

	for (k = 1; k < 3; k++)
		if (m->high[k] - m->low[k] > longest)
			longest = m->high[k] - m->low[k];
	for (k = 0; k < 3; k++) {
		o->size[k] = longest > 0.0f ? across / longest : 1.0f;
		o->place[k] = -(m->low[k] + m->high[k]) * 0.5f * o->size[k];
	}
	o->place[1] = -m->low[1] * o->size[1];
}

/* An object of a mesh, standing on the ground at the middle; `across`: how large its longest side is made. */
static int
add_object(int mesh, float across, int kind, float r, float g, float b)
{
	struct sc_object *o;

	if (mesh < 0 || sc.object_count == SC_OBJECTS || !show_mesh(mesh))
		return -1;
	o = &sc.objects[sc.object_count];
	memset(o, 0, sizeof *o);
	name_free(o->name, sc.meshes[mesh].name);
	o->mesh = mesh;
	fit_object(o, across);
	sc_material(o, kind);
	o->colour[0] = r, o->colour[1] = g, o->colour[2] = b;
	sc_object_matrix(o);
	return sc.object_count++;
}

static int
add_light(float x, float y, float z, float radius, float power)
{
	struct sc_light *l;

	if (sc.light_count == SC_LIGHTS)
		return -1;
	l = &sc.lights[sc.light_count];
	memset(l, 0, sizeof *l);
	name_free(l->name, "Light");
	l->place[0] = x, l->place[1] = y, l->place[2] = z;
	l->radius = radius, l->power = power;
	l->colour[0] = l->colour[1] = 1.0f, l->colour[2] = 0.95f;
	return sc.light_count++;
}

static char mesh_files[SC_MESHES][256];	/* the file each mesh was read from; "" for plane, box, ball */

/* A mesh from a file, or the one of that file read before; -1 with the status line saying why not. */
static int
import_mesh(const char *path)
{
	const char *leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char why[120], name[SC_NAME];
	int m;

	snprintf(name, sizeof name, "%.*s", (int)(strcspn(leaf, ".") < SC_NAME - 1 ? strcspn(leaf, ".") : SC_NAME - 1), leaf);
	for (m = 0; m < sc.mesh_count; m++)
		if (!strcmp(sc.meshes[m].name, name))
			return m;
	say("Reading the mesh...");
	m = sc_mesh_load(path, why, sizeof why);
	if (m < 0) {
		say(why);
		return -1;
	}
	snprintf(sc.meshes[m].name, SC_NAME, "%s", name);
	snprintf(mesh_files[m], sizeof mesh_files[m], "%s", path);
	return m;
}

static void
choose(int row)
{
	tree.count = sc.object_count + sc.light_count;
	tree.selected = row < tree.count ? row : -1;
	ui_list_show(&tree);
	fill_tables();
	draw_panel();
}

static void
delete_chosen(void)
{
	int o = chosen_object(), l = chosen_light();

	if (o >= 0) {
		memmove(&sc.objects[o], &sc.objects[o + 1], (size_t)(sc.object_count - o - 1) * sizeof sc.objects[0]);
		sc.object_count--;
	} else if (l >= 0) {
		memmove(&sc.lights[l], &sc.lights[l + 1], (size_t)(sc.light_count - l - 1) * sizeof sc.lights[0]);
		sc.light_count--;
	}
	choose(-1);
}

/* An object's place, turn and size out of its matrix, as the handles left it. */
static void
from_matrix(struct sc_object *o)
{
	float len[3], r20, r21, r22, r10, r00;
	int k;

	for (k = 0; k < 3; k++) {
		const float *c = &o->matrix[4 * k];

		len[k] = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
		o->size[k] = len[k] > 1e-6f ? len[k] : 1e-6f;
		o->place[k] = o->matrix[12 + k];
	}
	r00 = o->matrix[0] / o->size[0], r10 = o->matrix[1] / o->size[0], r20 = o->matrix[2] / o->size[0];
	r21 = o->matrix[6] / o->size[1], r22 = o->matrix[10] / o->size[2];
	o->turn[1] = asinf(r20 < -1.0f ? 1.0f : r20 > 1.0f ? -1.0f : -r20) * 57.29578f;
	o->turn[0] = atan2f(r21, r22) * 57.29578f;
	o->turn[2] = atan2f(r10, r00) * 57.29578f;
}

/* ---- the panel (panel.cpp draws it, and reads it from here: panel.h) ---- */

static void
draw_panel(void)
{
	ui_dirty = 1;
}

static const char *
row_label(int row)
{
	static char text[SC_NAME + 8];

	if (row < sc.object_count)
		return sc.objects[row].name;
	snprintf(text, sizeof text, "* %s", sc.lights[row - sc.object_count].name);
	return text;
}

/* The values a line of three shows and its small buttons change: an object's place, turn
 * or size, or a light's place. */
static float *
line_values(int line, float *step)
{
	int o = chosen_object(), l = chosen_light();

	*step = line == 0 ? 0.1f : line == 1 ? 15.0f : 0.0f;
	if (o >= 0)
		return line == 0 ? sc.objects[o].place : line == 1 ? sc.objects[o].turn : sc.objects[o].size;
	return l >= 0 && line == 0 ? sc.lights[l].place : NULL;
}

static int
slider_most(int n)
{
	return n == S_SAMPLES ? SAMPLE_STEPS - 1 : n == S_TURNS ? TURNS - 1 : 255;
}

/* A slider's number of the selected thing or of the scene: where it is, and from what it goes how far. */
static float *
slider_place(int n, float *least, float *span)
{
	struct sc_object *o = chosen_object() >= 0 ? &sc.objects[chosen_object()] : NULL;
	struct sc_light *l = chosen_light() >= 0 ? &sc.lights[chosen_light()] : NULL;

	*least = 0.0f, *span = 1.0f;
	switch (n) {
	case S_RED: case S_GREEN: case S_BLUE: return o ? &o->colour[n] : l ? &l->colour[n] : NULL;
	case S_RADIUS: *least = 0.02f, *span = 0.98f; return l ? &l->radius : NULL;
	case S_POWER: *span = 100.0f; return l ? &l->power : NULL;
	case S_SKY: *span = 2.0f; return &sc.sky_power;
	case S_SKY_RED: case S_SKY_GREEN: case S_SKY_BLUE: return &sc.sky[n - S_SKY_RED];
	case S_ROUGH: return o ? &o->rough : NULL;
	case S_METAL: return o ? &o->metal : NULL;
	case S_GLASS: return o ? &o->glass : NULL;
	case S_IOR: *least = 1.0f, *span = 1.5f; return o ? &o->ior : NULL;
	case S_DENSITY: *span = 10.0f; return o ? &o->density : NULL;
	case S_GLOW: *span = 20.0f; return o ? &o->glow : NULL;
	}
	return NULL;
}

static int
slider_value(int n, int set, int value)
{
	float least, span, *f;

	if (n == S_SAMPLES) { if (set) samples_at = value; return samples_at; }
	if (n == S_TURNS) { if (set) bounces = value + 1; return bounces - 1; }
	f = slider_place(n, &least, &span);
	if (!f)
		return -1;
	if (set)
		*f = least + (float)value * span / 255.0f;
	value = (int)((*f - least) * 255.0f / span + 0.5f);
	return value < 0 ? 0 : value > 255 ? 255 : value;
}

static void
number_text(char *text, int size, float v)
{
	int whole = (int)(v < 0.0f ? -v : v), part = (int)(((v < 0.0f ? -v : v) - (float)whole) * 100.0f + 0.5f);

	if (part == 100)
		whole++, part = 0;
	snprintf(text, (size_t)size, "%s%d.%02d", v < 0.0f ? "-" : "", whole, part);
}

/* What panel.cpp reads (panel.h). */
int path_rows(void) { return sc.object_count + sc.light_count; }
const char *path_row(int row) { return row_label(row); }
int path_selected(void) { return tree.selected; }
int path_object(void) { return chosen_object(); }
int path_light(void) { return chosen_light(); }
int path_tab(void) { return tab; }
int path_what(void) { return what; }
float *path_line(int line, float *step) { return line_values(line, step); }
int path_slider(int n, int set, int value) { return slider_value(n, set, value); }
int path_slider_most(int n) { return slider_most(n); }
int path_live(void) { return live; }
int path_smooth(void) { return smooth; }
int path_tracing(void) { return tracing && !pt.stop; }
int path_showing(void) { return showing; }
const char *path_detail(void) { return detail_names[detail_at] + 1; }
void path_picture_size(int *w, int *h) { picture_size(w, h); }
const char *path_example(int i) { return i < EXAMPLE_COUNT ? examples[i].label : NULL; }

const float *
path_colour(void)
{
	int o = chosen_object(), l = chosen_light();

	return o >= 0 ? sc.objects[o].colour : l >= 0 ? sc.lights[l].colour : NULL;
}

/* A slider's value in words: a colour's part as it is counted, a whole number of a light's power, the others to hundredths. */
void
path_slider_text(int n, char *text, int size)
{
	float least, span, *f = slider_place(n, &least, &span);

	text[0] = 0;
	if (n == S_SAMPLES && sample_steps[samples_at])
		snprintf(text, (size_t)size, "%d", sample_steps[samples_at]);
	else if (n == S_SAMPLES)
		snprintf(text, (size_t)size, "no end");
	else if (n == S_TURNS)
		snprintf(text, (size_t)size, "%d", bounces);
	else if (f && (n <= S_BLUE || n >= S_SKY_RED))
		snprintf(text, (size_t)size, "%d", slider_value(n, 0, 0));
	else if (f && n == S_POWER)
		snprintf(text, (size_t)size, "%d", (int)(*f + 0.5f));
	else if (f)
		number_text(text, size, *f);
}

/* A menu's row that cannot be done now. */
int
path_greyed(int id)
{
	int most = sample_steps[samples_at] ? sample_steps[samples_at] : ENDLESS, any = tree.selected >= 0;

	return (id == A_EXPORT && !traced) || (id == A_PICTURE && (!traced || stale)) || ((id == A_DUPLICATE || id == A_DELETE) && !any) ||
	       (id == A_STOP && !tracing) || (id == A_RENDER && !stale && (tracing || sample >= most));
}

/* For a test: the controls of the panel's last frame that lie over another, and the words wider than their place. */
static int layout_asked;	/* (said after the next frame, which is of the panel as it is now) */

static void
check_layout(void)
{
	layout_asked = 1, ui_dirty = 1;
}

/* For a test without a screen: what the scene has and what is selected, on the console. */
static void
tell(void)
{
	int o = chosen_object(), l = chosen_light(), x, y;

	if (!getenv("NXPATH_PRINT"))
		return;
	check_layout();
	fprintf(stderr, "nxpath: %d objects, %d lights", sc.object_count, sc.light_count);
	if (o >= 0)
		fprintf(stderr, "; %s (rough %d, metal %d, glass %d, index %d, density %d, glow %d) at %d %d %d (hundredths), size %d, colour %d %d %d", sc.objects[o].name,
			(int)(sc.objects[o].rough * 100.0f + 0.5f), (int)(sc.objects[o].metal * 100.0f + 0.5f), (int)(sc.objects[o].glass * 100.0f + 0.5f),
			(int)(sc.objects[o].ior * 100.0f + 0.5f), (int)(sc.objects[o].density * 100.0f + 0.5f), (int)(sc.objects[o].glow * 100.0f + 0.5f),
			(int)(sc.objects[o].place[0] * 100.0f), (int)(sc.objects[o].place[1] * 100.0f), (int)(sc.objects[o].place[2] * 100.0f),
			(int)(sc.objects[o].size[0] * 100.0f), (int)(sc.objects[o].colour[0] * 255.0f), (int)(sc.objects[o].colour[1] * 255.0f),
			(int)(sc.objects[o].colour[2] * 255.0f));
	if (l >= 0)
		fprintf(stderr, "; %s, power %d", sc.lights[l].name, (int)sc.lights[l].power);
	ui_window_origin(window, &x, &y);
	fprintf(stderr, "; background %d of %02x%02x%02x", (int)(sc.sky_power * 100.0f + 0.5f), (int)(sc.sky[0] * 255.0f + 0.5f), (int)(sc.sky[1] * 255.0f + 0.5f),
		(int)(sc.sky[2] * 255.0f + 0.5f));
	fprintf(stderr, "; detail %d, samples %d, turns %d, live %d; window at %d,%d, view %d x %d", detail_at + 1, sample_steps[samples_at], bounces, live, x, y,
		view_w, view_h);
	fprintf(stderr, "\n");
}

/* The scene was changed: the tables for it, and the traced picture is of another scene. */
static void
changed(void)
{
	int i;

	tell();
	for (i = 0; i < sc.object_count; i++)
		sc_object_matrix(&sc.objects[i]);
	invalidate();
	fill_tables();
	draw_panel();
}

/* The four dialogs are ImGui's chooser of a file (panel_files, panel.cpp): called with no file each asks it, and
 * with the file it was given (A_FILES) does what it is for. */
static void
import_dialog(const char *path)
{
	int m;

	if (!path) {
		panel_files(A_IMPORT, "Import a mesh", "/root/Models", ".obj .ply", NULL);
		draw_panel();
		return;
	}
	if ((m = import_mesh(path)) < 0)
		return;
	if ((m = add_object(m, 1.0f, SC_MATT, 0.8f, 0.8f, 0.8f)) < 0) {
		say("No room for it: the scene or the GPU's memory is full.");
		return;
	}
	say("");
	tab = 0;
	choose(m);
	changed();
}

static void
export_dialog(const char *chosen)
{
	char path[512];

	if (!traced || phase != 1) {
		say("There is no picture yet.");
		return;
	}
	if (!chosen) {
		panel_files(A_EXPORT, "Export the picture", "/root/Pictures", ".ppm", "render.ppm");
		draw_panel();
		return;
	}
	snprintf(path, sizeof path, "%s", chosen);
	ui_files_ending(path, sizeof path, ".ppm");
	if (write_picture(path))
		say("The picture is written.");
}

/* A copy of what is selected, beside it. */
static void
duplicate(void)
{
	int o = chosen_object(), l = chosen_light();

	if (o >= 0 && sc.object_count < SC_OBJECTS) {
		struct sc_object *made = &sc.objects[sc.object_count];

		*made = sc.objects[o];
		name_free(made->name, sc.meshes[made->mesh].name);
		made->place[0] += 0.4f;
		choose(sc.object_count++);
	} else if (l >= 0 && sc.light_count < SC_LIGHTS) {
		struct sc_light *made = &sc.lights[sc.light_count];

		*made = sc.lights[l];
		name_free(made->name, "Light");
		made->place[0] += 0.4f;
		choose(sc.object_count + sc.light_count++);
	} else
		say("No room for another.");
}

/* The camera where it sees every object: round the middle of them, as far as they reach. */
static void
whole_view(void)
{
	float low[3] = {1e30f, 1e30f, 1e30f}, high_[3] = {-1e30f, -1e30f, -1e30f}, reach = 0.0f;
	int i, k;

	for (i = 0; i < sc.object_count; i++) {
		const struct sc_object *o = &sc.objects[i];
		const struct sc_mesh *m = &sc.meshes[o->mesh];
		float half = 0.0f;

		/* (the ground is as large as one likes: it does not count) */
		if ((m->high[1] - m->low[1]) * o->size[1] * 8.0f < (m->high[0] - m->low[0]) * o->size[0])
			continue;
		for (k = 0; k < 3; k++)
			half += (m->high[k] - m->low[k]) * o->size[k] * (m->high[k] - m->low[k]) * o->size[k] * 0.25f;
		half = sqrtf(half);
		for (k = 0; k < 3; k++) {
			float c = o->matrix[k] * (m->low[0] + m->high[0]) * 0.5f + o->matrix[4 + k] * (m->low[1] + m->high[1]) * 0.5f +
				  o->matrix[8 + k] * (m->low[2] + m->high[2]) * 0.5f + o->matrix[12 + k];

			if (c - half < low[k]) low[k] = c - half;
			if (c + half > high_[k]) high_[k] = c + half;
		}
	}
	if (low[0] > high_[0])
		return;
	for (k = 0; k < 3; k++) {
		sc.camera.target[k] = (low[k] + high_[k]) * 0.5f;
		reach += (high_[k] - low[k]) * (high_[k] - low[k]) * 0.25f;
	}
	sc.camera.far = sqrtf(reach) / sc.camera.lens * 1.1f + 0.5f;
	invalidate();
}

static void
add_thing(int id)
{
	int made = -1;

	if (id == A_PLANE)
		made = add_object(sc_mesh_plane(), 4.0f, SC_MATT, 0.7f, 0.7f, 0.7f);
	else if (id == A_BOX)
		made = add_object(sc_mesh_box(), 0.6f, SC_MATT, 0.8f, 0.6f, 0.3f);
	else if (id == A_BALL)
		made = add_object(sc_mesh_ball(16), 0.6f, SC_MIRROR, 0.95f, 0.95f, 0.95f);
	else if (id == A_LIGHT && (made = add_light(0.0f, 2.0f, 0.0f, 0.1f, 30.0f)) >= 0)
		made += sc.object_count;
	if (made < 0) {
		say("No room for another.");
		return;
	}
	tab = 0;
	choose(made);
	changed();
}

/* ---- scenes in files (docs/apps/raytrace.md, "Example scenes") ---- */

/* The next word of a line, one in quotes with its spaces; NULL at the end or at a '#'. */
static char *
next_word(char **at)
{
	char *p = *at, *word;

	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (!*p || *p == '#')
		return *at = p, NULL;
	if (*p == '"')
		for (word = ++p; *p && *p != '"'; p++)
			continue;
	else
		for (word = p; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'; p++)
			continue;
	if (*p)
		*p++ = 0;
	*at = p;
	return word;
}

/* `count` numbers of the line: false when they are not there. */
static int
numbers(char **at, float *out, int count)
{
	char *word, *end;
	int i;

	for (i = 0; i < count; i++) {
		if (!(word = next_word(at)))
			return 0;
		out[i] = strtof(word, &end);
		if (end == word || *end)
			return 0;
	}
	return 1;
}

/* object NAME MESH, then what it is: MESH is plane, box, ball or a file (from the scene's folder). */
static const char *
scene_object(char **at, const char *folder)
{
	static const char *const made[6] = {"rough", "metal", "glass", "index", "density", "glow"};
	char *name = next_word(at), *mesh = next_word(at), *key, path[300];
	struct sc_object *o;
	float f[3];
	int m = -1, i, k;

	if (!name || !mesh)
		return "an object wants a name and a mesh";
	if (!strcmp(mesh, "plane") || !strcmp(mesh, "box") || !strcmp(mesh, "ball")) {
		for (i = 0; i < sc.mesh_count && m < 0; i++)
			if (!strcmp(sc.meshes[i].name, mesh))
				m = i;
		if (m < 0)
			m = mesh[1] == 'l' ? sc_mesh_plane() : mesh[1] == 'o' ? sc_mesh_box() : sc_mesh_ball(16);
	} else {
		snprintf(path, sizeof path, "%s%s", mesh[0] == '/' ? "" : folder, mesh);
		m = import_mesh(path);
	}
	if (m < 0 || (i = add_object(m, 1.0f, SC_MATT, 0.8f, 0.8f, 0.8f)) < 0)
		return "its mesh could not be read, or there is no room for it";
	o = &sc.objects[i];
	sc.object_count--;	/* (its own name is not another's) */
	name_free(o->name, name);
	sc.object_count++;
	for (k = 0; k < 3; k++)
		o->size[k] = 1.0f, o->place[k] = 0.0f;
	while ((key = next_word(at))) {
		for (i = 0; i < 6 && strcmp(key, made[i]); i++)
			continue;
		if (i < 6) {
			if (!numbers(at, f, 1))
				return "a number is missing";
			*(i == 0 ? &o->rough : i == 1 ? &o->metal : i == 2 ? &o->glass : i == 3 ? &o->ior : i == 4 ? &o->density : &o->glow) = f[0];
		} else if (!strcmp(key, "kind")) {
			key = next_word(at);
			for (i = 0; key && i < SC_KINDS && strcasecmp(key, kind_names[i] + 1); i++)
				continue;
			if (!key || i == SC_KINDS)
				return "kind is matt, plastic, metal, mirror, glass or glow";
			sc_material(o, i);
		} else if (!strcmp(key, "fit") || !strcmp(key, "scale")) {
			if (!numbers(at, f, 1))
				return "a number is missing";
			if (key[0] == 'f')
				fit_object(o, f[0]);
			else
				o->size[0] = o->size[1] = o->size[2] = f[0];
		} else if (!strcmp(key, "colour") || !strcmp(key, "place") || !strcmp(key, "move") || !strcmp(key, "turn") || !strcmp(key, "size")) {
			float *to = key[0] == 'c' ? o->colour : key[0] == 't' ? o->turn : key[0] == 's' ? o->size : o->place;

			if (!numbers(at, f, 3))
				return "three numbers are missing";
			for (k = 0; k < 3; k++)
				to[k] = key[0] == 'm' ? to[k] + f[k] : f[k];
		} else
			return "an object has no such thing";
	}
	sc_object_matrix(o);
	return NULL;
}

/* light NAME place X Y Z radius R power P colour R G B */
static const char *
scene_light(char **at)
{
	char *name = next_word(at), *key;
	struct sc_light *l;
	int made;

	if (!name || (made = add_light(0.0f, 2.0f, 0.0f, 0.1f, 30.0f)) < 0)
		return "a light wants a name, and room";
	l = &sc.lights[made];
	sc.light_count--;
	name_free(l->name, name);
	sc.light_count++;
	while ((key = next_word(at)))
		if (!strcmp(key, "place") || !strcmp(key, "colour")) {
			if (!numbers(at, key[0] == 'p' ? l->place : l->colour, 3))
				return "three numbers are missing";
		} else if (!strcmp(key, "radius") || !strcmp(key, "power")) {
			if (!numbers(at, key[0] == 'r' ? &l->radius : &l->power, 1))
				return "a number is missing";
		} else
			return "a light has no such thing";
	return NULL;
}

/* camera target X Y Z turn T tilt T far F lens L; background colour R G B power P */
static const char *
scene_view(char **at, int camera)
{
	struct sc_camera *c = &sc.camera;
	char *key;

	while ((key = next_word(at))) {
		float *to = NULL;
		int count = 1;

		if (camera)
			to = !strcmp(key, "target") ? (count = 3, c->target) : !strcmp(key, "turn") ? &c->turn : !strcmp(key, "tilt") ? &c->tilt :
			     !strcmp(key, "far") ? &c->far : !strcmp(key, "lens") ? &c->lens : NULL;
		else
			to = !strcmp(key, "colour") ? (count = 3, sc.sky) : !strcmp(key, "power") ? &sc.sky_power : NULL;
		if (!to)
			return camera ? "a camera has no such thing" : "a background has no such thing";
		if (!numbers(at, to, count))
			return "a number is missing";
	}
	return NULL;
}

static char scene_name[100] = "my.scene", scene_folder[256] = "/root/Models";

/* A scene file in place of the scene there is: false (and the status line says why) when a
 * line of it is wrong, with what was before that line kept. */
static int
load_scene(const char *path)
{
	const char *leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path, *wrong = NULL;
	char line[400], folder[256], text[160], *at, *word;
	FILE *file = fopen(path, "r");
	int number = 0;

	if (!file) {
		snprintf(text, sizeof text, "%.100s could not be read.", leaf);
		say(text);
		return 0;
	}
	snprintf(folder, sizeof folder, "%.*s", (int)(leaf - path), path);
	snprintf(scene_name, sizeof scene_name, "%.99s", leaf);
	sc.object_count = sc.light_count = 0;
	sc.sky[0] = 0.6f, sc.sky[1] = 0.75f, sc.sky[2] = 1.0f, sc.sky_power = 0.0f;
	memset(&sc.camera, 0, sizeof sc.camera);
	sc.camera.target[1] = 0.45f, sc.camera.turn = 35.0f, sc.camera.tilt = 22.0f, sc.camera.far = 4.2f, sc.camera.lens = 0.6f;
	while (!wrong && fgets(line, sizeof line, file)) {
		number++;
		at = line;
		if (!(word = next_word(&at)))
			continue;
		wrong = !strcmp(word, "object") ? scene_object(&at, folder) : !strcmp(word, "light") ? scene_light(&at) :
			!strcmp(word, "camera") ? scene_view(&at, 1) : !strcmp(word, "background") ? scene_view(&at, 0) : "what is that?";
	}
	fclose(file);
	if (wrong) {
		snprintf(text, sizeof text, "%.60s, line %d: %s", leaf, number, wrong);
		fprintf(stderr, "nxpath: %s\n", text);
	} else
		snprintf(text, sizeof text, "%.100s", leaf);
	say(text);
	return !wrong;
}

/* The scene file a word of the command line names: a .scene file, or an example's name. */
static const char *
scene_named(const char *word)
{
	static char path[300];
	size_t length = strlen(word);

	if (length > 6 && !strcmp(word + length - 6, ".scene"))
		return word;
	snprintf(path, sizeof path, EXAMPLES "/%.200s.scene", word);
	return !strchr(word, '/') && !strchr(word, '.') && access(path, R_OK) == 0 ? path : NULL;
}

static void
open_scene(const char *path)
{
	load_scene(path);
	tab = TAB_OBJECT;
	choose(-1);
	changed();
}

static void
open_dialog(const char *path)
{
	if (path)
		open_scene(path);
	else
		panel_files(A_OPEN, "Open a scene", strcmp(scene_folder, "/root/Models") ? scene_folder : EXAMPLES, ".scene", NULL), draw_panel();
}

static void
write_numbers(FILE *file, const char *key, const float *v, int count)
{
	fprintf(file, " %s", key);
	while (count--)
		fprintf(file, " %.9g", *v++);
}

/* The scene as a file load_scene reads back the same; false when it could not be written. */
static int
save_scene(const char *path)
{
	FILE *file = fopen(path, "w");
	int i;

	if (!file)
		return 0;
	fprintf(file, "# A scene of nxpath (docs/apps/raytrace.md, \"Example scenes\")\ncamera");
	write_numbers(file, "target", sc.camera.target, 3), write_numbers(file, "turn", &sc.camera.turn, 1);
	write_numbers(file, "tilt", &sc.camera.tilt, 1), write_numbers(file, "far", &sc.camera.far, 1);
	write_numbers(file, "lens", &sc.camera.lens, 1);
	fprintf(file, "\nbackground");
	write_numbers(file, "colour", sc.sky, 3), write_numbers(file, "power", &sc.sky_power, 1);
	for (i = 0; i < sc.object_count; i++) {
		const struct sc_object *o = &sc.objects[i];

		fprintf(file, "\nobject \"%s\" \"%s\"", o->name, mesh_files[o->mesh][0] ? mesh_files[o->mesh] : sc.meshes[o->mesh].name);
		write_numbers(file, "size", o->size, 3), write_numbers(file, "place", o->place, 3), write_numbers(file, "turn", o->turn, 3);
		write_numbers(file, "colour", o->colour, 3), write_numbers(file, "rough", &o->rough, 1), write_numbers(file, "metal", &o->metal, 1);
		write_numbers(file, "glass", &o->glass, 1), write_numbers(file, "index", &o->ior, 1);
		write_numbers(file, "density", &o->density, 1), write_numbers(file, "glow", &o->glow, 1);
	}
	for (i = 0; i < sc.light_count; i++) {
		const struct sc_light *l = &sc.lights[i];

		fprintf(file, "\nlight \"%s\"", l->name);
		write_numbers(file, "place", l->place, 3), write_numbers(file, "radius", &l->radius, 1);
		write_numbers(file, "power", &l->power, 1), write_numbers(file, "colour", l->colour, 3);
	}
	fprintf(file, "\n");
	return fclose(file) == 0;
}

static void
save_dialog(const char *chosen)
{
	char path[512], text[160];
	const char *leaf;

	if (!chosen) {
		panel_files(A_SAVE, "Save the scene", scene_folder, ".scene", scene_name);
		draw_panel();
		return;
	}
	snprintf(path, sizeof path, "%s", chosen);
	ui_files_ending(path, sizeof path, ".scene");
	leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	if (!save_scene(path)) {
		snprintf(text, sizeof text, "%.100s could not be written.", leaf);
		say(text);
		return;
	}
	snprintf(scene_folder, sizeof scene_folder, "%.*s", (int)(leaf - path - (leaf > path + 1)), path);
	snprintf(scene_name, sizeof scene_name, "%.99s", leaf);
	snprintf(text, sizeof text, "The scene is written to %.100s.", leaf);
	say(text);
}

/* What a control or a menu's row asks for. */
static void
act(int id)
{
	int o = chosen_object();

	if (id >= A_LESS && id < A_SLIDER) {
		int up = id >= A_MORE, n = id - (up ? A_MORE : A_LESS), line = n / 3, k = n % 3;
		float step, *v = line_values(line, &step);

		if (!v)
			return;
		if (line == 2)	/* (a size grows by a tenth of itself, all three ways at once) */
			v[0] *= up ? 1.1f : 1.0f / 1.1f, v[1] *= up ? 1.1f : 1.0f / 1.1f, v[2] *= up ? 1.1f : 1.0f / 1.1f;
		else
			v[k] += up ? step : -step;
		changed();
		return;
	}
	switch (id) {
	case A_OPEN: open_dialog(NULL); break;
	case A_SAVE: save_dialog(NULL); break;
	case A_IMPORT: import_dialog(NULL); break;
	case A_EXPORT: export_dialog(NULL); break;
	case A_CLOSE: pt.stop = 2; exit(0);
	case A_PLANE: case A_BOX: case A_BALL: case A_LIGHT: add_thing(id); break;
	case A_DUPLICATE: duplicate(), changed(); break;
	case A_DELETE: delete_chosen(), changed(); break;
	case A_SCENE: showing = 0, view_dirty = 1, draw_panel(); break;
	case A_PICTURE:
		if (traced && !stale)
			showing = 2, view_dirty = 1, draw_panel();
		break;
	case A_WHOLE: whole_view(); break;
	case A_RENDER:
		tab = TAB_RENDER;
		if (!trace_more())
			invalidate(), wanted = 1;
		draw_panel();
		break;
	case A_STOP:
		if (tracing && !pt.stop)
			pt.stop = 1;
		stale = wanted = 0;
		draw_panel();
		break;
	case A_LIVE: live = !live, still_since = now_ms(), keep_settings(), tell(), draw_panel(); break;
	case A_SMOOTH: smooth = !smooth, view_dirty = 1, keep_settings(), draw_panel(); break;
	case A_TAB: case A_TAB + 1: case A_TAB + 2: tab = id - A_TAB, tell(), draw_panel(); break;
	case A_DETAIL: detail_at = (detail_at + 1) % DETAILS, keep_settings(), invalidate(), tell(), draw_panel(); break;
	case A_WHAT: case A_WHAT + 1: case A_WHAT + 2: what = id - A_WHAT, view_dirty = 1, draw_panel(); break;
	default:
		if (id >= A_EXAMPLE && id < A_EXAMPLE + EXAMPLE_COUNT) {
			char path[200];

			snprintf(path, sizeof path, EXAMPLES "/%s.scene", examples[id - A_EXAMPLE].file);
			open_scene(path);
		}
		break;
	case A_MATERIAL: case A_MATERIAL + 1: case A_MATERIAL + 2: case A_MATERIAL + 3: case A_MATERIAL + 4: case A_MATERIAL + 5:
		if (o >= 0)
			sc_material(&sc.objects[o], id - A_MATERIAL), changed();
		break;
	}
}

/* What the panel's frame asked for, in its order (panel.h): a slider moved or let go, a row chosen, a control. */
static void
asked(const struct panel_frame *f)
{
	int i;

	for (i = 0; i < f->asked_count; i++) {
		int id = f->asked[i];

		if (getenv("NXPATH_PRINT"))
			fprintf(stderr, "nxpath: the panel asked for %d (%d)\n", id, f->values[i]);

		if (id >= A_SLIDER && id < A_SLIDER + SLIDERS) {
			int n = id - A_SLIDER;

			slider_value(n, 1, f->values[i]);
			if (n != S_SAMPLES)
				changed();
			else if (!trace_more())
				invalidate();
			draw_panel();
		} else if (id >= A_SLIDE && id < A_SLIDE + SLIDERS) {
			/* (a slider let go: the picture's settings kept, and the still view's clock from now) */
			if (tab == TAB_RENDER)
				keep_settings();
			still_since = now_ms();
		} else if (id >= A_CHOOSE && id < A_SLIDE)
			choose(id - A_CHOOSE);
		else if (id == A_FILES) {
			/* (a file chosen: its dialog, by the action that asked) */
			const char *path = panel_files_path();

			if (f->values[i] == A_OPEN) open_dialog(path);
			else if (f->values[i] == A_SAVE) save_dialog(path);
			else if (f->values[i] == A_IMPORT) import_dialog(path);
			else if (f->values[i] == A_EXPORT) export_dialog(path);
		}
		else
			act(id);
	}
}

/* ---- the picture: the view, and what is clicked in it ---- */

static const volatile uint32_t *
control(int offset)
{
	return (const volatile uint32_t *)((const char *)seglPalette() - 0x400 + offset);
}

/* Where the pointer is in the window, and its buttons, from the machine's own words. */
static void
pointer_window(int *x, int *y, int *buttons)
{
	int ox, oy;

	ui_window_origin(window, &ox, &oy);
	*x = (int)*control(0x20) - ox, *y = (int)*control(0x24) - oy, *buttons = (int)*control(0x28);
}

/* The same in the view's pixels. */
static void
pointer_now(int *x, int *y, int *buttons)
{
	pointer_window(x, y, buttons);
	*y -= bar;
}

/* The thing an eye's ray through a point of the picture meets first, by each thing's ball:
 * its row of the list, or -1. */
static int
thing_at(int px, int py)
{
	const struct sc_camera *c = &sc.camera;
	float sx = (2.0f * ((float)px + 0.5f) / (float)view_w - 1.0f) * c->lens * (float)view_w / (float)view_h;
	float sy = (1.0f - 2.0f * ((float)py + 0.5f) / (float)view_h) * c->lens, way[3], len = 0.0f, best = 1e30f;
	int i, k, found = -1;

	for (k = 0; k < 3; k++)
		way[k] = c->ahead[k] + c->right[k] * sx + c->up[k] * sy, len += way[k] * way[k];
	for (len = sqrtf(len), k = 0; k < 3; k++)
		way[k] /= len;
	for (i = 0; i < sc.object_count + sc.light_count; i++) {
		float centre[3], radius = 0.0f, to[3], along, off2;

		if (i < sc.object_count) {
			const struct sc_object *o = &sc.objects[i];
			const struct sc_mesh *m = &sc.meshes[o->mesh];
			float mid[3], half[3];

			for (k = 0; k < 3; k++)
				mid[k] = (m->low[k] + m->high[k]) * 0.5f, half[k] = (m->high[k] - m->low[k]) * 0.5f * o->size[k];
			for (k = 0; k < 3; k++)
				centre[k] = o->matrix[k] * mid[0] + o->matrix[4 + k] * mid[1] + o->matrix[8 + k] * mid[2] + o->matrix[12 + k];
			/* (a flat thing, the ground, is picked by the others' misses only: its ball is everywhere) */
			radius = half[0] > half[2] ? half[0] : half[2];
			radius = half[1] * 8.0f < radius ? 0.0f : sqrtf(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]) * 0.8f;
		} else {
			const struct sc_light *l = &sc.lights[i - sc.object_count];

			for (k = 0; k < 3; k++)
				centre[k] = l->place[k];
			radius = l->radius < 0.08f ? 0.08f : l->radius;
		}
		for (k = 0; k < 3; k++)
			to[k] = centre[k] - c->eye[k];
		along = to[0] * way[0] + to[1] * way[1] + to[2] * way[2];
		off2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2] - along * along;
		if (along > 0.0f && off2 < radius * radius && along < best)
			best = along, found = i;
	}
	return found;
}

/* ---- the picture smoothed on the lanes ---- */

static GLuint smooth_texture;

/* Whether the picture is shown smoothed: a smoothing of it is there and it has few samples yet. */
static int
smooth_shown(void)
{
	return smooth && smooth_ready && phase == 1 && sample < SMOOTH_MOST;
}

/* Whether a picture is to be smoothed: shown on the lanes' field, or (NXPATH_SMOOTH=1) where there is none by this
 * core's plain calls, for a test; not while the field traces the paths itself. */
static int
smooth_here(void)
{
	static int asked;

	if (!smooth || use_field || !(mcw_field_lanes() > 0 || getenv("NXPATH_SMOOTH")))
		return 0;
	/* (its memory when it is first wanted, after the scene's meshes have theirs; none: not smoothed) */
	if (!smooth_pixels && !asked) {
		asked = 1;
		if ((smooth_pixels = seglMemory(MOST_W * MOST_H * 4)) != NULL)
			memset(smooth_pixels, 0, MOST_W * MOST_H * 4);
	}
	return smooth_pixels != NULL;
}

/* The picture's smoothing, from the main loop: a job's end shown; a new one when the picture has more samples
 * shown than the last was made of, every 250 ms at most. Not waited for. */
static void
smooth_poll(void)
{
	if (smoothing) {
		if (!mcw_field_ready())
			return;
		smoothing = 0, smooth_ready = 1, view_dirty = 1;
	}
	if (!smooth_here() || phase != 1 || !traced || stale || sample >= SMOOTH_MOST || sample == smoothed || now_ms() - smooth_at < 250)
		return;
	seglSettle();	/* (the list in flight may read the texture) */
	smooth_job.pixels = pt.pixels, smooth_job.out = smooth_pixels;
	smooth_job.width = pt.width, smooth_job.height = pt.height, smooth_job.samples = (float)sample;
	glBindTexture(GL_TEXTURE_2D, smooth_texture);
	seglTexturePointer(smooth_pixels, pt.width, pt.height, GL_RGBA);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	smoothing = 1, smoothed = sample, smooth_at = now_ms();
	mcw_field_begin(pt_smooth_texel, &smooth_job, 0, (uint32_t)(pt.width * pt.height / 4), smooth_pixels, 1);
}

static unsigned
cycles(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

/* For a test (NXPATH_SMOOTH=1, at a picture's end): the picture smoothed by this core, texel by texel, with the most
 * instructions a texel took and the smoothed picture's sum. */
static void
smooth_check(void)
{
	unsigned most = 0, all = 0, sum = 0, t, from;
	int i;

	if (!smooth_here())
		return;
	smooth_job.pixels = pt.pixels, smooth_job.out = smooth_pixels;
	smooth_job.width = pt.width, smooth_job.height = pt.height, smooth_job.samples = (float)sample;
	for (t = 0; t < (unsigned)(pt.width * pt.height / 4); t++) {
		unsigned again;

		/* (each texel twice, the fewer counted: a timer's interrupt in a call is the kernel's instructions) */
		from = cycles();
		pt_smooth_texel(t, &smooth_job);
		from = cycles() - from;
		again = cycles();
		pt_smooth_texel(t, &smooth_job);
		again = cycles() - again;
		from = again < from ? again : from;
		most = from > most ? from : most, all += from;
	}
	for (i = 0; i < pt.width * pt.height; i++)
		sum = sum * 31 + smooth_pixels[i];
	fprintf(stderr, "nxpath: smoothed %d x %d of %d samples: sum %08x, %u instructions a texel on average, the most %u\n", pt.width, pt.height,
		sample, sum, all / (unsigned)(pt.width * pt.height / 4), most);
}

/* A traced picture over the view: all of it, or a test's picture as large as fits. */
static void
draw_traced(void)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	int w = view_w, h = view_h;

	if (fixed_w) {
		h = view_w * fixed_h / fixed_w;
		if (h > view_h)
			h = view_h, w = view_h * fixed_w / fixed_h;
		glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	/* (OpenGL's y from the window's foot: the view lies on the line of figures) */
	glViewport((view_w - w) / 2, STATUS + (view_h - h) / 2, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(corners, uv, showing == 1 ? small_texture : smooth_shown() ? smooth_texture : texture, 255, 255, 0);
	glViewport(0, STATUS, view_w, view_h);
	shown_at = now_ms();
}

/* A frame: the view, the scene by OpenGL or the traced picture of it, and over it ImGui's menus, panel, line of
 * figures and the handles of what is selected; then what the panel asked for. */
static void
draw_view(void)
{
	float view[16], projection[16], m[16];
	int i, o = chosen_object(), l = chosen_light();
	struct panel_frame f;

	sc_camera_set(&sc.camera);
	view_matrices(view, projection);
	glViewport(0, STATUS, view_w, view_h);
	if (showing) {
		glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		draw_traced();
	}
	else {
		/* (behind the scene the sky, about as the traced picture shows it) */
		for (i = 0; i < 3; i++)
			m[i] = sqrtf(sc.sky[i] * sc.sky_power > 1.0f ? 1.0f : sc.sky[i] * sc.sky_power);
		glClearColor(m[0], m[1], m[2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(projection);
		glMatrixMode(GL_MODELVIEW);
		glEnable(GL_DEPTH_TEST);
		glDisable(GL_TEXTURE_2D);
		for (i = 0; i < sc.object_count; i++) {
			const struct shown_mesh *shown = sc.meshes[sc.objects[i].mesh].shown;

			glLoadMatrixf(view);
			glMultMatrixf(sc.objects[i].matrix);
			seglTagged(shown->vertices, shown->count, tables + 256 * i);
		}
		for (i = 0; i < sc.light_count && ball_mesh >= 0; i++) {
			const struct shown_mesh *shown = sc.meshes[ball_mesh].shown;

			light_matrix(&sc.lights[i], m);
			glLoadMatrixf(view);
			glMultMatrixf(m);
			seglTagged(shown->vertices, shown->count, tables + 256 * (SC_OBJECTS + i));
		}
	}
	memset(&f, 0, sizeof f);
	f.wide = wide, f.high = high, f.view_w = view_w, f.view_h = view_h;
	f.handles = have_gizmo && (o >= 0 || l >= 0) && !(showing && fixed_w);
	if (f.handles) {
		if (o >= 0)
			memcpy(m, sc.objects[o].matrix, sizeof m);
		else
			light_matrix(&sc.lights[l], m), m[0] = m[5] = m[10] = 1.0f;
		f.view = view, f.projection = projection, f.matrix = m, f.what = o >= 0 ? what : GIZMO_MOVE, f.drag = drag != 0;
	}
	glDisable(GL_DEPTH_TEST);
	panel_draw(&f);
	if (f.moved == 1 && o >= 0) {
		memcpy(sc.objects[o].matrix, m, sizeof m);
		from_matrix(&sc.objects[o]);
	} else if (f.moved == 1)
		sc.lights[l].place[0] = m[12], sc.lights[l].place[1] = m[13], sc.lights[l].place[2] = m[14];
	seglSwap();
	on_handle = f.moved;
	view_dirty = ui_dirty = 0, ui_at = now_ms();
	if (layout_asked) {
		layout_asked = 0;
		fprintf(stderr, "nxpath: tab %d, %d controls: %d over another, %d with words wider than their place, the lowest ends at %d of %d\n", tab,
			f.controls, f.over, f.cut, f.lowest, high - STATUS);
	}
	if (f.moved == 1) {
		invalidate();
		fill_tables();
		draw_panel();
	}
	asked(&f);
	if (view_press) {
		int free;

		if (panel_press_given(view_press, &free)) {
			if (free)
				press_view(view_x, view_y, view_button);
			view_press = 0;
		}
	}
}

static void
set_window(int w, int h)
{
	wide = w < PANEL + 80 ? PANEL + 80 : w, high = h < 200 ? 200 : h;
	view_w = wide - PANEL, view_h = high - STATUS - bar;
	seglWindowChanged();
	invalidate();
}

/* A button went down in the picture, not on a handle: the left one picks what is there and
 * turns the view while held, the right one moves the view. */
static void
press_view(int x, int y, int button)
{
	if (button == 1) {
		int row = thing_at(x, y);

		if (row != tree.selected)
			choose(row);
	}
	drag = button, drag_x = x, drag_y = y;
}

/* The view follows the pointer while a button is held in the picture. */
static void
drag_view(void)
{
	struct sc_camera *c = &sc.camera;
	int x, y, buttons, k;

	pointer_now(&x, &y, &buttons);
	if (!(buttons & (drag == 1 ? 1 : 2))) {
		drag = 0;
		still_since = now_ms();
		return;
	}
	if (x == drag_x && y == drag_y)
		return;
	if (drag == 1) {
		c->turn += (float)(x - drag_x) * 0.4f;
		c->tilt += (float)(y - drag_y) * 0.4f;
		c->tilt = c->tilt > 89.0f ? 89.0f : c->tilt < -89.0f ? -89.0f : c->tilt;
	} else
		for (k = 0; k < 3; k++)
			c->target[k] += (c->up[k] * (float)(y - drag_y) - c->right[k] * (float)(x - drag_x)) * c->far * 0.002f;
	drag_x = x, drag_y = y;
	invalidate();
}

static void
events(void)
{
	GR_EVENT e;
	int x, y;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return;
		ui_origin_note(&e);
		switch (e.type) {
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (e.general.wid != window)
				break;
			pt.stop = 2;
			exit(0);
		case GR_EVENT_TYPE_EXPOSURE:
			if (e.exposure.wid == window)
				view_dirty = 1;
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (e.update.utype == GR_UPDATE_SIZE && e.update.wid == window)
				set_window(e.update.width, e.update.height);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (e.button.wid != window)
				break;
			panel_pointer(e.button.x, e.button.y);
			if (e.button.changebuttons & GR_BUTTON_L) panel_button(0, 0);
			if (e.button.changebuttons & GR_BUTTON_R) panel_button(1, 0);
			if (e.button.changebuttons & GR_BUTTON_M) panel_button(2, 0);
			still_since = now_ms();
			view_dirty = 1;	/* (a handle let go: its library is told by the next frame) */
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (e.button.wid != window)
				break;
			x = e.button.x, y = e.button.y;
			panel_pointer(x, y);
			if (ui_wheel(&e)) {
				if (!panel_wants_pointer() && x < view_w && y >= bar && y < bar + view_h) {
					sc.camera.far *= ui_wheel(&e) > 0 ? 1.1f : 0.9f;
					sc.camera.far = sc.camera.far < 0.5f ? 0.5f : sc.camera.far > 60.0f ? 60.0f : sc.camera.far;
					invalidate();
				} else
					panel_wheel(ui_wheel(&e) > 0 ? 1 : -1);
				break;
			}
			{
				int n = 0;

				if (e.button.changebuttons & GR_BUTTON_L) n = panel_button(0, 1);
				if (e.button.changebuttons & GR_BUTTON_R) n = panel_button(1, 1);
				if (e.button.changebuttons & GR_BUTTON_M) panel_button(2, 1);
				/* (a press over ImGui's is ImGui's, a menu open over the view too: decided when a frame gave it ImGui; over a
				 * handle the left one is the handle's) */
				if (n && x < view_w && y >= bar && y < bar + view_h && !(have_gizmo && on_handle && (e.button.changebuttons & GR_BUTTON_L)))
					view_press = n, view_x = x, view_y = y - bar, view_button = e.button.changebuttons & GR_BUTTON_L ? 1 : 2;
			}
			view_dirty = 1;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_KEY_UP:
			/* (a number typed into a slider is ImGui's; the program's keys otherwise) */
			if (panel_typing()) {
				panel_key(e.keystroke.ch, e.keystroke.modifiers, e.type == GR_EVENT_TYPE_KEY_DOWN);
				break;
			}
			if (e.type == GR_EVENT_TYPE_KEY_UP)
				break;
			if ((e.keystroke.modifiers & MWKMOD_CTRL) && (e.keystroke.ch == 'd' || e.keystroke.ch == 4)) {
				act(A_DUPLICATE);
				break;
			}
			switch (e.keystroke.ch) {
			case MWKEY_DELETE: act(A_DELETE); break;
			case MWKEY_HOME: act(A_WHOLE); break;
			case 'g': act(A_WHAT + GIZMO_MOVE); break;
			case 'r': act(A_WHAT + GIZMO_TURN); break;
			case 's': act(A_WHAT + GIZMO_SIZE); break;
			case 'l': act(A_LIVE); break;
			case MWKEY_F5: act(A_RENDER); break;
			case MWKEY_ESCAPE: act(tracing ? A_STOP : A_SCENE); break;
			}
			break;
		}
	}
}

/* What the program starts with: a ground, the teapot (or a box, without its file), a mirror
 * ball and a light under a background that gives no light; and beside them the mesh it was
 * started with. Or the scene file it was started with: true then. */
static int
first_scene(const char *more)
{
	const char *scene = more ? scene_named(more) : NULL;
	int teapot, made;

	if (scene) {
		ball_mesh = sc_mesh_ball(16);
		show_mesh(ball_mesh);
		load_scene(scene);
		return 1;
	}
	teapot = access("/root/Models/teapot.obj", R_OK) == 0 ? import_mesh("/root/Models/teapot.obj") : -1;

	add_object(sc_mesh_plane(), 6.0f, SC_MATT, 0.62f, 0.6f, 0.56f);
	snprintf(sc.objects[0].name, SC_NAME, "Ground");
	made = add_object(teapot >= 0 ? teapot : sc_mesh_box(), 1.4f, SC_MATT, 0.85f, 0.45f, 0.25f);
	ball_mesh = sc_mesh_ball(16);
	show_mesh(ball_mesh);
	made = add_object(ball_mesh, 0.6f, SC_MIRROR, 0.95f, 0.95f, 0.95f);
	if (made >= 0) {
		sc.objects[made].place[0] = 1.1f, sc.objects[made].place[2] = -0.5f;
		snprintf(sc.objects[made].name, SC_NAME, "Ball");
	}
	add_light(-1.2f, 2.4f, -1.4f, getenv("NXPATH_RADIUS") ? (float)atoi(getenv("NXPATH_RADIUS")) * 0.01f : 0.12f, 50.0f);
	made = reg_int("pathtrace.background.colour", 0x99bfff);
	sc.sky[0] = (float)(made >> 16 & 255) / 255.0f, sc.sky[1] = (float)(made >> 8 & 255) / 255.0f, sc.sky[2] = (float)(made & 255) / 255.0f;
	sc.sky_power = (float)reg_int("pathtrace.background", 0) * 0.01f;
	sc.camera.target[1] = 0.45f, sc.camera.turn = 35.0f, sc.camera.tilt = 22.0f, sc.camera.far = 4.2f, sc.camera.lens = 0.6f;
	say("");
	if (more && (made = import_mesh(more)) >= 0 && (made = add_object(made, 1.0f, SC_MATT, 0.8f, 0.8f, 0.8f)) >= 0)
		sc.objects[made].place[0] = -1.2f;
	return 0;
}

int
main(int argc, char **argv)
{
	GR_SCREEN_INFO info;
	const char *text;
	int w, h, i;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxpath: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	wide = wide > info.cols - 20 ? info.cols - 20 : wide, high = high > info.rows - 70 ? info.rows - 70 : high;
	ui_init();
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Path tracer", GR_ROOT_WINDOW_ID, -1, -1, wide, high, BLACK);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	if (seglInit(window) < 0) {
		fprintf(stderr, "nxpath: this machine has no GPU\n");
		return 1;
	}
	/* (the window is ImGui's frames over OpenGL's: panel.cpp) */
	have_gizmo = panel_init(window);
	if (have_gizmo)
		bar = panel_bar();
	view_w = wide - PANEL, view_h = high - STATUS - bar;
	GrSelectEvents(window, EVENTS);	/* (ImGui's platform asked for others: the pointer is read from the machine's words) */
	full_pixels = seglMemory(MOST_W * MOST_H * 4);
	small_pixels = seglMemory(SMALL * 4);
	tables = seglMemory(256 * 4 * (SC_OBJECTS + SC_LIGHTS));
	pt.sums = mcw_alloc(MOST_W * MOST_H * 4 * sizeof(float));
	rounds_set = getenv("NXPATH_ROUNDS") ? atoi(getenv("NXPATH_ROUNDS")) : -1;
	pt.slots = mcw_alloc(MC_MAX_CORES * sizeof(struct pt_slot));
	if (!full_pixels || !small_pixels || !tables || !pt.sums || !pt.slots) {
		fprintf(stderr, "nxpath: no memory for the picture\n");
		return 1;
	}
	/* (this core's first stores there: a worker's would stop it) */
	memset(full_pixels, 0, MOST_W * MOST_H * 4);
	memset(small_pixels, 0, SMALL * 4);
	pt.pixels = full_pixels;
	glGenTextures(1, &texture);
	glGenTextures(1, &small_texture);
	glGenTextures(1, &smooth_texture);
	atexit(mcw_close);
	detail_at = reg_int("pathtrace.detail", detail_at) % DETAILS;
	samples_at = reg_int("pathtrace.samples", samples_at) % SAMPLE_STEPS;
	bounces = reg_int("pathtrace.turns", bounces);
	bounces = bounces < 1 || bounces > TURNS ? 8 : bounces;
	live = reg_int("pathtrace.live", live) != 0;
	smooth = reg_int("pathtrace.smooth", smooth) != 0;
	if ((text = getenv("NXPATH_LIVE")) != NULL)
		live = atoi(text);
	if ((text = getenv("NXPATH_SIZE")) != NULL && sscanf(text, "%dx%d", &w, &h) == 2 && w >= 8 && h >= 8 && w * h <= MOST_W * MOST_H)
		fixed_w = w & ~3, fixed_h = h;
	if ((text = getenv("NXPATH_SAMPLES")) != NULL)
		for (i = 0; i < SAMPLE_STEPS; i++)
			if (sample_steps[i] == atoi(text))
				samples_at = i;
	leave_when_done = getenv("NXPATH_EXIT") != NULL;
	if ((text = getenv("NXPATH_FIELD")) != NULL)
		field_wanted = atoi(text) != 0;
	take_workers((text = getenv("NXPATH_CORES")) != NULL ? atoi(text) : MC_MAX_CORES);
	if (first_scene(argc > 1 ? argv[1] : NULL))
		choose(-1);
	else
		choose(argc > 1 && sc.object_count > 3 ? sc.object_count - 1 : 1);
	changed();
	if ((text = getenv("NXPATH_WRITE")) != NULL)
		printf("nxpath: %s %s\n", text, save_scene(text) ? "written" : "not written");
	if (getenv("NXPATH_AUTO"))
		begin_trace();
	for (;;) {
		static int last_x, last_y;
		int x, y, buttons;

		events();
		if (drag)
			drag_view();
		if (tracing)
			trace_on();
		/* (ImGui's controls and the handles light up under the pointer, and follow it: a frame when it has moved) */
		pointer_window(&x, &y, &buttons);
		if (have_gizmo && (x != last_x || y != last_y) && x >= 0 && y >= 0 && x < wide && y < high) {
			panel_pointer(x, y);
			ui_dirty = 1;
		}
		last_x = x, last_y = y;
		smooth_poll();
		if (tracing && showing && now_ms() - shown_at >= 400)
			view_dirty = 1;
		/* (the view has been still for a moment, with nothing held: its picture is begun) */
		if (stale && !tracing && (wanted || (live && !drag && !buttons && now_ms() - still_since >= STILL)))
			begin_trace();
		/* (a frame for the panel alone, a figure changed, at most every UI_MS; for input or the view at once) */
		if (view_dirty || (have_gizmo && panel_busy()) || (ui_dirty && now_ms() - ui_at >= UI_MS))
			draw_view();
		else if (workers || !tracing)
			mcw_rest();
	}
}
