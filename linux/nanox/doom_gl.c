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
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES/segl.h>
#include "mcw.h"
#include "mcwx.h"
#include "doomdef.h"
#include "doomstat.h"
#include "hu_lib.h"
#include "hu_stuff.h"
#include "i_system.h"
#include "m_swap.h"
#include "p_local.h"
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
extern unsigned short **texturecolumnofs;
extern byte **texturecomposite;
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
static int frame_number;		/* the first core's count of views; a view has its own in what it is told */

static GLfixed quad_xyz[12], quad_uv[8], fan_xyz[MAX_CORNERS * 3], fan_uv[MAX_CORNERS * 2];

/* For the figures doom_video.c prints: the first core's instructions in the view, and (in [3])
 * the things drawn; what making the list cost whoever made it, and how many things it drew. */
unsigned doom_view[4];

/* Memory a worker core writes: it begins and ends on 16 bytes that are nobody else's. */
static void *
room(size_t bytes)
{
	void *memory = calloc(1, ((bytes + 15) & ~(size_t)15) + 16);

	if (memory)
		mcw_touch(memory, bytes);
	return memory;
}

/*
 * What a view is made from (docs/games/doom.md, "The view on worker cores"): the first core writes
 * it between two tics, a worker core makes the GPU's list from it while the next tic runs,
 * and reads nothing of the game that a tic writes.
 */
struct told_thing { const void *id; fixed_t x, y, z; angle_t angle; unsigned picture; short sector, shadow; int unused; };
struct told_sector { fixed_t floor, ceiling; short s, light, floorpic, ceilingpic; };
struct told_wall { int own, texture; fixed_t offset; int unused; };
#define TOLD_LINKS 1024		/* as doom_fast.c has them */
#define TOLD_GONE 256
static struct {
	fixed_t x, y, z, sin, cos;
	angle_t angle;
	int extralight, white, window_x, window_y, wide, high, frame;
	int sector, light, unseen;		/* the viewer's sector, its light, and whether the weapon is a shadow */
	const void *self;
	struct { const state_t *state; fixed_t sx, sy; } weapon[NUMPSPRITES];
	int all;				/* every thing of the level, sector by sector, in the lists' order */
	int things, links, goings, sectors, walls, anims;
	struct told_thing *thing;
	struct told_sector *sector_now;
	struct told_wall *wall;
	struct { int anim, now; } anim[32];
	const void *linked[TOLD_LINKS], *gone[TOLD_GONE];
} told;
static int thing_room;

/*
 * The tables of Doom's own renderer cost 2.5M instructions whenever the view's size is set
 * (R_ExecuteSetViewSize calls this). While the GPU draws the kept level nothing reads them:
 * they are made when that renderer, or its walk of the tree, is first wanted.
 */
void R_InitTextureMapping_port(void);
void R_ExecuteSetViewSize(void);
static int tables_wanted, tables_stale;

void
R_InitTextureMapping(void)
{
	tables_stale = doom_gl && !tables_wanted;
	if (!tables_stale)
		R_InitTextureMapping_port();
}

static void
soft_tables(void)
{
	tables_wanted = 1;
	if (tables_stale)
		R_ExecuteSetViewSize();
}

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
/* A worker core makes no picture (that reads the game's data, which the first core's tic may
 * be moving): it asks, shows the old one meanwhile, and the first core makes it before the
 * next view. */
enum { ASK_WALL, ASK_FLAT, ASK_SPRITE, ASK_SKY };
#define ASKS 64
static int asked[ASKS], asked_count;
#define on_worker mcw_on_worker()	/* (a share's call may run on any core) */
static int *wall_now, *flat_now;	/* texturetranslation and flattranslation as the view was told */

static GLuint
ask(int kind, int number)
{
	if (asked_count < ASKS)
		asked[asked_count++] = kind << 24 | number;
	return 0;
}

/* Wall `number`'s columns into its picture; `columns` has them found already (R_GetColumn), or is NULL. */
static void
wall_copy(byte *picture, int number, byte *const *columns)
{
	const int width = texturewidthmask[number] + 1, height = textureheight[number] >> FRACBITS;
	int x;

	for (x = 0; x < width && texturecolumnlump[number][x] <= 0; x++)
		;
	if (x < width)	/* (a picture of whole columns has no holes to clear first) */
		memset(picture, KEY, width * height);
	for (x = 0; x < width; x++) {
		byte *column = columns ? columns[x] : R_GetColumn(number, x), *to = picture + x * height;

		if (texturecolumnlump[number][x] > 0)
			column_posts(to, height, (column_t *)(column - 3));	/* straight from one patch: it may leave holes */
		else
			memcpy(to, column, height);
	}
}

/* (`number` is the picture after animation: wall_now's) */
static GLuint
wall_texture(int number)
{
	int width, height, x;
	byte *picture;
	GLuint name;

	if (wall_gl[number])
		return wall_gl[number];
	if (on_worker)
		return ask(ASK_WALL, number);
	width = texturewidthmask[number] + 1;
	height = textureheight[number] >> FRACBITS;
	picture = seglMemory(width * height);
	glGenTextures(1, &name);
	if (!picture || !name) {
		wall_gl[number] = name;
		return 0;
	}
	wall_copy(picture, number, NULL);
	glBindTexture(GL_TEXTURE_2D, name);
	seglTexturePointer(picture, height, width, GL_COLOR_INDEX8_EXT);
	return wall_gl[number] = name;	/* (named last: a worker core may be looking) */
}

/* The sky's picture, the right way up: it is laid on the screen, not on a wall. */
static GLuint
sky_texture(void)
{
	int number, width, height, x, row;
	byte *picture, *columns[512];
	GLuint name;

	if (sky_gl)
		return sky_gl;
	if (on_worker)
		return ask(ASK_SKY, 0);
	number = texturetranslation[skytexture];
	width = texturewidthmask[number] + 1;
	height = textureheight[number] >> FRACBITS;
	picture = seglMemory(width * height);
	glGenTextures(1, &name);
	if (!picture || !name) {
		sky_gl = name;
		return 0;
	}
	/* a row at a time (stores a row apart would fill the write cache every few dozen) */
	for (x = 0; x < width && x < 512; x++)
		columns[x] = R_GetColumn(number, x);
	for (row = 0; row < height; row++)
		for (x = 0; x < width && x < 512; x++)
			picture[row * width + x] = columns[x][row];
	glBindTexture(GL_TEXTURE_2D, name);
	seglTexturePointer(picture, width, height, GL_COLOR_INDEX8_EXT);
	return sky_gl = name;
}

/* (`number` is the picture after animation: flat_now's) */
static GLuint
flat_texture(int number)
{
	byte *picture;
	GLuint name;

	if (number < 0 || number >= flat_count)
		return 0;
	if (flat_gl[number])
		return flat_gl[number];
	if (on_worker)
		return ask(ASK_FLAT, number);
	picture = seglMemory(64 * 64);
	glGenTextures(1, &name);
	if (!picture || !name) {
		flat_gl[number] = name;
		return 0;
	}
	memcpy(picture, W_CacheLumpNum(firstflat + number, PU_CACHE), 64 * 64);
	glBindTexture(GL_TEXTURE_2D, name);
	seglTexturePointer(picture, 64, 64, GL_COLOR_INDEX8_EXT);
	return flat_gl[number] = name;
}

static GLuint
sprite_texture(int lump)
{
	patch_t *patch;
	int width, height, x;
	byte *picture;
	GLuint name;

	if (sprite_gl[lump])
		return sprite_gl[lump];
	if (on_worker)
		return ask(ASK_SPRITE, lump);
	patch = W_CacheLumpNum(firstspritelump + lump, PU_CACHE);
	width = SHORT(patch->width);
	height = SHORT(patch->height);
	picture = seglMemory(width * height);
	glGenTextures(1, &name);
	if (!picture || !name) {
		sprite_gl[lump] = name;
		return 0;
	}
	/* on its side, as a wall's picture is: stores down a column of an upright one are a row
	 * apart, which fills the machine's write cache after a few dozen */
	memset(picture, KEY, width * height);
	for (x = 0; x < width; x++)
		column_posts(picture + x * height, height, (column_t *)((byte *)patch + LONG(patch->columnofs[x])));
	sprite_height[lump] = height;
	glBindTexture(GL_TEXTURE_2D, name);
	seglTexturePointer(picture, height, width, GL_COLOR_INDEX8_EXT);
	return sprite_gl[lump] = name;
}

static void things_asked(void);

