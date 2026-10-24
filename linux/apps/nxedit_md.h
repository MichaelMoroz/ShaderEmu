/*
 * nxedit_md.h: Markdown made HTML for the editor's reader (docs/system/nanox.md, "The editor's
 * reader"): the browser's engine (web.h, web_layout.h) lays the HTML out. CommonMark's blocks
 * (headings, paragraphs, block quotes, lists in lists, code indented or fenced, rules) and
 * GitHub's tables; inline: emphasis, strong, strike, code, links, autolinks, pictures as their
 * description, hard breaks. Not here: references ([text][name]), HTML of the text's own (shown
 * as text). No part of the window system: tools/web_test.c lays a .md file out through it.
 *
 *   char *html = md_html(count, line_of, &size);   line_of(i, &n): line i's bytes, without its newline
 */
#ifndef NXEDIT_MD_H
#define NXEDIT_MD_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct md_buf { char *s; int n, room; };

static void
md_grow(struct md_buf *b, int more)
{
	int room = b->room ? b->room : 256;
	char *to;

	while (room < b->n + more + 1)
		room *= 2;
	if ((to = realloc(b->s, room)) == NULL) {
		fprintf(stderr, "md: out of memory\n");
		exit(1);
	}
	b->s = to, b->room = room;
}

static inline void
md_need(struct md_buf *b, int more)
{
	if (b->n + more + 1 > b->room)
		md_grow(b, more);
}

static void
md_put(struct md_buf *b, const char *s, int n)
{
	if (n < 0)
		n = (int)strlen(s);
	md_need(b, n);
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}

static inline void
md_putc(struct md_buf *b, int c)
{
	md_need(b, 1);
	b->s[b->n++] = (char)c;
	b->s[b->n] = 0;
}

/* Text as HTML: the four characters that would be markup said as entities. An entity the text
 * writes itself (&amp;, &#169;) is kept when `entities`. */
static void
md_esc(struct md_buf *b, const char *s, int n, int entities)
{
	int i, j, from;

	for (i = 0; i < n; i++) {
		/* (a run of plain text at once: nearly every byte is one) */
		for (from = i; i < n && s[i] != '<' && s[i] != '>' && s[i] != '"' && s[i] != '&'; i++)
			;
		if (i > from)
			md_put(b, s + from, i - from);
		if (i == n)
			break;
		switch (s[i]) {
		case '<': md_put(b, "&lt;", 4); break;
		case '>': md_put(b, "&gt;", 4); break;
		case '"': md_put(b, "&quot;", 6); break;
		case '&':
			for (j = i + 1; entities && j < n && j < i + 32 && (isalnum((unsigned char)s[j]) || (j == i + 1 && s[j] == '#')); j++)
				;
			if (entities && j < n && j > i + 1 && s[j] == ';')
				md_putc(b, '&');
			else
				md_put(b, "&amp;", 5);
			break;
		default:
			md_putc(b, s[i]);
		}
	}
}

/* A code block's text: a line's first spaces are no-break ones (the engine starts a line at its
 * first word, in a <pre> too). */
static void
md_code(struct md_buf *b, const char *s, int n)
{
	int i = 0, from;

	while (i < n) {
		for (; i < n && s[i] == ' '; i++)
			md_put(b, "&#160;", 6);
		for (from = i; i < n && s[i] != '\n'; i++)
			;
		md_esc(b, s + from, i - from, 0);
		if (i < n)
			md_putc(b, s[i++]);
	}
}

static int
md_punct(int c)
{
	return c > 0 && c < 128 && ispunct(c);
}

static int
md_white(int c)
{
	return c == 0 || c == ' ' || c == '\t' || c == '\n';
}

/* ---- inline ---- */

enum { MT_TEXT, MT_DELIM };
struct md_tok {
	int kind, at, n;		/* text: its HTML in the call's buffer */
	char c;				/* a run of * _ ~: the character, what is left of it, how long it was */
	int count, orig, open, close, active;
	unsigned char otag[8], ctag[8];	/* tags it opens (inner first) and closes: 1 em, 2 strong, 3 del */
	int ot, ct;
};

/* After the code span whose opening run of backticks is at `at`: where it ends, or -1. */
static int
md_code_end(const char *s, int n, int at, int *k)
{
	int j, m;

	for (*k = 0; at + *k < n && s[at + *k] == '`'; ++*k)
		;
	for (j = at + *k; j < n; j += m ? m : 1) {
		for (m = 0; j + m < n && s[j + m] == '`'; m++)
			;
		if (m == *k)
			return j;
	}
	return -1;
}

