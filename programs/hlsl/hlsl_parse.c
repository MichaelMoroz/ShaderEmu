/* HLSL's text to a syntax tree: tokens (with #define), then the program's structs, uniforms,
 * constants and functions. */
#include <stdio.h>
#include <stdlib.h>
#include "hlsl_int.h"

/* ---- tokens ---- */

typedef struct { const char *name; int ntoks; tok *toks; int busy; } macro;
static macro *macros;
static int nmacros;

static int is_id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_id(int c) { return is_id0(c) || (c >= '0' && c <= '9'); }

static void put(tok t)
{
	if ((hc->ntoks & 255) == 0)
		hc->toks = hl_grow(hc->toks, hc->ntoks * sizeof(tok), (hc->ntoks + 256) * sizeof(tok));
	hc->toks[hc->ntoks++] = t;
}

static const char *const multi[] = { "<<=", ">>=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
	"++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "::" };

/* A number with a point or an exponent: its digits as an integer, then scaled in doubles
 * (musl's strtof works in long doubles, each operation a long call). */
static float read_float(const char *s)
{
	uint64_t m = 0;
	int e = 0, digits = 0, es = 0, neg = 0;
	double v, p = 10;

	for (; *s >= '0' && *s <= '9'; s++)
		if (digits < 18) m = m * 10 + (uint64_t)(*s - '0'), digits += m != 0;
		else e++;
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9'; s++)
			if (digits < 18) m = m * 10 + (uint64_t)(*s - '0'), digits += m != 0, e--;
	if (*s == 'e' || *s == 'E') {
		s++;
		if (*s == '-') neg = 1, s++;
		else if (*s == '+') s++;
		for (; *s >= '0' && *s <= '9'; s++) es = es * 10 + (*s - '0');
		e += neg ? -es : es;
	}
	v = (double)m;
	/* v * 10^e by squaring */
	for (es = e < 0 ? -e : e; es; es >>= 1, p *= p)
		if (es & 1) v = e < 0 ? v / p : v * p;
	return (float)v;
}

/* One token at s; returns its end. */
static const char *one(const char *s, int line, tok *t)
{
	int k;

	memset(t, 0, sizeof *t);
	t->line = line;
	if (is_id0(*s)) {
		const char *e = s;

		while (is_id(*e)) e++;
		t->kind = TK_ID, t->s = hl_intern(s, (int)(e - s));
		return e;
	}
	if ((*s >= '0' && *s <= '9') || (*s == '.' && s[1] >= '0' && s[1] <= '9')) {
		char *e;
		const char *p = s;
		int isf = 0;

		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			t->kind = TK_INT, t->i = (uint32_t)strtoul(s + 2, &e, 16);
			p = e;
		} else {
			while ((*p >= '0' && *p <= '9')) p++;
			if (*p == '.') { isf = 1; p++; while (*p >= '0' && *p <= '9') p++; }
			if (*p == 'e' || *p == 'E') {
				isf = 1, p++;
				if (*p == '+' || *p == '-') p++;
				while (*p >= '0' && *p <= '9') p++;
			}
			if (isf) t->kind = TK_FLOAT, t->f = read_float(s);
			else
				t->kind = TK_INT, t->i = (uint32_t)strtoul(s, 0, 10);
		}
		if (*p == 'f' || *p == 'F' || *p == 'h' || *p == 'H') {
			if (t->kind == TK_INT) t->kind = TK_FLOAT, t->f = (float)t->i;
			p++;
		} else if (*p == 'u' || *p == 'U')
			t->uns = 1, p++;
		else if (*p == 'l' || *p == 'L')
			p++;
		return p;
	}
	if (strchr("<>+-*/%&|^=!:", *s) && *s)
		for (k = 0; k < (int)(sizeof multi / sizeof *multi); k++)
			if (multi[k][0] == s[0] && multi[k][1] == s[1] && (!multi[k][2] || multi[k][2] == s[2])) {
				t->kind = TK_P, t->p = 256 + k;
				return s + (multi[k][2] ? 3 : 2);
			}
	if (strchr("+-*/%<>=!&|^~?:;,.()[]{}", *s) && *s) {
		t->kind = TK_P, t->p = *s;
		return s + 1;
	}
	hl_fail(line, "a character that is not HLSL: '%c'", *s);
}

