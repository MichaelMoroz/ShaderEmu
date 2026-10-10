/*
 * The level and the models as this machine's GPU takes them (docs/quake.md): vertices made
 * once, when a level or a model loads; the level kept in GPU memory; and what is in view found
 * from the leaves the viewer's leaf can see, not by a walk of the BSP tree.
 */
#include "quakedef.h"

void qglFan (const unsigned *vertices, int count);
void qglModel (const void *model, int pose, const unsigned char *vertices, const unsigned *coords, const unsigned *light,
	int count);
unsigned *qglLights (int key, int *fresh);
void R_StoreEfrags (efrag_t **ppefrag);
void SE_NewMap (void);

typedef struct { int normal[3], dist; } plane_i;	/* normal in 1/16384ths, dist in units * 16384 */
typedef struct { short box[6]; } box_i;

static plane_i	*surface_planes;	/* of the world's surfaces, turned to face their front */
static box_i	*leaf_boxes;
static mleaf_t	**seen;			/* the leaves the viewer's leaf sees */
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

/* A polygon's vertices as compact ones (docs/gpu.md): with its texture, then with its light map. */
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
 * by light map; each run is a command that stays in every frame's list (docs/quake.md).
 * A frame then sends the level's matrices and what changed. Water and sky are not kept.
 */
#define KEPT_SPECIAL	(SURF_DRAWSKY | SURF_DRAWTURB)
#define MAX_LIT		512

typedef struct { texture_t *texture; int quads, shown; } run_t;

extern int	lightmap_textures, solidskytexture, alphaskytexture, glwidth, glheight;

static int	kept_on, kept_runs, kept_hidden, world_drawn;
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

int	se_map_serial;	/* goes up when the models go: what is remembered about a level is then stale */

