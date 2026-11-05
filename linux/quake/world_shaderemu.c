/*
 * The level and the models as this machine's GPU takes them (docs/games/quake.md): vertices made
 * once, when a level or a model loads; the level kept in GPU memory; and what is in view found
 * from the leaves the viewer's leaf can see, not by a walk of the BSP tree.
 */
#include <stdlib.h>
#include "quakedef.h"
#include "mcwx.h"
#include "se_scene.h"	/* (what the frame's drawing reads: scene_shaderemu.c) */

void qglFan (const unsigned *vertices, int count);
void qglModel (const void *model, int pose, const unsigned char *vertices, const unsigned *coords, const unsigned *light,
	int count);
unsigned *qglLights (int key, int *fresh);
unsigned *qglLightWords (unsigned name);
void qglLightChanged (unsigned name);
void R_StoreEfrags (efrag_t **ppefrag);
void SE_NewMap (void);

int	se_map_serial;	/* goes up when the models go: what is remembered about a level is then stale */

typedef struct { int normal[3], dist; } plane_i;	/* normal in 1/16384ths, dist in units * 16384 */
typedef struct { short box[6]; } box_i;

static plane_i	*surface_planes;	/* of the world's surfaces, turned to face their front */
static box_i	*leaf_boxes;
static mleaf_t	**seen;			/* the leaves the viewer's leaf sees */
static const short **seen_box;		/* and the box of each */
static int	seen_count;
static mleaf_t	*seen_from;
static byte	*seen_out;		/* by place in `seen`: the view's plane the leaf was last outside of */
static int	seen_statics, seen_kept;	/* what the list was made with: it leaves out leaves with nothing to do */
static model_t	*made_for;

/* ---- when a level loads ---- */

static unsigned float_bits (float f)
{
	union { float f; unsigned u; } c = {f};

	return c.u;
}

/* texture coordinates as a compact vertex holds them, in 1,024ths */
static unsigned coord_bits (float s, float t)
{
	return ((unsigned)(int)(s * 1024.0f) & 0xffff) | (unsigned)(int)(t * 1024.0f) << 16;
}

/* A polygon's vertices as compact ones (docs/machine/gpu.md): with its texture, then with its light map. */
void SE_BuildPoly (glpoly_t *poly)
{
	int	i, n = poly->numverts;
	float	low_s = poly->verts[0][3], low_t = poly->verts[0][4];
	unsigned *to = Hunk_Alloc (n * 2 * 16);

	poly->compact = to;
	for (i = 1; i < n; i++) {
		if (poly->verts[i][3] < low_s)
			low_s = poly->verts[i][3];
		if (poly->verts[i][4] < low_t)
			low_t = poly->verts[i][4];
	}
	/* whole repeats of the texture off all of them: a vertex holds coordinates within 32 */
	low_s = floor (low_s), low_t = floor (low_t);
	for (i = 0; i < n; i++, to += 4) {
		const float *v = poly->verts[i];

		to[0] = to[n * 4] = float_bits (v[0]);
		to[1] = to[n * 4 + 1] = float_bits (v[1]);
		to[2] = to[n * 4 + 2] = float_bits (v[2]);
		to[3] = coord_bits (v[3] - low_s, v[4] - low_t);
		to[n * 4 + 3] = coord_bits (v[5], v[6]);
	}
}

/* ---- the level kept in GPU memory ---- */

/*
 * The level's polygons are written into GPU memory once, as quads sorted by texture and again
 * by light map; each run is a command that stays in every frame's list (docs/games/quake.md).
 * A frame then sends the level's matrices and what changed. The sky (two commands) and water (one
 * a texture, one face of two twins, its ripple written by workers) are kept after those runs.
 */
#define KEPT_SPECIAL	(SURF_DRAWSKY | SURF_DRAWTURB)
#define MAX_LIT		512

typedef struct { texture_t *texture; int quads, shown; } run_t;

extern int	lightmap_textures, solidskytexture, alphaskytexture, glwidth, glheight;

static int	kept_on, kept_runs, kept_anims, kept_hidden, world_drawn;
static int	level_sky, level_water;	/* the level has any */
static int	sky_run;		/* the first of the sky's two commands, or -1 */
static run_t	*runs;
static byte	*styled;		/* by surface: its light flickers or switches */
static int	styled_count;
static msurface_t *lit[2][MAX_LIT];	/* surfaces a moving light reaches: this frame's, the last one's */
static int	lit_count[2], lit_now;
static msurface_t **special_marks;	/* the surfaces of leaves with something special that are it */
static int	*special_first;		/* by leaf: where its are in that, and how many */
static byte	*leaf_special;		/* a leaf has water or sky in it (1), surfaces that flicker (2) */

static int kept_surface (const msurface_t *s)
{
	return !(s->flags & KEPT_SPECIAL) && s->polys && s->polys->numverts >= 3;
}

/* a polygon's fan of compact vertices as quads, two of its triangles each */
static unsigned *put_quads (unsigned *to, const unsigned *v, int n)
{
	int i, k, last;

	for (i = 1; i + 1 < n; i += 2, to += 16) {
		last = i + 2 < n ? i + 2 : n - 1;
		for (k = 0; k < 4; k++) {
			to[k] = v[k];
			to[4 + k] = v[4 * i + k];
			to[8 + k] = v[4 * (i + 1) + k];
			to[12 + k] = v[4 * last + k];
		}
	}
	return to;
}

/* ---- what workers do of a frame (docs/games/quake.md, "The other workers") ---- */

#define MAX_WARP	64		/* corners of one of water's polygons (they are cut into small ones) */
#define MAX_STYLED	4096		/* surfaces whose light flickers or switches */
#define LIGHT_ROW	128		/* a light map's row, in words (gl_rsurf.c) */
#define LIGHT_CALLS	1		/* the light maps' shares, a worker's call each */
enum { HOW_FRAME, HOW_WAIT, HOW_APART };

extern float	turbsin[];
extern cvar_t	gl_flashblend, r_fullbright, r_wateralpha;
extern int	lightmap_textures;
int		se_lights_changed;	/* a style's brightness is another (R_AnimateLight) */

/* What a variable says of a piece of the frame: "frame", this core in the frame, as before;
 * "wait", its workers, waited for; else its workers at their own pace. Without them, the first. */
/* A job's code is read here first: a worker stops at a page of it that no core has run yet,
 * until this core next asks after it. */
static void text_here (const void *function)
{
	mcw_touch_code ((const void *)((unsigned)function & ~4095u), 2 * 4096);
}

static int how_of (const char *variable, int first, int workers)
{
	const char *how = getenv (variable);

	if ((how && !strcmp (how, "frame")) || SE_Workers () < first + workers - 1)
		return HOW_FRAME;
	return how && !strcmp (how, "wait") ? HOW_WAIT : HOW_APART;
}

/* floorf, which is a call, for numbers an int holds */
static inline float floor_of (float f)
{
	float whole = (float)(int)f;

	return whole > f ? whole - 1 : whole;
}

#include "se_real.h"	/* (the light maps', the water's and the frame's jobs: the first core's) */

/* -- light maps that flicker: made again by workers when a style's brightness changes -- */

typedef struct { const byte *samples; unsigned *to; unsigned char wide, high, page, maps; byte styles[MAXLIGHTMAPS]; } styled_t;
static styled_t	styled_surfs[MAX_STYLED] __attribute__ ((aligned (16)));
/* the brightness of its styles each one's light map was made with: its worker's to write */
static int	styled_cached[MAX_STYLED][MAXLIGHTMAPS] __attribute__ ((aligned (16)));
static struct { int values[MAX_LIGHTSTYLES]; int count, unused[3]; } light_job __attribute__ ((aligned (16)));
static int	light_how = -1, light_on, light_was;
static se_group_t	light_out;	/* the workers' calls, while they are out */
/* The pages of light maps the workers make, each copied into the VRAM again (qglLightChanged) in a list after every frame
 * their job is out and the first after it is joined: the GPU reads them there, not in the pool the workers write. The
 * core that gives the job out raises light_gen (with the scene on a worker, the first core), and the core that draws
 * makes the uploads when it sees a count it has not (light_seen): GL is that core's alone. A word each, texels apart. */
