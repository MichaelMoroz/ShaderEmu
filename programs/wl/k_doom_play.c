/*
 * doom_play: Doom's game tic for its monsters (linuxdoom p_map.c, p_maputl.c, p_enemy.c, copied in fixed point):
 * P_Move's step in one of eight directions through P_TryMove, P_CheckPosition over the blockmap (P_BlockThingsIterator
 * with PIT_CheckThing, P_BlockLinesIterator with PIT_CheckLine: the line's box, P_BoxOnLineSide, P_PointOnLineSide,
 * P_LineOpening), the step and drop-off tests, P_UnsetThingPosition and P_SetThingPosition relinking the thing into
 * its block, and P_NewChaseDir's turn when blocked. R_PointInSubsector is a lookup in the map's grid of sectors.
 * The map: 16 x 16 rooms of 256 units (floors 0 to 48 apart, some walls one-sided or blocking), a diagonal wall
 * in a fifth of them, a blockmap of 128 units. A unit: 150 monsters chasing a point for 4 tics.
 */
#include <stdlib.h>
#include <string.h>
#include "wl.h"

#define FRACBITS 16
#define FRACUNIT (1 << FRACBITS)
#define MAPBLOCKSHIFT (FRACBITS + 7)
#define MAXRADIUS (32 * FRACUNIT)
#define GRID 16
#define ROOM (256 * FRACUNIT)
#define BMAP (GRID * 2)
#define MAXLINES 1200
#define THINGS 150
#define TICS 4
#define ML_BLOCKING 1
#define MF_SOLID 2
#define MF_SHOOTABLE 4
enum { BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT };
enum { ST_HORIZONTAL, ST_VERTICAL, ST_POSITIVE, ST_NEGATIVE };
enum { DI_EAST, DI_NORTHEAST, DI_NORTH, DI_NORTHWEST, DI_WEST, DI_SOUTHWEST, DI_SOUTH, DI_SOUTHEAST, DI_NODIR };
typedef int fixed_t;

typedef struct { fixed_t floorheight, ceilingheight; } sector_t;
typedef struct { fixed_t x, y; } vertex_t;
typedef struct {
    vertex_t v1, v2;
    fixed_t dx, dy, bbox[4];
    int flags, slopetype, twosided;
    sector_t *frontsector, *backsector;
} line_t;
typedef struct mobj_s {
    fixed_t x, y, z, floorz, ceilingz, radius, height, speed;
    int flags, movedir, movecount, bnext, bprev, block;
} mobj_t;

static sector_t sectors[GRID * GRID];
static line_t lines[MAXLINES];
static int numlines;
static short blockmap_offsets[BMAP * BMAP], blockmaplump[MAXLINES * 8 + BMAP * BMAP];
static mobj_t start_things[THINGS];
static const fixed_t xspeed[8] = {FRACUNIT, 47000, 0, -47000, -FRACUNIT, -47000, 0, 47000};
static const fixed_t yspeed[8] = {0, 47000, FRACUNIT, 47000, 0, -47000, -FRACUNIT, -47000};

struct play {
    mobj_t things[THINGS];
    int blocklinks[BMAP * BMAP], validcount, linevalid[MAXLINES];
    fixed_t tmbbox[4], tmx, tmy, tmfloorz, tmceilingz, tmdropoffz, opentop, openbottom, lowfloor;
    mobj_t *tmthing;
    uint32_t rnd;
};

static fixed_t FixedMul(fixed_t a, fixed_t b) { return (fixed_t)(((long long)a * (long long)b) >> FRACBITS); }

static sector_t *R_PointInSector(fixed_t x, fixed_t y)
{
    int cx = x / ROOM, cy = y / ROOM;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx >= GRID) cx = GRID - 1;
    if (cy >= GRID) cy = GRID - 1;
    return &sectors[cy * GRID + cx];
}

static int P_PointOnLineSide(fixed_t x, fixed_t y, const line_t *line)
{
    fixed_t dx, dy, left, right;
    if (!line->dx) {
        if (x <= line->v1.x) return line->dy > 0;
        return line->dy < 0;
    }
    if (!line->dy) {
        if (y <= line->v1.y) return line->dx < 0;
        return line->dx > 0;
    }
    dx = (x - line->v1.x);
    dy = (y - line->v1.y);
    left = FixedMul(line->dy >> FRACBITS, dx);
    right = FixedMul(dy, line->dx >> FRACBITS);
    if (right < left) return 0;
    return 1;
}

