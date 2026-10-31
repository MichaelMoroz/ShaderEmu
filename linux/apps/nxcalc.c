/*
 * nxcalc: the calculator, laid out as Windows 7's: Standard, Scientific (with the operators'
 * precedence and brackets) and Programmer (a word of 8 to 64 bits), a history of results and
 * the keyboard of Windows' calculator. Numbers are doubles, the compiler's software arithmetic.
 * `nxcalc -e KEYS...` types the keys without a window and prints the display: a test.
 */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>

#define MENU_H 20
#define KW 34		/* a key */
#define KH 27
#define GAP 5
#define MARGIN 11
#define DISPLAY_H 58
#define HISTORY_H 112	/* six rows */
#define BITS_H 50
#define RADIO_W 84
#define RADIO_ROW 19
#define HISTORY_MOST 100
#define STACK_MOST 60
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)
/* the window's ground: Windows 7's calculator is pale blue */
#define GROUND (ui_seven ? MWRGB(217, 228, 241) : UI_FACE)

enum { STANDARD, SCIENTIFIC, PROGRAMMER };
enum { DEG, RAD, GRAD };

/* the keys; 0 to 15 are the digits */
enum {
	K_DOT = 16, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ, K_BACK, K_CE, K_C, K_NEG, K_SQRT, K_PCT, K_RECIP,
	K_MC, K_MR, K_MS, K_MPLUS, K_MMINUS,
	K_INV, K_LN, K_LP, K_RP, K_INT, K_SINH, K_SIN, K_SQR, K_FACT, K_DMS, K_COSH, K_COS, K_POW, K_ROOT,
	K_PI, K_TANH, K_TAN, K_CUBE, K_CBRT, K_FE, K_EXP, K_MOD, K_LOG, K_TEN,
	K_ROL, K_ROR, K_OR, K_XOR, K_LSH, K_RSH, K_NOT, K_AND,
};
#define IS_DIGIT(id) ((id) >= 0 && (id) < 16)

/* What the menus and some keys do. */
enum { C_STANDARD = 100, C_SCIENTIFIC, C_PROGRAMMER, C_HISTORY, C_GROUPING, C_COPY, C_PASTE, C_COPY_HISTORY,
	C_CLEAR_HISTORY, C_ABOUT, C_DEG, C_RAD, C_GRAD, C_HEX, C_DEC, C_OCT, C_BIN, C_QWORD, C_DWORD, C_WORD, C_BYTE,
	C_MENU };

/* A key's place: in the standard grid (every mode's), the scientific columns or the programmer's,
 * by column and row. A label's ^ raises the next character; \1 is a root sign, \2 an arrow
 * left, \3 plus-minus, \4 pi. `inverse` is the label while Inv is on. */
enum { PANE_STD, PANE_SCI, PANE_PROG };
struct key { int id, pane, col, row, cols, rows; const char *label, *inverse; };
static const struct key keys[] = {
	{ K_MC, 0, 0, 0, 1, 1, "MC" }, { K_MR, 0, 1, 0, 1, 1, "MR" }, { K_MS, 0, 2, 0, 1, 1, "MS" },
	{ K_MPLUS, 0, 3, 0, 1, 1, "M+" }, { K_MMINUS, 0, 4, 0, 1, 1, "M-" },
	{ K_BACK, 0, 0, 1, 1, 1, "\2" }, { K_CE, 0, 1, 1, 1, 1, "CE" }, { K_C, 0, 2, 1, 1, 1, "C" },
	{ K_NEG, 0, 3, 1, 1, 1, "\3" }, { K_SQRT, 0, 4, 1, 1, 1, "\1" },
	{ 7, 0, 0, 2, 1, 1, "7" }, { 8, 0, 1, 2, 1, 1, "8" }, { 9, 0, 2, 2, 1, 1, "9" },
	{ K_DIV, 0, 3, 2, 1, 1, "/" }, { K_PCT, 0, 4, 2, 1, 1, "%" },
	{ 4, 0, 0, 3, 1, 1, "4" }, { 5, 0, 1, 3, 1, 1, "5" }, { 6, 0, 2, 3, 1, 1, "6" },
	{ K_MUL, 0, 3, 3, 1, 1, "*" }, { K_RECIP, 0, 4, 3, 1, 1, "1/x" },
	{ 1, 0, 0, 4, 1, 1, "1" }, { 2, 0, 1, 4, 1, 1, "2" }, { 3, 0, 2, 4, 1, 1, "3" },
	{ K_SUB, 0, 3, 4, 1, 1, "-" }, { K_EQ, 0, 4, 4, 1, 2, "=" },
	{ 0, 0, 0, 5, 2, 1, "0" }, { K_DOT, 0, 2, 5, 1, 1, "." }, { K_ADD, 0, 3, 5, 1, 1, "+" },

	{ K_INV, 1, 1, 1, 1, 1, "Inv" }, { K_LN, 1, 2, 1, 1, 1, "ln", "e^x" }, { K_LP, 1, 3, 1, 1, 1, "(" },
	{ K_RP, 1, 4, 1, 1, 1, ")" },
	{ K_INT, 1, 0, 2, 1, 1, "Int", "Frac" }, { K_SINH, 1, 1, 2, 1, 1, "sinh", "sinh^-^1" },
	{ K_SIN, 1, 2, 2, 1, 1, "sin", "sin^-^1" }, { K_SQR, 1, 3, 2, 1, 1, "x^2" }, { K_FACT, 1, 4, 2, 1, 1, "n!" },
	{ K_DMS, 1, 0, 3, 1, 1, "dms", "deg" }, { K_COSH, 1, 1, 3, 1, 1, "cosh", "cosh^-^1" },
	{ K_COS, 1, 2, 3, 1, 1, "cos", "cos^-^1" }, { K_POW, 1, 3, 3, 1, 1, "x^y" }, { K_ROOT, 1, 4, 3, 1, 1, "^y\1x" },
	{ K_PI, 1, 0, 4, 1, 1, "\4", "2\4" }, { K_TANH, 1, 1, 4, 1, 1, "tanh", "tanh^-^1" },
	{ K_TAN, 1, 2, 4, 1, 1, "tan", "tan^-^1" }, { K_CUBE, 1, 3, 4, 1, 1, "x^3" }, { K_CBRT, 1, 4, 4, 1, 1, "^3\1x" },
	{ K_FE, 1, 0, 5, 1, 1, "F-E" }, { K_EXP, 1, 1, 5, 1, 1, "Exp" }, { K_MOD, 1, 2, 5, 1, 1, "Mod" },
	{ K_LOG, 1, 3, 5, 1, 1, "log" }, { K_TEN, 1, 4, 5, 1, 1, "10^x" },

	{ K_MOD, 2, 1, 0, 1, 1, "Mod" }, { 10, 2, 2, 0, 1, 1, "A" },
	{ K_LP, 2, 0, 1, 1, 1, "(" }, { K_RP, 2, 1, 1, 1, 1, ")" }, { 11, 2, 2, 1, 1, 1, "B" },
	{ K_ROL, 2, 0, 2, 1, 1, "RoL" }, { K_ROR, 2, 1, 2, 1, 1, "RoR" }, { 12, 2, 2, 2, 1, 1, "C" },
	{ K_OR, 2, 0, 3, 1, 1, "Or" }, { K_XOR, 2, 1, 3, 1, 1, "Xor" }, { 13, 2, 2, 3, 1, 1, "D" },
	{ K_LSH, 2, 0, 4, 1, 1, "Lsh" }, { K_RSH, 2, 1, 4, 1, 1, "Rsh" }, { 14, 2, 2, 4, 1, 1, "E" },
	{ K_NOT, 2, 0, 5, 1, 1, "Not" }, { K_AND, 2, 1, 5, 1, 1, "And" }, { 15, 2, 2, 5, 1, 1, "F" },
};
#define KEY_COUNT ((int)(sizeof keys / sizeof keys[0]))

/* ---- the state ---- */

static int mode, angle, base = 10, word = 32, grouping, show_history, inv, fe;
struct num { double d; uint64_t u; };	/* d in Standard and Scientific, u (in the word) in Programmer */
static struct num value, memory;
static int has_memory;
static char entry[96];		/* what is being typed, while `typing` */
static int typing;
static char error[40];		/* shown in place of a number until a key clears it */
/* what waits to be worked out: operands, operators (-1 for a bracket) and where each bracket is in `expr` */
static struct num vals[STACK_MOST + 2];
static int ops[STACK_MOST + 2], paren_at[STACK_MOST + 2], nvals, nops, nparens;
static char expr[512];		/* the expression's line */
static char operand[300];	/* the operand as functions made it ("sqrt(9)"), or empty: the number */
static int shown, shown_at;	/* the operand is a bracket already in `expr`, from shown_at */
static int after_op, op_at;	/* an operator was the last key, and where it is in `expr` */
static int again_op;		/* `=` again does the last operation once more */
static struct num again_val;
static struct { char text[160]; struct num result; } history[HISTORY_MOST];
static struct ui_list history_list = { 0, 0, 0, HISTORY_H, 0, 0, -1 };