static unsigned char	light_pages[64];
static int	light_page_count, light_upload;
static volatile unsigned	light_gen __attribute__ ((aligned (16)));
static unsigned	light_seen __attribute__ ((aligned (16)));

/* A worker's share: the light maps of every `step`th page of them, so that no two workers
 * store to the same 16 bytes. GLQuake's sum of a surface's maps by their styles (R_BuildLightMap). */
static uint32_t light_share (uint32_t first, uint32_t step)
{
	int	i, m, x, y, made = 0;

	for (i = 0; i < light_job.count; i++) {
		const styled_t	*s = &styled_surfs[i];
		int		*was = styled_cached[i], scale[MAXLIGHTMAPS], differs = 0, size = s->wide * s->high;
		const byte	*light = s->samples;
		unsigned	*to = s->to;

		if (s->page % step != first)
			continue;
		for (m = 0; m < s->maps; m++) {
			scale[m] = light_job.values[s->styles[m]];
			differs |= scale[m] ^ was[m];
		}
		if (!differs)
			continue;
		for (m = 0; m < s->maps; m++)
			was[m] = scale[m];
		for (y = 0; y < s->high; y++, to += LIGHT_ROW, light += s->wide)
			for (x = 0; x < s->wide; x++) {
				unsigned	v = 0;

				for (m = 0; m < s->maps; m++)
					v += light[m * size + x] * (unsigned)scale[m];
				v >>= 7;
				to[x] = (v > 255 ? 255 : v) * 0x010101u;
			}
		made++;
	}
	return (uint32_t)made;
}

/* The level's surfaces with such light, and where each one's is in its light map in GPU memory. */
static void light_build (model_t *world)
{
	msurface_t	*s;
	int		i, m, n = 0;

	light_on = 0;
	light_job.count = 0;
	light_page_count = light_upload = 0;
	if (light_how < 0) {
		light_how = how_of ("QUAKE_LIGHT", SE_WORKER_LIGHT, SE_LIGHT_WORKERS);
		text_here (light_share);
		mcw_touch (styled_surfs, sizeof styled_surfs);
		mcw_touch (styled_cached, sizeof styled_cached);
		mcw_touch (&light_job, sizeof light_job);
	}
	if (light_how == HOW_FRAME || !world->lightdata)
		return;
	for (i = 0, s = world->surfaces; i < world->numsurfaces; i++, s++) {
		styled_t	*e = &styled_surfs[n];
		unsigned	*words;
		int		any = 0;

		for (m = 0; m < MAXLIGHTMAPS && s->styles[m] != 255; m++)
			any |= s->styles[m] != 0;
		if (!any || !s->samples || (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB)))
			continue;
		/* (too many, or a light map that cannot stay where it is: this core's, as before) */
		if (n == MAX_STYLED || !qglStay (lightmap_textures + s->lightmaptexturenum) ||
		    (words = qglLightWords (lightmap_textures + s->lightmaptexturenum)) == NULL)
			return;
		e->samples = s->samples;
		e->to = words + s->light_t * LIGHT_ROW + s->light_s;
		e->wide = (s->extents[0] >> 4) + 1, e->high = (s->extents[1] >> 4) + 1;
		e->page = s->lightmaptexturenum, e->maps = m;
		for (m = 0; m < light_page_count && light_pages[m] != e->page; m++)
			;
		if (m == light_page_count)
			light_pages[light_page_count++] = e->page;
		m = e->maps;
		for (m = 0; m < MAXLIGHTMAPS; m++) {
			e->styles[m] = s->styles[m];
			styled_cached[n][m] = s->cached_light[m];
		}
		n++;
	}
	light_job.count = n;
	light_on = 1;
	light_was = 1;
	se_lights_changed = 1;
}

static void light_settle (void)
{
	SE_GroupJoin (&light_out);
}

/* Whether the workers have the flickering light maps this frame (and R_RenderBrushPoly leaves them be). */
int SE_LightsApart (void)
{
	return light_on && gl_flashblend.value && !r_fullbright.value;
}

/* A frame: when a style's brightness has changed and the workers are free, they are told. */
static void light_frame (void)
{
	int k, i;

	if (!light_was) {
		/* this core made them meanwhile (a setting): what the workers know of them is stale */
		light_settle ();
		for (i = 0; i < light_job.count; i++)
			styled_cached[i][0] = -1;
		light_was = se_lights_changed = 1;
	}
	if (!se_lights_changed || !light_job.count)
		return;
	if (light_out.n) {
		if (!SE_GroupReady (&light_out))
			return;
		SE_GroupJoin (&light_out);
	}
	se_lights_changed = 0;
	memcpy (light_job.values, d_lightstylevalue, sizeof light_job.values);
	if (light_how == HOW_WAIT)
		seglSettle ();	/* (the list being drawn reads the light maps as they are) */
	/* (two calls: in a demo the styles change nearly every frame, and a call is a cost of its own here) */
	for (k = 0; k < LIGHT_CALLS; k++)
		SE_GroupAdd (&light_out, light_share, k, LIGHT_CALLS);
	light_upload = 2;
	if (light_how == HOW_WAIT)
		light_settle ();
}

/* The light maps' job for the frame, if the styles have changed (on this core: it is a call). */
void SE_LightFrame (void)
{
	if (kept_on && SE_LightsApart ())
		light_frame ();
	else
		light_was = 0;
	if (light_out.n) {
		/* (a job done and not yet joined, as no style changed since: joined, so the pages stop going again) */
		if (SE_GroupReady (&light_out))
			SE_GroupJoin (&light_out);
		light_upload = 2;
	}
	if (light_upload > 0) {
		light_upload--;
		light_gen++;
	}
}

/* The drawing core's: the pages into the VRAM again, if the light job has been out since it last looked. */
static void light_uploads (void)
{
	unsigned	gen = light_gen;
	int		i;

	if (gen == light_seen)
		return;
	light_seen = gen;
	for (i = 0; i < light_page_count; i++)
		qglLightChanged (lightmap_textures + light_pages[i]);
}

int SE_LightWait (void) { return light_how == HOW_WAIT; }

/* Whether the level is made for the GPU already (the first frame of a level makes it). */
int SE_WorldMade (void) { return made_for && made_for == cl.worldmodel; }

/* -- water: kept in GPU memory, and its ripple written there by workers -- */

typedef struct { msurface_t *surf; unsigned *quads; } water_t;
static water_t	*water_surfs;
static int	*water_of;		/* by surface: which of water_surfs, or -1 */
static int	*water_seen[2], water_seen_count[2], water_seen_quads[2], water_now;	/* those the viewer's leaf sees */
static int	water_busy, water_apart_last, water_workers;	/* the frame's jobs ripple it (SE_FrameJobs) */
static int	water_how = -1, water_on, water_count, water_first_run, water_runs, water_shown;
static unsigned	*water_stamp, water_serial;
static struct { const int *list; int count; float clock; int unused; } water_job __attribute__ ((aligned (16)));

static inline unsigned warp_bits (float s, float t)
{
	return ((unsigned)(int)(s * 1024.0f) & 0xffff) | (unsigned)(int)(t * 1024.0f) << 16;
}

/* GLQuake's EmitWaterPolys for a surface's quads where they are kept: each corner's texture
 * coordinates ripple by a table of sines and the clock. Only that word of a vertex is stored. */
static void water_warp (const water_t *w, float clock)
{
	unsigned	word[MAX_WARP], *to = w->quads;
	glpoly_t	*p;
	const float	*v;
	float		s, t, base_s = 0, base_t = 0, step = 256 / (2 * 3.14159265f);
	int		i, n;

	for (p = w->surf->polys; p; p = p->next) {
		n = p->numverts < MAX_WARP ? p->numverts : MAX_WARP;
		for (i = 0, v = p->verts[0]; i < n; i++, v += VERTEXSIZE) {
			s = (v[3] + turbsin[(int)((v[4] * 0.125f + clock) * step) & 255]) * (1.0f / 64);
			t = (v[4] + turbsin[(int)((v[3] * 0.125f + clock) * step) & 255]) * (1.0f / 64);
			if (!i)
				base_s = floor_of (s) - 1, base_t = floor_of (t) - 1;	/* whole repeats off: see SE_BuildPoly */
			word[i] = warp_bits (s - base_s, t - base_t);
		}
		for (i = 1; i + 1 < n; i += 2, to += 16) {
			to[3] = word[0], to[7] = word[i], to[11] = word[i + 1];
			to[15] = word[i + 2 < n ? i + 2 : n - 1];
		}
	}
}

