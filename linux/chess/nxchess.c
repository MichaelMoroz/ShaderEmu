/*
 * nxchess: chess on the desktop (docs/games/chess.md). The engine is engine.h and search.h, its
 * search on the worker cores share.h. Click a piece and a square, or drag, or the arrows and
 * Space. `nxchess perft|search|time|solve|book` are its checks and measures, with no window.
 */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_single.h"
#include "mcw.h"
#include "mcwx.h"
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

#include "share.h"
#include "pieces.h"

#define SQ 48
#define MENU_H 20
#define BX 22			/* the board's corner in the window: letters and numbers round it */
#define BY (MENU_H + 8)
#define PANEL_W 200
#define WIN_W (BX + 8 * SQ + 12 + PANEL_W + 8)
#define WIN_H (BY + 8 * SQ + 24)
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ)

/* ---- thinking ---- */

static int workers_wanted = MOST_WORKERS, workers, keepers, searchers;
static void *memory;
static unsigned memory_bytes;
static int memory_for;
static chess_core *lone;		/* the first core's, with no workers */
static uint64_t lone_stop_at;		/* in ms */
static int lone_target;
static int (*lone_events)(void);	/* a look at the window while the first core searches */
static int thinking;
static int think_target;
static uint64_t think_stop_at, think_began;
static int age;

static uint64_t
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)(t.tv_nsec / 1000000);
}

/* The table every core keeps and the first core's. Allocated once, for the most cores asked. */
static int
core_memory(int cores)
{
	unsigned core_bytes = (sizeof(chess_core) + 15) & ~15u, local = (1u << LOCAL_BITS) * sizeof(chess_entry), n = cores ? (unsigned)cores : 1;
	unsigned bytes = (cores ? (1u << SHARED_BITS) * sizeof(chess_entry) : (1u << 16) * sizeof(chess_entry)) +
		n * (core_bytes + local + RING * sizeof(chess_entry) + sizeof(chess_head) + sizeof(chess_status)) + 4096 * 3;
	char *at;
	unsigned k;

	if (memory && memory_for >= cores && (cores || memory_for == 0))
		return 1;
	if (memory)
		munmap(memory, memory_bytes);
	memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		memory = NULL;
		return 0;
	}
	memory_bytes = bytes;
	memory_for = cores;
	at = (char *)(((uintptr_t)memory + 15) & ~(uintptr_t)15);
	ch_shared = (chess_entry *)at;
	at += (cores ? 1u << SHARED_BITS : 1u << 16) * sizeof(chess_entry);
	ch_cores = (chess_core *)at;
	at += n * core_bytes;
	ch_locals = (chess_entry *)at;
	at += n * local;
	ch_rings = (chess_entry *)at;
	at += n * RING * sizeof(chess_entry);
	ch_heads = (chess_head *)at;
	at += n * sizeof(chess_head);
	ch_status = (chess_status *)at;
	return 1;
}

#define CORE(k) ((chess_core *)((char *)ch_cores + (k) * ((sizeof(chess_core) + 15) & ~15u)))

static int
chess_poll(chess_core *c)
{
	if (c == lone) {
		if (lone_target)
			return 0;
		if (lone_events && lone_events())
			return 1;
		return now_ms() >= lone_stop_at;
	}
	mcw_add(0, c->nodes - c->pad);
	c->pad = c->nodes;
	return ch_stop.word || (u32)c->depth <= ch_deepest.word;
}

static void
worker_piece(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t i;

	(void)ctx;
	for (i = from; i < to; i++) {
		if ((int)i < keepers)
			keeper_run((int)i, (u32 *)CORE(i)->moves, searchers, keepers, (u32)age);
		else {
			chess_core *c = CORE(i);
			int who = (int)i - keepers;

			c->pad = 0;
			c->table = ch_locals + (i << LOCAL_BITS);
			c->table_mask = (1u << LOCAL_BITS) - 1;
			c->shared = ch_shared;
			c->shared_mask = (1u << SHARED_BITS) - 1;
			c->ring = ch_rings + who * RING;
			c->ring_head = &ch_heads[who].head;
			c->told = ch_heads[who].head;
			searcher_run(c, who, searchers);
			mcw_add(0, c->nodes - c->pad);
		}
	}
}

/* What the workers' code and memory want before they start: a worker stops at a page no core has been to. */
static void
workers_ready(void)
{
	extern char _start[], etext[];

	mcw_touch(memory, memory_bytes);
	mcw_touch_code(_start, (unsigned)(etext - _start));
}

static int first_think_done, workers_opened, lend_workers;
static int think_depth, think_score, think_best, think_side;
static uint32_t think_nodes;

/* Begins a search of the position. ms: until when; target: to what depth (0: as far as there is time). */
static void
think_begin(const chess_pos *root, int ms, int target)
{
	int i;

	think_began = now_ms();
	think_stop_at = think_began + (uint64_t)ms;
	think_target = target;
	thinking = 1;
	think_depth = 0, think_score = 0, think_best = 0;
	age = (age + 1) & 0x7fff;
	ch_job.stamp++;
	ch_job.root = *root;
	ch_job.target = target ? target : CH_MAX_PLY;
	ch_stop.word = 0;
	ch_deepest.word = 0;
	if (!workers_opened && workers_wanted > 0) {
		/* (lent, so that a program that wants them has them: they stay open between searches) */
		if (lend_workers)
			mcw_lend();
		workers = mcw_open(workers_wanted);
		workers_opened = 1;
	}
	if (workers && !core_memory(workers)) {
		mcw_close();
		workers = 0;
		workers_wanted = 0;
	}
	if (!workers) {
		chess_core *c;

		if (!core_memory(memory_for ? memory_for : 0))
			return;
		c = lone = CORE(0);
		c->table = ch_shared;
		c->table_mask = (1u << 16) - 1;
		c->shared = NULL;
		c->ring = NULL;
		c->who = c->among = 0;
		return;
	}
	keepers = workers >= 192 ? 8 : workers >= 96 ? 4 : workers >= 48 ? 2 : workers >= 4 ? 1 : 0;
	searchers = workers - keepers;
	lone = NULL;
	if (first_think_done < workers) {
		workers_ready();
		first_think_done = workers;
	}
	mcw_counters();
	mcw_set(0, 0);
	{
		static mcw_for_opts o;

		o.grain = 1;
		o.first_idle = 1;
		o.workers = workers;
		mcw_for_linger(0);
		mcw_for_begin((uint32_t)workers, worker_piece, NULL, &o);
		}
}