enum { D_DISPLAY = 1, D_KEYS = 2, D_HISTORY = 4, D_ALL = 8 };
static int dirty;

/* ---- the window ---- */

static GR_WINDOW_ID window;
static int width, height, grid_x, sci_x, prog_x, grid_y, display_y, bits_y;
static GR_FONT_ID big, small;
static struct ui_widths big_widths, small_widths;
static int big_h, small_h, var_h, small_own;
static int lit = -1;		/* the key shown pressed, an index of keys[] */
static int held = -1;		/* the key the pointer went down on */
static struct { int x, y, w, h, command; } radios[12];
static int radio_count;

static const char *const menu_names[] = { "View", "Edit", "Help" };
static int menu_x[4];
static struct ui_menu_item view_items[] = {
	{ "Standard   Alt+1", -1, C_STANDARD }, { "Scientific   Alt+2", -1, C_SCIENTIFIC }, { "Programmer   Alt+3", -1, C_PROGRAMMER },
	{ "History   Ctrl+H", -1, C_HISTORY, NULL, 0, UI_MENU_LINE }, { "Digit grouping", -1, C_GROUPING },
};
static struct ui_menu_item edit_items[] = {
	{ "Copy   Ctrl+C", -1, C_COPY }, { "Paste   Ctrl+V", -1, C_PASTE },
	{ "Copy history", -1, C_COPY_HISTORY, NULL, 0, UI_MENU_LINE }, { "Clear history", -1, C_CLEAR_HISTORY },
};
static struct ui_menu_item help_items[] = { { "About Calculator   F1", -1, C_ABOUT } };
static struct ui_menu_item pointer_items[] = { { "Copy", -1, C_COPY }, { "Paste", -1, C_PASTE } };

static void
remember(const char *key, int n)
{
	if (window)	/* (the test without a window keeps the defaults) */
		reg_set_int(key, n);
}

/* ---- numbers ---- */

static uint64_t
mask(void)
{
	return word == 64 ? ~(uint64_t)0 : ((uint64_t)1 << word) - 1;
}

static int64_t
as_signed(uint64_t u)
{
	u &= mask();
	if (word < 64 && (u >> (word - 1) & 1))
		u |= ~mask();
	return (int64_t)u;
}

static struct num
from_u(uint64_t u)
{
	struct num n;

	n.u = u & mask();
	n.d = (double)as_signed(n.u);
	return n;
}

static struct num
from_d(double d)
{
	struct num n;
	double t = trunc(d);

	n.d = d;
	n.u = (t != t || t >= 9.2e18 || t <= -9.2e18) ? 0 : (uint64_t)(int64_t)t & mask();
	return n;
}

/* Separators into the first run of digits of s (after a sign), every `every` from the right. */
static void
group(char *s, int every, char sep)
{
	char out[200], *start = s + (*s == '-'), *end = start;
	int n, i, k = 0;

	while (isalnum((unsigned char)*end) && *end != 'e')
		end++;
	n = (int)(end - start);
	memcpy(out, s, start - s);
	k = (int)(start - s);
	for (i = 0; i < n && k < (int)sizeof out - 3; i++) {
		if (i > 0 && (n - i) % every == 0)
			out[k++] = sep;
		out[k++] = start[i];
	}
	snprintf(out + k, sizeof out - k, "%s", end);
	strcpy(s, out);
}

/* A double as Windows' calculator shows it: 15 digits, or 16 for a whole number; F-E: exponent always. */
static void
format_double(double v, char *out, int size)
{
	char t[64], *e, *z;

	if (v == 0)
		snprintf(t, sizeof t, "0");
	else if (fe)
		snprintf(t, sizeof t, "%.15e", v);
	else if (fabs(v) < 1e16 && v == floor(v))
		snprintf(t, sizeof t, "%.0f", v);
	else
		snprintf(t, sizeof t, "%.15g", v);
	if ((e = strchr(t, 'e')) != NULL) {
		char exponent[16];

		/* "1.500000e+03" is "1.5e+3" */
		snprintf(exponent, sizeof exponent, "e%c%d", e[1], atoi(e + 2));
		*e = 0;
		if (strchr(t, '.')) {
			for (z = e - 1; *z == '0'; z--)
				*z = 0;
			if (*z == '.')
				*z = 0;
		}
		strcat(t, exponent);
	}
	snprintf(out, size, "%s", t);
}

/* A number in the mode and base shown, with the digits grouped when `grouped`. */
static void
format_num(struct num n, char *out, int size, int grouped)
{
	char t[100];
	int i = 99, b = base;
	uint64_t u = n.u & mask();

	if (mode != PROGRAMMER) {
		format_double(n.d, t, sizeof t);
		if (grouped)
			group(t, 3, ',');
	} else if (base == 10) {
		snprintf(t, sizeof t, "%lld", (long long)as_signed(u));
		if (grouped)
			group(t, 3, ',');
	} else {
		char digits[70];

		digits[69] = 0;
		i = 69;
		do {
			digits[--i] = "0123456789ABCDEF"[u % b];
			u /= b;
		} while (u);
		snprintf(t, sizeof t, "%s", digits + i);
		if (grouped)
			group(t, b == 8 ? 3 : 4, ' ');
	}
	snprintf(out, size, "%s", t);
}

static struct num
parse_entry(void)
{
	if (mode == PROGRAMMER)
		return from_u(strtoull(entry, NULL, base));
	return from_d(strtod(entry, NULL));
}

static struct num
current(void)
{
	return typing ? parse_entry() : value;
}

/* What the display shows: the error, the number being typed, or the value. */
static void
display_text(char *out, int size, int grouped)
{
	char t[200];

	if (error[0]) {
		snprintf(out, size, "%s", error);
		return;
	}
	if (!typing) {
		format_num(value, out, size, grouped);
		return;
	}
	snprintf(t, sizeof t, "%s", entry);
	if (grouped)
		group(t, mode != PROGRAMMER || base == 10 ? 3 : base == 8 ? 3 : 4, mode != PROGRAMMER || base == 10 ? ',' : ' ');
	snprintf(out, size, "%s", t);
}

/* ---- the expression ---- */

static void
say(const char *text)
{
	size_t n = strlen(expr);

	snprintf(expr + n, sizeof expr - n, "%s", text);
}

static void
history_add(const char *text, struct num result)
{
	char r[100];
	int rows = ui_list_rows(&history_list);

	if (history_list.count == HISTORY_MOST) {
		memmove(history, history + 1, (HISTORY_MOST - 1) * sizeof history[0]);
		history_list.count--;
	}
	format_num(result, r, sizeof r, grouping);
	snprintf(history[history_list.count].text, sizeof history[0].text, "%.100s %.58s", text, r);
	history[history_list.count].result = result;
	history_list.count++;
	history_list.selected = -1;
	history_list.top = history_list.count > rows ? history_list.count - rows : 0;
	dirty |= D_HISTORY;
}

static void
clear_all(void)
{
	if (error[0])
		dirty |= D_KEYS;
	nvals = nops = nparens = 0;
	expr[0] = operand[0] = error[0] = 0;
	typing = shown = after_op = again_op = 0;
	memset(&value, 0, sizeof value);
}

static void
fail(const char *why)
{
	clear_all();
	snprintf(error, sizeof error, "%s", why);
	dirty |= D_KEYS;
}

/* A result that is no number fails; true if it did. */
static int
failed(double r)
{
	if (r != r)
		fail("Invalid input");
	else if (isinf(r))
		fail("Overflow");
	else
		return 0;
	return 1;
}

static const char *
op_name(int op)
{
	switch (op) {
	case K_ADD: return "+";
	case K_SUB: return "-";
	case K_MUL: return "*";
	case K_DIV: return "/";
	case K_MOD: return "Mod";
	case K_POW: return "^";
	case K_ROOT: return "yroot";
	case K_AND: return "And";
	case K_OR: return "Or";
	case K_XOR: return "Xor";
	case K_LSH: return "Lsh";
	case K_RSH: return "Rsh";
	}
	return "?";
}

/* Standard works from left to right; the others as C (Programmer) or as algebra has it. */
static int
precedence(int op)
{
	if (mode == STANDARD)
		return 1;
	if (mode == PROGRAMMER)
		switch (op) {
		case K_OR: return 1;
		case K_XOR: return 2;
		case K_AND: return 3;
		case K_LSH: case K_RSH: return 4;
		case K_ADD: case K_SUB: return 5;
		default: return 6;
		}
	return op == K_ADD || op == K_SUB ? 1 : op == K_POW || op == K_ROOT ? 3 : 2;
}