static uint32_t water_share (uint32_t first, uint32_t step)
{
	int	i;

	for (i = (int)first; i < water_job.count; i += (int)step)
		water_warp (&water_surfs[water_job.list[i]], water_job.clock);
	return 0;
}

/*
 * Water is two faces on one plane, one a side (the BSP's), and the GPU draws both: by its own
 * cuts each, which fight. By surface, the earlier face it is the other side of, or -1.
 */
static int *water_twins (model_t *world)
{
	int		n = world->nummodelsurfaces, size = 64, i, e, k, *twin, *table;
	unsigned	*key;
	msurface_t	*s, *o;

	while (size < 2 * n)
		size *= 2;
	key = malloc ((n + size) * sizeof (int));
	if (!key)
		return NULL;
	twin = Hunk_AllocName (n * sizeof (int), "water"), table = (int *)key + n;
	for (i = 0; i < size; i++)
		table[i] = -1;
	for (i = 0, s = world->surfaces; i < n; i++, s++) {
		unsigned sum = 0;

		twin[i] = -1;
		if (!(s->flags & SURF_DRAWTURB))
			continue;
		/* the corners' sum, which either side's winding has */
		for (e = 0; e < s->numedges; e++) {
			int		edge = world->surfedges[s->firstedge + e];
			const float	*v = world->vertexes[edge >= 0 ? world->edges[edge].v[0] : world->edges[-edge].v[1]].position;

			sum += (unsigned)(int)(v[0] * 8) * 3 + (unsigned)(int)(v[1] * 8) * 5 + (unsigned)(int)(v[2] * 8) * 7;
		}
		key[i] = sum = sum * 2654435761u ^ (unsigned)s->numedges;
		for (k = sum & (size - 1); table[k] >= 0; k = (k + 1) & (size - 1)) {
			o = world->surfaces + table[k];
			if (key[table[k]] == sum && o->plane == s->plane && ((o->flags ^ s->flags) & SURF_PLANEBACK) &&
			    o->texinfo->texture == s->texinfo->texture && o->numedges == s->numedges && twin[table[k]] < 0)
				break;
		}
		if (table[k] >= 0)
			twin[i] = table[k];
		else
			table[k] = i;
	}
	free (key);
	return twin;
}

static int water_quads_of (const msurface_t *s)
{
	const glpoly_t	*p;
	int		quads = 0;

	for (p = s->polys; p; p = p->next)
		quads += p->numverts >= 3 ? ((p->numverts < MAX_WARP ? p->numverts : MAX_WARP) - 1) / 2 : 0;
	return quads;
}

/* A frame: the workers ripple the water the viewer's leaf saw last frame, as often as they get
 * round. How many shares of it there are (0: none this frame). */
static int water_due (void)
{
	if (!water_apart_last || !water_seen_count[water_now])
		return 0;
	water_job.list = water_seen[water_now];
	water_job.count = water_seen_count[water_now];
	water_job.clock = cl.time;
	/* (a worker keeps some 190 vertices a pass, and a frame is four passes or more) */
	water_workers = 1 + water_seen_quads[water_now] / 160;
	if (water_workers > SE_WATER_WORKERS)
		water_workers = SE_WATER_WORKERS;
	if (water_how == HOW_WAIT)
		seglSettle ();
	water_busy = 1;
	return water_workers;
}

/*
 * The frame's jobs, given out once a frame when the entities are placed (cl_main.c): one call has
 * the light under the models that moved (gl_rlight.c), the water's ripple and the particles
 * (r_part.c), and a fight's particles or much water take more calls. Each call is a cost of its
 * own to this core, which was most of what a job a piece cost.
 */
#define FRAME_MORE	8
static mcw_handle	frame_first, frame_more[FRAME_MORE];
static int		frame_out, frame_first_joined, frame_more_count;

static uint32_t frame_job (uint32_t parts, uint32_t particles)
{
	if (parts & 1)
		SE_LightAheadJob ();
	if (parts & 2)
		water_share (0, water_workers);
	if (particles)
		SE_ParticlesJob (particles, 0);
	return 0;
}

/* Whether the first call has ended (it is joined then): the light it found may be used. */
int SE_FrameFirst (void)
{
	if (frame_out && !frame_first_joined && mcw_ready (frame_first)) {
		mcw_join (frame_first);
		frame_first_joined = 1;
	}
	return !frame_out || frame_first_joined;
}

/* 1 when none of the frame's jobs is out: what they made is taken. */
int SE_FramePoll (void)
{
	int i;

	if (!frame_out)
		return 1;
	if (!SE_FrameFirst ())
		return 0;
	for (i = 0; i < frame_more_count; i++)
		if (!mcw_ready (frame_more[i]))
			return 0;
	for (i = 0; i < frame_more_count; i++)
		mcw_join (frame_more[i]);
	frame_more_count = frame_out = water_busy = 0;
	SE_ParticlesTaken ();
	return 1;
}

void SE_FrameSettle (void)
{
	while (!SE_FramePoll ())
		mcw_rest ();
	SE_GunSettle ();
}

void SE_FrameJobs (void)
{
	static int	frame = -1;
	unsigned	particles[SE_PARTICLE_WORKERS] = {0};
	int		parts = 0, calls, k;

	if (frame == host_framecount)
		return;		/* (once a frame: when the entities are placed, or when the frame is given out) */
	frame = host_framecount;
	SE_ParticlesClock ();
	if (!SE_FramePoll ())
		return;
	if (SE_LightAheadPrepare ())
		parts |= 1;
	if (water_how != HOW_FRAME && water_due ())
		parts |= 2;
	calls = SE_ParticlesGive (particles);
	if (!parts && !calls)
		return;
	frame_first = mcw_call (frame_job, parts, particles[0]);
	frame_out = 1;
	frame_first_joined = 0;
	for (k = 1; (parts & 2) && k < water_workers; k++)
		frame_more[frame_more_count++] = mcw_call (water_share, k, water_workers);
	for (k = 1; k < calls; k++)
		frame_more[frame_more_count++] = mcw_call (SE_ParticlesJob, particles[k], 0);
	if (((parts & 2) && water_how == HOW_WAIT) || SE_ParticlesWait ())	/* (all of them: the next frame gives none while any is out) */
		SE_FrameSettle ();
}

#include "se_scene.h"

/* -- brush models (doors, boxes of ammunition): their triangles kept as the level's quads are -- */

#define MAX_BRUSH	256
typedef struct { texture_t *texture; int light, count; unsigned *triangles; unsigned drawn[24]; } brush_run_t;
typedef struct { int first, textures, lights; } brush_t;
static brush_t		brushes[MAX_BRUSH];
static brush_run_t	*brush_runs;
static int		brush_count, brush_how = -1;
void qglTrianglesAt (const unsigned *vertices, int count, unsigned *kept);
void seglKeptLaidHere (int index, int u, int v, int du, int dv);	/* gles.c */

/* How many vertices a brush model's triangles are, twice over (0: it is drawn a polygon at a time). */
static int brush_vertices (const model_t *m)
{
	const msurface_t	*s = m->surfaces + m->firstmodelsurface;
	int			i, vertices = 0;

	for (i = 0; i < m->nummodelsurfaces; i++, s++) {
		if ((s->flags & KEPT_SPECIAL) || !s->polys || s->polys->next || s->polys->numverts < 3 || !s->polys->compact ||
		    s->lightmaptexturenum < 0 || s->lightmaptexturenum >= 64)
			return 0;
		vertices += (s->polys->numverts - 2) * 6;
	}
	return vertices;
}