/* A token, its macro expanded */
static void emit(tok t, int depth)
{
	int m;

	if (t.kind == TK_ID)
		for (m = 0; m < nmacros; m++)
			if (macros[m].name == t.s && !macros[m].busy) {
				int k;

				if (depth > 32) hl_fail(t.line, "macros within macros too deep");
				macros[m].busy = 1;
				for (k = 0; k < macros[m].ntoks; k++) {
					tok u = macros[m].toks[k];

					u.line = t.line;
					emit(u, depth + 1);
				}
				macros[m].busy = 0;
				return;
			}
	put(t);
}

static void directive(const char *s, const char *e, int line)
{
	const char *p = s + 1;

	while (p < e && (*p == ' ' || *p == '\t')) p++;
	if (!strncmp(p, "define", 6)) {
		const char *n0, *n1;
		macro *m;

		p += 6;
		while (p < e && (*p == ' ' || *p == '\t')) p++;
		n0 = p;
		while (p < e && is_id(*p)) p++;
		n1 = p;
		if (n1 == n0) hl_fail(line, "#define without a name");
		if (*p == '(') hl_fail(line, "#define with parameters is not supported: use a function");
		macros = hl_grow(macros, nmacros * sizeof *macros, (nmacros + 1) * sizeof *macros);
		m = &macros[nmacros++];
		m->name = hl_intern(n0, (int)(n1 - n0));
		while (p < e) {
			tok t;

			while (p < e && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
			if (p >= e) break;
			if (p[0] == '/' && p[1] == '/') break;
			p = one(p, line, &t);
			m->toks = hl_grow(m->toks, m->ntoks * sizeof(tok), (m->ntoks + 1) * sizeof(tok));
			m->toks[m->ntoks++] = t;
		}
	} else if (strncmp(p, "pragma", 6) && strncmp(p, "line", 4))
		hl_fail(line, "a directive other than #define and #pragma");
}

void hl_lex(void)
{
	const char *s = hc->src;
	int line = 1, bol = 1;
	tok t;

	macros = 0, nmacros = 0;
	while (*s) {
		if (*s == '\n') { line++, s++, bol = 1; continue; }
		if (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\f' || *s == '\v') { s++; continue; }
		if (s[0] == '/' && s[1] == '/') { while (*s && *s != '\n') s++; continue; }
		if (s[0] == '/' && s[1] == '*') {
			s += 2;
			while (*s && !(s[0] == '*' && s[1] == '/')) line += *s++ == '\n';
			if (*s) s += 2;
			continue;
		}
		if (*s == '#' && bol) {
			const char *e = s;

			while (*e && *e != '\n') {
				if (e[0] == '\\' && e[1] == '\n') hl_fail(line, "a directive over lines");
				e++;
			}
			directive(s, e, line);
			s = e;
			continue;
		}
		bol = 0;
		s = one(s, line, &t);
		emit(t, 0);
	}
	memset(&t, 0, sizeof t);
	t.kind = TK_EOF, t.line = line;
	put(t);
}

/* ---- types ---- */

int hl_type_comps(type t)
{
	int n;

	if (t.base == B_VOID) return 0;
	n = t.shape == S_STRUCT ? hc->structs[t.st].comps : t.rows * t.cols;
	return t.arr ? n * t.arr : n;
}

int hl_type_eq(type a, type b)
{
	return a.base == b.base && a.shape == b.shape && a.rows == b.rows && a.cols == b.cols && a.st == b.st && a.arr == b.arr;
}

const char *hl_type_name(type t, char *buf)
{
	static const char *const bases[] = { "void", "bool", "int", "uint", "float", "struct" };

	if (t.shape == S_STRUCT) return hc->structs[t.st].name;
	if (t.shape == S_SCALAR) sprintf(buf, "%s", bases[t.base]);
	else if (t.shape == S_VEC) sprintf(buf, "%s%d", bases[t.base], t.cols);
	else sprintf(buf, "%s%dx%d", bases[t.base], t.rows, t.cols);
	if (t.arr) sprintf(buf + strlen(buf), "[%d]", t.arr);
	return buf;
}

int hl_parse_type_name(const char *s, type *t)
{
	static const struct { const char *n; int base; } names[] = {
		{ "float", B_FLOAT }, { "half", B_FLOAT }, { "double", B_FLOAT }, { "min16float", B_FLOAT }, { "min10float", B_FLOAT },
		{ "int", B_INT }, { "min16int", B_INT }, { "uint", B_UINT }, { "dword", B_UINT }, { "min16uint", B_UINT },
		{ "bool", B_BOOL }, { "void", B_VOID }
	};
	int k, n;

	memset(t, 0, sizeof *t);
	for (k = 0; k < (int)(sizeof names / sizeof *names); k++) {
		n = (int)strlen(names[k].n);
		if (strncmp(s, names[k].n, n)) continue;
		s += n;
		t->base = (uint8_t)names[k].base, t->rows = 1, t->cols = 1;
		if (!*s) { t->shape = S_SCALAR; return 1; }
		if (t->base == B_VOID || s[0] < '1' || s[0] > '4') return 0;
		t->cols = (uint8_t)(s[0] - '0');
		if (!s[1]) { t->shape = S_VEC; return 1; }
		if (s[1] != 'x' || s[2] < '1' || s[2] > '4' || s[3]) return 0;
		t->shape = S_MAT, t->rows = t->cols, t->cols = (uint8_t)(s[2] - '0');
		return 1;
	}
	for (k = 0; k < hc->nstructs; k++)
		if (hc->structs[k].name == s) {
			t->base = B_STRUCT, t->shape = S_STRUCT, t->st = (int16_t)k, t->rows = t->cols = 1;
			return 1;
		}
	return 0;
}

/* ---- the parser ---- */

#define T (&hc->toks[hc->at])
#define TN(n) (&hc->toks[hc->at + (n) < hc->ntoks ? hc->at + (n) : hc->ntoks - 1])

static int is_p(int p) { return T->kind == TK_P && T->p == p; }
static int is_word(const char *w) { return T->kind == TK_ID && !strcmp(T->s, w); }
static int accept(int p) { if (is_p(p)) { hc->at++; return 1; } return 0; }
static int accept_word(const char *w) { if (is_word(w)) { hc->at++; return 1; } return 0; }

static void expect(int p)
{
	if (!accept(p)) {
		char what[4] = { (char)p, 0 };

		hl_fail(T->line, "'%s' expected", p < 256 ? what : multi[p - 256]);
	}
}

static const char *ident(void)
{
	if (T->kind != TK_ID) hl_fail(T->line, "a name expected");
	return hc->toks[hc->at++].s;
}

static node *mk(int k, int line)
{
	node *n = hl_alloc(sizeof *n);

	n->k = (uint8_t)k, n->line = line;
	return n;
}

static int is_type_at(int off)
{
	type t;
	tok *k = TN(off);

	if (k->kind != TK_ID) return 0;
	if (!strcmp(k->s, "vector") || !strcmp(k->s, "matrix")) return 1;
	return hl_parse_type_name(k->s, &t);
}

static int qualifiers(void)
{
	int q = 0;

	for (;;) {
		if (accept_word("const")) q |= Q_CONST;
		else if (accept_word("static")) q |= Q_STATIC;
		else if (accept_word("uniform")) q |= Q_UNIFORM;
		else if (accept_word("in")) q |= Q_IN;
		else if (accept_word("out")) q |= Q_OUT;
		else if (accept_word("inout")) q |= Q_IN | Q_OUT;
		else if (accept_word("extern") || accept_word("inline") || accept_word("precise") || accept_word("row_major") ||
			 accept_word("column_major") || accept_word("nointerpolation") || accept_word("linear") || accept_word("noperspective") ||
			 accept_word("centroid") || accept_word("volatile"))
			;
		else return q;
	}
}

static int const_int(void)
{
	int neg = accept('-');

	if (T->kind != TK_INT) hl_fail(T->line, "a whole number expected");
	return neg ? -(int)hc->toks[hc->at++].i : (int)hc->toks[hc->at++].i;
}

static type parse_type(void)
{
	type t;
	int line = T->line;

	if (accept_word("vector")) {
		expect('<');
		if (T->kind != TK_ID || !hl_parse_type_name(T->s, &t) || t.shape != S_SCALAR) hl_fail(line, "vector<a scalar, n> expected");
		hc->at++, expect(',');
		t.cols = (uint8_t)const_int(), t.shape = S_VEC;
		expect('>');
		if (t.cols < 1 || t.cols > 4) hl_fail(line, "a vector of 1 to 4");
		return t;
	}
	if (accept_word("matrix")) {
		expect('<');
		if (T->kind != TK_ID || !hl_parse_type_name(T->s, &t) || t.shape != S_SCALAR) hl_fail(line, "matrix<a scalar, r, c> expected");
		hc->at++, expect(',');
		t.rows = (uint8_t)const_int(), expect(',');
		t.cols = (uint8_t)const_int(), t.shape = S_MAT;
		expect('>');
		return t;
	}
	if (T->kind != TK_ID || !hl_parse_type_name(T->s, &t)) hl_fail(line, "a type expected");
	hc->at++;
	return t;
}

static void skip_semantic(void)
{
	/* `: SV_Target`, `: register(b0)`, `: packoffset(c0.x)` */
	if (accept(':')) {
		ident();
		if (accept('(')) {
			while (!is_p(')') && T->kind != TK_EOF) hc->at++;
			expect(')');
		}
	}
}

static node *expr(void);
static node *assign_expr(void);
static node *stmt(void);

static node *primary(void)
{
	node *n;
	int line = T->line;

	if (T->kind == TK_INT) {
		n = mk(N_INT, line), n->i = (int32_t)T->i, n->flags = T->uns;
		hc->at++;
		return n;
	}
	if (T->kind == TK_FLOAT) {
		n = mk(N_FLOAT, line), n->f = T->f;
		hc->at++;
		return n;
	}
	if (accept_word("true")) { n = mk(N_BOOL, line), n->i = 1; return n; }
	if (accept_word("false")) { n = mk(N_BOOL, line), n->i = 0; return n; }
	if (accept('(')) {
		/* a cast: (type) unary */
		if (is_type_at(0) && (TN(1)->kind == TK_P && (TN(1)->p == ')' || TN(1)->p == '<'))) {
			type t = parse_type();

			while (accept('[')) { t.arr = (int16_t)const_int(); expect(']'); }
			expect(')');
			n = mk(N_CAST, line), n->ty = t;
			n->a = mk(N_EMPTY, line);
			{
				extern node *hl_unary(void);
				n->a = hl_unary();
			}
			return n;
		}
		n = expr();
		expect(')');
		return n;
	}
	if (T->kind == TK_ID) {
		if (is_word("vector") || is_word("matrix")) {
			type t = parse_type();

			n = mk(N_CALL, line), n->ty = t, n->flags = 1;	/* a constructor of a written type */
			n->name = hl_intern("<type>", 6);
		} else {
			n = mk(N_ID, line), n->name = ident();
			if (!is_p('(')) return n;
			n->k = N_CALL;
		}
		expect('(');
		if (!is_p(')')) {
			node **tail = &n->a;

			do {
				*tail = assign_expr();
				tail = &(*tail)->next;
			} while (accept(','));
		}
		expect(')');
		return n;
	}
	if (is_p('{')) {
		node **tail;

		hc->at++;
		n = mk(N_LIST, line);
		tail = &n->a;
		while (!is_p('}')) {
			*tail = is_p('{') ? primary() : assign_expr();
			tail = &(*tail)->next;
			if (!accept(',')) break;
		}
		expect('}');
		return n;
	}
	hl_fail(line, "an expression expected");
}

static node *postfix(void)
{
	node *n = primary(), *m;

	for (;;) {
		int line = T->line;

		if (accept('.')) {
			m = mk(N_MEMBER, line), m->a = n, m->name = ident();
			n = m;
		} else if (accept('[')) {
			m = mk(N_INDEX, line), m->a = n, m->b = expr();
			expect(']');
			n = m;
		} else if (is_p(P_INC) || is_p(P_DEC)) {
			m = mk(N_POST, line), m->op = (int16_t)T->p, m->a = n;
			hc->at++;
			n = m;
		} else
			return n;
	}
}

node *hl_unary(void)
{
	int line = T->line;
	node *n;

	if (is_p('-') || is_p('+') || is_p('!') || is_p('~')) {
		n = mk(N_UN, line), n->op = (int16_t)T->p;
		hc->at++;
		n->a = hl_unary();
		return n;
	}
	if (is_p(P_INC) || is_p(P_DEC)) {
		n = mk(N_PRE, line), n->op = (int16_t)T->p;
		hc->at++;
		n->a = hl_unary();
		return n;
	}
	return postfix();
}

static int prec(int p)
{
	switch (p) {
	case '*': case '/': case '%': return 10;
	case '+': case '-': return 9;
	case P_SHL: case P_SHR: return 8;
	case '<': case '>': case P_LE: case P_GE: return 7;
	case P_EQ: case P_NE: return 6;
	case '&': return 5;
	case '^': return 4;
	case '|': return 3;
	case P_ANDAND: return 2;
	case P_OROR: return 1;
	}
	return 0;
}

static node *binary(int min)
{
	node *n = hl_unary();

	while (T->kind == TK_P && prec(T->p) >= min && prec(T->p)) {
		int p = T->p, line = T->line;
		node *m = mk(N_BIN, line);

		hc->at++;
		m->op = (int16_t)p, m->a = n, m->b = binary(prec(p) + 1);
		n = m;
	}
	return n;
}

static node *cond_expr(void)
{
	node *n = binary(1);

	if (is_p('?')) {
		node *m = mk(N_COND, T->line);

		hc->at++;
		m->a = n, m->b = assign_expr();
		expect(':');
		m->c = assign_expr();
		return m;
	}
	return n;
}

static node *assign_expr(void)
{
	node *n = cond_expr();

	if (T->kind == TK_P && (T->p == '=' || (T->p >= P_SHLEQ && T->p <= P_XOREQ))) {
		node *m = mk(N_ASSIGN, T->line);

		m->op = (int16_t)T->p;
		hc->at++;
		m->a = n, m->b = assign_expr();
		return m;
	}
	return n;
}

static node *expr(void)
{
	node *n = assign_expr();

	while (is_p(',')) {
		node *m = mk(N_COMMA, T->line);

		hc->at++;
		m->a = n, m->b = assign_expr();
		n = m;
	}
	return n;
}

/* `type a = 1, b[2] = {...};` as declarations one after another */
static node *declaration(int q, type base, int line)
{
	node *first = 0, **tail = &first;

	do {
		node *d = mk(N_DECL, T->line);

		d->flags = q, d->ty = base, d->name = ident();
		while (accept('[')) {
			d->ty.arr = (int16_t)const_int();
			expect(']');
		}
		skip_semantic();
		if (accept('=')) d->a = assign_expr();
		*tail = d, tail = &d->next;
	} while (accept(','));
	expect(';');
	(void)line;
	return first;
}

static int attributes(int *unroll)
{
	int f = 0;

	while (is_p('[') && TN(1)->kind == TK_ID) {
		hc->at++;
		if (accept_word("unroll")) {
			f |= A_UNROLL;
			if (accept('(')) { *unroll = const_int(); expect(')'); }
		} else if (accept_word("loop"))
			f |= A_LOOP;
		else {
			ident();
			if (accept('(')) { while (!is_p(')') && T->kind != TK_EOF) hc->at++; expect(')'); }
		}
		expect(']');
	}
	return f;
}

static node *block_body(int line)
{
	node *b = mk(N_BLOCK, line), **tail = &b->a;

	while (!is_p('}')) {
		if (T->kind == TK_EOF) hl_fail(line, "a '{' that is not closed");
		*tail = stmt();
		while (*tail) tail = &(*tail)->next;
	}
	expect('}');
	return b;
}

static node *stmt(void)
{
	int line = T->line, unroll = 0, at = attributes(&unroll);
	node *n;

	if (accept('{')) return block_body(line);
	if (accept(';')) return mk(N_EMPTY, line);
	if (accept_word("if")) {
		n = mk(N_IF, line);
		expect('(');
		n->a = expr();
		expect(')');
		n->b = stmt();
		if (accept_word("else")) n->c = stmt();
		return n;
	}
	if (accept_word("for")) {
		n = mk(N_FOR, line), n->flags = at, n->unroll = unroll;
		expect('(');
		if (!accept(';')) {
			int q = qualifiers();

			if (is_type_at(0) && TN(1)->kind == TK_ID) {
				type t = parse_type();

				n->a = declaration(q, t, line);
			} else {
				n->a = mk(N_EXPR, line), n->a->a = expr();
				expect(';');
			}
		}
		if (!is_p(';')) n->b = expr();
		expect(';');
		if (!is_p(')')) n->c = expr();
		expect(')');
		n->d = stmt();
		return n;
	}
	if (accept_word("while")) {
		n = mk(N_WHILE, line), n->flags = at, n->unroll = unroll;
		expect('(');
		n->a = expr();
		expect(')');
		n->d = stmt();
		return n;
	}
	if (accept_word("do")) {
		n = mk(N_DO, line), n->flags = at, n->unroll = unroll;
		n->d = stmt();
		if (!accept_word("while")) hl_fail(T->line, "while expected after do's body");
		expect('(');
		n->a = expr();
		expect(')');
		expect(';');
		return n;
	}
	if (accept_word("break")) { expect(';'); return mk(N_BREAK, line); }
	if (accept_word("continue")) { expect(';'); return mk(N_CONTINUE, line); }
	if (accept_word("discard")) { expect(';'); return mk(N_DISCARD, line); }
	if (accept_word("return")) {
		n = mk(N_RETURN, line);
		if (!is_p(';')) n->a = expr();
		expect(';');
		return n;
	}
	{
		int save = hc->at, q = qualifiers();

		if (is_type_at(0) && TN(1)->kind == TK_ID) {
			type t = parse_type();

			return declaration(q, t, line);
		}
		if (q) hl_fail(line, "a type expected");
		hc->at = save;
	}
	n = mk(N_EXPR, line);
	n->a = expr();
	expect(';');
	return n;
}

static void struct_decl(void)
{
	struct_def *s;
	int line = T->line;
	const char *name = ident();

	hc->structs = hl_grow(hc->structs, hc->nstructs * sizeof *s, (hc->nstructs + 1) * sizeof *s);
	s = &hc->structs[hc->nstructs];
	s->name = name;
	expect('{');
	while (!accept('}')) {
		type t;

		qualifiers();
		t = parse_type();
		do {
			type m = t;
			const char *mn = ident();

			while (accept('[')) { m.arr = (int16_t)const_int(); expect(']'); }
			skip_semantic();
			s->mt = hl_grow(s->mt, s->nmembers * sizeof(type), (s->nmembers + 1) * sizeof(type));
			s->mname = hl_grow(s->mname, s->nmembers * sizeof(char *), (s->nmembers + 1) * sizeof(char *));
			s->moff = hl_grow(s->moff, s->nmembers * sizeof(int), (s->nmembers + 1) * sizeof(int));
			s->mt[s->nmembers] = m, s->mname[s->nmembers] = mn, s->moff[s->nmembers] = s->comps;
			s->comps += hl_type_comps(m);
			s->nmembers++;
		} while (accept(','));
		expect(';');
	}
	accept(';');
	hc->nstructs++;
	if (s->comps > MAXC) hl_fail(line, "struct %s has more than %d components", name, MAXC);
}

static void add_global(const char *name, type t, int q, node *init, int line)
{
	global *g = hl_alloc(sizeof *g), **tail = &hc->globals;

	g->name = name, g->ty = t, g->flags = q, g->init = init, g->line = line, g->offset = -1;
	/* a global that is neither static nor const is a uniform, as in HLSL */
	if (!(q & (Q_STATIC | Q_CONST))) g->flags |= Q_UNIFORM;
	if ((g->flags & Q_UNIFORM) && !(q & Q_STATIC)) {
		g->offset = 16 + hc->uniform_bytes;
		hc->uniform_bytes += 4 * hl_type_comps(t);
		if (16 + hc->uniform_bytes > 2047) hl_fail(line, "more uniforms than fit in 2 KB");
	}
	while (*tail) tail = &(*tail)->next;
	*tail = g;
}

/* Whether a name is assigned somewhere under n (as a whole or in part), or handed to an out parameter. */
static int assigns(node *n, const char *name);

static int target_is(node *n, const char *name)
{
	while (n->k == N_MEMBER || n->k == N_INDEX) n = n->a;
	return n->k == N_ID && n->name == name;
}

static int assigns(node *n, const char *name)
{
	for (; n; n = n->next) {
		if ((n->k == N_ASSIGN || n->k == N_PRE || n->k == N_POST) && target_is(n->a, name)) return 1;
		if (n->k == N_CALL) {
			/* a call may hand it to an out parameter: taken as written */
			node *a;

			int k = 0;

			for (a = n->a; a; a = a->next, k++)
				if (target_is(a, name)) {
					func *f;

					for (f = hc->funcs; f; f = f->next)
						if (f->name == n->name && k < f->nparams && (f->params[k].flags & Q_OUT)) return 1;
					if (!strcmp(n->name, "sincos") && k > 0) return 1;
				}
		}
		if (n->k == N_DECL && n->name == name) {
			/* shadowed from here on: what follows in this list is another variable */
			if (n->a && assigns(n->a, name)) return 1;
			return 0;
		}
		if (assigns(n->a, name) || assigns(n->b, name) || assigns(n->c, name) || assigns(n->d, name)) return 1;
	}
	return 0;
}

int hl_assigns(node *n, const char *name) { return assigns(n, name); }

static int count_nodes(node *n)
{
	int c = 0;

	for (; n; n = n->next) c += 1 + count_nodes(n->a) + count_nodes(n->b) + count_nodes(n->c) + count_nodes(n->d);
	return c;
}

int hl_count_nodes(node *n) { return count_nodes(n); }

static void function(type ret, const char *name, int line)
{
	func *f = hl_alloc(sizeof *f), **tail = &hc->funcs;
	int p;

	f->name = name, f->ret = ret, f->line = line;
	expect('(');
	if (!is_p(')') && !(is_word("void") && TN(1)->kind == TK_P && TN(1)->p == ')'))
		do {
			param *pp;
			int q = qualifiers();

			f->params = hl_grow(f->params, f->nparams * sizeof *pp, (f->nparams + 1) * sizeof *pp);
			pp = &f->params[f->nparams++];
			pp->ty = parse_type();
			pp->name = ident();
			while (accept('[')) { pp->ty.arr = (int16_t)const_int(); expect(']'); }
			pp->flags = q & (Q_IN | Q_OUT) ? q & (Q_IN | Q_OUT) : Q_IN;
			skip_semantic();
			if (accept('=')) hl_fail(T->line, "a parameter's default value is not supported");
		} while (accept(','));
	else if (is_word("void"))
		hc->at++;
	expect(')');
	skip_semantic();
	if (accept(';')) return;	/* (a declaration before its definition) */
	if (!is_p('{')) hl_fail(T->line, "'{' expected for function %s", name);
	hc->at++;
	f->body = block_body(line);
	for (p = 0; p < f->nparams && p < 31; p++)
		if (assigns(f->body->a, f->params[p].name)) f->written |= 1 << p;
	f->cost = count_nodes(f->body);
	while (*tail) tail = &(*tail)->next;
	*tail = f;
}

void hl_parse(void)
{
	hc->at = 0;
	while (T->kind != TK_EOF) {
		int line = T->line, q;
		type t;

		if (accept(';')) continue;
		if (accept_word("struct")) { struct_decl(); continue; }
		if (accept_word("cbuffer") || accept_word("tbuffer")) {
			ident();
			skip_semantic();
			expect('{');
			while (!accept('}')) {
				node *d;

				q = qualifiers();
				t = parse_type();
				for (d = declaration(q, t, line); d; d = d->next)
					add_global(d->name, d->ty, (q & ~(Q_STATIC | Q_CONST)) | Q_UNIFORM, 0, d->line);
			}
			accept(';');
			continue;
		}
		if (is_word("Texture2D") || is_word("SamplerState") || is_word("sampler") || is_word("Texture3D") || is_word("RWTexture2D"))
			hl_fail(line, "textures and samplers are not supported: the lanes read memory only through uniforms");
		attributes(&q);
		q = qualifiers();
		t = parse_type();
		{
			const char *name = ident();

			if (is_p('(')) { function(t, name, line); continue; }
			hc->at--;
		}
		{
			node *d;

			for (d = declaration(q, t, line); d; d = d->next)
				add_global(d->name, d->ty, q, d->a, d->line);
		}
	}
}
