/*
 * On the build computer: perft against the known numbers, deeper than the machine's check, and the
 * plain search for its nodes (docs/games/chess.md, "Checking it").
 *   gcc -O2 -I linux/chess -o chess_host linux/chess/test/chess_host.c && ./chess_host [DEPTH]
 */
#include <stdio.h>
#include <stdlib.h>
#include "../search.h"

static int chess_poll(chess_core *c) { (void)c; return 0; }

static const struct { const char *fen; int depth; u32 nodes; } perfts[] = {
	{ "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 5, 4865609 },
	{ "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603 },
	{ "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 6, 11030083 },
	{ "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 5, 15833292 },
	{ "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487 },
	{ "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594 },
};

/* The machine's `nxchess search` on the same positions: their sum must be the same. */
static const char *const bench[] = {
	"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	"r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP1N1PPP/R2QK2R w KQ - 0 8",
	"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	"2rq1rk1/pb2bppp/1pn1pn2/2pp4/3P4/1PNBPN2/PB3PPP/2RQ1RK1 w - - 0 12",
	"8/5pk1/4p1p1/3pP2p/1P1P3P/2r3P1/5PK1/1R6 w - - 0 40",
};

int
main(int argc, char **argv)
{
	static u32 stack[8192];
	static chess_entry table[1 << 16];
	static chess_core core;
	chess_core *c = &core;
	int i, bad = 0, depth = argc > 1 ? atoi(argv[1]) : 7;
	u32 sum = 0;

	chess_tables();
	for (i = 0; i < (int)(sizeof perfts / sizeof perfts[0]); i++) {
		chess_pos p;
		u32 got;

		chess_set_fen(&p, perfts[i].fen);
		got = chess_perft(&p, perfts[i].depth, stack);
		printf("perft %d: %u, %s\n", perfts[i].depth, got, got == perfts[i].nodes ? "as known" : "WRONG");
		bad += got != perfts[i].nodes;
	}
	printf("%s\n", bad ? "FAIL" : "PASS");
	for (i = 0; i < (int)(sizeof bench / sizeof bench[0]); i++) {
		int d, s = 0;

		memset(table, 0, sizeof table);
		c->table = table;
		c->table_mask = (1 << 16) - 1;
		chess_set_fen(&c->pos, bench[i]);
		c->nodes = 0, c->poll_at = 256, c->top = c->ply = 0, c->over = 0, c->root_best = 0;
		core_reset(c);
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
		}
		printf("position %d: depth %d score %d nodes %u\n", i, depth, s, c->nodes);
		sum = sum * 33 + (u32)s;
		sum = sum * 33 + (u32)c->root_best;
		sum = sum * 33 + c->nodes;
	}
	printf("sum %08x\n", sum);
	return bad != 0;
}
