/*
 * Parts of Doom's game code written again for this machine (docs/games/doom.md): the same answers
 * in fewer instructions, and without stores that fill the machine's write cache. A demo must
 * play the same: DOOM_HASH_TICS=N prints a sum of the game's state every N tics.
 *
 *   DOOM_PORT=sight,look,think,find,names,lines,group   the port's own code for any of
 *                                those (find: a point's subsector; names: of lumps and textures;
 *                                lines: what is in a blockmap cell; group: a sector's lines)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "doomdef.h"
#include "doomstat.h"
#include "m_bbox.h"
#include "m_random.h"
#include "m_swap.h"
#include "p_local.h"
#include "r_main.h"
#include "z_zone.h"
#include "r_state.h"
#include "w_wad.h"
#include "mcwx.h"

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
static int sight_mark[65536];

static int use_port_sight = -1, use_port_look, use_port_think, use_port_find, use_port_names, use_port_lines, use_port_group, use_port_rest;

boolean P_LookForPlayers_port(mobj_t *actor, boolean allaround);
void P_RunThinkers_port(void);

/* A thing's `validcount`, which the game has and never uses, is ours: bit 31 says it is asleep
 * (P_RunThinkers), the rest that it is among the changed: the view's number and where. */
#define ASLEEP		0x80000000u
static int stamps_stale = 1;		/* every thing's validcount is to be 0 again: another level, or the numbers went round */

static void
which_port(void)
{
	const char *port = getenv("DOOM_PORT");

	use_port_sight = port && strstr(port, "sight");
	use_port_look = port && strstr(port, "look");
	use_port_think = port && strstr(port, "think");
	use_port_find = port && strstr(port, "find");
	use_port_names = port && strstr(port, "names");
	use_port_lines = port && strstr(port, "lines");
	use_port_group = port && strstr(port, "group");
	use_port_rest = port && strstr(port, "rest");
}

struct trace { fixed_t x, y, dx, dy, x2, y2; };
/* A walk's marks of the lines it has seen (one number a walk) and its slopes: a core's own. */
struct sight { int *mark, pass; fixed_t zstart, top, bottom; short *heights, count; };
#define HEIGHTS 7		/* the sectors a walk ahead notes: more, and any move makes it stale */

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

/* A walk ahead notes the sectors whose heights decided it (HEIGHTS + 1: too many to note). */
static void
sight_heights(struct sight *c, int front, int back)
{
	int i, f = 0, b = front == back;

	for (i = 0; i < c->count && i < HEIGHTS; i++)
		f |= c->heights[i] == front, b |= c->heights[i] == back;
	if (!f && c->count <= HEIGHTS)
		c->count < HEIGHTS ? (c->heights[c->count++] = front) : c->count++;
	if (!b && c->count <= HEIGHTS)
		c->count < HEIGHTS ? (c->heights[c->count++] = back) : c->count++;
}

/*
 * Whether the line from t1 to t2 crosses nothing that blocks sight. The port's walk over the
 * BSP tree, nearer side first, without its calls: the far sides wait on a stack of their own.
 */
static int
sight_walk(const struct trace *trace, struct sight *c)
{
	const fixed_t tx = trace->x, ty = trace->y, tdx = trace->dx, tdy = trace->dy, x2 = trace->x2, y2 = trace->y2;
	const fixed_t sightzstart = c->zstart;
	const node_t *const all = nodes;
	const int pass = c->pass;
	int *const sight_mark = c->mark;
	int waiting[128], depth = 0, bspnum = numnodes - 1;
	fixed_t topslope = c->top, bottomslope = c->bottom;

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
				break;
			front = seg->frontsector;
			back = seg->backsector;
			if (c->heights)
				sight_heights(c, front - sectors, back - sectors);
			if (front->floorheight == back->floorheight && front->ceilingheight == back->ceilingheight)
				continue;
			opentop = front->ceilingheight < back->ceilingheight ? front->ceilingheight : back->ceilingheight;
			openbottom = front->floorheight > back->floorheight ? front->floorheight : back->floorheight;
			if (openbottom >= opentop)
				break;	/* a closed door */
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
				break;
		}
		if (count) {
			c->top = topslope;
			c->bottom = bottomslope;
			return 0;
		}
		if (!depth) {
			c->top = topslope;
			c->bottom = bottomslope;
			return 1;
		}
		bspnum = waiting[--depth];
	}
}

/* ---- sight checks made ahead (docs/games/doom.md, "Sight checks made ahead") ---- */

/*
 * A worker core walks, during a tic, the checks the next tic will likely ask for: the
 * monsters that next look or chase, against the player where his next move takes him if
 * nothing is in the way. A check finds its answer here when its eight numbers are the same.
 */
struct ahead {
	const mobj_t *t1, *t2;
	fixed_t k[8];
	int gen, tic;
	angle_t angle;			/* t1's, for `front` */
	short heights[HEIGHTS];
	char seen, count, front;	/* front: t2 in front of a monster that looks, or 2 behind its back (no walk) */
};
/* The player between two tics, and his next two commands: taken by the first core for the worker. */
struct player_then {
	const mobj_t *mo;
	int loads;			/* doom_level_loads then */
	fixed_t x, y, z, height, momx, momy;
	angle_t angle;
	int rest, attacked, known, next;
	signed char forward[2], side[2];
	short turn[2];
};
#define AHEAD 128			/* a power of two */
static struct ahead *ahead_of[2];	/* by the tic's parity: the worker writes one while this core reads the other */
static const struct ahead *ahead_now;	/* this tic's, once the call that made it is joined */
static int ahead_tic, ahead_on;
static int ahead_counter = -1;		/* the counter of the worker's walks */
/*
 * The monsters whose state ends two tics on, by that tic's parity (the first core lists them as
 * it counts their states down); the player as he was when the worker was given them; the
 * worker's marks (one walk ahead is out at a time).
 */