static int P_BoxOnLineSide(const fixed_t *tmbox, const line_t *ld)
{
    int p1 = 0, p2 = 0;
    switch (ld->slopetype) {
    case ST_HORIZONTAL:
        p1 = tmbox[BOXTOP] > ld->v1.y;
        p2 = tmbox[BOXBOTTOM] > ld->v1.y;
        if (ld->dx < 0) p1 ^= 1, p2 ^= 1;
        break;
    case ST_VERTICAL:
        p1 = tmbox[BOXRIGHT] < ld->v1.x;
        p2 = tmbox[BOXLEFT] < ld->v1.x;
        if (ld->dy < 0) p1 ^= 1, p2 ^= 1;
        break;
    case ST_POSITIVE:
        p1 = P_PointOnLineSide(tmbox[BOXLEFT], tmbox[BOXTOP], ld);
        p2 = P_PointOnLineSide(tmbox[BOXRIGHT], tmbox[BOXBOTTOM], ld);
        break;
    case ST_NEGATIVE:
        p1 = P_PointOnLineSide(tmbox[BOXRIGHT], tmbox[BOXTOP], ld);
        p2 = P_PointOnLineSide(tmbox[BOXLEFT], tmbox[BOXBOTTOM], ld);
        break;
    }
    if (p1 == p2) return p1;
    return -1;
}

static void P_LineOpening(struct play *p, const line_t *linedef)
{
    const sector_t *front = linedef->frontsector, *back = linedef->backsector;
    p->opentop = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
    if (front->floorheight > back->floorheight) p->openbottom = front->floorheight, p->lowfloor = back->floorheight;
    else p->openbottom = back->floorheight, p->lowfloor = front->floorheight;
}

static int PIT_CheckLine(struct play *p, const line_t *ld)
{
    if (p->tmbbox[BOXRIGHT] <= ld->bbox[BOXLEFT] || p->tmbbox[BOXLEFT] >= ld->bbox[BOXRIGHT] || p->tmbbox[BOXTOP] <= ld->bbox[BOXBOTTOM] ||
        p->tmbbox[BOXBOTTOM] >= ld->bbox[BOXTOP])
        return 1;
    if (P_BoxOnLineSide(p->tmbbox, ld) != -1) return 1;
    if (!ld->twosided) return 0;
    if (ld->flags & ML_BLOCKING) return 0;
    P_LineOpening(p, ld);
    if (p->opentop < p->tmceilingz) p->tmceilingz = p->opentop;
    if (p->openbottom > p->tmfloorz) p->tmfloorz = p->openbottom;
    if (p->lowfloor < p->tmdropoffz) p->tmdropoffz = p->lowfloor;
    return 1;
}

static int PIT_CheckThing(struct play *p, mobj_t *thing)
{
    fixed_t blockdist;
    if (!(thing->flags & (MF_SOLID | MF_SHOOTABLE))) return 1;
    blockdist = thing->radius + p->tmthing->radius;
    if (abs(thing->x - p->tmx) >= blockdist || abs(thing->y - p->tmy) >= blockdist) return 1;
    if (thing == p->tmthing) return 1;
    return !(thing->flags & MF_SOLID);
}

static int P_BlockLinesIterator(struct play *p, int x, int y)
{
    if (x < 0 || y < 0 || x >= BMAP || y >= BMAP) return 1;
    for (const short *list = blockmaplump + blockmap_offsets[y * BMAP + x]; *list != -1; list++) {
        const line_t *ld = &lines[*list];
        if (p->linevalid[*list] == p->validcount) continue;
        p->linevalid[*list] = p->validcount;
        if (!PIT_CheckLine(p, ld)) return 0;
    }
    return 1;
}

static int P_BlockThingsIterator(struct play *p, int x, int y)
{
    if (x < 0 || y < 0 || x >= BMAP || y >= BMAP) return 1;
    for (int t = p->blocklinks[y * BMAP + x]; t >= 0; t = p->things[t].bnext)
        if (!PIT_CheckThing(p, &p->things[t])) return 0;
    return 1;
}

