/*
 * Doom's 3D view drawn by the GPU through OpenGL (programs/linux/gles.c), in place of the
 * software renderer's pixel loops. Doom still walks its BSP tree and decides what is visible;
 * what it finds becomes textured quads (walls), polygons (floors and ceilings) and billboards
 * (things), with the depth buffer doing the rest. Textures are Doom's own 8-bit pictures,
 * looked up in the game's palette, so palette effects work as they always did.
 *
 * linux/nanox/doom.sh compiles the renderer's entry points under other names; the ones here
 * call those when the GPU is not drawing the view.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES/segl.h>
#include "doomdef.h"
#include "doomstat.h"
#include "hu_lib.h"
#include "hu_stuff.h"
#include "m_swap.h"
#include "r_local.h"
#include "m_bbox.h"
#include "m_fixed.h"
#include "r_sky.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

void R_RenderPlayerView_soft(player_t *player);
void R_StoreWallRange_soft(int start, int stop);
void R_Subsector_soft(int num);
void R_SetupFrame(player_t *player);
void R_AddLine(seg_t *line);
extern short **texturecolumnlump;
extern int *texturewidthmask;
extern int numtextures;
extern int skyflatnum;

#define KEY		247		/* palette index that means "nothing here" */
#define NEAR		(4 * FRACUNIT)
#define MAX_CORNERS	48

int doom_gl;				/* set by doom_video.c: the GPU draws the view */
int doom_gl_scene;			/* a view was drawn for the frame being built */
int doom_gl_kept = 1;			/* and it can be drawn again as it was (doom_gl_again) */

static int in_view;			/* inside our R_RenderPlayerView */
static int keyed_first, keyed_last = -1;	/* the rows given the key again for this frame */
static seg_t *level_segs;		/* what the per-level tables below were made for */
static int level_count;
static fixed_t *seg_length;
static int *seg_frame;			/* the frame each seg was last drawn in */
static int *subsector_frame;
static struct { int first, count; } *polygon;	/* per subsector, into corners */
static fixed_t (*corners)[2];
static int corner_count, corner_room;
static GLuint *wall_gl, *flat_gl, *sprite_gl, sky_gl;
static short *sprite_height;
static int flat_count;
static int frame_number;

static GLfixed quad_xyz[12], quad_uv[8], fan_xyz[MAX_CORNERS * 3], fan_uv[MAX_CORNERS * 2];

/*
 * a / b in 16.16, for |a| >> 14 < |b| (FixedDiv has seen to that), rounded toward zero: the
 * whole part by one division, the sixteen bits after it as many at a time as fit. The
 * remainder is below b, so it can move up by the bits b leaves free at the top.
 */
fixed_t
FixedDiv2(fixed_t a, fixed_t b)
{
	unsigned ua = a < 0 ? -(unsigned)a : (unsigned)a, ub = b < 0 ? -(unsigned)b : (unsigned)b;
	unsigned q = ua / ub, r = ua % ub, top = ub;
	int i = 0, room = 0;

	if (!(top >> 16))
		room = 16;
	else {
		if (!(top >> 24))
			room += 8, top <<= 8;
		if (!(top >> 28))
			room += 4, top <<= 4;
		if (!(top >> 30))
			room += 2, top <<= 2;
		if (!(top >> 31))
			room++;
	}
	while (room && i < 16) {
		int step = 16 - i < room ? 16 - i : room;

		r <<= step;
		q = q << step | r / ub;
		r %= ub;
		i += step;
	}
	for (; i < 16; i++) {
		unsigned carry = r >> 31;

		r <<= 1;
		q <<= 1;
		if (carry || r >= ub) {
			r -= ub;
			q |= 1;
		}
	}
	return (a ^ b) < 0 ? -(fixed_t)q : (fixed_t)q;
}

/* For the figures doom_video.c prints: instructions in the view, and (in [3]) the things drawn. */
unsigned doom_view[4];

/* ---- full-screen pictures ---- */

static int page_held;			/* the screen holds the page last drawn, but for what dirtybox covers */
static int keyed;			/* the view's area of the screen holds the key, but for what dirtybox covers */

void V_DrawPatch_cpu(int x, int y, int scrn, patch_t *patch);

/*
 * The title, help and intermission pages are drawn again every frame, a post at a time. A
 * page is decoded once here and copied after that (the columns' offsets tell pages apart,
 * should the cache hand the same memory to another).
 */
void
V_DrawPatch(int x, int y, int scrn, patch_t *patch)
{
	static byte *page;
	static patch_t *of;
	static unsigned sum_of;
	unsigned sum = 0;
	int i, first = 0, last = SCREENHEIGHT - 1;

	if (x || y || scrn || SHORT(patch->width) != SCREENWIDTH || SHORT(patch->height) != SCREENHEIGHT ||
	    patch->leftoffset || patch->topoffset) {
		V_DrawPatch_cpu(x, y, scrn, patch);
		return;
	}
	for (i = 0; i < SCREENWIDTH; i += 8)
		sum = sum * 31 + (unsigned)patch->columnofs[i];
	if (!page)
		page = malloc(SCREENWIDTH * SCREENHEIGHT);
	if (of != patch || sum_of != sum) {
		byte *screen = screens[0];

		screens[0] = page;
		V_DrawPatch_cpu(0, 0, 0, patch);
		screens[0] = screen;
		of = patch;
		sum_of = sum;
	}
	else if (page_held) {
		/* the screen has this page already, but for the rows drawn on since (a menu, or none) */
		first = dirtybox[BOXBOTTOM] < 0 ? 0 : dirtybox[BOXBOTTOM];
		last = dirtybox[BOXTOP] >= SCREENHEIGHT ? SCREENHEIGHT - 1 : dirtybox[BOXTOP];
	}
	if (first <= last)
		memcpy(screens[0] + first * SCREENWIDTH, page + first * SCREENWIDTH, (last - first + 1) * SCREENWIDTH);
	M_ClearBox(dirtybox);
	page_held = 1;
	keyed = 0;	/* and a view that comes after it finds none of its key */
}

/* ---- textures: Doom's pictures as rows of palette indices in GPU memory ---- */

/* Copies a column's posts to where that column is kept: `height` bytes, top first. */
static void
column_posts(byte *to, int height, const column_t *post)
{
	for (; post->topdelta != 0xff; post = (const column_t *)((const byte *)post + post->length + 4))
		if (post->topdelta < height)
			memcpy(to + post->topdelta, (const byte *)post + 3,
			       post->topdelta + post->length > height ? height - post->topdelta : post->length);
}

/*
 * A wall's picture is kept on its side: a texel's row is the wall's column, so that each of
 * Doom's columns is one run of bytes to copy. Whoever draws with it gives (v, u) for (u, v).
 */
static GLuint
wall_texture(int number)
{
	int width, height, x;
	byte *picture;

	number = texturetranslation[number];
	if (wall_gl[number])
		return wall_gl[number];
	width = texturewidthmask[number] + 1;
	height = textureheight[number] >> FRACBITS;
	picture = seglMemory(width * height);
	glGenTextures(1, &wall_gl[number]);
	if (!picture || !wall_gl[number])
		return 0;
	memset(picture, KEY, width * height);
	for (x = 0; x < width; x++) {
		byte *column = R_GetColumn(number, x), *to = picture + x * height;

		if (texturecolumnlump[number][x] > 0)
			column_posts(to, height, (column_t *)(column - 3));	/* straight from one patch: it may leave holes */
		else
			memcpy(to, column, height);
	}
	glBindTexture(GL_TEXTURE_2D, wall_gl[number]);
	seglTexturePointer(picture, height, width, GL_COLOR_INDEX8_EXT);
	return wall_gl[number];
}

/* The sky's picture, the right way up: it is laid on the screen, not on a wall. */
static GLuint
sky_texture(void)
{
	int number = texturetranslation[skytexture], width, height, x, row;
	byte *picture, *columns[512];

	if (sky_gl)
		return sky_gl;
	width = texturewidthmask[number] + 1;
	height = textureheight[number] >> FRACBITS;
	picture = seglMemory(width * height);
	glGenTextures(1, &sky_gl);
	if (!picture || !sky_gl)
		return 0;
	/* a row at a time (stores a row apart would fill the write cache every few dozen) */
	for (x = 0; x < width && x < 512; x++)
		columns[x] = R_GetColumn(number, x);
	for (row = 0; row < height; row++)
		for (x = 0; x < width && x < 512; x++)
			picture[row * width + x] = columns[x][row];
	glBindTexture(GL_TEXTURE_2D, sky_gl);
	seglTexturePointer(picture, width, height, GL_COLOR_INDEX8_EXT);
	return sky_gl;
}

