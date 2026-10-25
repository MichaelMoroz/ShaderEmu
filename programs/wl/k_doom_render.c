/*
 * doom_render: Doom's software view, fixed point all through. Copied from the Doom source of microwindows'
 * contrib (the machine's port, linux/nanox/doom.sh): R_PointOnSide, R_PointToAngle with SlopeDiv and tantoangle,
 * R_RenderBSPNode, R_CheckBBox, R_AddLine, R_ClipSolidWallSegment, R_ClipPassWallSegment, R_ClearClipSegs,
 * R_DrawColumn, R_DrawSpan, FixedMul; FixedDiv2 is the port's (doom_fast.c: no doubles). R_StoreWallRange is a
 * shortened one: the scale from R_ScaleFromGlobalAngle at both ends, stepped across, a wall column per x with
 * the clip arrays (R_RenderSegLoop's work without its upper and lower parts); the floor and ceiling are a span
 * a row (R_MapPlane's step from the distance a row) before the walls. The map: 32 x 32 cells of 128 units, a
 * fifth solid, sector heights by cell; the tree splits the cells in halves (axial partitions, as most of Doom's).
 * A unit: one 320 x 200 frame from a fixed point in the map.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wl.h"

typedef int fixed_t;
typedef unsigned angle_t;
#define FRACBITS 16
#define FRACUNIT (1 << FRACBITS)
#define ANG90 0x40000000u
#define ANG180 0x80000000u
#define ANG270 0xc0000000u
#define FINEANGLES 8192
#define ANGLETOFINESHIFT 19
#define SLOPERANGE 2048
#define NF_SUBSECTOR 0x8000
#define BOXTOP 0
#define BOXBOTTOM 1
#define BOXLEFT 2
#define BOXRIGHT 3
#define SCREENWIDTH 320
#define SCREENHEIGHT 200
#define CELLS 32
#define CELL (128 * FRACUNIT)
#define MAXSEGS 32

typedef struct { fixed_t floorheight, ceilingheight; short floorpic, ceilingpic, lightlevel; } sector_t;
typedef struct { fixed_t x, y; } vertex_t;
typedef struct { vertex_t v1, v2; sector_t *frontsector, *backsector; int midtexture; } seg_t;
typedef struct { int firstline, numlines; } subsector_t;
typedef struct { fixed_t x, y, dx, dy; fixed_t bbox[2][4]; unsigned short children[2]; } node_t;
typedef struct { int first, last; } cliprange_t;

static angle_t tantoangle[SLOPERANGE + 1];
static fixed_t finesine[FINEANGLES + FINEANGLES / 4], *finecosine = finesine + FINEANGLES / 4;
static int viewangletox[FINEANGLES / 2];
static fixed_t distscale[SCREENWIDTH], yslope[SCREENHEIGHT];
static sector_t sectors[CELLS * CELLS];
static unsigned char solid[CELLS][CELLS];
static seg_t segs[CELLS * CELLS * 4];
static subsector_t subsectors[CELLS * CELLS];
static node_t nodes[CELLS * CELLS];
static int numnodes, numsubsectors, numsegs;
static unsigned char textures[8][128 * 128], flats[4][64 * 64], colormaps[32][256];

static fixed_t viewx, viewy, viewz;
static angle_t viewangle, clipangle;
static const int centerx = SCREENWIDTH / 2, centery = SCREENHEIGHT / 2;
static const fixed_t centerxfrac = (SCREENWIDTH / 2) << FRACBITS, projection = (SCREENWIDTH / 2) << FRACBITS;

/* what a frame stores: the core's own */
struct scratch {
    unsigned char screen[SCREENWIDTH * SCREENHEIGHT];
    cliprange_t solidsegs[MAXSEGS], *newend;
    short floorclip[SCREENWIDTH], ceilingclip[SCREENWIDTH];
    const seg_t *curline;
    const sector_t *frontsector;
    angle_t rw_angle1;
};