/* The pictures a worker core asked for, made by the first while that worker has no view. */
static void
asked_make(void)
{
	int i;

	for (i = 0; i < asked_count; i++) {
		const int number = asked[i] & 0xffffff;

		switch (asked[i] >> 24) {
		case ASK_WALL: wall_texture(number); break;
		case ASK_FLAT: flat_texture(number); break;
		case ASK_SPRITE: sprite_texture(number); break;
		default: sky_texture(); break;
		}
	}
	asked_count = 0;
	things_asked();
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

/* (n << 16) / (n + m) for n, m > 0 (under 2^47), as C's division has it: the quotient is under 65,536,
 * guessed from a float division of the top 31 bits and made exact by the remainder (the library's
 * 64-bit division took 200 instructions a call, 8% of a level's load; a bit at a time, 150). */
static long long
fraction(unsigned long long n, unsigned long long m)
{
	const unsigned long long d = n + m, whole = n << 16;
	const unsigned high = (unsigned)(d >> 32);
	const int shift = high ? 33 - __builtin_clz(high) : (unsigned)d >> 31;
	unsigned q = (unsigned)((float)(int)(unsigned)(n >> shift) * 65536.0f / (float)(int)(unsigned)(d >> shift));
	unsigned long long product;

	if (q > 65535)
		q = 65535;
	product = (unsigned long long)q * d;
	while (product > whole) {
		q--;
		product -= d;
	}
	while (whole - product >= d) {
		q++;
		product += d;
	}
	return q;
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
			t = fraction(di < 0 ? -di : di, di < 0 ? dj : -dj);	/* (opposite signs) */
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

/* Whole-number square root, for seg lengths: all but the last eight bits are the root of the
 * number without its last sixteen, which fits a word for any seg under 16,384 units. */
static unsigned
root(unsigned long long v)
{
	unsigned top = (unsigned)(v >> 16), r = 0, bit = 1u << 30;

	if (v >> 48) {
		r = root(v >> 2) << 1;	/* (longer: twice the root of a quarter, or one more) */
		return r + ((unsigned long long)(r + 1) * (r + 1) <= v);
	}
	while (bit > top)
		bit >>= 2;
	while (bit) {
		if (top >= r + bit) {
			top -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	r <<= 8;
	for (bit = 128; bit; bit >>= 1)
		if ((unsigned long long)(r | bit) * (r | bit) <= v)
			r |= bit;
	return r;
}

/* ---- the level, kept in GPU memory ---- */

/*
 * Walls and floors do not move, but for doors and lifts: the whole level is written once, as
 * quads in GPU memory and a command for each run of them with one texture and one light, and
 * the depth buffer decides what is seen. A frame then sends what changed (docs/games/doom.md).
 */
enum { WALL_MID, WALL_TOP, WALL_BOTTOM, WALL_HOLES, WALL_SKY, FLOOR, CEILING, SKY };

struct piece { int of, sector, texture, unique, quad, quads; fixed_t offset; short shade; unsigned char kind; };
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
static struct told_sector *now_;	/* per sector: what the game has, as the view was last told */
static int piece_count, group_count, world_quads;
static uint32_t *world_vertices;
static GLuint world_name;		/* a texture's name, so the vertices' memory is given back with the level's */
static int *sector_group;		/* per sector, its first group; one more at the end */
static int *depend_first, *depend;	/* per sector, the walls that stand on its heights */
static struct own *own;		/* walls of lines that do something: their picture may change */
static int own_count;
static int *moving, moving_count;	/* groups whose picture is one of an animation's */
static int light_seen[2] = { -1, -1 };	/* per set of commands, as the lights in struct seen and group */
static int *sliding, sliding_count;	/* into own: the walls of lines that scroll */
static short **seen_from;		/* per sector: the sectors it can see into (sectors_seen) */
static short *seen_lists;		/* the room those lists are in, */
static int seen_room, seen_used;	/* in shorts */
static fixed_t eye_low, eye_high;	/* with the eye from one to the other no floor or ceiling turns its back */
/* The floors, and the ceilings, in order of height, with how many of each are under the eye;
 * a sector that has moved since is `loose`, and looked at by itself (sector_faces). */
struct level { fixed_t height; int sector; };
static struct level *floors_up, *ceilings_up;
static int floor_count, ceiling_count, floors_under, ceilings_under;
static unsigned char *loose;
static int *loose_list, loose_count;
static int again;			/* a picture was asked for: every run's is looked at in the next view */

/*
 * What the game has changed since the level was written: it says so where it moves a floor
 * or ceiling, changes a sector's light or picture, or a wall's picture (doom_fast.c), and a
 * view is told. These are the sectors each set of commands has still to be brought up to.
 */
static unsigned char *marked;		/* per sector: bit n says it is in pending[n] */
static int *pending[2], pending_count[2];	/* per set of commands: sectors to look at again */
static struct { int texture; fixed_t offset; } *own_told;	/* per wall of own: as the view was last told */
static int anim_told[32];		/* per animation (p_spec.c has room for 32): its first picture as last told */
extern int doom_walls_changed, doom_level_loads;
static int level_made;

#define THING_PLACES 512		/* a power of two */
/* A place's quad is 64 bytes; they are 80 apart, so that one field of many of them (a frame
 * turns every thing in view) does not land in the same few sets of the machine's write cache. */
#define PLACE_BYTES 80
struct place {
	const void *of;
	const spriteframe_t *pictures;	/* of the thing's sprite and frame, which `key` is */
	unsigned key;
	fixed_t x, y, high, top, tall;
	angle_t angle;			/* the view's, when the quad was turned to face it */
	int lump, frame, grey, wide, left;	/* (wide and left in whole pixels) */
	/* which of eight pictures shows (turn) is found again when the thing faces another way,
	 * or has moved against the viewer by more than `still` since it was found */
	angle_t facing;
	fixed_t rx, ry, still;
	short index;
	unsigned char flip, turn;
} __attribute__((aligned(16)));		/* (whole 16s: two cores write places side by side) */
static struct place places[THING_PLACES];

/*
 * The things as the view was told of them, in lists by sector that are in the order of the
 * game's own (a thing put into a sector's list goes to its head). A thing is known by where
 * the game keeps it.
 */
#define THINGS 4096			/* a power of two */
struct thing { const void *id; fixed_t x, y, z; angle_t angle; unsigned picture; short sector, next, prev; unsigned char shadow; };

/*
 * A share of the things. With more worker cores than one each has the things whose places end
 * in its number (a share's places are every `crew`th), and its own lists and notes of them;
 * the first share's core makes the rest of the view too, and has the things that are blended.
 */
#define CREW 4
static struct share {
	struct thing *things;
	short *chain, *hash;		/* things with one hash, and the free ones, are chains */
	short *first;			/* per sector */
	int free;
	short lately[2][THING_PLACES];	/* the places drawn from in this frame, and in the last */
	int lately_count[2], lately_now;
	int asked[ASKS], asked_count;	/* sprites it had no picture of */
	unsigned drawn, cost;		/* things, and instructions, since the first core last took them */
	short once[4096];		/* the sectors in sight, when nobody has listed them */
} __attribute__((aligned(16))) shares[CREW];
static int crew = 1;			/* how many shares there are: 1, 2 or 4 */
static int direct = 1;			/* no worker core: a view reads the game's own things, between two tics */

/* Where a thing's place is looked for first, among the places. */
static inline unsigned
place_at(const void *id)
{
	return ((unsigned)(size_t)id >> 4) * 2654435761u >> 23;
}

static inline int
share_of(const void *id, int shadow)
{
	return shadow ? 0 : place_at(id) & (crew - 1);
}

static inline unsigned
thing_at(const void *id)
{
	return ((unsigned)(size_t)id >> 4) * 2654435761u >> 20;
}

static void
things_clear(struct share *me)
{
	int i;

	for (i = 0; i < THINGS; i++) {
		me->chain[i] = i + 1 < THINGS ? i + 1 : -1;
		me->hash[i] = -1;
		me->things[i].id = NULL;
	}
	me->free = 0;
	for (i = 0; i < numsectors; i++)
		me->first[i] = -1;
}

static int
thing_find(const struct share *me, const void *id)
{
	int i;

	for (i = me->hash[thing_at(id)]; i >= 0; i = me->chain[i])
		if (me->things[i].id == id)
			break;
	return i;
}

static int
thing_new(struct share *me, const void *id)
{
	const unsigned at = thing_at(id);
	const int i = me->free;

	if (i < 0)
		return -1;
	me->free = me->chain[i];
	me->chain[i] = me->hash[at];
	me->hash[at] = i;
	me->things[i].id = id;
	me->things[i].sector = -1;
	return i;
}

/* Out of its sector's list, and into one at its head. */
static void
thing_out(struct share *me, int i)
{
	struct thing *thing = &me->things[i];

	if (thing->sector < 0)
		return;
	if (thing->prev >= 0)
		me->things[thing->prev].next = thing->next;
	else
		me->first[thing->sector] = thing->next;
	if (thing->next >= 0)
		me->things[thing->next].prev = thing->prev;
	thing->sector = -1;
}

static void
thing_in(struct share *me, int i, int sector)
{
	struct thing *thing = &me->things[i];

	thing->sector = sector;
	thing->prev = -1;
	thing->next = me->first[sector];
	if (thing->next >= 0)
		me->things[thing->next].prev = i;
	me->first[sector] = i;
}

static void
thing_gone(struct share *me, const void *id)
{
	const unsigned at = thing_at(id);
	int i = thing_find(me, id), k;

	if (i < 0)
		return;
	thing_out(me, i);
	if (me->hash[at] == i)
		me->hash[at] = me->chain[i];
	else
		for (k = me->hash[at]; k >= 0; k = me->chain[k])
			if (me->chain[k] == i) {
				me->chain[k] = me->chain[i];
				break;
			}
	me->things[i].id = NULL;
	me->chain[i] = me->free;
	me->free = i;
}

static inline void
thing_is(struct thing *thing, const struct told_thing *is)
{
	thing->x = is->x;
	thing->y = is->y;
	thing->z = is->z;
	thing->angle = is->angle;
	thing->picture = is->picture;
	thing->shadow = is->shadow;
}

/* What the view was told of a share's things goes into its lists. Done twice it leaves them
 * the same. (The first share asks after every thing gone or put in: a blended one is its own.) */
static void
things_told(struct share *me)
{
	const int mine = me - shares, others = crew - 1;
	int i, k;

	if (told.all) {
		things_clear(me);
		/* backwards, each to its list's head: the lists are in the order told */
		for (i = told.things - 1; i >= 0; i--)
			if (share_of(told.thing[i].id, told.thing[i].shadow) == mine && (k = thing_new(me, told.thing[i].id)) >= 0) {
				thing_is(&me->things[k], &told.thing[i]);
				thing_in(me, k, told.thing[i].sector);
			}
		return;
	}
	for (i = 0; i < told.goings; i++)
		if (!mine || (place_at(told.gone[i]) & others) == mine)
			thing_gone(me, told.gone[i]);
	for (i = 0; i < told.things; i++) {
		const struct told_thing *is = &told.thing[i];

		if (share_of(is->id, is->shadow) != mine || ((k = thing_find(me, is->id)) < 0 && (k = thing_new(me, is->id)) < 0))
			continue;
		thing_is(&me->things[k], is);
		if (me->things[k].sector != is->sector && me->things[k].sector != -is->sector - 2) {
			thing_out(me, k);
			me->things[k].sector = -is->sector - 2;	/* (below -1: the sector it is to be put into) */
		}
	}
	/* the game put these into their sectors' lists, in this order */
	for (i = 0; i < told.links; i++)
		if ((!mine || (place_at(told.linked[i]) & others) == mine) && (k = thing_find(me, told.linked[i])) >= 0 &&
		    me->things[k].sector != -1) {
			const int sector = me->things[k].sector < 0 ? -me->things[k].sector - 2 : me->things[k].sector;

			thing_out(me, k);
			thing_in(me, k, sector);
		}
}

/* Both sets of commands have the sectors the view was told of to look at. */
static void
sectors_told(void)
{
	int i, n;

	for (i = 0; i < told.sectors; i++) {
		const int s = told.sector_now[i].s;

		for (n = 0; n < 2; n++)
			if (!(marked[s] & (1 << n)))
				pending[n][pending_count[n]++] = s;
		marked[s] = 3;
	}
}

static int
grey_of(int level, int shade)
{
	if (told.white)
		return 255;
	/* Doom has sixteen light levels, and so a sector that glows changes half as often. A kept
	 * level does not take the gun's flash this way (flash() below). */
	level = (level >= 248 ? 255 : (level & ~15) + 8) + shade + (world_on ? 0 : told.extralight << 4);
	return level < 0 ? 0 : level > 255 ? 255 : level;
}

static inline uint32_t
uv_word(fixed_t u, fixed_t v)
{
	return ((uint32_t)(u >> 6) & 0xffff) | (uint32_t)(v >> 6) << 16;
}

/* A wall's quad, from the heights and pictures the view was told; one of no size if it is not there. */
static void
wall_piece(const struct piece *p)
{
	seg_t *seg = &segs[p->of];
	side_t *side = seg->sidedef;
	line_t *line = seg->linedef;
	const struct told_sector *front = &now_[seg->frontsector - sectors], *back = seg->backsector ? &now_[seg->backsector - sectors] : NULL;
	uint32_t *to = world_vertices + 16 * p->quad;
	fixed_t high, low, top, tall, u1, u2, v1, v2;
	int texture = p->texture, width, height, k;

	switch (p->kind) {
	case WALL_MID:
		high = front->ceiling;
		low = front->floor;
		top = (line->flags & ML_DONTPEGBOTTOM ? low + textureheight[wall_now[texture]] : high) + side->rowoffset;
		break;
	case WALL_TOP:
		high = front->ceiling;
		low = back->ceiling;
		top = (line->flags & ML_DONTPEGTOP ? high : low + textureheight[wall_now[texture]]) + side->rowoffset;
		break;
	case WALL_BOTTOM:
		high = back->floor;
		low = front->floor;
		top = (line->flags & ML_DONTPEGBOTTOM ? front->ceiling : high) + side->rowoffset;
		break;
	case WALL_SKY:
		/* between two ceilings that are both sky: sky too, and it hides what is behind it */
		texture = skytexture;
		high = front->ceiling;
		low = top = back->ceiling;
		break;
	default:
		/* a picture with holes across an opening: one copy of it, not repeated up and down */
		tall = textureheight[wall_now[texture]];
		high = front->ceiling < back->ceiling ? front->ceiling : back->ceiling;
		low = front->floor > back->floor ? front->floor : back->floor;
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
	width = texturewidthmask[wall_now[texture]] + 1;
	height = textureheight[wall_now[texture]] >> FRACBITS;
	u1 = (p->offset + seg->offset) / width;
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
	p->offset = kind < FLOOR ? segs[of].sidedef->textureoffset : 0;
}

/* Into piece_order: by sector first, which a count does, then the few of each sector among themselves,
 * by numbers (a piece is 32 bytes: moving pieces was 4% of a level's load) and a halving search for
 * where each goes, after the last that is not after it (the order a stable sort gives). */
static void
sort_pieces(void)
{
	struct piece *sorted = room((piece_count + 1) * sizeof *sorted);
	int *start = calloc(numsectors + 3, sizeof *start), *at = malloc((piece_count + 1) * sizeof *at), i, s;

	for (i = 0; i < piece_count; i++)
		start[pieces[i].sector + 2]++;
	for (s = 0; s <= numsectors; s++)
		start[s + 2] += start[s + 1];
	for (i = 0; i < piece_count; i++)
		at[start[pieces[i].sector + 1]++] = i;
	for (s = 0; s <= numsectors; s++)
		for (i = start[s] + 1; i < start[s + 1]; i++) {
			const int moved = at[i];
			int low = start[s], high = i;

			while (low < high) {
				const int middle = (low + high) >> 1;

				if (piece_order(&pieces[at[middle]], &pieces[moved]) > 0)
					high = middle;
				else
					low = middle + 1;
			}
			memmove(at + low + 1, at + low, (i - low) * sizeof *at);
			at[low] = moved;
		}
	for (i = 0; i < piece_count; i++)
		sorted[i] = pieces[at[i]];
	free(at);
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
		*shown = flat_now[g->texture];
		return flat_texture(*shown);
	}
	*shown = wall_now[g->texture];
	return wall_texture(*shown);
}

/* A run takes the picture it has now. A worker core that finds none made asks, and the run
 * keeps the one it had until the next view. */
static void
group_repaint(int g)
{
	int shown;
	const GLuint name = group_picture(&groups[g], &shown);

	if (!name && on_worker) {
		again = 1;
		return;
	}
	groups[g].shown = shown;
	seglKeptTexture(g, name);
}

static void
world_free(void)
{
	int s;

	free(pieces);
	free(groups);
	free(seen);
	free(now_);
	free(sector_group);
	free(depend_first);
	free(depend);
	free(own);
	free(own_told);
	free(moving);
	free(sliding);
	free(marked);
	free(pending[0]);
	free(pending[1]);
	free(floors_up);
	free(ceilings_up);
	free(loose);
	free(loose_list);
	free(seen_from);
	free(seen_lists);
	for (s = 0; s < CREW; s++) {
		free(shares[s].first);
		shares[s].first = NULL;
	}
	free(told.sector_now);
	free(told.wall);
	floors_up = ceilings_up = NULL;
	loose = NULL;
	loose_list = NULL;
	seen_from = NULL;
	seen_lists = NULL;
	marked = NULL;
	pending[0] = pending[1] = sliding = NULL;
	pieces = NULL;
	groups = NULL;
	seen = NULL;
	now_ = told.sector_now = NULL;
	told.wall = NULL;
	sector_group = depend_first = depend = moving = NULL;
	own = NULL;
	own_told = NULL;
	if (world_name)
		glDeleteTextures(1, &world_name);
	world_name = 0;
	world_on = 0;
	seglKeep(0, 0);
}

/*
 * A level's wall and floor pictures before its groups ask for them one by one, which was a third of
 * the first core's load: this core finds each picture's columns (R_GetColumn may make a composite
 * in the zone, and a lump is read from the file) and holds their memory in the zone, and the workers
 * copy them into GPU memory, a picture each (seglMemory's are 16 bytes apart). In batches, so that
 * the zone never holds more than BATCH pictures' lumps at once.
 */
#define BATCH 64
struct making { byte *picture; int number, flat; GLuint name; byte **columns; };

static void
pictures_make(void *ctx, uint32_t from, uint32_t to)
{
	struct making *m = ctx;

	for (; from < to; from++)
		if (m[from].flat)
			memcpy(m[from].picture, m[from].columns[0], 64 * 64);
		else
			wall_copy(m[from].picture, m[from].number, m[from].columns);
}

/* A wall picture's lumps (but its composite) held in the zone (PU_STATIC) or let go (PU_CACHE); with
 * `columns`, where its columns lie. */
static void
wall_lumps(int number, int tag, byte **columns)
{
	byte *at = NULL;
	int x, lump, last = 0;

	for (x = 0; x <= texturewidthmask[number]; x++) {
		lump = texturecolumnlump[number][x];
		if (lump > 0 && lump != last)
			at = W_CacheLumpNum(lump, tag);
		if (lump > 0)
			last = lump;
		if (columns)
			columns[x] = (lump > 0 ? at : texturecomposite[number]) + texturecolumnofs[number][x];
	}
}

static void
pictures_ahead(void)
{
	static const mcw_for_opts each = { 1, 0, NULL, 0, 0, 0 };	/* a picture a piece */
	struct making *m = malloc(BATCH * sizeof *m);
	byte *wanted = calloc(numtextures + flat_count + 1, 1), **columns = NULL;
	int g, k, n = 0, x, used, number, flat, bytes;

	if (!m || !wanted) {
		free(m);
		free(wanted);
		return;
	}
	mcw_touch_code((const void *)pictures_make, 64);
	mcw_touch_code((const void *)wall_copy, 256);
	mcw_touch_code((const void *)column_posts, 64);
	for (g = 0; g <= group_count; g++) {
		if (g < group_count) {
			const struct group *group = &groups[g];

			if (group->kind == WALL_SKY || group->kind == SKY)
				continue;
			flat = group->kind >= FLOOR;
			number = flat ? flat_now[group->texture] : wall_now[group->texture];
			if (flat ? number < 0 || number >= flat_count || flat_gl[number] : wall_gl[number])
				continue;
			if (wanted[flat ? numtextures + number : number])
				continue;
			wanted[flat ? numtextures + number : number] = 1;
			bytes = flat ? 64 * 64 : (texturewidthmask[number] + 1) * (textureheight[number] >> FRACBITS);
			m[n].number = number;
			m[n].flat = flat;
			m[n].picture = seglMemory(bytes);
			glGenTextures(1, &m[n].name);
			if (!m[n].picture || !m[n].name) {
				if (flat)
					flat_gl[number] = m[n].name;
				else
					wall_gl[number] = m[n].name;
				continue;
			}
			mcw_touch(m[n].picture, bytes);
			if (++n < BATCH)
				continue;
		}
		if (!n)
			continue;
		/* the composites first (making one reads patches, which leaves them PU_CACHE), held; then
		 * the lumps, held, and nothing else asked of the zone until they are let go */
		for (k = used = 0; k < n; k++) {
			number = m[k].number;
			used += m[k].flat ? 1 : texturewidthmask[number] + 1;
			if (m[k].flat)
				continue;
			for (x = 0; x <= texturewidthmask[number] && texturecolumnlump[number][x] > 0; x++)
				;
			if (x <= texturewidthmask[number]) {
				R_GetColumn(number, x);
				Z_ChangeTag(texturecomposite[number], PU_STATIC);
			}
		}
		if (!(columns = realloc(columns, (used + 1) * sizeof *columns)))
			break;	/* (what is not made is made as it is asked for) */
		for (k = used = 0; k < n; k++) {
			number = m[k].number;
			m[k].columns = columns + used;
			if (m[k].flat) {
				columns[used++] = W_CacheLumpNum(firstflat + number, PU_STATIC);
			} else {
				wall_lumps(number, PU_STATIC, columns + used);
				used += texturewidthmask[number] + 1;
			}
		}
		mcw_for_opt(n, pictures_make, m, &each);
		for (k = 0; k < n; k++) {
			number = m[k].number;
			glBindTexture(GL_TEXTURE_2D, m[k].name);
			if (m[k].flat) {
				seglTexturePointer(m[k].picture, 64, 64, GL_COLOR_INDEX8_EXT);
				flat_gl[number] = m[k].name;
				W_CacheLumpNum(firstflat + number, PU_CACHE);
				continue;
			}
			seglTexturePointer(m[k].picture, textureheight[number] >> FRACBITS, texturewidthmask[number] + 1, GL_COLOR_INDEX8_EXT);
			wall_gl[number] = m[k].name;
			wall_lumps(number, PU_CACHE, NULL);
			if (texturecomposite[number])
				Z_ChangeTag(texturecomposite[number], PU_CACHE);
		}
		n = 0;
	}
	free(columns);
	free(wanted);
	free(m);
}

/* Into order of height (a Shell sort: a level has hundreds of sectors and few heights). */
static void
by_height(struct level *level, int count)
{
	int gap, i, k;

	for (gap = count / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11)
		for (i = gap; i < count; i++) {
			const struct level moved = level[i];

			for (k = i; k >= gap && level[k - gap].height > moved.height; k -= gap)
				level[k] = level[k - gap];
			level[k] = moved;
		}
}

void doom_told_level(void);

/* Makes the level's pieces, sorts them into groups, and writes all of it. */
static void
world_build(void)
{
	int i, s, count = 0, g;
	anim_def *anim;

	for (i = 0; i < numsegs; i++)
		count += segs[i].backsector ? 3 : 1;
	count += 2 * numsubsectors;
	pieces = room((count + 1) * sizeof *pieces);
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
	groups = room((piece_count + 1) * sizeof *groups);
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
	for (i = 0; i < THING_PLACES; i++) {
		uint8_t *quad = (uint8_t *)world_vertices + world_quads * 64 + i * PLACE_BYTES;

		places[i].index = i;
		seglKeptQuads(group_count + i, quad - (world_quads + i) * 64, world_quads + i, 1, 0, 255, 1);
		seglKeptShown(group_count + i, 0);
	}
	seen_from = room((numsectors + 1) * sizeof *seen_from);
	/* (a list for every sector, were each to see every other; more than a megabyte is not kept) */
	seen_room = numsectors * (numsectors + 1) < (1 << 19) ? numsectors * (numsectors + 1) : 1 << 19;
	seen_lists = room(seen_room * sizeof *seen_lists);
	seen_used = 0;

	seen = room(numsectors * sizeof *seen);
	now_ = room((numsectors + 1) * sizeof *now_);
	told.sector_now = room((numsectors + 1) * sizeof *told.sector_now);
	sector_group = room((numsectors + 2) * sizeof *sector_group);
	depend_first = room((numsectors + 2) * sizeof *depend_first);
	depend = room((2 * piece_count + 1) * sizeof *depend);
	own = room((piece_count + 1) * sizeof *own);
	moving = room((group_count + 1) * sizeof *moving);
	sliding = room((piece_count + 1) * sizeof *sliding);
	pending[0] = room((numsectors + 1) * sizeof *pending[0]);
	pending[1] = room((numsectors + 1) * sizeof *pending[1]);
	marked = room(numsectors + 1);
	for (i = 0; i < CREW; i++) {
		struct share *share = &shares[i];

		if (!share->things) {
			share->things = room(THINGS * sizeof *share->things);
			share->chain = room(THINGS * sizeof *share->chain);
			share->hash = room(THINGS * sizeof *share->hash);
		}
		share->first = room((numsectors + 1) * sizeof *share->first);
		share->lately_count[0] = share->lately_count[1] = share->asked_count = 0;
		things_clear(share);
	}
	doom_told_level();
	own_count = moving_count = sliding_count = pending_count[0] = pending_count[1] = 0;
	again = 0;
	eye_low = 1;	/* no eye is between these: the first frame looks at every sector */
	eye_high = 0;
	for (s = 0; s < numsectors; s++) {
		now_[s].s = s;
		seen[s].floor = now_[s].floor = sectors[s].floorheight;
		seen[s].ceiling = now_[s].ceiling = sectors[s].ceilingheight;
		now_[s].light = sectors[s].lightlevel;
		seen[s].light[0] = seen[s].light[1] = sectors[s].lightlevel >> 4;
		seen[s].floorpic = now_[s].floorpic = sectors[s].floorpic;
		seen[s].ceilingpic = now_[s].ceilingpic = sectors[s].ceilingpic;
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
				if (seg->linedef->special == 48)
					sliding[sliding_count++] = own_count - 1;
			}
			wall_piece(p);
		} else {
			plane_piece(p, p->kind == FLOOR ? subsectors[p->of].sector->floorheight : subsectors[p->of].sector->ceilingheight);
		}
	}
	own_told = malloc((own_count + 1) * sizeof *own_told);
	told.wall = room((own_count + 1) * sizeof *told.wall);
	for (i = 0; i < own_count; i++) {
		own_told[i].texture = own[i].texture;
		own_told[i].offset = own[i].offset;
	}
	light_seen[0] = light_seen[1] = told.white;
	pictures_ahead();
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
	floors_up = room((numsectors + 1) * sizeof *floors_up);
	ceilings_up = room((numsectors + 1) * sizeof *ceilings_up);
	loose = room(numsectors + 1);
	loose_list = room((numsectors + 1) * sizeof *loose_list);
	floor_count = ceiling_count = loose_count = 0;
	for (i = 0; i < numsectors; i++) {
		if (seen[i].floor_group >= 0) {
			floors_up[floor_count].height = seen[i].floor;
			floors_up[floor_count++].sector = i;
		}
		if (seen[i].ceiling_group >= 0) {
			ceilings_up[ceiling_count].height = seen[i].ceiling;
			ceilings_up[ceiling_count++].sector = i;
		}
	}
	by_height(floors_up, floor_count);
	by_height(ceilings_up, ceiling_count);
	floors_under = floor_count;	/* as the runs are now: every floor and ceiling shown */
	ceilings_under = 0;
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
	const struct told_sector *sector = &now_[s];
	int i, g;

	for (i = depend_first[s]; i < depend_first[s + 1]; i++)
		wall_piece(&pieces[depend[i]]);
	for (g = sector_group[s]; g < sector_group[s + 1]; g++)
		if (groups[g].kind >= FLOOR) {
			uint32_t *to = world_vertices + 16 * groups[g].first + 2;
			fixed_t height = groups[g].kind == FLOOR ? sector->floor : sector->ceiling;

			for (i = 4 * groups[g].quads; i > 0; i--, to += 4)
				*to = height;
		}
	seen[s].floor = sector->floor;
	seen[s].ceiling = sector->ceiling;
	if (!loose[s]) {
		loose[s] = 1;
		loose_list[loose_count++] = s;
	}
}

/* A sector's light has changed: the grey of its runs, as grey_of() has it for a kept level. */
static void
sector_relit(int s, int set)
{
	const int level = now_[s].light, end = sector_group[s + 1], white = told.white;
	const int base = level >= 248 ? 255 : (level & ~15) + 8;
	struct group *group = &groups[sector_group[s]];
	int g;

	seen[s].light[set] = level >> 4;
	for (g = sector_group[s]; g < end; g++, group++) {
		int grey = base + group->shade;

		grey = white || grey > 255 ? 255 : grey < 0 ? 0 : grey;
		if (grey != group->grey[set])
			seglKeptGreyIn(set, g, group->grey[set] = grey);
	}
}

static void
sector_repainted(int s)
{
	int g;

	seen[s].floorpic = now_[s].floorpic;
	seen[s].ceilingpic = now_[s].ceilingpic;
	for (g = sector_group[s]; g < sector_group[s + 1]; g++)
		if (groups[g].kind >= FLOOR) {
			groups[g].texture = groups[g].kind == FLOOR ? now_[s].floorpic : now_[s].ceilingpic;
			group_repaint(g);
		}
}

/*
 * A floor is seen from above and a ceiling from below, as Doom draws them: never their backs.
 * The heights between which the eye changes none of that are narrowed to this sector's.
 */
static void
sector_faces(struct seen *was, fixed_t eye)
{
	int hidden;

	if (was->floor_group >= 0) {
		if ((hidden = was->floor >= eye) != was->floor_hidden)
			seglKeptShown(was->floor_group, (was->floor_hidden = hidden) ? 0 : groups[was->floor_group].quads);
		if (hidden && was->floor < eye_high)
			eye_high = was->floor;
		else if (!hidden && was->floor >= eye_low)
			eye_low = was->floor + 1;
	}
	if (was->ceiling_group >= 0) {
		if ((hidden = was->ceiling <= eye) != was->ceiling_hidden)
			seglKeptShown(was->ceiling_group, (was->ceiling_hidden = hidden) ? 0 : groups[was->ceiling_group].quads);
		if (hidden && was->ceiling > eye_low)
			eye_low = was->ceiling;
		else if (!hidden && was->ceiling <= eye_high)
			eye_high = was->ceiling - 1;
	}
}

/* A floor (or a ceiling) the eye has passed shows its face, or its back, which is nothing. */
static void
face_shown(int s, int ceiling, int shown)
{
	struct seen *was = &seen[s];
	unsigned char *hidden = ceiling ? &was->ceiling_hidden : &was->floor_hidden;
	int group = ceiling ? was->ceiling_group : was->floor_group;

	if (loose[s] || *hidden == !shown)
		return;
	*hidden = !shown;
	seglKeptShown(group, shown ? groups[group].quads : 0);
}

/* The eye is here now: what it has passed on its way, of the sectors that have not moved. */
static void
eye_passed(fixed_t eye)
{
	while (floors_under < floor_count && floors_up[floors_under].height < eye)
		face_shown(floors_up[floors_under++].sector, 0, 1);
	while (floors_under > 0 && floors_up[floors_under - 1].height >= eye)
		face_shown(floors_up[--floors_under].sector, 0, 0);
	while (ceilings_under < ceiling_count && ceilings_up[ceilings_under].height <= eye)
		face_shown(ceilings_up[ceilings_under++].sector, 1, 0);
	while (ceilings_under > 0 && ceilings_up[ceilings_under - 1].height > eye)
		face_shown(ceilings_up[--ceilings_under].sector, 1, 1);
}

/* A wall of a line that does something: a switch's picture, a wall that slides. */
static void
own_update(struct own *o, int texture, fixed_t offset)
{
	if (texture == o->texture && offset == o->offset)
		return;
	pieces[o->piece].offset = o->offset = offset;
	pieces[o->piece].texture = groups[o->group].texture = o->texture = texture;
	wall_piece(&pieces[o->piece]);
	group_repaint(o->group);
}

/* What has changed since the last frame goes to the GPU's copy: heights, lights, pictures. */
static void
world_update(void)
{
	/* a run's grey is one word of each of its commands, 64 bytes apart, which the write cache
	 * holds few of: only the set this frame is built in is brought up to date */
	int set = seglSet(), light = told.white, s, i, k, waiting = 0;
	const int from = told.sector * numsectors;
	const byte *reject = rejectmatrix;
	const fixed_t eye = told.z;

	if (light != light_seen[set])
		for (s = 0; s < numsectors; s++)
			sector_relit(s, set);
	light_seen[set] = light;
	/* the sectors the game has changed since this set was last built */
	for (i = 0; i < pending_count[set]; i++) {
		const struct told_sector *sector = &now_[s = pending[set][i]];
		struct seen *was = &seen[s];

		if (sector->floor != was->floor || sector->ceiling != was->ceiling)
			sector_moved(s);
		/* a light the viewer's sector cannot see waits, marked, until it can: most that glow do */
		if (sector->light >> 4 == was->light[set])
			marked[s] &= ~(1 << set);
		else if (reject[(from + s) >> 3] & (1 << ((from + s) & 7)))
			pending[set][waiting++] = s;
		else {
			marked[s] &= ~(1 << set);
			sector_relit(s, set);
		}
		if (sector->floorpic != was->floorpic || sector->ceilingpic != was->ceilingpic)
			sector_repainted(s);
		if (loose[s])
			sector_faces(was, eye);
	}
	pending_count[set] = waiting;
	if (eye < eye_low || eye > eye_high) {
		eye_low = MININT;
		eye_high = MAXINT;
		for (i = 0; i < loose_count; i++)
			sector_faces(&seen[loose_list[i]], eye);
	}
	eye_passed(eye);
	for (i = 0; i < told.walls; i++)
		own_update(&own[told.wall[i].own], told.wall[i].texture, told.wall[i].offset);
	/* an animation's pictures all move on together, as its first one has */
	for (i = 0; i < told.anims; i++) {
		const anim_def *anim = &anims[told.anim[i].anim];
		int *now = anim->istexture ? wall_now : flat_now;

		for (k = 0; k < anim->numpics; k++)
			now[anim->basepic + k] = anim->basepic + (told.anim[i].now - anim->basepic + k) % anim->numpics;
	}
	for (i = 0; told.anims && i < moving_count; i++) {
		struct group *group = &groups[moving[i]];
		int now = group->kind >= FLOOR ? flat_now[group->texture] : wall_now[group->texture];

		if (now != group->shown)
			group_repaint(moving[i]);
	}
	if (again) {
		/* a picture that was asked for: whichever run has not the one it should */
		again = 0;
		for (i = 0; i < sector_group[numsectors]; i++) {
			int now = groups[i].kind >= FLOOR ? flat_now[groups[i].texture] : wall_now[groups[i].texture];

			if (now != groups[i].shown)
				group_repaint(i);
		}
	}
}

/* The angle of (x, y) from the eye: Doom's R_PointToAngle, with nothing of the game's read. */
static angle_t
angle_to(fixed_t x, fixed_t y)
{
#define SLOPE(num, den) ((unsigned)(den) < 512 ? SLOPERANGE : ((unsigned)(num) << 3) / ((unsigned)(den) >> 8) <= SLOPERANGE ? \
			 ((unsigned)(num) << 3) / ((unsigned)(den) >> 8) : SLOPERANGE)
	if (!x && !y)
		return 0;
	if (x >= 0) {
		if (y >= 0)
			return x > y ? tantoangle[SLOPE(y, x)] : ANG90 - 1 - tantoangle[SLOPE(x, y)];
		y = -y;
		return x > y ? -tantoangle[SLOPE(y, x)] : ANG270 + tantoangle[SLOPE(x, y)];
	}
	x = -x;
	if (y >= 0)
		return x > y ? ANG180 - 1 - tantoangle[SLOPE(y, x)] : ANG90 + tantoangle[SLOPE(x, y)];
	y = -y;
	return x > y ? ANG180 + tantoangle[SLOPE(y, x)] : ANG270 - 1 - tantoangle[SLOPE(x, y)];
#undef SLOPE
}

/* The things that may be in view: in a sector the viewer's can see into, and not behind or beside it. */
static void billboard(fixed_t x, fixed_t y, fixed_t z, angle_t angle, unsigned picture, int shadow, int level);

/* What a frame's things all use, read once; and where the next place drawn from is noted. */
struct eye { fixed_t x, y, sin, cos; angle_t angle; int frame, count; uint8_t *quads; short *lately; };

/* A thing's own place, or else one of its share's nothing was drawn from in the last frame or this one. */
static struct place *
thing_place(const struct thing *thing, unsigned at, int frame)
{
	struct place *place = NULL;
	int probe;

	if (thing->shadow)
		return NULL;	/* one that is blended */
	for (probe = 0; probe < 8 * crew; probe += crew) {
		struct place *p = &places[(at + probe) & (THING_PLACES - 1)];

		if (p->of == thing->id)
			return p;
		if (!place && p->frame + 1 < frame)
			place = p;
	}
	return place;
}

/*
 * Which of a frame's eight pictures the viewer sees, as Doom has it, and how far the thing
 * may move against the viewer before that can be another: the angle is `within` its eighth
 * by so much, less what R_PointToAngle may be off by (far below the 0.35 degrees taken), and
 * a move of d at distance r turns it by no more than asin(d / r).
 */
static void
thing_turn(struct place *place, const struct thing *thing, fixed_t rx, fixed_t ry)
{
	unsigned a = angle_to(rx, ry) - thing->angle + (unsigned)(ANG45 / 2) * 9, within = a & 0x1fffffff;
	int far = (abs(rx) > abs(ry) ? abs(rx) : abs(ry)) >> FRACBITS, space;

	if (within > 0x10000000)
		within = 0x1fffffff - within;
	space = (int)(within >> 16) - 64;
	place->turn = a >> 29;
	place->facing = thing->angle;
	place->rx = rx;
	place->ry = ry;
	place->still = space > 0 && far >= 32 ? (space * far) << 2 : -1;
}

/*
 * A thing in view keeps a place among the kept commands while it stays in view: a frame then
 * turns its quad to face the viewer, and sends a picture, a height or a light only when
 * they change. A thing no longer drawn gives its place up.
 */
static inline __attribute__((always_inline)) void
thing_kept(struct share *me, const struct thing *thing, int level, int level_grey, struct eye *eye)
{
	const unsigned at = place_at(thing->id), key = thing->picture;
	struct place *place = &places[at];
	int changed = 0, grey, command;
	fixed_t high;
	uint32_t *to;

	if (place->of != thing->id || thing->shadow) {
		place = thing_place(thing, at, eye->frame);
		if (!place) {
			/* no room, or one that is blended: the first share's core draws it by itself */
			if (me == shares)
				billboard(thing->x, thing->y, thing->z, thing->angle, thing->picture, thing->shadow, level);
			return;
		}
		if (place->of != thing->id) {
			changed = 2;	/* its first frame here */
			place->key = ~key;
			place->still = -1;
		}
	}
	eye->count++;
	if (place->key != key) {
		place->key = key;
		place->pictures = &sprites[key & 0xffff].spriteframes[key >> 16 & FF_FRAMEMASK];
		changed |= 1;
	}
	if (place->pictures->rotate) {
		const fixed_t rx = thing->x - eye->x, ry = thing->y - eye->y;

		if (thing->angle != place->facing || abs(rx - place->rx) + abs(ry - place->ry) > place->still) {
			const int turn = place->turn;

			thing_turn(place, thing, rx, ry);
			changed |= place->turn != turn;
		}
	} else if (place->turn) {
		place->turn = 0;
		changed |= 1;
	}
	command = group_count + place->index;
	to = (uint32_t *)(eye->quads + place->index * PLACE_BYTES);
	if (changed) {
		const int lump = place->pictures->lump[place->turn], flip = place->pictures->flip[place->turn];

		if (changed & 2 || lump != place->lump || flip != place->flip) {
			GLuint name = sprite_gl[lump];

			if (!name && on_worker) {
				/* asked for: the picture it had for now. A thing new here has the place
				 * all the same, as it would have had, and shows nothing yet (no lump) */
				if (me->asked_count < ASKS)
					me->asked[me->asked_count++] = lump;
				place->key = ~key;
				place->still = -1;
				if (changed & 2) {
					place->of = thing->id;
					place->lump = -1;
				}
				if (place->lump < 0) {
					if (place->frame != eye->frame)
						*eye->lately++ = place->index;
					place->frame = eye->frame;
					return;
				}
			} else if (!name && !(name = sprite_texture(lump))) {
				place->of = NULL;
				return;
			} else {
				if (!(changed & 2) && place->lump < 0)
					seglKeptShown(command, 1);	/* the picture it waited for */
				seglKeptTexture(command, name);
				to[3] = to[15] = (flip ? 1024 : 0) << 16;	/* (v, u): the picture lies on its side */
				to[7] = to[11] = (flip ? 0 : 1024) << 16;
				to[11] |= 1024;
				to[15] |= 1024;
				place->of = thing->id;
				place->lump = lump;
				place->flip = flip;
				place->wide = spritewidth[lump] >> FRACBITS;
				place->left = (flip ? spritewidth[lump] - spriteoffset[lump] : spriteoffset[lump]) >> FRACBITS;
				place->top = spritetopoffset[lump];
				place->tall = sprite_height[lump] << FRACBITS;
				place->high = thing->z + place->top + 1;	/* and so the heights and the corners are written below */
				place->grey = -1;
			}
		}
	}
	high = thing->z + place->top;
	if (place->high != high) {
		place->high = high;
		to[2] = to[6] = high;
		to[10] = to[14] = high - place->tall;
		place->x = thing->x + 1;
	}
	if (place->x != thing->x || place->y != thing->y || place->angle != eye->angle) {
		/* the viewer's right is (sin, -cos) of the view angle; a width is whole pixels */
		const fixed_t x1 = thing->x - place->left * eye->sin, y1 = thing->y + place->left * eye->cos;

		place->x = thing->x;
		place->y = thing->y;
		place->angle = eye->angle;
		to[0] = to[12] = x1;
		to[1] = to[13] = y1;
		to[4] = to[8] = x1 + place->wide * eye->sin;
		to[5] = to[9] = y1 - place->wide * eye->cos;
	}
	grey = key >> 16 & FF_FULLBRIGHT ? 255 : level_grey;
	if (place->grey != grey)
		seglKeptGrey(command, place->grey = grey);
	if (place->frame != eye->frame) {
		if (place->frame + 1 < eye->frame)
			seglKeptShown(command, 1);	/* it was not drawn in the last frame */
		*eye->lately++ = place->index;	/* (each place once a frame: there is room for all) */
		place->frame = eye->frame;
	}
}

/* The places a share drew from in the last frame and not in this one show nothing from now on. */
static void
things_gone(struct share *me, int frame)
{
	const short *before = me->lately[!me->lately_now];
	int i;

	for (i = me->lately_count[!me->lately_now]; i > 0; i--, before++)
		if (places[*before].frame != frame)
			seglKeptShown(group_count + *before, 0);
	me->lately_now = !me->lately_now;
	me->lately_count[me->lately_now] = 0;
}

/* The sectors a sector's row of REJECT lets it see, ended by -1. The first share lists them
 * for all when they are first asked for, while there is room; else each into a list of its own. */
static const short *
sectors_seen(struct share *me, int from)
{
	const byte *reject = rejectmatrix;
	short *list, *first;
	int s, n = 0, at = from * numsectors;

	if (seen_from[from])
		return seen_from[from];
	for (s = 0; s < numsectors; s++, at++)
		n += !(reject[at >> 3] & (1 << (at & 7)));
	if (me == shares && seen_used + n + 1 <= seen_room) {
		first = list = seen_lists + seen_used;
		seen_used += n + 1;
	} else {
		first = list = me->once;
		if (n >= (int)(sizeof me->once / sizeof me->once[0]))
			n = 0;
	}
	for (s = 0, at = from * numsectors; n && s < numsectors; s++, at++)
		if (!(reject[at >> 3] & (1 << (at & 7))))
			*list++ = s;
	*list = -1;
	if (first != me->once)
		seen_from[from] = first;	/* (when it is whole: another core may be looking) */
	return first;
}

/* With no worker core the things are the game's own, read between two tics as they lie. */
static void
world_things_direct(struct share *me)
{
	const short *list = sectors_seen(me, told.sector);
	const int cs = told.cos >> 4, sn = told.sin >> 4, vx = told.x >> FRACBITS, vy = told.y >> FRACBITS, margin = 96 << 12;
	const mobj_t *self = told.self, *m;
	struct eye eye = { told.x, told.y, told.sin, told.cos, told.angle, told.frame, 0, (uint8_t *)world_vertices + world_quads * 64,
			   me->lately[me->lately_now] };

	for (; *list >= 0; list++) {
		int level, level_grey;

		if (!(m = sectors[*list].thinglist))
			continue;
		level = now_[*list].light;
		level_grey = grey_of(level, 0);
		for (; m; m = m->snext) {
			int dx = (m->x >> FRACBITS) - vx, dy = (m->y >> FRACBITS) - vy;
			int ahead = dx * cs + dy * sn + margin, beside = dx * sn - dy * cs;

			if (ahead >= 0 && beside <= ahead && -beside <= ahead && m != self) {
				const struct thing thing = { m, m->x, m->y, m->z, m->angle, m->sprite | (unsigned)m->frame << 16, 0, 0, 0,
							     (m->flags & MF_SHADOW) != 0 };

				thing_kept(me, &thing, level, level_grey, &eye);
			}
		}
	}
	me->drawn += eye.count;
	me->lately_count[me->lately_now] = eye.lately - me->lately[me->lately_now];
	things_gone(me, eye.frame);
}

static void
world_things(struct share *me)
{
	const short *list = sectors_seen(me, told.sector);
	/* in whole map units, with twelve bits of the view's direction: products fit a word */
	const int cs = told.cos >> 4, sn = told.sin >> 4, vx = told.x >> FRACBITS, vy = told.y >> FRACBITS, margin = 96 << 12;
	const void *self = told.self;
	struct eye eye = { told.x, told.y, told.sin, told.cos, told.angle, told.frame, 0, (uint8_t *)world_vertices + world_quads * 64,
			   me->lately[me->lately_now] };

	for (; *list >= 0; list++) {
		int i = me->first[*list], level, level_grey;

		if (i < 0)
			continue;
		level = now_[*list].light;
		level_grey = grey_of(level, 0);
		for (; i >= 0; i = me->things[i].next) {
			const struct thing *thing = &me->things[i];
			int dx = (thing->x >> FRACBITS) - vx, dy = (thing->y >> FRACBITS) - vy;
			int ahead = dx * cs + dy * sn + margin, beside = dx * sn - dy * cs;

			if (ahead < 0 || beside > ahead || -beside > ahead || thing->id == self)
				continue;
			thing_kept(me, thing, level, level_grey, &eye);
		}
	}
	me->drawn += eye.count;
	me->lately_count[me->lately_now] = eye.lately - me->lately[me->lately_now];
	things_gone(me, eye.frame);
}

/* A new level: everything kept per seg, subsector and texture starts again. */
static void
new_level(void)
{
	static int textures_then, flats_then, sprites_then;
	const anim_def *anim;
	fixed_t box[4][2];
	int i, low = -16000, high = 16000;

	world_free();
	level_made = doom_level_loads;
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
	free(wall_now);
	free(flat_now);
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
	/* the pictures after animation, as the view is told of them from here on */
	wall_now = room((numtextures + 1) * sizeof *wall_now);
	flat_now = room((flat_count + 1) * sizeof *flat_now);
	memcpy(wall_now, texturetranslation, (numtextures + 1) * sizeof *wall_now);
	memcpy(flat_now, flattranslation, (flat_count + 1) * sizeof *flat_now);
	for (anim = anims, i = 0; anim < lastanim && i < 32; anim++, i++)
		anim_told[i] = anim->istexture ? texturetranslation[anim->basepic] : flattranslation[anim->basepic];
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
		fprintf(stderr, "doom_gl: %d segs, %d subsectors, %d polygon corners, %d flats; %u KB of texture memory left\n", numsegs,
			numsubsectors, corner_count, flat_count, seglMemoryLeft() >> 10);
}

/* ---- what the first core tells a view (doom_fast.c has what the game said) ---- */

#define TOLD_MOST 600
extern int doom_told_all, doom_told_changes, doom_told_links, doom_told_goings, doom_told_sector_count;
extern mobj_t *doom_told_changed[], *doom_told_linked[], *doom_told_gone[];
extern short *doom_told_sectors;
void doom_thing_changed(mobj_t *thing);
void doom_told_taken(int things);
void doom_view_finish(void);		/* doom_video.c: the view a worker core has is waited for, and shown */
int doom_view_inline;			/* so many views more are the first core's own to draw: a level's first */

static inline void
told_is(struct told_thing *is, const mobj_t *thing)
{
	is->id = thing;
	is->x = thing->x;
	is->y = thing->y;
	is->z = thing->z;
	is->angle = thing->angle;
	is->picture = thing->sprite | (unsigned)thing->frame << 16;
	is->sector = thing->subsector->sector - sectors;
	is->shadow = (thing->flags & MF_SHADOW) != 0;
}

static void
wall_told(int i)
{
	const int texture = *own[i].picture;
	const fixed_t offset = own[i].side->textureoffset;

	if (texture == own_told[i].texture && offset == own_told[i].offset)
		return;
	own_told[i].texture = told.wall[told.walls].texture = texture;
	own_told[i].offset = told.wall[told.walls].offset = offset;
	told.wall[told.walls++].own = i;
}

/* Everything a view needs of the game as it is now, between two tics. No worker has a view. */
static void
view_take(void)
{
	const player_t *player = viewplayer;
	const sector_t *sector = player->mo->subsector->sector;
	const anim_def *anim;
	const mobj_t *thing;
	int i, s;

	told.x = viewx;
	told.y = viewy;
	told.z = viewz;
	told.sin = viewsin;
	told.cos = viewcos;
	told.angle = viewangle;
	told.extralight = extralight;
	told.white = player->fixedcolormap != 0;
	told.window_x = viewwindowx;
	told.window_y = viewwindowy;
	told.wide = scaledviewwidth;
	told.high = viewheight;
	told.sector = sector - sectors;
	told.light = sector->lightlevel;
	told.unseen = player->powers[pw_invisibility] > 4 * 32 || player->powers[pw_invisibility] & 8;
	told.self = player->mo;
	for (i = 0; i < NUMPSPRITES; i++) {
		told.weapon[i].state = player->psprites[i].state;
		told.weapon[i].sx = player->psprites[i].sx;
		told.weapon[i].sy = player->psprites[i].sy;
	}
	told.things = told.links = told.goings = told.sectors = told.walls = told.anims = 0;
	told.frame = ++frame_number;
	if (segs != level_segs || numsegs != level_count || doom_level_loads != level_made) {
		unsigned began, now;

		__asm__ volatile("rdcycle %0" : "=r"(began));
		asked_count = 0;
		for (i = 0; i < CREW; i++)
			shares[i].asked_count = 0;
		new_level();
		doom_view_inline = 3;
		__asm__ volatile("rdcycle %0" : "=r"(now));
		if (getenv("DOOM_GL_DEBUG"))
			fprintf(stderr, "doom_gl: making the level for the GPU took %uk instructions\n", (now - began) / 1000);
	}
	if (!world_on)
		return;		/* drawn the port's way, from the game itself */
	asked_make();
	/* a player's thing turns with no state, place or height changed */
	for (i = 0; i < MAXPLAYERS && !direct; i++)
		if (playeringame[i] && players[i].mo)
			doom_thing_changed(players[i].mo);
	told.all = doom_told_all;
	if (direct) {
		/* (the things are read where they are: world_things_direct) */
	} else if (told.all) {
		for (s = 0, i = 0; s < numsectors; s++)
			for (thing = sectors[s].thinglist; thing; thing = thing->snext)
				i++;
		if (i > thing_room) {
			free(told.thing);
			told.thing = room((thing_room = i + 1024) * sizeof *told.thing);
		}
		for (s = 0; s < numsectors; s++)
			for (thing = sectors[s].thinglist; thing; thing = thing->snext)
				told_is(&told.thing[told.things++], thing);
	} else {
		if (thing_room < TOLD_MOST) {
			free(told.thing);
			told.thing = room((thing_room = TOLD_MOST + 1024) * sizeof *told.thing);
		}
		for (i = 0; i < doom_told_changes; i++) {
			thing = doom_told_changed[i];
			if (thing && !(thing->flags & MF_NOSECTOR))
				told_is(&told.thing[told.things++], thing);
		}
		memcpy(told.linked, doom_told_linked, (told.links = doom_told_links) * sizeof told.linked[0]);
		memcpy(told.gone, doom_told_gone, (told.goings = doom_told_goings) * sizeof told.gone[0]);
	}
	for (i = 0; i < doom_told_sector_count; i++) {
		struct told_sector *is = &told.sector_now[told.sectors++];

		is->s = s = doom_told_sectors[i];
		is->floor = sectors[s].floorheight;
		is->ceiling = sectors[s].ceilingheight;
		is->light = sectors[s].lightlevel;
		is->floorpic = sectors[s].floorpic;
		is->ceilingpic = sectors[s].ceilingpic;
		now_[s] = *is;		/* (here, so that every share's core has it from its first instruction) */
	}
	doom_told_taken(!direct);
	if (doom_walls_changed) {
		doom_walls_changed = 0;
		for (i = 0; i < own_count; i++)
			wall_told(i);
	} else {
		for (i = 0; i < sliding_count; i++)
			wall_told(sliding[i]);
	}
	for (anim = anims, i = 0; anim < lastanim && i < 32; anim++, i++) {
		const int now = anim->istexture ? texturetranslation[anim->basepic] : flattranslation[anim->basepic];

		if (now != anim_told[i]) {
			told.anim[told.anims].anim = i;
			told.anim[told.anims++].now = anim_told[i] = now;
		}
	}
}

/*
 * DOOM_VIEW_CHECK=1, with the view the first core's own: what the view was told, after it took
 * it, against the game as it is: every thing of every sector's list in its order, the sectors,
 * the walls that change and the pictures after animation.
 */
static void
view_check(void)
{
	static unsigned views, wrong;
	int s, i, bad = 0;

	for (s = 0; s < numsectors && bad < 4; s++) {
		const mobj_t *m = sectors[s].thinglist;
		int at[CREW];

		/* each share's list of the sector is the game's list without the other shares' things */
		for (i = 0; i < crew; i++)
			at[i] = shares[i].first[s];
		for (; m; m = m->snext) {
			const int mine = share_of(m, (m->flags & MF_SHADOW) != 0);
			const struct thing *t = at[mine] < 0 ? NULL : &shares[mine].things[at[mine]];

			if (!t || t->id != m || t->x != m->x || t->y != m->y || t->z != m->z || t->angle != m->angle ||
			    t->picture != (m->sprite | (unsigned)m->frame << 16) || t->shadow != ((m->flags & MF_SHADOW) != 0))
				break;
			at[mine] = t->next;
		}
		for (i = 0; i < crew; i++)
			if (m || at[i] >= 0) {
				bad += fprintf(stderr, "doomcheck: tic %d sector %d: a thing of kind %d is not as told\n", gametic, s, m ? (int)m->type : -1) > 0;
				break;
			}
		if (now_[s].floor != sectors[s].floorheight || now_[s].ceiling != sectors[s].ceilingheight || now_[s].light != sectors[s].lightlevel ||
		    now_[s].floorpic != sectors[s].floorpic || now_[s].ceilingpic != sectors[s].ceilingpic)
			bad += fprintf(stderr, "doomcheck: tic %d sector %d is not as told\n", gametic, s) > 0;
	}
	for (i = 0; i < own_count && bad < 4; i++)
		if (own[i].texture != *own[i].picture || own[i].offset != own[i].side->textureoffset)
			bad += fprintf(stderr, "doomcheck: tic %d: a wall is not as told\n", gametic) > 0;
	if (memcmp(wall_now, texturetranslation, numtextures * sizeof *wall_now) || memcmp(flat_now, flattranslation, flat_count * sizeof *flat_now))
		bad += fprintf(stderr, "doomcheck: tic %d: the pictures after animation are not as told\n", gametic) > 0;
	wrong += bad != 0;
	if (++views % 500 == 0)
		fprintf(stderr, "doomcheck: %u views, %u not as told\n", views, wrong);
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
	GLuint name = wall_texture(texturetranslation[texture]);
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
	GLuint name = flat_texture(flattranslation[picture]);
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
billboard(fixed_t x, fixed_t y, fixed_t z, angle_t angle, unsigned picture, int shadow, int level)
{
	spriteframe_t *frame = &sprites[picture & 0xffff].spriteframes[picture >> 16 & FF_FRAMEMASK];
	int turn = 0, lump, flip;
	fixed_t left, width, x1, y1, x2, y2, high, low;
	GLuint name;

	if (frame->rotate)
		turn = (angle_to(x - told.x, y - told.y) - angle + (unsigned)(ANG45 / 2) * 9) >> 29;
	lump = frame->lump[turn];
	flip = frame->flip[turn];
	name = sprite_gl[lump] ? sprite_gl[lump] : sprite_texture(lump);
	if (!name)
		return;
	shares[0].drawn++;
	width = spritewidth[lump];
	left = flip ? width - spriteoffset[lump] : spriteoffset[lump];
	/* the viewer's right is (sin, -cos) of the view angle */
	x1 = x - FixedMul(left, told.sin);
	y1 = y + FixedMul(left, told.cos);
	x2 = x1 + FixedMul(width, told.sin);
	y2 = y1 - FixedMul(width, told.cos);
	high = z + spritetopoffset[lump];
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
	if (shadow)
		seglQuad(quad_xyz, quad_uv, name, 0, 110, 1);
	else
		seglQuad(quad_xyz, quad_uv, name, grey_of(picture >> 16 & FF_FULLBRIGHT ? 255 : level, 0), 255, 1);
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
				billboard(thing->x, thing->y, thing->z, thing->angle, thing->sprite | (unsigned)thing->frame << 16,
					  (thing->flags & MF_SHADOW) != 0, sector->lightlevel);
	}
	while (count--)
		R_AddLine(seg++);
}

/* The sky: one picture a quarter turn wide behind everything, sliding with the view angle. */
static void
sky(void)
{
	GLuint name = sky_texture();
	fixed_t u = (fixed_t)(((told.angle + ANG45) >> 22) << 8), v = ((100 - told.high / 2) << FRACBITS) / 128;
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
		quad_uv[k * 2 + 1] = v + (k >= 2 ? (told.high << FRACBITS) / 128 : 0);
	}
	seglQuad(quad_xyz, quad_uv, name, 255, 255, 0);
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

	if (!told.extralight || told.white)
		return;
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glColor4ub(255, 255, 255, told.extralight > 3 ? 32 : told.extralight << 3);	/* about what a level more does to a dim room */
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
	int i, k;

	for (i = 0; i < NUMPSPRITES; i++) {
		const state_t *state = told.weapon[i].state;
		spriteframe_t *frame;
		fixed_t scale = (told.wide << FRACBITS) / 320, x, y, w, h;
		GLuint name;
		int lump, flip;

		if (!state)
			continue;
		frame = &sprites[state->sprite].spriteframes[state->frame & FF_FRAMEMASK];
		lump = frame->lump[0];
		flip = frame->flip[0];
		name = sprite_texture(lump);
		if (!name)
			continue;
		x = (told.wide << FRACBITS) / 2 + FixedMul(told.weapon[i].sx - 160 * FRACUNIT - spriteoffset[lump], scale);
		y = (told.high << FRACBITS) / 2 + FixedMul(told.weapon[i].sy - spritetopoffset[lump] - 100 * FRACUNIT - FRACUNIT / 2, scale);
		w = FixedMul(spritewidth[lump], scale);
		h = FixedMul(sprite_height[lump] << FRACBITS, scale);
		for (k = 0; k < 4; k++) {
			int right = k == 1 || k == 2, low = k >= 2;

			quad_xyz[k * 3] = (x + (right ? w : 0)) / told.wide;
			quad_xyz[k * 3 + 1] = (y + (low ? h : 0)) / told.high;
			quad_xyz[k * 3 + 2] = 0;
			quad_uv[k * 2 + 1] = right != flip ? FRACUNIT : 0;	/* (v, u) */
			quad_uv[k * 2] = low ? FRACUNIT : 0;
		}
		/* (unseen, it is a dark shape blended over the view) */
		if (told.unseen)
			seglQuad(quad_xyz, quad_uv, name, 0, 110, 1);
		else
			seglQuad(quad_xyz, quad_uv, name, grey_of(state->frame & FF_FULLBRIGHT ? 255 : told.light, 0), 255, 1);
	}
}