static unsigned *put_triangles (unsigned *to, const unsigned *v, int n)
{
	int i;

	for (i = 1; i + 1 < n; i++, to += 12) {
		memcpy (to, v, 16);
		memcpy (to + 4, v + 4 * i, 32);
	}
	return to;
}

/* A model's runs: its surfaces by texture, in the order the textures first come, then by light map. */
static unsigned *brush_build (model_t *m, unsigned *to)
{
	msurface_t	*s, *first = m->surfaces + m->firstmodelsurface;
	brush_t		*b = &brushes[brush_count];
	brush_run_t	*r = brush_runs + b->first;
	int		i, k, page;

	b->textures = b->lights = 0;
	for (i = 0, s = first; i < m->nummodelsurfaces; i++, s++) {
		for (k = 0; k < i && first[k].texinfo->texture != s->texinfo->texture; k++)
			continue;
		if (k < i)
			continue;
		r->texture = s->texinfo->texture, r->light = -1, r->triangles = to, r->drawn[0] = 0;
		for (k = i; k < m->nummodelsurfaces; k++)
			if (first[k].texinfo->texture == r->texture)
				to = put_triangles (to, first[k].polys->compact, first[k].polys->numverts);
		r->count = (int)(to - r->triangles) / 4;
		r++, b->textures++;
	}
	for (page = 0; page < 64; page++) {
		r->texture = NULL, r->light = page, r->triangles = to, r->drawn[0] = 0;
		for (k = 0, s = first; k < m->nummodelsurfaces; k++, s++)
			if (s->lightmaptexturenum == page)
				to = put_triangles (to, s->polys->compact + s->polys->numverts * 4, s->polys->numverts);
		if (to == r->triangles)
			continue;
		r->count = (int)(to - r->triangles) / 4;
		r++, b->lights++;
	}
	b[1].first = b->first + b->textures + b->lights;
	m->se_brush = ++brush_count;
	m->se_brush_serial = se_map_serial;
	return to;
}

int SE_BrushKept (entity_t *e)
{
	const model_t *m = e->model;

	return m->se_brush && m->se_brush_serial == se_map_serial && kept_on && SE_LightsApart ();
}

/* Its draws with its textures, or with its light maps (R_DrawBrushModel sets the blending). */
void SE_BrushDraw (entity_t *e, int light)
{
	const brush_t		*b = &brushes[e->model->se_brush - 1];
	brush_run_t		*r = brush_runs + b->first + (light ? b->textures : 0);
	int			n = light ? b->lights : b->textures;

	for (; n > 0; n--, r++) {
		GL_Bind (light ? lightmap_textures + r->light : R_TextureAnimation (r->texture)->gl_texturenum);
		qglTrianglesAt (r->triangles, r->count, r->drawn);
	}
}

static unsigned	place[MAX_EDICTS][3];	/* where each entity was when last asked about, as bits */

/* Is this entity where it was the last time this was asked? (And now it is noted there.) */
int SE_SamePlace (int number, const float *origin)
{
	const unsigned	*now = (const unsigned *)origin;
	unsigned	*was = place[number];
	int		same = was[0] == now[0] && was[1] == now[1] && was[2] == now[2];

	was[0] = now[0], was[1] = now[1], was[2] = now[2];
	return same;
}

void SE_ClearMap (void)
{
	/* (the workers read the level that is going) */
	SE_SceneOwn ();
	light_settle ();
	SE_FrameSettle ();
	light_on = water_on = 0;
	light_job.count = 0;
	water_seen_count[0] = water_seen_count[1] = 0;
	se_map_serial++;
	memset (place, 0xff, sizeof place);
	if (kept_on) {
		seglKeep (0, 0);
		qglRelease ();
	}
	kept_on = kept_runs = styled_count = 0;
	lit_count[0] = lit_count[1] = 0;
	seen_from = NULL;
	made_for = NULL;
}