/* a op b into *r; true if it failed (and the calculator says why). */
static int
apply(int op, struct num a, struct num b, struct num *r)
{
	double x = a.d, y = b.d, z = 0;

	if (mode == PROGRAMMER) {
		int64_t sa = as_signed(a.u), sb = as_signed(b.u);
		uint64_t u = 0, n = b.u & mask();

		switch (op) {
		case K_ADD: u = a.u + b.u; break;
		case K_SUB: u = a.u - b.u; break;
		case K_MUL: u = a.u * b.u; break;
		case K_DIV:
		case K_MOD:
			if (sb == 0) {
				fail("Cannot divide by zero");
				return 1;
			}
			if (sb == -1)
				u = op == K_DIV ? (uint64_t)0 - (uint64_t)sa : 0;
			else
				u = (uint64_t)(op == K_DIV ? sa / sb : sa % sb);
			break;
		case K_AND: u = a.u & b.u; break;
		case K_OR: u = a.u | b.u; break;
		case K_XOR: u = a.u ^ b.u; break;
		case K_LSH: u = n >= (uint64_t)word ? 0 : a.u << n; break;
		case K_RSH: u = (uint64_t)(sa >> (n >= (uint64_t)word ? word - 1 : (int)n)); break;
		}
		*r = from_u(u);
		return 0;
	}
	switch (op) {
	case K_ADD: z = x + y; break;
	case K_SUB: z = x - y; break;
	case K_MUL: z = x * y; break;
	case K_DIV:
	case K_MOD:
		if (y == 0) {
			fail(x == 0 ? "Result is undefined" : "Cannot divide by zero");
			return 1;
		}
		z = op == K_DIV ? x / y : fmod(x, y);
		break;
	case K_POW:
		if (x == 0 && y < 0) {
			fail("Cannot divide by zero");
			return 1;
		}
		z = x < 0 && y != floor(y) ? NAN : pow(x, y);
		break;
	case K_ROOT:	/* x yroot y: the y-th root of x */
		if (y == 0)
			z = NAN;
		else if (x < 0)
			z = y == floor(y) && fmod(y, 2) != 0 ? -pow(-x, 1 / y) : NAN;
		else
			z = pow(x, 1 / y);
		break;
	}
	if (failed(z))
		return 1;
	*r = from_d(z);
	return 0;
}

static int
apply_top(void)
{
	struct num r;
	int op = ops[--nops];

	if (nvals < 2)
		return 0;
	nvals -= 2;
	if (apply(op, vals[nvals], vals[nvals + 1], &r))
		return 1;
	vals[nvals++] = r;
	return 0;
}

/* Works out what waits above a bracket while it binds at least as tightly as an operator of
 * precedence p (more tightly, for one that groups from the right); true if it failed. */
static int
reduce(int p, int right)
{
	while (nops && ops[nops - 1] >= 0 &&
	       (precedence(ops[nops - 1]) > p || (precedence(ops[nops - 1]) == p && !right)))
		if (apply_top())
			return 1;
	return 0;
}

/* The operand's text for the expression's line. */
static void
operand_text(char *out, int size, struct num a)
{
	if (operand[0])
		snprintf(out, size, "%s", operand);
	else
		format_num(a, out, size, grouping);
}

/* A number being typed ends: it is the value. */
static void
commit(void)
{
	if (typing)
		value = parse_entry();
	typing = 0;
}

/* The operand is replaced (a number typed, recalled, a constant): a bracket shown goes. */
static void
new_operand(void)
{
	if (shown)
		expr[shown_at] = 0;
	shown = after_op = 0;
	operand[0] = 0;
}

static void
type_digit(int id)
{
	char t[100];
	int digits = 0;
	const char *c;

	if (!typing) {
		new_operand();
		snprintf(entry, sizeof entry, "0");
		typing = 1;
	}
	if (mode == PROGRAMMER) {
		uint64_t u;

		if (id >= base || id == K_DOT)
			return;
		snprintf(t, sizeof t, "%s%c", strcmp(entry, "0") ? entry : "", "0123456789ABCDEF"[id]);
		errno = 0;
		u = strtoull(t, NULL, base);
		if (errno || (u & ~mask()) || strlen(t) > 64)
			return;
		snprintf(entry, sizeof entry, "%s", t);
		return;
	}
	if (id == K_DOT) {
		if (!strchr(entry, '.') && !strchr(entry, 'e'))
			strcat(entry, ".");
		return;
	}
	if (id > 9)
		return;
	if ((c = strchr(entry, 'e')) != NULL) {
		snprintf(t, sizeof t, "%s%c", entry, '0' + id);
		if (strlen(c + 2) < 4 && !isinf(strtod(t, NULL)))	/* (no exponent past a double's) */
			snprintf(entry, sizeof entry, "%s", t);
		return;
	}
	for (c = entry; *c; c++)
		digits += isdigit((unsigned char)*c) != 0;
	if (digits >= 16)
		return;
	if (!strcmp(entry, "0") || !strcmp(entry, "-0"))
		entry[strlen(entry) - 1] = 0;
	snprintf(entry + strlen(entry), sizeof entry - strlen(entry), "%c", '0' + id);
}

static void
backspace(void)
{
	int n = (int)strlen(entry);

	if (!typing)
		return;
	if (n > 0)
		entry[--n] = 0;
	if (n >= 2 && (entry[n - 1] == '+' || entry[n - 1] == '-') && entry[n - 2] == 'e')
		entry[n -= 2] = 0;
	if (!n || !strcmp(entry, "-"))
		snprintf(entry, sizeof entry, "0");
}

static void
binary(int op)
{
	char t[300];
	int right = mode == SCIENTIFIC && (op == K_POW || op == K_ROOT);

	if (after_op && nops && ops[nops - 1] >= 0) {
		/* an operator after an operator takes its place */
		nops--;
		expr[op_at] = 0;
	} else {
		struct num a = current();

		if (nvals >= STACK_MOST || nops >= STACK_MOST)
			return;
		typing = 0;
		if (!shown) {
			operand_text(t, sizeof t, a);
			say(t);
			say(" ");
		}
		shown = 0;
		operand[0] = 0;
		vals[nvals++] = a;
	}
	if (reduce(precedence(op), right))
		return;
	value = vals[nvals - 1];
	ops[nops++] = op;
	op_at = (int)strlen(expr);
	say(op_name(op));
	say(" ");
	after_op = 1;
}

static void
open_bracket(void)
{
	if (nops >= STACK_MOST)
		return;
	typing = 0;
	new_operand();
	paren_at[nparens++] = (int)strlen(expr);
	ops[nops++] = -1;
	say("( ");
}

static void
close_bracket(void)
{
	char t[300];
	struct num a;

	if (!nparens || nvals >= STACK_MOST)
		return;
	a = current();
	typing = 0;
	if (!shown) {
		operand_text(t, sizeof t, a);
		say(t);
		say(" ");
	}
	vals[nvals++] = a;
	while (nops && ops[nops - 1] >= 0)
		if (apply_top())
			return;
	nops--;
	value = vals[--nvals];
	shown_at = paren_at[--nparens];
	say(") ");
	shown = 1;
	operand[0] = 0;
	after_op = 0;
}

static void
equals(void)
{
	char t[300], r[100];
	struct num a = current(), result;

	typing = 0;
	if (!nops && !nvals) {
		value = a;	/* (a number typed alone stays) */
		if (again_op && !operand[0] && !shown) {
			/* = again: the last operation once more, on what is shown */
			format_num(a, t, sizeof t, grouping);
			format_num(again_val, r, sizeof r, grouping);
			snprintf(expr, sizeof expr, "%s %s %s =", t, op_name(again_op), r);
			if (apply(again_op, a, again_val, &result))
				return;
			history_add(expr, result);
			value = result;
		} else if (operand[0] || shown) {
			operand_text(t, sizeof t, a);
			snprintf(expr, sizeof expr, "%s =", shown ? expr + shown_at : t);
			history_add(expr, a);
			value = a;
		}
		expr[0] = operand[0] = 0;
		shown = after_op = 0;
		return;
	}
	if (!shown) {
		operand_text(t, sizeof t, a);
		say(t);
		say(" ");
	}
	again_op = 0;
	if (nops && ops[nops - 1] >= 0)
		again_op = ops[nops - 1], again_val = a;
	vals[nvals++] = a;
	while (nops) {
		if (ops[nops - 1] < 0) {
			nops--;
			say(") ");
		} else if (apply_top())
			return;
	}
	result = vals[0];
	nvals = nparens = 0;
	say("=");
	history_add(expr, result);
	expr[0] = operand[0] = 0;
	value = result;
	shown = after_op = 0;
}

/* An angle in the unit chosen, to radians. */
static double
radians(double x)
{
	return angle == DEG ? x * M_PI / 180 : angle == GRAD ? x * M_PI / 200 : x;
}

static double
from_radians(double x)
{
	return angle == DEG ? x * 180 / M_PI : angle == GRAD ? x * 200 / M_PI : x;
}

/* sin, or (cosine) cos, exact where degrees or grads make it so: sin 30 is 0.5, cos 90 is 0. */
static double
sine(double x, int cosine)
{
	double whole = angle == DEG ? 360 : angle == GRAD ? 400 : 0, r, k;
	static const double sixth[12] = { 0, 0.5, 0, 1, 0, 0.5, 0, -0.5, 0, -1, 0, -0.5 };

	if (whole) {
		r = fmod(x + (cosine ? whole / 4 : 0), whole);
		r = r < 0 ? r + whole : r;
		r = r >= whole ? 0 : r;	/* (a tiny negative r plus whole rounds to whole) */
		k = r / (whole / 12);
		if (k == floor(k) && sixth[(int)k] != 0)
			return sixth[(int)k];
		k = r / (whole / 4);
		if (k == floor(k))
			return ((int)k & 1) ? ((int)k == 1 ? 1 : -1) : 0;
	}
	r = cosine ? cos(radians(x)) : sin(radians(x));
	return fabs(r) < 1e-15 ? 0 : r;
}

