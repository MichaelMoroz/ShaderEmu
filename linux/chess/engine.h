/*
 * The chess engine of nxchess (docs/games/chess.md): written for this machine, with no code of
 * another's. A board of 0x88 squares in which each rank is one texel, piece lists, 32-bit
 * words only: the machine has no 64-bit registers and no bit-scan, so bitboards would be its
 * slowest form. No system call and no library but memcpy/memset: a worker runs all of it.
 *
 * Everything a search stores is in its `chess_core`: a core has one of its own.
 */
#ifndef CHESS_ENGINE_H
#define CHESS_ENGINE_H

#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int8_t s8;

#define CH_INF 30000
#define CH_MATE 29000
#define CH_IS_MATE(s) ((s) > CH_MATE - 512 || (s) < -CH_MATE + 512)
#define CH_MAX_PLY 96
#define CH_MAX_GAME 512
#define CH_MOVES 256

enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };
#define PIECE(side, type) ((side) << 3 | (type))
#define TYPE(p) ((p) & 7)
#define SIDE(p) ((p) >> 3)
#define BAD(sq) ((sq) & 0x88)
#define S88(s64) (((s64) + ((s64) & 56)))		/* 0 to 63 into 0x88 */
#define S64(s88) (((s88) + ((s88) & 7)) >> 1)		/* and back */

/* A move: the squares as 0 to 63 and, for a promotion, what to: 0 queen, 1 knight, 2 rook, 3 bishop. 0 is "no move". */
#define MOVE(from, to) ((u16)(S64(from) | S64(to) << 6))
#define M_FROM(m) (S88((m) & 63))
#define M_TO(m) (S88((m) >> 6 & 63))
#define M_PROMO(m) ((m) >> 12 & 3)

typedef struct {
	u8 board[128];		/* in a square of 0x88 its piece, 8 further its place in the list */
	u8 list[2][16];		/* where each side's pieces are; the king is always first */
	u32 hash0, hash1;
	u32 pawn_key;		/* of the pawns alone */
	s16 mg, eg;		/* material and places, white's less black's */
	u8 count[2];
	u8 side, castle, ep, half;	/* castle: 1 2 white's short, long, 4 8 black's; ep: 0x88 square or 0xff */
	s8 phase;
	u8 big[2];		/* pieces that are not pawns */
	u8 pad;
} chess_pos;

typedef struct {
	u32 hash0, hash1, pawn_key;
	s16 mg, eg;
	u8 castle, ep, half, captured, cap_at;
	s8 phase;
} chess_undo;

/* ---- tables made at the start: read by every core ---- */

static u32 zob[16][64][2], zob_castle[16][2], zob_ep[8][2], zob_side[2];
static s8 attack_dir[240];		/* the step from a square toward another 0x77 + diff away, 0: not on a line */
static u8 attack_kind[240];		/* which pieces can attack across that difference, bit of the type (bit 0: white pawn, bit 7: black) */
static s16 place_mg[16][64], place_eg[16][64];	/* value and place of a piece on a square, signed for white's sum */
static const s8 knight_step[8] = { 31, 33, 14, 18, -31, -33, -14, -18 };
static const s8 line_step[8] = { 1, -1, 16, -16, 15, 17, -15, -17 };	/* the rook's four, then the bishop's */
static const u8 castle_keep[128] = {
	[0x00] = 13, [0x04] = 12, [0x07] = 14, [0x70] = 7, [0x74] = 3, [0x77] = 11,
};	/* what a move from or to a square leaves of the castling (0 elsewhere means all: see make) */
#define KEEP(sq) (castle_keep[sq] ? castle_keep[sq] : 15)

static const s16 value_mg[7] = { 0, 90, 325, 345, 490, 980, 0 }, value_eg[7] = { 0, 115, 300, 320, 520, 960, 0 };
static const u8 phase_of[7] = { 0, 0, 1, 1, 2, 4, 0 };