static int P_CheckPosition(struct play *p, mobj_t *thing, fixed_t x, fixed_t y)
{
    int xl, xh, yl, yh, bx, by;
    sector_t *newsec;
    p->tmthing = thing, p->tmx = x, p->tmy = y;
    p->tmbbox[BOXTOP] = y + thing->radius, p->tmbbox[BOXBOTTOM] = y - thing->radius;
    p->tmbbox[BOXRIGHT] = x + thing->radius, p->tmbbox[BOXLEFT] = x - thing->radius;
    newsec = R_PointInSector(x, y);
    p->tmfloorz = p->tmdropoffz = newsec->floorheight;
    p->tmceilingz = newsec->ceilingheight;
    p->validcount++;
    xl = (p->tmbbox[BOXLEFT] - MAXRADIUS) >> MAPBLOCKSHIFT, xh = (p->tmbbox[BOXRIGHT] + MAXRADIUS) >> MAPBLOCKSHIFT;
    yl = (p->tmbbox[BOXBOTTOM] - MAXRADIUS) >> MAPBLOCKSHIFT, yh = (p->tmbbox[BOXTOP] + MAXRADIUS) >> MAPBLOCKSHIFT;
    for (bx = xl; bx <= xh; bx++)
        for (by = yl; by <= yh; by++)
            if (!P_BlockThingsIterator(p, bx, by)) return 0;
    xl = p->tmbbox[BOXLEFT] >> MAPBLOCKSHIFT, xh = p->tmbbox[BOXRIGHT] >> MAPBLOCKSHIFT;
    yl = p->tmbbox[BOXBOTTOM] >> MAPBLOCKSHIFT, yh = p->tmbbox[BOXTOP] >> MAPBLOCKSHIFT;
    for (bx = xl; bx <= xh; bx++)
        for (by = yl; by <= yh; by++)
            if (!P_BlockLinesIterator(p, bx, by)) return 0;
    return 1;
}

static void P_UnsetThingPosition(struct play *p, mobj_t *thing)
{
    if (thing->bnext >= 0) p->things[thing->bnext].bprev = thing->bprev;
    if (thing->bprev >= 0) p->things[thing->bprev].bnext = thing->bnext;
    else if (thing->block >= 0) p->blocklinks[thing->block] = thing->bnext;
}

static void P_SetThingPosition(struct play *p, mobj_t *thing)
{
    int blockx = thing->x >> MAPBLOCKSHIFT, blocky = thing->y >> MAPBLOCKSHIFT, me = (int)(thing - p->things);
    if (blockx >= 0 && blockx < BMAP && blocky >= 0 && blocky < BMAP) {
        thing->block = blocky * BMAP + blockx;
        thing->bprev = -1;
        thing->bnext = p->blocklinks[thing->block];
        if (thing->bnext >= 0) p->things[thing->bnext].bprev = me;
        p->blocklinks[thing->block] = me;
    } else thing->block = -1, thing->bnext = thing->bprev = -1;
}

static int P_TryMove(struct play *p, mobj_t *thing, fixed_t x, fixed_t y)
{
    if (!P_CheckPosition(p, thing, x, y)) return 0;
    if (p->tmceilingz - p->tmfloorz < thing->height) return 0;   // doesn't fit
    if (p->tmceilingz - thing->z < thing->height) return 0;       // mobj must lower itself to fit
    if (p->tmfloorz - thing->z > 24 * FRACUNIT) return 0;          // too big a step up
    if (p->tmfloorz - p->tmdropoffz > 24 * FRACUNIT) return 0;     // don't stand over a dropoff
    P_UnsetThingPosition(p, thing);
    thing->floorz = p->tmfloorz, thing->ceilingz = p->tmceilingz;
    thing->x = x, thing->y = y;
    thing->z = thing->floorz;
    P_SetThingPosition(p, thing);
    return 1;
}

static int P_Move(struct play *p, mobj_t *actor)
{
    if (actor->movedir == DI_NODIR) return 0;
    fixed_t tryx = actor->x + actor->speed * xspeed[actor->movedir], tryy = actor->y + actor->speed * yspeed[actor->movedir];
    return P_TryMove(p, actor, tryx, tryy);
}