/* The answer so far: the deepest whole search any searcher has, the higher score where they are level. */
static void
think_look(void)
{
	int i, d = 0, score = 0, best = 0;

	for (i = 0; i < searchers; i++) {
		const chess_status *s = &ch_status[i];

		if (s->stamp != ch_job.stamp)
			continue;
		if (s->depth > d || (s->depth == d && s->depth && s->score > score))
			d = s->depth, score = s->score, best = s->best;
	}
	if (d > think_depth || best) {
		think_depth = d, think_score = score, think_best = best;
	}
	if ((u32)d > ch_deepest.word)
		ch_deepest.word = (u32)d;
}

/* Whether the search has what it was asked for: the depth, the time, or a mate with no shorter one left to find. */
static int
think_done(void)
{
	if (think_depth && CH_IS_MATE(think_score) && think_depth >= CH_MATE - abs(think_score))
		return 1;
	return think_target ? think_depth >= think_target : now_ms() >= think_stop_at;
}

/* Ends the search: the workers stop within a pass or a few. */
static void
think_end(void)
{
	ch_stop.word = 1;
	mc_publish();
	think_look();
	mcw_for_wait();
	mcw_for_end();
	mcw_counters_settle();
	think_nodes = mcw_get(0);
	think_look();
	thinking = 0;
	if (mcw_asked()) {
		/* another program wants the workers: they are its now, and the first core goes on alone */
		mcw_close();
		workers = 0;
		workers_wanted = 0;
	}
}

/* The first core alone: deeper and deeper, with what it found so far kept. The window is looked at from chess_poll. */
static void
lone_search(const chess_pos *root, int ms, int target)
{
	chess_core *c = lone;
	int d, s = 0;

	lone_target = target;
	lone_stop_at = now_ms() + (uint64_t)ms;
	c->pos = *root;
	c->nodes = 0;
	c->poll_at = 256;
	c->top = c->ply = 0;
	c->over = 0;
	c->root_best = 0;
	core_reset(c);
	think_depth = 0;
	for (d = 1; d < CH_MAX_PLY - 8 && (!target || d <= target); d++) {
		int alpha = -CH_INF, beta = CH_INF, widen = 24, v;

		c->depth = d;
		if (d >= 4 && !CH_IS_MATE(s))
			alpha = s - widen, beta = s + widen;
		for (;;) {
			v = chess_search(c, alpha, beta, d, 1);
			if (c->over)
				break;
			if (v <= alpha)
				widen *= 3, alpha = widen > 400 ? -CH_INF : v - widen;
			else if (v >= beta)
				widen *= 3, beta = widen > 400 ? CH_INF : v + widen;
			else
				break;
		}
		if (c->root_best)
			think_best = c->root_best;
		if (c->over)
			break;
		s = v;
		think_depth = d, think_score = s;
		if ((!target && now_ms() >= lone_stop_at) || (CH_IS_MATE(s) && d >= CH_MATE - abs(s)))
			break;
	}
	think_nodes = c->nodes;
	c->over = 0;
	thinking = 0;
}

/* ---- the game ---- */

static chess_pos game[CH_MAX_GAME + 1];
static u16 played[CH_MAX_GAME];
static char san[CH_MAX_GAME][8];
static int moves_made;
static int over_kind;		/* 0 the game goes on; else what ended it */
enum { END_MATE = 1, END_STALE, END_REPEAT, END_FIFTY, END_MATERIAL };