/* Places, from white's side, a8 first as one reads a board; written by hand for this engine. */
static const s8 table_mg[6][64] = {
	{  0,  0,  0,  0,  0,  0,  0,  0,  40, 45, 45, 50, 50, 45, 45, 40,  12, 16, 22, 28, 28, 22, 16, 12,   4,  6, 10, 22, 22, 10,  6,  4,
	   0,  0,  4, 18, 18,  2,  0,  0,   2, -2, -4,  2,  2, -6, -2,  2,   2,  6,  6,-14,-14,  8,  6,  2,   0,  0,  0,  0,  0,  0,  0,  0 },
	{-50,-35,-25,-20,-20,-25,-35,-50, -35,-15,  0,  5,  5,  0,-15,-35, -25,  5, 12, 18, 18, 12,  5,-25, -18,  5, 16, 22, 22, 16,  5,-18,
	 -18,  2, 14, 20, 20, 14,  2,-18, -22,  6, 12, 12, 12, 12,  6,-22, -32,-14,  2,  6,  6,  2,-14,-32, -50,-28,-22,-20,-20,-22,-28,-50 },
	{-18,-10,-10,-10,-10,-10,-10,-18, -10,  0,  0,  0,  0,  0,  0,-10, -10,  0,  6,  8,  8,  6,  0,-10, -10,  5,  6, 10, 10,  6,  5,-10,
	 -10,  2, 10, 10, 10, 10,  2,-10, -10, 10,  8,  8,  8,  8, 10,-10, -10,  8,  2,  2,  2,  2,  8,-10, -18,-10,-12,-10,-10,-12,-10,-18 },
	{  2,  4,  4,  6,  6,  4,  4,  2,  10, 14, 14, 14, 14, 14, 14, 10,  -4,  0,  0,  0,  0,  0,  0, -4,  -4,  0,  0,  0,  0,  0,  0, -4,
	  -4,  0,  0,  0,  0,  0,  0, -4,  -4,  0,  0,  0,  0,  0,  0, -4,  -4,  0,  0,  2,  2,  0,  0, -4,  -2,  0,  4,  8,  8,  4,  0, -2 },
	{-16,-10,-10, -4, -4,-10,-10,-16, -10,  0,  0,  0,  0,  0,  0,-10, -10,  0,  4,  4,  4,  4,  0,-10,  -4,  0,  4,  4,  4,  4,  0, -4,
	  -2,  0,  4,  4,  4,  4,  0, -4, -10,  4,  4,  4,  4,  4,  0,-10, -10,  0,  4,  0,  0,  0,  0,-10, -16,-10,-10, -4, -4,-10,-10,-16 },
	{-40,-45,-45,-50,-50,-45,-45,-40, -35,-40,-40,-50,-50,-40,-40,-35, -30,-35,-35,-45,-45,-35,-35,-30, -25,-30,-35,-45,-45,-35,-30,-25,
	 -15,-22,-25,-30,-30,-25,-22,-15, -10,-15,-18,-22,-22,-18,-15,-10,  12, 10, -4,-12,-12, -4, 10, 12,  14, 30,  8, -8,  0, -6, 30, 18 },
};
static const s8 table_eg[6][64] = {
	{  0,  0,  0,  0,  0,  0,  0,  0,  70, 65, 60, 55, 55, 60, 65, 70,  40, 36, 30, 26, 26, 30, 36, 40,  18, 14, 10,  6,  6, 10, 14, 18,
	   8,  6,  2,  0,  0,  2,  6,  8,   2,  2,  0,  2,  2,  0,  2,  2,   4,  4,  4,  6,  6,  4,  4,  4,   0,  0,  0,  0,  0,  0,  0,  0 },
	{-40,-28,-18,-14,-14,-18,-28,-40, -28,-12,  0,  2,  2,  0,-12,-28, -18,  0, 10, 14, 14, 10,  0,-18, -14,  4, 14, 18, 18, 14,  4,-14,
	 -14,  2, 12, 18, 18, 12,  2,-14, -18,  0,  6, 12, 12,  6,  0,-18, -28,-12,  0,  2,  2,  0,-12,-28, -40,-28,-18,-14,-14,-18,-28,-40 },
	{-14, -8, -8, -6, -6, -8, -8,-14,  -8, -2,  0,  0,  0,  0, -2, -8,  -8,  0,  4,  4,  4,  4,  0, -8,  -6,  2,  4,  6,  6,  4,  2, -6,
	  -6,  2,  4,  6,  6,  4,  2, -6,  -8,  0,  4,  4,  4,  4,  0, -8,  -8, -2,  0,  0,  0,  0, -2, -8, -14, -8, -8, -6, -6, -8, -8,-14 },
	{  6,  6,  6,  6,  6,  6,  6,  6,   6,  6,  6,  6,  6,  6,  6,  6,   2,  2,  2,  2,  2,  2,  2,  2,   2,  2,  2,  2,  2,  2,  2,  2,
	   0,  0,  0,  0,  0,  0,  0,  0,  -2, -2, -2, -2, -2, -2, -2, -2,  -4, -4, -4, -4, -4, -4, -4, -4,  -4, -4, -2, -2, -2, -2, -4, -4 },
	{ -8, -4, -4, -2, -2, -4, -4, -8,  -4,  2,  4,  4,  4,  4,  2, -4,  -4,  4,  8,  8,  8,  8,  4, -4,  -2,  4,  8, 10, 10,  8,  4, -2,
	  -2,  4,  8, 10, 10,  8,  4, -2,  -4,  4,  8,  8,  8,  8,  4, -4,  -4,  2,  4,  4,  4,  4,  2, -4,  -8, -4, -4, -2, -2, -4, -4, -8 },
	{-40,-20,-12, -8, -8,-12,-20,-40, -14,  6, 10, 12, 12, 10,  6,-14,  -8, 12, 20, 24, 24, 20, 12, -8,  -6, 14, 24, 30, 30, 24, 14, -6,
	  -8, 10, 20, 26, 26, 20, 10, -8, -12,  4, 12, 16, 16, 12,  4,-12, -20, -6,  2,  6,  6,  2, -6,-20, -40,-26,-16,-12,-12,-16,-26,-40 },
};

