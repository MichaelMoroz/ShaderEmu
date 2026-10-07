/*
 * Parts of Doom's game code written again for this machine (docs/doom.md): the same answers
 * in fewer instructions, and without stores that fill the machine's write cache. A demo must
 * play the same: DOOM_HASH_TICS=N prints a sum of the game's state every N tics.
 *
 *   DOOM_PORT=sight,look,think   the port's own sight check, search for players, thinker
 *                                loop (any of them), for comparing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "doomstat.h"
#include "m_random.h"
#include "p_local.h"
#include "r_main.h"
#include "z_zone.h"
#include "r_state.h"

boolean P_CheckSight_port(mobj_t *t1, mobj_t *t2);
fixed_t P_InterceptVector2(divline_t *v2, divline_t *v1);
extern fixed_t sightzstart, topslope, bottomslope;
extern int sightcounts[2];
extern int rndindex, prndindex;

/*
 * A line that an iterator has visited is marked here, not in the line: Doom's lines are 64
 * bytes apart, and stores that far apart share a few sets of the write cache.
 */
int doom_line_mark[65536];
static int sight_mark[65536], sight_pass;

static int use_port_sight = -1, use_port_look, use_port_think;

boolean P_LookForPlayers_port(mobj_t *actor, boolean allaround);
void P_RunThinkers_port(void);

static void
which_port(void)
{
	const char *port = getenv("DOOM_PORT");

	use_port_sight = port && strstr(port, "sight");
	use_port_look = port && strstr(port, "look");
	use_port_think = port && strstr(port, "think");
}

struct trace { fixed_t x, y, dx, dy, x2, y2; };

/* Which side of a line a point is on: 0 front, 1 back, 2 on it. As the port's P_DivlineSide. */
static inline int
side_of(fixed_t x, fixed_t y, fixed_t nx, fixed_t ny, fixed_t ndx, fixed_t ndy)
{
	fixed_t left, right;

	if (!ndx) {
		if (x == nx)
			return 2;
		if (x <= nx)
			return ndy > 0;
		return ndy < 0;
	}
	if (!ndy) {
		if (x == ny)	/* the port compares x here, and so do we */
			return 2;
		if (y <= ny)
			return ndx < 0;
		return ndx > 0;
	}
	left = (ndy >> FRACBITS) * ((x - nx) >> FRACBITS);
	right = ((y - ny) >> FRACBITS) * (ndx >> FRACBITS);
	if (right < left)
		return 0;
	if (left == right)
		return 2;
	return 1;
}

/*
 * Whether the line from t1 to t2 crosses nothing that blocks sight. The port's walk over the
 * BSP tree, nearer side first, without its calls: the far sides wait on a stack of their own.
 */
static int
sight_walk(const struct trace *trace)
{
	const fixed_t tx = trace->x, ty = trace->y, tdx = trace->dx, tdy = trace->dy, x2 = trace->x2, y2 = trace->y2;
	const node_t *const all = nodes;
	const int pass = sight_pass;
	int waiting[128], depth = 0, bspnum = numnodes - 1;

	for (;;) {
		const subsector_t *sub;
		const seg_t *seg;
		int count;

		while (!(bspnum & NF_SUBSECTOR)) {
			const node_t *bsp = &all[bspnum];
			int side = side_of(tx, ty, bsp->x, bsp->y, bsp->dx, bsp->dy);

			if (side == 2)
				side = 0;	/* on the line: both sides are crossed */
			if (side != side_of(x2, y2, bsp->x, bsp->y, bsp->dx, bsp->dy)) {
				if (depth == 128)
					return -1;	/* a tree deeper than any we know: the port's way */
				waiting[depth++] = bsp->children[side ^ 1];
			}
			bspnum = bsp->children[side];
		}
		sub = &subsectors[bspnum == -1 ? 0 : bspnum & ~NF_SUBSECTOR];
		seg = &segs[sub->firstline];
		for (count = sub->numlines; count; seg++, count--) {
			const line_t *line = seg->linedef;
			int *mark = &sight_mark[line - lines];
			fixed_t x1, y1, ldx, ldy, opentop, openbottom, frac, slope;
			const sector_t *front, *back;
			divline_t divl, strace;

			if (*mark == pass)
				continue;	/* seen from its other side */
			*mark = pass;
			x1 = line->v1->x;
			y1 = line->v1->y;
			if (side_of(x1, y1, tx, ty, tdx, tdy) == side_of(line->v2->x, line->v2->y, tx, ty, tdx, tdy))
				continue;
			ldx = line->dx;
			ldy = line->dy;
			if (side_of(tx, ty, x1, y1, ldx, ldy) == side_of(x2, y2, x1, y1, ldx, ldy))
				continue;
			if (!(line->flags & ML_TWOSIDED))
				return 0;
			front = seg->frontsector;
			back = seg->backsector;
			if (front->floorheight == back->floorheight && front->ceilingheight == back->ceilingheight)
				continue;
			opentop = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
			openbottom = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
			if (openbottom >= opentop)
				return 0;	/* a closed door */
			divl.x = x1;
			divl.y = y1;
			divl.dx = ldx;
			divl.dy = ldy;
			strace.x = tx;
			strace.y = ty;
			strace.dx = tdx;
			strace.dy = tdy;
			frac = P_InterceptVector2(&strace, &divl);
			if (front->floorheight != back->floorheight) {
				slope = FixedDiv(openbottom - sightzstart, frac);
				if (slope > bottomslope)
					bottomslope = slope;
			}
			if (front->ceilingheight != back->ceilingheight) {
				slope = FixedDiv(opentop - sightzstart, frac);
				if (slope < topslope)
					topslope = slope;
			}
			if (topslope <= bottomslope)
				return 0;
		}
		if (!depth)
			return 1;
		bspnum = waiting[--depth];
	}
}