static GLuint
flat_texture(int number)
{
	byte *picture;

	number = flattranslation[number];
	if (number < 0 || number >= flat_count)
		return 0;
	if (flat_gl[number])
		return flat_gl[number];
	picture = seglMemory(64 * 64);
	glGenTextures(1, &flat_gl[number]);
	if (!picture || !flat_gl[number])
		return 0;
	memcpy(picture, W_CacheLumpNum(firstflat + number, PU_CACHE), 64 * 64);
	glBindTexture(GL_TEXTURE_2D, flat_gl[number]);
	seglTexturePointer(picture, 64, 64, GL_COLOR_INDEX8_EXT);
	return flat_gl[number];
}

static GLuint
sprite_texture(int lump)
{
	patch_t *patch;
	int width, height, x;
	byte *picture;

	if (sprite_gl[lump])
		return sprite_gl[lump];
	patch = W_CacheLumpNum(firstspritelump + lump, PU_CACHE);
	width = SHORT(patch->width);
	height = SHORT(patch->height);
	picture = seglMemory(width * height);
	glGenTextures(1, &sprite_gl[lump]);
	if (!picture || !sprite_gl[lump])
		return 0;
	/* on its side, as a wall's picture is: stores down a column of an upright one are a row
	 * apart, which fills the machine's write cache after a few dozen */
	memset(picture, KEY, width * height);
	for (x = 0; x < width; x++)
		column_posts(picture + x * height, height, (column_t *)((byte *)patch + LONG(patch->columnofs[x])));
	sprite_height[lump] = height;
	glBindTexture(GL_TEXTURE_2D, sprite_gl[lump]);
	seglTexturePointer(picture, height, width, GL_COLOR_INDEX8_EXT);
	return sprite_gl[lump];
}

/* ---- the level's floor plan: a convex polygon per subsector ---- */

static void
keep_polygon(int subsector, fixed_t (*p)[2], int n)
{
	if (corner_count + n > corner_room) {
		corner_room = (corner_count + n) * 2 + 256;
		corners = realloc(corners, corner_room * sizeof *corners);
	}
	polygon[subsector].first = corner_count;
	polygon[subsector].count = n;
	memcpy(corners + corner_count, p, n * sizeof *p);
	corner_count += n;
}

/* The part of polygon `in` on `side` of a node's partition line (as R_PointOnSide has sides). */
static int
clip(fixed_t (*in)[2], int n, node_t *node, int side, fixed_t (*out)[2])
{
	long long a = node->dx >> FRACBITS, b = node->dy >> FRACBITS, d[MAX_CORNERS];
	int i, m = 0;

	for (i = 0; i < n; i++) {
		d[i] = a * ((long long)in[i][1] - node->y) - b * ((long long)in[i][0] - node->x);
		if (side)
			d[i] = -d[i];	/* now the kept side is where d <= 0 */
	}
	for (i = 0; i < n && m < MAX_CORNERS - 1; i++) {
		int j = (i + 1) % n;

		if (d[i] <= 0) {
			out[m][0] = in[i][0];
			out[m++][1] = in[i][1];
		}
		if ((d[i] < 0 && d[j] > 0) || (d[i] > 0 && d[j] < 0)) {
			long long di = d[i], dj = d[j], t;

			while (di > (1LL << 45) || di < -(1LL << 45) || dj > (1LL << 45) || dj < -(1LL << 45)) {
				di >>= 1;
				dj >>= 1;
			}
			t = (di << 16) / (di - dj);
			out[m][0] = in[i][0] + (fixed_t)((((long long)in[j][0] - in[i][0]) * t) >> 16);
			out[m++][1] = in[i][1] + (fixed_t)((((long long)in[j][1] - in[i][1]) * t) >> 16);
		}
	}
	return m;
}

static void
split(int number, fixed_t (*p)[2], int n)
{
	fixed_t part[MAX_CORNERS][2];
	node_t *node;
	int side, m;

	if (number & NF_SUBSECTOR) {
		keep_polygon(number == -1 ? 0 : number & ~NF_SUBSECTOR, p, n);
		return;
	}
	node = &nodes[number];
	for (side = 0; side < 2; side++) {
		m = clip(p, n, node, side, part);
		if (m >= 3)
			split(node->children[side], part, m);
		else if (node->children[side] & NF_SUBSECTOR)
			keep_polygon(node->children[side] & ~NF_SUBSECTOR, part, 0);
	}
}

/* Whole-number square root, for seg lengths. */
static unsigned
root(unsigned long long v)
{
	unsigned long long r = 0, bit = 1ULL << 62;

	while (bit > v)
		bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (unsigned)r;
}

/* ---- the level, kept in GPU memory ---- */

/*
 * Walls and floors do not move, but for doors and lifts: the whole level is written once, as
 * quads in GPU memory and a command for each run of them with one texture and one light, and
 * the depth buffer decides what is seen. A frame then sends what changed (docs/doom.md).
 */
enum { WALL_MID, WALL_TOP, WALL_BOTTOM, WALL_HOLES, WALL_SKY, FLOOR, CEILING, SKY };

struct piece { int of, sector, texture, unique, quad, quads; short shade; unsigned char kind; };
struct group { int sector, texture, shown, first, quads, piece, pieces; short shade, grey[2]; unsigned char kind, own, hidden; };
struct seen { fixed_t floor, ceiling; short light[2], floorpic, ceilingpic, floor_group, ceiling_group; unsigned char floor_hidden, ceiling_hidden; };
struct own { const short *picture; const side_t *side; int piece, group, offset, texture; };
typedef struct { boolean istexture; int picnum, basepic, numpics, speed; } anim_def;	/* p_spec.c's anim_t */

extern anim_def anims[], *lastanim;
extern byte *rejectmatrix;

static int world_on;			/* the level is kept; 0: drawn again every frame, the port's way */
static int world_wanted = -1;
static struct piece *pieces;
static struct group *groups;
static struct seen *seen;
static int piece_count, group_count, world_quads;
static uint32_t *world_vertices;
static GLuint world_name;		/* a texture's name, so the vertices' memory is given back with the level's */
static int *sector_group;		/* per sector, its first group; one more at the end */
static int *depend_first, *depend;	/* per sector, the walls that stand on its heights */
static struct own *own;		/* walls of lines that do something: their picture may change */
static int own_count;
static int *moving, moving_count;	/* groups whose picture is one of an animation's */
static int light_seen[2] = { -1, -1 };	/* per set of commands, as the lights in struct seen and group */

#define THING_PLACES 512		/* a power of two */
/* A place's quad is 64 bytes; they are 80 apart, so that one field of many of them (a frame
 * turns every thing in view) does not land in the same few sets of the machine's write cache. */
#define PLACE_BYTES 80
struct place { const mobj_t *of; fixed_t x, y, high; angle_t angle; int lump, frame, grey; unsigned char flip; };
static struct place places[THING_PLACES];
static uint32_t *
place_quad(int i)
{
	return (uint32_t *)((uint8_t *)world_vertices + world_quads * 64 + i * PLACE_BYTES);
}

static short lately[THING_PLACES];	/* the places drawn from in this frame */
static int lately_count;

static int
grey_of(int level, int shade)
{
	if (viewplayer->fixedcolormap)
		return 255;
	/* Doom has sixteen light levels, and so a sector that glows changes half as often. A kept
	 * level does not take the gun's flash this way (flash() below). */
	level = (level >= 248 ? 255 : (level & ~15) + 8) + shade + (world_on ? 0 : extralight << 4);
	return level < 0 ? 0 : level > 255 ? 255 : level;
}

static inline uint32_t
uv_word(fixed_t u, fixed_t v)
{
	return ((uint32_t)(u >> 6) & 0xffff) | (uint32_t)(v >> 6) << 16;
}