static inline fixed_t FixedMul(fixed_t a, fixed_t b) { return (fixed_t)(((long long)a * (long long)b) >> FRACBITS); }

static fixed_t FixedDiv2(fixed_t a, fixed_t b)
{
    unsigned ua = a < 0 ? -(unsigned)a : (unsigned)a, ub = b < 0 ? -(unsigned)b : (unsigned)b;
    unsigned q = ua / ub, r = ua % ub, top = ub;
    int i = 0, room = 0;
    if (!(top >> 16)) room = 16;
    else {
        if (!(top >> 24)) room += 8, top <<= 8;
        if (!(top >> 28)) room += 4, top <<= 4;
        if (!(top >> 30)) room += 2, top <<= 2;
        if (!(top >> 31)) room++;
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
        r <<= 1, q <<= 1;
        if (carry || r >= ub) r -= ub, q |= 1;
    }
    return (a ^ b) < 0 ? -(fixed_t)q : (fixed_t)q;
}

static fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    if ((abs(a) >> 14) >= abs(b)) return (a ^ b) < 0 ? (fixed_t)0x80000000 : 0x7fffffff;
    return FixedDiv2(a, b);
}

static int SlopeDiv(unsigned num, unsigned den)
{
    unsigned ans;
    if (den < 512) return SLOPERANGE;
    ans = (num << 3) / (den >> 8);
    return ans <= SLOPERANGE ? (int)ans : SLOPERANGE;
}

static int R_PointOnSide(fixed_t x, fixed_t y, const node_t *node)
{
    fixed_t dx, dy, left, right;
    if (!node->dx) return x <= node->x ? node->dy > 0 : node->dy < 0;
    if (!node->dy) return y <= node->y ? node->dx < 0 : node->dx > 0;
    dx = x - node->x, dy = y - node->y;
    if ((node->dy ^ node->dx ^ dx ^ dy) & 0x80000000) return ((node->dy ^ dx) & 0x80000000) != 0;
    left = FixedMul(node->dy >> FRACBITS, dx);
    right = FixedMul(dy, node->dx >> FRACBITS);
    return right < left ? 0 : 1;
}

static angle_t R_PointToAngle(fixed_t x, fixed_t y)
{
    x -= viewx, y -= viewy;
    if (!x && !y) return 0;
    if (x >= 0) {
        if (y >= 0) return x > y ? tantoangle[SlopeDiv(y, x)] : ANG90 - 1 - tantoangle[SlopeDiv(x, y)];
        y = -y;
        return x > y ? -tantoangle[SlopeDiv(y, x)] : ANG270 + tantoangle[SlopeDiv(x, y)];
    }
    x = -x;
    if (y >= 0) return x > y ? ANG180 - 1 - tantoangle[SlopeDiv(y, x)] : ANG90 + tantoangle[SlopeDiv(x, y)];
    y = -y;
    return x > y ? ANG180 + tantoangle[SlopeDiv(y, x)] : ANG270 - 1 - tantoangle[SlopeDiv(x, y)];
}

static void R_DrawColumn(unsigned char *screen, int dc_x, int dc_yl, int dc_yh, fixed_t dc_iscale, fixed_t dc_texturemid,
                         const unsigned char *dc_source, const unsigned char *dc_colormap)
{
    int count = dc_yh - dc_yl;
    unsigned char *dest;
    fixed_t frac, fracstep;
    if (count < 0) return;
    dest = screen + dc_yl * SCREENWIDTH + dc_x;
    fracstep = dc_iscale;
    frac = dc_texturemid + (dc_yl - centery) * fracstep;
    do {
        *dest = dc_colormap[dc_source[(frac >> FRACBITS) & 127]];
        dest += SCREENWIDTH;
        frac += fracstep;
    } while (count--);
}