static const char start_fen[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static void
square_name(int sq, char *out)
{
	out[0] = (char)('a' + (sq & 7));
	out[1] = (char)('1' + (sq >> 4));
}

static const char *
move_text(int m)
{
	static char s[6];

	if (!m)
		return "none";
	square_name(M_FROM(m), s);
	square_name(M_TO(m), s + 2);
	s[4] = (m & PROMO_BIT) ? "qnrb"[M_PROMO(m)] : 0;
	s[5] = 0;
	return s;
}

/* The legal moves of a position. */
static int
legal_moves(chess_pos *p, u16 *out)
{
	static u32 raw[CH_MOVES];
	int n = chess_moves(p, raw, 1), i, k = 0, us = p->side;

	for (i = 0; i < n; i++) {
		chess_undo u;
		int m = (int)raw[i];

		chess_make(p, m, &u);
		if (!chess_attacked(p, KING_AT(p, us), us ^ 1))
			out[k++] = (u16)m;
		chess_unmake(p, m, &u);
	}
	return k;
}

/* The move as a reader writes it. */
static void
write_san(chess_pos *p, int m, char *out)
{
	static const char names[7] = " NBRQK";
	u16 list[CH_MOVES];
	int from = M_FROM(m), to = M_TO(m), piece = p->board[from], type = TYPE(piece), cap = p->board[to], i, n, same_file = 0, same_rank = 0, others = 0;
	char *w = out;
	chess_undo u;

	if (type == KING && (to - from == 2 || from - to == 2)) {
		strcpy(w, to > from ? "O-O" : "O-O-O");
		w += strlen(w);
	} else {
		if (type == PAWN) {
			if (cap || (from & 7) != (to & 7))
				*w++ = (char)('a' + (from & 7)), *w++ = 'x';
		} else {
			*w++ = names[type - 1];
			n = legal_moves(p, list);
			for (i = 0; i < n; i++) {
				int f = M_FROM(list[i]);

				if (f != from && M_TO(list[i]) == to && p->board[f] == piece) {
					others++;
					same_file |= (f & 7) == (from & 7);
					same_rank |= (f >> 4) == (from >> 4);
				}
			}
			if (others) {
				if (!same_file)
					*w++ = (char)('a' + (from & 7));
				else if (!same_rank)
					*w++ = (char)('1' + (from >> 4));
				else
					*w++ = (char)('a' + (from & 7)), *w++ = (char)('1' + (from >> 4));
			}
			if (cap)
				*w++ = 'x';
		}
		square_name(to, w);
		w += 2;
		if (m & PROMO_BIT)
			*w++ = '=', *w++ = "QNRB"[M_PROMO(m)];
	}
	chess_make(p, m, &u);
	if (IN_CHECK(p)) {
		u16 l2[CH_MOVES];

		*w++ = legal_moves(p, l2) ? '+' : '#';
	}
	chess_unmake(p, m, &u);
	*w = 0;
}

static void
game_keys_set(void)
{
	int i;

	for (i = 0; i < moves_made && i < CH_MAX_GAME; i++)
		game_keys[i] = game[i].hash1;
	game_count = moves_made;
}

static int
little_material(const chess_pos *p)
{
	int s, i, minors = 0;

	for (s = 0; s < 2; s++)
		for (i = 1; i < p->count[s]; i++) {
			int t = TYPE(p->board[p->list[s][i]]);

			if (t != KNIGHT && t != BISHOP)
				return 0;
			minors++;
		}
	return minors <= 1;
}

static void
game_check_end(void)
{
	chess_pos *p = &game[moves_made];
	u16 list[CH_MOVES];
	int i, seen = 1;

	over_kind = 0;
	if (!legal_moves(p, list))
		over_kind = IN_CHECK(p) ? END_MATE : END_STALE;
	else if (p->half >= 100)
		over_kind = END_FIFTY;
	else if (little_material(p))
		over_kind = END_MATERIAL;
	else {
		for (i = moves_made - 2; i >= 0 && i >= moves_made - p->half; i -= 2)
			seen += game[i].hash1 == p->hash1 && game[i].hash0 == p->hash0;
		if (seen >= 3)
			over_kind = END_REPEAT;
	}
}

static void
game_new(const char *fen)
{
	moves_made = 0;
	chess_set_fen(&game[0], fen);
	game_keys_set();
	over_kind = 0;
}

static void
game_play(int m)
{
	chess_pos *p = &game[moves_made];
	chess_undo u;

	write_san(p, m, san[moves_made]);
	game[moves_made + 1] = *p;
	chess_make(&game[moves_made + 1], m, &u);
	played[moves_made++] = (u16)m;
	game_keys_set();
	game_check_end();
}

/* ---- the opening book: lines a player knows, as squares; each move is checked to be legal when it is taken ---- */

static const char *const book[] = {
	"e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6 e1g1 f8e7 f1e1 b7b5 a4b3 d7d6",
	"e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d3 d7d6",
	"e2e4 e7e5 g1f3 g8f6 f3e5 d7d6 e5f3 f6e4 d2d4 d6d5",
	"e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6",
	"e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g8f6 b1c3 e7e5",
	"e2e4 c7c5 g1f3 e7e6 d2d4 c5d4 f3d4 a7a6",
	"e2e4 e7e6 d2d4 d7d5 b1c3 g8f6 c1g5 f8e7 e4e5 f6d7",
	"e2e4 e7e6 d2d4 d7d5 b1d2 c7c5",
	"e2e4 c7c6 d2d4 d7d5 b1c3 d5e4 c3e4 c8f5 e4g3 f5g6",
	"e2e4 d7d6 d2d4 g8f6 b1c3 g7g6 g1f3 f8g7",
	"d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 c1g5 f8e7 e2e3 e8g8 g1f3",
	"d2d4 d7d5 c2c4 c7c6 g1f3 g8f6 b1c3 d5c4 a2a4 c8f5",
	"d2d4 d7d5 c2c4 d5c4 g1f3 g8f6 e2e3 e7e6 f1c4 c7c5",
	"d2d4 g8f6 c2c4 e7e6 b1c3 f8b4 e2e3 e8g8 f1d3 d7d5",
	"d2d4 g8f6 c2c4 e7e6 g1f3 b7b6 g2g3 c8a6",
	"d2d4 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6 g1f3 e8g8 f1e2 e7e5",
	"d2d4 g8f6 c2c4 g7g6 b1c3 d7d5 c4d5 f6d5 e2e4 d5c3 b2c3 f8g7",
	"c2c4 e7e5 b1c3 g8f6 g1f3 b8c6 g2g3 d7d5",
	"c2c4 c7c5 g1f3 g8f6 b1c3 b8c6",
	"g1f3 d7d5 g2g3 g8f6 f1g2 e7e6 e1g1 f8e7",
	"g1f3 g8f6 c2c4 g7g6 b1c3 f8g7 e2e4 d7d6",
};

static uint32_t book_seed = 1;

/* A move of the book for the game as it stands; 0 where it has none. */
static int
book_move(void)
{
	static char so_far[CH_MAX_GAME * 5 + 2];
	const char *next[sizeof book / sizeof book[0]];
	int i, n = 0, at = 0;
	u16 list[CH_MOVES];

	{
		chess_pos start;

		chess_set_fen(&start, start_fen);
		if (moves_made > 16 || start.hash1 != game[0].hash1)
			return 0;
	}
	for (i = 0; i < moves_made; i++) {
		strcpy(so_far + at, move_text(played[i]));
		at += 4;
		so_far[at++] = ' ';
	}
	so_far[at] = 0;
	for (i = 0; i < (int)(sizeof book / sizeof book[0]); i++)
		if (!strncmp(book[i], so_far, (size_t)at) && strlen(book[i]) >= (size_t)at + 4)
			next[n++] = book[i] + at;
	if (!n)
		return 0;
	book_seed ^= book_seed << 13, book_seed ^= book_seed >> 17, book_seed ^= book_seed << 5;
	{
		const char *pick = next[book_seed % (uint32_t)n];
		int k = legal_moves(&game[moves_made], list);

		for (i = 0; i < k; i++)
			if (!strncmp(move_text(list[i]), pick, 4))
				return list[i];
	}
	return 0;
}

/* ---- checks and measures, with no window ---- */

static const char *const bench[] = {
	"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
	"r1bq1rk1/pp2bppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP1N1PPP/R2QK2R w KQ - 0 8",
	"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
	"2rq1rk1/pb2bppp/1pn1pn2/2pp4/3P4/1PNBPN2/PB3PPP/2RQ1RK1 w - - 0 12",
	"8/5pk1/4p1p1/3pP2p/1P1P3P/2r3P1/5PK1/1R6 w - - 0 40",
};

/* Positions with one good move, as the tests of tactics have them: the move and what it wins by. */
static const struct { const char *fen, *best; } tactics[] = {
	{ "r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 0 1", "h5f7" },		/* the scholar's mate */
	{ "6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1", "d1d8" },						/* the back rank */
	{ "1k1r4/pp1b1R2/3q2pp/4p3/2B5/4Q3/PPP2B2/2K5 b - - 0 1", "d6d1" },				/* the first of Bratko and Kopec's: a queen given */
	{ "r1b3kr/ppp1Bp1p/1b6/n2P4/2p3q1/2Q2N2/P4PPP/RN2R1K1 w - - 0 1", "c3h8" },			/* mate in three, the queen given */
	{ "r3k3/8/8/3N4/8/8/8/4K3 w - - 0 1", "d5c7" },						/* a fork */
	{ "8/P7/8/8/8/8/k7/7K w - - 0 1", "a7a8q" },							/* a queen */
};

static uint32_t sum_add(uint32_t sum, uint32_t v) { return sum * 33 + v; }

static int
bench_perft(void)
{
	static const struct { const char *fen; int depth; u32 nodes; } t[] = {
		{ start_fen, 4, 197281 },
		{ "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862 },
		{ "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238 },
		{ "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333 },
		{ "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379 },
		{ "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 3, 89890 },
	};
	static u32 stack[2048];
	int i, bad = 0;

	for (i = 0; i < (int)(sizeof t / sizeof t[0]); i++) {
		chess_pos p;
		u32 got;

		chess_set_fen(&p, t[i].fen);
		got = chess_perft(&p, t[i].depth, stack);
		printf("perft %d: %u, %s\n", t[i].depth, got, got == t[i].nodes ? "as known" : "WRONG");
		bad += got != t[i].nodes;
	}
	printf("nxchess: perft %s\n", bad ? "FAIL" : "PASS");
	return bad != 0;
}

static void
print_think(const char *what, int i, uint64_t ms)
{
	printf("%s %d: depth %d score %d best %s nodes %u, %u ms, %u nodes a second\n", what, i, think_depth, think_score, move_text(think_best), think_nodes,
		(unsigned)ms, ms ? (unsigned)((uint64_t)think_nodes * 1000 / ms) : 0);
	fflush(stdout);
}

/* Waits for the search on the workers: ends at the time or the depth. Each look costs a read of each searcher's word. */
static void
think_wait(void)
{
	int look = 0;

	for (;;) {
		if (!(++look & 3)) {
			think_look();
			if (think_done())
				break;
		}
		mcw_rest();
	}
	think_end();
}

/* nxchess search DEPTH: the same answer and nodes each run with no workers, whose sum is the check. */
static int
bench_search(int depth)
{
	uint32_t sum = 0;
	int i;

	workers_wanted = 0;
	for (i = 0; i < (int)(sizeof bench / sizeof bench[0]); i++) {
		chess_pos p;
		uint64_t began;

		chess_set_fen(&p, bench[i]);
		began = now_ms();
		think_begin(&p, 0, depth);
		memset(ch_shared, 0, (1u << 16) * sizeof(chess_entry));
		lone_search(&p, 0, depth);
		print_think("position", i, now_ms() - began);
		sum = sum_add(sum_add(sum_add(sum, (u32)think_score), (u32)think_best), think_nodes);
	}
	printf("nxchess: sum %08x\n", sum);
	return 0;
}

/* nxchess time MS: how deep and how many nodes in that time, on the workers wanted (NXCHESS_WORKERS). */
static int
bench_time(int ms)
{
	uint64_t total_nodes = 0, total_ms = 0;
	int i, depths = 0;

	for (i = 0; i < (int)(sizeof bench / sizeof bench[0]); i++) {
		chess_pos p;
		uint64_t began;

		chess_set_fen(&p, bench[i]);
		began = now_ms();
		think_begin(&p, ms, 0);
		if (lone)
			lone_search(&p, ms, 0);
		else
			think_wait();
		print_think("position", i, now_ms() - began);
		total_nodes += think_nodes;
		total_ms += now_ms() - began;
		depths += think_depth;
	}
	printf("nxchess: %d workers (%d keepers), %u nodes a second, depth %d.%d on average\n", workers, keepers, (unsigned)(total_nodes * 1000 / (total_ms ? total_ms : 1)),
		depths / 5, depths * 2 % 10);
	return 0;
}

/* nxchess solve MS: the tactics in that time each, on the workers wanted. */
static int
bench_solve(int ms, int depth)
{
	int i, right = 0, asked = 0;

	for (i = 0; i < (int)(sizeof tactics / sizeof tactics[0]); i++) {
		chess_pos p;

		asked++;
		chess_set_fen(&p, tactics[i].fen);
		think_begin(&p, ms, depth);
		if (lone)
			lone_search(&p, ms, depth);
		else
			think_wait();
		printf("tactic %d: %s (%s) depth %d score %d\n", i, move_text(think_best), tactics[i].best, think_depth, think_score);
		fflush(stdout);
		right += !strcmp(move_text(think_best), tactics[i].best);
	}
	printf("nxchess: %d of %d right\n", right, asked);
	return right != asked;
}

/* nxchess book: every line of the book is legal, move by move. */
static int
bench_book(void)
{
	int i, bad = 0;

	for (i = 0; i < (int)(sizeof book / sizeof book[0]); i++) {
		const char *at = book[i];
		chess_pos p;

		chess_set_fen(&p, start_fen);
		while (*at) {
			u16 list[CH_MOVES];
			int n = legal_moves(&p, list), k, found = 0;
			chess_undo u;

			for (k = 0; k < n && !found; k++)
				if (!strncmp(move_text(list[k]), at, 4)) {
					chess_make(&p, list[k], &u);
					found = 1;
				}
			if (!found) {
				printf("book line %d: %.4s is not legal\n", i, at);
				bad++;
				break;
			}
			at += at[4] ? 5 : 4;
		}
	}
	printf("nxchess: book %s\n", bad ? "FAIL" : "PASS");
	return bad != 0;
}

/* ---- the window ---- */

static GR_WINDOW_ID window;
static int printing;
static int level = 3, play_as = 0, flipped;	/* play_as: 0 white, 1 black, 2 two players, 3 watch */
static int picked = -1, last_from = -1, last_to = -1, drag_from = -1;
static int key_square = 0x14, key_shown;
static u16 legal[CH_MOVES];
static int nlegal;
static int fixed_ms;		/* NXCHESS_MS: a fixed time a move */
static int stop_now;
static char status_text[80];
static uint64_t status_at;
static GR_COLOR panel_ground;

#define C_LIGHT MWRGB(238, 216, 180)
#define C_DARK MWRGB(170, 124, 84)
#define C_PICK MWRGB(120, 180, 80)
#define C_LAST_L MWRGB(226, 226, 130)
#define C_LAST_D MWRGB(186, 170, 74)
#define C_DOT MWRGB(60, 120, 60)

static const struct { const char *name; int ms, depth; } levels[7] = {
	{ "1  Beginner: 2 plies", 300, 2 }, { "2  Novice: 3 plies", 500, 3 }, { "3  Player: 5 plies", 1000, 5 }, { "4  Club: 2 s", 2000, 0 },
	{ "5  Strong: 5 s", 5000, 0 }, { "6  Expert: 15 s", 15000, 0 }, { "7  Deep: 60 s", 60000, 0 },
};

enum { C_NEW = 1, C_UNDO, C_MOVE_NOW, C_FLIP, C_EXIT, C_WHITE, C_BLACK, C_TWO, C_WATCH, C_LEVEL, C_HELP = C_LEVEL + 7, C_ABOUT };
static const char *const menu_names[] = { "Game", "Play", "Strength", "Help" };
static int menu_x[5];
static struct ui_menu_item game_items[] = {
	{ "New game   N", -1, C_NEW }, { "Undo   U", -1, C_UNDO }, { "Move now   M", -1, C_MOVE_NOW }, { "Turn the board   F", -1, C_FLIP, NULL, 0, UI_MENU_LINE },
	{ "Exit", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static struct ui_menu_item play_items[] = {
	{ "As white", -1, C_WHITE }, { "As black", -1, C_BLACK }, { "Two players", -1, C_TWO }, { "Watch", -1, C_WATCH },
};
static struct ui_menu_item level_items[7];
static const struct ui_menu_item help_items[] = { { "How to play", -1, C_HELP }, { "About Chess", -1, C_ABOUT } };
static const struct { const struct ui_menu_item *items; int count; } menus[4] = { { game_items, 5 }, { play_items, 4 }, { level_items, 7 }, { help_items, 2 } };

static int
board_x(int sq)
{
	return BX + (flipped ? 7 - (sq & 7) : sq & 7) * SQ;
}

static int
board_y(int sq)
{
	return BY + (flipped ? sq >> 4 : 7 - (sq >> 4)) * SQ;
}

static int
square_at(int x, int y)
{
	int c, r;

	if (x < BX || y < BY || x >= BX + 8 * SQ || y >= BY + 8 * SQ)
		return -1;
	c = (x - BX) / SQ, r = (y - BY) / SQ;
	return (flipped ? r : 7 - r) * 16 + (flipped ? 7 - c : c);
}

static void
draw_square(int sq)
{
	const chess_pos *p = &game[moves_made];
	int x = board_x(sq), y = board_y(sq), dark = !(((sq >> 4) + sq) & 1), piece = p->board[sq], i;
	GR_COLOR ground = dark ? C_DARK : C_LIGHT;

	if (sq == picked)
		ground = C_PICK;
	else if (sq == last_from || sq == last_to)
		ground = dark ? C_LAST_D : C_LAST_L;
	ui_fill(window, x, y, SQ, SQ, ground);
	if (piece) {
		int side = SIDE(piece), type = TYPE(piece) - 1, in = (SQ - PIECE_SIZE) / 2;

		GrSetGCForeground(ui_gc, side ? MWRGB(240, 240, 240) : WHITE);
		GrBitmap(window, ui_gc, x + in, y + in, PIECE_SIZE, PIECE_SIZE, (GR_BITMAP *)piece_art[side][type][0]);
		GrSetGCForeground(ui_gc, BLACK);
		GrBitmap(window, ui_gc, x + in, y + in, PIECE_SIZE, PIECE_SIZE, (GR_BITMAP *)piece_art[side][type][1]);
	}
	if (picked >= 0)
		for (i = 0; i < nlegal; i++)
			if (M_FROM(legal[i]) == picked && M_TO(legal[i]) == sq) {
				GrSetGCForeground(ui_gc, C_DOT);
				GrFillEllipse(window, ui_gc, x + SQ / 2, y + SQ / 2, 6, 6);
				break;
			}
	if (key_shown && sq == key_square) {
		GrSetGCForeground(ui_gc, MWRGB(30, 80, 200));
		GrRect(window, ui_gc, x + 1, y + 1, SQ - 2, SQ - 2);
		GrRect(window, ui_gc, x + 2, y + 2, SQ - 4, SQ - 4);
	}
}

static void
draw_board(void)
{
	int r, c;
	char t[2] = { 0, 0 };

	for (r = 0; r < 8; r++)
		for (c = 0; c < 8; c++)
			draw_square(r * 16 + c);
	GrSetGCForeground(ui_gc, BLACK);
	GrRect(window, ui_gc, BX - 1, BY - 1, 8 * SQ + 2, 8 * SQ + 2);
	for (c = 0; c < 8; c++) {
		t[0] = (char)('a' + (flipped ? 7 - c : c));
		ui_text(window, BX + c * SQ + SQ / 2 - 3, BY + 8 * SQ + 4, t, 1, BLACK, 0);
		t[0] = (char)('1' + (flipped ? c : 7 - c));
		ui_text(window, 8, BY + c * SQ + SQ / 2 - 7, t, 1, BLACK, 0);
	}
}

static const char *
end_text(void)
{
	switch (over_kind) {
	case END_MATE: return game[moves_made].side ? "Checkmate: white wins" : "Checkmate: black wins";
	case END_STALE: return "Stalemate: a draw";
	case END_REPEAT: return "A draw: repeated";
	case END_FIFTY: return "A draw: fifty moves";
	case END_MATERIAL: return "A draw: nothing to mate with";
	}
	return NULL;
}

#define PX (BX + 8 * SQ + 12)
#define MOVES_Y (BY + 76)
#define MOVE_ROW 15
#define MOVE_ROWS ((WIN_H - MOVES_Y - 8) / MOVE_ROW)

static void
draw_status(void)
{
	char line[64];
	const char *end = end_text();

	ui_fill(window, PX, BY, PANEL_W, 70, panel_ground);
	if (end)
		ui_text(window, PX + 4, BY + 4, end, -1, BLACK, 0);
	else {
		snprintf(line, sizeof line, "%s to move%s", game[moves_made].side ? "Black" : "White", IN_CHECK(&game[moves_made]) ? ": check" : "");
		ui_text(window, PX + 4, BY + 4, line, -1, BLACK, 0);
	}
	ui_text(window, PX + 4, BY + 22, levels[level - 1].name, -1, UI_SHADOW, 0);
	ui_text(window, PX + 4, BY + 40, status_text, -1, BLACK, 0);
}

static void
draw_moves(void)
{
	int first = (moves_made + 1) / 2 - MOVE_ROWS, r, y;

	first = first < 0 ? 0 : first;
	ui_fill(window, PX, MOVES_Y, PANEL_W, WIN_H - MOVES_Y - 4, WHITE);
	for (r = 0, y = MOVES_Y + 2; r < MOVE_ROWS && first * 2 < moves_made; r++, first++, y += MOVE_ROW) {
		char n[8];

		snprintf(n, sizeof n, "%d.", first + 1);
		ui_text(window, PX + 4, y, n, -1, UI_SHADOW, 0);
		ui_text(window, PX + 36, y, san[first * 2], -1, BLACK, 0);
		if (first * 2 + 1 < moves_made)
			ui_text(window, PX + 112, y, san[first * 2 + 1], -1, BLACK, 0);
	}
}

static void
draw_all(void)
{
	int i;

	ui_fill(window, 0, 0, WIN_W, MENU_H, UI_FACE);
	for (i = 0; i < 4; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, WIN_W, 1, UI_SHADOW);
	ui_fill(window, 0, MENU_H, WIN_W, WIN_H - MENU_H, UI_FACE);
	panel_ground = UI_FACE;
	draw_board();
	draw_status();
	draw_moves();
}

static void
measure_menus(void)
{
	int i, x;

	for (i = 0, x = 2; i < 4; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[4] = x;
}

static int
engine_to_move(void)
{
	if (over_kind)
		return 0;
	return play_as == 3 || (play_as < 2 && game[moves_made].side != play_as);
}

static void
print_state(const char *what)
{
	char fen_sum[16];

	if (!printing)
		return;
	snprintf(fen_sum, sizeof fen_sum, "%08x", game[moves_made].hash1);
	printf("nxchess: %s: %d moves, last %s %s, position %s, level %d, as %d%s%s\n", what, moves_made, moves_made ? san[moves_made - 1] : "-",
		moves_made ? move_text(played[moves_made - 1]) : "-", fen_sum, level, play_as, end_text() ? ", " : "", end_text() ? end_text() : "");
	fflush(stdout);
}

/* The squares the last move changed: a castling and a capture in passing are more than two. */
static void
draw_changed(void)
{
	int r, c;

	for (r = 0; r < 8; r++)
		for (c = 0; c < 8; c++) {
			int sq = r * 16 + c;

			if (game[moves_made].board[sq] != game[moves_made - 1].board[sq])
				draw_square(sq);
		}
}

static void
pick_clear(void)
{
	int i, old = picked;

	if (old < 0)
		return;
	picked = -1;
	for (i = 0; i < nlegal; i++)
		if (M_FROM(legal[i]) == old)
			draw_square(M_TO(legal[i]));
	draw_square(old);
	nlegal = 0;
}

static void
after_move(void)
{
	int m = played[moves_made - 1], old_from = last_from, old_to = last_to;

	pick_clear();
	last_from = M_FROM(m), last_to = M_TO(m);
	draw_changed();
	if (old_from >= 0)
		draw_square(old_from), draw_square(old_to);
	draw_status();
	draw_moves();
	GrFlush();
}

static void
pick(int sq)
{
	int i;

	pick_clear();
	picked = sq;
	nlegal = legal_moves(&game[moves_made], legal);
	draw_square(sq);
	for (i = 0; i < nlegal; i++)
		if (M_FROM(legal[i]) == sq)
			draw_square(M_TO(legal[i]));
}

/* What it thinks, from white's side, as a board's reader takes it. */
static void
think_status(void)
{
	char score[24] = "";

	if (think_depth) {
		int white = think_side ? -think_score : think_score;

		if (CH_IS_MATE(think_score))
			snprintf(score, sizeof score, "mate in %d", (CH_MATE - abs(think_score) + 1) / 2);
		else
			snprintf(score, sizeof score, "%c%d.%02d", white < 0 ? '-' : '+', abs(white) / 100, abs(white) % 100);
	}
	snprintf(status_text, sizeof status_text, "depth %d  %s", think_depth, score);
}

static void
think_status_line(uint32_t nodes, uint64_t ms)
{
	char more[48];

	think_status();
	snprintf(more, sizeof more, ", %u kn/s", ms ? (unsigned)(nodes / ms) : 0);
	if (strlen(status_text) + strlen(more) < sizeof status_text)
		strcat(status_text, more);
	draw_status();
	if (workers) {
		snprintf(more, sizeof more, "%d workers", workers);
		ui_text(window, PX + 4, BY + 54, more, -1, UI_SHADOW, 0);
	}
}

static void command(int id);
static void game_event(GR_EVENT *event);

/* A look at the window from the first core's own search: every 256 nodes. */
static int
events_while_alone(void)
{
	static int count;
	GR_EVENT event;

	stop_now = 0;
	if (++count & 7)
		return 0;
	while (GrPeekEvent(&event)) {
		GrGetNextEvent(&event);
		game_event(&event);
	}
	return stop_now;
}

static void
engine_play(void)
{
	int m = think_best;

	if (!m) {
		u16 list[CH_MOVES];

		if (!legal_moves(&game[moves_made], list))
			return;
		m = list[0];
	}
	game_play(m);
	think_status_line(think_nodes, now_ms() - think_began + 1);
	after_move();
	print_state("engine");
	if (printing) {
		printf("nxchess: said depth %d score %d nodes %u workers %d\n", think_depth, think_score, think_nodes, workers);
		fflush(stdout);
	}
}

static void
engine_start(void)
{
	int ms = fixed_ms ? fixed_ms : levels[level - 1].ms, m = getenv("NXCHESS_BOOK") && !atoi(getenv("NXCHESS_BOOK")) ? 0 : book_move();

	if (m) {
		think_best = m;
		think_nodes = 0;
		think_depth = 0;
		snprintf(status_text, sizeof status_text, "from the book");
		game_play(m);
		after_move();
		draw_status();
		print_state("book");
		return;
	}
	snprintf(status_text, sizeof status_text, "thinking");
	draw_status();
	GrFlush();
	think_depth = 0, think_score = 0, think_best = 0;
	think_side = game[moves_made].side;
	think_begin(&game[moves_made], ms, levels[level - 1].depth);
	if (lone) {
		lone_events = events_while_alone;
		lone_search(&game[moves_made], ms, levels[level - 1].depth);
		if (!stop_now || moves_made)
			engine_play();
	}
	status_at = now_ms();
}

/* Back to the human's move: a search under way ends first and its answer is dropped. */
static void
think_cancel(void)
{
	if (thinking && !lone)
		think_end();
	thinking = 0;
	stop_now = 1;
}

static void
undo(void)
{
	int back = play_as < 2 && moves_made >= 2 && !engine_to_move() ? 2 : 1;

	think_cancel();
	if (moves_made < back)
		back = moves_made;
	if (!back)
		return;
	moves_made -= back;
	game_keys_set();
	over_kind = 0;
	game_check_end();
	picked = -1, nlegal = 0;
	last_from = last_to = -1;
	if (moves_made) {
		last_from = M_FROM(played[moves_made - 1]);
		last_to = M_TO(played[moves_made - 1]);
	}
	status_text[0] = 0;
	draw_all();
	print_state("undo");
}

static void
new_game(void)
{
	think_cancel();
	game_new(getenv("NXCHESS_FEN") ? getenv("NXCHESS_FEN") : start_fen);
	picked = -1, nlegal = 0, last_from = last_to = -1;
	status_text[0] = 0;
	draw_all();
	print_state("new game");
}

static void
try_move(int from, int to)
{
	int i, m = 0, promotions = 0;
	static const char *const answers[4] = { "Queen", "Knight", "Rook", "Bishop" };

	for (i = 0; i < nlegal; i++)
		if (M_FROM(legal[i]) == from && M_TO(legal[i]) == to) {
			m = legal[i];
			promotions += (legal[i] & PROMO_BIT) != 0;
		}
	if (!m) {
		pick_clear();
		return;
	}
	if (promotions) {
		int a = getenv("NXCHESS_PROMOTE") ? atoi(getenv("NXCHESS_PROMOTE")) : ui_ask("Promotion", "What should the pawn become?", answers, 4, NULL);

		GrSetFocus(window);
		a = a < 0 ? 0 : a;
		m = (m & 0x0fff) | PROMO_BIT | a << 12;
	}
	pick_clear();
	game_play(m);
	after_move();
	print_state("played");
}

static void
square_clicked(int sq)
{
	const chess_pos *p = &game[moves_made];

	if (over_kind || engine_to_move())
		return;
	if (picked >= 0 && picked != sq && (!p->board[sq] || SIDE(p->board[sq]) != p->side)) {
		try_move(picked, sq);
		return;
	}
	if (picked == sq) {
		pick_clear();
		return;
	}
	if (p->board[sq] && SIDE(p->board[sq]) == p->side)
		pick(sq);
	else
		pick_clear();
}

static void
set_flipped_for_side(void)
{
	flipped = play_as == 1;
}

static void
command(int id)
{
	static const char *const ok[1] = { "OK" };

	switch (id) {
	case C_NEW:
		new_game();
		break;
	case C_UNDO:
		undo();
		break;
	case C_MOVE_NOW:
		if (thinking && !lone)
			think_stop_at = 0, think_target = 0;
		stop_now = 1;
		break;
	case C_FLIP:
		flipped = !flipped;
		draw_all();
		break;
	case C_WHITE: case C_BLACK: case C_TWO: case C_WATCH:
		think_cancel();
		play_as = id - C_WHITE;
		reg_set_int("nxchess.play", play_as);
		set_flipped_for_side();
		draw_all();
		print_state("play");
		break;
	case C_HELP:
		ui_ask("How to play", "Click a piece, then where it goes, or drag it; or the arrows and Space.\n"
			"N a new game, U takes back, M tells it to move now, F turns the board.\n"
			"Strength: at its first three it looks only so far. At the others it has\n"
			"so many seconds a move, and every worker core of the machine looks with it.", ok, 1, NULL);
		GrSetFocus(window);
		break;
	case C_ABOUT:
		ui_ask("About Chess", "Chess for the ShaderX86 desktop.\nThe search is on every worker core.\nPieces from DejaVu Sans.", ok, 1, NULL);
		GrSetFocus(window);
		break;
	case C_EXIT:
		think_cancel();
		GrClose();
		exit(0);
	default:
		if (id >= C_LEVEL && id < C_LEVEL + 7) {
			level = id - C_LEVEL + 1;
			reg_set_int("nxchess.level", level);
			draw_status();
			print_state("level");
		}
	}
}

static void
open_menu(int which, int keyboard)
{
	int wx, wy, i;

	for (i = 0; i < 4; i++)
		play_items[i].flags = (play_items[i].flags & ~UI_MENU_CHECK) | (play_as == i ? UI_MENU_CHECK : 0);
	for (i = 0; i < 7; i++)
		level_items[i].flags = level == i + 1 ? UI_MENU_CHECK : 0;
	ui_window_origin(window, &wx, &wy);
	ui_menu_open(menus[which].items, menus[which].count, wx + menu_x[which], wy + MENU_H, 0, keyboard);
}

static void
key_move(int dfile, int drank)
{
	int old = key_square, f = (old & 7) + (flipped ? -dfile : dfile), r = (old >> 4) + (flipped ? -drank : drank);

	key_square = ((r + 8) & 7) * 16 + ((f + 8) & 7);
	key_shown = 1;
	draw_square(old);
	draw_square(key_square);
}

static void
key(int ch)
{
	switch (ch) {
	case 'n': case 'N': case MWKEY_F2: command(C_NEW); break;
	case 'u': case 'U': case MWKEY_BACKSPACE: command(C_UNDO); break;
	case 'm': case 'M': command(C_MOVE_NOW); break;
	case 'f': case 'F': command(C_FLIP); break;
	case MWKEY_F10: open_menu(0, 1); break;
	case MWKEY_LEFT: key_move(-1, 0); break;
	case MWKEY_RIGHT: key_move(1, 0); break;
	case MWKEY_UP: key_move(0, 1); break;
	case MWKEY_DOWN: key_move(0, -1); break;
	case ' ': case MWKEY_ENTER:
		if (!key_shown)
			key_move(0, 0);
		else
			square_clicked(key_square);
		break;
	case MWKEY_ESCAPE:
		pick_clear();
		if (key_shown) {
			key_shown = 0;
			draw_square(key_square);
		}
		break;
	}
}

/* What the window said, in any state. */
static void
game_event(GR_EVENT *event)
{
	int id, i, sq;

	ui_origin_note(event);
	if (event->type != GR_EVENT_TYPE_TIMEOUT && ui_restyle()) {
		ui_menu_restyle();
		measure_menus();
		draw_all();
	}
	switch (ui_menu_event(event, &id)) {
	case UI_MENU_CHOSEN:
		GrSetFocus(window);
		command(id);
		return;
	case UI_MENU_CLOSED:
		GrSetFocus(window);
		return;
	case UI_MENU_TOOK:
		return;
	}
	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		if (event->exposure.wid == window)
			draw_all();
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if (event->button.wid != window)
			break;
		if (event->button.y < MENU_H) {
			for (i = 0; i < 4; i++)
				if (event->button.x >= menu_x[i] && event->button.x < menu_x[i + 1])
					open_menu(i, 0);
			break;
		}
		sq = square_at(event->button.x, event->button.y);
		if (sq >= 0) {
			square_clicked(sq);
			drag_from = picked == sq ? sq : -1;
		}
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		if (event->button.wid == window && drag_from >= 0) {
			sq = square_at(event->button.x, event->button.y);
			if (sq >= 0 && sq != drag_from && picked == drag_from)
				square_clicked(sq);
			drag_from = -1;
		}
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		if (event->keystroke.wid == window)
			key(event->keystroke.ch);
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		if (event->general.wid == window)
			command(C_EXIT);
		break;
	}
}

static int
run_window(void)
{
	GR_EVENT event;
	int i;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	if (ui_single_instance()) {
		GrClose();
		return 0;
	}
	lend_workers = 1;
	atexit(mcw_close);
	printing = getenv("NXCHESS_PRINT") ? atoi(getenv("NXCHESS_PRINT")) : 0;
	fixed_ms = getenv("NXCHESS_MS") ? atoi(getenv("NXCHESS_MS")) : 0;
	level = reg_int("nxchess.level", 3);
	level = level < 1 || level > 7 ? 3 : level;
	play_as = reg_int("nxchess.play", 0);
	play_as = play_as < 0 || play_as > 3 ? 0 : play_as;
	book_seed = getenv("NXCHESS_SEED") ? (uint32_t)strtoul(getenv("NXCHESS_SEED"), NULL, 0) : (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16;
	book_seed = book_seed ? book_seed : 1;
	set_flipped_for_side();
	for (i = 0; i < 7; i++) {
		level_items[i].label = levels[i].name;
		level_items[i].icon = -1;
		level_items[i].id = C_LEVEL + i;
	}
	measure_menus();
	game_new(getenv("NXCHESS_FEN") ? getenv("NXCHESS_FEN") : start_fen);
	window = GrNewWindowEx(UI_FIXED, "Chess", GR_ROOT_WINDOW_ID, -1, -1, WIN_W, WIN_H, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	if (printing) {
		printf("nxchess: %d x %d, a1 at %d,%d in the window, squares of %d, level %d, as %d\n", WIN_W, WIN_H, BX, BY + 7 * SQ, SQ, level, play_as);
		fflush(stdout);
	}
	for (;;) {
		if (thinking && !lone) {
			GrGetNextEventTimeout(&event, 40);
			if (event.type != GR_EVENT_TYPE_TIMEOUT)
				game_event(&event);
			if (thinking && !lone) {
				uint64_t now = now_ms();

				think_look();
				if (think_done()) {
					think_end();
					engine_play();
				} else if (now - status_at >= 500) {
					status_at = now;
					think_status_line(mcw_get(0), now - think_began + 1);
				}
			}
			continue;
		}
		if (engine_to_move() && !thinking) {
			engine_start();
			continue;
		}
		GrGetNextEvent(&event);
		game_event(&event);
	}
}

int
main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "";

	chess_tables();
	if (getenv("NXCHESS_WORKERS"))
		workers_wanted = atoi(getenv("NXCHESS_WORKERS"));
	if (!strcmp(what, "perft"))
		return bench_perft();
	if (!strcmp(what, "book"))
		return bench_book();
	if (!strcmp(what, "search"))
		return bench_search(argc > 2 ? atoi(argv[2]) : 7);
	if (!strcmp(what, "time"))
		return bench_time(argc > 2 ? atoi(argv[2]) : 5000);
	if (!strcmp(what, "solve"))
		return bench_solve(argc > 2 ? atoi(argv[2]) : 2000, argc > 3 ? atoi(argv[3]) : 0);
	return run_window();
}