static double
factorial(double x)
{
	double r = 1;
	int i;

	if (x < 0 && x == floor(x))
		return NAN;
	if (x != floor(x))
		return tgamma(x + 1);
	if (x > 170)
		return INFINITY;
	for (i = 2; i <= (int)x; i++)
		r *= i;
	return r;
}

/* A function of the operand, worked out at once: sqrt(9) shown in the expression's line. */
static void
unary(int id)
{
	struct num a = current(), r;
	char was[300], wrapped[300];
	const char *name = "";
	double x = a.d, y = 0, s, c;
	int i = inv;

	typing = 0;
	if (shown) {
		snprintf(was, sizeof was, "%s", expr + shown_at);
		if (was[0] && was[strlen(was) - 1] == ' ')
			was[strlen(was) - 1] = 0;
		expr[shown_at] = 0;
		shown = 0;
	} else
		operand_text(was, sizeof was, a);
	after_op = 0;
	if (mode == PROGRAMMER) {
		uint64_t u = a.u & mask(), top = (uint64_t)1 << (word - 1);

		switch (id) {
		case K_NOT: r = from_u(~u), name = "not"; break;
		case K_NEG: r = from_u((uint64_t)0 - u), name = "negate"; break;
		case K_ROL: r = from_u(u << 1 | ((u & top) ? 1 : 0)), name = "RoL"; break;
		case K_ROR: r = from_u(u >> 1 | ((u & 1) ? top : 0)), name = "RoR"; break;
		default: return;
		}
		value = r;
		snprintf(operand, sizeof operand, "%s(%s)", name, was);
		return;
	}
	if (id >= K_INV && i) {
		inv = 0;
		dirty |= D_KEYS;
	}
	switch (id) {
	case K_SQRT: y = x < 0 ? NAN : sqrt(x), name = "sqrt"; break;
	case K_SQR: y = x * x, name = "sqr"; break;
	case K_CUBE: y = x * x * x, name = "cube"; break;
	case K_CBRT: y = cbrt(x), name = "cuberoot"; break;
	case K_RECIP:
		if (x == 0) {
			fail("Cannot divide by zero");
			return;
		}
		y = 1 / x, name = "reciproc";
		break;
	case K_NEG: y = -x, name = "negate"; break;
	case K_PCT: y = nvals ? vals[nvals - 1].d * x / 100 : 0; break;
	case K_FACT: y = factorial(x), name = "fact"; break;
	case K_SIN: y = i ? (fabs(x) > 1 ? NAN : from_radians(asin(x))) : sine(x, 0), name = i ? "asin" : "sin"; break;
	case K_COS: y = i ? (fabs(x) > 1 ? NAN : from_radians(acos(x))) : sine(x, 1), name = i ? "acos" : "cos"; break;
	case K_TAN:
		name = i ? "atan" : "tan";
		if (i) {
			y = from_radians(atan(x));
			break;
		}
		s = sine(x, 0), c = sine(x, 1);
		y = c == 0 ? NAN : s / c;
		break;
	case K_SINH: y = i ? asinh(x) : sinh(x), name = i ? "asinh" : "sinh"; break;
	case K_COSH: y = i ? (x < 1 ? NAN : acosh(x)) : cosh(x), name = i ? "acosh" : "cosh"; break;
	case K_TANH: y = i ? (fabs(x) >= 1 ? NAN : atanh(x)) : tanh(x), name = i ? "atanh" : "tanh"; break;
	case K_LN: y = i ? exp(x) : x <= 0 ? NAN : log(x), name = i ? "powe" : "ln"; break;
	case K_LOG: y = x <= 0 ? NAN : log10(x), name = "log"; break;
	case K_TEN: y = pow(10, x), name = "powten"; break;
	case K_INT: y = i ? x - trunc(x) : trunc(x), name = i ? "frac" : "int"; break;
	case K_DMS: {
		/* degrees as degrees, minutes and seconds (d.mmss), or (Inv) back */
		double d = trunc(x), f = fabs(x - d), m, sec;

		if (i) {
			m = trunc(f * 100 + 1e-9);
			sec = (f * 100 - m) * 100;
			y = d + (x < 0 ? -1 : 1) * (m / 60 + sec / 3600), name = "degrees";
		} else {
			m = trunc(f * 60 + 1e-9);
			sec = (f * 60 - m) * 60;
			y = d + (x < 0 ? -1 : 1) * (m / 100 + sec / 10000), name = "dms";
		}
		break;
	}
	default: return;
	}
	if (failed(y))
		return;
	r = from_d(y);
	value = r;
	if (id == K_PCT)
		format_num(r, operand, sizeof operand, grouping);
	else {
		snprintf(wrapped, sizeof wrapped, "%s(%s)", name, was);
		snprintf(operand, sizeof operand, "%s", wrapped);
	}
}

/* ---- keys ---- */

static int
pane_shown(int pane)
{
	return pane == PANE_STD || (pane == PANE_SCI && mode == SCIENTIFIC) || (pane == PANE_PROG && mode == PROGRAMMER);
}

/* Whether a key does anything now: the others are drawn grey. */
static int
works(int id)
{
	if (error[0])
		return IS_DIGIT(id) ? id < (mode == PROGRAMMER ? base : 10) : id == K_C || id == K_CE || id == K_BACK || id == K_DOT;
	if (IS_DIGIT(id))
		return id < (mode == PROGRAMMER ? base : 10);
	if (mode == PROGRAMMER && (id == K_DOT || id == K_SQRT || id == K_PCT || id == K_RECIP))
		return 0;
	if (id == K_MC || id == K_MR)
		return has_memory;
	return 1;
}

/* The key of an id as this mode shows it, an index of keys[], or -1. */
static int
key_index(int id)
{
	int k;

	for (k = 0; k < KEY_COUNT; k++)
		if (keys[k].id == id && pane_shown(keys[k].pane))
			return k;
	return -1;
}

/* What a key does to the calculator. */
static void
press(int id)
{
	struct num a;

	if (key_index(id) < 0 || !works(id))
		return;
	dirty |= D_DISPLAY;
	if (error[0]) {
		clear_all();
		if (!IS_DIGIT(id) && id != K_DOT)
			return;
	}
	if (IS_DIGIT(id) || id == K_DOT) {
		type_digit(id);
		return;
	}
	switch (id) {
	case K_ADD: case K_SUB: case K_MUL: case K_DIV: case K_MOD: case K_POW: case K_ROOT:
	case K_AND: case K_OR: case K_XOR: case K_LSH: case K_RSH:
		binary(id);
		break;
	case K_EQ: equals(); break;
	case K_LP: open_bracket(); break;
	case K_RP: close_bracket(); break;
	case K_BACK: backspace(); break;
	case K_CE:
		typing = 0;
		new_operand();
		memset(&value, 0, sizeof value);
		break;
	case K_C: clear_all(); break;
	case K_MC:
		has_memory = 0;
		memset(&memory, 0, sizeof memory);
		dirty |= D_KEYS;
		break;
	case K_MR:
		typing = 0;
		new_operand();
		value = memory;
		break;
	case K_MS: case K_MPLUS: case K_MMINUS:
		commit();
		a = value;
		if (id == K_MS)
			memory = a;
		else if (mode == PROGRAMMER)
			memory = from_u(id == K_MPLUS ? memory.u + a.u : memory.u - a.u);
		else
			memory = from_d(id == K_MPLUS ? memory.d + a.d : memory.d - a.d);
		if (!has_memory)
			dirty |= D_KEYS;
		has_memory = 1;
		break;
	case K_INV:
		inv = !inv;
		dirty |= D_KEYS;
		break;
	case K_FE: fe = !fe; break;
	case K_EXP:
		if (!typing) {
			char t[100];

			format_num(current(), t, sizeof t, 0);
			new_operand();
			snprintf(entry, sizeof entry, "%.90s", strchr(t, 'e') ? "0" : t);
			typing = 1;
		}
		if (!strchr(entry, 'e'))
			strcat(entry, "e+");
		break;
	case K_NEG:
		if (typing && mode != PROGRAMMER) {
			char *e = strchr(entry, 'e');

			if (e)
				e[1] = e[1] == '-' ? '+' : '-';
			else if (entry[0] == '-')
				memmove(entry, entry + 1, strlen(entry));
			else if (strcmp(entry, "0") && strlen(entry) < sizeof entry - 2) {
				memmove(entry + 1, entry, strlen(entry) + 1);
				entry[0] = '-';
			}
			break;
		}
		unary(id);
		break;
	case K_PI:
		typing = 0;
		new_operand();
		value = from_d(inv ? 2 * M_PI : M_PI);
		if (inv)
			dirty |= D_KEYS;
		inv = 0;
		break;
	default:
		unary(id);
	}
}