/* A wall's quad, from the heights and textures as they are now; one of no size if it is not there. */
static void
wall_piece(const struct piece *p)
{
	seg_t *seg = &segs[p->of];
	side_t *side = seg->sidedef;
	line_t *line = seg->linedef;
	sector_t *front = seg->frontsector, *back = seg->backsector;
	uint32_t *to = world_vertices + 16 * p->quad;
	fixed_t high, low, top, tall, u1, u2, v1, v2;
	int texture, width, height, k;

	switch (p->kind) {
	case WALL_MID:
		texture = side->midtexture;
		high = front->ceilingheight;
		low = front->floorheight;
		top = (line->flags & ML_DONTPEGBOTTOM ? low + textureheight[texturetranslation[texture]] : high) + side->rowoffset;
		break;
	case WALL_TOP:
		texture = side->toptexture;
		high = front->ceilingheight;
		low = back->ceilingheight;
		top = (line->flags & ML_DONTPEGTOP ? high : low + textureheight[texturetranslation[texture]]) + side->rowoffset;
		break;
	case WALL_BOTTOM:
		texture = side->bottomtexture;
		high = back->floorheight;
		low = front->floorheight;
		top = (line->flags & ML_DONTPEGBOTTOM ? front->ceilingheight : high) + side->rowoffset;
		break;
	case WALL_SKY:
		/* between two ceilings that are both sky: sky too, and it hides what is behind it */
		texture = skytexture;
		high = front->ceilingheight;
		low = top = back->ceilingheight;
		break;
	default:
		/* a picture with holes across an opening: one copy of it, not repeated up and down */
		texture = side->midtexture;
		tall = textureheight[texturetranslation[texture]];
		high = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
		low = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
		top = (line->flags & ML_DONTPEGBOTTOM ? low + tall : high) + side->rowoffset;
		if (top < high)
			high = top;
		if (top - tall > low)
			low = top - tall;
		break;
	}
	if (!texture || high <= low) {
		for (k = 0; k < 4; k++, to += 4) {
			to[0] = seg->v1->x;
			to[1] = seg->v1->y;
			to[2] = low;
			to[3] = 0;
		}
		return;
	}
	width = texturewidthmask[texturetranslation[texture]] + 1;
	height = textureheight[texturetranslation[texture]] >> FRACBITS;
	u1 = (side->textureoffset + seg->offset) / width;
	v1 = (top - high) / height;
	u2 = u1 + seg_length[p->of] / width;
	u2 -= u1 & ~0xffff;
	u1 &= 0xffff;
	v2 = v1 + (high - low) / height;
	v2 -= v1 & ~0xffff;
	v1 &= 0xffff;
	to[0] = to[12] = seg->v1->x;
	to[1] = to[13] = seg->v1->y;
	to[4] = to[8] = seg->v2->x;
	to[5] = to[9] = seg->v2->y;
	to[2] = to[6] = high;
	to[10] = to[14] = low;
	to[3] = uv_word(v1, u1);	/* (v, u): the picture lies on its side */
	to[7] = uv_word(v1, u2);
	to[11] = uv_word(v2, u2);
	to[15] = uv_word(v2, u1);
}

/* A subsector's floor or ceiling: its polygon as a fan, two triangles of it to a quad. */
static void
plane_piece(const struct piece *p, fixed_t height)
{
	fixed_t (*c)[2] = corners + polygon[p->of].first;
	int n = polygon[p->of].count, i, k;
	uint32_t *to = world_vertices + 16 * p->quad;
	fixed_t u0 = (c[0][0] >> 6) & ~0xffff, v0 = (-c[0][1] >> 6) & ~0xffff;

	for (i = 1; i + 1 < n; i += 2) {
		int corner[4] = { 0, i, i + 1, i + 2 < n ? i + 2 : n - 1 };

		for (k = 0; k < 4; k++, to += 4) {
			to[0] = c[corner[k]][0];
			to[1] = c[corner[k]][1];
			to[2] = height;
			to[3] = uv_word((c[corner[k]][0] >> 6) - u0, (-c[corner[k]][1] >> 6) - v0);
		}
	}
}

static int
piece_order(const void *a, const void *b)
{
	const struct piece *p = a, *q = b;
	int pc = p->kind >= FLOOR ? p->kind : p->kind == WALL_HOLES, qc = q->kind >= FLOOR ? q->kind : q->kind == WALL_HOLES;

	return p->sector != q->sector ? p->sector - q->sector : pc != qc ? pc - qc : p->texture != q->texture ? p->texture - q->texture :
	       p->shade != q->shade ? p->shade - q->shade : p->unique - q->unique;
}

static void
add_piece(int of, int kind, int sector, int texture, int shade, int unique, int quads)
{
	struct piece *p = &pieces[piece_count++];

	p->of = of;
	p->kind = kind;
	p->sector = sector;
	p->texture = texture;
	p->shade = shade;
	p->unique = unique ? piece_count : 0;
	p->quads = quads;
}

/* Into piece_order: by sector first, which a count does, then the few of each sector among themselves. */
static void
sort_pieces(void)
{
	struct piece *sorted = malloc((piece_count + 1) * sizeof *sorted);
	int *start = calloc(numsectors + 3, sizeof *start), i, s;

	for (i = 0; i < piece_count; i++)
		start[pieces[i].sector + 2]++;
	for (s = 0; s <= numsectors; s++)
		start[s + 2] += start[s + 1];
	for (i = 0; i < piece_count; i++)
		sorted[start[pieces[i].sector + 1]++] = pieces[i];
	for (s = 0; s <= numsectors; s++)
		for (i = start[s] + 1; i < start[s + 1]; i++) {
			struct piece moved = sorted[i];
			int k = i;

			for (; k > start[s] && piece_order(&sorted[k - 1], &moved) > 0; k--)
				sorted[k] = sorted[k - 1];
			sorted[k] = moved;
		}
	free(start);
	free(pieces);
	pieces = sorted;
}

/* What a group's picture is right now, as a texture, and the number it has after animation. */
static GLuint
group_picture(const struct group *g, int *shown)
{
	if (g->kind == WALL_SKY || g->kind == SKY) {
		*shown = skytexture;
		return sky_texture();
	}
	if (g->kind >= FLOOR) {
		*shown = flattranslation[g->texture];
		return flat_texture(g->texture);
	}
	*shown = texturetranslation[g->texture];
	return wall_texture(g->texture);
}

static void
world_free(void)
{
	free(pieces);
	free(groups);
	free(seen);
	free(sector_group);
	free(depend_first);
	free(depend);
	free(own);
	free(moving);
	pieces = NULL;
	groups = NULL;
	seen = NULL;
	sector_group = depend_first = depend = moving = NULL;
	own = NULL;
	if (world_name)
		glDeleteTextures(1, &world_name);
	world_name = 0;
	world_on = 0;
	seglKeep(0, 0);
}

