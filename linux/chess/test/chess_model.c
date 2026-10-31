/*
 * The search on many cores as a model on the build computer: every core a coroutine, a pass 8 nodes
 * of each, the keepers' stores made at the pass's end. Fast enough to try a change on 255 cores
 * in seconds, and it says what the machine says (docs/games/chess.md, "The model").
 *   gcc -O2 -I linux/chess -I programs/mc -o chess_model linux/chess/test/chess_model.c
 *   ./chess_model DEPTH [PASS_LIMIT]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
#include "../engine.h"

#define NODES_A_PASS 6
static int model_on;
static void model_node(void);
#define CH_EACH_NODE() model_node()

#include "../search.h"
#define mc_end_pass() model_pass_end()
static void model_pass_end(void);
#define CHESS_NO_MC
#include "../share.h"

static ucontext_t main_ctx, core_ctx[256];
static int current, cores;
static int node_count[256];
static u32 passes;
static chess_core *model_cores[256];

static void
model_yield(void)
{
	int me = current;

	swapcontext(&core_ctx[me], &main_ctx);
}

static void
model_node(void)
{
	if (model_on && ++node_count[current] >= NODES_A_PASS) {
		node_count[current] = 0;
		model_yield();
	}
}

static void model_pass_end(void) { model_yield(); }

static int
chess_poll(chess_core *c)
{
	return ch_stop.word || (u32)c->depth <= ch_deepest.word;
}

static int nkeepers, nsearchers;
static u32 *reads[8];
static u32 age = 1;

static void
core_main(int who)
{
	if (who < nkeepers)
		keeper_run(who, reads[who], nsearchers, nkeepers, age);
	else
		searcher_run(model_cores[who - nkeepers], who - nkeepers, nsearchers);
	for (;;)
		model_yield();
}

/* The moves as the machine's `nxchess time` positions. */
static const char *const bench[] = {
	"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	"r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP1N1PPP/R2QK2R w KQ - 0 8",
	"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	"2rq1rk1/pb2bppp/1pn1pn2/2pp4/3P4/1PNBPN2/PB3PPP/2RQ1RK1 w - - 0 12",
	"8/5pk1/4p1p1/3pP2p/1P1P3P/2r3P1/5PK1/1R6 w - - 0 40",
};

static u32 serial_nodes[64];

static void
serial(chess_pos *root, int depth)
{
	static chess_core c0;
	static chess_entry t[1 << 16];
	chess_core *c = &c0;
	int d, s = 0;

	memset(t, 0, sizeof t);
	c->table = t, c->table_mask = (1 << 16) - 1;
	c->pos = *root;
	c->nodes = 0, c->poll_at = 0xffffffffu, c->top = c->ply = 0, c->over = 0, c->root_best = 0, c->who = c->among = 0, c->ring = NULL, c->shared = NULL;
	core_reset(c);
	model_on = 0;
	for (d = 1; d <= depth; d++) {
		int alpha = -CH_INF, beta = CH_INF, widen = 24, v;

		c->depth = d;
		if (d >= 4 && !CH_IS_MATE(s))
			alpha = s - widen, beta = s + widen;
		for (;;) {
			v = chess_search(c, alpha, beta, d, 1);
			if (v <= alpha)
				widen *= 3, alpha = widen > 400 ? -CH_INF : v - widen;
			else if (v >= beta)
				widen *= 3, beta = widen > 400 ? CH_INF : v + widen;
			else
				break;
		}
		s = v;
		serial_nodes[d] = c->nodes;
	}
}