/* A holodeck's sitter with tracked hands holds the weapon (docs/world/holodeck.md, "Played from the
 * seat"): its pictures stand in the world at the right hand, facing the viewer, 4 mm a pixel
 * of Doom's 320-wide screen, the hand at that screen's (160, 150). 1 if they were drawn. */
static int
weapon_in_hand(void)
{
	const volatile unsigned *host = seglHostState();
	float wide, tall, metre;
	fixed_t pixel, hx, hy, hz, px, py, pz;
	int i;

	if (!host || !SEGL_HOST_HOLODECK(host) || !SEGL_HOST_HANDS(host) || !told.high)
		return 0;
	/* map units a metre: the room's plane, 1.28 by 0.8 m, is so far from the camera (GpuVolumePass.cginc) */
	wide = 1.28f, tall = 0.8f * told.wide / told.high;
	metre = 2 * 4 * powf(8192 / 4, SEGL_HOST_PLANE(host) / 255.0f) / (wide < tall ? wide : tall);
	pixel = (fixed_t)(metre * 0.004f * FRACUNIT);
	hx = SEGL_HOST_HAND_X(host, 1) * pixel;
	hy = SEGL_HOST_HAND_Y(host, 1) * pixel;
	hz = SEGL_HOST_HAND_Z(host, 1) * pixel;
	/* the viewer's right is (sin, -cos) of the view angle, forward (cos, sin) */
	px = told.x + FixedMul(hz, told.cos) + FixedMul(hx, told.sin);
	py = told.y + FixedMul(hz, told.sin) - FixedMul(hx, told.cos);
	pz = told.z + hy;
	for (i = 0; i < NUMPSPRITES; i++) {
		const state_t *state = told.weapon[i].state;
		spriteframe_t *frame;
		fixed_t left, width, x1, y1, x2, y2, high, low;
		GLuint name;
		int lump, flip;

		if (!state)
			continue;
		frame = &sprites[state->sprite].spriteframes[state->frame & FF_FRAMEMASK];
		lump = frame->lump[0];
		flip = frame->flip[0];
		name = sprite_texture(lump);
		if (!name)
			continue;
		left = FixedMul(told.weapon[i].sx - spriteoffset[lump] - 160 * FRACUNIT, pixel);
		width = FixedMul(spritewidth[lump], pixel);
		x1 = px + FixedMul(left, told.sin);
		y1 = py - FixedMul(left, told.cos);
		x2 = x1 + FixedMul(width, told.sin);
		y2 = y1 - FixedMul(width, told.cos);
		high = pz - FixedMul(told.weapon[i].sy - spritetopoffset[lump] - 150 * FRACUNIT, pixel);
		low = high - sprite_height[lump] * pixel;
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
		seglQuad(quad_xyz, quad_uv, name, grey_of(state->frame & FF_FULLBRIGHT ? 255 : told.light, 0), 255, 1);
	}
	return 1;
}