/* P_NewChaseDir: toward the target, then the other axis, then the old way, then any other */
static void P_NewChaseDir(struct play *p, mobj_t *actor, fixed_t tx, fixed_t ty)
{
    static const int opposite[] = {DI_WEST, DI_SOUTHWEST, DI_SOUTH, DI_SOUTHEAST, DI_EAST, DI_NORTHEAST, DI_NORTH, DI_NORTHWEST, DI_NODIR};
    static const int diags[] = {DI_NORTHWEST, DI_NORTHEAST, DI_SOUTHWEST, DI_SOUTHEAST};
    fixed_t deltax = tx - actor->x, deltay = ty - actor->y;
    int d[3], olddir = actor->movedir, turnaround = opposite[olddir], tdir;
    d[1] = deltax > 10 * FRACUNIT ? DI_EAST : deltax < -10 * FRACUNIT ? DI_WEST : DI_NODIR;
    d[2] = deltay < -10 * FRACUNIT ? DI_SOUTH : deltay > 10 * FRACUNIT ? DI_NORTH : DI_NODIR;
    if (d[1] != DI_NODIR && d[2] != DI_NODIR) {
        actor->movedir = diags[((deltay < 0) << 1) + (deltax > 0)];
        if (actor->movedir != turnaround && P_Move(p, actor)) return;
    }
    if (((p->rnd = p->rnd * 1103515245 + 12345) >> 16 & 255) > 200 || abs(deltay) > abs(deltax)) tdir = d[1], d[1] = d[2], d[2] = tdir;
    if (d[1] == turnaround) d[1] = DI_NODIR;
    if (d[2] == turnaround) d[2] = DI_NODIR;
    if (d[1] != DI_NODIR && (actor->movedir = d[1], P_Move(p, actor))) return;
    if (d[2] != DI_NODIR && (actor->movedir = d[2], P_Move(p, actor))) return;
    if (olddir != DI_NODIR && (actor->movedir = olddir, P_Move(p, actor))) return;
    for (tdir = DI_EAST; tdir <= DI_SOUTHEAST; tdir++)
        if (tdir != turnaround && (actor->movedir = tdir, P_Move(p, actor))) return;
    if (turnaround != DI_NODIR && (actor->movedir = turnaround, P_Move(p, actor))) return;
    actor->movedir = DI_NODIR;
}

static void add_line(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2, sector_t *front, sector_t *back, int flags)
{
    line_t *l = &lines[numlines++];
    l->v1.x = x1, l->v1.y = y1, l->v2.x = x2, l->v2.y = y2, l->dx = x2 - x1, l->dy = y2 - y1;
    l->frontsector = front, l->backsector = back ? back : front, l->twosided = back != 0, l->flags = flags;
    l->bbox[BOXLEFT] = x1 < x2 ? x1 : x2, l->bbox[BOXRIGHT] = x1 < x2 ? x2 : x1;
    l->bbox[BOXBOTTOM] = y1 < y2 ? y1 : y2, l->bbox[BOXTOP] = y1 < y2 ? y2 : y1;
    l->slopetype = !l->dx ? ST_VERTICAL : !l->dy ? ST_HORIZONTAL : ((l->dy > 0) == (l->dx > 0)) ? ST_POSITIVE : ST_NEGATIVE;
}