static u32 zob_state = 0x9e3779b9u;
static u32
zob_next(void)
{
	zob_state ^= zob_state << 13, zob_state ^= zob_state >> 17, zob_state ^= zob_state << 5;
	return zob_state;
}

static void
chess_tables(void)
{
	int p, s, i, k, t;

	for (p = 0; p < 16; p++)
		for (s = 0; s < 64; s++)
			for (i = 0; i < 2; i++)
				zob[p][s][i] = zob_next();
	for (i = 0; i < 2; i++) {
		for (k = 0; k < 16; k++)
			zob_castle[k][i] = zob_next();
		for (k = 0; k < 8; k++)
			zob_ep[k][i] = zob_next();
		zob_side[i] = zob_next();
	}
	for (t = PAWN; t <= KING; t++)
		for (s = 0; s < 64; s++) {
			int up = 56 - (s & 56) + (s & 7);	/* a8 first in the tables; s 0 is a1 */
			int mg = value_mg[t] + table_mg[t - 1][up], eg = value_eg[t] + table_eg[t - 1][up];

			place_mg[PIECE(0, t)][s] = (s16)mg, place_eg[PIECE(0, t)][s] = (s16)eg;
			place_mg[PIECE(1, t)][s ^ 56] = (s16)-mg, place_eg[PIECE(1, t)][s ^ 56] = (s16)-eg;
		}
	for (k = 0; k < 8; k++) {
		int step = line_step[k];

		for (i = 1; i < 8; i++) {
			attack_dir[119 + step * i] = (s8)step;
			attack_kind[119 + step * i] |= 1 << QUEEN | (k < 4 ? 1 << ROOK : 1 << BISHOP) | (i == 1 ? 1 << KING : 0);
		}
	}
	for (k = 0; k < 8; k++) {
		attack_kind[119 + knight_step[k]] |= 1 << KNIGHT;
		attack_dir[119 + knight_step[k]] = knight_step[k];
	}
	/* a pawn of white's attacks 15 and 17 up: what it stands from what it attacks, target - from */
	attack_kind[119 + 15] |= 1 << 0, attack_kind[119 + 17] |= 1 << 0;
	attack_kind[119 - 15] |= 1 << 7, attack_kind[119 - 17] |= 1 << 7;
}