/* [text](destination "title") from the '[' at `at`: the text's and the destination's bounds and
 * where it ends; 0 when it is not one. */
static int
md_link(const char *s, int n, int at, int *ta, int *tb, int *da, int *db, int *end)
{
	int j = at + 1, depth = 0, parens = 0, k, close;

	while (j < n) {
		if (s[j] == '\\' && j + 1 < n) {
			j += 2;
			continue;
		}
		if (s[j] == '`' && (close = md_code_end(s, n, j, &k)) >= 0) {
			j = close + k;
			continue;
		}
		if (s[j] == '[')
			depth++;
		else if (s[j] == ']' && depth-- == 0)
			break;
		j++;
	}
	if (j + 1 >= n || s[j + 1] != '(')
		return 0;
	*ta = at + 1, *tb = j;
	for (j += 2; j < n && md_white(s[j]); j++)
		;
	if (j < n && s[j] == '<') {
		*da = ++j;
		while (j < n && s[j] != '>' && s[j] != '\n')
			j++;
		if (j >= n || s[j] != '>')
			return 0;
		*db = j++;
	} else {
		*da = j;
		for (; j < n && !md_white(s[j]); j++) {
			if (s[j] == '\\' && j + 1 < n)
				j++;
			else if (s[j] == '(')
				parens++;
			else if (s[j] == ')' && parens-- == 0)
				break;
		}
		*db = j;
	}
	while (j < n && md_white(s[j]))
		j++;
	if (j < n && (s[j] == '"' || s[j] == '\'' || s[j] == '(')) {
		char q = s[j] == '(' ? ')' : s[j];

		for (j++; j < n && s[j] != q; j++)
			if (s[j] == '\\')
				j++;
		j++;
		while (j < n && md_white(s[j]))
			j++;
	}
	if (j >= n || s[j] != ')')
		return 0;
	*end = j + 1;
	return 1;
}

/* An address as an attribute's value, its backslashes' escapes undone. */
static void
md_href(struct md_buf *b, const char *s, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (s[i] == '\\' && i + 1 < n && md_punct(s[i + 1]))
			i++;
		md_esc(b, s + i, 1, 0);
	}
}

static void md_inline(struct md_buf *out, const char *s, int n, int depth);

static void
md_link_out(struct md_buf *b, const char *href, int hn, const char *text, int tn, int depth, int literal)
{
	md_put(b, "<a href=\"", -1);
	md_href(b, href, hn);
	md_put(b, "\">", 2);
	if (literal)
		md_esc(b, text, tn, 0);
	else
		md_inline(b, text, tn, depth + 1);
	md_put(b, "</a>", 4);
}

/* The length of a bare web address at s (http:// or https://), its closing punctuation left out. */
static int
md_bare_url(const char *s, int n)
{
	int j, open = 0, close = 0;

	if (!(n > 8 && !strncmp(s, "https://", 8)) && !(n > 7 && !strncmp(s, "http://", 7)))
		return 0;
	for (j = 0; j < n && !md_white(s[j]) && s[j] != '<'; j++)
		open += s[j] == '(', close += s[j] == ')';
	while (j > 8 && (strchr(".,;:!?'\"*_~`", s[j - 1]) || (s[j - 1] == ')' && close > open))) {
		close -= s[j - 1] == ')';
		j--;
	}
	return j;
}

static void
md_tag(struct md_buf *b, int tag, int closing)
{
	static const char *const names[] = { "", "em", "strong", "del" };

	md_putc(b, '<');
	if (closing)
		md_putc(b, '/');
	md_put(b, names[tag], -1);
	md_putc(b, '>');
}

/* What may begin more than text in a paragraph (and "http", an address). */
static const unsigned char md_special[256] = { ['\\'] = 1, ['\n'] = 1, ['`'] = 1, ['!'] = 1, ['['] = 1, ['<'] = 1, ['*'] = 1,
	['_'] = 1, ['~'] = 1, ['&'] = 1 };

/* A paragraph's (or a heading's, a cell's) text as HTML. */
static void
md_inline(struct md_buf *out, const char *s, int n, int depth)
{
	struct md_buf txt = { 0 };
	struct md_tok *t = NULL, *o, *c;
	int tn = 0, troom = 0, i = 0, from = 0, k, j, ta, tb, da, db, end, ci, oi, use;

#define MD_TOKEN(KIND) do { \
		if (tn == troom) { \
			troom = troom ? troom * 2 : 32; \
			if ((t = realloc(t, troom * sizeof *t)) == NULL) { fprintf(stderr, "md: out of memory\n"); exit(1); } \
		} \
		memset(&t[tn], 0, sizeof *t); \
		t[tn].kind = KIND; \
		tn++; \
	} while (0)