/* The key (or, in *command, the command) a key of the keyboard is; -1 for none. */
static int
key_of(int ch, int modifiers, int *command)
{
	int ctrl = (modifiers & MWKMOD_CTRL) != 0, alt = (modifiers & MWKMOD_ALT) != 0, letter = 0;

	*command = 0;
	if (ctrl && ch < 128 && isalpha(ch))
		letter = tolower(ch);
	else if (ch >= 1 && ch <= 26 && (ctrl || (ch != MWKEY_BACKSPACE && ch != MWKEY_TAB && ch != '\n' && ch != MWKEY_ENTER)))
		letter = 'a' + ch - 1;	/* (Ctrl+H comes as 8, Ctrl+M as 13) */
	if (letter) {
		switch (letter) {
		case 'c': *command = C_COPY; return -1;
		case 'v': *command = C_PASTE; return -1;
		case 'h': *command = C_HISTORY; return -1;
		case 'l': return K_MC;
		case 'r': return K_MR;
		case 'm': return K_MS;
		case 'p': return K_MPLUS;
		case 'q': return K_MMINUS;
		case 'g': return K_TEN;
		case 'y': return K_ROOT;
		case 'b': return K_CBRT;
		}
		return -1;
	}
	if (alt) {
		*command = ch == '1' ? C_STANDARD : ch == '2' ? C_SCIENTIFIC : ch == '3' ? C_PROGRAMMER : 0;
		return -1;
	}
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	if (ch >= MWKEY_KP0 && ch <= MWKEY_KP9)
		return ch - MWKEY_KP0;
	if (mode == PROGRAMMER && ch < 128 && isxdigit(ch))
		return 10 + tolower(ch) - 'a';
	switch (ch) {
	case '.': case ',': case MWKEY_KP_PERIOD: return K_DOT;
	case '+': case MWKEY_KP_PLUS: return K_ADD;
	case '-': case MWKEY_KP_MINUS: return K_SUB;
	case '*': case MWKEY_KP_MULTIPLY: return K_MUL;
	case '/': case MWKEY_KP_DIVIDE: return K_DIV;
	case '=': case MWKEY_ENTER: case '\n': case MWKEY_KP_ENTER: case MWKEY_KP_EQUALS: return K_EQ;
	case MWKEY_ESCAPE: return K_C;
	case MWKEY_DELETE: return K_CE;
	case MWKEY_BACKSPACE: case 127: return K_BACK;
	case MWKEY_F9: return K_NEG;
	case '@': return K_SQRT;
	case '%': return mode == PROGRAMMER ? K_MOD : K_PCT;
	case '(': return K_LP;
	case ')': return K_RP;
	case MWKEY_F1: *command = C_ABOUT; return -1;
	case MWKEY_F10: *command = C_MENU; return -1;
	}
	if (mode == SCIENTIFIC)
		switch (ch) {
		case 'r': case 'R': return K_RECIP;
		case 's': return K_SIN;
		case 'o': return K_COS;
		case 't': return K_TAN;
		case 'i': return K_INV;
		case 'n': return K_LN;
		case 'l': return K_LOG;
		case 'q': return K_SQR;
		case 'y': case '^': return K_POW;
		case '#': return K_CUBE;
		case '!': return K_FACT;
		case 'p': return K_PI;
		case 'x': case 'e': return K_EXP;
		case 'v': return K_FE;
		case 'm': return K_DMS;
		case ';': return K_INT;
		case MWKEY_F3: *command = C_DEG; return -1;
		case MWKEY_F4: *command = C_RAD; return -1;
		case MWKEY_F5: *command = C_GRAD; return -1;
		}
	else if (mode == PROGRAMMER)
		switch (ch) {
		case '&': return K_AND;
		case '|': return K_OR;
		case '^': return K_XOR;
		case '~': return K_NOT;
		case '<': return K_LSH;
		case '>': return K_RSH;
		case MWKEY_F5: *command = C_HEX; return -1;
		case MWKEY_F6: *command = C_DEC; return -1;
		case MWKEY_F7: *command = C_OCT; return -1;
		case MWKEY_F8: *command = C_BIN; return -1;
		case MWKEY_F12: *command = C_QWORD; return -1;
		case MWKEY_F2: *command = C_DWORD; return -1;
		case MWKEY_F3: *command = C_WORD; return -1;
		case MWKEY_F4: *command = C_BYTE; return -1;
		}
	else if (ch == 'r' || ch == 'R')
		return K_RECIP;
	return -1;
}

/* ---- drawing ---- */

static void
fonts(void)
{
	GR_FONT_INFO info;

	if (big)
		GrDestroyFont(big);
	if (small && small_own)	/* (ui_var may have been made anew under it: never destroy that) */
		GrDestroyFont(small);
	big = access("/usr/share/font-ui-17.sef", R_OK) == 0 ? GrCreateFontEx("/usr/share/font-ui-17.sef", 0, 0, NULL) :
		GrCreateFontEx(GR_FONT_SYSTEM_VAR, 0, 0, NULL);
	small = ui_smooth ? GrCreateFontEx("/usr/share/font-ui-11.sef", 0, 0, NULL) : ui_var;
	small_own = small != ui_var;
	big_widths.known = small_widths.known = 0;
	GrGetFontInfo(big, &info);
	big_h = info.height;
	GrGetFontInfo(small, &info);
	small_h = info.height;
	GrGetFontInfo(ui_var, &info);
	var_h = info.height;
}

static void
text_in(GR_FONT_ID font, int x, int y, const char *text, int count, GR_COLOR colour)
{
	GrSetGCFont(ui_gc, font);
	GrSetGCForeground(ui_gc, colour);
	GrText(window, ui_gc, x, y, (void *)text, count < 0 ? (int)strlen(text) : count, GR_TFASCII | GR_TFTOP);
}

static void
line(int x0, int y0, int x1, int y1)
{
	GrLine(window, ui_gc, x0, y0, x1, y1);
}

/* A label's signs, drawn of lines: the root, the arrow, plus-minus, pi; and their widths. */
static const int sign_w[5] = { 0, 10, 13, 9, 9 };

static void
sign(int which, int x, int y, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	switch (which) {
	case 1:
		line(x, y + 7, x + 2, y + 7);
		line(x + 2, y + 7, x + 4, y + 12);
		line(x + 4, y + 12, x + 7, y + 1);
		line(x + 7, y + 1, x + 10, y + 1);
		break;
	case 2:
		line(x + 1, y + 7, x + 11, y + 7);
		line(x + 1, y + 7, x + 5, y + 3);
		line(x + 1, y + 7, x + 5, y + 11);
		line(x + 2, y + 7, x + 5, y + 4);
		line(x + 2, y + 7, x + 5, y + 10);
		break;
	case 3:
		line(x + 1, y + 6, x + 7, y + 6);
		line(x + 4, y + 3, x + 4, y + 9);
		line(x + 1, y + 11, x + 7, y + 11);
		break;
	case 4:
		line(x, y + 4, x + 8, y + 4);
		line(x + 2, y + 4, x + 1, y + 11);
		line(x + 6, y + 4, x + 6, y + 10);
		line(x + 6, y + 11, x + 7, y + 11);
		break;
	}
}

/* Draws a label at x, y (with draw false only measures it); its width. */
static int
label(int x, int y, const char *s, GR_COLOR colour, int draw)
{
	int at = x, n;

	while (*s) {
		if (*s == '^' && s[1]) {
			if (draw)
				text_in(small, at, y - (small == ui_var ? 4 : 2), s + 1, 1, colour);
			at += ui_width(small == ui_var ? &ui_var_widths : &small_widths, small, s + 1, 1);
			s += 2;
		} else if (*s >= 1 && *s <= 4) {
			if (draw)
				sign(*s, at, y, colour);
			at += sign_w[(int)*s];
			s++;
		} else {
			for (n = 0; s[n] && s[n] != '^' && (s[n] < 1 || s[n] > 4); n++)
				;
			if (draw)
				ui_text(window, at, y, s, n, colour, 0);
			at += ui_width(&ui_var_widths, ui_var, s, n);
			s += n;
		}
	}
	return at - x;
}

static void
key_rect(const struct key *k, int *x, int *y, int *w, int *h)
{
	int x0 = k->pane == PANE_SCI ? sci_x : k->pane == PANE_PROG ? prog_x : grid_x;

	*x = x0 + k->col * (KW + GAP);
	*y = grid_y + k->row * (KH + GAP);
	*w = k->cols * KW + (k->cols - 1) * GAP;
	*h = k->rows * KH + (k->rows - 1) * GAP;
}

/* The colour of a key's label in the classic look: Windows 95's, blue digits and red operators. */
static GR_COLOR
classic_colour(int id)
{
	if (IS_DIGIT(id) || id == K_DOT || id == K_NEG || id == K_SQRT || id == K_PCT || id == K_RECIP)
		return MWRGB(0, 0, 255);
	if ((id >= K_ADD && id <= K_C) || (id >= K_MC && id <= K_MMINUS) || id == K_MOD || id == K_POW || id == K_ROOT ||
	    (id >= K_OR && id <= K_AND))
		return MWRGB(192, 0, 0);
	return MWRGB(128, 0, 128);
}