/* The view's matrices, column by column as glLoadMatrixx takes them: Doom's view is a quarter
 * turn wide with square pixels; x and y are on the ground and z is up. */
static GLfixed to_clip[16], to_eye[16];

static void
view_matrices(void)
{
	static int wide = -1, high;
	const fixed_t far = 8192 * FRACUNIT;

	if (wide != told.wide || high != told.high) {
		wide = told.wide;
		high = told.high;
		to_clip[0] = FRACUNIT;
		to_clip[5] = (told.wide << FRACBITS) / told.high;
		to_clip[10] = -FixedDiv(far + NEAR, far - NEAR);
		to_clip[14] = -FixedDiv(FixedMul(2 * NEAR, far >> 8), (far - NEAR) >> 8);
		to_clip[11] = -FRACUNIT;
	}
	to_eye[0] = told.sin;
	to_eye[4] = -told.cos;
	to_eye[12] = -(FixedMul(told.sin, told.x) - FixedMul(told.cos, told.y));
	to_eye[9] = FRACUNIT;
	to_eye[13] = -told.z;
	to_eye[2] = -told.cos;
	to_eye[6] = -told.sin;
	to_eye[14] = FixedMul(told.cos, told.x) + FixedMul(told.sin, told.y);
	to_eye[15] = FRACUNIT;
}