#define MD_FLUSH() do { \
		if (txt.n > from) { MD_TOKEN(MT_TEXT); t[tn - 1].at = from, t[tn - 1].n = txt.n - from; } \
		from = txt.n; \
	} while (0)

	md_need(&txt, n + 16);
	while (i < n) {
		int ch = (unsigned char)s[i];

#define MD_PLAIN(at) (!md_special[(unsigned char)s[at]] && (s[at] != 'h' || at + 4 >= n || strncmp(s + at, "http", 4)))
		if (MD_PLAIN(i)) {
			/* a run of what can only be text, at once */
			for (k = i + 1; k < n && MD_PLAIN(k); k++)
				;
			md_esc(&txt, s + i, k - i, 0);
			i = k;
			continue;
		}
		if (ch == '\\' && i + 1 < n && md_punct(s[i + 1])) {
			md_esc(&txt, s + i + 1, 1, 0);
			i += 2;
		} else if (ch == '\\' && i + 1 < n && s[i + 1] == '\n') {
			md_put(&txt, "<br>", 4);
			i += 2;
		} else if (ch == '\n') {
			md_put(&txt, i >= 2 && s[i - 1] == ' ' && s[i - 2] == ' ' ? "<br>" : "\n", -1);
			i++;
		} else if (ch == '`') {
			if ((j = md_code_end(s, n, i, &k)) < 0) {
				md_put(&txt, s + i, k);
				i += k;
				continue;
			}
			ta = i + k, tb = j;
			/* (a space at each end is the span's padding, unless it is all spaces) */
			for (end = ta; end < tb && (s[end] == ' ' || s[end] == '\n'); end++)
				;
			if (tb - ta >= 2 && end < tb && (s[ta] == ' ' || s[ta] == '\n') && (s[tb - 1] == ' ' || s[tb - 1] == '\n'))
				ta++, tb--;
			md_put(&txt, "<code>", 6);
			for (; ta < tb; ta++)
				md_esc(&txt, s[ta] == '\n' ? " " : s + ta, 1, 0);
			md_put(&txt, "</code>", 7);
			i = j + k;
		} else if (ch == '!' && i + 1 < n && s[i + 1] == '[' && md_link(s, n, i + 1, &ta, &tb, &da, &db, &end)) {
			/* a picture: its description, as a picture that could not be shown */
			md_put(&txt, "<span class=pic>[", -1);
			md_esc(&txt, s + ta, tb - ta, 1);
			md_put(&txt, "]</span>", -1);
			i = end;
		} else if (ch == '[' && depth < 4 && md_link(s, n, i, &ta, &tb, &da, &db, &end)) {
			md_link_out(&txt, s + da, db - da, s + ta, tb - ta, depth, 0);
			i = end;
		} else if (ch == '<') {
			/* <scheme:address> or <name@place> */
			for (j = i + 1; j < n && s[j] != '>' && s[j] != '<' && !md_white(s[j]); j++)
				;
			for (k = i + 1; k < j && (isalnum((unsigned char)s[k]) || s[k] == '+' || s[k] == '.' || s[k] == '-'); k++)
				;
			if (j < n && s[j] == '>' && k < j && k - i > 2 && isalpha((unsigned char)s[i + 1]) && s[k] == ':') {
				md_link_out(&txt, s + i + 1, j - i - 1, s + i + 1, j - i - 1, depth, 1);
				i = j + 1;
			} else if (j < n && s[j] == '>' && memchr(s + i + 1, '@', j - i - 1)) {
				md_put(&txt, "<a href=\"mailto:", -1);
				md_esc(&txt, s + i + 1, j - i - 1, 0);
				md_put(&txt, "\">", 2);
				md_esc(&txt, s + i + 1, j - i - 1, 0);
				md_put(&txt, "</a>", 4);
				i = j + 1;
			} else {
				md_put(&txt, "&lt;", 4);
				i++;
			}
		} else if (ch == 'h' && (i == 0 || !isalnum((unsigned char)s[i - 1])) && depth == 0 && (k = md_bare_url(s + i, n - i)) > 0) {
			md_link_out(&txt, s + i, k, s + i, k, depth, 1);
			i += k;
		} else if (ch == '*' || ch == '_' || ch == '~') {
			int before = i ? (unsigned char)s[i - 1] : 0, after, left, right;

			for (k = 0; i + k < n && s[i + k] == ch; k++)
				;
			after = i + k < n ? (unsigned char)s[i + k] : 0;
			if (ch == '~' && k != 2) {
				md_put(&txt, s + i, k);
				i += k;
				continue;
			}
			left = !md_white(after) && (!md_punct(after) || md_white(before) || md_punct(before));
			right = !md_white(before) && (!md_punct(before) || md_white(after) || md_punct(after));
			MD_FLUSH();
			MD_TOKEN(MT_DELIM);
			c = &t[tn - 1];
			c->c = (char)ch, c->count = c->orig = k, c->active = 1;
			c->open = ch == '_' ? left && (!right || md_punct(before)) : left;
			c->close = ch == '_' ? right && (!left || md_punct(after)) : right;
			i += k;
		} else {
			md_esc(&txt, s + i, 1, 1);
			i++;
		}
	}
	MD_FLUSH();
	/* emphasis: each closer with the nearest opener of its kind before it (CommonMark's
	 * delimiter runs, the rule of three too); what lies between them is text */
	for (ci = 0; ci < tn; ci++) {
		c = &t[ci];
		while (c->kind == MT_DELIM && c->active && c->close && c->count) {
			for (oi = ci - 1; oi >= 0; oi--) {
				o = &t[oi];
				if (o->kind != MT_DELIM || !o->active || !o->open || !o->count || o->c != c->c)
					continue;
				if (((o->open && o->close) || (c->open && c->close)) && (o->orig + c->orig) % 3 == 0 && (o->orig % 3 || c->orig % 3))
					continue;
				if (c->c == '~' && (o->count < 2 || c->count < 2))
					continue;
				break;
			}
			if (oi < 0)
				break;
			use = c->c == '~' || (o->count >= 2 && c->count >= 2) ? 2 : 1;
			o->count -= use, c->count -= use;
			if (o->ot < 8 && c->ct < 8) {
				o->otag[o->ot++] = c->ctag[c->ct++] = c->c == '~' ? 3 : use;
			}
			for (k = oi + 1; k < ci; k++)
				t[k].active = 0;
		}
	}
	for (k = 0; k < tn; k++) {
		if (t[k].kind == MT_TEXT) {
			md_put(out, txt.s + t[k].at, t[k].n);
			continue;
		}
		for (j = 0; j < t[k].ct; j++)
			md_tag(out, t[k].ctag[j], 1);
		for (j = 0; j < t[k].count; j++)
			md_putc(out, t[k].c);
		for (j = t[k].ot - 1; j >= 0; j--)
			md_tag(out, t[k].otag[j], 0);
	}
	free(t);
	free(txt.s);
