/*
 * nxsolitaire: Klondike, the patience of seven columns (docs/apps/solitaire.md). The cards, their
 * backs and the table are drawn here; nothing is taken from another product.
 *
 * Played with a hand as well as a mouse (porting-games, "Controls for VR"): a click picks a card
 * (and those over it) and a click elsewhere puts it there; a press dragged past SLOP pixels carries
 * it, and its meaning is taken at the release; a double click, the right button, or the Up button
 * sends a card to the foundations; the stock is clicked to turn cards over. Undo goes back a move
 * at a time to the deal, Finish puts up every card that can go, by itself once nothing is hidden.
 * Draw one or three is the registry's (nxsolitaire.draw), the game in play is kept in
 * /persistence/nxsolitaire/game when the window closes and taken up again at the next start, and the
 * statistics in /persistence/nxsolitaire/stats.
 *
 *   nxsolitaire [SEED]          a deal by its number (the same cards for the same number)
 *   NXSOLITAIRE_PRINT=1         what happens, on standard output, for a test (places in the window)
 *   NXSOLITAIRE_DRAW=1|3        the draw without the registry; NXSOLITAIRE_DEAL=nearly a deal Finish ends
 */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_single.h"
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#define MENU_H 20
#define CW 60			/* a card */
#define CH 84
#define STEP_X 70		/* from one column to the next */
#define LEFT 10
#define TOP_Y (MENU_H + 8)	/* the stock, the waste and the foundations */
#define TAB_Y (TOP_Y + CH + 14)	/* the seven columns */
#define TAB_ROOM 310		/* the columns' height at most */
#define BAR_H 32		/* the buttons and the counts at the bottom */
#define WIN_W (LEFT * 2 + 6 * STEP_X + CW)
#define WIN_H (TAB_Y + TAB_ROOM + BAR_H)
#define BAR_Y (WIN_H - BAR_H)
#define DOWN_STEP 6		/* a hidden card's edge under the next */
#define UP_STEP 20		/* a shown card's, when the column has room */
#define FAN 14			/* the waste's three cards of a draw of three */
#define SLOP 8			/* a press that moves further is a drag (a hand's beam holds 7: "Steady pointer") */
#define DOUBLE_MS 500
#define FINISH_MS 70		/* a card put up every so often by Finish */
#define BUTTON_W 60
#define BUTTON_H 24
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
	GR_EVENT_MASK_CLOSE_REQ)
#define KEPT "/persistence/nxsolitaire"

/* A card is suit * 13 + rank (rank 0 the ace, 12 the king), UP when its face is shown. Clubs and
 * spades are black, diamonds and hearts red. */
#define UP 64
#define CARD(c) ((c) & 63)
#define RANK(c) (CARD(c) % 13)
#define SUIT(c) (CARD(c) / 13)
#define IS_RED(c) (SUIT(c) == 1 || SUIT(c) == 2)
enum { STOCK, WASTE, FOUND, TAB = FOUND + 4, PILES = TAB + 7 };
static uint8_t pile[PILES][52];
static int count[PILES];

/* A move, kept for Undo: cards from one pile to another, the stock turned over, or the waste back. */
enum { M_MOVE, M_DRAW, M_RECYCLE };
struct move { uint8_t kind, from, to, n, flipped; };
#define MOST_MOVES 2048
static struct move history[MOST_MOVES];
static int moves;

static uint32_t seed;
static int draw_n = 1, won, finishing, counted, printing, nearly;
static long played_ms;		/* the time of this deal's play before the clock's start */
static long clock_from = -1;	/* when the clock last started, or -1: it runs from the first move to the win */

/* what is picked (a click), held (a press not yet a drag) or carried (a drag) */
static int sel_pile = -1, sel_index;
static int press_pile = -1, press_index, press_x, press_y, dragging, drag_dx, drag_dy, drag_x, drag_y, press_button;
static int last_click_pile = -1, last_click_index;
static long last_click_ms;
static int pressed_button = -1;	/* a button of the bar held down */

static struct { int played, won, streak, best_streak, best_seconds, best_moves, open; } stats;