void SE_ClearMap (void)
{
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

	kept_on = 0;
	if (COM_CheckParm ("-nokeep"))
		return;
	sky_run = -1;
	for (i = 0, s = world->surfaces; i < surfaces; i++, s++) {
		if (kept_surface (s))
			quads += (s->polys->numverts - 1) / 2;
		if (s->flags & SURF_DRAWSKY)
			for (p = s->polys; p; p = p->next)
				sky += p->numverts >= 3 && p->numverts <= 64 ? (p->numverts - 1) / 2 : 0;
	}
	if (!quads)
		return;
	block = qglBlock ((quads * 2 + sky) * 64);
	runs = Hunk_AllocName ((world->numtextures + 64) * sizeof (run_t), "runs");
	styled = Hunk_AllocName (world->numsurfaces, "styled");
	if (!block) {
		Con_Printf ("no GPU memory to keep the level in (%i KB): drawn a polygon at a time\n", quads * 2 * 64 >> 10);
		return;
	}
	/* by texture, then by light map: a run's quads are together and its command says where */
	to = block;
	for (t = 0; t < world->numtextures + 64; t++) {
		int light = t >= world->numtextures;
		texture_t *texture = light ? NULL : world->textures[t];

		if (!light && !texture)
			continue;
		first = (to - block) / 16;
		for (i = 0, s = world->surfaces; i < surfaces; i++, s++) {
			if (!kept_surface (s) || (light ? s->lightmaptexturenum != t - world->numtextures : s->texinfo->texture != texture))
				continue;
			to = put_quads (to, s->polys->compact + (light ? s->polys->numverts * 4 : 0), s->polys->numverts);
		}
		if ((to - block) / 16 == first)
			continue;
		runs[count].texture = texture;
		runs[count].quads = (to - block) / 16 - first;
		runs[count].shown = light ? lightmap_textures + t - world->numtextures : texture->gl_texturenum;
		count++;
	}
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
	if (sky && (!qglStay (solidskytexture) || !qglStay (alphaskytexture)))
		sky = 0;
	if (seglKeep (count + (sky ? 2 : 0), quads * 2 + sky * 2) < 0) {
		Con_Printf ("the level is too large to keep in GPU memory (%i quads): drawn a polygon at a time\n", quads * 2 + sky * 2);
		qglRelease ();
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
	kept_runs = count;
	kept_hidden = 0;
	kept_on = 1;
	Con_Printf ("the level is kept in GPU memory: %i quads in %i runs, %i KB; %i surfaces flicker\n", quads * 2, count,
		quads * 2 * 64 >> 10, styled_count);
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
	for (i = 0; i < kept_runs; i++) {
		if (kept_hidden)
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
		 * The sky lies on the picture, not on its polygons (docs/quake.md): four repeats of it in
		 * 1,024 pixels, moved by where the viewer looks and by time, the clouds twice as fast.
		 */
		float	u = -r_refdef.viewangles[1] * (1.0f / 90) * glwidth * (4.0f / 1024);
		float	v = r_refdef.viewangles[0] * (1.0f / 74) * glheight * (4.0f / 1024);
		float	slow = cl.time * (1.0f / 16), fast = cl.time * (1.0f / 8);

		seglKeptLaid (sky_run, fixed_part (u + slow), fixed_part (v + slow), 4 << 16, 4 << 16);
		seglKeptLaid (sky_run + 1, fixed_part (u + fast), fixed_part (v + fast), 4 << 16, 4 << 16);
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

void SE_MarkWorld (void)
{
	model_t	*world = cl.worldmodel;
	plane_i	view[4];
	int	i, j, k, eye[3];

	if (made_for != world)
		SE_NewMap ();
	se_warp_time = cl.time;
	if (kept_on)
		kept_frame ();
	if (seen_from != r_viewleaf || r_novis.value || seen_statics != cl.num_statics || seen_kept != kept_on) {
		byte *vis = Mod_LeafPVS (r_viewleaf, world);

		seen_count = 0;
		/* (of a kept level a leaf has something to do each frame only if it has water, sky or
		 * lights that flicker in it, or a torch or the like standing in it: the others, which
		 * are most, are not looked at at all) */
		for (i = 0; i < world->numleafs; i++)
			if ((r_novis.value || (vis[i >> 3] & (1 << (i & 7))))
			    && (!kept_on || leaf_special[i + 1] || world->leafs[i + 1].efrags))
				seen[seen_count++] = &world->leafs[i + 1];
		seen_from = r_viewleaf;
		seen_statics = cl.num_statics;
		seen_kept = kept_on;
	}
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 3; j++)
			view[i].normal[j] = (int)(frustum[i].normal[j] * 16384.0f);
		view[i].dist = (int)(frustum[i].dist * 16384.0f);
	}
	for (j = 0; j < 3; j++)
		eye[j] = (int)(modelorg[j] * 4.0f);	/* quarter units: the products below fit 32 bits */

	for (i = 0; i < seen_count; i++) {
		mleaf_t		*leaf = seen[i];
		const short	*box = leaf_boxes[leaf - world->leafs].box;
		msurface_t	**mark = leaf->firstmarksurface;
		int		marks = leaf->nummarksurfaces;

		/* outside one of the view's planes: its corner furthest along the plane is behind it.
		 * (The plane it was outside of last frame first: it mostly still is, and most leaves
		 * that can be seen from here are outside the view.) */
		{
			const int *n = view[k = seen_out[i]].normal;

			if (n[0] * box[n[0] < 0 ? 0 : 3] + n[1] * box[n[1] < 0 ? 1 : 4] + n[2] * box[n[2] < 0 ? 2 : 5] < view[k].dist)
				continue;
		}
		for (k = 0; k < 4; k++) {
			const int *n = view[k].normal;

			if (n[0] * box[n[0] < 0 ? 0 : 3] + n[1] * box[n[1] < 0 ? 1 : 4] + n[2] * box[n[2] < 0 ? 2 : 5] < view[k].dist)
				break;
		}
		if (k < 4) {
			seen_out[i] = (byte)k;
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
				if (styled[surf - world->surfaces]) {
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

#define MAX_WARP	64		/* corners of one of their polygons (they are cut into small ones) */
#define MAX_SKY		8192		/* vertices of sky in view */

extern float	turbsin[];
extern int	solidskytexture, alphaskytexture;

/* GLQuake's EmitWaterPolys: each vertex's texture coordinates ripple by a table of sines. */
#define WATER_QUADS	96		/* of one surface, drawn together */
void qglQuads (const unsigned int *vertices, int n);

void SE_Water (msurface_t *fa)
{
	static unsigned	quads[WATER_QUADS * 16];
	unsigned	out[MAX_WARP * 4], *to = quads;
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
				base_s = floorf (s) - 1, base_t = floorf (t) - 1;	/* whole repeats off: see SE_BuildPoly */
			out[4 * i] = ((const unsigned *)v)[0];
			out[4 * i + 1] = ((const unsigned *)v)[1];
			out[4 * i + 2] = ((const unsigned *)v)[2];
			out[4 * i + 3] = coord_bits (s - base_s, t - base_t);
		}
		se_count_add (8, n);
		/* the polygon as a fan of quads (what the GPU's library makes of a fan), with the
		 * surface's other polygons: one draw for them all */
		for (i = 1; i + 1 < n; i += 2) {
			int last = i + 2 < n ? i + 2 : n - 1;

			if (to == quads + WATER_QUADS * 16) {
				qglQuads (quads, WATER_QUADS * 4);
				to = quads;
			}
			{
				const unsigned *a = out, *b = out + 4 * i, *c = out + 4 * (i + 1), *d = out + 4 * last;

				to[0] = a[0], to[1] = a[1], to[2] = a[2], to[3] = a[3];
				to[4] = b[0], to[5] = b[1], to[6] = b[2], to[7] = b[3];
				to[8] = c[0], to[9] = c[1], to[10] = c[2], to[11] = c[3];
				to[12] = d[0], to[13] = d[1], to[14] = d[2], to[15] = d[3];
			}
			to += 16;
		}
	}
	if (to != quads)
		qglQuads (quads, (int)(to - quads) / 4);
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