#undef MD_TOKEN
#undef MD_FLUSH
#undef MD_PLAIN
}

/* ---- blocks ---- */

enum { MB_DOC, MB_QUOTE, MB_LIST, MB_ITEM };
enum { ML_NONE, ML_PARA, ML_ICODE, ML_FENCE, ML_TABLE };
#define MD_DEPTH 32
#define MD_COLUMNS 32

struct md_state {
	struct md_buf *out;
	struct { int kind, indent, ordered; char marker; } box[MD_DEPTH];
	int depth, leaf, para_lines, blank_code;
	struct md_buf text;		/* the leaf's lines so far */
	char fence_c;
	int fence_n, fence_indent;
	int columns, rows;
	char align[MD_COLUMNS];
};

static int
md_spaces(const char *l, int n, int p)
{
	int k = 0;

	while (p + k < n && l[p + k] == ' ')
		k++;
	return k;
}

/* A rule: three or more of - * _ and nothing but spaces among them. */
static int
md_is_rule(const char *l, int n, int q)
{
	char c = q < n ? l[q] : 0;
	int count = 0;

	if (c != '-' && c != '*' && c != '_')
		return 0;
	for (; q < n; q++) {
		if (l[q] == c)
			count++;
		else if (l[q] != ' ')
			return 0;
	}
	return count >= 3;
}

/* A list's marker at q: its width (0 for none), whether it is a number, the number, its character. */
static int
md_marker(const char *l, int n, int q, int *ordered, int *number, char *mark)
{
	int j = q;

	if (q < n && (l[q] == '-' || l[q] == '*' || l[q] == '+')) {
		if (q + 1 < n && l[q + 1] != ' ')
			return 0;
		*ordered = 0, *number = 0, *mark = l[q];
		return 1;
	}
	for (*number = 0; j < n && j - q < 9 && isdigit((unsigned char)l[j]); j++)
		*number = *number * 10 + l[j] - '0';
	if (j == q || j >= n || (l[j] != '.' && l[j] != ')') || (j + 1 < n && l[j + 1] != ' '))
		return 0;
	*ordered = 1, *mark = l[j];
	return j + 1 - q;
}