static void R_DrawSpan(unsigned char *screen, int ds_y, int ds_x1, int ds_x2, fixed_t ds_xfrac, fixed_t ds_yfrac, fixed_t ds_xstep,
                       fixed_t ds_ystep, const unsigned char *ds_source, const unsigned char *ds_colormap)
{
    fixed_t xfrac = ds_xfrac, yfrac = ds_yfrac;
    unsigned char *dest = screen + ds_y * SCREENWIDTH + ds_x1;
    int count = ds_x2 - ds_x1, spot;
    do {
        spot = ((yfrac >> (16 - 6)) & (63 * 64)) + ((xfrac >> 16) & 63);
        *dest++ = ds_colormap[ds_source[spot]];
        xfrac += ds_xstep;
        yfrac += ds_ystep;
    } while (count--);
}

/* R_ScaleFromGlobalAngle */
static fixed_t scale_at(struct scratch *s, angle_t visangle, angle_t normalangle, fixed_t distance)
{
    angle_t anglea = ANG90 + (visangle - viewangle), angleb = ANG90 + (visangle - normalangle);
    int sinea = finesine[anglea >> ANGLETOFINESHIFT], sineb = finesine[angleb >> ANGLETOFINESHIFT];
    fixed_t num = FixedMul(projection, sineb), den = FixedMul(distance, sinea), scale;
    (void)s;
    if (den > num >> 16) {
        scale = FixedDiv(num, den);
        if (scale > 64 * FRACUNIT) scale = 64 * FRACUNIT;
        else if (scale < 256) scale = 256;
    } else scale = 64 * FRACUNIT;
    return scale;
}

/* a shortened R_StoreWallRange: a middle wall for a solid seg, the step for a window, columns per x */
static void R_StoreWallRange(struct scratch *s, int start, int stop)
{
    const seg_t *line = s->curline;
    const sector_t *front = s->frontsector, *back = line->backsector;
    angle_t normalangle = R_PointToAngle(line->v2.x, line->v2.y) - R_PointToAngle(line->v1.x, line->v1.y) + ANG90 + viewangle * 0;
    fixed_t dx = line->v1.x - viewx, dy = line->v1.y - viewy;
    fixed_t distance = abs(dx) / 2 + abs(dy) / 2 + 16 * FRACUNIT;
    angle_t a1 = viewangle + ((angle_t)(centerx - start) << 22), a2 = viewangle + ((angle_t)(centerx - stop) << 22);
    fixed_t scale1 = scale_at(s, a1, normalangle, distance), scale2 = scale_at(s, a2, normalangle, distance);
    fixed_t step = stop > start ? (scale2 - scale1) / (stop - start) : 0, scale = scale1;
    fixed_t top = front->ceilingheight - viewz, bottom = front->floorheight - viewz;
    fixed_t back_top = back ? back->ceilingheight - viewz : top, back_bottom = back ? back->floorheight - viewz : bottom;
    const unsigned char *texture = textures[line->midtexture & 7];
    const unsigned char *light = colormaps[(front->lightlevel >> 3) & 31];
    for (int x = start; x <= stop; x++, scale += step) {
        int yl = ((centery << FRACBITS) - FixedMul(top, scale)) >> FRACBITS, yh = ((centery << FRACBITS) - FixedMul(bottom, scale)) >> FRACBITS;
        if (yl < s->ceilingclip[x] + 1) yl = s->ceilingclip[x] + 1;
        if (yh > s->floorclip[x] - 1) yh = s->floorclip[x] - 1;
        fixed_t iscale = FixedDiv(FRACUNIT, scale | 1);
        int texcol = (x * 37 + (distance >> 18)) & 127;
        if (!back) {
            R_DrawColumn(s->screen, x, yl, yh, iscale, top, texture + texcol * 128, light);
            s->ceilingclip[x] = (short)SCREENHEIGHT, s->floorclip[x] = -1;
        } else {
            int mid_t = ((centery << FRACBITS) - FixedMul(back_top, scale)) >> FRACBITS;
            int mid_b = ((centery << FRACBITS) - FixedMul(back_bottom, scale)) >> FRACBITS;
            if (mid_t > yl) { R_DrawColumn(s->screen, x, yl, mid_t < yh ? mid_t : yh, iscale, top, texture + texcol * 128, light); yl = mid_t; }
            if (mid_b < yh) { R_DrawColumn(s->screen, x, mid_b > yl ? mid_b : yl, yh, iscale, back_bottom, texture + texcol * 128, light); yh = mid_b; }
            if (yl - 1 > s->ceilingclip[x]) s->ceilingclip[x] = (short)(yl - 1);
            if (yh + 1 < s->floorclip[x]) s->floorclip[x] = (short)(yh + 1);
        }
    }
}