static void setup(void)
{
    uint32_t seed = 0xd0e1;
    for (int i = 0; i < GRID * GRID; i++) {
        sectors[i].floorheight = (fixed_t)(wl_rand(&seed) % 4) * 16 * FRACUNIT;
        sectors[i].ceilingheight = sectors[i].floorheight + (96 + (fixed_t)(wl_rand(&seed) % 3) * 32) * FRACUNIT;
    }
    for (int cy = 0; cy < GRID; cy++)
        for (int cx = 0; cx < GRID; cx++) {
            sector_t *s = &sectors[cy * GRID + cx];
            fixed_t x = cx * ROOM, y = cy * ROOM;
            /* the room's west and south walls; the map's edge one-sided */
            uint32_t r = wl_rand(&seed);
            add_line(x, y, x, y + ROOM, s, cx ? &sectors[cy * GRID + cx - 1] : 0, (r & 7) == 0 ? ML_BLOCKING : 0);
            add_line(x + ROOM, y, x, y, s, cy ? &sectors[(cy - 1) * GRID + cx] : 0, (r >> 3 & 7) == 0 ? ML_BLOCKING : 0);
            if (cx == GRID - 1) add_line(x + ROOM, y + ROOM, x + ROOM, y, s, 0, 0);
            if (cy == GRID - 1) add_line(x, y + ROOM, x + ROOM, y + ROOM, s, 0, 0);
            if (r % 5 == 0) add_line(x + 64 * FRACUNIT, y + 64 * FRACUNIT, x + 160 * FRACUNIT, y + (r & 64 ? 180 : 100) * FRACUNIT, s, 0, 0);
            for (int k = 0; k < 2; k++)   /* a few short columns */
                if ((r >> (10 + k)) & 1) {
                    fixed_t px = x + (80 + k * 96) * FRACUNIT, py = y + 160 * FRACUNIT;
                    add_line(px, py, px + 16 * FRACUNIT, py, s, 0, 0);
                }
        }
    /* the blockmap: each block lists every line whose box touches it */
    int at = 0;
    for (int by = 0; by < BMAP; by++)
        for (int bx = 0; bx < BMAP; bx++) {
            fixed_t x0 = bx << MAPBLOCKSHIFT, y0 = by << MAPBLOCKSHIFT, x1 = x0 + (1 << MAPBLOCKSHIFT), y1 = y0 + (1 << MAPBLOCKSHIFT);
            blockmap_offsets[by * BMAP + bx] = (short)at;
            for (int i = 0; i < numlines; i++)
                if (lines[i].bbox[BOXRIGHT] >= x0 && lines[i].bbox[BOXLEFT] <= x1 && lines[i].bbox[BOXTOP] >= y0 && lines[i].bbox[BOXBOTTOM] <= y1)
                    blockmaplump[at++] = (short)i;
            blockmaplump[at++] = -1;
        }
    for (int t = 0; t < THINGS; t++) {
        mobj_t *m = &start_things[t];
        m->x = (fixed_t)(32 + wl_rand(&seed) % (GRID * 256 - 64)) * FRACUNIT, m->y = (fixed_t)(32 + wl_rand(&seed) % (GRID * 256 - 64)) * FRACUNIT;
        m->radius = (t % 3 ? 20 : 30) * FRACUNIT, m->height = 56 * FRACUNIT, m->speed = 8 + t % 4;
        m->flags = MF_SOLID | MF_SHOOTABLE, m->movedir = t % 8, m->z = m->floorz = R_PointInSector(m->x, m->y)->floorheight;
    }
}

static void *inst(void) { return wl_alloc(sizeof(struct play)); }

static uint32_t run(void *ctx)
{
    struct play *p = ctx;
    uint32_t sum = 0;
    memcpy(p->things, start_things, sizeof p->things);
    for (int i = 0; i < BMAP * BMAP; i++) p->blocklinks[i] = -1;
    for (int t = 0; t < THINGS; t++) P_SetThingPosition(p, &p->things[t]);
    p->rnd = 1;
    for (int tic = 0; tic < TICS; tic++) {
        fixed_t tx = (GRID * 128 + tic * 64) * FRACUNIT, ty = (GRID * 128 - tic * 32) * FRACUNIT;   /* the player they chase */
        for (int t = 0; t < THINGS; t++) {   /* A_Chase: keep the way while it goes, else a new one */
            mobj_t *actor = &p->things[t];
            if (--actor->movecount < 0 || !P_Move(p, actor)) {
                P_NewChaseDir(p, actor, tx, ty);
                actor->movecount = (int)((p->rnd >> 16) & 15);
            }
        }
    }
    for (int t = 0; t < THINGS; t++) sum = sum * 31 + (uint32_t)(p->things[t].x >> 12) + (uint32_t)(p->things[t].y >> 12) * 3 + (uint32_t)p->things[t].movedir;
    return sum;
}

const wl_kernel wl_doom_play = {"doom_play", "Doom: monsters' P_Move, P_TryMove, P_CheckPosition over the blockmap", setup, inst, run};
