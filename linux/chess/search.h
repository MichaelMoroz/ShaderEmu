/*
 * The search one core makes, the same on the first core of a machine with no workers and on each
 * of many (share.h): principal variation search with a table, null moves, late move reductions,
 * killers and history, and captures to quiet.
 *
 * All it stores is in its chess_core and its own ring of what it tells the shared table, so that
 * on a worker it stores to nothing another core does. It reads the position's past and the shared
 * table, which others change as it reads: a texel is whole as the pass began, and a key in the
 * entry says whose it is.
 */
#ifndef CHESS_SEARCH_H
#define CHESS_SEARCH_H

#ifndef CH_EACH_NODE
#define CH_EACH_NODE()
#endif
#ifndef RING_DEPTH
#define RING_DEPTH 2		/* what the shared table is told: deep enough to save another core work */
#endif
#ifndef SPLIT_DEPTH
#define SPLIT_DEPTH 3		/* where searchers go different ways */
#endif

#include "engine.h"

/* 16 bytes: one texel. */
typedef struct {
	u32 key0, key1;
	u16 move;
	s16 score;
	u8 depth, bound;
	u16 age;
} chess_entry;

enum { B_NONE, B_UPPER, B_LOWER, B_EXACT };

#define HISTORY_SIZE (2 * 7 * 64)
#define RING 64			/* entries a core's ring holds: more than a keeper leaves in a pass */

#define MOVE_STACK 2048

typedef struct chess_core {
	/* The texels stored most. */
	chess_pos pos;
	u32 nodes, poll_at;
	int over;			/* told to stop, or this depth is no use */
	int ply;			/* from the root of the search */
	int top;			/* of the move stack */
	int root_best;			/* the root's best so far */
	u32 who, among;			/* this searcher of how many at this node (share.h); 1 of 1 alone */
	int depth;			/* of the search now */
	chess_entry *table;		/* this core's */
	u32 table_mask;
	const chess_entry *shared;	/* the shared table, read only; NULL: none */
	u32 shared_mask;
	chess_entry *ring;		/* where this core tells the shared table; NULL: none */
	volatile u32 *ring_head;	/* how many it has told, a texel of its own */
	u32 told;
	u32 pad;
	u16 killers[CH_MAX_PLY][2];
	s16 history[HISTORY_SIZE];
	chess_pawn_entry pawns[PAWN_TABLE];
	u32 keys[CH_MAX_PLY + 16];	/* each position's hash1, by search ply */
	u32 moves[MOVE_STACK];		/* the move stack */
} chess_core;

/* Whether the search should stop: every so many nodes. The program's (share.h, nxchess.c). */
static int chess_poll(chess_core *c);

/* The game so far: read by every core. */
static u32 game_keys[CH_MAX_GAME];
static int game_count;

static inline int
hist_index(int piece, int to)
{
	return (SIDE(piece) * 7 + TYPE(piece)) * 64 + S64(to);
}

static void
core_reset(chess_core *c)
{
	memset(c->killers, 0, sizeof c->killers);
	memset(c->history, 0, sizeof c->history);
}

static inline int
is_repeat(const chess_core *c)
{
	const chess_pos *p = &c->pos;
	int n = c->ply, back, half = p->half;
	u32 key = p->hash1;

	if (half < 4)
		return 0;
	for (back = 2; back <= half; back += 2) {
		int at = n - back;
		u32 k;

		if (at >= 0)
			k = c->keys[at];
		else if (game_count + at >= 0)
			k = game_keys[game_count + at];
		else
			break;
		if (k == key)
			return 1;
	}
	return 0;
}

static inline void
pick_next(u32 *moves, int from, int n)
{
	int best = from, i;

	for (i = from + 1; i < n; i++)
		if (moves[i] >> 16 > moves[best] >> 16)
			best = i;
	if (best != from) {
		u32 t = moves[best];

		moves[best] = moves[from], moves[from] = t;
	}
}

static inline int
piece_value(int type)
{
	static const s16 worth[7] = { 0, 100, 320, 330, 500, 950, 2000 };

	return worth[type];
}