/* glOrthox(0, 1, 1, 0, -1, 1), column by column. */
static const GLfixed flat_view[16] = { 2 * FRACUNIT, 0, 0, 0, 0, -2 * FRACUNIT, 0, 0, 0, 0, -FRACUNIT, 0, -FRACUNIT, FRACUNIT, 0, FRACUNIT };

/*
 * The game's part of a view, between two tics: where the eye is, the key under the 2D picture,
 * and what the view is told. The list is made from that afterwards (doom_view_draw), by a
 * worker core while the next tic runs, or by this one.
 */
void
R_RenderPlayerView(player_t *player)
{
	unsigned began, now;
	int y;

	__asm__ volatile("rdcycle %0" : "=r"(began));
	if (!doom_gl) {
		soft_tables();
		R_RenderPlayerView_soft(player);
		return;
	}
	R_SetupFrame(player);
	doom_view_finish();
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
	view_take();
	__asm__ volatile("rdcycle %0" : "=r"(now));
	doom_view[0] += now - began;
}

/* The sprites the shares' cores asked for (asked_make). */
static void
things_asked(void)
{
	int i, k;

	for (k = 0; k < CREW; k++) {
		for (i = 0; i < shares[k].asked_count; i++)
			sprite_texture(shares[k].asked[i]);
		shares[k].asked_count = 0;
	}
}