/* Makes the level's pieces, sorts them into groups, and writes all of it. */
static void
world_build(void)
{
	int i, s, room = 0, g;
	anim_def *anim;

	for (i = 0; i < numsegs; i++)
		room += segs[i].backsector ? 3 : 1;
	room += 2 * numsubsectors;
	pieces = malloc((room + 1) * sizeof *pieces);
	piece_count = 0;
	for (i = 0; i < numsegs; i++) {
		seg_t *seg = &segs[i];
		side_t *side = seg->sidedef;
		int sector = seg->frontsector - sectors, does = seg->linedef->special != 0;
		int shade = seg->v1->y == seg->v2->y ? -16 : seg->v1->x == seg->v2->x ? 16 : 0;

		if (!seg->backsector) {
			if (side->midtexture)
				add_piece(i, WALL_MID, sector, side->midtexture, shade, does, 1);
			continue;
		}
		if (seg->frontsector->ceilingpic == skyflatnum && seg->backsector->ceilingpic == skyflatnum)
			add_piece(i, WALL_SKY, numsectors, 0, 0, 0, 1);		/* the sky is a sector of its own, after the rest */
		else if (side->toptexture)
			add_piece(i, WALL_TOP, sector, side->toptexture, shade, does, 1);
		if (side->bottomtexture)
			add_piece(i, WALL_BOTTOM, sector, side->bottomtexture, shade, does, 1);
		if (side->midtexture)
			add_piece(i, WALL_HOLES, sector, side->midtexture, shade, does, 1);
	}
	for (i = 0; i < numsubsectors; i++) {
		sector_t *sector = subsectors[i].sector;
		int quads = (polygon[i].count - 1) / 2;

		if (polygon[i].count < 3)
			continue;
		add_piece(i, FLOOR, sector - sectors, sector->floorpic, 0, 0, quads);
		if (sector->ceilingpic != skyflatnum)
			add_piece(i, CEILING, sector - sectors, sector->ceilingpic, 0, 0, quads);
		else
			add_piece(i, SKY, numsectors, 0, 0, 0, quads);
	}
	sort_pieces();
	groups = malloc((piece_count + 1) * sizeof *groups);
	group_count = world_quads = 0;
	for (i = 0; i < piece_count; i++) {
		struct piece *p = &pieces[i];
		struct group *last = group_count ? &groups[group_count - 1] : NULL;

		if (!last || piece_order(&pieces[last->piece], p)) {
			last = &groups[group_count++];
			last->sector = p->sector;
			last->texture = p->texture;
			last->shade = p->shade;
			last->kind = p->kind;
			last->own = p->unique != 0;
			last->hidden = 0;
			last->first = world_quads;
			last->quads = last->pieces = 0;
			last->piece = i;
		}
		p->quad = world_quads;
		world_quads += p->quads;
		last->quads += p->quads;
		last->pieces++;
	}
	glColorKeySE(KEY);	/* the commands take it with them */
	world_vertices = world_quads ? seglMemory(world_quads * 64 + THING_PLACES * PLACE_BYTES) : NULL;
	if (!world_vertices || seglKeep(group_count + THING_PLACES, world_quads + THING_PLACES) < 0) {
		fprintf(stderr, "doom: this level is not kept in GPU memory (%d runs, %d quads)\n", group_count, world_quads);
		world_free();
		doom_gl_kept = 0;
		return;
	}
	glGenTextures(1, &world_name);
	glBindTexture(GL_TEXTURE_2D, world_name);
	seglTexturePointer(world_vertices, world_quads * 64 + THING_PLACES * PLACE_BYTES, 1, GL_COLOR_INDEX8_EXT);
	/* the things' places: a quad and a command each, showing nothing until a thing takes it */
	memset(places, 0, sizeof places);
	lately_count = 0;
	for (i = 0; i < THING_PLACES; i++) {
		seglKeptQuads(group_count + i, (uint8_t *)place_quad(i) - (world_quads + i) * 64, world_quads + i, 1, 0, 255, 1);
		seglKeptShown(group_count + i, 0);
	}

	seen = malloc(numsectors * sizeof *seen);
	sector_group = malloc((numsectors + 2) * sizeof *sector_group);
	depend_first = calloc(numsectors + 2, sizeof *depend_first);
	depend = malloc((2 * piece_count + 1) * sizeof *depend);
	own = malloc((piece_count + 1) * sizeof *own);
	moving = malloc((group_count + 1) * sizeof *moving);
	own_count = moving_count = 0;
	for (s = 0; s < numsectors; s++) {
		seen[s].floor = sectors[s].floorheight;
		seen[s].ceiling = sectors[s].ceilingheight;
		seen[s].light[0] = seen[s].light[1] = sectors[s].lightlevel >> 4;
		seen[s].floorpic = sectors[s].floorpic;
		seen[s].ceilingpic = sectors[s].ceilingpic;
		seen[s].floor_group = seen[s].ceiling_group = -1;
		seen[s].floor_hidden = seen[s].ceiling_hidden = 0;
	}
	/* which walls stand on which sector's heights: counted, then listed */
	for (i = 0; i < piece_count; i++)
		if (pieces[i].kind < FLOOR) {
			seg_t *seg = &segs[pieces[i].of];

			depend_first[seg->frontsector - sectors + 2]++;
			if (seg->backsector && seg->backsector != seg->frontsector)
				depend_first[seg->backsector - sectors + 2]++;
		}
	for (s = 0; s < numsectors; s++)
		depend_first[s + 2] += depend_first[s + 1];
	for (i = 0; i < piece_count; i++) {
		struct piece *p = &pieces[i];

		if (p->kind < FLOOR) {
			seg_t *seg = &segs[p->of];

			depend[depend_first[seg->frontsector - sectors + 1]++] = i;
			if (seg->backsector && seg->backsector != seg->frontsector)
				depend[depend_first[seg->backsector - sectors + 1]++] = i;
			if (p->unique) {
				struct own *o = &own[own_count++];

				o->side = seg->sidedef;
				o->picture = p->kind == WALL_TOP ? &o->side->toptexture : p->kind == WALL_BOTTOM ? &o->side->bottomtexture :
					     &o->side->midtexture;
				o->piece = i;
				o->texture = p->texture;
				o->offset = o->side->textureoffset;
			}
			wall_piece(p);
		} else {
			plane_piece(p, p->kind == FLOOR ? subsectors[p->of].sector->floorheight : subsectors[p->of].sector->ceilingheight);
		}
	}
	light_seen[0] = light_seen[1] = viewplayer->fixedcolormap != 0;
	for (g = 0, s = 0; g < group_count; g++) {
		struct group *group = &groups[g];
		GLuint name = group_picture(group, &group->shown);

		while (s <= group->sector)
			sector_group[s++] = g;
		group->grey[0] = group->grey[1] = group->sector < numsectors ? grey_of(sectors[group->sector].lightlevel, group->shade) : 255;
		seglKeptQuads(g, world_vertices, group->first, group->quads, name, group->grey[0], group->kind == WALL_HOLES);
		if (group->sector == numsectors)
			continue;
		for (anim = anims; anim < lastanim; anim++)
			if (!anim->istexture == (group->kind >= FLOOR) && group->texture >= anim->basepic &&
			    group->texture < anim->basepic + anim->numpics) {
				moving[moving_count++] = g;
				break;
			}
		if (group->kind == FLOOR)
			seen[group->sector].floor_group = g;
		else if (group->kind == CEILING)
			seen[group->sector].ceiling_group = g;
	}
	for (i = 0; i < own_count; i++)
		for (g = sector_group[pieces[own[i].piece].sector];; g++)
			if (groups[g].piece == own[i].piece) {
				own[i].group = g;
				break;
			}
	while (s <= numsectors + 1)
		sector_group[s++] = group_count;
	world_on = doom_gl_kept = 1;
	if (getenv("DOOM_GL_DEBUG"))
		fprintf(stderr, "doom_gl: the level is %d quads in %d runs; %d on lines that do something, %d that may change picture\n",
			world_quads, group_count, own_count, moving_count);
}

/* A sector's floor or ceiling has moved: its planes go to the new height, and the walls on it. */
static void
sector_moved(int s)
{
	const sector_t *sector = &sectors[s];
	int i, g;

	for (i = depend_first[s]; i < depend_first[s + 1]; i++)
		wall_piece(&pieces[depend[i]]);
	for (g = sector_group[s]; g < sector_group[s + 1]; g++)
		if (groups[g].kind >= FLOOR) {
			uint32_t *to = world_vertices + 16 * groups[g].first + 2;
			fixed_t height = groups[g].kind == FLOOR ? sector->floorheight : sector->ceilingheight;

			for (i = 4 * groups[g].quads; i > 0; i--, to += 4)
				*to = height;
		}
	seen[s].floor = sector->floorheight;
	seen[s].ceiling = sector->ceilingheight;
}

static void
sector_relit(int s, int set)
{
	int g;

	seen[s].light[set] = sectors[s].lightlevel >> 4;
	for (g = sector_group[s]; g < sector_group[s + 1]; g++) {
		int grey = grey_of(sectors[s].lightlevel, groups[g].shade);

		if (grey != groups[g].grey[set])
			seglKeptGreyIn(set, g, groups[g].grey[set] = grey);
	}
}

static void
sector_repainted(int s)
{
	int g;

	seen[s].floorpic = sectors[s].floorpic;
	seen[s].ceilingpic = sectors[s].ceilingpic;
	for (g = sector_group[s]; g < sector_group[s + 1]; g++)
		if (groups[g].kind >= FLOOR) {
			groups[g].texture = groups[g].kind == FLOOR ? sectors[s].floorpic : sectors[s].ceilingpic;
			seglKeptTexture(g, group_picture(&groups[g], &groups[g].shown));
		}
}