/* Whether a piece of `by` attacks the square. */
static inline int
chess_attacked(const chess_pos *p, int sq, int by)
{
	const u8 *list = p->list[by];
	int i, n = p->count[by];

	for (i = 0; i < n; i++) {
		int from = list[i], d = sq - from + 119, kinds = attack_kind[d], type;

		if (!kinds)
			continue;
		type = TYPE(p->board[from]);
		if (type == PAWN) {
			if (kinds & (by ? 0x80 : 1))
				return 1;
		} else if (kinds & 1 << type) {
			int step = attack_dir[d], at;

			if (type == KNIGHT || type == KING)
				return 1;
			for (at = from + step; at != sq && !p->board[at]; at += step)
				;
			if (at == sq)
				return 1;
		}
	}
	return 0;
}

#define KING_AT(p, side) ((p)->list[side][0])
#define IN_CHECK(p) chess_attacked(p, KING_AT(p, (p)->side), (p)->side ^ 1)

static inline void
put_piece(chess_pos *p, int piece, int sq)
{
	int s = S64(sq), side = SIDE(piece), at = p->count[side]++;

	p->board[sq] = (u8)piece;
	p->board[sq + 8] = (u8)at;
	p->list[side][at] = (u8)sq;
	p->hash0 ^= zob[piece][s][0], p->hash1 ^= zob[piece][s][1];
	if (TYPE(piece) == PAWN)
		p->pawn_key ^= zob[piece][s][1];
	p->mg += place_mg[piece][s], p->eg += place_eg[piece][s];
	p->phase += phase_of[TYPE(piece)];
	p->big[side] += TYPE(piece) != PAWN;
}

/* Sets the position from a FEN; 0 for what cannot be read. */
static int
chess_set_fen(chess_pos *p, const char *fen)
{
	int rank = 7, file = 0, kings = 0;
	const char *names = " PNBRQK";

	memset(p, 0, sizeof *p);
	p->ep = 0xff;
	/* the kings first, whatever the order: the king is the first of its list */
	{
		const char *f = fen;
		int r = 7, c = 0;

		for (; *f && *f != ' '; f++) {
			if (*f == '/')
				r--, c = 0;
			else if (*f >= '1' && *f <= '8')
				c += *f - '0';
			else {
				if ((*f == 'K' || *f == 'k') && r >= 0 && c < 8) {
					put_piece(p, PIECE(*f == 'k', KING), r * 16 + c);
					kings++;
				}
				c++;
			}
		}
	}
	if (kings != 2 || p->count[0] != 1 || p->count[1] != 1)
		return 0;
	for (; *fen && *fen != ' '; fen++) {
		if (*fen == '/')
			rank--, file = 0;
		else if (*fen >= '1' && *fen <= '8')
			file += *fen - '0';
		else {
			int side = *fen >= 'a', ch = side ? *fen - 32 : *fen, t;

			for (t = 1; t < 7 && names[t] != ch; t++)
				;
			if (t == 7 || rank < 0 || file > 7)
				return 0;
			if (t != KING)
				put_piece(p, PIECE(side, t), rank * 16 + file);
			file++;
		}
	}
	while (*fen == ' ')
		fen++;
	p->side = *fen == 'b';
	if (*fen)
		fen++;
	while (*fen == ' ')
		fen++;
	for (; *fen && *fen != ' '; fen++)
		p->castle |= *fen == 'K' ? 1 : *fen == 'Q' ? 2 : *fen == 'k' ? 4 : *fen == 'q' ? 8 : 0;
	while (*fen == ' ')
		fen++;
	if (*fen >= 'a' && *fen <= 'h' && fen[1] >= '1' && fen[1] <= '8') {
		p->ep = (u8)((fen[1] - '1') * 16 + fen[0] - 'a');
		fen += 2;
	} else if (*fen)
		fen++;
	while (*fen == ' ')
		fen++;
	for (; *fen >= '0' && *fen <= '9'; fen++)
		p->half = (u8)(p->half * 10 + *fen - '0');
	p->hash0 ^= zob_castle[p->castle][0], p->hash1 ^= zob_castle[p->castle][1];
	if (p->ep != 0xff)
		p->hash0 ^= zob_ep[p->ep & 7][0], p->hash1 ^= zob_ep[p->ep & 7][1];
	if (p->side)
		p->hash0 ^= zob_side[0], p->hash1 ^= zob_side[1];
	return 1;
}