static void
draw_key(int index, int pressed)
{
	const struct key *k = &keys[index];
	const char *text = inv && k->inverse ? k->inverse : k->label;
	int x, y, w, h, lw, enabled = works(k->id), tx, ty;

	if (!window)
		return;
	key_rect(k, &x, &y, &w, &h);
	if (ui_seven)
		ui_button_seven(window, x, y, w, h, pressed);
	else {
		ui_fill(window, x, y, w, h, UI_FACE);
		if (pressed) {
			ui_edges(window, x, y, w, h, BLACK, BLACK);
			ui_edges(window, x + 1, y + 1, w - 2, h - 2, UI_SHADOW, UI_FACE);
		} else {
			ui_edges(window, x, y, w, h, WHITE, BLACK);
			ui_edges(window, x + 1, y + 1, w - 2, h - 2, UI_FACE, UI_SHADOW);
		}
	}
	if (k->id == K_INV && inv && ui_seven)
		ui_button_seven(window, x, y, w, h, 1);
	lw = label(0, 0, text, 0, 0);
	tx = x + (w - lw) / 2 + (pressed && !ui_seven), ty = y + (h - var_h) / 2 + (pressed && !ui_seven);
	if (!enabled && !ui_seven)
		label(tx + 1, ty + 1, text, WHITE, 1);
	label(tx, ty, text, !enabled ? (ui_seven ? MWRGB(160, 160, 160) : UI_SHADOW) : ui_seven ? BLACK :
	      classic_colour(k->id), 1);
	if (k->id == K_INV && inv && !ui_seven)
		ui_edges(window, x + 3, y + 3, w - 6, h - 6, BLACK, BLACK);
}

static void
draw_keys(void)
{
	int k;

	for (k = 0; k < KEY_COUNT; k++)
		if (pane_shown(keys[k].pane))
			draw_key(k, k == lit || k == held);
}

/* A field: Windows 7's pale gradient in a soft edge, or Windows 95's white well. */
static void
field(int x, int y, int w, int h)
{
	int i;

	if (ui_seven) {
		for (i = 0; i < 8; i++)
			ui_fill(window, x + 1, y + 1 + i * (h - 2) / 8, w - 2, (i + 1) * (h - 2) / 8 - i * (h - 2) / 8,
				MWRGB(234 + i * 21 / 8, 242 + i * 13 / 8, 251 + i * 4 / 8));
		GrSetGCForeground(ui_gc, MWRGB(142, 156, 173));
		line(x + 1, y, x + w - 2, y);
		line(x + 1, y + h - 1, x + w - 2, y + h - 1);
		line(x, y + 1, x, y + h - 2);
		line(x + w - 1, y + 1, x + w - 1, y + h - 2);
	} else {
		ui_fill(window, x, y, w, h, WHITE);
		ui_bevel(window, x, y, w, h, 1);
		ui_edges(window, x + 1, y + 1, w - 2, h - 2, MWRGB(64, 64, 64), UI_FACE);
	}
}

/* Text right-aligned to `right` in the first of the fonts it fits in, its left cut when none. */
static void
fitted(const char *text, int right, int y, int room, GR_COLOR colour, int bottom)
{
	GR_FONT_ID font[3] = { big, ui_var, small };
	struct ui_widths *widths[3] = { &big_widths, &ui_var_widths, small == ui_var ? &ui_var_widths : &small_widths };
	int height[3] = { big_h, var_h, small_h }, f, w = 0, n = (int)strlen(text);

	for (f = 0; f < 3; f++)
		if ((w = ui_width(widths[f], font[f], text, n)) <= room)
			break;
	if (f == 3) {
		f = 2;
		while (n > 0 && (w = ui_width(widths[f], font[f], text, n)) > room)
			text++, n--;
	}
	text_in(font[f], right - w, bottom - height[f], text, n, colour);
	(void)y;
}

static void
draw_display(void)
{
	char t[200], count[16];
	int x = MARGIN, w = width - 2 * MARGIN, y = display_y, room = w - 40, n, ew;
	char line_text[sizeof expr + sizeof operand];
	const char *e = line_text;
	GR_COLOR grey = ui_seven ? MWRGB(80, 80, 80) : BLACK;

	if (!window)
		return;
	/* the expression, and the operand when functions made it ("sqrt(9)") */
	snprintf(line_text, sizeof line_text, "%s%s", expr, shown || typing ? "" : operand);
	field(x, y, w, DISPLAY_H);
	/* the expression's line, its start cut with dots when it does not fit */
	n = (int)strlen(e);
	ew = ui_width(&small_widths, small, e, n);
	if (small == ui_var)
		ew = ui_width(&ui_var_widths, ui_var, e, n);
	if (nparens)	/* (room for the brackets' count at the left) */
		room -= 30;
	if (ew > room)	/* (and for the dots, when cut) */
		room -= 14;
	while (n > 0 && ew > room) {
		e++, n--;
		ew = ui_width(small == ui_var ? &ui_var_widths : &small_widths, small, e, n);
	}
	if (e != line_text)
		text_in(small, x + w - 6 - ew - 12, y + 5, "...", 3, grey);
	text_in(small, x + w - 6 - ew, y + 5, e, n, grey);
	if (nparens) {
		snprintf(count, sizeof count, "(=%d", nparens);
		text_in(small, x + 6, y + 5, count, -1, grey);
	}
	if (has_memory)
		text_in(small, x + 6, y + DISPLAY_H - 6 - small_h, "M", 1, grey);
	display_text(t, sizeof t, grouping);
	fitted(t, x + w - 7, y, w - 30, BLACK, y + DISPLAY_H - 4);
}

/* The bits of the word, 64 in two rows of nibbles, those above the word grey. */
static void
bits_geometry(int *nibble, int *digit)
{
	*digit = ui_fw;
	*nibble = (width - 2 * MARGIN) / 8;
	if (*nibble < 4 * *digit + 2)
		*digit = (*nibble - 2) / 4;
}

static void
draw_bits(void)
{
	int nibble, digit, row, n, b, x;
	uint64_t u = current().u & mask();
	char t[4];

	if (!window || mode != PROGRAMMER)
		return;
	bits_geometry(&nibble, &digit);
	ui_fill(window, MARGIN, bits_y, width - 2 * MARGIN, BITS_H, GROUND);
	for (row = 0; row < 2; row++) {
		int y = bits_y + row * (BITS_H / 2);

		for (n = 0; n < 8; n++) {
			int high = (row == 0 ? 63 : 31) - n * 4;
			char four[5];

			for (b = 0; b < 4; b++)
				four[b] = (u >> (high - b) & 1) ? '1' : '0';
			four[4] = 0;
			x = MARGIN + n * nibble;
			if (high < word)
				ui_text(window, x, y, four, 4, BLACK, 1);
			else
				ui_text(window, x, y, four, 4, ui_seven ? MWRGB(170, 170, 170) : UI_SHADOW, 1);
			if (n == 0 || n == 4 || n == 7) {
				int at = n == 7 ? high - 3 : high;

				snprintf(t, sizeof t, "%d", at);
				text_in(small, x + (n == 7 ? 3 * digit : 0), y + ui_fh, t, -1, ui_seven ? MWRGB(90, 90, 90) : BLACK);
			}
		}
	}
}

/* Which bit a point of the bits is on, or -1. */
static int
bit_at(int px, int py)
{
	int nibble, digit, row, n, d;

	bits_geometry(&nibble, &digit);
	if (mode != PROGRAMMER || py < bits_y || py >= bits_y + BITS_H || px < MARGIN || px >= MARGIN + 8 * nibble)
		return -1;
	row = (py - bits_y) / (BITS_H / 2);
	n = (px - MARGIN) / nibble;
	d = (px - MARGIN - n * nibble) / digit;
	if (row > 1 || d > 3 || (py - bits_y) % (BITS_H / 2) > ui_fh)
		return -1;
	return (row == 0 ? 63 : 31) - n * 4 - d;
}

static void
group_box(int x, int y, int w, int h)
{
	if (ui_seven)
		ui_edges(window, x, y, w, h, MWRGB(180, 195, 212), MWRGB(180, 195, 212));
	else {
		ui_edges(window, x, y, w, h, UI_SHADOW, WHITE);
		ui_edges(window, x + 1, y + 1, w - 2, h - 2, WHITE, UI_SHADOW);
	}
}

static void
radio(int x, int y, const char *text, int on)
{
	int cx = x + 7, cy = y + RADIO_ROW / 2;

	GrSetGCForeground(ui_gc, WHITE);
	GrFillEllipse(window, ui_gc, cx, cy, 5, 5);
	GrSetGCForeground(ui_gc, ui_seven ? MWRGB(112, 112, 112) : UI_SHADOW);
	GrEllipse(window, ui_gc, cx, cy, 5, 5);
	if (on) {
		GrSetGCForeground(ui_gc, ui_seven ? MWRGB(40, 110, 200) : BLACK);
		GrFillEllipse(window, ui_gc, cx, cy, 2, 2);
	}
	ui_text(window, x + 16, y + (RADIO_ROW - var_h) / 2 + 1, text, -1, BLACK, 0);
}