static void kept_build (model_t *world)
{
	int		surfaces = world->nummodelsurfaces, i, t, m, quads = 0, count = 0, at = 0, first, sky = 0;
	msurface_t	*s;
	unsigned	*block, *to, corners[64 * 4];
	texture_t	*frame;
	glpoly_t	*p;
	int		slots = world->numtextures + 64, *first_of, *next_of, *slot_of_info;
	int		water = 0, sky_put, r, brush = 0, brush_most = 0, *twin = NULL, twins = 0;
	struct { texture_t *texture; int first, quads; } *water_run = NULL;

	kept_on = water_on = 0;
	level_sky = level_water = 0;
	if (COM_CheckParm ("-nokeep"))
		return;
	sky_run = -1;
	if (water_how < 0) {
		water_how = how_of ("QUAKE_WATER", SE_WORKER_WATER, SE_WATER_WORKERS);
		text_here (water_share);
		mcw_touch (&water_job, sizeof water_job);
	}
	if (water_how != HOW_FRAME && r_wateralpha.value == 1.0f)
		twin = water_twins (world);
	for (i = 0, s = world->surfaces; i < surfaces; i++, s++) {
		if (kept_surface (s))
			quads += (s->polys->numverts - 1) / 2;
		level_sky |= (s->flags & SURF_DRAWSKY) != 0;
		if (s->flags & SURF_DRAWTURB) {
			level_water = 1;
			if (twin && twin[i] >= 0)
				twins++;
			else if (water_how != HOW_FRAME && r_wateralpha.value == 1.0f)
				water += water_quads_of (s);
		}
		if (s->flags & SURF_DRAWSKY)
			for (p = s->polys; p; p = p->next)
				sky += p->numverts >= 3 && p->numverts <= 64 ? (p->numverts - 1) / 2 : 0;
	}
	if (!quads)
		return;
	/* the other brush models there are: the level's own (doors), and those of files (boxes) */
	brush_count = 0;
	if (brush_how < 0)
		brush_how = !(getenv ("QUAKE_BRUSH") && !strcmp (getenv ("QUAKE_BRUSH"), "frame"));
	for (i = 2; brush_how && i < MAX_MODELS && cl.model_precache[i]; i++) {
		model_t *model = cl.model_precache[i];

		if (model->type == mod_brush && (m = brush_vertices (model)) != 0)
			brush += m, brush_most += 2 * model->nummodelsurfaces;
	}
	block = qglBlock ((quads * 2 + sky + water) * 64 + brush * 16);
	brush_runs = Hunk_AllocName ((brush_most + 1) * sizeof (brush_run_t), "brushes");
	runs = Hunk_AllocName ((2 * world->numtextures + 64) * sizeof (run_t), "runs");
	styled = Hunk_AllocName (world->numsurfaces, "styled");
	if (water) {
		int n = 0;

		for (i = 0, s = world->surfaces; i < surfaces; i++, s++)
			n += (s->flags & SURF_DRAWTURB) != 0;
		water_surfs = Hunk_AllocName (n * sizeof (water_t), "water");
		water_of = Hunk_AllocName (surfaces * sizeof (int), "water");
		water_seen[0] = Hunk_AllocName (2 * n * sizeof (int), "water");
		water_seen[1] = water_seen[0] + n;
		water_stamp = Hunk_AllocName (n * sizeof (unsigned), "water");
		for (i = 0; i < surfaces; i++)
			water_of[i] = -1;
	}
	if (!block) {
		Con_Printf ("no GPU memory to keep the level in (%i KB): drawn a polygon at a time\n", quads * 2 * 64 >> 10);
		return;
	}
	/*
	 * By texture, then by light map: a run's quads are together and its command says where.
	 * The surfaces of each are listed first, in their own order (going through all of them
	 * for every texture and light map was a tenth of a level's loading).
	 */
	first_of = malloc ((2 * slots + 2 * surfaces + world->numtexinfo + 1) * sizeof (int));
	if (!first_of)
		Sys_Error ("no memory to sort the level's surfaces");
	next_of = first_of + 2 * slots;		/* two lists a surface: its texture's, its light map's */
	slot_of_info = next_of + 2 * surfaces;
	for (i = 0; i < 2 * slots; i++)
		first_of[i] = -1;
	for (i = 0; i < world->numtexinfo; i++) {
		slot_of_info[i] = -1;
		for (t = 0; t < world->numtextures; t++)
			if (world->textures[t] && world->textures[t] == world->texinfo[i].texture) {
				slot_of_info[i] = t;
				break;
			}
	}
	for (i = surfaces - 1, s = world->surfaces + i; i >= 0; i--, s--) {
		if (!kept_surface (s))
			continue;
		t = slot_of_info[s->texinfo - world->texinfo];
		next_of[2 * i] = t < 0 ? -1 : first_of[t];
		if (t >= 0)
			first_of[t] = i;
		t = s->lightmaptexturenum;
		next_of[2 * i + 1] = t < 0 || t >= 64 ? -1 : first_of[world->numtextures + t];
		if (t >= 0 && t < 64)
			first_of[world->numtextures + t] = i;
	}
	to = block;
	for (t = 0; t < slots; t++) {
		int light = t >= world->numtextures;
		texture_t *texture = light ? NULL : world->textures[t];

		if (!light && !texture)
			continue;
		first = (to - block) / 16;
		for (i = first_of[t]; i >= 0; i = next_of[2 * i + light]) {
			s = world->surfaces + i;
			to = put_quads (to, s->polys->compact + (light ? s->polys->numverts * 4 : 0), s->polys->numverts);
		}
		if ((to - block) / 16 == first)
			continue;
		runs[count].texture = texture;
		runs[count].quads = (to - block) / 16 - first;
		runs[count].shown = light ? lightmap_textures + t - world->numtextures : texture->gl_texturenum;
		count++;
	}
	free (first_of);
	/* the sky's polygons, once: its two layers are two commands on the same vertices */
	for (i = 0, s = world->surfaces; sky && i < surfaces; i++, s++) {
		if (!(s->flags & SURF_DRAWSKY))
			continue;
		for (p = s->polys; p; p = p->next) {
			if (p->numverts < 3 || p->numverts > 64)
				continue;
			for (m = 0; m < p->numverts; m++) {
				corners[4 * m] = float_bits (p->verts[m][0]);
				corners[4 * m + 1] = float_bits (p->verts[m][1]);
				corners[4 * m + 2] = float_bits (p->verts[m][2]);
				corners[4 * m + 3] = 0;
			}
			to = put_quads (to, corners, p->numverts);
		}
	}
	sky_put = sky;
	if (sky && (!qglStay (solidskytexture) || !qglStay (alphaskytexture)))
		sky = 0;
	/* the water's quads, by texture: where each surface's are, and a run a texture */
	water_count = water_runs = 0;
	if (water) {
		water_run = malloc (world->numtextures * sizeof *water_run);
		for (t = 0; water_run && t < world->numtextures; t++) {
			texture_t *texture = world->textures[t];

			if (!texture || texture->name[0] != '*')
				continue;
			first = (to - block) / 16;
			for (i = 0, s = world->surfaces; i < surfaces; i++, s++) {
				if (!(s->flags & SURF_DRAWTURB) || s->texinfo->texture != texture || !(m = water_quads_of (s)))
					continue;
				if (twin && twin[i] >= 0) {
					water_of[i] = water_of[twin[i]];	/* (its leaves ripple the other side) */
					continue;
				}
				water_of[i] = water_count;
				water_surfs[water_count].surf = s, water_surfs[water_count].quads = to;
				for (p = s->polys; p; p = p->next) {
					int n = p->numverts < MAX_WARP ? p->numverts : MAX_WARP;

					for (m = 0; m < n; m++) {
						corners[4 * m] = float_bits (p->verts[m][0]);
						corners[4 * m + 1] = float_bits (p->verts[m][1]);
						corners[4 * m + 2] = float_bits (p->verts[m][2]);
						corners[4 * m + 3] = 0;
					}
					if (n >= 3)
						to = put_quads (to, corners, n);
				}
				water_warp (&water_surfs[water_count++], 0);
			}
			if ((to - block) / 16 == first)
				continue;
			if (!qglStay (texture->gl_texturenum))
				water = 0;
			water_run[water_runs].texture = texture, water_run[water_runs].first = first - (quads * 2 + sky_put);
			water_run[water_runs++].quads = (to - block) / 16 - first;
		}
		if (!water_run)
			water = 0;
	}
	if (!water)
		water_runs = 0;
	if (seglKeep (count + (sky ? 2 : 0) + water_runs, quads * 2 + sky * 2 + water) < 0) {
		Con_Printf ("the level is too large to keep in GPU memory (%i quads): drawn a polygon at a time\n", quads * 2 + sky * 2 + water);
		qglRelease ();
		free (water_run);
		return;
	}
	for (i = 0; i < count; i++) {
		int ok = qglStay (runs[i].shown);

		/* every picture of a texture that animates stays, so that a frame can swap them */
		for (frame = runs[i].texture, m = 0; ok && frame && m < 20; m++) {
			ok = qglStay (frame->gl_texturenum);
			frame = frame->anim_next == runs[i].texture ? NULL : frame->anim_next;
		}
		if (!ok) {
			Con_Printf ("no GPU memory for the level's textures: drawn a polygon at a time\n");
			seglKeep (0, 0);
			qglRelease ();
			free (water_run);
			return;
		}
		seglKeptQuads (i, block, at, runs[i].quads, runs[i].shown, 255, 0);
		if (!runs[i].texture)
			seglKeptBlend (i, 3, 1);	/* a light map multiplies what is there */
		at += runs[i].quads;
	}
	for (i = 0, s = world->surfaces; i < surfaces; i++, s++) {
		if (!kept_surface (s))
			continue;
		for (m = 0; m < MAXLIGHTMAPS && s->styles[m] != 255; m++)
			if (s->styles[m] != 0) {
				styled[i] = 1;
				styled_count++;
				break;
			}
	}
	for (i = 0; i <= world->numleafs; i++)
		for (m = 0; m < world->leafs[i].nummarksurfaces; m++)
			if (styled[world->leafs[i].firstmarksurface[m] - world->surfaces])
				leaf_special[i] |= 2;
	if (sky) {
		/* (the second command's vertices are the first's: its mesh places are the next `sky` quads) */
		seglKeptQuads (count, block, quads * 2, sky, solidskytexture, 255, 0);
		seglKeptBlend (count, 0, 1);
		seglKeptQuads (count + 1, block - sky * 16, quads * 2 + sky, sky, alphaskytexture, 255, 0);
		seglKeptBlend (count + 1, 1, 1);
		runs[count].texture = runs[count + 1].texture = NULL;
		runs[count].quads = runs[count + 1].quads = sky;
		sky_run = count;
		count += 2;
	}
	/* (the water's vertices are after the sky's one set; its mesh places after the sky's two) */
	for (i = 0, kept_anims = 0; i < count; i++)
		if (runs[i].texture && runs[i].texture->anim_total)
			kept_anims = i + 1;
	water_first_run = count;
	for (r = 0; r < water_runs; r++, count++) {
		seglKeptQuads (count, block + (sky_put - sky * 2) * 16, quads * 2 + sky * 2 + water_run[r].first, water_run[r].quads,
			water_run[r].texture->gl_texturenum, 255, 0);
		runs[count].texture = NULL;
		runs[count].quads = water_run[r].quads;
		runs[count].shown = water_run[r].texture->gl_texturenum;
	}
	free (water_run);
	brushes[0].first = 0;
	for (i = 2; brush && brush_count < MAX_BRUSH - 1 && i < MAX_MODELS && cl.model_precache[i]; i++)
		if (cl.model_precache[i]->type == mod_brush && brush_vertices (cl.model_precache[i]))
			to = brush_build (cl.model_precache[i], to);
	water_on = water_runs > 0;
	water_shown = 1;
	kept_runs = count;
	kept_hidden = 0;
	kept_on = 1;
	light_build (world);
	/* (one line, said once: a line said while a level loads is a frame drawn) */
	Con_Printf ("the level is kept in GPU memory: %i quads in %i runs, %i KB; %i surfaces flicker; the workers have %i quads of water (%i faces the other side's) and %i light maps; %i brush models kept, %i KB\n",
		quads * 2 + water, count, (quads * 2 + water) * 64 >> 10, styled_count, water_on ? water : 0, twins, light_on ? light_job.count : 0,
		brush_count, brush * 16 >> 10);
}