static inline void
move_piece(chess_pos *p, int piece, int from, int to)
{
	int a = S64(from), b = S64(to), at = p->board[from + 8];

	p->board[from] = 0;
	p->board[to] = (u8)piece;
	p->board[to + 8] = (u8)at;
	p->list[SIDE(piece)][at] = (u8)to;
	p->hash0 ^= zob[piece][a][0] ^ zob[piece][b][0];
	p->hash1 ^= zob[piece][a][1] ^ zob[piece][b][1];
	if (TYPE(piece) == PAWN)
		p->pawn_key ^= zob[piece][a][1] ^ zob[piece][b][1];
	p->mg += place_mg[piece][b] - place_mg[piece][a];
	p->eg += place_eg[piece][b] - place_eg[piece][a];
}

static inline void
take_piece(chess_pos *p, int piece, int sq)
{
	int side = SIDE(piece), at = p->board[sq + 8], last = --p->count[side], s = S64(sq);

	if (at != last) {
		int moved = p->list[side][last];

		p->list[side][at] = (u8)moved;
		p->board[moved + 8] = (u8)at;
	}
	p->board[sq] = 0;
	p->hash0 ^= zob[piece][s][0], p->hash1 ^= zob[piece][s][1];
	if (TYPE(piece) == PAWN)
		p->pawn_key ^= zob[piece][s][1];
	p->mg -= place_mg[piece][s], p->eg -= place_eg[piece][s];
	p->phase -= phase_of[TYPE(piece)];
	p->big[side] -= TYPE(piece) != PAWN;
}

/* Back into its old place in the list, the piece that took that place going to the end again. */
static inline void
give_back(chess_pos *p, int piece, int sq, int at)
{
	int side = SIDE(piece), last = p->count[side]++;

	if (at != last) {
		int moved = p->list[side][at];

		p->list[side][last] = (u8)moved;
		p->board[moved + 8] = (u8)last;
	}
	p->list[side][at] = (u8)sq;
	p->board[sq] = (u8)piece;
	p->board[sq + 8] = (u8)at;
	p->big[side] += TYPE(piece) != PAWN;
}

static inline void
chess_make(chess_pos *p, int m, chess_undo *u)
{
	int from = M_FROM(m), to = M_TO(m), piece = p->board[from], side = p->side, type = TYPE(piece), cap = p->board[to], keep;

	u->hash0 = p->hash0, u->hash1 = p->hash1, u->pawn_key = p->pawn_key, u->mg = p->mg, u->eg = p->eg;
	u->castle = p->castle, u->ep = p->ep, u->half = p->half, u->phase = p->phase;
	u->captured = (u8)cap;
	u->cap_at = 0;
	if (p->ep != 0xff)
		p->hash0 ^= zob_ep[p->ep & 7][0], p->hash1 ^= zob_ep[p->ep & 7][1];
	p->ep = 0xff;
	p->half++;
	if (cap) {
		u->cap_at = p->board[to + 8];
		take_piece(p, cap, to);
		p->half = 0;
	}
	if (type == PAWN) {
		p->half = 0;
		if (to - from == 32 || from - to == 32) {
			p->ep = (u8)((from + to) >> 1);
			p->hash0 ^= zob_ep[to & 7][0], p->hash1 ^= zob_ep[to & 7][1];
		} else if (to == u->ep) {
			int at = to + (side ? 16 : -16);

			u->captured = (u8)p->board[at];
			u->cap_at = p->board[at + 8];
			take_piece(p, u->captured, at);
		}
		move_piece(p, piece, from, to);
		if ((to >> 4) == (side ? 0 : 7)) {
			static const u8 promo_type[4] = { QUEEN, KNIGHT, ROOK, BISHOP };
			int promo = PIECE(side, promo_type[M_PROMO(m)]), s = S64(to);

			p->board[to] = (u8)promo;
			p->hash0 ^= zob[piece][s][0] ^ zob[promo][s][0];
			p->hash1 ^= zob[piece][s][1] ^ zob[promo][s][1];
			p->pawn_key ^= zob[piece][s][1];
			p->mg += place_mg[promo][s] - place_mg[piece][s];
			p->eg += place_eg[promo][s] - place_eg[piece][s];
			p->phase += phase_of[TYPE(promo)];
			p->big[side]++;
		}
	} else {
		move_piece(p, piece, from, to);
		if (type == KING && (to - from == 2 || from - to == 2)) {
			int rook = PIECE(side, ROOK);

			if (to > from)
				move_piece(p, rook, to + 1, to - 1);
			else
				move_piece(p, rook, to - 2, to + 1);
		}
	}
	keep = p->castle & KEEP(from) & KEEP(to);
	if (keep != p->castle) {
		p->hash0 ^= zob_castle[p->castle][0] ^ zob_castle[keep][0];
		p->hash1 ^= zob_castle[p->castle][1] ^ zob_castle[keep][1];
		p->castle = (u8)keep;
	}
	p->side = (u8)(side ^ 1);
	p->hash0 ^= zob_side[0], p->hash1 ^= zob_side[1];
}