/* Scores in the high half, bigger first: the table's move, captures by what they take and with what, promotions, killers, history. */
static inline void
score_moves(chess_core *c, u32 *moves, int n, int tt_move, int ply)
{
	const u8 *board = c->pos.board;
	int k0 = c->killers[ply][0], k1 = c->killers[ply][1], i;

	for (i = 0; i < n; i++) {
		int m = moves[i] & 0x7fff, score, mark = moves[i] & 0x8000;

		if (m == tt_move)
			score = 60000;
		else {
			int from = M_FROM(m), to = M_TO(m), victim = board[to], piece = board[from];

			if (victim)
				score = 40000 + TYPE(victim) * 64 - TYPE(piece);
			else if (m & PROMO_BIT)
				score = (m & 0x3000) ? 1000 : 40000 + QUEEN * 64;
			else if (to == c->pos.ep && TYPE(piece) == PAWN)
				score = 40000 + PAWN * 64 - PAWN;
			else if (m == k0)
				score = 30000;
			else if (m == k1)
				score = 29000;
			else {
				score = 16384 + c->history[hist_index(piece, to)] / 2;
				score = score < 1 ? 1 : score > 28000 ? 28000 : score;
			}
		}
		moves[i] = (u32)m | (u32)mark | (u32)score << 16;
	}
}

static inline int
score_to_table(int s, int ply)
{
	return s > CH_MATE - 512 ? s + ply : s < -CH_MATE + 512 ? s - ply : s;
}

static inline int
score_from_table(int s, int ply)
{
	return s > CH_MATE - 512 ? s - ply : s < -CH_MATE + 512 ? s + ply : s;
}

/*
 * Whether a move wants the king looked at once it is made: an attack on the king asks for every
 * piece of theirs, a hundred instructions and more on each move of a node, and only a move of the
 * king, a move out of a line with it, a capture in passing or a reply to a check can expose it.
 */
#define KING_AT_RISK(p, from, to, king, in_check) \
	((in_check) || (from) == (king) || (attack_kind[(from) - (king) + 119] & 1 << QUEEN) || \
	 ((to) == (p)->ep && TYPE((p)->board[from]) == PAWN))

/* What material and places say, for the side to move: what the eval starts from. */
static inline int
eval_quick(const chess_pos *p)
{
	int phase = p->phase > 24 ? 24 : p->phase, score = (p->mg * phase + p->eg * (24 - phase)) / 24;

	return (p->side ? -score : score) + 10;
}

/* The rest of the eval seldom says more than this: past it no need to ask. */
#define LAZY_MARGIN 220

/* Captures until nothing is left to take. */
static int
quiesce(chess_core *c, int alpha, int beta, int qply)
{
	chess_pos *p = &c->pos;
	int ply = c->ply, in_check, best, n, i, us = p->side;
	u32 *moves;

	c->nodes++;
	CH_EACH_NODE();
	if (ply >= CH_MAX_PLY - 2 || c->top > MOVE_STACK - CH_MOVES)
		return chess_eval(p, c->pawns);
	moves = c->moves + c->top;
	in_check = qply < 2 && IN_CHECK(p);
	if (in_check) {
		best = -CH_MATE + ply;
		n = chess_moves(p, moves, 1);
	} else {
		best = eval_quick(p);
		if (best - LAZY_MARGIN >= beta)
			return best - LAZY_MARGIN;
		if (best + LAZY_MARGIN > alpha)
			best = chess_eval(p, c->pawns);
		if (best >= beta)
			return best;
		if (best > alpha)
			alpha = best;
		if (qply >= 12)
			return best;
		n = chess_moves(p, moves, 0);
	}
	c->top += n;
	for (i = 0; i < n; i++) {
		const u8 *board = p->board;
		int m = moves[i] & 0xffff, victim = TYPE(board[M_TO(m)]), own = TYPE(board[M_FROM(m)]), score;

		score = in_check ? (victim ? 40000 + victim * 64 - own : 1) : victim * 64 - own + ((m & PROMO_BIT) ? 400 : 0);
		moves[i] = (u32)m | (u32)(score < 1 ? 1 : score) << 16;
	}
	for (i = 0; i < n; i++) {
		int m, s, stand;
		chess_undo u;

		pick_next(moves, i, n);
		m = moves[i] & 0xffff;
		if (!in_check) {
			int to = M_TO(m), victim = TYPE(p->board[to]), own = TYPE(p->board[M_FROM(m)]);

			/* a capture that cannot reach alpha, and one a pawn guards that gives more than it takes */
			stand = best + (victim ? piece_value(victim) : 100) + 180;
			if (stand <= alpha && !(m & PROMO_BIT) && p->phase > 4)
				continue;
			if (own > victim && victim && own != KING) {
				int fwd = us ? -16 : 16, a = to + fwd - 1, b = to + fwd + 1, pawn = PIECE(us ^ 1, PAWN);

				if ((!BAD(a) && p->board[a] == pawn) || (!BAD(b) && p->board[b] == pawn)) {
					if (piece_value(own) - 40 > piece_value(victim))
						continue;
				}
			}
		}
		{
			int risk = KING_AT_RISK(p, M_FROM(m), M_TO(m), KING_AT(p, us), in_check);

			chess_make(p, m, &u);
			if (risk && chess_attacked(p, KING_AT(p, us), us ^ 1)) {
				chess_unmake(p, m, &u);
				continue;
			}
		}
		if (in_check && best == -CH_MATE + ply)
			best = -CH_INF;
		c->keys[ply] = u.hash1;
		c->ply = ply + 1;
		s = -quiesce(c, -beta, -alpha, qply + 1);
		c->ply = ply;
		chess_unmake(p, m, &u);
		if (c->over)
			return 0;
		if (s > best) {
			best = s;
			if (s > alpha) {
				alpha = s;
				if (s >= beta)
					break;
			}
		}
	}
	c->top -= n;
	if (in_check && best == -CH_INF)
		best = -CH_MATE + ply;
	return best;
}