int
main(int argc, char **argv)
{
	int depth = argc > 1 ? atoi(argv[1]) : 8, b, w, i;
	static const int sizes[] = { 15, 63, 255 };
	u32 limit = argc > 2 ? (u32)atoi(argv[2]) : 40000;
	static char stacks[256][1 << 18];

	chess_tables();
	ch_shared = calloc(1u << SHARED_BITS, sizeof(chess_entry));
	ch_locals = calloc(256u << LOCAL_BITS, sizeof(chess_entry));
	ch_rings = calloc(256 * RING, sizeof(chess_entry));
	ch_heads = calloc(256, sizeof(chess_head));
	ch_status = calloc(256, sizeof(chess_status));
	for (i = 0; i < 8; i++)
		reads[i] = calloc(256, 4);
	for (i = 0; i < 256; i++) {
		model_cores[i] = calloc(1, sizeof(chess_core));
		model_cores[i]->table = ch_locals + (i << LOCAL_BITS);
		model_cores[i]->table_mask = (1u << LOCAL_BITS) - 1;
	}
	for (w = 0; w < 3; w++) {
		double sum_speed = 0;
		int n = sizes[w];

		nkeepers = n >= 192 ? 8 : n >= 96 ? 4 : n >= 48 ? 2 : n >= 4 ? 1 : 0;
		nsearchers = n - nkeepers;
		for (b = 0; b < 5; b++) {
			chess_pos p;
			int reached = 0, s;
			u32 at[64] = { 0 };
			{
				unsigned long long nodes = 0;

				chess_set_fen(&p, bench[b]);
				serial(&p, depth);
				memset(ch_shared, 0, (1u << SHARED_BITS) * sizeof(chess_entry));
				memset(ch_status, 0, 256 * sizeof(chess_status));
				for (i = 0; i < 256; i++)
					memset(model_cores[i]->table, 0, (1u << LOCAL_BITS) * sizeof(chess_entry)), ch_heads[i].head = 0, model_cores[i]->told = 0;
				ch_job.root = p;
				ch_job.target = depth;
				ch_stop.word = 0, ch_deepest.word = 0;
				age = (age + 1) & 0xffff;
				cores = n;
				for (i = 0; i < n; i++) {
					getcontext(&core_ctx[i]);
					core_ctx[i].uc_stack.ss_sp = stacks[i];
					core_ctx[i].uc_stack.ss_size = sizeof stacks[i];
					core_ctx[i].uc_link = NULL;
					makecontext(&core_ctx[i], (void (*)(void))core_main, 1, i);
					node_count[i] = 0;
					if (i >= nkeepers) {
						chess_core *c = model_cores[i - nkeepers];

						c->shared = ch_shared, c->shared_mask = (1u << SHARED_BITS) - 1;
						c->ring = ch_rings + (i - nkeepers) * RING;
						c->ring_head = &ch_heads[i - nkeepers].head;
						c->told = 0;
					}
				}
				model_on = 1;
				for (passes = 0; passes < limit && !reached; passes++) {
					for (i = 0; i < n; i++) {
						current = i;
						swapcontext(&main_ctx, &core_ctx[i]);
					}
					for (s = 0; s < nsearchers; s++) {
						int d = ch_status[s].depth;

						if ((u32)d > ch_deepest.word) {
							int k;

							for (k = ch_deepest.word + 1; k <= d; k++)
								at[k] = passes;
							ch_deepest.word = (u32)d;
						}
						if (d >= depth)
							reached = 1;
					}
				}
				model_on = 0;
				ch_stop.word = 1;
				for (i = 0; i < n; i++) {
					current = i;
					swapcontext(&main_ctx, &core_ctx[i]);
				}
				model_on = 0;
				for (s = 0; s < nsearchers; s++)
					nodes += model_cores[s]->nodes;
				printf("W=%3d position %d: depth %d in %u passes, %llu nodes (plain: %u nodes, %u passes): %.1f x; depth 6: plain %u, %u passes\n", n, b, (int)ch_deepest.word, passes, nodes,
					serial_nodes[depth], serial_nodes[depth] / NODES_A_PASS, (double)serial_nodes[depth] / NODES_A_PASS / (passes ? passes : 1),
					serial_nodes[6] / NODES_A_PASS, at[6]);
				sum_speed += (double)serial_nodes[depth] / NODES_A_PASS / (passes ? passes : 1);
				fflush(stdout);
			}
		}
		printf("W=%3d mean speed-up to depth %d: %.1f\n", n, depth, sum_speed / 5);
	}
	return 0;
}