/* the part of a number after its whole part, 16.16: where in a repeating texture it is */
static int fixed_part (float f)
{
	return (int)((f - floorf (f)) * 65536.0f);
}

/* A moving light reaches this surface this frame (R_MarkLights). */
void SE_Lit (msurface_t *surf)
{
	if (kept_on && surf - cl.worldmodel->surfaces < cl.worldmodel->nummodelsurfaces && kept_surface (surf) &&
	    lit_count[lit_now] < MAX_LIT)
		lit[lit_now][lit_count[lit_now]++] = surf;
}

/* What a frame changes in the kept level. */
static void kept_frame (void)
{
	int		i;
	msurface_t	*s;

	seglKeptMatrices ();
	/* (no run after the first kept_anims has a texture that is one of several) */
	for (i = 0; i < (kept_hidden ? kept_runs : kept_anims); i++) {
		if (kept_hidden && (water_shown || i < water_first_run || i >= water_first_run + water_runs))
			seglKeptShown (i, runs[i].quads);
		if (runs[i].texture && runs[i].texture->anim_total) {
			int name = R_TextureAnimation (runs[i].texture)->gl_texturenum;

			if (name != runs[i].shown) {
				seglKeptTexture (i, name);
				runs[i].shown = name;
			}
		}
	}
	kept_hidden = 0;
	world_drawn = 1;
	if (sky_run >= 0) {
		/*
		 * The sky lies on the picture, not on its polygons (docs/games/quake.md): four repeats of it in
		 * 1,024 pixels, moved by where the viewer looks and by time, the clouds twice as fast.
		 */
		float	u = -r_refdef.viewangles[1] * (1.0f / 90) * glwidth * (4.0f / 1024);
		float	v = r_refdef.viewangles[0] * (1.0f / 74) * glheight * (4.0f / 1024);
		float	slow = cl.time * (1.0f / 16), fast = cl.time * (1.0f / 8);

		/* (in the set being built: a list being drawn reads the other, which waited for it) */
		seglKeptLaidHere (sky_run, fixed_part (u + slow), fixed_part (v + slow), 4 << 16, 4 << 16);
		seglKeptLaidHere (sky_run + 1, fixed_part (u + fast), fixed_part (v + fast), 4 << 16, 4 << 16);
	}
	/* light maps a moving light came to or left (those that flicker: SE_MarkWorld, when in view) */
	for (i = 0; i < lit_count[lit_now]; i++)
		if (lit[lit_now][i]->dlightframe == r_framecount)
			R_BuildLightMap (lit[lit_now][i], NULL, 0);
	for (i = 0; i < lit_count[!lit_now]; i++) {
		s = lit[!lit_now][i];
		if (s->dlightframe != r_framecount && s->cached_dlight)
			R_BuildLightMap (s, NULL, 0);
	}
	lit_now = !lit_now;
	lit_count[lit_now] = 0;
}

/* A frame that drew no level (a console over everything, a level loading) must not show the kept one. */
void SE_FrameEnd (void)
{
	int i;

	if (kept_on && !world_drawn && !kept_hidden) {
		for (i = 0; i < kept_runs; i++)
			seglKeptShown (i, 0);
		kept_hidden = 1;
	}
	world_drawn = 0;
}

/* The world's planes and leaf boxes as integers. */
void SE_NewMap (void)
{
	model_t	*world = cl.worldmodel;
	int	i, j;

	seen_from = NULL;
	seen_count = 0;
	made_for = NULL;
	if (!world)
		return;
	surface_planes = Hunk_AllocName (world->numsurfaces * sizeof (plane_i), "planes_i");
	leaf_boxes = Hunk_AllocName ((world->numleafs + 1) * sizeof (box_i), "boxes_i");
	seen = Hunk_AllocName ((world->numleafs + 1) * sizeof (mleaf_t *), "seen");
	seen_box = Hunk_AllocName ((world->numleafs + 1) * sizeof (short *), "seen");
	seen_out = Hunk_AllocName (world->numleafs + 1, "seen_out");
	for (i = 0; i < world->numsurfaces; i++) {
		const msurface_t *s = &world->surfaces[i];
		int back = s->flags & SURF_PLANEBACK ? -1 : 1;

		for (j = 0; j < 3; j++)
			surface_planes[i].normal[j] = back * (int)(s->plane->normal[j] * 16384.0f);
		surface_planes[i].dist = back * (int)(s->plane->dist * 16384.0f);
	}
	for (i = 0; i <= world->numleafs; i++)
		for (j = 0; j < 6; j++)
			leaf_boxes[i].box[j] = (short)world->leafs[i].minmaxs[j];
	leaf_special = Hunk_AllocName (world->numleafs + 1, "special");
	for (i = 0; i <= world->numleafs; i++)
		for (j = 0; j < world->leafs[i].nummarksurfaces; j++)
			if (world->leafs[i].firstmarksurface[j]->flags & KEPT_SPECIAL)
				leaf_special[i] |= 1;
	made_for = world;
	kept_build (world);
	/* of a leaf with water, sky or flickering light, the surfaces that are any of those: a
	 * frame went through all of such a leaf's surfaces to find them, 640 of them at a level's
	 * start, 25 instructions each */
	{
		int total = 0, at = 0;

		for (i = 0; i <= world->numleafs; i++)
			if (leaf_special[i])
				total += world->leafs[i].nummarksurfaces;
		special_marks = Hunk_AllocName ((total + 1) * sizeof (msurface_t *), "marks");
		special_first = Hunk_AllocName ((world->numleafs + 1) * 2 * sizeof (int), "marks");
		for (i = 0; i <= world->numleafs; i++) {
			special_first[2 * i] = at;
			if (leaf_special[i] && styled)
				for (j = 0; j < world->leafs[i].nummarksurfaces; j++) {
					msurface_t *surf = world->leafs[i].firstmarksurface[j];

					if ((surf->flags & KEPT_SPECIAL) || styled[surf - world->surfaces])
						special_marks[at++] = surf;
				}
			special_first[2 * i + 1] = at - special_first[2 * i];
		}
	}
}

/* ---- a frame ---- */

/* What GLQuake's R_RecursiveWorldNode did: every surface in view is put on its texture's chain. */
float	se_warp_time;	/* the clock water ripples by (gl_warp.c) */

/* The things that stand in the leaves the viewer's leaf sees (torches and the like), once each. */
static entity_t	*static_seen[MAX_STATIC_ENTITIES];
static short	static_box[MAX_STATIC_ENTITIES][6];
static byte	static_out[MAX_STATIC_ENTITIES], static_boxed[MAX_STATIC_ENTITIES];
static unsigned	static_stamp[MAX_STATIC_ENTITIES], static_serial;
static int	static_count, seen_free, mark_leaves = -1;
int		se_no_chains;	/* this frame put no surface of the level on a chain: R_DrawWorld has none to draw */

static void static_add (efrag_t *frag)
{
	for (; frag; frag = frag->leafnext) {
		entity_t	*e = frag->entity;
		unsigned	n = (unsigned)(e - cl_static_entities);
		int		j;

		if (n >= MAX_STATIC_ENTITIES || static_stamp[n] == static_serial)
			continue;
		static_stamp[n] = static_serial;
		static_seen[static_count] = e;
		static_out[static_count] = 0;
		/* a model's box as R_DrawAliasModel has it, and two units more: only what that would cull too is left out */
		static_boxed[static_count] = e->model && e->model->type == mod_alias;
		for (j = 0; static_boxed[static_count] && j < 3; j++) {
			static_box[static_count][j] = (short)((int)floorf (e->origin[j] + e->model->mins[j]) - 2);
			static_box[static_count][j + 3] = (short)((int)ceilf (e->origin[j] + e->model->maxs[j]) + 2);
		}
		static_count++;
	}
}