/* The rows of a group of choices, each a command; `across` lays them side by side. */
static void
radio_group(int x, int y, int w, int h, const char *const *names, const int *commands, int count, int chosen, int across)
{
	int i, used = 0, space, rx = x + 4;

	ui_fill(window, x, y, w, h, GROUND);
	group_box(x, y, w, h);
	/* side by side: each as wide as its name, the room left shared between them */
	for (i = 0; i < count; i++)
		used += 18 + ui_width(&ui_var_widths, ui_var, names[i], -1);
	space = count > 1 && w - 8 > used ? (w - 8 - used) / (count - 1) : 0;
	for (i = 0; i < count && radio_count < 12; i++) {
		int rw = across ? 18 + ui_width(&ui_var_widths, ui_var, names[i], -1) : w - 12;
		int ry = across ? y + (h - RADIO_ROW) / 2 : y + 4 + i * RADIO_ROW;

		if (!across)
			rx = x + 6;
		radio(rx, ry, names[i], i == chosen);
		radios[radio_count].x = rx, radios[radio_count].y = ry;
		radios[radio_count].w = rw, radios[radio_count].h = RADIO_ROW;
		rx += rw + space;
		radios[radio_count++].command = commands[i];
	}
}

static void
draw_radios(void)
{
	static const char *const angles[] = { "Degrees", "Radians", "Grads" };
	static const char *const short_angles[] = { "Deg", "Rad", "Grad" };
	static const int angle_c[] = { C_DEG, C_RAD, C_GRAD };
	static const char *const bases[] = { "Hex", "Dec", "Oct", "Bin" };
	static const int base_c[] = { C_HEX, C_DEC, C_OCT, C_BIN };
	static const char *const words[] = { "Qword", "Dword", "Word", "Byte" };
	static const int word_c[] = { C_QWORD, C_DWORD, C_WORD, C_BYTE };
	int h = 4 * RADIO_ROW + 8, i, used = 0;

	radio_count = 0;
	if (!window)
		return;
	if (mode == SCIENTIFIC) {
		/* the names in full only where they fit side by side with room between them */
		for (i = 0; i < 3; i++)
			used += 18 + 8 + ui_width(&ui_var_widths, ui_var, angles[i], -1);
		radio_group(sci_x, grid_y, 5 * KW + 4 * GAP, KH, used <= 5 * KW + 4 * GAP ? angles : short_angles,
			    angle_c, 3, angle, 1);
	}
	if (mode == PROGRAMMER) {
		radio_group(MARGIN, grid_y, RADIO_W, h, bases, base_c, 4, base == 16 ? 0 : base == 10 ? 1 : base == 8 ? 2 : 3, 0);
		radio_group(MARGIN, grid_y + h + 8, RADIO_W, h, words, word_c, 4, word == 64 ? 0 : word == 32 ? 1 : word == 16 ? 2 : 3, 0);
	}
}

static const char *
history_label(int row)
{
	return history[row].text;
}

static void
draw_history(void)
{
	if (!window || !show_history)
		return;
	if (history_list.count == 0) {
		ui_list_draw(window, &history_list, history_label);
		ui_text(window, history_list.x + 8, history_list.y + 6, "There is no history yet.", -1, UI_SHADOW, 0);
		return;
	}
	ui_list_draw(window, &history_list, history_label);
}