static void R_ClipSolidWallSegment(struct scratch *s, int first, int last)
{
    cliprange_t *next, *start = s->solidsegs;
    while (start->last < first - 1) start++;
    if (first < start->first) {
        if (last < start->first - 1) {
            R_StoreWallRange(s, first, last);
            next = s->newend;
            s->newend++;
            while (next != start) { *next = *(next - 1); next--; }
            next->first = first, next->last = last;
            return;
        }
        R_StoreWallRange(s, first, start->first - 1);
        start->first = first;
    }
    if (last <= start->last) return;
    next = start;
    while (last >= (next + 1)->first - 1) {
        R_StoreWallRange(s, next->last + 1, (next + 1)->first - 1);
        next++;
        if (last <= next->last) { start->last = next->last; goto crunch; }
    }
    R_StoreWallRange(s, next->last + 1, last);
    start->last = last;
crunch:
    if (next == start) return;
    while (next++ != s->newend) *++start = *next;
    s->newend = start + 1;
}

static void R_ClipPassWallSegment(struct scratch *s, int first, int last)
{
    cliprange_t *start = s->solidsegs;
    while (start->last < first - 1) start++;
    if (first < start->first) {
        if (last < start->first - 1) { R_StoreWallRange(s, first, last); return; }
        R_StoreWallRange(s, first, start->first - 1);
    }
    if (last <= start->last) return;
    while (last >= (start + 1)->first - 1) {
        R_StoreWallRange(s, start->last + 1, (start + 1)->first - 1);
        start++;
        if (last <= start->last) return;
    }
    R_StoreWallRange(s, start->last + 1, last);
}

static void R_AddLine(struct scratch *s, const seg_t *line)
{
    int x1, x2;
    angle_t angle1, angle2, span, tspan;
    const sector_t *backsector, *frontsector = s->frontsector;
    s->curline = line;
    angle1 = R_PointToAngle(line->v1.x, line->v1.y);
    angle2 = R_PointToAngle(line->v2.x, line->v2.y);
    span = angle1 - angle2;
    if (span >= ANG180) return;
    s->rw_angle1 = angle1;
    angle1 -= viewangle, angle2 -= viewangle;
    tspan = angle1 + clipangle;
    if (tspan > 2 * clipangle) {
        tspan -= 2 * clipangle;
        if (tspan >= span) return;
        angle1 = clipangle;
    }
    tspan = clipangle - angle2;
    if (tspan > 2 * clipangle) {
        tspan -= 2 * clipangle;
        if (tspan >= span) return;
        angle2 = -clipangle;
    }
    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;
    x1 = viewangletox[angle1], x2 = viewangletox[angle2];
    if (x1 == x2) return;
    backsector = line->backsector;
    if (!backsector) goto clipsolid;
    if (backsector->ceilingheight <= frontsector->floorheight || backsector->floorheight >= frontsector->ceilingheight) goto clipsolid;
    if (backsector->ceilingheight != frontsector->ceilingheight || backsector->floorheight != frontsector->floorheight) goto clippass;
    if (backsector->ceilingpic == frontsector->ceilingpic && backsector->floorpic == frontsector->floorpic &&
        backsector->lightlevel == frontsector->lightlevel && line->midtexture == 0)
        return;
clippass:
    R_ClipPassWallSegment(s, x1, x2 - 1);
    return;
clipsolid:
    R_ClipSolidWallSegment(s, x1, x2 - 1);
}