#define DUE 256
static struct scout { struct sight sight; struct player_then player; const mobj_t *due[2][DUE]; int due_count[2]; } *scout;
int doom_plane_gen;			/* goes up whenever a floor or ceiling moves, or a level is loaded */
extern int doom_level_loads;
static int *moved_at, moved_loads = -1;	/* per sector: doom_plane_gen when it last moved */
unsigned doom_ahead_count[2];		/* checks walked or answered, and of those answered from here */

static inline unsigned
ahead_hash(const mobj_t *t1, const mobj_t *t2)
{
	return (((unsigned)(size_t)t1 >> 4) * 2654435761u ^ ((unsigned)(size_t)t2 >> 4)) >> 25;
}

static inline void
ahead_key(fixed_t *k, const mobj_t *t1, const mobj_t *t2)
{
	k[0] = t1->x, k[1] = t1->y, k[2] = t1->z, k[3] = t1->height;
	k[4] = t2->x, k[5] = t2->y, k[6] = t2->z, k[7] = t2->height;
}

/* The walk for those eight numbers (t1's place and height, t2's): 1 seen, 0 not, -1 the port's. */
static int
sight_of(const fixed_t *k, struct sight *c)
{
	struct trace t;

	c->pass++;
	c->zstart = k[2] + k[3] - (k[3] >> 2);
	c->top = k[6] + k[7] - c->zstart;
	c->bottom = k[6] - c->zstart;
	t.x = k[0];
	t.y = k[1];
	t.x2 = k[4];
	t.y2 = k[5];
	t.dx = k[4] - k[0];
	t.dy = k[5] - k[1];
	return sight_walk(&t, c);
}

/* No floor or ceiling the walk went by has moved since it was made. */
static inline int
heights_same(const struct ahead *a)
{
	int i;

	if (a->gen == doom_plane_gen)
		return 1;
	if (a->count > HEIGHTS)
		return 0;
	for (i = 0; i < a->count && moved_at && moved_loads == doom_level_loads; i++)
		if (moved_at[a->heights[i]] > a->gen)
			return 0;
	return 1;
}

/* What the worker walked for t1 and t2 at these eight numbers, if it did. */
static const struct ahead *
ahead_find(const mobj_t *t1, const mobj_t *t2, const fixed_t *k)
{
	unsigned h = ahead_hash(t1, t2), probe;

	for (probe = 0; probe < 4; probe++, h = (h + 1) & (AHEAD - 1)) {
		const struct ahead *a = &ahead_now[h];

		if (a->tic != ahead_tic)
			break;
		if (a->t1 == t1 && a->t2 == t2)
			return (a->k[0] ^ k[0]) | (a->k[1] ^ k[1]) | (a->k[2] ^ k[2]) | (a->k[3] ^ k[3]) | (a->k[4] ^ k[4]) |
			       (a->k[5] ^ k[5]) | (a->k[6] ^ k[6]) | (a->k[7] ^ k[7]) ? NULL : a;
	}
	return NULL;
}

boolean
P_CheckSight(mobj_t *t1, mobj_t *t2)
{
	static struct sight here = { sight_mark };
	const struct ahead *a;
	fixed_t k[8];
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
	doom_ahead_count[0]++;
	ahead_key(k, t1, t2);
	if (ahead_now && (a = ahead_find(t1, t2, k)) && a->front != 2 && heights_same(a)) {
		doom_ahead_count[1]++;
		return a->seen;
	}
	validcount++;
	seen = sight_of(k, &here);
	sightzstart = here.zstart;
	topslope = here.top;
	bottomslope = here.bottom;
	return seen < 0 ? P_CheckSight_port(t1, t2) : seen;
}

extern byte *demo_p;
extern boolean demoplayback;
extern ticcmd_t netcmds[MAXPLAYERS][BACKUPTICS];
extern int maketic;
extern void *mainzone;			/* z_zone.c's: its first word is its size */
void A_Look(mobj_t *actor);
void A_Chase(mobj_t *actor);

/* A pointer into the zone, where the game's things are: the worker reads them while they change. */
static inline int
in_zone(const void *p)
{
	const char *zone = mainzone;

	return zone && (const char *)p > zone && (const char *)p < zone + *(const int *)zone && !((size_t)p & 3);
}

/* The command of the player's for tic next + ahead (`next` the next to run): from the demo, or as made. */
static int
command_ahead(const player_t *player, int next, int ahead, struct player_then *then)
{
	const int tic = next + ahead;

	if (demoplayback) {
		const byte *at = demo_p + 4 * ahead;

		if (!demo_p || demo_p[0] == 0x80 || (ahead && *at == 0x80))
			return 0;	/* (DEMOMARKER: the demo ends) */
		then->forward[ahead] = (signed char)at[0];
		then->side[ahead] = (signed char)at[1];
		then->turn[ahead] = (short)((unsigned char)at[2] << 8);
	} else if (maketic > tic) {
		const ticcmd_t *cmd = &netcmds[player - players][tic % BACKUPTICS];

		then->forward[ahead] = cmd->forwardmove;
		then->side[ahead] = cmd->sidemove;
		then->turn[ahead] = cmd->angleturn;
	} else {
		return 0;
	}
	return 1;
}

/* What the worker needs of the player, between two tics (`next` the next to run). */
static void
player_take(struct player_then *then, int next)
{
	const mobj_t *mo = players[consoleplayer].mo;
	const player_t *player = mo ? mo->player : NULL;

	then->next = next;
	then->mo = mo;
	then->loads = doom_level_loads;
	then->known = player && player->playerstate == PST_LIVE && player->health > 0 && !(player->cheats & (CF_NOCLIP | CF_NOMOMENTUM)) &&
		      mo->z <= mo->floorz && command_ahead(player, next, 0, then) && command_ahead(player, next, 1, then);
	if (!then->known)
		return;
	then->x = mo->x, then->y = mo->y, then->z = mo->z, then->height = mo->height;
	then->momx = mo->momx, then->momy = mo->momy, then->angle = mo->angle;
	then->rest = mo->reactiontime;
	then->attacked = (mo->flags & MF_JUSTATTACKED) != 0;
}