/* The cells of a table's row: their bounds in `at` (start, end pairs); how many. A pipe with a
 * backslash before it is no border. */
static int
md_cells(const char *l, int n, int *at, int most)
{
	int p = 0, cells = 0, start;

	while (p < n && l[p] == ' ')
		p++;
	if (p < n && l[p] == '|')
		p++;
	while (n > p && (l[n - 1] == ' '))
		n--;
	if (n > p && l[n - 1] == '|' && (n < 2 || l[n - 2] != '\\'))
		n--;
	for (start = p; p <= n; p++) {
		if (p < n && l[p] == '\\') {
			p++;
			continue;
		}
		if (p == n || l[p] == '|') {
			if (cells < most)
				at[2 * cells] = start, at[2 * cells + 1] = p;
			cells++;
			start = p + 1;
		}
	}
	return cells;
}

static int
md_has_pipe(const char *l, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		if (l[i] == '\\')
			i++;
		else if (l[i] == '|')
			return 1;
	}
	return 0;
}

/* A table's second line, ---|:---:|---: : the columns' alignment; how many columns, 0 for none. */
static int
md_delimiters(const char *l, int n, char *align)
{
	int at[2 * MD_COLUMNS], cells = md_cells(l, n, at, MD_COLUMNS), i, a, b, dashes;

	if (!md_has_pipe(l, n) || cells > MD_COLUMNS)
		return 0;
	for (i = 0; i < cells; i++) {
		for (a = at[2 * i], b = at[2 * i + 1]; a < b && l[a] == ' '; a++)
			;
		while (b > a && l[b - 1] == ' ')
			b--;
		if (b == a)
			return 0;
		align[i] = l[a] == ':' ? (l[b - 1] == ':' ? 'c' : 'l') : l[b - 1] == ':' ? 'r' : 0;
		for (dashes = 0, a += l[a] == ':', b -= l[b - 1] == ':' && b - 1 >= a; a < b; a++, dashes++)
			if (l[a] != '-')
				return 0;
		if (!dashes)
			return 0;
	}
	return cells;
}

/* One row of cells: each cell's text, its pipes' backslashes taken out, made HTML. */
static void
md_row(struct md_state *st, const char *l, int n, const char *tag)
{
	static const char *const aligns[] = { "", " align=left", " align=center", " align=right" };
	int at[2 * MD_COLUMNS], cells = md_cells(l, n, at, MD_COLUMNS), i, a, b;
	struct md_buf cell = { 0 };

	md_put(st->out, "<tr>", 4);
	for (i = 0; i < st->columns; i++) {
		md_putc(st->out, '<');
		md_put(st->out, tag, -1);
		md_put(st->out, aligns[st->align[i] == 'l' ? 1 : st->align[i] == 'c' ? 2 : st->align[i] == 'r' ? 3 : 0], -1);
		md_putc(st->out, '>');
		if (i < cells) {
			for (a = at[2 * i], b = at[2 * i + 1]; a < b && l[a] == ' '; a++)
				;
			while (b > a && l[b - 1] == ' ')
				b--;
			for (cell.n = 0; a < b; a++)
				if (!(l[a] == '\\' && a + 1 < b && l[a + 1] == '|'))
					md_putc(&cell, l[a]);
			md_inline(st->out, cell.s ? cell.s : "", cell.n, 0);
		}
		md_put(st->out, "</", 2);
		md_put(st->out, tag, -1);
		md_putc(st->out, '>');
	}
	md_put(st->out, "</tr>\n", -1);
	free(cell.s);
}

/* A heading's id: its words in lower case, joined by dashes (as GitHub makes them). */
static void
md_slug(struct md_buf *b, const char *s, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];

		if (isalnum(c) || c >= 0x80 || c == '_' || c == '-')
			md_putc(b, c >= 0x80 ? c : tolower(c));
		else if (c == ' ')
			md_putc(b, '-');
	}
}

static void
md_heading(struct md_state *st, int level, const char *s, int n)
{
	char tag[8];

	while (n > 0 && s[n - 1] == ' ')
		n--;
	snprintf(tag, sizeof tag, "h%d", level);
	md_putc(st->out, '<');
	md_put(st->out, tag, -1);
	md_put(st->out, " id=\"", -1);
	md_slug(st->out, s, n);
	md_put(st->out, "\">", 2);
	md_inline(st->out, s, n, 0);
	md_put(st->out, "</", 2);
	md_put(st->out, tag, -1);
	md_put(st->out, ">\n", 2);
}