static const int checkcoord[12][4] = {{3, 0, 2, 1}, {3, 0, 2, 0}, {3, 1, 2, 0}, {0}, {2, 0, 2, 1}, {0, 0, 0, 0},
                                      {3, 1, 3, 0}, {0}, {2, 0, 3, 1}, {2, 1, 3, 1}, {2, 1, 3, 0}};

static int R_CheckBBox(struct scratch *s, const fixed_t *bspcoord)
{
    int boxx, boxy, boxpos, sx1, sx2;
    fixed_t x1, y1, x2, y2;
    angle_t angle1, angle2, span, tspan;
    cliprange_t *start;
    boxx = viewx <= bspcoord[BOXLEFT] ? 0 : viewx < bspcoord[BOXRIGHT] ? 1 : 2;
    boxy = viewy >= bspcoord[BOXTOP] ? 0 : viewy > bspcoord[BOXBOTTOM] ? 1 : 2;
    boxpos = (boxy << 2) + boxx;
    if (boxpos == 5) return 1;
    x1 = bspcoord[checkcoord[boxpos][0]], y1 = bspcoord[checkcoord[boxpos][1]];
    x2 = bspcoord[checkcoord[boxpos][2]], y2 = bspcoord[checkcoord[boxpos][3]];
    angle1 = R_PointToAngle(x1, y1) - viewangle;
    angle2 = R_PointToAngle(x2, y2) - viewangle;
    span = angle1 - angle2;
    if (span >= ANG180) return 1;
    tspan = angle1 + clipangle;
    if (tspan > 2 * clipangle) {
        tspan -= 2 * clipangle;
        if (tspan >= span) return 0;
        angle1 = clipangle;
    }
    tspan = clipangle - angle2;
    if (tspan > 2 * clipangle) {
        tspan -= 2 * clipangle;
        if (tspan >= span) return 0;
        angle2 = -clipangle;
    }
    angle1 = (angle1 + ANG90) >> ANGLETOFINESHIFT;
    angle2 = (angle2 + ANG90) >> ANGLETOFINESHIFT;
    sx1 = viewangletox[angle1], sx2 = viewangletox[angle2];
    if (sx1 == sx2) return 0;
    sx2--;
    start = s->solidsegs;
    while (start->last < sx2) start++;
    if (sx1 >= start->first && sx2 <= start->last) return 0;
    return 1;
}

static void R_Subsector(struct scratch *s, int num)
{
    const subsector_t *sub = &subsectors[num];
    for (int i = 0; i < sub->numlines; i++) {
        s->frontsector = segs[sub->firstline + i].frontsector;
        R_AddLine(s, &segs[sub->firstline + i]);
    }
}

static void R_RenderBSPNode(struct scratch *s, int bspnum)
{
    const node_t *bsp;
    int side;
    if (bspnum & NF_SUBSECTOR) { R_Subsector(s, bspnum & ~NF_SUBSECTOR); return; }
    bsp = &nodes[bspnum];
    side = R_PointOnSide(viewx, viewy, bsp);
    R_RenderBSPNode(s, bsp->children[side]);
    if (R_CheckBBox(s, bsp->bbox[side ^ 1])) R_RenderBSPNode(s, bsp->children[side ^ 1]);
}