/* Moving back only what make moved: no sums, they are in the undo. */
static inline void
slide_back(chess_pos *p, int piece, int from, int to)
{
	int at = p->board[to + 8];

	p->board[to] = 0;
	p->board[from] = (u8)piece;
	p->board[from + 8] = (u8)at;
	p->list[SIDE(piece)][at] = (u8)from;
}

static inline void
chess_unmake(chess_pos *p, int m, const chess_undo *u)
{
	int from = M_FROM(m), to = M_TO(m), side = p->side ^ 1, piece = p->board[to], type = TYPE(piece);

	p->side = (u8)side;
	if (type == KING && (to - from == 2 || from - to == 2)) {
		int rook = PIECE(side, ROOK);

		if (to > from)
			slide_back(p, rook, to + 1, to - 1);
		else
			slide_back(p, rook, to - 2, to + 1);
	}
	slide_back(p, piece, from, to);
	if (m & 0x4000) {	/* (PROMO_BIT: a promotion, as the generator marks it) */
		p->board[from] = PIECE(side, PAWN);
		p->big[side]--;
	}
	if (u->captured) {
		int at = to;

		if (TYPE(p->board[from]) == PAWN && to == u->ep)
			at = to + (side ? 16 : -16);
		give_back(p, u->captured, at, u->cap_at);
	}
	p->hash0 = u->hash0, p->hash1 = u->hash1, p->pawn_key = u->pawn_key, p->mg = u->mg, p->eg = u->eg;
	p->castle = u->castle, p->ep = u->ep, p->half = u->half, p->phase = u->phase;
}

/* A null move and its undoing. */
static inline void
chess_make_null(chess_pos *p, chess_undo *u)
{
	u->hash0 = p->hash0, u->hash1 = p->hash1, u->ep = p->ep, u->half = p->half;
	if (p->ep != 0xff)
		p->hash0 ^= zob_ep[p->ep & 7][0], p->hash1 ^= zob_ep[p->ep & 7][1];
	p->ep = 0xff;
	p->half = 0;
	p->side ^= 1;
	p->hash0 ^= zob_side[0], p->hash1 ^= zob_side[1];
}

static inline void
chess_unmake_null(chess_pos *p, const chess_undo *u)
{
	p->side ^= 1;
	p->hash0 = u->hash0, p->hash1 = u->hash1, p->ep = u->ep, p->half = u->half;
}

/*
 * Moves as they are generated: bit 14 says promotion, for chess_unmake; the high half of a
 * word is its score for the order. Pseudo-legal: a move that leaves the king attacked is told
 * after chess_make.
 */
#define PROMO_BIT 0x4000

static inline u32 *
add_pawn(u32 *out, int from, int to, int last, int all)
{
	u32 m = MOVE(from, to);

	if (to >> 4 == last) {
		*out++ = m | PROMO_BIT;
		if (all) {
			*out++ = m | PROMO_BIT | 1u << 12;
			*out++ = m | PROMO_BIT | 2u << 12;
			*out++ = m | PROMO_BIT | 3u << 12;
		}
	} else
		*out++ = m;
	return out;
}

