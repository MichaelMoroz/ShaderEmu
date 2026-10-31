/*
 * The search on many cores (docs/games/chess.md, "The search on the workers"). Cores of this
 * machine see each other's stores a pass later and must not store to one texel, so no two write
 * one table. Every searcher searches the same position by itself, each in its own order and some
 * a depth further, and what makes that a gain is what they know of each other's work:
 *
 *   - Every searcher has a small table of its own, and a ring in which it tells what it found
 *     at a depth worth telling; only it stores to these.
 *   - A shared table is read by all and written by a few cores that do nothing else, the keepers:
 *     its texels are cut in shards, each a keeper's alone. A keeper reads the rings in turn
 *     and stores what lies in its shard. So a searcher sees what another found 2 or 3 passes on.
 *   - The first core only looks: which searcher has finished the deepest, what it said, and
 *     when to stop. It writes `deepest` and `stop`, which the searchers read.
 *
 * What a core stores is a whole number of texels, one writer each. A result is therefore not a
 * sum of the workers' number: with workers a fixed position gives the right move, not fixed nodes.
 */
#ifndef CHESS_SHARE_H
#define CHESS_SHARE_H

#include "search.h"
#ifndef CHESS_NO_MC
#include "mc.h"
#endif

#define SHARED_BITS 17		/* 2 MB */
#define LOCAL_BITS 10		/* 16 KB a core */
#define MOST_WORKERS 63		/* deeper in fixed time than 255, which only has more nodes (docs/games/chess.md) */

/* A searcher's word to the first core: a texel of its own. */
typedef struct {
	s16 depth, score;	/* the deepest whole search; 0: none yet */
	int best;
	u32 nodes;
	u32 stamp;	/* of which search: what another search left is not looked at */
} chess_status;

typedef struct {
	u32 head;
	u32 pad[3];
} chess_head;

/* In its order of the workers: the keepers' first. */
typedef struct {
	chess_pos root;
	int searchers, keepers;
	int target;
	u32 stamp;
} chess_job;

/* The words every core reads and the first writes: each alone in its texel. */
typedef union { volatile u32 word; u32 texel[4]; } chess_word __attribute__((aligned(16)));
static chess_word ch_stop_word, ch_deepest_word;
#define ch_stop ch_stop_word
#define ch_deepest ch_deepest_word

static chess_job ch_job __attribute__((aligned(16)));
static chess_core *ch_cores;		/* by share */
static chess_entry *ch_locals;		/* a core's table: 1 << LOCAL_BITS entries each */
static chess_entry *ch_rings;		/* RING entries each */
static chess_head *ch_heads;
static chess_status *ch_status;
static chess_entry *ch_shared;

static inline void
shared_put(const chess_entry *n, u32 age)
{
	chess_entry *e = &ch_shared[n->key0 & ((1u << SHARED_BITS) - 1)];

	if (e->age == age && e->depth > n->depth + (e->key0 == n->key0 && e->key1 == n->key1 ? 0 : 1) && n->bound != B_EXACT)
		return;
	e->key0 = n->key0, e->key1 = n->key1, e->move = n->move, e->score = n->score;
	e->depth = n->depth, e->bound = n->bound, e->age = (u16)age;
}

/*
 * A keeper: a look at every ring each pass. It stores only where (key0's bits at the top of the
 * index) say its shard is. It keeps how far it has read of each ring in its own memory.
 */
static void
keeper_run(int who, u32 *read_to, int searchers, int keepers, u32 age)
{
	u32 shard_shift = SHARED_BITS - 3, shard_mask = 7;
	int s;

	for (s = 0; s < searchers; s++)
		read_to[s] = 0;
	while (!ch_stop.word) {
		for (s = 0; s < searchers; s++) {
			u32 head = ch_heads[s].head, at = read_to[s];

			if (at == head)
				continue;
			if (head - at > RING)
				at = head - RING;
			{
				const chess_entry *ring = ch_rings + s * RING;

				for (; at != head; at++) {
					const chess_entry *n = &ring[at & (RING - 1)];

					if ((n->key0 >> shard_shift & shard_mask) % (u32)keepers == (u32)who)
						shared_put(n, age);
				}
			}
			read_to[s] = head;
		}
		mc_end_pass();
	}
}

/* A searcher: deeper and deeper until told to stop. */
static void
searcher_run(chess_core *c, int who, int searchers)
{
	chess_status *st = &ch_status[who];
	int d = 1, score = 0;

	c->pos = ch_job.root;
	c->top = c->ply = 0;
	c->over = 0;
	c->nodes = 0;
	c->poll_at = 256;
	c->who = (u32)who;
	c->among = (u32)searchers;
	c->depth = 0;
	core_reset(c);
	st->depth = 0, st->best = 0;
	__asm__ volatile("" : : : "memory");
	st->stamp = ch_job.stamp;
	while (!ch_stop.word && d < CH_MAX_PLY - 8) {
		int alpha = -CH_INF, beta = CH_INF, widen = 24, s;
		u32 deepest = ch_deepest.word;

		/* no use in what another has finished: and one in eight looks one further */
		if ((u32)d <= deepest)
			d = (int)deepest + 1;
		if ((who & 7) == 7 && d < ch_job.target)
			d++;
		c->depth = d;
		if (d >= 4 && !CH_IS_MATE(score))
			alpha = score - widen, beta = score + widen;
		for (;;) {
			s = chess_search(c, alpha, beta, d, 1);
			if (c->over)
				break;
			if (s <= alpha)
				widen *= 3, alpha = widen > 400 ? -CH_INF : s - widen;
			else if (s >= beta)
				widen *= 3, beta = widen > 400 ? CH_INF : s + widen;
			else
				break;
		}
		if (c->over) {
			if (ch_stop.word)
				break;
			/* another has gone past this depth: on from there */
			c->over = 0;
			c->top = c->ply = 0;
			c->pos = ch_job.root;
			continue;
		}
		score = s;
		st->best = c->root_best;
		st->score = (s16)s;
		st->depth = (s16)d;
		d++;
	}
	st->nodes = c->nodes;
}

#endif