static void
md_close_leaf(struct md_state *st)
{
	switch (st->leaf) {
	case ML_PARA:
		md_put(st->out, "<p>", 3);
		md_inline(st->out, st->text.s, st->text.n, 0);
		md_put(st->out, "</p>\n", -1);
		break;
	case ML_ICODE:
	case ML_FENCE:
		md_put(st->out, "<pre><code>", -1);
		md_code(st->out, st->text.s ? st->text.s : "", st->text.n && st->text.s[st->text.n - 1] == '\n' ? st->text.n - 1 : st->text.n);
		md_put(st->out, "</code></pre>\n", -1);
		break;
	case ML_TABLE:
		md_put(st->out, st->rows ? "</tbody></table>\n" : "</table>\n", -1);
		break;
	}
	st->leaf = ML_NONE;
	st->text.n = 0;
	st->para_lines = st->blank_code = st->rows = 0;
}

static void
md_close_box(struct md_state *st)
{
	int k = st->box[st->depth - 1].kind;

	md_close_leaf(st);
	md_put(st->out, k == MB_QUOTE ? "</blockquote>\n" : k == MB_ITEM ? "</li>\n" : st->box[st->depth - 1].ordered ? "</ol>\n" : "</ul>\n", -1);
	st->depth--;
}

static int
md_open_box(struct md_state *st, int kind)
{
	if (st->depth == MD_DEPTH)
		return 0;
	md_close_leaf(st);
	memset(&st->box[st->depth], 0, sizeof st->box[0]);
	st->box[st->depth++].kind = kind;
	return 1;
}

/* A fence: three or more backticks or tildes; their count, 0 for none. */
static int
md_fence(const char *l, int n, int q)
{
	int k = 0;

	if (q >= n || (l[q] != '`' && l[q] != '~'))
		return 0;
	while (q + k < n && l[q + k] == l[q])
		k++;
	if (k < 3 || (l[q] == '`' && memchr(l + q + k, '`', n - q - k)))
		return 0;
	return k;
}

static int
md_atx(const char *l, int n, int q)
{
	int k = 0;

	while (q + k < n && l[q + k] == '#')
		k++;
	return k >= 1 && k <= 6 && (q + k == n || l[q + k] == ' ') ? k : 0;
}