/* The moves of the side to move into out; how many. `quiets` 0: captures and queen promotions only. */
static int
chess_moves(const chess_pos *p, u32 *start, int quiets)
{
	u32 *out = start;
	int side = p->side, i, n = p->count[side];
	const u8 *list = p->list[side];
	const u8 *board = p->board;

	for (i = 0; i < n; i++) {
		int from = list[i], piece = board[from], type = TYPE(piece), k, to, target;

		switch (type) {
		case PAWN: {
			int up = side ? -16 : 16, last = side ? 0 : 7, startrank = side ? 6 : 1;

			to = from + up;
			if (!board[to] && (quiets || to >> 4 == last)) {
				out = add_pawn(out, from, to, last, quiets);
				if (from >> 4 == startrank && !board[to + up])
					*out++ = MOVE(from, to + up);
			}
			for (k = -1; k <= 1; k += 2) {
				to = from + up + k;
				if (BAD(to))
					continue;
				target = board[to];
				if ((target && SIDE(target) != side) || to == p->ep)
					out = add_pawn(out, from, to, last, quiets);
			}
			break;
		}
		case KNIGHT:
			for (k = 0; k < 8; k++) {
				to = from + knight_step[k];
				if (BAD(to))
					continue;
				target = board[to];
				if (target ? SIDE(target) != side : quiets)
					*out++ = MOVE(from, to);
			}
			break;
		case KING:
			for (k = 0; k < 8; k++) {
				to = from + line_step[k];
				if (BAD(to))
					continue;
				target = board[to];
				if (target ? SIDE(target) != side : quiets)
					*out++ = MOVE(from, to);
			}
			if (quiets && (p->castle & (side ? 12 : 3)) && (from & 15) == 4) {
				int them = side ^ 1, sh = side ? 4 : 1, lo = side ? 8 : 2;

				if ((p->castle & sh) && !board[from + 1] && !board[from + 2] && !chess_attacked(p, from, them) &&
				    !chess_attacked(p, from + 1, them))
					*out++ = MOVE(from, from + 2);
				if ((p->castle & lo) && !board[from - 1] && !board[from - 2] && !board[from - 3] &&
				    !chess_attacked(p, from, them) && !chess_attacked(p, from - 1, them))
					*out++ = MOVE(from, from - 2);
			}
			break;
		default: {
			int a = type == BISHOP ? 4 : 0, b = type == ROOK ? 4 : 8;

			for (k = a; k < b; k++) {
				int step = line_step[k];

				for (to = from + step; !BAD(to); to += step) {
					target = board[to];
					if (target) {
						if (SIDE(target) != side)
							*out++ = MOVE(from, to);
						break;
					}
					if (quiets)
						*out++ = MOVE(from, to);
				}
			}
		}
		}
	}
	return (int)(out - start);
}

/* ---- what a position is worth ---- */

/* What the pawns alone say, kept by a key of theirs: most positions of a search have the pawns of the last. */
typedef struct {
	u32 key;
	s16 mg, eg;
	u8 files[2];		/* a side's: bit f, a pawn on file f */
	u8 pad[6];
} chess_pawn_entry;
#define PAWN_TABLE 256