/* The water of a leaf the viewer's leaf sees, for the workers' list. */
static void water_add (int leaf, int into)
{
	msurface_t	**mark = special_marks + special_first[2 * leaf];
	int		j, w;

	for (j = special_first[2 * leaf + 1]; j > 0; j--, mark++) {
		if (!((*mark)->flags & SURF_DRAWTURB) || (w = water_of[*mark - cl.worldmodel->surfaces]) < 0 || water_stamp[w] == water_serial)
			continue;
		water_stamp[w] = water_serial;
		water_seen[into][water_seen_count[into]++] = w;
		water_seen_quads[into] += water_quads_of (*mark);
	}
}

void SE_MarkWorld (void)
{
	model_t	*world = cl.worldmodel;
	/* a plane of the view, and which of a box's six numbers are its corner furthest along it */
	struct { int normal[3], dist, far[3], unused; } view[4], *v;
	int	i, j, k, eye[3], count, lights_apart, water_apart, free_of_leaves;
	mleaf_t	**list;
	const short **boxes;
	byte	*out;

	if (made_for != world)
		SE_NewMap ();
	if (mark_leaves < 0)
		mark_leaves = getenv ("QUAKE_MARK") && !strcmp (getenv ("QUAKE_MARK"), "leaves");
	se_warp_time = cl.time;
	se_no_chains = 0;
	water_apart = kept_on && water_on && r_wateralpha.value == 1.0f;
	if (water_on && water_apart != water_shown) {
		/* (water one can see through is drawn in the frame, after everything, as before) */
		for (i = 0; i < water_runs; i++)
			seglKeptShown (water_first_run + i, water_apart ? runs[water_first_run + i].quads : 0);
		water_shown = water_apart;
	}
	if (kept_on) {
		kept_frame ();
		SE_SbarHead ();		/* (gl_screen.c: the status bar, when it can be the frame's first draws) */
	}
	lights_apart = kept_on && SE_LightsApart ();
	if (!se_scene_split)
		SE_LightFrame ();	/* (else this core did, when it gave the frame out) */
	light_uploads ();
	/* with the light maps and the water the workers', and the sky kept, a frame has nothing to
	 * ask of a leaf: what stands in the leaves in sight is a list of its own */
	free_of_leaves = lights_apart && (water_apart || !level_water) && (sky_run >= 0 || !level_sky) && !mark_leaves && !r_novis.value;
	if (seen_from != r_viewleaf || r_novis.value || seen_statics != cl.num_statics || seen_kept != kept_on || seen_free != free_of_leaves) {
		byte *vis = Mod_LeafPVS (r_viewleaf, world);
		/* (of a kept level a leaf has something to do each frame only if it has water, sky or
		 * lights that flicker in it, or a torch or the like standing in it: the others, which
		 * are most, are not looked at at all) */
		int	all = r_novis.value != 0, count = 0, bits, into = !water_now;

		/* (the list a worker still reads is left as it is) */
		if (water_busy && water_job.list == water_seen[into])
			into = water_now;
		if (water_apart) {
			water_seen_count[into] = water_seen_quads[into] = 0;
			water_serial++;
		}
		static_count = 0;
		static_serial++;
		/* (eight leaves a byte of the table, and most bytes are 0) */
		for (i = 0; i < world->numleafs; i += 8) {
			if ((bits = all ? 255 : vis[i >> 3]) == 0)
				continue;
			for (j = i; bits && j < world->numleafs; j++, bits >>= 1) {
				if (!(bits & 1))
					continue;
				if (free_of_leaves) {
					if (world->leafs[j + 1].efrags)
						static_add (world->leafs[j + 1].efrags);
				} else if (!kept_on || leaf_special[j + 1] || world->leafs[j + 1].efrags) {
					seen_box[count] = leaf_boxes[j + 1].box;
					seen[count++] = &world->leafs[j + 1];
				}
				if (water_apart && (leaf_special[j + 1] & 1))
					water_add (j + 1, into);
			}
		}
		if (water_apart)
			water_now = into;
		seen_count = count;
		seen_from = r_viewleaf;
		seen_statics = cl.num_statics;
		seen_kept = kept_on;
		seen_free = free_of_leaves;
	}
	water_apart_last = water_apart;	/* (the frame's jobs ripple it from the next frame on) */
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 3; j++) {
			view[i].normal[j] = (int)(frustum[i].normal[j] * 16384.0f);
			view[i].far[j] = view[i].normal[j] < 0 ? j : j + 3;
		}
		view[i].dist = (int)(frustum[i].dist * 16384.0f);
	}
	if (free_of_leaves) {
		se_no_chains = 1;
		for (i = 0; i < static_count; i++) {
			const short	*box = static_box[i];
			entity_t	*e = static_seen[i];

			if (static_boxed[i]) {
				v = &view[static_out[i]];
				if (v->normal[0] * box[v->far[0]] + v->normal[1] * box[v->far[1]] + v->normal[2] * box[v->far[2]] < v->dist)
					continue;
				for (k = 0, v = view; k < 4; k++, v++)
					if (v->normal[0] * box[v->far[0]] + v->normal[1] * box[v->far[1]] + v->normal[2] * box[v->far[2]] < v->dist)
						break;
				if (k < 4) {
					static_out[i] = (byte)k;
					continue;
				}
			}
			if (cl_numvisedicts < MAX_VISEDICTS) {
				cl_visedicts[cl_numvisedicts++] = e;
				e->visframe = r_framecount;
			}
		}
		return;
	}
	for (j = 0; j < 3; j++)
		eye[j] = (int)(modelorg[j] * 4.0f);	/* quarter units: the products below fit 32 bits */

	/* (the lists in locals: a store below could be to any of them, for all the compiler knows) */
	list = seen, boxes = seen_box, out = seen_out, count = seen_count;
	for (i = 0; i < count; i++) {
		const short	*box = boxes[i];
		mleaf_t		*leaf;
		msurface_t	**mark;
		int		marks;

		/* outside one of the view's planes: its corner furthest along the plane is behind it.
		 * (The plane it was outside of last frame first: it mostly still is, and most leaves
		 * that can be seen from here are outside the view.) */
		v = &view[out[i]];
		if (v->normal[0] * box[v->far[0]] + v->normal[1] * box[v->far[1]] + v->normal[2] * box[v->far[2]] < v->dist)
			continue;
		for (k = 0, v = view; k < 4; k++, v++)
			if (v->normal[0] * box[v->far[0]] + v->normal[1] * box[v->far[1]] + v->normal[2] * box[v->far[2]] < v->dist)
				break;
		leaf = list[i];
		mark = leaf->firstmarksurface;
		marks = leaf->nummarksurfaces;
		if (k < 4) {
			out[i] = (byte)k;
			continue;
		}
		/* of a kept level only water and sky are put on the chains */
		if (kept_on) {
			int at = (int)(leaf - world->leafs);

			mark = special_marks + special_first[2 * at];
			marks = leaf_special[at] ? special_first[2 * at + 1] : 0;
		}
		for (j = marks; j > 0; j--, mark++) {
			msurface_t	*surf = *mark;
			const plane_i	*p;

			if (surf->visframe == r_framecount)
				continue;
			if (kept_on && !(surf->flags & (sky_run >= 0 ? SURF_DRAWTURB : KEPT_SPECIAL))) {
				/* kept: only its light map may need making again, when a style's brightness changed */
				if (styled[surf - world->surfaces] && !lights_apart) {
					int m;

					surf->visframe = r_framecount;
					for (m = 0; m < MAXLIGHTMAPS && surf->styles[m] != 255; m++)
						if (d_lightstylevalue[surf->styles[m]] != surf->cached_light[m]) {
							R_BuildLightMap (surf, NULL, 0);
							break;
						}
				}
				continue;
			}
			surf->visframe = r_framecount;
			if (water_apart && (surf->flags & SURF_DRAWTURB))
				continue;	/* kept, and the workers' */
			p = &surface_planes[surf - world->surfaces];
			if ((p->normal[0] >> 2) * eye[0] + (p->normal[1] >> 2) * eye[1] + (p->normal[2] >> 2) * eye[2] <= p->dist)
				continue;	/* seen from behind */
			surf->texturechain = surf->texinfo->texture->texturechain;
			surf->texinfo->texture->texturechain = surf;
		}
		if (leaf->efrags)
			R_StoreEfrags (&leaf->efrags);
	}
}