/* What has changed since the last frame goes to the GPU's copy: heights, lights, pictures. */
static void
world_update(void)
{
	/* a run's grey is one word of each of its commands, 64 bytes apart, which the write cache
	 * holds few of: only the set this frame is built in is brought up to date */
	int set = seglSet(), light = viewplayer->fixedcolormap != 0, relight = light != light_seen[set], s, i;
	const sector_t *sector = sectors;
	struct seen *was = seen;
	struct own *o = own;
	const fixed_t eye = viewz;

	light_seen[set] = light;
	for (s = 0; s < numsectors; s++, sector++, was++) {
		fixed_t floor = sector->floorheight, ceiling = sector->ceilingheight;
		int hidden;

		if (floor != was->floor || ceiling != was->ceiling)
			sector_moved(s);
		/* a floor is seen from above and a ceiling from below, as Doom draws them: never their backs */
		if ((hidden = floor >= eye) != was->floor_hidden && was->floor_group >= 0)
			seglKeptShown(was->floor_group, (was->floor_hidden = hidden) ? 0 : groups[was->floor_group].quads);
		if ((hidden = ceiling <= eye) != was->ceiling_hidden && was->ceiling_group >= 0)
			seglKeptShown(was->ceiling_group, (was->ceiling_hidden = hidden) ? 0 : groups[was->ceiling_group].quads);
		if (relight || sector->lightlevel >> 4 != was->light[set])
			sector_relit(s, set);
		if (sector->floorpic != was->floorpic || sector->ceilingpic != was->ceilingpic)
			sector_repainted(s);
	}
	/* a wall of a line that does something: a switch's picture, a wall that slides */
	for (i = own_count; i > 0; i--, o++) {
		int texture = *o->picture, offset = o->side->textureoffset;

		if (texture == o->texture && offset == o->offset)
			continue;
		o->offset = offset;
		pieces[o->piece].texture = groups[o->group].texture = o->texture = texture;
		wall_piece(&pieces[o->piece]);
		seglKeptTexture(o->group, group_picture(&groups[o->group], &groups[o->group].shown));
	}
	for (i = 0; i < moving_count; i++) {
		struct group *group = &groups[moving[i]];
		int now = group->kind >= FLOOR ? flattranslation[group->texture] : texturetranslation[group->texture];

		if (now != group->shown)
			seglKeptTexture(moving[i], group_picture(group, &group->shown));
	}
}

/* The things that may be in view: in a sector the viewer's can see into, and not behind or beside it. */
static void thing_billboard(mobj_t *thing, int level);

/*
 * A thing in view keeps a place among the kept commands while it stays in view: a frame then
 * turns its quad to face the viewer, and sends a picture, a height or a light only when
 * they change. A thing no longer drawn gives its place up.
 */
static void
thing_kept(mobj_t *thing, int level)
{
	spriteframe_t *frame = &sprites[thing->sprite].spriteframes[thing->frame & FF_FRAMEMASK];
	unsigned at = ((unsigned)(size_t)thing >> 4) * 2654435761u >> 23, probe;
	int turn = 0, lump, flip, grey, command;
	struct place *place = NULL;
	fixed_t left, width, x1, y1, high;
	uint32_t *to;
	GLuint name;

	if (frame->rotate)
		turn = (R_PointToAngle(thing->x, thing->y) - thing->angle + (unsigned)(ANG45 / 2) * 9) >> 29;
	lump = frame->lump[turn];
	flip = frame->flip[turn];
	/* its own place, or else one nothing was drawn from in the last frame or this one */
	for (probe = 0; probe < 8; probe++) {
		struct place *p = &places[(at + probe) & (THING_PLACES - 1)];

		if (p->of == thing) {
			place = p;
			break;
		}
		if (!place && p->frame + 1 < frame_number)
			place = p;
	}
	if (!place || (thing->flags & MF_SHADOW)) {
		thing_billboard(thing, level);	/* no room, or one that is blended */
		return;
	}
	doom_view[3]++;
	command = group_count + (int)(place - places);
	to = place_quad(place - places);
	grey = grey_of(thing->frame & FF_FULLBRIGHT ? 255 : level, 0);
	high = thing->z + spritetopoffset[lump];
	if (place->of != thing || place->lump != lump || place->flip != flip) {
		name = sprite_gl[lump] ? sprite_gl[lump] : sprite_texture(lump);
		if (!name) {
			place->of = NULL;
			return;
		}
		seglKeptTexture(command, name);
		to[3] = to[15] = (flip ? 1024 : 0) << 16;	/* (v, u): the picture lies on its side */
		to[7] = to[11] = (flip ? 0 : 1024) << 16;
		to[11] |= 1024;
		to[15] |= 1024;
		place->of = thing;
		place->lump = lump;
		place->flip = flip;
		place->high = high + 1;		/* and so the heights and the corners are written below */
		place->grey = -1;
	}
	if (place->high != high) {
		place->high = high;
		to[2] = to[6] = high;
		to[10] = to[14] = high - (sprite_height[lump] << FRACBITS);
		place->x = thing->x + 1;
	}
	if (place->x != thing->x || place->y != thing->y || place->angle != viewangle) {
		/* the viewer's right is (sin, -cos) of the view angle */
		width = spritewidth[lump];
		left = flip ? width - spriteoffset[lump] : spriteoffset[lump];
		place->x = thing->x;
		place->y = thing->y;
		place->angle = viewangle;
		x1 = thing->x - FixedMul(left, viewsin);
		y1 = thing->y + FixedMul(left, viewcos);
		to[0] = to[12] = x1;
		to[1] = to[13] = y1;
		to[4] = to[8] = x1 + FixedMul(width, viewsin);
		to[5] = to[9] = y1 - FixedMul(width, viewcos);
	}
	if (place->grey != grey)
		seglKeptGrey(command, place->grey = grey);
	if (place->frame + 1 < frame_number)
		seglKeptShown(command, 1);	/* it was not drawn in the last frame */
	if (place->frame != frame_number && lately_count < THING_PLACES)
		lately[lately_count++] = place - places;
	place->frame = frame_number;
}

/* The places drawn from in the last frame and not in this one show nothing from now on. */
static void
things_gone(void)
{
	static short before[THING_PLACES];
	static int before_count;
	int i;

	for (i = 0; i < before_count; i++)
		if (places[before[i]].frame != frame_number)
			seglKeptShown(group_count + before[i], 0);
	memcpy(before, lately, lately_count * sizeof lately[0]);
	before_count = lately_count;
	lately_count = 0;
}

static void
world_things(void)
{
	int from = (viewplayer->mo->subsector->sector - sectors) * numsectors, s;
	/* in whole map units, with twelve bits of the view's direction: products fit a word */
	const int cs = viewcos >> 4, sn = viewsin >> 4, vx = viewx >> FRACBITS, vy = viewy >> FRACBITS, margin = 96 << 12;
	const mobj_t *self = viewplayer->mo;
	const byte *reject = rejectmatrix;

	for (s = 0; s < numsectors; s++) {
		sector_t *sector = &sectors[s];
		mobj_t *thing = sector->thinglist;

		if (!thing || (reject[(from + s) >> 3] & (1 << ((from + s) & 7))))
			continue;
		for (; thing; thing = thing->snext) {
			int dx = (thing->x >> FRACBITS) - vx, dy = (thing->y >> FRACBITS) - vy;
			int ahead = dx * cs + dy * sn + margin, beside = dx * sn - dy * cs;

			if (ahead < 0 || beside > ahead || -beside > ahead || thing == self)
				continue;
			thing_kept(thing, sector->lightlevel);
		}
	}
	things_gone();
}