static GR_WINDOW_ID window, back, sheet;
static int menu_x[3];
static const char *const menu_names[] = { "Game", "Help" };
enum { C_NEW = 1, C_RESTART, C_UNDO, C_FINISH, C_DRAW1, C_DRAW3, C_STATS, C_EXIT, C_HELP, C_ABOUT };
static struct ui_menu_item game_items[] = {
	{ "New game   F2", -1, C_NEW }, { "Deal again", -1, C_RESTART }, { "Undo   Ctrl+Z", -1, C_UNDO, NULL, 0, UI_MENU_LINE },
	{ "Finish   F", -1, C_FINISH }, { "Draw one", -1, C_DRAW1, NULL, 0, UI_MENU_LINE }, { "Draw three", -1, C_DRAW3 },
	{ "Statistics...", -1, C_STATS, NULL, 0, UI_MENU_LINE }, { "Exit", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item help_items[] = { { "How to play", -1, C_HELP }, { "About Solitaire", -1, C_ABOUT } };
static const struct { const struct ui_menu_item *items; int count; } menus[2] = { { game_items, 8 }, { help_items, 2 } };
enum { B_NEW, B_UNDO, B_UP, B_FINISH, BUTTONS };
static const char *const button_names[BUTTONS] = { "New", "Undo", "Up", "Finish" };

static long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static int
seconds(void)
{
	return (int)((played_ms + (clock_from >= 0 ? now_ms() - clock_from : 0)) / 1000);
}

/* ---- the game ---- */

static uint32_t
next_random(uint32_t *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

/* The deal of a number: the pack shuffled by it (Fisher and Yates), a column of k cards for the k-th,
 * its last shown, row by row as a dealer deals, the 24 left the stock. tools/solitaire_deal.py is the same. */
static void
deal(uint32_t number)
{
	uint8_t pack[52];
	uint32_t s = number;
	int i, j, k, at = 0;

	for (i = 0; i < 52; i++)
		pack[i] = (uint8_t)i;
	for (i = 51; i > 0; i--) {
		uint8_t t;

		j = (int)(next_random(&s) % (uint32_t)(i + 1));
		t = pack[i], pack[i] = pack[j], pack[j] = t;
	}
	memset(count, 0, sizeof count);
	for (k = 0; k < 7; k++)
		for (i = k; i < 7; i++)
			pile[TAB + i][count[TAB + i]++] = pack[at++];
	for (i = 0; i < 7; i++)
		pile[TAB + i][i] |= UP;
	while (at < 52)
		pile[STOCK][count[STOCK]++] = pack[at++];
	if (nearly) {	/* (for a test of Finish: every card up to the jacks home, the queens on the kings) */
		static const uint8_t kings[4][2] = { { 12, 26 + 11 }, { 13 + 12, 11 }, { 26 + 12, 39 + 11 }, { 39 + 12, 13 + 11 } };

		memset(count, 0, sizeof count);
		for (k = 0; k < 4; k++)
			for (i = 0; i < 11; i++)
				pile[FOUND + k][count[FOUND + k]++] = (uint8_t)(k * 13 + i) | UP;
		for (k = 0; k < 4; k++)
			for (i = 0; i < 2; i++)
				pile[TAB + k][count[TAB + k]++] = kings[k][i] | UP;
	}
	moves = 0, won = 0, finishing = 0, counted = 0;
	played_ms = 0, clock_from = -1;
	sel_pile = -1, press_pile = -1, dragging = 0;
}

static int
top(int p)
{
	return count[p] ? pile[p][count[p] - 1] : -1;
}

/* Whether the card c (and those over it) may go on pile p. */
static int
fits(int c, int n, int p)
{
	int t = top(p);

	if (p >= FOUND && p < TAB)
		return n == 1 && (t < 0 ? RANK(c) == 0 : SUIT(t) == SUIT(c) && RANK(t) + 1 == RANK(c));
	if (p >= TAB)
		return t < 0 ? RANK(c) == 12 : (t & UP) && IS_RED(t) != IS_RED(c) && RANK(t) == RANK(c) + 1;
	return 0;
}

/* Whether the cards from index i of pile p up may be taken: a shown run of a column, or a top card. */
static int
movable(int p, int i)
{
	if (p < 0 || i < 0 || i >= count[p] || !(pile[p][i] & UP))
		return 0;
	return p >= TAB || i == count[p] - 1;
}

static void
apply(struct move m)
{
	int k;

	switch (m.kind) {
	case M_MOVE:
		for (k = 0; k < m.n; k++)
			pile[m.to][count[m.to] + k] = pile[m.from][count[m.from] - m.n + k];
		count[m.to] += m.n, count[m.from] -= m.n;
		if (m.flipped)
			pile[m.from][count[m.from] - 1] |= UP;
		break;
	case M_DRAW:	/* the stock's top n cards turned over onto the waste, the top first */
		for (k = 0; k < m.n; k++)
			pile[WASTE][count[WASTE]++] = pile[STOCK][--count[STOCK]] | UP;
		break;
	case M_RECYCLE:	/* the waste turned back over as the stock */
		for (k = 0; k < m.n; k++)
			pile[STOCK][count[STOCK]++] = pile[WASTE][--count[WASTE]] & ~UP;
		break;
	}
}

static void
unapply(struct move m)
{
	int k;

	switch (m.kind) {
	case M_MOVE:
		if (m.flipped)
			pile[m.from][count[m.from] - 1] &= ~UP;
		for (k = 0; k < m.n; k++)
			pile[m.from][count[m.from] + k] = pile[m.to][count[m.to] - m.n + k];
		count[m.from] += m.n, count[m.to] -= m.n;
		break;
	case M_DRAW:
		for (k = 0; k < m.n; k++)
			pile[STOCK][count[STOCK]++] = pile[WASTE][--count[WASTE]] & ~UP;
		break;
	case M_RECYCLE:
		for (k = 0; k < m.n; k++)
			pile[WASTE][count[WASTE]++] = pile[STOCK][--count[STOCK]] | UP;
		break;
	}
}

static void stats_save(void);

/* A deal's first move counts it as played; a deal before it left unwon ended the streak. */
static void
count_game(void)
{
	if (counted)
		return;
	counted = 1;
	if (stats.open)
		stats.streak = 0;
	stats.played++, stats.open = 1;
	stats_save();
}

static int
all_home(void)
{
	return count[FOUND] + count[FOUND + 1] + count[FOUND + 2] + count[FOUND + 3] == 52;
}

static void
record(struct move m)
{
	if (moves == MOST_MOVES) {	/* (the oldest is forgotten: Undo goes back as far as it can) */
		memmove(history, history + 1, (MOST_MOVES - 1) * sizeof history[0]);
		moves--;
	}
	apply(m);
	history[moves++] = m;
	count_game();
	if (clock_from < 0 && !won)
		clock_from = now_ms();
}

static void
check_won(void)
{
	int s;

	if (won || !all_home())
		return;
	won = 1, finishing = 0;
	s = seconds();
	played_ms = (long)s * 1000, clock_from = -1;
	stats.won++, stats.open = 0;
	if (++stats.streak > stats.best_streak)
		stats.best_streak = stats.streak;
	if (!stats.best_seconds || s < stats.best_seconds)
		stats.best_seconds = s;
	if (!stats.best_moves || moves < stats.best_moves)
		stats.best_moves = moves;
	stats_save();
}

/* n cards from the top of pile `from` to pile `to`, the column's new top shown. */
static int
move_cards(int from, int n, int to)
{
	struct move m = { M_MOVE, (uint8_t)from, (uint8_t)to, (uint8_t)n, 0 };

	if (from == to || n < 1 || n > count[from] || !movable(from, count[from] - n) || !fits(pile[from][count[from] - n], n, to))
		return 0;
	m.flipped = from >= TAB && count[from] > n && !(pile[from][count[from] - n - 1] & UP);
	record(m);
	check_won();
	return 1;
}

/* The stock's click: its cards turned over (one or three), or the waste back when it is empty. */
static int
turn_stock(void)
{
	struct move m = { M_DRAW, STOCK, WASTE, 0, 0 };

	if (count[STOCK]) {
		m.n = (uint8_t)(count[STOCK] < draw_n ? count[STOCK] : draw_n);
	} else if (count[WASTE]) {
		m.kind = M_RECYCLE, m.n = (uint8_t)count[WASTE];
	} else
		return 0;
	record(m);
	return 1;
}

/* The top card of pile p to a foundation it fits, if any. */
static int
send_up(int p)
{
	int f, c = top(p);

	if (c < 0 || !(c & UP) || p == STOCK || (p >= FOUND && p < TAB))
		return 0;
	for (f = FOUND; f < TAB; f++)
		if (fits(c, 1, f))
			return move_cards(p, 1, f);
	return 0;
}

/* One card up for Finish: of the waste's and the columns' tops, the lowest that fits. */
static int
finish_step(void)
{
	int p, best = -1, rank = 99;

	for (p = WASTE; p < PILES; p++) {
		int c = top(p), f;

		if (p >= FOUND && p < TAB)
			continue;
		if (c >= 0 && (c & UP) && RANK(c) < rank)
			for (f = FOUND; f < TAB; f++)
				if (fits(c, 1, f))
					best = p, rank = RANK(c);
	}
	return best >= 0 && send_up(best);
}

/* Nothing hidden and nothing in the stock or the waste: the game is won, and Finish plays it out. */
static int
open_game(void)
{
	int p, i;

	if (won || count[STOCK] || count[WASTE])
		return 0;
	for (p = TAB; p < PILES; p++)
		for (i = 0; i < count[p]; i++)
			if (!(pile[p][i] & UP))
				return 0;
	return 1;
}

static int
undo(void)
{
	if (!moves || won)
		return 0;
	unapply(history[--moves]);
	finishing = 0;
	return 1;
}

/* ---- what is kept ---- */

static void
stats_load(void)
{
	FILE *f = fopen(KEPT "/stats", "r");

	if (!f)
		return;
	if (fscanf(f, "played %d won %d streak %d best_streak %d best_seconds %d best_moves %d open %d", &stats.played, &stats.won,
		   &stats.streak, &stats.best_streak, &stats.best_seconds, &stats.best_moves, &stats.open) != 7)
		memset(&stats, 0, sizeof stats);
	fclose(f);
}

static void
stats_save(void)
{
	FILE *f;

	mkdir(KEPT, 0755);
	if (!(f = fopen(KEPT "/stats", "w")))
		return;	/* (no /persistence: the counts live as long as the program) */
	fprintf(f, "played %d won %d streak %d best_streak %d best_seconds %d best_moves %d open %d\n", stats.played, stats.won,
		stats.streak, stats.best_streak, stats.best_seconds, stats.best_moves, stats.open);
	fclose(f);
}

/* The game in play, kept when the window closes: its number, the draw, the time, every pile and the
 * moves Undo takes back. A won game or one not begun leaves nothing. */
static void
game_save(void)
{
	FILE *f;
	int p, i;

	if (won || !moves) {
		unlink(KEPT "/game");
		return;
	}
	mkdir(KEPT, 0755);
	if (!(f = fopen(KEPT "/game", "w")))
		return;
	fprintf(f, "nxsolitaire 1\n%u %d %d %d %d\n", seed, draw_n, seconds(), counted, moves);
	for (p = 0; p < PILES; p++) {
		fprintf(f, "%d", count[p]);
		for (i = 0; i < count[p]; i++)
			fprintf(f, " %d", pile[p][i]);
		fputc('\n', f);
	}
	for (i = 0; i < moves; i++)
		fprintf(f, "%d %d %d %d %d\n", history[i].kind, history[i].from, history[i].to, history[i].n, history[i].flipped);
	fclose(f);
}

static int
game_load(void)
{
	FILE *f = fopen(KEPT "/game", "r");
	unsigned number;
	int p, i, d, s, c, n, ok = 1, total = 0, seen[52] = { 0 };

	if (!f)
		return 0;
	if (fscanf(f, "nxsolitaire 1 %u %d %d %d %d", &number, &d, &s, &c, &n) != 5 || n < 0 || n > MOST_MOVES)
		ok = 0;
	for (p = 0; ok && p < PILES; p++) {
		if (fscanf(f, "%d", &count[p]) != 1 || count[p] < 0 || count[p] > 52)
			ok = 0;
		for (i = 0; ok && i < count[p]; i++) {
			int v;

			if (fscanf(f, "%d", &v) != 1 || CARD(v) > 51 || seen[CARD(v)]++)
				ok = 0;
			pile[p][i] = (uint8_t)v;
		}
		total += count[p];
	}
	for (i = 0; ok && i < n; i++) {
		int k, a, b, m, fl;

		if (fscanf(f, "%d %d %d %d %d", &k, &a, &b, &m, &fl) != 5 || k > M_RECYCLE || a >= PILES || b >= PILES)
			ok = 0;
		history[i] = (struct move){ (uint8_t)k, (uint8_t)a, (uint8_t)b, (uint8_t)m, (uint8_t)fl };
	}
	fclose(f);
	if (!ok || total != 52) {
		unlink(KEPT "/game");
		return 0;
	}
	seed = number, moves = n, counted = c, won = 0, finishing = 0;
	if (!getenv("NXSOLITAIRE_DRAW"))
		draw_n = d == 3 ? 3 : 1;
	played_ms = (long)s * 1000, clock_from = -1;	/* (the clock goes on at the next move) */
	return 1;
}

/* ---- where things are ---- */

static int
pile_x(int p)
{
	if (p == STOCK)
		return LEFT;
	if (p == WASTE)
		return LEFT + STEP_X;
	if (p < TAB)
		return LEFT + (3 + p - FOUND) * STEP_X;
	return LEFT + (p - TAB) * STEP_X;
}

/* How far apart a column's shown cards are: closer when it would pass the room it has. */
static int
up_step(int p)
{
	int down = 0, up, room;

	while (down < count[p] && !(pile[p][down] & UP))
		down++;
	up = count[p] - down;
	if (up < 2)
		return UP_STEP;
	room = (TAB_ROOM - 6 - CH - down * DOWN_STEP) / (up - 1);
	return room < UP_STEP ? (room < 8 ? 8 : room) : UP_STEP;
}

/* Where card i of pile p is drawn (its top left corner), and the waste's fan. */
static void
card_at(int p, int i, int *x, int *y)
{
	*x = pile_x(p), *y = p >= TAB ? TAB_Y : TOP_Y;
	if (p >= TAB) {
		int k, step = up_step(p);

		for (k = 0; k < i; k++)
			*y += (pile[p][k] & UP) ? step : DOWN_STEP;
	} else if (p == WASTE && draw_n == 3) {
		int shown = count[WASTE] < 3 ? count[WASTE] : 3, first = count[WASTE] - shown;

		if (i >= first)
			*x += (i - first) * FAN;
	}
}

/* The card of a pile under the point (its index), or -1 for the pile's place with no card there;
 * the pile is -1 when the point is on none. */
static int
hit(int x, int y, int *index)
{
	int p, i, cx, cy;

	for (p = 0; p < PILES; p++) {
		if (p >= TAB) {
			for (i = count[p] - 1; i >= 0; i--) {
				card_at(p, i, &cx, &cy);
				if (ui_inside(x, y, cx, cy, CW, CH)) {
					*index = i;
					return p;
				}
			}
			if (ui_inside(x, y, pile_x(p), TAB_Y, CW, CH)) {
				*index = -1;
				return p;
			}
			continue;
		}
		i = count[p] - 1;
		card_at(p, i < 0 ? 0 : i, &cx, &cy);
		if (ui_inside(x, y, cx, cy, CW, CH) || ui_inside(x, y, pile_x(p), TOP_Y, CW, CH)) {
			*index = i;
			return p;
		}
	}
	return -1;
}

static int
button_x(int b)
{
	return 8 + b * (BUTTON_W + 6);
}

static int
on_button(int x, int y)
{
	int b;

	for (b = 0; b < BUTTONS; b++)
		if (ui_inside(x, y, button_x(b), BAR_Y + 4, BUTTON_W, BUTTON_H))
			return b;
	return -1;
}

/* ---- for a test ---- */

static void
card_name(int c, char *to)
{
	if (!(c & UP)) {
		strcpy(to, "##");
		return;
	}
	to[0] = "A23456789TJQK"[RANK(c)];
	to[1] = "cdhs"[SUIT(c)];
	to[2] = 0;
}

/* What happened and every pile, a card as its rank and suit (## face down); the top's place in the window. */
static void
say(const char *what)
{
	static const char *const names[PILES] = { "stock", "waste", "f1", "f2", "f3", "f4", "t1", "t2", "t3", "t4", "t5", "t6", "t7" };
	char name[4];
	int p, i, x, y;

	if (!printing)
		return;
	printf("nxsolitaire: %s: moves %d, stock %d, waste %d, home %d %d %d %d, %s\n", what, moves, count[STOCK], count[WASTE],
	       count[FOUND], count[FOUND + 1], count[FOUND + 2], count[FOUND + 3], won ? "won" : finishing ? "finishing" : "playing");
	if (printing > 1)
		return;
	printf("nxsolitaire: piles:");
	for (p = WASTE; p < PILES; p++) {
		printf(" %s", names[p]);
		for (i = p == WASTE && count[p] > 3 ? count[p] - 3 : 0; i < count[p]; i++) {
			card_name(pile[p][i], name);
			printf("%c%s", i ? ' ' : ':', name);
		}
		if (!count[p])
			printf(":-");
		printf(" |");
	}
	printf("\nnxsolitaire: tops:");
	for (p = WASTE; p < PILES; p++) {
		card_at(p, count[p] ? count[p] - 1 : 0, &x, &y);
		printf(" %s %d,%d", names[p], x + CW / 2, y + 10);
	}
	printf("\n");
	if (sel_pile >= 0)
		printf("nxsolitaire: picked %s, %d cards\n", names[sel_pile], count[sel_pile] - sel_index);
	fflush(stdout);
}

static void
say_stats(void)
{
	if (printing)
		printf("nxsolitaire: statistics: played %d, won %d, streak %d, best streak %d, fastest %d s, fewest moves %d\n", stats.played,
		       stats.won, stats.streak, stats.best_streak, stats.best_seconds, stats.best_moves), fflush(stdout);
}

/* ---- drawing: every card is made once in a sheet, the table in a picture of the window's size ---- */

#define TABLE MWRGB(18, 112, 56)
#define TABLE_DARK MWRGB(10, 80, 38)
#define INK MWRGB(24, 24, 32)
#define RED_INK MWRGB(204, 16, 32)
#define PICKED MWRGB(255, 214, 40)
#define SHEET_W (13 * CW)
#define SHEET_H (5 * CH)
enum { S_BACK, S_EMPTY, S_STOCK_AGAIN, S_HOME };	/* the sheet's last row */

/* The suits' small signs (9 x 9), beside a card's rank. */
static const char *const small_suit[4][9] = {
	{ "   ###   ", "  #####  ", "   ###   ", " ## # ## ", "#########", " ## # ## ", "    #    ", "   ###   ", "         " },
	{ "    #    ", "   ###   ", "  #####  ", " ####### ", "#########", " ####### ", "  #####  ", "   ###   ", "    #    " },
	{ " ##   ## ", "#### ####", "#########", "#########", " ####### ", "  #####  ", "   ###   ", "    #    ", "         " },
	{ "    #    ", "   ###   ", "  #####  ", " ####### ", "#########", "#########", " ## # ## ", "    #    ", "   ###   " },
};
/* The court cards' letters, 5 x 7, drawn four times as large. */
static const char *const letters[3][7] = {
	{ "  ###", "    #", "    #", "    #", "#   #", "#   #", " ### " },
	{ " ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #" },
	{ "#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #" },
};

static void
bitmap(GR_DRAW_ID d, int x, int y, const char *const *rows, int w, int h, int scale, GR_COLOR colour)
{
	int r, c;

	for (r = 0; r < h; r++)
		for (c = 0; c < w; c++)
			if (rows[r][c] == '#')
				ui_fill(d, x + c * scale, y + r * scale, scale, scale, colour);
}

/* A heart's outline, point down (the usual curve of sines), across s pixels at cx, cy; flipped for a spade. */
static int
heart(GR_POINT *p, int cx, int cy, int s, int flip)
{
	int k, n = 40;

	for (k = 0; k < n; k++) {
		float t = (float)k * 6.2831853f / (float)n, st = sinf(t);
		float y = -(13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t)) / 32.0f;

		p[k].x = cx + (int)lrintf(st * st * st * 0.5f * (float)s);
		p[k].y = cy + (int)lrintf((flip ? -y - 0.08f : y + 0.04f) * (float)s);
	}
	return n;
}

/* A suit's large sign, s pixels across, centred at cx, cy. */
static void
pip(GR_DRAW_ID d, int suit, int cx, int cy, int s)
{
	GR_POINT p[48];
	int n;

	GrSetGCForeground(ui_gc, suit == 1 || suit == 2 ? RED_INK : INK);
	switch (suit) {
	case 0:	/* clubs: three leaves and a foot */
		GrFillEllipse(d, ui_gc, cx, cy - s * 22 / 100, s * 22 / 100, s * 22 / 100);
		GrFillEllipse(d, ui_gc, cx - s * 24 / 100, cy + s * 8 / 100, s * 22 / 100, s * 22 / 100);
		GrFillEllipse(d, ui_gc, cx + s * 24 / 100, cy + s * 8 / 100, s * 22 / 100, s * 22 / 100);
		p[0].x = cx, p[0].y = cy - s / 10, p[1].x = cx + s / 5, p[1].y = cy + s / 2, p[2].x = cx - s / 5, p[2].y = cy + s / 2;
		GrFillPoly(d, ui_gc, 3, p);
		break;
	case 1:	/* diamonds */
		p[0].x = cx, p[0].y = cy - s / 2, p[1].x = cx + s * 38 / 100, p[1].y = cy;
		p[2].x = cx, p[2].y = cy + s / 2, p[3].x = cx - s * 38 / 100, p[3].y = cy;
		GrFillPoly(d, ui_gc, 4, p);
		break;
	case 2:
		n = heart(p, cx, cy, s, 0);
		GrFillPoly(d, ui_gc, n, p);
		break;
	default:	/* spades: a heart the other way up, and a foot */
		n = heart(p, cx, cy - s / 12, s, 1);
		GrFillPoly(d, ui_gc, n, p);
		p[0].x = cx, p[0].y = cy, p[1].x = cx + s / 5, p[1].y = cy + s / 2, p[2].x = cx - s / 5, p[2].y = cy + s / 2;
		GrFillPoly(d, ui_gc, 3, p);
	}
}

/* A card's white face in its edge, the corners rounded by a pixel. */
static void
blank(GR_DRAW_ID d, int x, int y, GR_COLOR face, GR_COLOR edge)
{
	ui_fill(d, x, y, CW, CH, face);
	GrSetGCForeground(ui_gc, edge);
	GrRect(d, ui_gc, x, y, CW, CH);
	ui_fill(d, x, y, 1, 1, TABLE), ui_fill(d, x + CW - 1, y, 1, 1, TABLE);
	ui_fill(d, x, y + CH - 1, 1, 1, TABLE), ui_fill(d, x + CW - 1, y + CH - 1, 1, 1, TABLE);
}

static void
face_art(GR_DRAW_ID d, int c, int x, int y)
{
	static const char *const ranks[13] = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" };
	int r = RANK(c), s = SUIT(c), w;
	GR_COLOR ink = IS_RED(c) ? RED_INK : INK;

	blank(d, x, y, WHITE, MWRGB(90, 90, 100));
	/* the index, top left and bottom right */
	w = ui_width(&ui_var_widths, ui_var, ranks[r], -1);
	ui_text(d, x + 4, y + 3, ranks[r], -1, ink, 0);
	ui_text(d, x + 5, y + 3, ranks[r], -1, ink, 0);	/* (twice, a pixel apart: bold) */
	bitmap(d, x + 4, y + 18, small_suit[s], 9, 9, 1, ink);
	ui_text(d, x + CW - 6 - w, y + CH - 30, ranks[r], -1, ink, 0);
	ui_text(d, x + CW - 5 - w, y + CH - 30, ranks[r], -1, ink, 0);
	bitmap(d, x + CW - 13, y + CH - 14, small_suit[s], 9, 9, 1, ink);
	if (r >= 10) {	/* a court card: its letter large in a frame of its suit's tint, a sign over and under */
		GR_COLOR tint = IS_RED(c) ? MWRGB(252, 226, 220) : MWRGB(222, 230, 246);

		ui_fill(d, x + 15, y + 12, CW - 30, CH - 24, tint);
		GrSetGCForeground(ui_gc, ink);
		GrRect(d, ui_gc, x + 15, y + 12, CW - 30, CH - 24);
		bitmap(d, x + 20, y + 28, letters[r - 10], 5, 7, 4, ink);
		pip(d, s, x + CW / 2, y + 20, 10);
		pip(d, s, x + CW / 2, y + CH - 20, 10);
	} else
		pip(d, s, x + CW / 2, y + CH / 2 + 2, r == 0 ? 34 : 26);
}

/* The back: a blue field with a white frame and a lattice of lighter lines, a diamond at its middle. */
static void
back_art(GR_DRAW_ID d, int x, int y)
{
	int k;
	GR_POINT p[4];

	blank(d, x, y, MWRGB(30, 70, 170), INK);
	GrSetGCForeground(ui_gc, WHITE);
	GrRect(d, ui_gc, x + 3, y + 3, CW - 6, CH - 6);
	GrSetGCForeground(ui_gc, MWRGB(80, 130, 230));
	for (k = -CH; k < CW; k += 8) {	/* lines both ways inside the frame, cut to it */
		int x0 = k, y0 = 0, x1 = k + CH, y1 = CH;

		if (x0 < 0)
			y0 = -x0, x0 = 0;
		if (x1 > CW)
			y1 -= x1 - CW, x1 = CW;
		if (y0 < 5) x0 += 5 - y0, y0 = 5;
		if (x0 < 5) y0 += 5 - x0, x0 = 5;
		if (y1 > CH - 6) x1 -= y1 - (CH - 6), y1 = CH - 6;
		if (x1 > CW - 6) y1 -= x1 - (CW - 6), x1 = CW - 6;
		if (x1 > x0) {
			GrLine(d, ui_gc, x + x0, y + y0, x + x1, y + y1);
			GrLine(d, ui_gc, x + CW - 1 - x0, y + y0, x + CW - 1 - x1, y + y1);
		}
	}
	p[0].x = x + CW / 2, p[0].y = y + CH / 2 - 14, p[1].x = x + CW / 2 + 10, p[1].y = y + CH / 2;
	p[2].x = x + CW / 2, p[2].y = y + CH / 2 + 14, p[3].x = x + CW / 2 - 10, p[3].y = y + CH / 2;
	GrSetGCForeground(ui_gc, MWRGB(250, 214, 60));
	GrFillPoly(d, ui_gc, 4, p);
}

/* An empty place: a darker green with an edge; the stock's when it can be turned again (a ring),
 * a foundation's (an A). */
static void
place_art(GR_DRAW_ID d, int x, int y, int kind)
{
	ui_fill(d, x, y, CW, CH, TABLE);
	ui_fill(d, x + 1, y + 1, CW - 2, CH - 2, TABLE_DARK);
	GrSetGCForeground(ui_gc, MWRGB(90, 170, 110));
	GrRect(d, ui_gc, x, y, CW, CH);
	if (kind == S_STOCK_AGAIN) {
		GrEllipse(d, ui_gc, x + CW / 2, y + CH / 2, 14, 14);
		GrEllipse(d, ui_gc, x + CW / 2, y + CH / 2, 13, 13);
		ui_fill(d, x + CW / 2 + 9, y + CH / 2 - 16, 10, 8, TABLE_DARK);	/* the ring's gap, and its arrow's head */
		ui_fill(d, x + CW / 2 + 9, y + CH / 2 - 12, 6, 2, MWRGB(90, 170, 110));
		ui_fill(d, x + CW / 2 + 13, y + CH / 2 - 16, 2, 6, MWRGB(90, 170, 110));
	} else if (kind == S_HOME)
		bitmap(d, x + CW / 2 - 10, y + CH / 2 - 14, (const char *const[]){ "  #  ", " # # ", "#   #", "#   #", "#####", "#   #", "#   #" },
		       5, 7, 4, MWRGB(90, 170, 110));
}

static void
make_sheet(void)
{
	int c;

	if (sheet)
		GrDestroyWindow(sheet);
	sheet = GrNewPixmap(SHEET_W, SHEET_H, NULL);
	ui_fill(sheet, 0, 0, SHEET_W, SHEET_H, TABLE);
	for (c = 0; c < 52; c++)
		face_art(sheet, c, RANK(c) * CW, SUIT(c) * CH);
	back_art(sheet, S_BACK * CW, 4 * CH);
	place_art(sheet, S_EMPTY * CW, 4 * CH, S_EMPTY);
	place_art(sheet, S_STOCK_AGAIN * CW, 4 * CH, S_STOCK_AGAIN);
	place_art(sheet, S_HOME * CW, 4 * CH, S_HOME);
}

static void
put_card(int c, int x, int y, int h)
{
	if (c & UP)
		GrCopyArea(back, ui_gc, x, y, CW, h, sheet, RANK(c) * CW, SUIT(c) * CH, MWROP_COPY);
	else
		GrCopyArea(back, ui_gc, x, y, CW, h, sheet, S_BACK * CW, 4 * CH, MWROP_COPY);
}

static void
put_place(int kind, int x, int y)
{
	GrCopyArea(back, ui_gc, x, y, CW, CH, sheet, kind * CW, 4 * CH, MWROP_COPY);
}

static void
outline(int x, int y, int w, int h)
{
	GrSetGCForeground(ui_gc, PICKED);
	GrRect(back, ui_gc, x - 1, y - 1, w + 2, h + 2);
	GrRect(back, ui_gc, x - 2, y - 2, w + 4, h + 4);
}

static void
draw_bar(void)
{
	char text[80];
	int b, s = seconds(), w;

	ui_fill(back, 0, BAR_Y, WIN_W, BAR_H, UI_FACE);
	ui_fill(back, 0, BAR_Y, WIN_W, 1, UI_SHADOW);
	for (b = 0; b < BUTTONS; b++) {
		int tw = ui_width(&ui_var_widths, ui_var, button_names[b], -1);

		if (ui_seven)
			ui_button_seven(back, button_x(b), BAR_Y + 4, BUTTON_W, BUTTON_H, pressed_button == b);
		else {
			ui_fill(back, button_x(b), BAR_Y + 4, BUTTON_W, BUTTON_H, UI_FACE);
			ui_bevel(back, button_x(b), BAR_Y + 4, BUTTON_W, BUTTON_H, pressed_button == b);
		}
		ui_text(back, button_x(b) + (BUTTON_W - tw) / 2, BAR_Y + 10, button_names[b], -1,
			(b == B_UNDO && (!moves || won)) || (b == B_FINISH && won) ? UI_SHADOW : BLACK, 0);
	}
	if (won)
		snprintf(text, sizeof text, "Won in %d moves, %d:%02d", moves, s / 60, s % 60);
	else
		snprintf(text, sizeof text, "Moves %d   %d:%02d   Deal %u", moves, s / 60, s % 60, seed);
	w = ui_width(&ui_var_widths, ui_var, text, -1);
	ui_text(back, WIN_W - 10 - w, BAR_Y + 10, text, -1, BLACK, 0);
}

/* The whole window into its picture, then the part that changed (all of it with w 0) to the window. */
static void
render(int x0, int y0, int w0, int h0)
{
	int p, i, x, y, from;

	ui_fill(back, 0, 0, WIN_W, MENU_H, UI_FACE);
	for (i = 0; i < 2; i++)
		ui_text(back, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(back, 0, MENU_H - 1, WIN_W, 1, UI_SHADOW);
	ui_fill(back, 0, MENU_H, WIN_W, BAR_Y - MENU_H, TABLE);
	for (p = 0; p < PILES; p++) {
		int lifted = dragging && p == press_pile ? press_index : count[p];

		if (p == STOCK) {
			if (count[p])
				put_card(top(p), pile_x(p), TOP_Y, CH);
			else
				put_place(count[WASTE] ? S_STOCK_AGAIN : S_EMPTY, pile_x(p), TOP_Y);
			continue;
		}
		if (p < TAB) {	/* the waste and the foundations: the top, or the waste's three */
			from = p == WASTE && draw_n == 3 ? count[p] - 3 : count[p] - 1;
			from = from < 0 ? 0 : from;
			if (lifted == 0)
				put_place(p == WASTE ? S_EMPTY : S_HOME, pile_x(p), TOP_Y);
			else if (from >= lifted)
				put_card(pile[p][lifted - 1], pile_x(p), TOP_Y, CH);	/* (the one under a card carried off) */
			for (i = from; i < lifted; i++) {
				card_at(p, i, &x, &y);
				put_card(pile[p][i], x, y, CH);
			}
		} else {
			put_place(S_EMPTY, pile_x(p), TAB_Y);
			for (i = 0; i < lifted; i++) {
				card_at(p, i, &x, &y);
				put_card(pile[p][i], x, y, CH);
			}
		}
	}
	if (sel_pile >= 0 && !dragging) {
		int x1, y1;

		card_at(sel_pile, sel_index, &x, &y);
		card_at(sel_pile, count[sel_pile] - 1, &x1, &y1);
		outline(x, y, x1 + CW - x, y1 + CH - y);
	}
	if (dragging) {
		int fx, fy;

		card_at(press_pile, press_index, &fx, &fy);	/* (the run keeps its own steps, from its first card) */
		for (i = press_index; i < count[press_pile]; i++) {
			card_at(press_pile, i, &x, &y);
			put_card(pile[press_pile][i], drag_x + x - fx, drag_y + y - fy, CH);
		}
	}
	draw_bar();
	if (w0 <= 0)
		x0 = 0, y0 = 0, w0 = WIN_W, h0 = WIN_H;
	GrCopyArea(window, ui_gc, x0, y0, w0, h0, back, x0, y0, MWROP_COPY);
}

static void
render_all(void)
{
	render(0, 0, 0, 0);
}

/* A carried run's rectangle on the table: what a move of it changes. */
static void
drag_box(int *x, int *y, int *w, int *h)
{
	int x1, y1, xa, ya;

	card_at(press_pile, press_index, &xa, &ya);
	card_at(press_pile, count[press_pile] - 1, &x1, &y1);
	*x = drag_x - 3, *y = drag_y - 3, *w = x1 - xa + CW + 6, *h = y1 - ya + CH + 6;
}

/* ---- what the player does ---- */

static void
measure_menus(void)
{
	int i, x;

	for (i = 0, x = 2; i < 2; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[2] = x;
}

static void
new_game(uint32_t number)
{
	seed = number ? number : (uint32_t)(now_ms() ^ (long)getpid() << 12) % 1000000u + 1;
	deal(seed);
	unlink(KEPT "/game");
	render_all();
	say("deal");
}

/* After a move by the player: Finish starts by itself once the game is open. */
static void
after(const char *what)
{
	if (open_game() && !finishing)
		finishing = 1;
	render_all();
	say(what);
	if (won)
		say_stats();
}

/* The window's events while a question is up: its picture shown again, a close kept. */
static void leave(void);

static void
other_event(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		GrCopyArea(window, ui_gc, event->exposure.x, event->exposure.y, event->exposure.width, event->exposure.height, back,
			   event->exposure.x, event->exposure.y, MWROP_COPY);
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		leave();
}

static void
set_draw(int n)
{
	draw_n = n;
	reg_set_int("nxsolitaire.draw", n);
	render_all();
}

static void
show_stats(void)
{
	static const char *const answers[2] = { "OK", "Reset" };
	char text[300];

	for (;;) {
		snprintf(text, sizeof text,
			 "Games played:  %d\nGames won:  %d (%d%%)\nWins in a row:  %d, the most %d\nFastest win:  %d:%02d\nFewest moves:  %d",
			 stats.played, stats.won, stats.played ? stats.won * 100 / stats.played : 0, stats.streak, stats.best_streak,
			 stats.best_seconds / 60, stats.best_seconds % 60, stats.best_moves);
		say_stats();
		if (ui_ask("Statistics", text, answers, 2, other_event) != 1)
			break;
		memset(&stats, 0, sizeof stats);
		stats.open = counted && !won;
		stats_save();
	}
	GrSetFocus(window);
}

static void
leave(void)
{
	game_save();
	if (printing)
		printf("nxsolitaire: %s\n", won || !moves ? "nothing kept" : "game kept"), fflush(stdout);
	GrClose();
	exit(0);
}

static void
command(int id)
{
	static const char *const ok[1] = { "OK" };

	sel_pile = -1;
	switch (id) {
	case C_NEW:
		new_game(0);
		break;
	case C_RESTART:
		deal(seed);
		render_all();
		say("deal again");
		break;
	case C_UNDO:
		if (undo())
			after("undo");
		break;
	case C_FINISH:
		if (!won) {
			finishing = 1;
			render_all();
			say("finish");
		}
		break;
	case C_DRAW1: case C_DRAW3:
		set_draw(id == C_DRAW1 ? 1 : 3);
		break;
	case C_STATS:
		show_stats();
		break;
	case C_HELP:
		ui_ask("How to play", "Build the four foundations up by suit, from the ace to the king.\n"
		       "In the seven columns a card goes on one a rank higher of the other colour,\n"
		       "and only a king on an empty column. Click the stock to turn cards over.\n"
		       "Click a card to pick it, then click where it goes, or drag it there.\n"
		       "A double click, the right button or Up sends a card to its foundation.\n"
		       "Keys: F2 a new game, Ctrl+Z or U undo, F finish, space the stock.", ok, 1, other_event);
		GrSetFocus(window);
		break;
	case C_ABOUT:
		ui_ask("About Solitaire", "Solitaire (Klondike)\nThe patience of seven columns, for the ShaderEmu desktop.", ok, 1, other_event);
		GrSetFocus(window);
		break;
	case C_EXIT:
		leave();
	}
}

static void
open_menu(int which, int keyboard)
{
	int wx, wy;

	game_items[1].flags = 0;
	game_items[2].flags = UI_MENU_LINE | (!moves || won ? UI_MENU_GREY : 0);
	game_items[3].flags = won ? UI_MENU_GREY : 0;
	game_items[4].flags = UI_MENU_LINE | (draw_n == 1 ? UI_MENU_CHECK : 0);
	game_items[5].flags = draw_n == 3 ? UI_MENU_CHECK : 0;
	ui_window_origin(window, &wx, &wy);
	ui_menu_open(menus[which].items, menus[which].count, wx + menu_x[which], wy + MENU_H, 0, keyboard);
}

/* A click (a press and its release with no drag) on pile p's card i (-1 its empty place). */
static void
click(int p, int i, int button)
{
	long t = now_ms();
	int twice = p == last_click_pile && i == last_click_index && t - last_click_ms < DOUBLE_MS;

	last_click_pile = p, last_click_index = i, last_click_ms = t;
	if (p == STOCK) {
		sel_pile = -1;
		if (turn_stock())
			after("stock");
		return;
	}
	if (button == GR_BUTTON_R || twice) {	/* to a foundation */
		sel_pile = -1;
		last_click_pile = -1;
		if (i >= 0 && i == count[p] - 1 && send_up(p))
			after("up");
		else
			render_all();
		return;
	}
	if (sel_pile >= 0) {	/* a card picked: this is where it goes, or another card to pick */
		int from = sel_pile, n = count[sel_pile] - sel_index;

		sel_pile = -1;
		if (p != from && move_cards(from, n, p)) {
			last_click_pile = -1;
			after("placed");
			return;
		}
		if (p == from) {
			render_all();
			say("put down");
			return;
		}
	}
	if (movable(p, i)) {
		sel_pile = p, sel_index = i;
		render_all();
		say("picked");
	} else
		render_all();
}

/* The pile under most of a carried run when it is let go, among those it fits: else none. */
static int
drop_target(void)
{
	int p, best = -1, most = 0, c = pile[press_pile][press_index], n = count[press_pile] - press_index;

	for (p = FOUND; p < PILES; p++) {
		int x, y, w, h;

		if (p == press_pile || !fits(c, n, p))
			continue;
		card_at(p, count[p] ? count[p] - 1 : 0, &x, &y);
		w = CW - abs(drag_x - x), h = CH - abs(drag_y - y);
		if (w > 0 && h > 0 && w * h > most)
			most = w * h, best = p;
	}
	return best;
}

static void
button_down(GR_EVENT_BUTTON *b)
{
	int i = -1, p;

	if (b->y < MENU_H) {
		for (i = 0; i < 2; i++)
			if (b->x >= menu_x[i] && b->x < menu_x[i + 1])
				open_menu(i, 0);
		return;
	}
	if (finishing)
		return;
	ui_moves(window, EVENTS, 1);
	if ((pressed_button = on_button(b->x, b->y)) >= 0) {
		render(0, BAR_Y, WIN_W, BAR_H);
		return;
	}
	p = hit(b->x, b->y, &i);
	press_pile = p, press_index = i, press_x = b->x, press_y = b->y, dragging = 0;
	press_button = (b->changebuttons & GR_BUTTON_R) ? GR_BUTTON_R : GR_BUTTON_L;
}

static void
pointer_moved(GR_EVENT_MOUSE *m)
{
	int x, y, w, h, x2, y2, w2, h2;

	if (press_pile < 0 || press_button != GR_BUTTON_L)
		return;
	if (!dragging) {
		if (abs(m->x - press_x) < SLOP && abs(m->y - press_y) < SLOP)
			return;
		if (!movable(press_pile, press_index)) {
			press_pile = -1;	/* (a press off a card that can be carried: nothing, at its release too) */
			return;
		}
		card_at(press_pile, press_index, &x, &y);
		drag_dx = press_x - x, drag_dy = press_y - y;
		drag_x = x, drag_y = y;
		sel_pile = -1;
		dragging = 1;
		render_all();
	}
	drag_box(&x, &y, &w, &h);	/* where the run was and where it is now: the window changes there */
	drag_x = m->x - drag_dx, drag_y = m->y - drag_dy;
	drag_box(&x2, &y2, &w2, &h2);
	{
		int ux = x < x2 ? x : x2, uy = y < y2 ? y : y2;
		int ur = x + w > x2 + w2 ? x + w : x2 + w2, ub = y + h > y2 + h2 ? y + h : y2 + h2;

		render(ux, uy, ur - ux, ub - uy);
	}
}

static void
button_up(GR_EVENT_BUTTON *b)
{
	int was = pressed_button, p = press_pile, i = press_index;

	if (!(b->buttons & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M)))
		ui_moves(window, EVENTS, 0);
	if (was >= 0) {
		pressed_button = -1;
		render(0, BAR_Y, WIN_W, BAR_H);
		if (on_button(b->x, b->y) != was)
			return;
		if (was == B_NEW)
			command(C_NEW);
		else if (was == B_UNDO)
			command(C_UNDO);
		else if (was == B_FINISH)
			command(C_FINISH);
		else {	/* Up: the picked card, else the lowest card that can go */
			int s = sel_pile;

			sel_pile = -1;
			if (s >= 0 && sel_index == count[s] - 1 ? send_up(s) : finish_step())
				after("up");
			else
				render_all();
		}
		return;
	}
	if (dragging && p >= 0) {
		int to = drop_target(), n = count[p] - i;

		dragging = 0, press_pile = -1;
		if (to >= 0 && move_cards(p, n, to))
			after("dragged");
		else {
			render_all();
			say("dropped back");
		}
		return;
	}
	press_pile = -1;
	if (p >= 0)
		click(p, i, press_button);
	else if (sel_pile >= 0) {
		sel_pile = -1;
		render_all();
	}
}

static void
key(GR_EVENT_KEYSTROKE *k)
{
	int ch = k->ch;

	if (ch == MWKEY_F2 || ch == 'n')
		command(C_NEW);
	else if (ch == 'u' || ch == MWKEY_BACKSPACE || ((k->modifiers & MWKMOD_CTRL) && (ch == 'z' || ch == 26)))
		command(C_UNDO);
	else if (ch == 'f')
		command(C_FINISH);
	else if (ch == ' ') {
		sel_pile = -1;
		if (turn_stock())
			after("stock");
	} else if (ch == MWKEY_F10)
		open_menu(0, 1);
	else if (ch == MWKEY_ESCAPE && sel_pile >= 0) {
		sel_pile = -1;
		render_all();
	} else if (ch == 'q')
		leave();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int id, resumed = 0;

	if (GrOpen() < 0)
		return 1;
	if (ui_single_instance()) {
		GrClose();
		return 0;
	}
	ui_init();
	printing = getenv("NXSOLITAIRE_PRINT") ? atoi(getenv("NXSOLITAIRE_PRINT")) : 0;
	nearly = getenv("NXSOLITAIRE_DEAL") && !strcmp(getenv("NXSOLITAIRE_DEAL"), "nearly");
	draw_n = getenv("NXSOLITAIRE_DRAW") ? atoi(getenv("NXSOLITAIRE_DRAW")) : reg_int("nxsolitaire.draw", 1);
	draw_n = draw_n == 3 ? 3 : 1;
	stats_load();
	measure_menus();
	window = GrNewWindowEx(UI_FIXED, "Solitaire", GR_ROOT_WINDOW_ID, -1, -1, WIN_W, WIN_H, TABLE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	back = GrNewPixmap(WIN_W, WIN_H, NULL);
	make_sheet();
	if (argc > 1 || nearly)
		seed = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 1, deal(seed);
	else if ((resumed = game_load()) == 0)
		seed = (uint32_t)(now_ms() ^ (long)getpid() << 12) % 1000000u + 1, deal(seed);
	if (printing) {
		printf("nxsolitaire: %d x %d, cards of %d x %d, the stock at %d,%d, buttons at y %d: New %d, Undo %d, Up %d, Finish %d\n", WIN_W,
		       WIN_H, CW, CH, LEFT + CW / 2, TOP_Y + CH / 2, BAR_Y + 4 + BUTTON_H / 2, button_x(B_NEW) + BUTTON_W / 2,
		       button_x(B_UNDO) + BUTTON_W / 2, button_x(B_UP) + BUTTON_W / 2, button_x(B_FINISH) + BUTTON_W / 2);
		fflush(stdout);
	}
	render_all();
	say(resumed ? "resumed" : "deal");
	for (;;) {
		if (finishing)
			GrGetNextEventTimeout(&event, FINISH_MS);
		else if (clock_from >= 0 && !won)	/* (the clock is drawn each second while it runs) */
			GrGetNextEventTimeout(&event, (GR_TIMEOUT)(1000 - (now_ms() - clock_from + played_ms) % 1000 + 1));
		else
			GrGetNextEvent(&event);
		ui_origin_note(&event);
		if (event.type == GR_EVENT_TYPE_TIMEOUT) {
			if (finishing) {	/* a card up, until none can go or all are home */
				if (finish_step()) {
					render_all();
					if (won) {
						say("finished");
						say_stats();
					}
				} else {
					finishing = 0;
					render_all();
					say("finish stopped");
				}
			} else if (!won)
				render(0, BAR_Y, WIN_W, BAR_H);
			continue;
		}
		if (ui_restyle()) {	/* (Settings changed the look) */
			ui_menu_restyle();
			measure_menus();
			make_sheet();
			render_all();
		}
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id);
			continue;
		case UI_MENU_CLOSED:
			GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window)
				GrCopyArea(window, ui_gc, event.exposure.x, event.exposure.y, event.exposure.width, event.exposure.height, back,
					   event.exposure.x, event.exposure.y, MWROP_COPY);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid == window && !ui_wheel(&event))
				button_down(&event.button);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (event.mouse.wid == window)
				pointer_moved(&event.mouse);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (event.button.wid == window)
				button_up(&event.button);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.wid == window)
				key(&event.keystroke);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid == window)
				leave();
			break;
		}
	}
}