static void
draw_menu_bar(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 3; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

static void
draw_all(void)
{
	if (!window)
		return;
	ui_fill(window, 0, MENU_H, width, height - MENU_H, GROUND);
	draw_menu_bar();
	draw_history();
	draw_display();
	draw_bits();
	draw_radios();
	draw_keys();
}

static void
flush(void)
{
	if (!window || !dirty)
		return;
	if (dirty & D_ALL)
		draw_all();
	else {
		if (dirty & D_KEYS)
			draw_keys();
		if (dirty & D_DISPLAY)
			draw_display();
		if (dirty & D_DISPLAY)
			draw_bits();
		if (dirty & D_HISTORY)
			draw_history();
	}
	dirty = 0;
}

/* The places of everything for the mode, and the window's size. */
static void
layout(void)
{
	int y = MENU_H + 8, i, x;

	if (!ui_gc)	/* (the test without a window) */
		return;
	for (i = 0, x = 2; i < 3; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[3] = x;
	sci_x = prog_x = MARGIN;
	if (mode == SCIENTIFIC)
		grid_x = sci_x + 5 * (KW + GAP) + 5;
	else if (mode == PROGRAMMER) {
		prog_x = MARGIN + RADIO_W + 8;
		grid_x = prog_x + 3 * (KW + GAP);
	} else
		grid_x = MARGIN;
	width = grid_x + 5 * KW + 4 * GAP + MARGIN;
	history_list.x = MARGIN, history_list.width = width - 2 * MARGIN;
	if (show_history) {
		history_list.y = y;
		y += HISTORY_H + 6;
	}
	display_y = y;
	y += DISPLAY_H + 8;
	if (mode == PROGRAMMER) {
		bits_y = y;
		y += BITS_H + 6;
	}
	grid_y = y;
	height = grid_y + 6 * KH + 5 * GAP + MARGIN;
	if (window)
		GrResizeWindow(window, width, height);
	dirty |= D_ALL;
}

/* ---- commands ---- */

static void
set_mode(int to)
{
	struct num a = current();

	if (to == mode)
		return;
	if (to == PROGRAMMER) {
		value = from_u(from_d(a.d).u);
		memory = from_u(from_d(memory.d).u);
	} else if (mode == PROGRAMMER) {
		value = from_d((double)as_signed(a.u));
		memory = from_d((double)as_signed(memory.u));
	} else
		value = a;
	nvals = nops = nparens = 0;
	expr[0] = operand[0] = error[0] = 0;
	typing = shown = after_op = again_op = inv = 0;
	mode = to;
	remember("nxcalc.mode", mode);
	layout();
}

static void
set_base(int to)
{
	commit();
	new_operand();
	base = to;
	remember("nxcalc.base", base);
	dirty |= D_ALL;
}

static void
set_word(int to)
{
	int i;

	commit();
	word = to;
	value = from_u(value.u);
	memory = from_u(memory.u);
	for (i = 0; i < nvals; i++)
		vals[i] = from_u(vals[i].u);
	remember("nxcalc.word", word);
	dirty |= D_ALL;
}

static void
type_char(int ch, int modifiers);

/* The clipboard's text typed, as Windows' calculator takes it: "12*(3+4)=" works it out. */
static void
paste(void)
{
	int n, i;
	char *text = ui_clip_get(&n);

	if (!text)
		return;
	if (mode != PROGRAMMER && n < 60) {
		/* one number, as Copy gives it: its signs and its exponent are not operators */
		char *end;
		const char *s = text;

		strtod(s, &end);
		while (end > s && isspace((unsigned char)end[0]))
			end++;
		if (end > s && !*end && n < (int)sizeof entry - 1 && (isdigit((unsigned char)s[s[0] == '-']) || s[s[0] == '-'] == '.') &&
		    strspn(s, "0123456789.eE+- \n") == (size_t)n) {
			/* (the entry itself: Exp is no key of the standard pane, and a key that is not there is not pressed) */
			int at = 0;

			if (error[0])
				clear_all();
			if (!typing) {
				new_operand();
				typing = 1;
			}
			for (; s < end; s++)
				entry[at++] = *s == 'E' ? 'e' : *s;
			entry[at] = 0;
			dirty |= D_DISPLAY;
			free(text);
			return;
		}
	}
	for (i = 0; i < n && i < 400; i++)
		if (text[i] > ' ' && text[i] < 127 && !(text[i] == ',' && mode != PROGRAMMER))	/* (a control byte is a Ctrl key: Ctrl+V pastes again) */
			type_char((unsigned char)text[i], 0);
		else if (text[i] == '\n' && i + 1 < n)
			type_char('=', 0);
	free(text);
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
}

static void
open_menu(int i, int keyboard)
{
	int wx, wy, k;

	for (k = 0; k < 3; k++)
		view_items[k].flags = (k == mode ? UI_MENU_CHECK : 0);
	view_items[3].flags = UI_MENU_LINE | (show_history ? UI_MENU_CHECK : 0);
	view_items[4].flags = grouping ? UI_MENU_CHECK : 0;
	edit_items[2].flags = UI_MENU_LINE | (history_list.count ? 0 : UI_MENU_GREY);
	edit_items[3].flags = history_list.count ? 0 : UI_MENU_GREY;
	ui_window_origin(window, &wx, &wy);
	if (i == 0)
		ui_menu_open(view_items, 5, wx + menu_x[0], wy + MENU_H, 0, keyboard);
	else if (i == 1)
		ui_menu_open(edit_items, 4, wx + menu_x[1], wy + MENU_H, 0, keyboard);
	else
		ui_menu_open(help_items, 1, wx + menu_x[2], wy + MENU_H, 0, keyboard);
}

static void
command(int id)
{
	static const char *const ok[] = { "OK" };
	char t[200];
	int i, n;

	switch (id) {
	case C_STANDARD: case C_SCIENTIFIC: case C_PROGRAMMER:
		set_mode(id - C_STANDARD);
		break;
	case C_HISTORY:
		show_history = !show_history;
		remember("nxcalc.history", show_history);
		layout();
		break;
	case C_GROUPING:
		grouping = !grouping;
		remember("nxcalc.grouping", grouping);
		dirty |= D_DISPLAY;
		break;
	case C_COPY:
		display_text(t, sizeof t, 0);
		ui_clip_set(t, (int)strlen(t));
		break;
	case C_PASTE:
		paste();
		break;
	case C_COPY_HISTORY: {
		char *all = malloc(HISTORY_MOST * 162);

		for (i = n = 0; all && i < history_list.count; i++)
			n += snprintf(all + n, HISTORY_MOST * 162 - n, "%s\n", history[i].text);
		if (all)
			ui_clip_set(all, n);
		free(all);
		break;
	}
	case C_CLEAR_HISTORY:
		history_list.count = history_list.top = 0;
		history_list.selected = -1;
		dirty |= D_HISTORY;
		break;
	case C_ABOUT:
		ui_ask_icon = "information";
		ui_ask("About Calculator", "Calculator\nStandard, Scientific and Programmer, as Windows 7's.\n"
		       "Numbers are doubles: 15 significant digits.", ok, 1, others);
		ui_ask_icon = NULL;
		GrSetFocus(window);
		break;
	case C_DEG: case C_RAD: case C_GRAD:
		angle = id - C_DEG;
		remember("nxcalc.angle", angle);
		dirty |= D_ALL;
		break;
	case C_HEX: set_base(16); break;
	case C_DEC: set_base(10); break;
	case C_OCT: set_base(8); break;
	case C_BIN: set_base(2); break;
	case C_QWORD: set_word(64); break;
	case C_DWORD: set_word(32); break;
	case C_WORD: set_word(16); break;
	case C_BYTE: set_word(8); break;
	case C_MENU:
		if (window)
			open_menu(0, 1);
		break;
	}
}

/* A key of the keyboard: the calculator's key it stands for, shown pressed until it is let go. */
static void
type_char(int ch, int modifiers)
{
	int c, id = key_of(ch, modifiers, &c);

	if (c)
		command(c);
	else if (id >= 0) {
		press(id);
		if (window && (c = key_index(id)) >= 0 && works(id)) {
			flush();
			if (lit >= 0 && lit != c)
				draw_key(lit, 0);
			lit = c;
			draw_key(lit, 1);
		}
	}
}

/* nxcalc -e: each argument typed; {name} is a key or a command by name. Prints the display. */
static int
test(int argc, char **argv)
{
	static const struct { const char *name; int id; } named[] = {
		{ "std", -C_STANDARD }, { "sci", -C_SCIENTIFIC }, { "prog", -C_PROGRAMMER }, { "deg", -C_DEG },
		{ "rad", -C_RAD }, { "grad", -C_GRAD }, { "hex", -C_HEX }, { "dec", -C_DEC }, { "oct", -C_OCT },
		{ "bin", -C_BIN }, { "qword", -C_QWORD }, { "dword", -C_DWORD }, { "word", -C_WORD }, { "byte", -C_BYTE },
		{ "group", -C_GROUPING },
		{ "sqrt", K_SQRT }, { "sqr", K_SQR }, { "cube", K_CUBE }, { "cbrt", K_CBRT }, { "recip", K_RECIP },
		{ "neg", K_NEG }, { "pct", K_PCT }, { "fact", K_FACT }, { "sin", K_SIN }, { "cos", K_COS },
		{ "tan", K_TAN }, { "sinh", K_SINH }, { "cosh", K_COSH }, { "tanh", K_TANH }, { "ln", K_LN },
		{ "log", K_LOG }, { "ten", K_TEN }, { "int", K_INT }, { "dms", K_DMS }, { "pi", K_PI },
		{ "inv", K_INV }, { "fe", K_FE }, { "exp", K_EXP }, { "mod", K_MOD }, { "pow", K_POW },
		{ "root", K_ROOT }, { "rol", K_ROL }, { "ror", K_ROR }, { "not", K_NOT }, { "and", K_AND },
		{ "or", K_OR }, { "xor", K_XOR }, { "lsh", K_LSH }, { "rsh", K_RSH }, { "mc", K_MC }, { "mr", K_MR },
		{ "ms", K_MS }, { "m+", K_MPLUS }, { "m-", K_MMINUS }, { "ce", K_CE }, { "c", K_C }, { "back", K_BACK },
		{ "eq", K_EQ },
	};
	char t[200];
	int a, i, n;
	const char *s;

	for (a = 0; a < argc; a++)
		for (s = argv[a]; *s; s++) {
			if (*s != '{') {
				type_char((unsigned char)*s, 0);
				continue;
			}
			n = (int)strcspn(s + 1, "}");
			for (i = 0; i < (int)(sizeof named / sizeof named[0]); i++)
				if ((int)strlen(named[i].name) == n && !strncmp(named[i].name, s + 1, n)) {
					if (named[i].id < 0)
						command(-named[i].id);
					else
						press(named[i].id);
				}
			s += n + (s[n + 1] == '}');
		}
	display_text(t, sizeof t, grouping);
	printf("%s\t%s\n", t, expr);
	if (history_list.count)
		printf("history: %s\n", history[history_list.count - 1].text);
	return 0;
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int id, k, i, b;

	if (argc > 1 && !strcmp(argv[1], "-e"))
		return test(argc - 2, argv + 2);
	if (GrOpen() < 0)
		return 1;
	ui_init();
	ui_menu_restyle();
	fonts();
	mode = reg_int("nxcalc.mode", STANDARD);
	mode = mode < STANDARD || mode > PROGRAMMER ? STANDARD : mode;
	show_history = reg_int("nxcalc.history", 0) != 0;
	grouping = reg_int("nxcalc.grouping", 0) != 0;
	angle = reg_int("nxcalc.angle", DEG);
	angle = angle < DEG || angle > GRAD ? DEG : angle;
	base = reg_int("nxcalc.base", 10);
	base = base == 16 || base == 8 || base == 2 ? base : 10;
	word = reg_int("nxcalc.word", 32);
	word = word == 64 || word == 16 || word == 8 ? word : 32;
	layout();
	window = GrNewWindowEx(UI_FIXED, "Calculator", GR_ROOT_WINDOW_ID, -1, -1, width, height, GROUND);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEvent(&event);
		ui_origin_note(&event);
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id);
			flush();
			continue;
		case UI_MENU_CLOSED:
			GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		if (ui_restyle()) {	/* (Settings changed the look) */
			ui_menu_restyle();
			fonts();
			layout();	/* (the menu bar's names are placed by the font's widths) */
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window)
				dirty |= D_ALL;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			type_char(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_KEY_UP:
			if (lit >= 0) {
				draw_key(lit, 0);
				lit = -1;
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN: {
			int px = event.button.x, py = event.button.y;

			if (event.button.wid != window)
				break;
			if (show_history && ui_inside(px, py, history_list.x, history_list.y, history_list.width, HISTORY_H)) {
				int r = ui_list_event(&history_list, &event);

				/* a click on a result brings it back */
				if (r && !ui_wheel(&event) && history_list.selected >= 0 && px < history_list.x + history_list.width - UI_SCROLL_W - 2) {
					if (error[0])
						clear_all();
					typing = 0;
					new_operand();
					value = history[history_list.selected].result;
					dirty |= D_DISPLAY;
				}
				if (r)
					dirty |= D_HISTORY;
				break;
			}
			if (ui_wheel(&event))
				break;
			if (py < MENU_H) {
				for (i = 0; i < 3; i++)
					if (px >= menu_x[i] && px < menu_x[i + 1])
						open_menu(i, 0);
				break;
			}
			if ((event.button.changebuttons & GR_BUTTON_R) && ui_inside(px, py, MARGIN, display_y, width - 2 * MARGIN, DISPLAY_H)) {
				ui_menu_open(pointer_items, 2, event.button.rootx, event.button.rooty, 0, 0);
				break;
			}
			if (!(event.button.changebuttons & GR_BUTTON_L))
				break;
			for (i = 0; i < radio_count; i++)
				if (ui_inside(px, py, radios[i].x, radios[i].y, radios[i].w, radios[i].h))
					command(radios[i].command);
			if ((b = bit_at(px, py)) >= 0 && b < word && !error[0]) {
				value = from_u(current().u ^ (uint64_t)1 << b);
				typing = 0;
				new_operand();
				dirty |= D_DISPLAY;
			}
			for (k = 0; k < KEY_COUNT; k++) {
				int x, y, w, h;

				key_rect(&keys[k], &x, &y, &w, &h);
				if (pane_shown(keys[k].pane) && ui_inside(px, py, x, y, w, h) && works(keys[k].id)) {
					held = k;
					draw_key(k, 1);
				}
			}
			break;
		}
		case GR_EVENT_TYPE_BUTTON_UP:
			if (held >= 0) {
				int x, y, w, h, k = held;

				held = -1;
				key_rect(&keys[k], &x, &y, &w, &h);
				draw_key(k, 0);
				if (event.button.wid == window && ui_inside(event.button.x, event.button.y, x, y, w, h))
					press(keys[k].id);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid == window) {
				GrClose();
				return 0;
			}
			break;
		}
		flush();
	}
}