#define STOPSPEED	0x1000		/* as p_mobj.c has them */
#define FRICTION	0xe800

/*
 * Where the player's thing will be after the next two tics if nothing is in the way: for each
 * tic P_MovePlayer's turn and thrust from its command, P_XYMovement's steps and friction.
 */
static void
player_ahead(const struct player_then *then, fixed_t *k)
{
	fixed_t momx = then->momx, momy = then->momy, x = then->x, y = then->y, xmove, ymove;
	angle_t angle = then->angle;
	int rest = then->rest, attacked = then->attacked, ahead;

	for (ahead = 0; ahead < 2; ahead++) {
		int forward = then->forward[ahead], side = then->side[ahead], turn = then->turn[ahead];

		if (attacked)
			forward = 0xc800 / 512, side = turn = attacked = 0;
		if (rest) {
			rest--;
		} else {
			angle += (unsigned)turn << 16;
			if (forward) {
				momx += FixedMul(forward * 2048, finecosine[angle >> ANGLETOFINESHIFT]);
				momy += FixedMul(forward * 2048, finesine[angle >> ANGLETOFINESHIFT]);
			}
			if (side) {
				momx += FixedMul(side * 2048, finecosine[(angle - ANG90) >> ANGLETOFINESHIFT]);
				momy += FixedMul(side * 2048, finesine[(angle - ANG90) >> ANGLETOFINESHIFT]);
			}
		}
		if (!momx && !momy)
			continue;
		momx = momx > MAXMOVE ? MAXMOVE : momx < -MAXMOVE ? -MAXMOVE : momx;
		momy = momy > MAXMOVE ? MAXMOVE : momy < -MAXMOVE ? -MAXMOVE : momy;
		for (xmove = momx, ymove = momy; xmove || ymove;)
			if (xmove > MAXMOVE / 2 || ymove > MAXMOVE / 2) {
				x += xmove / 2;
				y += ymove / 2;
				xmove >>= 1;
				ymove >>= 1;
			} else {
				x += xmove;
				y += ymove;
				xmove = ymove = 0;
			}
		if (momx > -STOPSPEED && momx < STOPSPEED && momy > -STOPSPEED && momy < STOPSPEED && !forward && !side)
			momx = momy = 0;
		else
			momx = FixedMul(momx, FRICTION), momy = FixedMul(momy, FRICTION);
	}
	k[4] = x, k[5] = y, k[6] = then->z, k[7] = then->height;
}

/* R_PointToAngle2(0, 0, x, y), which writes viewx and viewy: this writes nothing. */
static angle_t
angle_of(fixed_t x, fixed_t y)
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

/*
 * The other end of a monster's check two tics on, if it will make one then: its state ends
 * then in A_Look or A_Chase, and the check is not passed by (as p_enemy.c has them). `next`
 * is the player's place then; t2's place goes into k; `front` as struct ahead has it.
 */
static const mobj_t *
check_ahead(const mobj_t *m, const mobj_t *player, const fixed_t *next, fixed_t *k, int *front)
{
	const state_t *now = m->state;
	const mobj_t *t2 = player;
	actionf_p1 action;

	*front = 0;
	if (now < states || now >= states + NUMSTATES || (unsigned)now->nextstate >= NUMSTATES)
		return NULL;
	action = states[now->nextstate].action.acp1;
	if (action == (actionf_p1)A_Look) {
		const mobj_t *heard = m->subsector->sector->soundtarget;

		if (heard && in_zone(heard) && (heard->flags & MF_SHOOTABLE)) {
			if (!(m->flags & MF_AMBUSH))
				return NULL;	/* it wakes without looking */
			t2 = heard;
		} else if (next) {
			const angle_t an = angle_of(next[4] - m->x, next[5] - m->y) - m->angle;

			*front = an > ANG90 && an < ANG270 && P_AproxDistance(next[4] - m->x, next[5] - m->y) > MELEERANGE ? 2 : 1;
		}
	} else if (action == (actionf_p1)A_Chase) {
		const mobjinfo_t *info = m->info;

		if (info < mobjinfo || info >= mobjinfo + NUMMOBJTYPES)
			return NULL;
		if (m->target && in_zone(m->target) && (m->target->flags & MF_SHOOTABLE)) {
			fixed_t x, y;

			if (m->flags & MF_JUSTATTACKED)
				return NULL;
			t2 = m->target;
			x = t2 == player && next ? next[4] : t2->x;
			y = t2 == player && next ? next[5] : t2->y;
			/* close enough to bite, or free to shoot */
			if (!(info->meleestate && t2->info >= mobjinfo && t2->info < mobjinfo + NUMMOBJTYPES &&
			      P_AproxDistance(x - m->x, y - m->y) < MELEERANGE - 20 * FRACUNIT + t2->info->radius) &&
			    !(info->missilestate && (!m->movecount || gameskill == sk_nightmare || fastparm)))
				return NULL;
		}
	} else {
		return NULL;
	}
	if (t2 == player) {
		if (!next || player->health <= 0)
			return NULL;
		k[4] = next[4], k[5] = next[5], k[6] = next[6], k[7] = next[7];
	} else {
		k[4] = t2->x, k[5] = t2->y, k[6] = t2->z, k[7] = t2->height;
	}
	return t2;
}

/* A worker's, beside the view: the checks of the tic after the next walked into the table of its
 * parity. Things are read while the first core changes them: what is kept is what was walked,
 * under the numbers it was walked for. */