static void
md_line(struct md_state *st, const char *raw, int rn)
{
	static struct md_buf line;
	const char *l;
	int n, p = 0, matched = 1, k, sp, q, blank, all, w, ordered, number, chosen;
	char mark;

	/* tabs to spaces, to the next multiple of 4 */
	line.n = 0;
	if (!memchr(raw, '\t', rn)) {
		md_put(&line, raw, rn && raw[rn - 1] == '\r' ? rn - 1 : rn);
		rn = 0;
	}
	for (k = 0; k < rn; k++) {
		if (raw[k] == '\t')
			do
				md_putc(&line, ' ');
			while (line.n % 4);
		else if (raw[k] != '\r' || k + 1 < rn)
			md_putc(&line, raw[k]);
	}
	md_putc(&line, 0);
	line.n--;
	l = line.s, n = line.n;
	/* the open boxes this line goes on in */
	for (k = 1; k < st->depth; k++, matched++) {
		sp = md_spaces(l, n, p);
		if (st->box[k].kind == MB_QUOTE) {
			if (sp > 3 || p + sp >= n || l[p + sp] != '>')
				break;
			p += sp + 1;
			p += p < n && l[p] == ' ';
		} else if (st->box[k].kind == MB_ITEM) {
			if (p + sp >= n)
				p = n;
			else if (sp >= st->box[k].indent)
				p += st->box[k].indent;
			else
				break;
		}
	}
	all = matched == st->depth;
	sp = md_spaces(l, n, p);
	q = p + sp;
	blank = q >= n;
	if (!all && st->leaf == ML_PARA && !blank && (sp >= 4 || (l[q] != '>' && !md_marker(l, n, q, &ordered, &number, &mark) &&
	    !md_is_rule(l, n, q) && !md_atx(l, n, q) && !md_fence(l, n, q)))) {
		/* a paragraph's lazy line: it goes on, whatever its boxes */
		md_putc(&st->text, '\n');
		md_put(&st->text, l + q, n - q);
		st->para_lines++;
		return;
	}
	while (st->depth > matched)
		md_close_box(st);
	/* new boxes: block quotes and list items */
	while (!(all && (st->leaf == ML_FENCE || (st->leaf == ML_ICODE && sp >= 4)))) {
		sp = md_spaces(l, n, p);
		q = p + sp;
		if (sp >= 4 || q >= n)
			break;
		if (l[q] == '>') {
			while (st->box[st->depth - 1].kind == MB_LIST)
				md_close_box(st);	/* (a list whose last item ended: the quote comes after it) */
			if (!md_open_box(st, MB_QUOTE))
				break;
			md_put(st->out, "<blockquote>\n", -1);
			p = q + 1;
			p += p < n && l[p] == ' ';
			all = 0;
			continue;
		}
		if (md_is_rule(l, n, q) || !(w = md_marker(l, n, q, &ordered, &number, &mark)))
			break;
		k = md_spaces(l, n, q + w);
		/* (an empty item, or a list not from 1, does not break into a paragraph) */
		if (st->leaf == ML_PARA && (q + w + k >= n || (ordered && number != 1)))
			break;
		chosen = q + w + k >= n || k >= 5 ? 1 : k;
		md_close_leaf(st);
		if (st->box[st->depth - 1].kind == MB_LIST &&
		    (st->box[st->depth - 1].ordered != ordered || st->box[st->depth - 1].marker != mark))
			md_close_box(st);
		if (st->box[st->depth - 1].kind != MB_LIST) {
			if (st->depth + 2 > MD_DEPTH)
				break;
			md_open_box(st, MB_LIST);
			st->box[st->depth - 1].ordered = ordered, st->box[st->depth - 1].marker = mark;
			if (ordered && number != 1) {
				char start[32];

				snprintf(start, sizeof start, "<ol start=%d>\n", number);
				md_put(st->out, start, -1);
			} else
				md_put(st->out, ordered ? "<ol>\n" : "<ul>\n", -1);
		}
		md_open_box(st, MB_ITEM);
		st->box[st->depth - 1].indent = q - p + w + chosen;
		md_put(st->out, "<li>", 4);
		p = q + w + (chosen < k ? chosen : k);
		all = 0;
	}
	while (st->box[st->depth - 1].kind == MB_LIST)
		md_close_box(st);	/* (its last item ended, and no other began) */
	/* the leaf */
	sp = md_spaces(l, n, p);
	q = p + sp;
	blank = q >= n;
	if (st->leaf == ML_FENCE) {
		if (sp <= 3 && (k = md_fence(l, n, q)) >= st->fence_n && l[q] == st->fence_c && md_spaces(l, n, q + k) + q + k == n) {
			md_close_leaf(st);
			return;
		}
		p += sp < st->fence_indent ? sp : st->fence_indent;
		md_put(&st->text, l + p, n - p);
		md_putc(&st->text, '\n');
		return;
	}
	if (st->leaf == ML_ICODE) {
		if (blank) {
			st->blank_code++;
			return;
		}
		if (sp >= 4) {
			for (; st->blank_code; st->blank_code--)
				md_putc(&st->text, '\n');
			md_put(&st->text, l + p + 4, n - p - 4);
			md_putc(&st->text, '\n');
			return;
		}
		md_close_leaf(st);
	}
	if (blank) {
		md_close_leaf(st);
		return;
	}
	if (sp >= 4 && st->leaf != ML_PARA) {
		md_close_leaf(st);
		st->leaf = ML_ICODE;
		md_put(&st->text, l + p + 4, n - p - 4);
		md_putc(&st->text, '\n');
		return;
	}
	if (st->leaf == ML_TABLE) {
		if (md_has_pipe(l + q, n - q)) {
			if (!st->rows++)
				md_put(st->out, "<tbody>\n", -1);
			md_row(st, l + q, n - q, "td");
			return;
		}
		md_close_leaf(st);
	}
	if (sp <= 3) {
		if ((k = md_fence(l, n, q)) > 0) {
			md_close_leaf(st);
			st->leaf = ML_FENCE;
			st->fence_c = l[q], st->fence_n = k, st->fence_indent = sp;
			return;
		}
		if ((k = md_atx(l, n, q)) > 0) {
			int e = n;

			md_close_leaf(st);
			/* (the closing #s, when a space comes before them) */
			while (e > q + k && l[e - 1] == ' ')
				e--;
			for (w = e; w > q + k && l[w - 1] == '#'; w--)
				;
			if (w < e && (w == q + k || l[w - 1] == ' '))
				e = w;
			q += k;
			q += md_spaces(l, e, q);
			md_heading(st, k, l + q, e > q ? e - q : 0);
			return;
		}
		if (st->leaf == ML_PARA && (l[q] == '=' || l[q] == '-')) {
			for (k = q; k < n && l[k] == l[q]; k++)
				;
			if (md_spaces(l, n, k) + k == n) {
				md_heading(st, l[q] == '=' ? 1 : 2, st->text.s, st->text.n);
				st->leaf = ML_NONE;
				st->text.n = st->para_lines = 0;
				return;
			}
		}
		if (md_is_rule(l, n, q)) {
			md_close_leaf(st);
			md_put(st->out, "<hr>\n", -1);
			return;
		}
		if (st->leaf == ML_PARA && st->para_lines == 1 && md_has_pipe(st->text.s, st->text.n)) {
			char align[MD_COLUMNS];
			int at[2 * MD_COLUMNS], columns = md_delimiters(l + q, n - q, align);

			if (columns && md_cells(st->text.s, st->text.n, at, MD_COLUMNS) == columns) {
				struct md_buf head = st->text;

				st->text.s = NULL, st->text.n = st->text.room = 0;
				st->leaf = ML_NONE;
				st->columns = columns;
				memcpy(st->align, align, sizeof align);
				md_put(st->out, "<table>\n<thead>\n", -1);
				md_row(st, head.s, head.n, "th");
				md_put(st->out, "</thead>\n", -1);
				free(head.s);
				st->leaf = ML_TABLE;
				st->rows = 0;
				return;
			}
		}
	}
	if (st->leaf != ML_PARA) {
		md_close_leaf(st);
		st->leaf = ML_PARA;
	} else
		md_putc(&st->text, '\n');
	md_put(&st->text, l + q, n - q);
	st->para_lines++;
}