/* the tree over cells [x0, x1) x [y0, y1): a subsector a cell */
static int build(int x0, int y0, int x1, int y1, fixed_t *box)
{
    box[BOXLEFT] = x0 * CELL, box[BOXRIGHT] = x1 * CELL, box[BOXBOTTOM] = y0 * CELL, box[BOXTOP] = y1 * CELL;
    if (x1 - x0 == 1 && y1 - y0 == 1) {
        int n = numsubsectors++, c = y0 * CELLS + x0;
        subsectors[n].firstline = numsegs;
        if (!solid[y0][x0]) {
            static const int ex[4][4] = {{0, 0, 1, 0}, {1, 0, 1, 1}, {1, 1, 0, 1}, {0, 1, 0, 0}}, nb[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
            for (int e = 0; e < 4; e++) {   /* the cell's four edges, clockwise seen from inside */
                int nx = x0 + nb[e][0], ny = y0 + nb[e][1];
                seg_t *g = &segs[numsegs++];
                g->v1.x = (x0 + ex[e][0]) * CELL, g->v1.y = (y0 + ex[e][1]) * CELL;
                g->v2.x = (x0 + ex[e][2]) * CELL, g->v2.y = (y0 + ex[e][3]) * CELL;
                g->frontsector = &sectors[c];
                g->backsector = nx < 0 || ny < 0 || nx >= CELLS || ny >= CELLS || solid[ny][nx] ? 0 : &sectors[ny * CELLS + nx];
                g->midtexture = g->backsector ? 0 : 1 + (c + e) % 7;
            }
        }
        subsectors[n].numlines = numsegs - subsectors[n].firstline;
        return n | NF_SUBSECTOR;
    }
    int n = numnodes++;
    node_t *node = &nodes[n];
    if (x1 - x0 >= y1 - y0) {
        int mid = (x0 + x1) / 2;
        node->x = mid * CELL, node->y = 0, node->dx = 0, node->dy = CELL;   /* front (0) is x > mid */
        node->children[0] = (unsigned short)build(mid, y0, x1, y1, node->bbox[0]);
        node->children[1] = (unsigned short)build(x0, y0, mid, y1, node->bbox[1]);
    } else {
        int mid = (y0 + y1) / 2;
        node->x = 0, node->y = mid * CELL, node->dx = -CELL, node->dy = 0;   /* front (0) is y > mid */
        node->children[0] = (unsigned short)build(x0, mid, x1, y1, node->bbox[0]);
        node->children[1] = (unsigned short)build(x0, y0, x1, mid, node->bbox[1]);
    }
    return n;
}

static void setup(void)
{
    uint32_t seed = 0xd00d;
    for (int i = 0; i <= SLOPERANGE; i++) tantoangle[i] = (angle_t)(atanf((float)i / SLOPERANGE) / 6.2831853f * 4294967296.0f);
    for (int i = 0; i < FINEANGLES + FINEANGLES / 4; i++) finesine[i] = (fixed_t)(sinf((i + 0.5f) * 6.2831853f / FINEANGLES) * FRACUNIT);
    /* R_InitTextureMapping: a 90 degree view */
    fixed_t focal = centerxfrac;
    for (int i = 0; i < FINEANGLES / 2; i++) {
        float tangent = tanf(((float)i - FINEANGLES / 4 + 0.5f) * 6.2831853f / FINEANGLES);
        int t;
        if (tangent > 2) t = -1;
        else if (tangent < -2) t = SCREENWIDTH + 1;
        else {
            t = (centerxfrac - (fixed_t)(tangent * focal) + FRACUNIT - 1) >> FRACBITS;
            if (t < -1) t = -1;
            else if (t > SCREENWIDTH + 1) t = SCREENWIDTH + 1;
        }
        viewangletox[i] = t;
    }
    for (int i = 0; i < FINEANGLES / 2; i++) viewangletox[i] = viewangletox[i] == -1 ? 0 : viewangletox[i] == SCREENWIDTH + 1 ? SCREENWIDTH : viewangletox[i];
    clipangle = ANG90 / 2;
    for (int x = 0; x < SCREENWIDTH; x++) distscale[x] = FRACUNIT + abs(x - centerx) * 300;
    for (int y = 0; y < SCREENHEIGHT; y++) yslope[y] = FixedDiv(SCREENWIDTH / 2 * FRACUNIT, abs(((y - centery) << FRACBITS) + FRACUNIT / 2));
    for (int y = 0; y < CELLS; y++)
        for (int x = 0; x < CELLS; x++) {
            solid[y][x] = (wl_rand(&seed) % 5 == 0) && !(x == 16 && y == 16);
            sector_t *s = &sectors[y * CELLS + x];
            s->floorheight = (int)(wl_rand(&seed) % 4) * 16 * FRACUNIT;
            s->ceilingheight = s->floorheight + (96 + (int)(wl_rand(&seed) % 4) * 32) * FRACUNIT;
            s->floorpic = (short)(wl_rand(&seed) % 4), s->ceilingpic = (short)(wl_rand(&seed) % 4), s->lightlevel = (short)(120 + wl_rand(&seed) % 136);
        }
    for (int t = 0; t < 8; t++)
        for (int i = 0; i < 128 * 128; i++) textures[t][i] = (unsigned char)wl_rand(&seed);
    for (int f = 0; f < 4; f++)
        for (int i = 0; i < 64 * 64; i++) flats[f][i] = (unsigned char)wl_rand(&seed);
    for (int l = 0; l < 32; l++)
        for (int c = 0; c < 256; c++) colormaps[l][c] = (unsigned char)(c * (32 - l) / 32);
    numnodes = numsubsectors = numsegs = 0;
    fixed_t box[4];
    build(0, 0, CELLS, CELLS, box);
    viewx = 16 * CELL + CELL / 2 + 1234, viewy = 16 * CELL + CELL / 3, viewz = sectors[16 * CELLS + 16].floorheight + 41 * FRACUNIT;
    viewangle = 0x2c000000u;
}

static void *inst(void) { return wl_alloc(sizeof(struct scratch)); }

static uint32_t run(void *ctx)
{
    struct scratch *s = ctx;
    uint32_t sum = 0;
    /* floor and ceiling: a span a row (R_MapPlane's distance and steps), then the walls over them */
    angle_t angle = (viewangle - ANG90) >> ANGLETOFINESHIFT;
    for (int y = 0; y < SCREENHEIGHT; y++) {
        if (y == centery) continue;
        fixed_t planeheight = y > centery ? 41 * FRACUNIT : 70 * FRACUNIT;
        fixed_t distance = FixedMul(planeheight, yslope[y]);
        fixed_t xstep = FixedMul(distance, finesine[(viewangle - ANG90) >> ANGLETOFINESHIFT]) / 160;
        fixed_t ystep = -FixedMul(distance, finecosine[(viewangle - ANG90) >> ANGLETOFINESHIFT]) / 160;
        fixed_t length = FixedMul(distance, distscale[0]);
        fixed_t xfrac = viewx + FixedMul(finecosine[angle], length), yfrac = -viewy - FixedMul(finesine[angle], length);
        int level = distance >> 20 > 31 ? 31 : distance >> 20;
        R_DrawSpan(s->screen, y, 0, SCREENWIDTH - 1, xfrac, yfrac, xstep, ystep, flats[y & 3], colormaps[level]);
    }
    s->solidsegs[0].first = -0x7fffffff, s->solidsegs[0].last = -1;   /* R_ClearClipSegs */
    s->solidsegs[1].first = SCREENWIDTH, s->solidsegs[1].last = 0x7fffffff;
    s->newend = s->solidsegs + 2;
    for (int x = 0; x < SCREENWIDTH; x++) s->floorclip[x] = SCREENHEIGHT, s->ceilingclip[x] = -1;
    R_RenderBSPNode(s, 0);
    for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; i += 61) sum = sum * 31 + s->screen[i];
    return sum + (uint32_t)(s->newend - s->solidsegs);
}

const wl_kernel wl_doom_render = {"doom_render", "Doom: R_RenderBSPNode, R_AddLine, clip lists, R_DrawColumn, R_DrawSpan", setup, inst, run};