static inline chess_entry *
table_slot(chess_core *c, const chess_pos *p)
{
	return &c->table[p->hash0 & c->table_mask];
}

static inline void
table_store(chess_core *c, int depth, int score, int bound, int move)
{
	const chess_pos *p = &c->pos;
	chess_entry *e = table_slot(c, p);

	if (e->key1 == p->hash1 && e->key0 == p->hash0 && e->depth > depth && bound != B_EXACT)
		return;
	e->key0 = p->hash0;
	e->key1 = p->hash1;
	e->move = (u16)move;
	e->score = (s16)score_to_table(score, c->ply);
	e->depth = (u8)depth;
	e->bound = (u8)bound;
	if (c->ring && depth >= RING_DEPTH) {
		chess_entry *o = &c->ring[c->told & (RING - 1)];

		*o = *e;
		*c->ring_head = ++c->told;
	}
}

/* A full-width node. */
static int
chess_search(chess_core *c, int alpha, int beta, int depth, int pv)
{
	chess_pos *p = &c->pos;
	u32 among, who, kid_among = 1, kid_who = 0;
	int end, split, deferring = 1, ply = c->ply, in_check, best, best_move = 0, tt_move = 0, n, i, legal = 0, us = p->side, alpha0 = alpha, s, eval = 0, tried = 0;
	u32 *moves;
	const chess_entry *e;

	if (ply && (is_repeat(c) || p->half >= 100))
		return 0;
	if (ply >= CH_MAX_PLY - 2 || c->top > MOVE_STACK - CH_MOVES)
		return chess_eval(p, c->pawns);
	/* a mate found already bounds what is left to look for */
	if (ply) {
		int most = CH_MATE - ply - 1, least = -CH_MATE + ply;

		if (alpha < least)
			alpha = least;
		if (beta > most)
			beta = most;
		if (alpha >= beta)
			return alpha;
	}
	in_check = IN_CHECK(p);
	if (in_check)
		depth++;
	if (depth <= 0)
		return quiesce(c, alpha, beta, 0);
	CH_EACH_NODE();
	if (++c->nodes >= c->poll_at) {
		c->poll_at = c->nodes + 256;
		if (chess_poll(c)) {
			c->over = 1;
			return 0;
		}
	}
	/* the table: this core's, then the shared one, which may know more */
	e = table_slot(c, p);
	if (e->key1 != p->hash1 || e->key0 != p->hash0)
		e = NULL;
	if (c->shared && (!e || e->depth < depth)) {
		const chess_entry *o = &c->shared[p->hash0 & c->shared_mask];

		if (o->key1 == p->hash1 && o->key0 == p->hash0 && (!e || o->depth >= e->depth))
			e = o;
	}
	if (e) {
		tt_move = e->move;
		if (e->depth >= depth && !pv && ply) {
			int t = score_from_table(e->score, ply), b = e->bound;

			if (b == B_EXACT || (b == B_LOWER && t >= beta) || (b == B_UPPER && t <= alpha))
				return t;
		}
	}
	if (!in_check && !pv) {
		eval = chess_eval(p, c->pawns);
		/* far above beta at a low depth: it will not fall */
		if (depth <= 3 && eval - 90 * depth >= beta && beta > -CH_MATE + 512 && beta < CH_MATE - 512)
			return eval;
		/* giving a move away still above beta */
		if (depth >= 3 && eval >= beta && p->big[us] > 0) {
			chess_undo u;
			int r = 3 + (depth >= 7);

			chess_make_null(p, &u);
			c->keys[ply] = u.hash1;
			c->ply = ply + 1;
			s = -chess_search(c, -beta, -beta + 1, depth - 1 - r, 0);
			c->ply = ply;
			chess_unmake_null(p, &u);
			if (c->over)
				return 0;
			if (s >= beta)
				return s > CH_MATE - 512 ? beta : s;
		}
	}
	moves = c->moves + c->top;
	n = chess_moves(p, moves, 1);
	c->top += n;
	among = c->among, who = c->who;
	split = among > 1 && depth >= SPLIT_DEPTH;
	if (split) {
		int a;
		u32 mine = who % (u32)n;

		for (a = 0; a < n; a++)
			if ((among >= (u32)n ? (u32)a == mine : (u32)a % among == who))
				moves[a] |= 0x8000;
		if (among >= (u32)n)
			kid_among = among / (u32)n + (among % (u32)n > mine), kid_who = who / (u32)n;
		else
			kid_among = 1, kid_who = 0;
	}
	score_moves(c, moves, n, tt_move, ply);
	best = -CH_INF;
	end = n;
	for (i = 0;; i++) {
		int m, quiet, new_depth, r;
		chess_undo u;

		if (i >= end) {
			if (end == n)
				break;
			end = n;	/* now the moves that are others': what they found is in the table by now */
			deferring = 0;
			{
				int left = n - i, k = (int)(who % (u32)left), a;
				u32 keep[256];

				for (a = 0; a < left; a++)
					keep[a] = moves[i + (a + k) % left];
				for (a = 0; a < left; a++)
					moves[i + a] = keep[a];
			}
		}
		if (deferring || !split)
			pick_next(moves, i, end);
		m = moves[i] & 0x7fff;
		/*
		 * After the first move a searcher looks first at the moves that are its own: those
		 * the searchers at this node give it by its number and the move's place in the list. They all
		 * came by one way, so they have the same list. The table is all they say to each other.
		 */
		if (split && legal && deferring) {
			if (!(moves[i] & 0x8000) && i < end - 1) {
				u32 t = moves[i];

				moves[i] = moves[end - 1], moves[--end] = t;
				i--;
				continue;
			}
		}
		quiet = !p->board[M_TO(m)] && !(m & PROMO_BIT);
		/* at the last plies a quiet move that cannot bring it up to alpha */
		if (!pv && !in_check && quiet && legal && best > -CH_MATE + 512) {
			if (depth <= 2 && eval + 120 * depth <= alpha)
				continue;
			if (depth <= 3 && tried >= 4 + depth * depth)
				continue;
		}
		{
			int risk = KING_AT_RISK(p, M_FROM(m), M_TO(m), KING_AT(p, us), in_check);

			chess_make(p, m, &u);
			if (risk && chess_attacked(p, KING_AT(p, us), us ^ 1)) {
				chess_unmake(p, m, &u);
				continue;
			}
		}
		legal++;
		tried += quiet;
		c->keys[ply] = u.hash1;
		c->ply = ply + 1;
		new_depth = depth - 1;
		/* what is its own this searcher goes down with those it shares it with; what is another's, as one more */
		if (split && legal > 1 && deferring)
			c->among = kid_among, c->who = kid_who;
		if (legal == 1)
			s = -chess_search(c, -beta, -alpha, new_depth, pv);
		else {
			r = 0;
#ifdef OPT_RANK
			int rank = split ? n - (int)(moves[i] >> 16) + 1 : legal;
#else
			int rank = legal;
#endif
			if (depth >= 3 && quiet && !in_check && rank > 3 && !IN_CHECK(p)) {
				r = 1 + (rank > 8) + (depth >= 6 && rank > 16);
				r -= pv != 0;
				if (r < 0)
					r = 0;
			}
			s = -chess_search(c, -alpha - 1, -alpha, new_depth - r, 0);
			if (!c->over && s > alpha && r)
				s = -chess_search(c, -alpha - 1, -alpha, new_depth, 0);
			if (!c->over && s > alpha && s < beta && pv)
				s = -chess_search(c, -beta, -alpha, new_depth, 1);
		}
		c->ply = ply;
		c->among = among, c->who = who;
		chess_unmake(p, m, &u);
		if (c->over)
			return 0;
		if (s > best) {
			best = s;
			best_move = m;
			if (!ply)
				c->root_best = m;
			if (s > alpha) {
				alpha = s;
				if (s >= beta) {
					if (quiet) {
						int to = M_TO(m), piece = p->board[M_FROM(m)], h = hist_index(piece, to), v = c->history[h] + depth * depth;

						c->history[h] = (s16)(v > 16000 ? 16000 : v);
						if (c->killers[ply][0] != m) {
							c->killers[ply][1] = c->killers[ply][0];
							c->killers[ply][0] = (u16)m;
						}
					}
					break;
				}
			}
		}
	}
	c->top -= n;
	if (!legal)
		return in_check ? -CH_MATE + ply : 0;
	table_store(c, depth, best, best >= beta ? B_LOWER : best > alpha0 ? B_EXACT : B_UPPER, best_move);
	return best;
}

#endif