/* A new level: everything kept per seg, subsector and texture starts again. */
static void
new_level(void)
{
	static int textures_then, flats_then, sprites_then;
	fixed_t box[4][2];
	int i, low = -16000, high = 16000;

	world_free();
	/* the last level's pictures go; the screen's texture (doom_video.c) is not ours */
	if (wall_gl) {
		glDeleteTextures(textures_then, wall_gl);
		glDeleteTextures(flats_then, flat_gl);
		glDeleteTextures(sprites_then, sprite_gl);
		glDeleteTextures(1, &sky_gl);
		sky_gl = 0;
	}
	textures_then = numtextures;
	sprites_then = numspritelumps;
	free(seg_length);
	free(seg_frame);
	free(subsector_frame);
	free(polygon);
	free(wall_gl);
	free(flat_gl);
	free(sprite_gl);
	free(sprite_height);
	flat_count = W_GetNumForName("F_END") - firstflat;
	flats_then = flat_count;
	seg_length = malloc(numsegs * sizeof *seg_length);
	seg_frame = calloc(numsegs, sizeof *seg_frame);
	subsector_frame = calloc(numsubsectors, sizeof *subsector_frame);
	polygon = calloc(numsubsectors, sizeof *polygon);
	wall_gl = calloc(numtextures, sizeof *wall_gl);
	flat_gl = calloc(flat_count, sizeof *flat_gl);
	sprite_gl = calloc(numspritelumps, sizeof *sprite_gl);
	sprite_height = calloc(numspritelumps, sizeof *sprite_height);
	for (i = 0; i < numsegs; i++) {
		long long dx = (segs[i].v2->x - segs[i].v1->x) >> 8, dy = (segs[i].v2->y - segs[i].v1->y) >> 8;

		seg_length[i] = (fixed_t)root(dx * dx + dy * dy) << 8;
	}
	corner_count = 0;
	box[0][0] = box[3][0] = low * FRACUNIT;
	box[1][0] = box[2][0] = high * FRACUNIT;
	box[0][1] = box[1][1] = low * FRACUNIT;
	box[2][1] = box[3][1] = high * FRACUNIT;
	if (numnodes > 0)
		split(numnodes - 1, box, 4);
	level_segs = segs;
	level_count = numsegs;
	if (world_wanted < 0) {
		const char *port = getenv("DOOM_PORT");

		world_wanted = !(port && strstr(port, "view"));	/* DOOM_PORT=view: what Doom finds in view, every frame */
	}
	if (world_wanted)
		world_build();
	else
		doom_gl_kept = 0;
	if (getenv("DOOM_GL_DEBUG"))
		fprintf(stderr, "doom_gl: %d segs, %d subsectors, %d polygon corners, %d flats\n", numsegs, numsubsectors,
			corner_count, flat_count);
}

/* ---- drawing ---- */

static void
light(int level)
{
	level = grey_of(level, 0);
	glColor4ub(level, level, level, 255);
}

/* A wall: from (x1,y1) to (x2,y2), between two heights, `top` being where the texture's first
 * row sits. Texture coordinates are kept near zero, where the device has them exact. */
static void
wall(seg_t *seg, int texture, fixed_t high, fixed_t low, fixed_t top, int level, int holes)
{
	GLuint name = wall_texture(texture);
	int width = texturewidthmask[texturetranslation[texture]] + 1;
	int height = textureheight[texturetranslation[texture]] >> FRACBITS;
	fixed_t u1 = (seg->sidedef->textureoffset + seg->offset) / width, u2;
	fixed_t v1 = (top - high) / height, v2;

	if (!name || high <= low)
		return;
	u2 = u1 + seg_length[seg - segs] / width;
	u2 -= u1 & ~0xffff;
	u1 &= 0xffff;
	v2 = v1 + (high - low) / height;
	v2 -= v1 & ~0xffff;
	v1 &= 0xffff;
	quad_xyz[0] = quad_xyz[9] = seg->v1->x;
	quad_xyz[1] = quad_xyz[10] = seg->v1->y;
	quad_xyz[3] = quad_xyz[6] = seg->v2->x;
	quad_xyz[4] = quad_xyz[7] = seg->v2->y;
	quad_xyz[2] = quad_xyz[5] = high;
	quad_xyz[8] = quad_xyz[11] = low;
	quad_uv[1] = quad_uv[7] = u1;	/* (v, u): the picture lies on its side */
	quad_uv[3] = quad_uv[5] = u2;
	quad_uv[0] = quad_uv[2] = v1;
	quad_uv[4] = quad_uv[6] = v2;
	/* walls along the grid get Doom's slight shading by direction */
	if (seg->v1->y == seg->v2->y)
		level -= 16;
	else if (seg->v1->x == seg->v2->x)
		level += 16;
	light(level);
	if (holes)
		glEnable(GL_ALPHA_TEST);
	glBindTexture(GL_TEXTURE_2D, name);
	glVertexPointer(3, GL_FIXED, 0, quad_xyz);
	glTexCoordPointer(2, GL_FIXED, 0, quad_uv);
	glDrawArrays(GL_QUADS, 0, 4);
	if (holes)
		glDisable(GL_ALPHA_TEST);
}

/* Doom found (part of) the current seg visible: draw the walls it stands for, once a frame. */
void
R_StoreWallRange(int start, int stop)
{
	seg_t *seg = curline;
	side_t *side = seg->sidedef;
	line_t *line = seg->linedef;
	sector_t *front = seg->frontsector, *back = seg->backsector;
	int level = front->lightlevel;

	if (!in_view) {
		R_StoreWallRange_soft(start, stop);
		return;
	}
	if (seg_frame[seg - segs] == frame_number)
		return;
	seg_frame[seg - segs] = frame_number;
	if (!back) {
		fixed_t top = line->flags & ML_DONTPEGBOTTOM ?
			front->floorheight + textureheight[texturetranslation[side->midtexture]] : front->ceilingheight;

		if (side->midtexture)
			wall(seg, side->midtexture, front->ceilingheight, front->floorheight, top + side->rowoffset, level, 0);
		return;
	}
	/* above the opening, unless both sides are open to the sky */
	if (back->ceilingheight < front->ceilingheight && side->toptexture &&
	    !(front->ceilingpic == skyflatnum && back->ceilingpic == skyflatnum)) {
		fixed_t top = line->flags & ML_DONTPEGTOP ? front->ceilingheight :
			back->ceilingheight + textureheight[texturetranslation[side->toptexture]];

		wall(seg, side->toptexture, front->ceilingheight, back->ceilingheight, top + side->rowoffset, level, 0);
	}
	/* below it */
	if (back->floorheight > front->floorheight && side->bottomtexture) {
		fixed_t top = line->flags & ML_DONTPEGBOTTOM ? front->ceilingheight : back->floorheight;

		wall(seg, side->bottomtexture, back->floorheight, front->floorheight, top + side->rowoffset, level, 0);
	}
	/* and a texture with holes across it: one copy of it, not repeated up and down */
	if (side->midtexture) {
		fixed_t open_high = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
		fixed_t open_low = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
		fixed_t tall = textureheight[texturetranslation[side->midtexture]];
		fixed_t top = (line->flags & ML_DONTPEGBOTTOM ? open_low + tall : open_high) + side->rowoffset;
		fixed_t high = top < open_high ? top : open_high, low = top - tall > open_low ? top - tall : open_low;

		wall(seg, side->midtexture, high, low, top, level, 1);
	}
}

/* A subsector's floor or ceiling: its polygon at one height, the flat repeating every 64 units. */
static void
plane(int subsector, fixed_t height, int picture, int level)
{
	fixed_t (*p)[2] = corners + polygon[subsector].first;
	int n = polygon[subsector].count, i;
	GLuint name = flat_texture(picture);
	fixed_t u0, v0;

	if (n < 3 || !name)
		return;
	u0 = (p[0][0] >> 6) & ~0xffff;
	v0 = (-p[0][1] >> 6) & ~0xffff;
	for (i = 0; i < n; i++) {
		fan_xyz[i * 3] = p[i][0];
		fan_xyz[i * 3 + 1] = p[i][1];
		fan_xyz[i * 3 + 2] = height;
		fan_uv[i * 2] = (p[i][0] >> 6) - u0;
		fan_uv[i * 2 + 1] = (-p[i][1] >> 6) - v0;
	}
	light(level);
	glBindTexture(GL_TEXTURE_2D, name);
	glVertexPointer(3, GL_FIXED, 0, fan_xyz);
	glTexCoordPointer(2, GL_FIXED, 0, fan_uv);
	glDrawArrays(GL_TRIANGLE_FAN, 0, n);
}