void
doom_ahead(void)
{
	const struct player_then *then = &scout->player;
	const int tic = then->next + 1, count = scout->due_count[tic & 1];
	const mobj_t *const *due = scout->due[tic & 1];
	struct ahead *table = ahead_of[tic & 1];
	struct sight *c = &scout->sight;
	const mobj_t *player = then->mo;
	fixed_t at[8];
	int i;

	if (!in_zone(player) || !in_zone(player->subsector) || !in_zone(player->subsector->sector))
		return;
	if (then->known)
		player_ahead(then, at);
	for (i = 0; i < count && i < DUE; i++) {
		const mobj_t *m = due[i], *t2;
		short heights[HEIGHTS];
		fixed_t k[8];
		unsigned h;
		int pnum, seen, probe, gen, front;

		if (!in_zone(m) || m->thinker.function.acp1 != (actionf_p1)P_MobjThinker || m->health <= 0 ||
		    !in_zone(m->subsector) || !in_zone(m->subsector->sector))
			continue;
		if (!(t2 = check_ahead(m, player, then->known ? at : NULL, k, &front)) || !in_zone(t2->subsector) || !in_zone(t2->subsector->sector))
			continue;
		pnum = (m->subsector->sector - sectors) * numsectors + (t2->subsector->sector - sectors);
		if (front != 2 && ((unsigned)pnum >= (unsigned)(numsectors * numsectors) || rejectmatrix[pnum >> 3] & (1 << (pnum & 7))))
			continue;
		k[0] = m->x, k[1] = m->y, k[2] = m->z, k[3] = m->height;
		h = ahead_hash(m, t2);
		for (probe = 0; probe < 4 && table[h].tic == tic; probe++)
			h = (h + 1) & (AHEAD - 1);
		if (probe == 4)
			continue;
		gen = *(volatile int *)&doom_plane_gen;
		c->heights = heights;
		c->count = 0;
		if ((seen = front == 2 ? 0 : sight_of(k, c)) < 0)
			continue;
		table[h].t1 = m;
		table[h].t2 = t2;
		memcpy(table[h].k, k, sizeof k);
		memcpy(table[h].heights, heights, sizeof heights);
		table[h].count = c->count;
		table[h].gen = gen;
		table[h].seen = seen;
		table[h].angle = m->angle;
		table[h].front = front;
		table[h].tic = tic;
		if (front != 2)
			mcw_add(ahead_counter, 1);
	}
}

/* Workers there are: the tables, the worker's marks (every line's), and its code made present. */
void
doom_ahead_init(void)
{
	const char *on = getenv("DOOM_AHEAD");
	int i;

	if (on && *on == '0')
		return;
	for (i = 0; i < 2; i++)
		if (!(ahead_of[i] = mcw_alloc(AHEAD * sizeof(struct ahead))))
			return;
	scout = mcw_alloc(sizeof *scout);
	if (!scout || !(scout->sight.mark = mcw_alloc(sizeof sight_mark)))
		return;
	mcw_touch(ahead_of[0], AHEAD * sizeof(struct ahead));
	mcw_touch(ahead_of[1], AHEAD * sizeof(struct ahead));
	mcw_touch(scout, sizeof *scout);
	mcw_touch(scout->sight.mark, sizeof sight_mark);
	mcw_touch_code((const void *)doom_ahead, 2048);
	mcw_touch_code((const void *)check_ahead, 1024);
	mcw_touch_code((const void *)sight_heights, 256);
	ahead_counter = mcw_counters() > 1 ? 1 : -1;
	ahead_on = 1;
}

/* The worker's walks so far. */
unsigned
doom_ahead_walked(void)
{
	return ahead_on > 0 && ahead_counter > 0 ? mcw_get(ahead_counter) : 0;
}

/* Between two tics (`next` the next to run): whether a worker is to walk the checks ahead, and
 * what it needs of the player; and when it has, which tic's they are. */
int
doom_ahead_wanted(int next)
{
	if (ahead_on <= 0)
		return 0;
	player_take(&scout->player, next);
	return 1;
}

void
doom_ahead_made(void)
{
	if (scout->player.loads == doom_level_loads)
		ahead_tic = scout->player.next + 1;
}

/*
 * A monster looks for a player to wake up for. As the port's, but whether the player is in
 * front of it is asked before whether it can be seen, and a player not seen is not looked
 * at again in the same search (with one player the port looks at him twice): the same answer.
 */
boolean
P_LookForPlayers(mobj_t *actor, boolean allaround)
{
	int c = 0, stop = (actor->lastlook - 1) & 3, unseen = 0;

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
		if (unseen & (1 << actor->lastlook))
			continue;
		unseen |= 1 << actor->lastlook;
		player = &players[actor->lastlook];
		if (player->health <= 0)
			continue;
		if (!allaround) {
			const struct ahead *a = NULL;
			fixed_t k[8];

			if (ahead_now) {
				ahead_key(k, actor, player->mo);
				a = ahead_find(actor, player->mo, k);
			}
			if (a && a->front && a->angle == actor->angle) {
				if (a->front == 2)
					continue;	/* behind its back, as the worker found */
			} else {
				angle_t an = R_PointToAngle2(actor->x, actor->y, player->mo->x, player->mo->y) - actor->angle;

				if (an > ANG90 && an < ANG270 &&
				    P_AproxDistance(player->mo->x - actor->x, player->mo->y - actor->y) > MELEERANGE)
					continue;	/* behind its back */
			}
		}
		if (!P_CheckSight(actor, player->mo))
			continue;
		actor->target = player->mo;
		return true;
	}
}

/*
 * Every thinker's turn. As the port's, but a thing at rest is seen to here: its state counted
 * down without the call, or passed by. Found at rest it is asleep, and is not asked again
 * whether it rests until the game wakes it (doom_thing_changed). DOOM_PORT=rest: none sleeps.
 */
