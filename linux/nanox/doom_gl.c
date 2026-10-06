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

static int in_view;			/* inside our R_RenderPlayerView */
static int keyed;			/* the view's area of the screen holds the key, but for what dirtybox covers */
static seg_t *level_segs;		/* what the per-level tables below were made for */
static int level_count;
static fixed_t *seg_length;
static int *seg_frame;			/* the frame each seg was last drawn in */
static int *subsector_frame;
static struct { int first, count; } *polygon;	/* per subsector, into corners */
static fixed_t (*corners)[2];
static int corner_count, corner_room;
static GLuint *wall_gl, *flat_gl, *sprite_gl;
static short *sprite_height;
static int flat_count;
static int frame_number;

static GLfixed quad_xyz[12], quad_uv[8], fan_xyz[MAX_CORNERS * 3], fan_uv[MAX_CORNERS * 2];

/* ---- full-screen pictures ---- */

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
	int i;

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
	memcpy(screens[0], page, SCREENWIDTH * SCREENHEIGHT);
	V_MarkRect(0, 0, SCREENWIDTH, SCREENHEIGHT);
}

/* ---- textures: Doom's pictures as rows of palette indices in GPU memory ---- */

/* Copies a column of posts into a picture `width` wide, at column `x`. */
static void
posts(byte *picture, int width, int height, int x, column_t *column)
{
	while (column->topdelta != 0xff) {
		byte *from = (byte *)column + 3;
		int k, row = column->topdelta;

		for (k = 0; k < column->length; k++, row++)
			if (row < height)
				picture[row * width + x] = from[k];
		column = (column_t *)((byte *)column + column->length + 4);
	}
}

static GLuint
wall_texture(int number)
{
	int width, height, x, row;
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
		byte *column = R_GetColumn(number, x);

		if (texturecolumnlump[number][x] > 0) {
			/* straight from one patch: posts, which may leave holes */
			posts(picture, width, height, x, (column_t *)(column - 3));
		} else {
			for (row = 0; row < height; row++)
				picture[row * width + x] = column[row];
		}
	}
	glBindTexture(GL_TEXTURE_2D, wall_gl[number]);
	seglTexturePointer(picture, width, height, GL_COLOR_INDEX8_EXT);
	return wall_gl[number];
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
	memset(picture, KEY, width * height);
	for (x = 0; x < width; x++)
		posts(picture, width, height, x, (column_t *)((byte *)patch + LONG(patch->columnofs[x])));
	sprite_height[lump] = height;
	glBindTexture(GL_TEXTURE_2D, sprite_gl[lump]);
	seglTexturePointer(picture, width, height, GL_COLOR_INDEX8_EXT);
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

/* A new level: everything kept per seg, subsector and texture starts again. */
static void
new_level(void)
{
	static int textures_then, flats_then, sprites_then;
	fixed_t box[4][2];
	int i, low = -16000, high = 16000;

	/* the last level's pictures go; the screen's texture (doom_video.c) is not ours */
	if (wall_gl) {
		glDeleteTextures(textures_then, wall_gl);
		glDeleteTextures(flats_then, flat_gl);
		glDeleteTextures(sprites_then, sprite_gl);
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
	if (getenv("DOOM_GL_DEBUG"))
		fprintf(stderr, "doom_gl: %d segs, %d subsectors, %d polygon corners, %d flats\n", numsegs, numsubsectors,
			corner_count, flat_count);
}

/* ---- drawing ---- */

static void
light(int level)
{
	level += extralight << 4;
	if (viewplayer->fixedcolormap)
		level = 255;
	level = level < 0 ? 0 : level > 255 ? 255 : level;
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
	quad_uv[0] = quad_uv[6] = u1;
	quad_uv[2] = quad_uv[4] = u2;
	quad_uv[1] = quad_uv[3] = v1;
	quad_uv[5] = quad_uv[7] = v2;
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
	name = sprite_texture(lump);
	if (!name)
		return;
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
	quad_uv[0] = quad_uv[6] = flip ? FRACUNIT : 0;
	quad_uv[2] = quad_uv[4] = flip ? 0 : FRACUNIT;
	quad_uv[1] = quad_uv[3] = 0;
	quad_uv[5] = quad_uv[7] = FRACUNIT;
	if (thing->flags & MF_SHADOW) {
		/* the half-seen: a dark shape blended over what is behind */
		glEnable(GL_BLEND);
		glColor4ub(0, 0, 0, 110);
	} else {
		light(thing->frame & FF_FULLBRIGHT ? 255 : level);
	}
	glEnable(GL_ALPHA_TEST);
	glBindTexture(GL_TEXTURE_2D, name);
	glVertexPointer(3, GL_FIXED, 0, quad_xyz);
	glTexCoordPointer(2, GL_FIXED, 0, quad_uv);
	glDrawArrays(GL_QUADS, 0, 4);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_BLEND);
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
	GLuint name = wall_texture(skytexture);
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
			quad_uv[k * 2] = right != flip ? FRACUNIT : 0;
			quad_uv[k * 2 + 1] = low ? FRACUNIT : 0;
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

void
R_RenderPlayerView(player_t *player)
{
	int width, height, y;
	GLfixed projection[4][4] = {{0}}, view[4][4] = {{0}};
	fixed_t far = 8192 * FRACUNIT;

	if (!doom_gl) {
		R_RenderPlayerView_soft(player);
		return;
	}
	R_SetupFrame(player);
	if (segs != level_segs || numsegs != level_count)
		new_level();
	frame_number++;
	doom_gl_scene = 1;
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

	/* the sky, in view pixels, as far away as depth goes */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthox(0, FRACUNIT, FRACUNIT, 0, -FRACUNIT, FRACUNIT);	/* the view is 1 by 1: exact in 16.16 */
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	sky();

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
	glMatrixMode(GL_PROJECTION);
	load(projection);
	glMatrixMode(GL_MODELVIEW);
	load(view);

	R_ClearClipSegs();
	in_view = 1;
	R_RenderBSPNode(numnodes - 1);
	in_view = 0;

	/* the weapon, flat on top */
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_ALPHA_TEST);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthox(0, FRACUNIT, FRACUNIT, 0, -FRACUNIT, FRACUNIT);	/* the view is 1 by 1: exact in 16.16 */
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	weapon();
	glDisable(GL_ALPHA_TEST);
}