/* A thing: its picture for the angle it is seen from, upright and facing the viewer. */
static void
thing_billboard(mobj_t *thing, int level)
{
	spriteframe_t *frame = &sprites[thing->sprite].spriteframes[thing->frame & FF_FRAMEMASK];
	int turn = 0, lump, flip;
	fixed_t left, width, x1, y1, x2, y2, high, low;
	GLuint name;

	if (frame->rotate)
		turn = (R_PointToAngle(thing->x, thing->y) - thing->angle + (unsigned)(ANG45 / 2) * 9) >> 29;
	lump = frame->lump[turn];
	flip = frame->flip[turn];
	name = sprite_gl[lump] ? sprite_gl[lump] : sprite_texture(lump);
	if (!name)
		return;
	doom_view[3]++;
	width = spritewidth[lump];
	left = flip ? width - spriteoffset[lump] : spriteoffset[lump];
	/* the viewer's right is (sin, -cos) of the view angle */
	x1 = thing->x - FixedMul(left, viewsin);
	y1 = thing->y + FixedMul(left, viewcos);
	x2 = x1 + FixedMul(width, viewsin);
	y2 = y1 - FixedMul(width, viewcos);
	high = thing->z + spritetopoffset[lump];
	low = high - (sprite_height[lump] << FRACBITS);
	quad_xyz[0] = quad_xyz[9] = x1;
	quad_xyz[1] = quad_xyz[10] = y1;
	quad_xyz[3] = quad_xyz[6] = x2;
	quad_xyz[4] = quad_xyz[7] = y2;
	quad_xyz[2] = quad_xyz[5] = high;
	quad_xyz[8] = quad_xyz[11] = low;
	quad_uv[1] = quad_uv[7] = flip ? FRACUNIT : 0;	/* (v, u): the picture lies on its side */
	quad_uv[3] = quad_uv[5] = flip ? 0 : FRACUNIT;
	quad_uv[0] = quad_uv[2] = 0;
	quad_uv[4] = quad_uv[6] = FRACUNIT;
	/* the half-seen are a dark shape blended over what is behind */
	if (thing->flags & MF_SHADOW)
		seglQuad(quad_xyz, quad_uv, name, 0, 110, 1);
	else
		seglQuad(quad_xyz, quad_uv, name, grey_of(thing->frame & FF_FULLBRIGHT ? 255 : level, 0), 255, 1);
}

/* Doom reached a subsector front to back: its floor, ceiling and things, then its segs. */
void
R_Subsector(int number)
{
	subsector_t *sub = &subsectors[number];
	sector_t *sector = sub->sector;
	seg_t *seg = &segs[sub->firstline];
	mobj_t *thing;
	int count = sub->numlines;

	if (!in_view) {
		R_Subsector_soft(number);
		return;
	}
	frontsector = sector;
	if (subsector_frame[number] != frame_number) {
		subsector_frame[number] = frame_number;
		if (sector->floorheight < viewz)
			plane(number, sector->floorheight, sector->floorpic, sector->lightlevel);
		if (sector->ceilingheight > viewz && sector->ceilingpic != skyflatnum)
			plane(number, sector->ceilingheight, sector->ceilingpic, sector->lightlevel);
	}
	if (sector->validcount != validcount) {
		sector->validcount = validcount;
		for (thing = sector->thinglist; thing; thing = thing->snext)
			if (thing != viewplayer->mo)
				thing_billboard(thing, sector->lightlevel);
	}
	while (count--)
		R_AddLine(seg++);
}

/* The sky: one picture a quarter turn wide behind everything, sliding with the view angle. */
static void
sky(void)
{
	GLuint name = sky_texture();
	fixed_t u = (fixed_t)(((viewangle + ANG45) >> 22) << 8), v = ((100 - viewheight / 2) << FRACBITS) / 128;
	fixed_t far = -(FRACUNIT - 2);
	int k;

	if (!name)
		return;
	u &= 0xffff;
	for (k = 0; k < 4; k++) {
		quad_xyz[k * 3] = (k == 1 || k == 2) ? FRACUNIT : 0;
		quad_xyz[k * 3 + 1] = k >= 2 ? FRACUNIT : 0;
		quad_xyz[k * 3 + 2] = far;
		quad_uv[k * 2] = (k == 1 || k == 2) ? u - FRACUNIT + 4 * FRACUNIT : u + 4 * FRACUNIT;
		quad_uv[k * 2 + 1] = v + (k >= 2 ? (viewheight << FRACBITS) / 128 : 0);
	}
	glColor4ub(255, 255, 255, 255);
	glBindTexture(GL_TEXTURE_2D, name);
	glVertexPointer(3, GL_FIXED, 0, quad_xyz);
	glTexCoordPointer(2, GL_FIXED, 0, quad_uv);
	glDrawArrays(GL_QUADS, 0, 4);
}

/*
 * The gun's flash lights everything for a tic or two. A new grey for every run of the level
 * would be a thousand stores: one white rectangle is added to the view instead, in the pass
 * that adds, before the weapon and the screen go on top.
 */
static void
flash(void)
{
	static const GLfixed over[12] = { 0, 0, FRACUNIT - 2, FRACUNIT, 0, FRACUNIT - 2, FRACUNIT, FRACUNIT, FRACUNIT - 2,
					  0, FRACUNIT, FRACUNIT - 2 };

	if (!extralight || viewplayer->fixedcolormap)
		return;
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glColor4ub(255, 255, 255, extralight > 3 ? 32 : extralight << 3);	/* about what a level more does to a dim room */
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(3, GL_FIXED, 0, over);
	glDrawArrays(GL_QUADS, 0, 4);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
}

/* The player's weapon: flat pictures over the view, placed as Doom places them on a 320-wide screen. */
static void
weapon(void)
{
	sector_t *sector = viewplayer->mo->subsector->sector;
	int i, k;

	for (i = 0; i < NUMPSPRITES; i++) {
		pspdef_t *psp = &viewplayer->psprites[i];
		spriteframe_t *frame;
		fixed_t scale = (scaledviewwidth << FRACBITS) / 320, x, y, w, h;
		GLuint name;
		int lump, flip;

		if (!psp->state)
			continue;
		frame = &sprites[psp->state->sprite].spriteframes[psp->state->frame & FF_FRAMEMASK];
		lump = frame->lump[0];
		flip = frame->flip[0];
		name = sprite_texture(lump);
		if (!name)
			continue;
		x = (scaledviewwidth << FRACBITS) / 2 + FixedMul(psp->sx - 160 * FRACUNIT - spriteoffset[lump], scale);
		y = (viewheight << FRACBITS) / 2 + FixedMul(psp->sy - spritetopoffset[lump] - 100 * FRACUNIT - FRACUNIT / 2, scale);
		w = FixedMul(spritewidth[lump], scale);
		h = FixedMul(sprite_height[lump] << FRACBITS, scale);
		for (k = 0; k < 4; k++) {
			int right = k == 1 || k == 2, low = k >= 2;

			quad_xyz[k * 3] = (x + (right ? w : 0)) / scaledviewwidth;
			quad_xyz[k * 3 + 1] = (y + (low ? h : 0)) / viewheight;
			quad_xyz[k * 3 + 2] = 0;
			quad_uv[k * 2 + 1] = right != flip ? FRACUNIT : 0;	/* (v, u) */
			quad_uv[k * 2] = low ? FRACUNIT : 0;
		}
		if (viewplayer->powers[pw_invisibility] > 4 * 32 || viewplayer->powers[pw_invisibility] & 8) {
			glEnable(GL_BLEND);
			glColor4ub(0, 0, 0, 110);
		} else {
			light(psp->state->frame & FF_FULLBRIGHT ? 255 : sector->lightlevel);
		}
		glBindTexture(GL_TEXTURE_2D, name);
		glVertexPointer(3, GL_FIXED, 0, quad_xyz);
		glTexCoordPointer(2, GL_FIXED, 0, quad_uv);
		glDrawArrays(GL_QUADS, 0, 4);
		glDisable(GL_BLEND);
	}
}

/* Column-major, for glLoadMatrixx. */
static void
load(const GLfixed rows[4][4])
{
	GLfixed m[16];
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			m[j * 4 + i] = rows[i][j];
	glLoadMatrixx(m);
}