static void
pawn_terms(const chess_pos *p, chess_pawn_entry *e)
{
	u8 rear[2][10], count[2][10];
	int side, i, mg = 0, eg = 0;

	memset(count, 0, sizeof count);
	for (side = 0; side < 2; side++)
		for (i = 1; i < p->count[side]; i++) {
			int sq = p->list[side][i];

			if (TYPE(p->board[sq]) == PAWN) {
				int file = (sq & 7) + 1, rank = side ? 7 - (sq >> 4) : sq >> 4;

				if (!count[side][file] || rank < rear[side][file])
					rear[side][file] = (u8)rank;
				count[side][file]++;
			}
		}
	for (side = 0; side < 2; side++) {
		const u8 *ours = count[side], *theirs = count[side ^ 1];
		const u8 *behind = rear[side ^ 1];
		int smg = 0, seg = 0, files = 0;

		for (i = 1; i < p->count[side]; i++) {
			int sq = p->list[side][i];

			if (TYPE(p->board[sq]) == PAWN) {
				static const u8 passed_mg[8] = { 0, 2, 4, 8, 16, 32, 50, 0 }, passed_eg[8] = { 0, 6, 12, 26, 50, 85, 125, 0 };
				int file = (sq & 7) + 1, rank = side ? 7 - (sq >> 4) : sq >> 4;

				files |= 1 << (file - 1);
				if (!ours[file - 1] && !ours[file + 1])
					smg -= 10, seg -= 12;
				if (ours[file] > 1)
					smg -= 6, seg -= 12;
				/* passed: no pawn of theirs ahead on this file or beside it */
				if ((!theirs[file] || 7 - behind[file] <= rank) && (!theirs[file - 1] || 7 - behind[file - 1] <= rank) &&
				    (!theirs[file + 1] || 7 - behind[file + 1] <= rank))
					smg += passed_mg[rank], seg += passed_eg[rank];
			}
		}
		e->files[side] = (u8)files;
		mg += side ? -smg : smg, eg += side ? -seg : seg;
	}
	e->mg = (s16)mg, e->eg = (s16)eg;
	e->key = p->pawn_key;
}

/*
 * For the side to move: material and places, kept by make; the pawns' terms, kept in a table of the
 * core's; here the bishops' pair, rooks on open files, how far bishops and rooks see, the king's shelter.
 */
static int
chess_eval(const chess_pos *p, chess_pawn_entry *table)
{
	chess_pawn_entry *e = &table[p->pawn_key & (PAWN_TABLE - 1)];
	int side, i, mg = p->mg, eg = p->eg, phase = p->phase;

	if (e->key != p->pawn_key)
		pawn_terms(p, e);
	mg += e->mg, eg += e->eg;
	for (side = 0; side < 2; side++) {
		int bishops = 0, smg = 0, seg = 0, ours = e->files[side], theirs = e->files[side ^ 1], n = p->count[side];
		const u8 *list = p->list[side];

		for (i = 1; i < n; i++) {
			int sq = list[i], type = TYPE(p->board[sq]);

			if (type == BISHOP || type == ROOK) {
				int k, a = type == BISHOP ? 4 : 0, b = a + 4, sees = 0;

				for (k = a; k < b; k++) {
					int step = line_step[k], at = sq + step;

					while (!BAD(at) && !p->board[at])
						sees++, at += step;
				}
				if (type == BISHOP) {
					bishops++;
					smg += (sees - 6) * 3, seg += (sees - 6) * 3;
				} else {
					int bit = 1 << (sq & 7);

					smg += (sees - 7) * 2, seg += (sees - 7) * 3;
					if (!(ours & bit)) {
						int half = (theirs & bit) != 0;

						smg += half ? 10 : 20, seg += half ? 6 : 10;
					}
				}
			}
		}
		if (bishops >= 2)
			smg += 28, seg += 40;
		/* the king's shelter: the pawns of its file and the two beside it */
		{
			int file = KING_AT(p, side) & 7, f, last = file < 7 ? file + 1 : 7;

			for (f = file ? file - 1 : 0; f <= last; f++)
				if (!(ours >> f & 1))
					smg -= theirs >> f & 1 ? 14 : 22;
		}
		mg += side ? -smg : smg, eg += side ? -seg : seg;
	}
	phase = phase > 24 ? 24 : phase;
	mg = (mg * phase + eg * (24 - phase)) / 24;
	return (p->side ? -mg : mg) + 10;
}

/* ---- perft: the moves against the known numbers ---- */

static u32
chess_perft(chess_pos *p, int depth, u32 *stack)
{
	int n = chess_moves(p, stack, 1), i;
	u32 total = 0;

	for (i = 0; i < n; i++) {
		chess_undo u;
		int us = p->side;

		chess_make(p, (int)stack[i], &u);
		if (!chess_attacked(p, KING_AT(p, us), us ^ 1))
			total += depth > 1 ? chess_perft(p, depth - 1, stack + n) : 1;
		chess_unmake(p, (int)stack[i], &u);
	}
	return total;
}

#endif