void SE_DrawPoly (glpoly_t *p, int lightmap)
{
	qglFan (p->compact + (lightmap ? p->numverts * 4 : 0), p->numverts);
}

/* ---- water and sky ---- */

#ifdef SE_COUNT
#define se_count_add(k, n) (se_count[k] += (n))
#else
#define se_count_add(k, n)
#endif

#define MAX_SKY		8192		/* vertices of sky in view */

extern int	solidskytexture, alphaskytexture;

/* GLQuake's EmitWaterPolys: each vertex's texture coordinates ripple by a table of sines. */
void qglQuads (const unsigned int *vertices, int n);
unsigned *qglQuadsRoom (int most);
void qglQuadsUsed (int quads);

void SE_Water (msurface_t *fa)
{
	unsigned	out[MAX_WARP * 4], *to;
	glpoly_t	*p;
	const float	*v;
	float		s, t, base_s = 0, base_t = 0, clock = se_warp_time, step = 256 / (2 * 3.14159265f);
	int		i, n;

	for (p = fa->polys; p; p = p->next) {
		n = p->numverts < MAX_WARP ? p->numverts : MAX_WARP;
		for (i = 0, v = p->verts[0]; i < n; i++, v += VERTEXSIZE) {
			s = (v[3] + turbsin[(int)((v[4] * 0.125f + clock) * step) & 255]) * (1.0f / 64);
			t = (v[4] + turbsin[(int)((v[3] * 0.125f + clock) * step) & 255]) * (1.0f / 64);
			if (!i)
				base_s = floor_of (s) - 1, base_t = floor_of (t) - 1;	/* whole repeats off: see SE_BuildPoly */
			out[4 * i] = ((const unsigned *)v)[0];
			out[4 * i + 1] = ((const unsigned *)v)[1];
			out[4 * i + 2] = ((const unsigned *)v)[2];
			out[4 * i + 3] = coord_bits (s - base_s, t - base_t);
		}
		se_count_add (8, n);
		/* the polygon as a fan of quads (what the GPU's library makes of a fan), written into
		 * the frame with the water's other polygons: one draw for them all */
		if (n < 3 || (to = qglQuadsRoom ((n - 1) / 2)) == NULL)
			continue;
		for (i = 1; i + 1 < n; i += 2, to += 24) {
			const unsigned *a = out, *b = out + 4 * i, *c = b + 4, *d = i + 2 < n ? c + 4 : c;

			/* (a quad's corners 0 1 2 and 0 2 3: the two triangles the GPU makes of one) */
			to[0] = to[12] = a[0], to[1] = to[13] = a[1], to[2] = to[14] = a[2], to[3] = to[15] = a[3];
			to[4] = b[0], to[5] = b[1], to[6] = b[2], to[7] = b[3];
			to[8] = to[16] = c[0], to[9] = to[17] = c[1], to[10] = to[18] = c[2], to[11] = to[19] = c[3];
			to[20] = d[0], to[21] = d[1], to[22] = d[2], to[23] = d[3];
		}
		qglQuadsUsed ((n - 1) / 2);
	}
}

/* GLQuake's sky: two layers of one picture sliding at two speeds, as if on a flattened dome. */
void SE_Sky (msurface_t *chain)
{
	static unsigned	first[MAX_SKY * 4], second[MAX_SKY * 4];	/* quads: the sky, and the clouds over it */
	unsigned	out[MAX_WARP * 4], over[MAX_WARP], *to = first, *later = second;
	msurface_t	*fa;
	glpoly_t	*p;
	const float	*v;
	float		dir[3], length, s, t, base_s = 0, base_t = 0;
	float		slow = cl.time * 8, fast = cl.time * 16;
	int		i, n;

	slow -= (int)slow & ~127;
	fast -= (int)fast & ~127;
	GL_Bind (solidskytexture);
	for (fa = chain; fa; fa = fa->texturechain)
		for (p = fa->polys; p; p = p->next) {
			n = p->numverts < MAX_WARP ? p->numverts : MAX_WARP;
			if (to + 16 * (n / 2 + 1) > first + MAX_SKY * 4)
				break;
			for (i = 0, v = p->verts[0]; i < n; i++, v += VERTEXSIZE) {
				dir[0] = v[0] - r_origin[0], dir[1] = v[1] - r_origin[1];
				dir[2] = (v[2] - r_origin[2]) * 3;	/* flatten the sphere */
				length = 6 * 63 / sqrtf (dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
				s = dir[0] * length * (1.0f / 128), t = dir[1] * length * (1.0f / 128);
				if (!i)
					base_s = floorf (s) - 2, base_t = floorf (t) - 2;
				out[4 * i] = ((const unsigned *)v)[0];
				out[4 * i + 1] = ((const unsigned *)v)[1];
				out[4 * i + 2] = ((const unsigned *)v)[2];
				out[4 * i + 3] = coord_bits (s + slow * (1.0f / 128) - base_s, t + slow * (1.0f / 128) - base_t);
				over[i] = coord_bits (s + fast * (1.0f / 128) - base_s, t + fast * (1.0f / 128) - base_t);
			}
			se_count_add (9, n);
			/* the polygon as a fan of quads, in both layers (a draw a polygon was two hundred
			 * instructions before its first vertex, twice) */
			for (i = 1; i + 1 < n; i += 2, to += 16, later += 16) {
				int last = i + 2 < n ? i + 2 : n - 1, k;

				for (k = 0; k < 4; k++) {
					int from = k == 0 ? 0 : k == 1 ? i : k == 2 ? i + 1 : last;

					to[4 * k] = later[4 * k] = out[4 * from];
					to[4 * k + 1] = later[4 * k + 1] = out[4 * from + 1];
					to[4 * k + 2] = later[4 * k + 2] = out[4 * from + 2];
					to[4 * k + 3] = out[4 * from + 3];
					later[4 * k + 3] = over[from];
				}
			}
		}
	if (to == first)
		return;
	qglQuads (first, (int)(to - first) / 4);
	/* the clouds in front, over the same polygons */
	glEnable (GL_BLEND);
	GL_Bind (alphaskytexture);
	qglQuads (second, (int)(later - second) / 4);
	glDisable (GL_BLEND);
}

/* ---- models ---- */

extern float	r_avertexnormal_dots[16][256];
extern float	*shadedots, shadelight;

/*
 * A model's pose: its vertices are bytes, which the matrix scales, and their texture
 * coordinates were packed when it loaded (mesh_shaderemu.c). The light at each of the
 * normals a vertex can have is a table the GPU looks up, made once for a light and a turn.
 */
void SE_DrawAliasFrame (aliashdr_t *hdr, int pose)
{
	static short	dots[16][256], least[16];
	static int	made;
	unsigned	*light = NULL;
	const short	*row;
	int		i, level, scale = (int)(shadelight * 256.0f) & ~7, turn, fresh;

	if (!made) {
		int j;

		for (i = 0; i < 16; i++)
			for (j = 0, least[i] = 32767; j < 256; j++) {
				dots[i][j] = (short)(r_avertexnormal_dots[i][j] * 256.0f);
				if (dots[i][j] < least[i])
					least[i] = dots[i][j];
			}
		made = 1;
	}
	/* (light in steps of 1/32: a thing at rest keeps its table) */
	turn = (shadedots - r_avertexnormal_dots[0]) / 256;
	row = dots[turn];
	/* a model lit nearly all over (a flame, which GLQuake gives full light) has no shading drawn */
	if (least[turn] * scale >> 8 < 224)
		light = qglLights (turn | scale << 4, &fresh);
	if (light && fresh)
		for (i = 0; i < 162; i++) {	/* (the normals there are: anorms.h) */
			level = row[i] * scale >> 8;
			light[i] = (unsigned)(level < 0 ? 0 : level > 255 ? 255 : level) * 0x010101u;
		}
	qglModel (currententity->model, pose, (const unsigned char *)hdr + hdr->posedata + pose * hdr->poseverts * sizeof (trivertx_t),
		(const unsigned *)((const byte *)hdr + hdr->commands), light, hdr->poseverts);
}