void
P_RunThinkers(void)
{
	thinker_t *th = thinkercap.next;
	const int stale = stamps_stale;
	const mobj_t **due = NULL;
#define AHEAD_DUE(m) do { int *n_ = &scout->due_count[gametic & 1]; if (*n_ < DUE) due[(*n_)++] = (m); } while (0)

	if (use_port_sight < 0)
		which_port();
	if (use_port_think) {
		P_RunThinkers_port();
		return;
	}
	stamps_stale = 0;
	ahead_now = ahead_on > 0 && ahead_tic == gametic ? ahead_of[gametic & 1] : NULL;
	if (ahead_on > 0) {
		due = scout->due[gametic & 1];	/* (the parity of gametic + 2) */
		scout->due_count[gametic & 1] = 0;
	}
	while (th != &thinkercap) {
		if (th->function.acv == (actionf_v)(-1)) {
			th->next->prev = th->prev;
			th->prev->next = th->next;
			Z_Free(th);
		} else if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
			mobj_t *m = (mobj_t *)th;

			if (stale)
				m->validcount = 0;	/* (a saved game's marks, or a level's of long ago) */
			if (m->validcount < 0) {
				if (m->tics != -1 && !--m->tics)
					P_SetMobjState(m, m->state->nextstate);
				else if (m->tics == 2 && due && (m->flags & MF_COUNTKILL))
					AHEAD_DUE(m);
			} else if (m->momx | m->momy | m->momz | (m->z ^ m->floorz) | (m->flags & MF_SKULLFLY)) {
				P_MobjThinker(m);
			} else if (m->tics != -1) {
				/* at rest: its thinker would count its state down and no more */
				if (!--m->tics)
					P_SetMobjState(m, m->state->nextstate);
				else if (!m->player && !use_port_rest)
					m->validcount |= ASLEEP;
				if (m->tics == 2 && due && (m->flags & MF_COUNTKILL))
					AHEAD_DUE(m);
			} else if ((m->flags & MF_COUNTKILL) && respawnmonsters) {
				P_MobjThinker(m);
			} else if (!m->player && !use_port_rest) {
				m->validcount |= ASLEEP;
			}
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

/* ---- what the view is told (doom_gl.c; docs/games/doom.md, "The view on worker cores") ---- */

/*
 * The game says which things it changed (doom.sh puts the calls in), took out or put into a
 * sector's list, in that order, and which sectors it wrote; a view takes them between tics.
 */
#define TOLD_MOST	600
#define TOLD_LINKS	1024
#define TOLD_GONE	256

int doom_told_all = 1;			/* more than fits, or another level: every thing again */
mobj_t *doom_told_changed[TOLD_MOST];	/* the things changed since the last view, each once; NULL: taken out since */
mobj_t *doom_told_linked[TOLD_LINKS];
mobj_t *doom_told_gone[TOLD_GONE];
int doom_told_changes, doom_told_links, doom_told_goings;
static unsigned told_view = 1;		/* this view's number: no thing has it before it is told of */
short *doom_told_sectors;		/* the sectors written since the last view, each once */
int doom_told_sector_count;
static unsigned char *sector_told;
static const sector_t *sectors_told;
static int sectors_told_count;
int doom_walls_changed;
int doom_level_loads;			/* the levels and saved games loaded so far */

/* The game is about to change what a view shows of a thing, or what P_RunThinkers goes by
 * (its state, its place, its heights, its speed): it is awake, and the next view is told. */
void
doom_thing_changed(mobj_t *thing)
{
	unsigned mark = thing->validcount;

	if (mark & ASLEEP)
		thing->validcount = mark &= ~ASLEEP;
	if (mark >> 12 == told_view || doom_told_all)
		return;
	if (doom_told_changes == TOLD_MOST) {
		doom_told_all = 1;
		return;
	}
	thing->validcount = told_view << 12 | doom_told_changes;
	doom_told_changed[doom_told_changes++] = thing;
}

void
doom_thing_linked(mobj_t *thing)
{
	doom_thing_changed(thing);
	if (doom_told_links == TOLD_LINKS)
		doom_told_all = 1;
	if (!doom_told_all)
		doom_told_linked[doom_told_links++] = thing;
}

void
doom_thing_gone(mobj_t *thing)
{
	const unsigned mark = thing->validcount & ~ASLEEP;

	if (doom_told_all)
		return;
	if (mark >> 12 == told_view)
		doom_told_changed[mark & 0xfff] = NULL;
	if (doom_told_goings == TOLD_GONE)
		doom_told_all = 1;
	else
		doom_told_gone[doom_told_goings++] = thing;
}

/* A view has taken the sectors, and with `things` the things: of those nothing is told any
 * more. (A view that reads the game's own things takes none, and none is noted.) */
void
doom_told_taken(int things)
{
	int i;

	if (things) {
		doom_told_changes = doom_told_links = doom_told_goings = doom_told_all = 0;
		if (++told_view == 1u << 19) {
			/* the numbers have gone round: the marks of long ago go, and every thing is told again */
			told_view = 1;
			stamps_stale = doom_told_all = 1;
		}
	}
	for (i = 0; i < doom_told_sector_count; i++)
		sector_told[doom_told_sectors[i]] = 0;
	doom_told_sector_count = 0;
}

/* The level the view keeps is this one: its sectors are told of from now on. */
void
doom_told_level(void)
{
	free(sector_told);
	free(doom_told_sectors);
	sector_told = calloc(numsectors + 1, 1);
	doom_told_sectors = malloc((numsectors + 1) * sizeof *doom_told_sectors);
	sectors_told = sectors;
	sectors_told_count = numsectors;
	doom_told_sector_count = 0;
	doom_walls_changed = 0;
}

void
doom_sector_changed(void *of)
{
	const sector_t *sector = of;
	int s = sector - sectors_told;

	if (sector < sectors_told || (unsigned)s >= (unsigned)sectors_told_count || sector_told[s])
		return;
	sector_told[s] = 1;
	doom_told_sectors[doom_told_sector_count++] = s;
}

result_e T_MovePlane_port(sector_t *sector, fixed_t speed, fixed_t dest, boolean crush, int floorOrCeiling, int direction);
void P_InitThinkers_port(void);

/* Every floor, ceiling, door and lift moves through here. */
result_e
T_MovePlane(sector_t *sector, fixed_t speed, fixed_t dest, boolean crush, int floorOrCeiling, int direction)
{
	doom_sector_changed(sector);
	doom_plane_gen++;
	if (moved_loads != doom_level_loads) {
		free(moved_at);
		moved_at = calloc(numsectors + 1, sizeof *moved_at);
		moved_loads = doom_level_loads;
	}
	if (moved_at)
		moved_at[sector - sectors] = doom_plane_gen;
	return T_MovePlane_port(sector, speed, dest, crush, floorOrCeiling, direction);
}

/* The game empties its thinkers when it sets a level up or loads a saved game: the kept level
 * is made anew, and the things told of so far are no more. */
void
P_InitThinkers(void)
{
	P_InitThinkers_port();
	doom_level_loads++;
	doom_plane_gen++;
	ahead_tic = -1;
	doom_told_all = stamps_stale = 1;
	sectors_told = NULL;
	sectors_told_count = 0;
}

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

/* ---- which subsector a point is in ---- */

subsector_t *R_PointInSubsector_port(fixed_t x, fixed_t y);

/* As the port's R_PointOnSide. */
static inline int
point_side(fixed_t x, fixed_t y, const node_t *node)
{
	fixed_t dx, dy, left, right;

	if (!node->dx)
		return x <= node->x ? node->dy > 0 : node->dy < 0;
	if (!node->dy)
		return y <= node->y ? node->dx < 0 : node->dx > 0;
	dx = x - node->x;
	dy = y - node->y;
	if ((node->dy ^ node->dx ^ dx ^ dy) & 0x80000000)
		return ((node->dy ^ dx) & 0x80000000) != 0;
	left = (fixed_t)(((long long)(node->dy >> FRACBITS) * dx) >> FRACBITS);
	right = (fixed_t)(((long long)dy * (node->dx >> FRACBITS)) >> FRACBITS);
	return right >= left;
}

/*
 * The deepest node every point of a blockmap cell comes to. The side is one for the whole
 * cell when it is one at the corners of each part the node's own x and y cut the cell into:
 * inside such a part the answer only goes one way with x and one way with y.
 */
static int
cell_search(int cell)
{
	const fixed_t x0 = bmaporgx + ((cell % bmapwidth) << MAPBLOCKSHIFT), x1 = x0 + (1 << MAPBLOCKSHIFT) - 1;
	const fixed_t y0 = bmaporgy + ((cell / bmapwidth) << MAPBLOCKSHIFT), y1 = y0 + (1 << MAPBLOCKSHIFT) - 1;
	int nodenum = numnodes - 1;

	while (!(nodenum & NF_SUBSECTOR)) {
		const node_t *node = &nodes[nodenum];
		fixed_t xs[4] = { x0, x1 }, ys[4] = { y0, y1 };
		int nx = 2, ny = 2, i, k, side;

		if (node->x > x0 && node->x <= x1) {
			xs[2] = node->x - 1;
			xs[3] = node->x;
			nx = 4;
		}
		if (node->y > y0 && node->y <= y1) {
			ys[2] = node->y - 1;
			ys[3] = node->y;
			ny = 4;
		}
		side = point_side(x0, y0, node);
		for (i = 0; i < nx; i++)
			for (k = 0; k < ny; k++)
				if (point_side(xs[i], ys[k], node) != side)
					return nodenum;
		nodenum = node->children[side];
	}
	return nodenum;
}

/*
 * As the port's, which walks the tree from its root for every move a thing tries: the walk
 * starts at the node the point's blockmap cell is all inside of (found when a cell is first
 * asked about), and the point asked about last is remembered (a move asks twice).
 */
subsector_t *
R_PointInSubsector(fixed_t x, fixed_t y)
{
	static int *cell_node, loads = -1;	/* per cell: that node's number plus one, 0 for not known */
	static fixed_t last_x, last_y;
	static subsector_t *last;
	int cx, cy, nodenum;

	if (use_port_sight < 0)
		which_port();
	if (!numnodes || use_port_find)
		return R_PointInSubsector_port(x, y);
	if (loads != doom_level_loads) {
		loads = doom_level_loads;
		free(cell_node);
		cell_node = calloc(bmapwidth * bmapheight + 1, sizeof *cell_node);
		last = NULL;
	}
	if (x == last_x && y == last_y && last)
		return last;
	cx = (x - bmaporgx) >> MAPBLOCKSHIFT;
	cy = (y - bmaporgy) >> MAPBLOCKSHIFT;
	nodenum = numnodes - 1;
	if ((unsigned)cx < (unsigned)bmapwidth && (unsigned)cy < (unsigned)bmapheight && cell_node) {
		int *known = &cell_node[cy * bmapwidth + cx];

		if (!*known)
			*known = cell_search(cy * bmapwidth + cx) + 1;
		nodenum = *known - 1;
	}
	while (!(nodenum & NF_SUBSECTOR)) {
		const node_t *node = &nodes[nodenum];

		nodenum = node->children[point_side(x, y, node)];
	}
	last_x = x;
	last_y = y;
	return last = &subsectors[nodenum & ~NF_SUBSECTOR];
}

/* ---- names ---- */

int W_CheckNumForName_port(char *name);
int R_CheckTextureNumForName_port(char *name);
void R_InitSpriteLumps_port(void);
ssize_t pread(int, void *, size_t, off_t);	/* (unistd.h and p_spec.h both have a `close`) */
extern char *reloadname;
extern int numtextures;
extern struct { char name[8]; } **textures;	/* r_data.c's texture_t begins with its name */

struct named { unsigned a, b; int number; };	/* up to eight letters as two words; number < 0: free */

static unsigned
name_hash(unsigned a, unsigned b)
{
	return (a * 2654435761u ^ b * 2246822519u) >> 12;
}

/* The first `length` letters of a name, capitals, as two words. */
static void
name_words(const char *name, int length, unsigned *a, unsigned *b)
{
	union { char s[8]; unsigned x[2]; } u = { { 0 } };
	int i;

	for (i = 0; i < length; i++)
		u.s[i] = name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i];
	*a = u.x[0];
	*b = u.x[1];
}

static int
name_find(const struct named *table, unsigned mask, unsigned a, unsigned b)
{
	unsigned h = name_hash(a, b) & mask;

	for (; table[h].number >= 0; h = (h + 1) & mask)
		if (table[h].a == a && table[h].b == b)
			return table[h].number;
	return -1;
}

/* Puts a name in unless it is there: the first number given for a name stays. */
static void
name_add(struct named *table, unsigned mask, unsigned a, unsigned b, int number)
{
	unsigned h = name_hash(a, b) & mask;

	for (; table[h].number >= 0; h = (h + 1) & mask)
		if (table[h].a == a && table[h].b == b)
			return;
	table[h].a = a;
	table[h].b = b;
	table[h].number = number;
}

static struct named *
name_table(unsigned names, unsigned *mask)
{
	struct named *table;
	unsigned size = 64, i;

	while (size < names * 3)
		size *= 2;
	table = malloc(size * sizeof *table);
	for (i = 0; table && i < size; i++)
		table[i].number = -1;
	*mask = size - 1;
	return table;
}

/*
 * A lump's number by its name, the last lump of that name. The port compares the name with
 * every lump's, some 3,000 instructions a search; here a table of the names answers, made
 * again whenever lumps have been added.
 */
int
W_CheckNumForName(char *name)
{
	static struct named *table;
	static lumpinfo_t *of;
	static int count = -1;
	static unsigned mask;
	unsigned a, b;
	int i, length = 0;

	if (use_port_sight < 0)
		which_port();
	if (use_port_names || reloadname)
		return W_CheckNumForName_port(name);
	if (of != lumpinfo || count != numlumps) {
		free(table);
		table = name_table(numlumps, &mask);
		if (!table)
			return W_CheckNumForName_port(name);
		for (i = numlumps - 1; i >= 0; i--)
			name_add(table, mask, *(unsigned *)lumpinfo[i].name, *(unsigned *)&lumpinfo[i].name[4], i);
		of = lumpinfo;
		count = numlumps;
	}
	while (length < 8 && name[length])
		length++;
	name_words(name, length, &a, &b);
	return name_find(table, mask, a, b);
}

/*
 * A texture's number by its name, the first that fits. The port's comparison stops at the
 * end of the shorter name, so a name fits every texture it begins, and every texture whose
 * name begins it: `begins` has all the beginnings of every texture's name, `whole` the names.
 */
int
R_CheckTextureNumForName(char *name)
{
	static struct named *whole, *begins, *answered;	/* answered: a name asked before, and its number + 1 */
	static void *of;
	static int count = -1, nameless, answers;
	static unsigned whole_mask, begins_mask, answered_mask;
	unsigned a, b, name_a, name_b;
	int i, k, length = 0, found, other;

	if (name[0] == '-')
		return 0;
	if (use_port_sight < 0)
		which_port();
	if (use_port_names)
		return R_CheckTextureNumForName_port(name);
	if (of != (void *)textures || count != numtextures) {
		free(whole);
		free(begins);
		free(answered);
		whole = name_table(numtextures, &whole_mask);
		begins = name_table(numtextures * 8, &begins_mask);
		answered = name_table(512, &answered_mask);
		answers = 0;
		if (!whole || !begins || !answered) {
			free(whole);
			free(begins);
			free(answered);
			whole = begins = answered = NULL;
			count = -1;
			return R_CheckTextureNumForName_port(name);
		}
		nameless = -1;
		for (i = 0; i < numtextures; i++) {
			for (k = 0; k < 8 && textures[i]->name[k]; k++) {
				name_words(textures[i]->name, k + 1, &a, &b);
				name_add(begins, begins_mask, a, b, i);
			}
			if (!k && nameless < 0)
				nameless = i;
			name_words(textures[i]->name, k, &a, &b);
			name_add(whole, whole_mask, a, b, i);
		}
		of = textures;
		count = numtextures;
	}
	while (length < 8 && name[length])
		length++;
	if (!length)
		return numtextures > 0 ? 0 : -1;	/* no name at all fits the first texture */
	name_words(name, length, &a, &b);
	/* a level's sides ask some 3,000 times for a hundred names: 100 instructions a search, 9% of its load */
	found = name_find(answered, answered_mask, a, b);
	if (found >= 0)
		return found - 1;
	name_a = a;
	name_b = b;
	found = name_find(begins, begins_mask, a, b);
	if (nameless >= 0 && (found < 0 || nameless < found))
		found = nameless;
	for (k = 1; k < length; k++) {
		name_words(name, k, &a, &b);
		other = name_find(whole, whole_mask, a, b);
		if (other >= 0 && (found < 0 || other < found))
			found = other;
	}
	if (answers < 512) {	/* (the table has room for three times as many) */
		name_add(answered, answered_mask, name_a, name_b, found + 1);
		answers++;
	}
	return found;
}

/*
 * Each sprite's width and offsets. The port reads every sprite whole for the eight bytes at
 * its head, 1.4 MB of the shareware game's data; here those eight bytes are read.
 */
void
R_InitSpriteLumps(void)
{
	int i;

	if (use_port_sight < 0)
		which_port();
	if (use_port_names || reloadname) {
		R_InitSpriteLumps_port();
		return;
	}
	firstspritelump = W_GetNumForName("S_START") + 1;
	lastspritelump = W_GetNumForName("S_END") - 1;
	numspritelumps = lastspritelump - firstspritelump + 1;
	spritewidth = Z_Malloc(numspritelumps * sizeof(void *), PU_STATIC, 0);
	spriteoffset = Z_Malloc(numspritelumps * sizeof(void *), PU_STATIC, 0);
	spritetopoffset = Z_Malloc(numspritelumps * sizeof(void *), PU_STATIC, 0);
	for (i = 0; i < numspritelumps; i++) {
		const lumpinfo_t *lump = &lumpinfo[firstspritelump + i];
		short head[4];
		const patch_t *patch = (const patch_t *)head;

		if (lump->size < 8 || !lump->handle || pread(fileno(lump->handle), head, 8, lump->position) != 8)
			patch = W_CacheLumpNum(firstspritelump + i, PU_CACHE);
		spritewidth[i] = SHORT(patch->width) << FRACBITS;
		spriteoffset[i] = SHORT(patch->leftoffset) << FRACBITS;
		spritetopoffset[i] = SHORT(patch->topoffset) << FRACBITS;
	}
}

/* ---- the lines of a blockmap cell ---- */

boolean P_BlockLinesIterator_port(int x, int y, boolean (*func)(line_t *));
boolean PIT_CheckLine(line_t *ld);
extern fixed_t tmbbox[4];

/*
 * As the port's. For a move's check, which is nearly every call, a line whose box the moving
 * thing's box is clear of is passed by here: the check would begin with that and return.
 */
boolean
P_BlockLinesIterator(int x, int y, boolean (*func)(line_t *))
{
	const fixed_t left = tmbbox[BOXLEFT], right = tmbbox[BOXRIGHT], top = tmbbox[BOXTOP], bottom = tmbbox[BOXBOTTOM];
	const int count = validcount;
	const short *list;

	if (func != PIT_CheckLine || use_port_lines)
		return P_BlockLinesIterator_port(x, y, func);
	if (x < 0 || y < 0 || x >= bmapwidth || y >= bmapheight)
		return true;
	for (list = blockmaplump + blockmap[y * bmapwidth + x]; *list != -1; list++) {
		line_t *ld = &lines[*list];

		if (doom_line_mark[*list] == count)
			continue;
		doom_line_mark[*list] = count;
		if (right <= ld->bbox[BOXLEFT] || left >= ld->bbox[BOXRIGHT] || top <= ld->bbox[BOXBOTTOM] || bottom >= ld->bbox[BOXTOP])
			continue;
		if (!PIT_CheckLine(ld))
			return false;
	}
	return true;
}

/* ---- the things of a blockmap cell ---- */

boolean P_BlockThingsIterator_port(int x, int y, boolean (*func)(mobj_t *));
boolean PIT_CheckThing(mobj_t *thing);
extern mobj_t *tmthing;
extern fixed_t tmx, tmy;

/*
 * As the port's. For a move's check a thing that cannot be in the way (nothing solid, or out
 * of reach, or the mover itself) is passed by here: the check would begin with that and return.
 */
boolean
P_BlockThingsIterator(int x, int y, boolean (*func)(mobj_t *))
{
	mobj_t *mobj;

	if (func != PIT_CheckThing || use_port_lines)
		return P_BlockThingsIterator_port(x, y, func);
	if (x < 0 || y < 0 || x >= bmapwidth || y >= bmapheight)
		return true;
	for (mobj = blocklinks[y * bmapwidth + x]; mobj; mobj = mobj->bnext) {
		fixed_t reach;

		if (!(mobj->flags & (MF_SOLID | MF_SPECIAL | MF_SHOOTABLE)))
			continue;
		reach = mobj->radius + tmthing->radius;
		if (abs(mobj->x - tmx) >= reach || abs(mobj->y - tmy) >= reach || mobj == tmthing)
			continue;
		if (!PIT_CheckThing(mobj))
			return false;
	}
	return true;
}

/* ---- a level's lines by sector ---- */

void P_GroupLines_port(void);

/*
 * As the port's, which looks at every line of the level for every sector (1.2M instructions
 * for the shareware game's first levels): here each line goes to its one or two sectors, in
 * the order of the lines, which the boxes depend on (M_AddToBox is not the same in any order).
 */
void
P_GroupLines(void)
{
	line_t **linebuffer, *li;
	fixed_t (*box)[4];
	sector_t *sector;
	int i, k, total = 0;

	if (use_port_sight < 0)
		which_port();
	if (use_port_group) {
		P_GroupLines_port();
		return;
	}
	for (i = 0; i < numsubsectors; i++)
		subsectors[i].sector = segs[subsectors[i].firstline].sidedef->sector;
	for (i = 0, li = lines; i < numlines; i++, li++) {
		total++;
		li->frontsector->linecount++;
		if (li->backsector && li->backsector != li->frontsector) {
			li->backsector->linecount++;
			total++;
		}
	}
	linebuffer = Z_Malloc(total * sizeof(void *), PU_LEVEL, 0);
	box = malloc((numsectors + 1) * sizeof *box);
	for (i = 0, sector = sectors; i < numsectors; i++, sector++) {
		M_ClearBox(box[i]);
		sector->lines = linebuffer;	/* (where its next line goes, until all are in) */
		linebuffer += sector->linecount;
	}
	for (i = 0, li = lines; i < numlines; i++, li++)
		for (k = 0; k < 2; k++) {
			sector = k ? li->backsector : li->frontsector;
			if (k && (!sector || sector == li->frontsector))
				break;
			*sector->lines++ = li;
			M_AddToBox(box[sector - sectors], li->v1->x, li->v1->y);
			M_AddToBox(box[sector - sectors], li->v2->x, li->v2->y);
		}
	for (i = 0, sector = sectors; i < numsectors; i++, sector++) {
		const fixed_t *bbox = box[i];
		int block;

		sector->lines -= sector->linecount;
		sector->soundorg.x = (bbox[BOXRIGHT] + bbox[BOXLEFT]) / 2;
		sector->soundorg.y = (bbox[BOXTOP] + bbox[BOXBOTTOM]) / 2;
		block = (bbox[BOXTOP] - bmaporgy + MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXTOP] = block >= bmapheight ? bmapheight - 1 : block;
		block = (bbox[BOXBOTTOM] - bmaporgy - MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXBOTTOM] = block < 0 ? 0 : block;
		block = (bbox[BOXRIGHT] - bmaporgx + MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXRIGHT] = block >= bmapwidth ? bmapwidth - 1 : block;
		block = (bbox[BOXLEFT] - bmaporgx - MAXRADIUS) >> MAPBLOCKSHIFT;
		sector->blockbox[BOXLEFT] = block < 0 ? 0 : block;
	}
	free(box);
}