/* How the reader's page looks: GitHub's look, in the page's pixels (16 is the machine's 13). A page
 * ends in an empty block with a heading's margin above it (.end), the place a heading after the
 * text would have (nxedit_page.h puts the next section there), and has MD_BOTTOM below its end. */
#define MD_BOTTOM 24
#define MD_SHEET \
	"body{margin:12px 18px 0;color:#1f2328;font-family:sans-serif;font-size:18px;line-height:1.45}" \
	"h1,h2,h3,h4,h5,h6{margin:22px 0 12px;line-height:1.25}" \
	"h1{font-size:34px;padding-bottom:6px;border-bottom:1px solid #d1d9e0}" \
	"h2{font-size:27px;padding-bottom:5px;border-bottom:1px solid #d1d9e0}" \
	"h3{font-size:22px}h4{font-size:19px}h5{font-size:17px}h6{font-size:16px;color:#59636e}" \
	"p{margin:0 0 12px}" \
	"a{color:#0969da;text-decoration:underline}" \
	"code{font-size:16px;background:#eff1f3}" \
	"pre{white-space:pre-wrap;margin:0 0 14px;padding:10px 12px;background:#f6f8fa;border:1px solid #e1e4e8;line-height:1.3}" \
	"pre code{background:transparent}" \
	"blockquote{margin:0 0 12px;padding:0 0 0 14px;color:#59636e;border-left:4px solid #d1d9e0}" \
	"ul,ol{margin:0 0 12px;padding-left:30px}" \
	"li ul,li ol{margin:0}" \
	"li p{margin:0 0 4px}" \
	"table{margin:0 0 14px;border-top:1px solid #d1d9e0;border-left:1px solid #d1d9e0}" \
	"th,td{padding:4px 9px;border-right:1px solid #d1d9e0;border-bottom:1px solid #d1d9e0;vertical-align:top}" \
	"th{background:#f6f8fa;font-weight:bold}" \
	"hr{margin:20px 0;border-top:3px solid #d1d9e0}" \
	".pic{color:#59636e;font-style:italic}" \
	".end{margin-top:22px}"

/* The whole text as a page of HTML; the caller frees it. */
static char *
md_html(int count, const char *(*line_of)(int i, int *n), int *size)
{
	struct md_buf out = { 0 };
	struct md_state st;
	const char *l;
	int i, n;

	memset(&st, 0, sizeof st);
	st.out = &out;
	st.depth = 1;
	md_put(&out, "<html><head><meta charset=utf-8><style>" MD_SHEET "</style></head><body>\n", -1);
	for (i = 0; i < count; i++) {
		l = line_of(i, &n);
		md_line(&st, l, n);
	}
	while (st.depth > 1)
		md_close_box(&st);
	md_close_leaf(&st);
	md_put(&out, "<div class=end></div></body></html>\n", -1);
	free(st.text.s);
	*size = out.n;
	return out.s;
}

#endif