static GLfixed last_projection[4][4], last_view[4][4];

/* glOrthox(0, 1, 1, 0, -1, 1), column by column. */
static const GLfixed flat_view[16] = { 2 * FRACUNIT, 0, 0, 0, 0, -2 * FRACUNIT, 0, 0, 0, 0, -FRACUNIT, 0, -FRACUNIT, FRACUNIT, 0, FRACUNIT };

void
R_RenderPlayerView(player_t *player)
{
	unsigned began;
	int width, height, y;

	__asm__ volatile("rdcycle %0" : "=r"(began));
	GLfixed projection[4][4] = {{0}}, view[4][4] = {{0}};
	fixed_t far = 8192 * FRACUNIT;

	if (!doom_gl) {
		R_RenderPlayerView_soft(player);
		return;
	}
	R_SetupFrame(player);
	if (segs != level_segs || numsegs != level_count) {
		unsigned now;

		new_level();
		__asm__ volatile("rdcycle %0" : "=r"(now));
		if (getenv("DOOM_GL_DEBUG"))
			fprintf(stderr, "doom_gl: making the level for the GPU took %uk instructions\n", (now - began) / 1000);
	}
	frame_number++;
	doom_gl_scene = 1;
	page_held = 0;
	/* The 2D picture is laid over the view: where the view shows, it holds the key. Doom
	 * notes every rectangle it draws on the screen, so only the rows drawn on since the
	 * last time need the key again (messages, menus; the first frame of a view). */
	{
		int first = keyed ? dirtybox[BOXBOTTOM] : 0, last = keyed ? dirtybox[BOXTOP] : SCREENHEIGHT - 1;

		if (first < viewwindowy)
			first = viewwindowy;
		if (last >= viewwindowy + viewheight)
			last = viewwindowy + viewheight - 1;
		for (y = first; y <= last; y++)
			memset(screens[0] + y * SCREENWIDTH + viewwindowx, KEY, scaledviewwidth);
		keyed_first = first;
		keyed_last = last;
		M_ClearBox(dirtybox);
		keyed = 1;
	}

	seglSize(&width, &height);
	/* the window may be any size: the view keeps its place on Doom's 320 by 200 screen */
	glViewport(viewwindowx * width / SCREENWIDTH, height - (viewwindowy + viewheight) * height / SCREENHEIGHT,
		scaledviewwidth * width / SCREENWIDTH, viewheight * height / SCREENHEIGHT);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glColorKeySE(KEY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	/* Doom's view: a quarter turn wide, square pixels; x and y on the ground, z up */
	projection[0][0] = FRACUNIT;
	projection[1][1] = (scaledviewwidth << FRACBITS) / viewheight;
	projection[2][2] = -FixedDiv(far + NEAR, far - NEAR);
	projection[2][3] = -FixedDiv(FixedMul(2 * NEAR, far >> 8), (far - NEAR) >> 8);
	projection[3][2] = -FRACUNIT;
	view[0][0] = viewsin;
	view[0][1] = -viewcos;
	view[0][3] = -(FixedMul(viewsin, viewx) - FixedMul(viewcos, viewy));
	view[1][2] = FRACUNIT;
	view[1][3] = -viewz;
	view[2][0] = -viewcos;
	view[2][1] = -viewsin;
	view[2][3] = FixedMul(viewcos, viewx) + FixedMul(viewsin, viewy);
	view[3][3] = FRACUNIT;
	memcpy(last_projection, projection, sizeof projection);
	memcpy(last_view, view, sizeof view);
	glMatrixMode(GL_PROJECTION);
	load(projection);
	glMatrixMode(GL_MODELVIEW);
	load(view);
	if (world_on) {
		/* the level is there already: its matrices, what has changed, and the things */
		seglKeptMatrices();
		for (y = sector_group[numsectors]; y < sector_group[numsectors + 1]; y++) {
			/* the sky's runs: its picture where sky() below would put it, by the pixel */
			int vw = scaledviewwidth * width / SCREENWIDTH, vh = viewheight * height / SCREENHEIGHT;
			int vx = viewwindowx * width / SCREENWIDTH, vy = viewwindowy * height / SCREENHEIGHT;
			fixed_t u = ((fixed_t)(((viewangle + ANG45) >> 22) << 8) & 0xffff) + 4 * FRACUNIT;
			fixed_t v = ((100 - viewheight / 2) << FRACBITS) / 128;

			if (vw > 0 && vh > 0)
				seglKeptLaid(y, u + (vx << FRACBITS) / vw, v - ((vy * viewheight) << 9) / vh, -(1024 << FRACBITS) / vw,
					((8 * viewheight) << FRACBITS) / vh);
		}
		world_update();
		world_things();
	} else {
		R_ClearClipSegs();
		in_view = 1;
		R_RenderBSPNode(numnodes - 1);
		in_view = 0;
	}

	/* the sky and then the weapon, in units of the view (1 by 1, y down: exact in 16.16). The
	 * sky is as far away as depth goes, behind everything whenever it is drawn. */
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(flat_view);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	sky();
	if (world_on)
		flash();

	/* the weapon, flat on top */
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_ALPHA_TEST);
	weapon();
	glDisable(GL_ALPHA_TEST);
	{
		unsigned now;

		__asm__ volatile("rdcycle %0" : "=r"(now));
		doom_view[0] += now - began;
	}
}

/* ---- the message at the top of the view ---- */

extern hu_stext_t w_message;	/* hu_stuff.c's, made reachable by doom.sh */
extern boolean message_on, chat_on;
void HU_Drawer(void);

/*
 * Doom draws its message again every frame, a letter at a time, and the view then has to put
 * the key back under it. While the words are the same and nothing else has drawn over them,
 * they are left on the screen as they are. (d_main.c calls this for HU_Drawer.)
 */
void
HU_Drawer_kept(void)
{
	static unsigned shown;	/* a sum of the words on the screen; 0 for none */
	unsigned now = 0;
	int box[4], i, k, top = SCREENHEIGHT, bottom = 0, tall = SHORT(w_message.l[0].f[0]->height) + 1;

	static int every_frame = -1;

	if (every_frame < 0) {
		const char *port = getenv("DOOM_PORT");

		every_frame = port && strstr(port, "message");	/* DOOM_PORT=message: as the port does, to compare */
	}
	if (!doom_gl || automapactive || chat_on || viewwindowx || every_frame) {
		HU_Drawer();
		shown = ~0u;
		return;
	}
	for (i = 0; i < w_message.h; i++) {
		const hu_textline_t *line = &w_message.l[i];

		if (line->y < top)
			top = line->y;
		if (line->y + tall > bottom)
			bottom = line->y + tall;
		for (k = 0; message_on && k < line->len; k++)
			now = now * 31 + (unsigned char)line->l[k];
	}
	if (message_on)
		now = (now * 31 + w_message.cl) | 1;
	if (bottom > viewwindowy + viewheight)
		bottom = viewwindowy + viewheight;
	if (now == shown && (keyed_last < top || keyed_first >= bottom))
		return;
	/* other words, or none: the key under them first, unless the view has just put it there */
	if (shown && !(keyed_first <= top && keyed_last >= bottom - 1))
		for (i = top; i < bottom; i++)
			memset(screens[0] + i * SCREENWIDTH, KEY, SCREENWIDTH);
	memcpy(box, dirtybox, sizeof box);
	HU_Drawer();
	memcpy(dirtybox, box, sizeof box);	/* these rows are ours to clear, not the view's */
	shown = now;
}

/*
 * The last view once more, for a frame in which Doom draws none (the screen melt): the kept
 * level and the things in their places, as they were, and the sky behind them.
 */
void
doom_gl_again(void)
{
	int width, height;

	if (!world_on)
		return;
	seglSize(&width, &height);
	glViewport(viewwindowx * width / SCREENWIDTH, height - (viewwindowy + viewheight) * height / SCREENHEIGHT,
		scaledviewwidth * width / SCREENWIDTH, viewheight * height / SCREENHEIGHT);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glMatrixMode(GL_PROJECTION);
	load(last_projection);
	glMatrixMode(GL_MODELVIEW);
	load(last_view);
	seglKeptMatrices();
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(flat_view);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	sky();
	doom_gl_scene = 1;
}