/* A worker core's pass ends until the GPU has drawn the list before this one: nothing the
 * list reads is written before (fence.i: a worker's pause would be its sleep). The library's
 * word that says so is the first share's to clear, and the others wait for it (two cores
 * storing to it in one pass is a race on the GPU): every kept command's change settles first. */
static volatile unsigned settled_frame;

static void
drawn_wait(int share)
{
	while (on_worker && (*(const volatile unsigned *)((const char *)seglPalette() - 0x400 + 0x10) ||
			     (share && settled_frame != (unsigned)told.frame)))
		mc_end_pass();
	if (!share) {
		seglSettle();
		settled_frame = told.frame;
	}
}

/*
 * The list of the view last told of: share 0 is the view and its own things, any other only
 * that share's things, which no other core writes. On a worker core nothing a tic writes is read.
 */
void
doom_view_draw(int share)
{
	static int check = -1;
	struct share *me = &shares[share];
	unsigned began, now;
	int width, height, y, held;

	__asm__ volatile("rdcycle %0" : "=r"(began));
	if (share) {
		if (world_on) {
			things_told(me);
			drawn_wait(share);
			world_things(me);
		}
		__asm__ volatile("rdcycle %0" : "=r"(now));
		me->cost += now - began;
		return;
	}
	seglSize(&width, &height);
	/* the window may be any size: the view keeps its place on Doom's 320 by 200 screen */
	glViewport(told.window_x * width / SCREENWIDTH, height - (told.window_y + told.high) * height / SCREENHEIGHT,
		told.wide * width / SCREENWIDTH, told.high * height / SCREENHEIGHT);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glColorKeySE(KEY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	view_matrices();
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(to_clip);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixx(to_eye);
	if (world_on) {
		if (!direct)
			things_told(me);
		sectors_told();
		/* the level is there already: its matrices, what has changed, and the things */
		drawn_wait(0);
		seglKeptMatrices();
		for (y = sector_group[numsectors]; y < sector_group[numsectors + 1]; y++) {
			/* the sky's runs: its picture where sky() below would put it, by the pixel */
			int vw = told.wide * width / SCREENWIDTH, vh = told.high * height / SCREENHEIGHT;
			int vx = told.window_x * width / SCREENWIDTH, vy = told.window_y * height / SCREENHEIGHT;
			fixed_t u = ((fixed_t)(((told.angle + ANG45) >> 22) << 8) & 0xffff) + 4 * FRACUNIT;
			fixed_t v = ((100 - told.high / 2) << FRACBITS) / 128;

			if (vw > 0 && vh > 0)
				seglKeptLaid(y, u + (vx << FRACBITS) / vw, v - ((vy * told.high) << 9) / vh, -(1024 << FRACBITS) / vw,
					((8 * told.high) << FRACBITS) / vh);
		}
		world_update();
		if (check < 0)
			check = getenv("DOOM_VIEW_CHECK") != NULL;
		if (check && !on_worker && !direct) {
			for (y = 1; y < crew; y++)
				things_told(&shares[y]);	/* (twice is as once: each share's own turn comes) */
			view_check();
		}
		if (direct)
			world_things_direct(me);
		else
			world_things(me);
	} else {
		soft_tables();
		R_ClearClipSegs();
		in_view = 1;
		R_RenderBSPNode(numnodes - 1);
		in_view = 0;
	}

	glEnable(GL_ALPHA_TEST);
	held = weapon_in_hand();
	glDisable(GL_ALPHA_TEST);

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
	if (!held)
		weapon();
	glDisable(GL_ALPHA_TEST);
	__asm__ volatile("rdcycle %0" : "=r"(now));
	me->cost += now - began;
}

/* So many worker cores make views: the things are shared out between 1, 2 or 4 of them. How
 * many shares there are; set before a level is made. With none the things are read where the
 * game has them, unless `told` (DOOM_VIEW=told: what workers would be told, to check it). */
int
doom_view_crew(int workers, int as_told)
{
	direct = !workers && !as_told;
	return crew = workers >= 4 ? 4 : workers >= 2 ? 2 : 1;
}

/* What the lists made since the last call cost their cores, and the things they drew. */
void
doom_view_taken(unsigned *cost, unsigned *things)
{
	int k;

	for (k = 0; k < CREW; k++) {
		*cost += shares[k].cost;
		*things += shares[k].drawn;
		shares[k].cost = shares[k].drawn = 0;
	}
}

/* How many pictures the worker core asked for in the view it made; and that view once more by
 * the first core, which makes them: for a picture that is held and compared. */
int
doom_view_asked(void)
{
	int k, count = asked_count;

	for (k = 0; k < CREW; k++)
		count += shares[k].asked_count;
	return count;
}

void
doom_view_again(void)
{
	int k;

	asked_make();
	seglDiscard();
	told.frame = ++frame_number;
	for (k = 0; k < crew; k++)
		doom_view_draw(k);
}

/* ---- the message at the top of the view ---- */

extern hu_stext_t w_message;	/* hu_stuff.c's, made reachable by doom.sh */
extern boolean message_on, chat_on;
void HU_Drawer(void);
void HU_Erase(void);

static unsigned message_shown;	/* a sum of the words on the screen; 0 for none */
static int message_stays;	/* in this frame they are left as they are, on the border above the view */

/* A sum of the message's words, 0 for none, and the rows they are drawn in. */
static unsigned
message_sum(int *top, int *bottom)
{
	int i, k, tall = SHORT(w_message.l[0].f[0]->height) + 1;
	unsigned now = 0;

	*top = SCREENHEIGHT;
	*bottom = 0;
	for (i = 0; i < w_message.h; i++) {
		const hu_textline_t *line = &w_message.l[i];

		if (line->y < *top)
			*top = line->y;
		if (line->y + tall > *bottom)
			*bottom = line->y + tall;
		for (k = 0; message_on && k < line->len; k++)
			now = now * 31 + (unsigned char)line->l[k];
	}
	return message_on ? (now * 31 + w_message.cl) | 1 : 0;
}

/* Doom draws the message itself, every frame. */
static int
message_apart(void)
{
	static int every_frame = -1;

	if (every_frame < 0) {
		const char *port = getenv("DOOM_PORT");

		every_frame = port && strstr(port, "message");	/* DOOM_PORT=message: as the port does, to compare */
	}
	return !doom_gl || automapactive || chat_on || every_frame;
}

/*
 * A view smaller than the screen has the message on the border above it, and Doom restores
 * the border and letters it again every frame. While the words are the same and nothing has
 * drawn on their rows since the last view, neither is done. (d_main.c calls this for HU_Erase.)
 */
void
HU_Erase_kept(void)
{
	int top, bottom;
	unsigned now = message_sum(&top, &bottom);

	message_stays = now && now == message_shown && keyed && viewwindowx && viewwindowy >= bottom && !message_apart() &&
			(dirtybox[BOXTOP] < top || dirtybox[BOXBOTTOM] >= bottom);
	if (!message_stays)
		HU_Erase();
}

/*
 * Doom draws its message again every frame, a letter at a time, and the view then has to put
 * the key back under it. While the words are the same and nothing else has drawn over them,
 * they are left on the screen as they are. (d_main.c calls this for HU_Drawer.)
 */
void
HU_Drawer_kept(void)
{
	int box[4], i, top, bottom;
	unsigned now = message_sum(&top, &bottom);

	if (message_apart() || (viewwindowx && viewwindowy < bottom)) {
		HU_Drawer();
		message_shown = ~0u;
		return;
	}
	if (viewwindowx) {
		/* on the border: HU_Erase_kept has cleared it, unless the words stay */
		if (message_stays || (!now && !message_shown))
			return;
		memcpy(box, dirtybox, sizeof box);
		HU_Drawer();
		memcpy(dirtybox, box, sizeof box);	/* our own letters are not somebody drawing over them */
		message_shown = now;
		return;
	}
	if (bottom > viewwindowy + viewheight)
		bottom = viewwindowy + viewheight;
	if (now == message_shown && (keyed_last < top || keyed_first >= bottom))
		return;
	/* other words, or none: the key under them first, unless the view has just put it there */
	if (message_shown && !(keyed_first <= top && keyed_last >= bottom - 1))
		for (i = top; i < bottom; i++)
			memset(screens[0] + i * SCREENWIDTH, KEY, SCREENWIDTH);
	memcpy(box, dirtybox, sizeof box);
	HU_Drawer();
	memcpy(dirtybox, box, sizeof box);	/* these rows are ours to clear, not the view's */
	message_shown = now;
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
	glViewport(told.window_x * width / SCREENWIDTH, height - (told.window_y + told.high) * height / SCREENHEIGHT,
		told.wide * width / SCREENWIDTH, told.high * height / SCREENHEIGHT);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(to_clip);	/* (as the last view left them) */
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixx(to_eye);
	seglKeptMatrices();
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(flat_view);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	sky();
	doom_gl_scene = 1;
}