boolean
P_CheckSight(mobj_t *t1, mobj_t *t2)
{
	struct trace t;
	int pnum, seen;

	if (use_port_sight < 0)
		which_port();
	if (use_port_sight)
		return P_CheckSight_port(t1, t2);
	pnum = (t1->subsector->sector - sectors) * numsectors + (t2->subsector->sector - sectors);
	if (rejectmatrix[pnum >> 3] & (1 << (pnum & 7))) {
		sightcounts[0]++;
		return false;
	}
	sightcounts[1]++;
	validcount++;
	sight_pass++;
	sightzstart = t1->z + t1->height - (t1->height >> 2);
	topslope = (t2->z + t2->height) - sightzstart;
	bottomslope = t2->z - sightzstart;
	t.x = t1->x;
	t.y = t1->y;
	t.x2 = t2->x;
	t.y2 = t2->y;
	t.dx = t2->x - t1->x;
	t.dy = t2->y - t1->y;
	seen = sight_walk(&t);
	return seen < 0 ? P_CheckSight_port(t1, t2) : seen;
}

/*
 * A monster looks for a player to wake up for. As the port's, but whether the player is in
 * front of it is asked before whether it can be seen: the same answer, and most of the sight
 * checks of a level full of monsters facing a wall are never made.
 */
boolean
P_LookForPlayers(mobj_t *actor, boolean allaround)
{
	int c = 0, stop = (actor->lastlook - 1) & 3;

	if (use_port_sight < 0)
		which_port();
	if (use_port_look)
		return P_LookForPlayers_port(actor, allaround);
	for (;; actor->lastlook = (actor->lastlook + 1) & 3) {
		player_t *player;

		if (!playeringame[actor->lastlook])
			continue;
		if (c++ == 2 || actor->lastlook == stop)
			return false;
		player = &players[actor->lastlook];
		if (player->health <= 0)
			continue;
		if (!allaround) {
			angle_t an = R_PointToAngle2(actor->x, actor->y, player->mo->x, player->mo->y) - actor->angle;

			if (an > ANG90 && an < ANG270 &&
			    P_AproxDistance(player->mo->x - actor->x, player->mo->y - actor->y) > MELEERANGE)
				continue;	/* behind its back */
		}
		if (!P_CheckSight(actor, player->mo))
			continue;
		actor->target = player->mo;
		return true;
	}
}

/*
 * Every thinker's turn. As the port's, but a thing at rest in a state that never ends is
 * passed by here: its thinker would look at the same five fields and return.
 */
void
P_RunThinkers(void)
{
	thinker_t *th = thinkercap.next;

	if (use_port_sight < 0)
		which_port();
	if (use_port_think) {
		P_RunThinkers_port();
		return;
	}
	while (th != &thinkercap) {
		if (th->function.acv == (actionf_v)(-1)) {
			th->next->prev = th->prev;
			th->prev->next = th->next;
			Z_Free(th);
		} else if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
			mobj_t *m = (mobj_t *)th;

			if (m->tics != -1 || m->momx || m->momy || m->momz || m->z != m->floorz ||
			    (m->flags & MF_SKULLFLY) || ((m->flags & MF_COUNTKILL) && respawnmonsters))
				P_MobjThinker(m);
		} else if (th->function.acp1) {
			th->function.acp1(th);
		}
		th = th->next;
	}
}

/* A sum over what a demo must reproduce: the random numbers used and every thing's state. */
unsigned
doom_state_hash(void)
{
	unsigned h = 2166136261u;
	thinker_t *th;

#define MIX(v) (h = (h ^ (unsigned)(v)) * 16777619u)
	MIX(prndindex);
	MIX(rndindex);
	MIX(leveltime);
	for (th = thinkercap.next; th != &thinkercap; th = th->next) {
		const mobj_t *m = (const mobj_t *)th;

		if (th->function.acp1 != (actionf_p1)P_MobjThinker)
			continue;
		MIX(m->x), MIX(m->y), MIX(m->z), MIX(m->angle), MIX(m->momx), MIX(m->momy), MIX(m->momz);
		MIX(m->type), MIX(m->tics), MIX(m->health), MIX(m->flags), MIX(m->state - states);
	}
#undef MIX
	return h;
}
