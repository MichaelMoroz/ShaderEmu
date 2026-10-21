/* The syntax tree to the IR: every call inlined, every vector split into its components,
 * constants folded, small loops of a known count unrolled, and every other loop given a place
 * where a lane may stop (I_YIELD). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "hlsl_int.h"

int hl_assigns(node *n, const char *name);
int hl_count_nodes(node *n);

#define ZERO 0		/* register 0: x0 */

/* ---- registers, constants, instructions ---- */

static int new_vreg(int cls, int var)
{
	vreg *v;

	if (hc->nvr == hc->vr_cap) {
		hc->vr = hl_grow(hc->vr, hc->vr_cap * sizeof(vreg), (hc->vr_cap * 2 + 256) * sizeof(vreg));
		hc->vr_cap = hc->vr_cap * 2 + 256;
	}
	v = &hc->vr[hc->nvr];
	v->cls = (uint8_t)cls, v->var = (uint8_t)var, v->def = -1, v->block = -2;
	return hc->nvr++;
}

static int emit(int op, int d, int a, int b, int32_t imm)
{
	ins *i;

	if (hc->ncode == hc->code_cap) {
		hc->code = hl_grow(hc->code, hc->code_cap * sizeof(ins), (hc->code_cap * 2 + 1024) * sizeof(ins));
		hc->code_cap = hc->code_cap * 2 + 1024;
	}
	if (hc->ncode > 400000) hl_fail(hc->line, "the shader makes too much code");
	i = &hc->code[hc->ncode];
	i->op = (uint8_t)op, i->rm = 0, i->d = d, i->a = a, i->b = b, i->imm = imm;
	return hc->ncode++;
}

static int konst_of(int cls, uint32_t bits)
{
	unsigned h = (bits * 2654435761u + (unsigned)cls) >> 22;
	int k;

	if (!hc->khash) {
		hc->khash = hl_alloc(1024 * sizeof(int));
		for (k = 0; k < 1024; k++) hc->khash[k] = -1;
	}
	for (k = hc->khash[h]; k >= 0; k = hc->knext[k])
		if (hc->ks[k].cls == cls && hc->ks[k].bits == bits) return -2 - k;
	if (hc->nks == hc->ks_cap) {
		hc->ks = hl_grow(hc->ks, hc->ks_cap * sizeof(konst), (hc->ks_cap * 2 + 64) * sizeof(konst));
		hc->knext = hl_grow(hc->knext, hc->ks_cap * sizeof(int), (hc->ks_cap * 2 + 64) * sizeof(int));
		hc->ks_cap = hc->ks_cap * 2 + 64;
	}
	k = hc->nks++;
	hc->ks[k].cls = (uint8_t)cls, hc->ks[k].bits = bits;
	hc->knext[k] = hc->khash[h], hc->khash[h] = k;
	return -2 - k;
}

static int kf(float f) { uint32_t b; memcpy(&b, &f, 4); return konst_of(C_F, b); }
static int ki(int32_t i) { return konst_of(C_X, (uint32_t)i); }
static int is_k(int o) { return o <= -2; }	/* (-1 is no operand) */
static uint32_t kbits(int o) { return hc->ks[-2 - o].bits; }
static float kfv(int o) { float f; uint32_t b = kbits(o); memcpy(&f, &b, 4); return f; }
static int32_t kiv(int o) { return (int32_t)kbits(o); }
static int cls_of(int o) { return o <= -2 ? hc->ks[-2 - o].cls : hc->vr[o].cls; }

static int label_new(void) { return hc->labels++; }
static void label(int l) { emit(I_LABEL, -1, -1, -1, l); }
static void jump(int l) { emit(I_JMP, -1, -1, -1, l); }

/* A constant made a register (each use its own: a constant costs a load where it is used) */
static int reg(int o)
{
	int d;

	if (!is_k(o)) return o;
	if (cls_of(o) == C_X) {
		if (kbits(o) == 0) return ZERO;
		d = new_vreg(C_X, 0);
		emit(I_ICONST, d, -1, -1, (int32_t)kbits(o));
	} else {
		d = new_vreg(C_F, 0);
		emit(I_FCONST, d, -1, -1, (int32_t)kbits(o));
	}
	return d;
}

/* ---- integer and float operations, folded where they can be ---- */

static int imm_ok(int op)
{
	return op == I_ADD || op == I_AND || op == I_OR || op == I_XOR || op == I_SLT || op == I_SLTU || op == I_SHL || op == I_SHR ||
	       op == I_SHRU;
}

static int iop(int op, int a, int b)
{
	int d;

	if (is_k(a) && (b == -1 || is_k(b))) {
		int32_t x = kiv(a), y = b == -1 ? 0 : kiv(b), r = 0;
		uint32_t ux = (uint32_t)x, uy = (uint32_t)y;

		switch (op) {
		case I_ADD: r = (int32_t)(ux + uy); break;
		case I_SUB: r = (int32_t)(ux - uy); break;
		case I_MUL: r = (int32_t)(ux * uy); break;
		case I_DIV: r = y == 0 ? -1 : (x == INT32_MIN && y == -1) ? x : x / y; break;
		case I_DIVU: r = uy == 0 ? -1 : (int32_t)(ux / uy); break;
		case I_REM: r = y == 0 ? x : (x == INT32_MIN && y == -1) ? 0 : x % y; break;
		case I_REMU: r = uy == 0 ? x : (int32_t)(ux % uy); break;
		case I_AND: r = x & y; break;
		case I_OR: r = x | y; break;
		case I_XOR: r = x ^ y; break;
		case I_SHL: r = (int32_t)(ux << (uy & 31)); break;
		case I_SHR: r = x >> (uy & 31); break;
		case I_SHRU: r = (int32_t)(ux >> (uy & 31)); break;
		case I_SLT: r = x < y; break;
		case I_SLTU: r = ux < uy; break;
		case I_SEQ: r = x == y; break;
		case I_SNE: r = x != y; break;
		case I_SEQZ: r = x == 0; break;
		}
		return ki(r);
	}
	if (op == I_SUB && is_k(b)) op = I_ADD, b = ki(-kiv(b));
	if (is_k(b) && (op == I_ADD || op == I_OR || op == I_XOR || op == I_SHL || op == I_SHR || op == I_SHRU) && kiv(b) == 0) return a;
	if (is_k(b) && op == I_MUL && kiv(b) == 1) return a;
	if (is_k(a) && (op == I_ADD || op == I_MUL || op == I_AND || op == I_OR || op == I_XOR || op == I_SEQ || op == I_SNE)) {
		int t = a;

		a = b, b = t;
		if ((op == I_ADD || op == I_OR || op == I_XOR) && kiv(b) == 0) return a;
		if (op == I_MUL && kiv(b) == 1) return a;
	}
	d = new_vreg(C_X, 0);
	if (b != -1 && is_k(b) && imm_ok(op) && kiv(b) >= -2048 && kiv(b) <= 2047) {
		emit(op, d, reg(a), -1, kiv(b));
		return d;
	}
	if (op == I_SEQ && is_k(b) && kiv(b) == 0) {
		emit(I_SEQZ, d, reg(a), -1, 0);
		return d;
	}
	if (op == I_SNE && is_k(b) && kiv(b) == 0) {
		emit(I_SNE, d, reg(a), ZERO, 0);
		return d;
	}
	{
		int ra = reg(a), rb = b == -1 ? -1 : reg(b);

		emit(op, d, ra, rb, 0);
	}
	return d;
}

static float fold_f(int op, float x, float y)
{
	switch (op) {
	case I_FADD: return x + y;
	case I_FSUB: return x - y;
	case I_FMUL: return x * y;
	case I_FDIV: return x / y;
	case I_FSQRT: return sqrtf(x);
	case I_FMIN: return x < y ? x : y;
	case I_FMAX: return x > y ? x : y;
	case I_FNEG: return -x;
	case I_FABS: return fabsf(x);
	case I_FSGNJ: return copysignf(x, y);
	}
	return 0;
}

static int fop(int op, int a, int b)
{
	int d;

	if (is_k(a) && (b == -1 || is_k(b))) return kf(fold_f(op, kfv(a), b == -1 ? 0 : kfv(b)));
	if (b != -1 && is_k(b)) {
		float y = kfv(b);

		if ((op == I_FADD || op == I_FSUB) && y == 0.0f) return a;
		if ((op == I_FMUL || op == I_FDIV) && y == 1.0f) return a;
		if (op == I_FMUL && y == -1.0f) return fop(I_FNEG, a, -1);
		if (op == I_FDIV && y != 0.0f) return fop(I_FMUL, a, kf(1.0f / y));
		if (op == I_FSUB) return fop(I_FADD, a, kf(-y));
	}
	if (is_k(a)) {
		float x = kfv(a);

		if (op == I_FADD && x == 0.0f) return b;
		if (op == I_FMUL && x == 1.0f) return b;
		if (op == I_FSUB && x == 0.0f) return fop(I_FNEG, b, -1);
		if (op == I_FMUL && x == -1.0f) return fop(I_FNEG, b, -1);
	}
	if (op == I_FNEG && !is_k(a) && hc->vr[a].def >= 0 && !hc->vr[a].var && hc->code[hc->vr[a].def].op == I_FNEG)
		return hc->code[hc->vr[a].def].a;
	d = new_vreg(C_F, 0);
	{
		int ra = reg(a), rb = b == -1 ? -1 : reg(b);

		hc->vr[d].def = emit(op, d, ra, rb, 0);
	}
	return d;
}

/* a float comparison: an integer 0 or 1 */
static int fcmp(int op, int a, int b)
{
	int d;

	if (is_k(a) && is_k(b)) {
		float x = kfv(a), y = kfv(b);

		return ki(op == I_FEQ ? x == y : op == I_FLT ? x < y : x <= y);
	}
	d = new_vreg(C_X, 0);
	{
		int ra = reg(a), rb = reg(b);

		emit(op, d, ra, rb, 0);
	}
	return d;
}

static int cvt(int o, int from, int to)
{
	int d;

	if (from == to || (from != B_FLOAT && to != B_FLOAT && to != B_BOOL)) return o;
	if (to == B_BOOL) return from == B_FLOAT ? iop(I_SEQZ, fcmp(I_FEQ, o, kf(0)), -1) : iop(I_SNE, o, ki(0));
	if (is_k(o)) {
		if (from == B_FLOAT) {
			float f = kfv(o);

			if (to == B_UINT) return ki((int32_t)(f <= 0 ? 0u : f >= 4294967040.0f ? 0xffffffffu : (uint32_t)f));
			return ki(f != f ? 0 : f >= 2147483520.0f ? INT32_MAX : f <= -2147483648.0f ? INT32_MIN : (int32_t)f);
		}
		return kf(from == B_UINT ? (float)(uint32_t)kiv(o) : (float)kiv(o));
	}
	if (from == B_FLOAT) {
		d = new_vreg(C_X, 0);
		emit(to == B_UINT ? I_FCVTWUS : I_FCVTWS, d, o, -1, 0);
		hc->code[hc->ncode - 1].rm = 1;	/* toward zero, as a cast is */
		return d;
	}
	d = new_vreg(C_F, 0);
	hc->vr[d].def = emit(from == B_UINT ? I_FCVTSWU : I_FCVTSW, d, o, -1, 0);
	return d;
}

/* ---- values ---- */

static type scalar_t(int base) { type t; memset(&t, 0, sizeof t); t.base = (uint8_t)base, t.shape = S_SCALAR, t.rows = t.cols = 1; return t; }
static type vec_t(int base, int n) { type t = scalar_t(base); if (n > 1) t.shape = S_VEC, t.cols = (uint8_t)n; return t; }
static type mat_t(int base, int r, int c) { type t = scalar_t(base); t.shape = S_MAT, t.rows = (uint8_t)r, t.cols = (uint8_t)c; return t; }
static int cls_base(int base) { return base == B_FLOAT ? C_F : C_X; }

static void vk(val *v, type t, int o) { int i; v->t = t, v->n = hl_type_comps(t); for (i = 0; i < v->n; i++) v->c[i] = o; }
static int all_k(const val *v) { int i; for (i = 0; i < v->n; i++) if (!is_k(v->c[i])) return 0; return 1; }
static int bindable(const val *v)
{
	int i;

	for (i = 0; i < v->n; i++)
		if (!is_k(v->c[i]) && hc->vr[v->c[i]].var) return 0;
	return 1;
}

static void new_var(val *v, type t)
{
	int i;
	int base = t.base;

	v->t = t, v->n = hl_type_comps(t);
	if (v->n > MAXC) hl_fail(hc->line, "a value of more than %d components", MAXC);
	if (t.shape == S_STRUCT) {
		/* each member its own class */
		struct_def *s = &hc->structs[t.st];
		int m, k, at = 0, reps = t.arr ? t.arr : 1, r;

		for (r = 0; r < reps; r++)
			for (m = 0; m < s->nmembers; m++)
				for (k = 0; k < hl_type_comps(s->mt[m]); k++) {
					type mt = s->mt[m];

					if (mt.shape == S_STRUCT) hl_fail(hc->line, "a struct in a struct is not supported");
					v->c[at++] = new_vreg(cls_base(mt.base), 1);
				}
		return;
	}
	for (i = 0; i < v->n; i++) v->c[i] = new_vreg(cls_base(base), 1);
}

/* The base of component i of a value of type t */
static int comp_base(type t, int i)
{
	if (t.shape == S_STRUCT) {
		struct_def *s = &hc->structs[t.st];
		int m;

		i %= s->comps;
		for (m = s->nmembers - 1; m >= 0; m--)
			if (i >= s->moff[m]) return s->mt[m].base;
	}
	return t.base;
}

static void move(int d, int s)
{
	if (d == s) return;
	if (is_k(s)) {
		if (hc->vr[d].cls == C_X) emit(I_ICONST, d, -1, -1, (int32_t)kbits(s));
		else emit(I_FCONST, d, -1, -1, (int32_t)kbits(s));
		return;
	}
	emit(hc->vr[d].cls == C_X ? I_MOV : I_FMOV, d, s, -1, 0);
}

static char tn1[64], tn2[64];

/* v as type t: a scalar to every component, a vector to fewer, the bases converted */
static void convert(val *v, type t, int line)
{
	int i, n = hl_type_comps(t);

	if (hl_type_eq(v->t, t)) return;
	if (t.shape == S_STRUCT || v->t.shape == S_STRUCT || t.arr || v->t.arr) {
		if (v->t.shape == S_SCALAR && !v->t.arr && is_k(v->c[0]) && kbits(v->c[0]) == 0) {
			/* (S)0 */
			for (i = 0; i < n; i++) v->c[i] = comp_base(t, i) == B_FLOAT ? kf(0) : ki(0);
			v->t = t, v->n = n;
			return;
		}
		if (hl_type_comps(v->t) == n && t.shape == v->t.shape && t.st == v->t.st) { v->t = t; return; }
		hl_fail(line, "%s cannot be made %s", hl_type_name(v->t, tn1), hl_type_name(t, tn2));
	}
	if (v->n == 1 && n > 1) {
		int c = cvt(v->c[0], v->t.base, t.base);

		for (i = 0; i < n; i++) v->c[i] = c;
	} else if (v->n >= n && (v->t.shape != S_MAT || t.shape != S_MAT)) {
		if (v->t.shape == S_MAT && t.shape != S_MAT && t.shape != S_SCALAR && v->n != n)
			hl_fail(line, "%s cannot be made %s", hl_type_name(v->t, tn1), hl_type_name(t, tn2));
		for (i = 0; i < n; i++) v->c[i] = cvt(v->c[i], v->t.base, t.base);
	} else if (v->t.shape == S_MAT && t.shape == S_MAT && v->t.rows >= t.rows && v->t.cols >= t.cols) {
		int r, c, k = 0, out[16];

		for (r = 0; r < t.rows; r++)
			for (c = 0; c < t.cols; c++) out[k++] = cvt(v->c[r * v->t.cols + c], v->t.base, t.base);
		for (i = 0; i < n; i++) v->c[i] = out[i];
	} else
		hl_fail(line, "%s cannot be made %s", hl_type_name(v->t, tn1), hl_type_name(t, tn2));
	v->t = t, v->n = n;
}

static void store(const val *dst, val *src, int line)
{
	int i;

	convert(src, dst->t, line);
	for (i = 0; i < dst->n; i++) move(dst->c[i], src->c[i]);
}

/* ---- names ---- */

static void push_sym(const char *name, int kind, val *v, global *g)
{
	sym *s;

	if (hc->nsyms == hc->syms_cap) {
		hc->syms = hl_grow(hc->syms, hc->syms_cap * sizeof(sym), (hc->syms_cap * 2 + 64) * sizeof(sym));
		hc->syms_cap = hc->syms_cap * 2 + 64;
	}
	s = &hc->syms[hc->nsyms++];
	s->name = name, s->kind = kind, s->v = v, s->g = g;
}

static sym *find_sym(const char *name)
{
	int i;

	for (i = hc->nsyms - 1; i >= hc->sym_floor; i--)
		if (hc->syms[i].name == name) return &hc->syms[i];
	for (i = hc->nglobal_syms - 1; i >= 0; i--)
		if (hc->syms[i].name == name) return &hc->syms[i];
	return 0;
}

static val *heap_val(const val *v)
{
	val *h = hl_alloc(sizeof(val) - sizeof(int) * (MAXC - (v->n ? v->n : 1)));

	h->t = v->t, h->n = v->n;
	memcpy(h->c, v->c, sizeof(int) * v->n);
	return h;
}

static void copy_val(val *d, const val *s) { d->t = s->t, d->n = s->n; memcpy(d->c, s->c, sizeof(int) * s->n); }

/* ---- expressions ---- */

static void gen(node *n, val *out);
static void gen_stmts(node *n);

static int as_cond(node *n)
{
	val v;

	gen(n, &v);
	if (v.n != 1) hl_fail(n->line, "a condition must be one value, not %s", hl_type_name(v.t, tn1));
	return cvt(v.c[0], v.t.base, B_BOOL);
}

static void branch_false(node *n, int l);

static void branch_true(node *n, int l)
{
	if (n->k == N_BIN && n->op == P_OROR) { branch_true(n->a, l); branch_true(n->b, l); return; }
	if (n->k == N_BIN && n->op == P_ANDAND) {
		int skip = label_new();

		branch_false(n->a, skip);
		branch_true(n->b, l);
		label(skip);
		return;
	}
	if (n->k == N_UN && n->op == '!') { branch_false(n->a, l); return; }
	{
		int c = as_cond(n);

		if (is_k(c)) { if (kiv(c)) jump(l); return; }
		emit(I_BNZ, -1, c, -1, l);
	}
}

static void branch_false(node *n, int l)
{
	if (n->k == N_BIN && n->op == P_ANDAND) { branch_false(n->a, l); branch_false(n->b, l); return; }
	if (n->k == N_BIN && n->op == P_OROR) {
		int skip = label_new();

		branch_true(n->a, skip);
		branch_false(n->b, l);
		label(skip);
		return;
	}
	if (n->k == N_UN && n->op == '!') { branch_true(n->a, l); return; }
	{
		int c = as_cond(n);

		if (is_k(c)) { if (!kiv(c)) jump(l); return; }
		emit(I_BZ, -1, c, -1, l);
	}
}

static int promote(int a, int b)
{
	if (a == B_FLOAT || b == B_FLOAT) return B_FLOAT;
	if (a == B_UINT || b == B_UINT) return B_UINT;
	return B_INT;
}

/* Two values brought to one shape for a component-wise operation */
static type common(val *a, val *b, int base, int line)
{
	type t;

	if (a->t.shape == S_STRUCT || b->t.shape == S_STRUCT || a->t.arr || b->t.arr) hl_fail(line, "arithmetic on a struct or an array");
	if (a->n == 1 && b->n == 1) t = scalar_t(base);
	else if (a->n == 1) t = b->t, t.base = (uint8_t)base;
	else if (b->n == 1) t = a->t, t.base = (uint8_t)base;
	else if (a->t.shape == S_MAT || b->t.shape == S_MAT) {
		if (a->t.shape != b->t.shape) hl_fail(line, "a matrix and a vector: use mul()");
		t = mat_t(base, a->t.rows < b->t.rows ? a->t.rows : b->t.rows, a->t.cols < b->t.cols ? a->t.cols : b->t.cols);
	} else
		t = vec_t(base, a->n < b->n ? a->n : b->n);
	convert(a, t, line);
	convert(b, t, line);
	return t;
}

static void arith(int op, val *a, val *b, val *out, int line)
{
	int base, i;
	type t;

	if (op == P_ANDAND || op == P_OROR) {
		t = common(a, b, B_BOOL, line);
		out->t = t, out->n = a->n;
		for (i = 0; i < a->n; i++) out->c[i] = iop(op == P_ANDAND ? I_AND : I_OR, a->c[i], b->c[i]);
		return;
	}
	base = promote(a->t.base, b->t.base);
	if (op == '%' || op == '&' || op == '|' || op == '^' || op == P_SHL || op == P_SHR) {
		if (base == B_FLOAT && op != '%') hl_fail(line, "a bitwise operation on floats");
	}
	t = common(a, b, base, line);
	if (op == '<' || op == '>' || op == P_LE || op == P_GE || op == P_EQ || op == P_NE) {
		out->t = t, out->t.base = B_BOOL, out->n = a->n;
		for (i = 0; i < a->n; i++) {
			int x = a->c[i], y = b->c[i], r;

			if (base == B_FLOAT) {
				switch (op) {
				case '<': r = fcmp(I_FLT, x, y); break;
				case '>': r = fcmp(I_FLT, y, x); break;
				case P_LE: r = fcmp(I_FLE, x, y); break;
				case P_GE: r = fcmp(I_FLE, y, x); break;
				case P_EQ: r = fcmp(I_FEQ, x, y); break;
				default: r = iop(I_SEQZ, fcmp(I_FEQ, x, y), -1);
				}
			} else {
				int lt = base == B_UINT ? I_SLTU : I_SLT;

				switch (op) {
				case '<': r = iop(lt, x, y); break;
				case '>': r = iop(lt, y, x); break;
				case P_LE: r = iop(I_XOR, iop(lt, y, x), ki(1)); break;
				case P_GE: r = iop(I_XOR, iop(lt, x, y), ki(1)); break;
				case P_EQ: r = iop(I_SEQ, x, y); break;
				default: r = iop(I_SNE, x, y);
				}
			}
			out->c[i] = r;
		}
		return;
	}
	out->t = t, out->n = a->n;
	for (i = 0; i < a->n; i++) {
		int x = a->c[i], y = b->c[i], r;

		if (base == B_FLOAT) {
			switch (op) {
			case '+': r = fop(I_FADD, x, y); break;
			case '-': r = fop(I_FSUB, x, y); break;
			case '*': r = fop(I_FMUL, x, y); break;
			case '/': r = fop(I_FDIV, x, y); break;
			case '%': {
				/* fmod: x - y * trunc(x / y) */
				int q = cvt(cvt(fop(I_FDIV, x, y), B_FLOAT, B_INT), B_INT, B_FLOAT);

				r = fop(I_FSUB, x, fop(I_FMUL, y, q));
				break;
			}
			default: hl_fail(line, "an operation floats do not have");
			}
		} else {
			int u = base == B_UINT;

			switch (op) {
			case '+': r = iop(I_ADD, x, y); break;
			case '-': r = iop(I_SUB, x, y); break;
			case '*': r = iop(I_MUL, x, y); break;
			case '/': r = iop(u ? I_DIVU : I_DIV, x, y); break;
			case '%': r = iop(u ? I_REMU : I_REM, x, y); break;
			case '&': r = iop(I_AND, x, y); break;
			case '|': r = iop(I_OR, x, y); break;
			case '^': r = iop(I_XOR, x, y); break;
			case P_SHL: r = iop(I_SHL, x, y); break;
			case P_SHR: r = iop(u ? I_SHRU : I_SHR, x, y); break;
			default: hl_fail(line, "an operation integers do not have");
			}
		}
		out->c[i] = r;
	}
}

/* A float select made with a branch: c ? a : b */
static int fsel(int c, int a, int b)
{
	int r, l;

	if (is_k(c)) return kiv(c) ? a : b;
	if (a == b) return a;
	r = new_vreg(cls_of(a), 1);
	l = label_new();
	move(r, b);
	emit(I_BZ, -1, c, -1, l);
	move(r, a);
	label(l);
	return r;
}

static int swizzle_index(char ch)
{
	switch (ch) {
	case 'x': case 'r': return 0;
	case 'y': case 'g': return 1;
	case 'z': case 'b': return 2;
	case 'w': case 'a': return 3;
	}
	return -1;
}

/* Components of a member or swizzle of a value of type t: how many, their places, the type */
static int member_of(type t, const char *name, int *at, type *mt, int line)
{
	int k, n = (int)strlen(name);

	if (t.arr) hl_fail(line, "an array has no member %s", name);
	if (t.shape == S_STRUCT) {
		struct_def *s = &hc->structs[t.st];
		int m;

		for (m = 0; m < s->nmembers; m++)
			if (s->mname[m] == name) {
				int c = hl_type_comps(s->mt[m]);

				for (k = 0; k < c; k++) at[k] = s->moff[m] + k;
				*mt = s->mt[m];
				return c;
			}
		hl_fail(line, "struct %s has no member %s", s->name, name);
	}
	if (t.shape == S_MAT) {
		/* _m00 or _11 swizzles */
		const char *p = name;
		int c = 0;

		while (*p == '_') {
			int r0, c0;

			if (p[1] == 'm') r0 = p[2] - '0', c0 = p[3] - '0', p += 4;
			else r0 = p[1] - '1', c0 = p[2] - '1', p += 3;
			if (r0 < 0 || r0 >= t.rows || c0 < 0 || c0 >= t.cols || c >= 4) hl_fail(line, "no matrix member %s", name);
			at[c++] = r0 * t.cols + c0;
		}
		if (*p || !c) hl_fail(line, "no matrix member %s", name);
		*mt = vec_t(t.base, c);
		return c;
	}
	if (n > 4) hl_fail(line, "a swizzle of more than four: %s", name);
	for (k = 0; k < n; k++) {
		at[k] = swizzle_index(name[k]);
		if (at[k] < 0 || at[k] >= (t.shape == S_SCALAR ? 1 : t.cols)) hl_fail(line, "no component %s of %s", name, hl_type_name(t, tn1));
	}
	*mt = vec_t(t.base, n);
	return n;
}

/* An element's type and size: of an array its element, of a matrix a row, of a vector a component */
static type element_of(type t, int *size, int *count, int line)
{
	type e = t;

	if (t.arr) {
		e.arr = 0;
		*size = hl_type_comps(e), *count = t.arr;
		return e;
	}
	if (t.shape == S_MAT) { *size = t.cols, *count = t.rows; return vec_t(t.base, t.cols); }
	if (t.shape == S_VEC) { *size = 1, *count = t.cols; return scalar_t(t.base); }
	hl_fail(line, "%s cannot be indexed", hl_type_name(t, tn1));
}

static void lval(node *n, val *out);

static void gen_index(node *n, val *out)
{
	val base, idx;
	int size, count, k, i;
	type e;

	gen(n->a, &base);
	gen(n->b, &idx);
	e = element_of(base.t, &size, &count, n->line);
	convert(&idx, scalar_t(B_INT), n->line);
	out->t = e, out->n = size;
	if (is_k(idx.c[0])) {
		k = kiv(idx.c[0]);
		if (k < 0 || k >= count) hl_fail(n->line, "index %d out of 0 to %d", k, count - 1);
		for (i = 0; i < size; i++) out->c[i] = base.c[k * size + i];
		return;
	}
	/* a select a component: element 0, then each other where the index is its number */
	{
		val r;

		new_var(&r, e);
		for (i = 0; i < size; i++) move(r.c[i], base.c[i]);
		for (k = 1; k < count; k++) {
			int l = label_new();

			emit(I_BZ, -1, iop(I_SEQ, idx.c[0], ki(k)), -1, l);
			for (i = 0; i < size; i++) move(r.c[i], base.c[k * size + i]);
			label(l);
		}
		copy_val(out, &r);
	}
}

static void assign_to(node *target, val *v, int line);

static func *find_func(const char *name, int nargs, val *args);
static int gen_intrinsic(node *n, val *args, int nargs, val *out);
static void call_user(func *f, node *call, val *args, int nargs, val *out);

static void construct(type t, val *args, int nargs, val *out, int line)
{
	int i, k, n = hl_type_comps(t), at = 0;

	if (nargs == 1 && args[0].n == 1 && n > 1) {
		if (t.shape == S_STRUCT) hl_fail(line, "a struct needs every value");
		vk(out, t, cvt(args[0].c[0], args[0].t.base, t.base));
		return;
	}
	out->t = t, out->n = n;
	for (k = 0; k < nargs; k++)
		for (i = 0; i < args[k].n; i++) {
			if (at >= n) hl_fail(line, "too many values for %s", hl_type_name(t, tn1));
			out->c[at] = cvt(args[k].c[i], comp_base(args[k].t, i), comp_base(t, at));
			at++;
		}
	if (at != n) hl_fail(line, "%s needs %d values, has %d", hl_type_name(t, tn1), n, at);
}

static void list_values(node *n, type t, val *out, int line)
{
	/* { a, {b, c}, ... } flattened, each converted to the component's base */
	int at = 0, total = hl_type_comps(t);

	out->t = t, out->n = total;
	{
		node *stack[16];
		int sp = 0;
		node *e = n->a;

		for (;;) {
			if (!e) {
				if (!sp) break;
				e = stack[--sp];
				continue;
			}
			if (e->k == N_LIST) {
				if (sp == 16) hl_fail(line, "lists too deep");
				stack[sp++] = e->next;
				e = e->a;
				continue;
			}
			{
				val v;
				int i;

				gen(e, &v);
				for (i = 0; i < v.n; i++) {
					if (at >= total) hl_fail(line, "too many values in the list");
					out->c[at] = cvt(v.c[i], v.t.base, comp_base(t, at));
					at++;
				}
			}
			e = e->next;
		}
	}
	if (at != total) hl_fail(line, "the list has %d values of %d", at, total);
}

static void gen_call(node *n, val *out)
{
	val *args;
	int nargs = 0, k;
	node *a;
	type t;

	for (a = n->a; a; a = a->next) nargs++;
	args = hl_alloc(sizeof(val) * (nargs ? nargs : 1));
	if (n->flags == 1 || hl_parse_type_name(n->name, &t)) {
		if (n->flags == 1) t = n->ty;
		for (a = n->a, k = 0; a; a = a->next, k++) gen(a, &args[k]);
		construct(t, args, nargs, out, n->line);
		return;
	}
	{
		func *f;
		int any = 0;

		for (f = hc->funcs; f; f = f->next)
			if (f->name == n->name) any = 1;
		if (any) {
			/* out parameters are not values: their arguments are made in call_user */
			for (f = hc->funcs; f; f = f->next)
				if (f->name == n->name && f->nparams == nargs) break;
			if (!f) hl_fail(n->line, "no function %s of %d arguments", n->name, nargs);
			for (a = n->a, k = 0; a; a = a->next, k++) {
				func *g;
				int out_only = 1;

				for (g = hc->funcs; g; g = g->next)
					if (g->name == n->name && g->nparams == nargs && (g->params[k].flags & Q_IN)) out_only = 0;
				if (out_only) {
					/* (written after the call: no value) */
					type pt = f->params[k].ty;

					vk(&args[k], pt, comp_base(pt, 0) == B_FLOAT ? kf(0) : ki(0));
				} else
					gen(a, &args[k]);
			}
			f = find_func(n->name, nargs, args);
			call_user(f, n, args, nargs, out);
			return;
		}
	}
	if (!strcmp(n->name, "sincos")) {
		/* sincos(x, out s, out c) */
		val x, s, c, o[1];

		if (nargs != 3) hl_fail(n->line, "sincos(x, out s, out c)");
		gen(n->a, &x);
		args[0] = x;
		if (!gen_intrinsic(&(node){ .k = N_CALL, .name = hl_intern("sin", 3), .line = n->line }, &x, 1, &s)) hl_fail(n->line, "sin");
		if (!gen_intrinsic(&(node){ .k = N_CALL, .name = hl_intern("cos", 3), .line = n->line }, &x, 1, &c)) hl_fail(n->line, "cos");
		assign_to(n->a->next, &s, n->line);
		assign_to(n->a->next->next, &c, n->line);
		(void)o;
		out->t = scalar_t(B_VOID), out->n = 0;
		return;
	}
	for (a = n->a, k = 0; a; a = a->next, k++) gen(a, &args[k]);
	if (!gen_intrinsic(n, args, nargs, out)) hl_fail(n->line, "no function %s", n->name);
}

static void global_value(global *g, val *out, int line)
{
	if (g->flags & Q_UNIFORM) {
		int i;

		out->t = g->ty, out->n = hl_type_comps(g->ty);
		for (i = 0; i < out->n; i++) {
			int d = new_vreg(cls_base(comp_base(g->ty, i)), 0);

			hc->vr[d].def = emit(I_LDU, d, -1, -1, g->offset + 4 * i);
			out->c[i] = d;
		}
		return;
	}
	if (!g->init) hl_fail(line, "constant %s has no value", g->name);
	if (g->init->k == N_LIST) list_values(g->init, g->ty, out, line);
	else {
		gen(g->init, out);
		convert(out, g->ty, line);
	}
}

static void gen(node *n, val *out)
{
	hc->line = n->line;
	switch (n->k) {
	case N_INT:
		vk(out, scalar_t(n->flags ? B_UINT : B_INT), ki(n->i));
		return;
	case N_FLOAT:
		vk(out, scalar_t(B_FLOAT), kf(n->f));
		return;
	case N_BOOL:
		vk(out, scalar_t(B_BOOL), ki(n->i));
		return;
	case N_ID: {
		sym *s = find_sym(n->name);

		if (!s) hl_fail(n->line, "no variable %s", n->name);
		if (s->kind == 0) { copy_val(out, s->v); return; }
		if (s->kind == 2 && s->v) { copy_val(out, s->v); return; }
		global_value(s->g, out, n->line);
		if (s->kind == 2 && all_k(out)) s->v = heap_val(out);	/* a constant made once */
		return;
	}
	case N_LIST:
		hl_fail(n->line, "a { } list outside a declaration");
	case N_BIN: {
		val a, b;

		if ((n->op == P_ANDAND || n->op == P_OROR)) {
			gen(n->a, &a);
			if (a.n == 1) {
				/* scalars: the second is made only when the first does not decide */
				int ca = cvt(a.c[0], a.t.base, B_BOOL), r, l;

				if (is_k(ca)) {
					if ((n->op == P_ANDAND) != (kiv(ca) != 0)) { vk(out, scalar_t(B_BOOL), ca); return; }
					gen(n->b, &b);
					vk(out, scalar_t(B_BOOL), cvt(b.c[0], b.t.base, B_BOOL));
					if (b.n != 1) hl_fail(n->line, "a scalar and a vector in && or ||");
					return;
				}
				r = new_vreg(C_X, 1), l = label_new();
				move(r, ca);
				emit(n->op == P_ANDAND ? I_BZ : I_BNZ, -1, ca, -1, l);
				gen(n->b, &b);
				if (b.n != 1) hl_fail(n->line, "a scalar and a vector in && or ||");
				move(r, cvt(b.c[0], b.t.base, B_BOOL));
				label(l);
				vk(out, scalar_t(B_BOOL), r);
				return;
			}
			gen(n->b, &b);
			convert(&a, (type){ B_BOOL, a.t.shape, a.t.rows, a.t.cols, 0, 0 }, n->line);
			convert(&b, (type){ B_BOOL, b.t.shape, b.t.rows, b.t.cols, 0, 0 }, n->line);
			arith(n->op, &a, &b, out, n->line);
			return;
		}
		gen(n->a, &a);
		gen(n->b, &b);
		if (a.t.base == B_BOOL && b.t.base == B_BOOL && (n->op == '&' || n->op == '|' || n->op == '^')) {
			arith(n->op == '&' ? P_ANDAND : n->op == '|' ? P_OROR : P_NE, &a, &b, out, n->line);
			return;
		}
		arith(n->op, &a, &b, out, n->line);
		return;
	}
	case N_UN: {
		int i;

		gen(n->a, out);
		if (n->op == '+') return;
		if (n->op == '!') {
			type t = out->t;

			t.base = B_BOOL;
			convert(out, t, n->line);
			for (i = 0; i < out->n; i++) out->c[i] = iop(I_SEQZ, out->c[i], -1);
			return;
		}
		if (n->op == '~') {
			for (i = 0; i < out->n; i++) out->c[i] = iop(I_XOR, out->c[i], ki(-1));
			return;
		}
		if (out->t.base == B_BOOL) {
			type t = out->t;

			t.base = B_INT;
			convert(out, t, n->line);
		}
		for (i = 0; i < out->n; i++)
			out->c[i] = out->t.base == B_FLOAT ? fop(I_FNEG, out->c[i], -1) : iop(I_SUB, ki(0), out->c[i]);
		return;
	}
	case N_PRE: case N_POST: {
		val v, one, r;

		gen(n->a, &v);
		if (n->k == N_POST) {
			/* the value before, in registers of its own: the variable is about to change */
			int i;

			for (i = 0; i < v.n; i++)
				if (!is_k(v.c[i]) && hc->vr[v.c[i]].var) {
					int t = new_vreg(hc->vr[v.c[i]].cls, 0);

					move(t, v.c[i]);
					v.c[i] = t;
				}
		}
		vk(&one, scalar_t(v.t.base == B_FLOAT ? B_FLOAT : B_INT), v.t.base == B_FLOAT ? kf(1) : ki(1));
		arith(n->op == P_INC ? '+' : '-', &v, &one, &r, n->line);
		{
			val keep;

			copy_val(&keep, &v);
			assign_to(n->a, &r, n->line);
			if (n->k == N_PRE) copy_val(out, &r);
			else copy_val(out, &keep);
		}
		return;
	}
	case N_ASSIGN: {
		val v;

		gen(n->b, &v);
		if (n->op != '=') {
			static const int ops[] = { P_SHL, P_SHR, '+', '-', '*', '/', '%', '&', '|', '^' };
			val cur, r;

			gen(n->a, &cur);
			if (cur.t.shape == S_MAT && v.t.shape == S_SCALAR && n->op == P_MULEQ) { }
			arith(ops[n->op - P_SHLEQ], &cur, &v, &r, n->line);
			copy_val(&v, &r);
		}
		assign_to(n->a, &v, n->line);
		copy_val(out, &v);
		return;
	}
	case N_COND: {
		val c, a, b;
		int i;

		gen(n->a, &c);
		if (c.n == 1) {
			int cc = cvt(c.c[0], c.t.base, B_BOOL), l1, l2;
			val r;

			if (is_k(cc)) { gen(kiv(cc) ? n->b : n->c, out); return; }
			l1 = label_new(), l2 = label_new();
			emit(I_BZ, -1, cc, -1, l1);
			gen(n->b, &a);
			new_var(&r, a.t);
			store(&r, &a, n->line);
			jump(l2);
			label(l1);
			gen(n->c, &b);
			store(&r, &b, n->line);
			label(l2);
			copy_val(out, &r);
			return;
		}
		/* a vector condition: both made, each component chosen */
		gen(n->b, &a);
		gen(n->c, &b);
		{
			type t = common(&a, &b, promote(a.t.base, b.t.base), n->line);

			convert(&c, (type){ B_BOOL, t.shape, t.rows, t.cols, 0, 0 }, n->line);
			out->t = t, out->n = a.n;
			for (i = 0; i < a.n; i++) out->c[i] = fsel(c.c[i], a.c[i], b.c[i]);
		}
		return;
	}
	case N_CALL:
		gen_call(n, out);
		return;
	case N_CAST:
		gen(n->a, out);
		if (out->t.shape != S_SCALAR && n->ty.shape != S_STRUCT && hl_type_comps(n->ty) == out->n && !n->ty.arr && !out->t.arr) {
			int i;

			for (i = 0; i < out->n; i++) out->c[i] = cvt(out->c[i], out->t.base, n->ty.base);
			out->t = n->ty;
			return;
		}
		convert(out, n->ty, n->line);
		return;
	case N_MEMBER: {
		val v;
		int at[MAXC], k, c;
		type mt;

		gen(n->a, &v);
		c = member_of(v.t, n->name, at, &mt, n->line);
		out->t = mt, out->n = c;
		for (k = 0; k < c; k++) out->c[k] = v.c[at[k]];
		return;
	}
	case N_INDEX:
		gen_index(n, out);
		return;
	case N_COMMA: {
		val v;

		gen(n->a, &v);
		gen(n->b, out);
		return;
	}
	}
	hl_fail(n->line, "not an expression");
}

/* What a name, member, swizzle or constant index names: registers of variables */
static void lval(node *n, val *out)
{
	switch (n->k) {
	case N_ID: {
		sym *s = find_sym(n->name);
		int i;

		if (!s) hl_fail(n->line, "no variable %s", n->name);
		if (s->kind != 0) hl_fail(n->line, "%s cannot be written", n->name);
		for (i = 0; i < s->v->n; i++)
			if (is_k(s->v->c[i]) || !hc->vr[s->v->c[i]].var) hl_fail(n->line, "%s is a constant", n->name);
		copy_val(out, s->v);
		return;
	}
	case N_MEMBER: {
		val v;
		int at[MAXC], k, c, j;
		type mt;

		lval(n->a, &v);
		c = member_of(v.t, n->name, at, &mt, n->line);
		for (k = 0; k < c; k++)
			for (j = 0; j < k; j++)
				if (at[j] == at[k]) hl_fail(n->line, "a swizzle written twice: %s", n->name);
		out->t = mt, out->n = c;
		for (k = 0; k < c; k++) out->c[k] = v.c[at[k]];
		return;
	}
	case N_INDEX: {
		val v, idx;
		int size, count, k, i;
		type e;

		lval(n->a, &v);
		gen(n->b, &idx);
		convert(&idx, scalar_t(B_INT), n->line);
		e = element_of(v.t, &size, &count, n->line);
		if (!is_k(idx.c[0])) hl_fail(n->line, "an index that is not a constant here");
		k = kiv(idx.c[0]);
		if (k < 0 || k >= count) hl_fail(n->line, "index %d out of 0 to %d", k, count - 1);
		out->t = e, out->n = size;
		for (i = 0; i < size; i++) out->c[i] = v.c[k * size + i];
		return;
	}
	}
	hl_fail(n->line, "this cannot be assigned to");
}

static int index_is_dynamic(node *t)
{
	for (; t->k == N_MEMBER || t->k == N_INDEX; t = t->a)
		if (t->k == N_INDEX) {
			int at = hc->ncode;
			val idx;

			gen(t->b, &idx);
			hc->ncode = at;	/* (looked at only: what it made is dropped) */
			if (idx.n != 1 || !is_k(idx.c[0])) return 1;
		}
	return 0;
}

static void assign_to(node *target, val *v, int line)
{
	val dst;

	if (index_is_dynamic(target)) {
		/* a[i] = v: every element, each where i is its number */
		val base, idx;
		int size, count, k, i;
		type e;

		if (target->k != N_INDEX) hl_fail(line, "a member of an element of a variable index cannot be assigned to");
		lval(target->a, &base);
		gen(target->b, &idx);
		convert(&idx, scalar_t(B_INT), line);
		e = element_of(base.t, &size, &count, line);
		convert(v, e, line);
		for (k = 0; k < count; k++) {
			int l = label_new();

			emit(I_BZ, -1, iop(I_SEQ, idx.c[0], ki(k)), -1, l);
			for (i = 0; i < size; i++) move(base.c[k * size + i], v->c[i]);
			label(l);
		}
		return;
	}
	lval(target, &dst);
	store(&dst, v, line);
	copy_val(v, &dst);
}

/* ---- intrinsics ---- */

#define PI 3.14159265358979f

static int f_floor(int x, int rm)
{
	int i, d;

	if (is_k(x)) {
		float f = kfv(x);

		return kf(rm == 2 ? floorf(f) : rm == 3 ? ceilf(f) : rm == 1 ? truncf(f) : rintf(f));
	}
	i = new_vreg(C_X, 0);
	emit(I_FCVTWS, i, x, -1, 0);
	hc->code[hc->ncode - 1].rm = (uint8_t)rm;
	d = new_vreg(C_F, 0);
	hc->vr[d].def = emit(I_FCVTSW, d, i, -1, 0);
	return d;
}

static int poly(int x, const float *c, int n)
{
	/* c[0] + x (c[1] + x (c[2] + ...)) */
	int r = kf(c[n - 1]), k;

	for (k = n - 2; k >= 0; k--) r = fop(I_FADD, fop(I_FMUL, r, x), kf(c[k]));
	return r;
}

static int f_sin(int x, int cosine)
{
	static const float c[] = { 6.28318531f, -41.3417022f, 81.6052493f, -76.7058597f, 42.0586940f };
	int r, a, z2;

	if (is_k(x)) return kf(cosine ? cosf(kfv(x)) : sinf(kfv(x)));
	r = fop(I_FMUL, x, kf(1.0f / (2.0f * PI)));
	if (cosine) r = fop(I_FADD, r, kf(0.25f));
	r = fop(I_FSUB, r, f_floor(r, 0));	/* -0.5 to 0.5 turns */
	a = fop(I_FABS, r, -1);
	a = fop(I_FMIN, a, fop(I_FSUB, kf(0.5f), a));
	r = fop(I_FSGNJ, a, r);		/* -0.25 to 0.25: sin(2 pi r) */
	z2 = fop(I_FMUL, r, r);
	return fop(I_FMUL, r, poly(z2, c, 5));
}

static int f_exp2(int x)
{
	static const float c[] = { 1.0f, 0.693147181f, 0.240226507f, 0.0555041087f, 0.00961812911f, 0.00133335581f, 0.000154035304f };
	int i, f, p, e;

	if (is_k(x)) return kf(exp2f(kfv(x)));
	x = fop(I_FMIN, fop(I_FMAX, x, kf(-126.0f)), kf(126.0f));
	i = new_vreg(C_X, 0);
	emit(I_FCVTWS, i, x, -1, 0);
	hc->code[hc->ncode - 1].rm = 2;
	f = new_vreg(C_F, 0);
	hc->vr[f].def = emit(I_FCVTSW, f, i, -1, 0);
	f = fop(I_FSUB, x, f);
	p = poly(f, c, 7);
	e = iop(I_SHL, iop(I_ADD, i, ki(127)), ki(23));
	{
		int s = new_vreg(C_F, 0);

		hc->vr[s].def = emit(I_FMVWX, s, e, -1, 0);
		return fop(I_FMUL, p, s);
	}
}

static int f_log2(int x)
{
	static const float c[] = { 2.0f / 0.693147181f, 2.0f / (3.0f * 0.693147181f), 2.0f / (5.0f * 0.693147181f),
				   2.0f / (7.0f * 0.693147181f), 2.0f / (9.0f * 0.693147181f) };
	int b, e, m, s, s2, ef;

	if (is_k(x)) return kf(log2f(kfv(x)));
	b = new_vreg(C_X, 0);
	emit(I_FMVXW, b, reg(x), -1, 0);
	e = iop(I_ADD, iop(I_SHRU, b, ki(23)), ki(-127));
	m = iop(I_OR, iop(I_AND, b, ki(0x7fffff)), ki(0x3f800000));
	{
		int mf = new_vreg(C_F, 0);

		hc->vr[mf].def = emit(I_FMVWX, mf, m, -1, 0);
		m = mf;
	}
	/* log2(m) for m in [1, 2): 2 atanh(s) / ln 2, s = (m - 1) / (m + 1) */
	s = fop(I_FDIV, fop(I_FSUB, m, kf(1)), fop(I_FADD, m, kf(1)));
	s2 = fop(I_FMUL, s, s);
	ef = cvt(e, B_INT, B_FLOAT);
	return fop(I_FADD, ef, fop(I_FMUL, s, poly(s2, c, 5)));
}

/* atan of z in [0, 1] */
static int atan01(int z)
{
	static const float c[] = { 0.99997726f, -0.33262347f, 0.19354346f, -0.11643287f, 0.05265332f, -0.01172120f };

	return fop(I_FMUL, z, poly(fop(I_FMUL, z, z), c, 6));
}

static int f_atan2(int y, int x)
{
	int ax, ay, mx, mn, p;

	if (is_k(x) && is_k(y)) return kf(atan2f(kfv(y), kfv(x)));
	ax = fop(I_FABS, x, -1), ay = fop(I_FABS, y, -1);
	mx = fop(I_FMAX, ax, ay), mn = fop(I_FMIN, ax, ay);
	p = atan01(fop(I_FDIV, mn, fop(I_FMAX, mx, kf(1e-30f))));
	p = fsel(fcmp(I_FLT, ax, ay), fop(I_FSUB, kf(PI / 2), p), p);
	p = fsel(fcmp(I_FLT, x, kf(0)), fop(I_FSUB, kf(PI), p), p);
	return fop(I_FSGNJ, p, y);
}

static int f_atan(int x)
{
	int a, inv, p;

	if (is_k(x)) return kf(atanf(kfv(x)));
	a = fop(I_FABS, x, -1);
	inv = fcmp(I_FLT, kf(1), a);
	p = atan01(fsel(inv, fop(I_FDIV, kf(1), a), a));
	p = fsel(inv, fop(I_FSUB, kf(PI / 2), p), p);
	return fop(I_FSGNJ, p, x);
}

static int f_dot(const val *a, const val *b)
{
	int r = fop(I_FMUL, a->c[0], b->c[0]), i;

	for (i = 1; i < a->n; i++) r = fop(I_FADD, r, fop(I_FMUL, a->c[i], b->c[i]));
	return r;
}

/* the arguments made floats of one shape (a scalar goes to every component) */
static type float_args(val *a, int n, int line)
{
	int k, size = 1;
	type t;

	for (k = 0; k < n; k++)
		if (a[k].n > 1 && (size == 1 || a[k].n < size)) size = a[k].n;
	t = vec_t(B_FLOAT, size);
	for (k = 0; k < n; k++)
		if (a[k].t.shape == S_MAT && a[k].n == size) t = a[k].t, t.base = B_FLOAT;
	for (k = 0; k < n; k++) convert(&a[k], t, line);
	return t;
}

static int gen_intrinsic(node *n, val *a, int na, val *out)
{
	const char *f = n->name;
	int line = n->line, i;

#define IS(s) (!strcmp(f, s))
#define NEED(k) do { if (na != (k)) hl_fail(line, "%s takes %d arguments", f, k); } while (0)
	if (IS("abs") || IS("sign")) {
		NEED(1);
		if (a[0].t.base == B_FLOAT) {
			copy_val(out, &a[0]);
			for (i = 0; i < a[0].n; i++) {
				int x = a[0].c[i];

				out->c[i] = IS("abs") ? fop(I_FABS, x, -1) : iop(I_SUB, fcmp(I_FLT, kf(0), x), fcmp(I_FLT, x, kf(0)));
			}
			if (IS("sign")) out->t.base = B_INT;
			return 1;
		}
		copy_val(out, &a[0]);
		for (i = 0; i < a[0].n; i++) {
			int x = a[0].c[i], neg = iop(I_SLT, x, ki(0));

			out->c[i] = IS("abs") ? iop(I_SUB, iop(I_XOR, x, iop(I_SUB, ki(0), neg)), iop(I_SUB, ki(0), neg))
					      : iop(I_SUB, iop(I_SLT, ki(0), x), neg);
		}
		return 1;
	}
	if (IS("min") || IS("max")) {
		NEED(2);
		if (a[0].t.base != B_FLOAT && a[1].t.base != B_FLOAT) {
			type t = common(&a[0], &a[1], promote(a[0].t.base, a[1].t.base), line);

			out->t = t, out->n = a[0].n;
			for (i = 0; i < a[0].n; i++) {
				int lt = iop(t.base == B_UINT ? I_SLTU : I_SLT, a[0].c[i], a[1].c[i]), r, l;

				if (is_k(lt)) { out->c[i] = (kiv(lt) != 0) == IS("min") ? a[0].c[i] : a[1].c[i]; continue; }
				r = new_vreg(C_X, 1), l = label_new();
				move(r, a[0].c[i]);
				emit(IS("min") ? I_BNZ : I_BZ, -1, lt, -1, l);
				move(r, a[1].c[i]);
				label(l);
				out->c[i] = r;
			}
			return 1;
		}
		out->t = float_args(a, 2, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(IS("min") ? I_FMIN : I_FMAX, a[0].c[i], a[1].c[i]);
		return 1;
	}
	if (IS("clamp")) {
		NEED(3);
		out->t = float_args(a, 3, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(I_FMIN, fop(I_FMAX, a[0].c[i], a[1].c[i]), a[2].c[i]);
		return 1;
	}
	if (IS("saturate")) {
		NEED(1);
		out->t = float_args(a, 1, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(I_FMIN, fop(I_FMAX, a[0].c[i], kf(0)), kf(1));
		return 1;
	}
	if (IS("lerp") || IS("mix")) {
		NEED(3);
		out->t = float_args(a, 3, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(I_FADD, a[0].c[i], fop(I_FMUL, fop(I_FSUB, a[1].c[i], a[0].c[i]), a[2].c[i]));
		return 1;
	}
	if (IS("mad")) {
		NEED(3);
		out->t = float_args(a, 3, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(I_FADD, fop(I_FMUL, a[0].c[i], a[1].c[i]), a[2].c[i]);
		return 1;
	}
	if (IS("step")) {
		NEED(2);
		out->t = float_args(a, 2, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) out->c[i] = cvt(fcmp(I_FLE, a[0].c[i], a[1].c[i]), B_INT, B_FLOAT);
		return 1;
	}
	if (IS("smoothstep")) {
		NEED(3);
		out->t = float_args(a, 3, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) {
			int t = fop(I_FDIV, fop(I_FSUB, a[2].c[i], a[0].c[i]), fop(I_FSUB, a[1].c[i], a[0].c[i]));

			t = fop(I_FMIN, fop(I_FMAX, t, kf(0)), kf(1));
			out->c[i] = fop(I_FMUL, fop(I_FMUL, t, t), fop(I_FSUB, kf(3), fop(I_FMUL, kf(2), t)));
		}
		return 1;
	}
	if (IS("sqrt") || IS("rsqrt") || IS("rcp") || IS("floor") || IS("ceil") || IS("frac") || IS("fract") || IS("trunc") || IS("round") ||
	    IS("sin") || IS("cos") || IS("tan") || IS("exp") || IS("exp2") || IS("log") || IS("log2") || IS("log10") || IS("atan") ||
	    IS("asin") || IS("acos") || IS("degrees") || IS("radians") || IS("sinh") || IS("cosh") || IS("tanh")) {
		NEED(1);
		out->t = float_args(a, 1, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) {
			int x = a[0].c[i], r;

			if (IS("sqrt")) r = fop(I_FSQRT, x, -1);
			else if (IS("rsqrt")) r = fop(I_FDIV, kf(1), fop(I_FSQRT, x, -1));
			else if (IS("rcp")) r = fop(I_FDIV, kf(1), x);
			else if (IS("floor")) r = f_floor(x, 2);
			else if (IS("ceil")) r = f_floor(x, 3);
			else if (IS("trunc")) r = f_floor(x, 1);
			else if (IS("round")) r = f_floor(x, 0);
			else if (IS("frac") || IS("fract")) r = fop(I_FSUB, x, f_floor(x, 2));
			else if (IS("sin")) r = f_sin(x, 0);
			else if (IS("cos")) r = f_sin(x, 1);
			else if (IS("tan")) r = fop(I_FDIV, f_sin(x, 0), f_sin(x, 1));
			else if (IS("exp")) r = f_exp2(fop(I_FMUL, x, kf(1.44269504f)));
			else if (IS("exp2")) r = f_exp2(x);
			else if (IS("log")) r = fop(I_FMUL, f_log2(x), kf(0.693147181f));
			else if (IS("log2")) r = f_log2(x);
			else if (IS("log10")) r = fop(I_FMUL, f_log2(x), kf(0.301029996f));
			else if (IS("atan")) r = f_atan(x);
			else if (IS("asin")) r = f_atan2(x, fop(I_FSQRT, fop(I_FMAX, fop(I_FSUB, kf(1), fop(I_FMUL, x, x)), kf(0)), -1));
			else if (IS("acos")) r = f_atan2(fop(I_FSQRT, fop(I_FMAX, fop(I_FSUB, kf(1), fop(I_FMUL, x, x)), kf(0)), -1), x);
			else if (IS("degrees")) r = fop(I_FMUL, x, kf(180.0f / PI));
			else if (IS("radians")) r = fop(I_FMUL, x, kf(PI / 180.0f));
			else {
				/* sinh, cosh, tanh from e^x */
				int e = f_exp2(fop(I_FMUL, x, kf(1.44269504f))), ie = fop(I_FDIV, kf(1), e);

				if (IS("sinh")) r = fop(I_FMUL, fop(I_FSUB, e, ie), kf(0.5f));
				else if (IS("cosh")) r = fop(I_FMUL, fop(I_FADD, e, ie), kf(0.5f));
				else r = fop(I_FDIV, fop(I_FSUB, e, ie), fop(I_FADD, e, ie));
			}
			out->c[i] = r;
		}
		return 1;
	}
	if (IS("pow") || IS("fmod") || IS("atan2")) {
		NEED(2);
		out->t = float_args(a, 2, line), out->n = a[0].n;
		for (i = 0; i < a[0].n; i++) {
			int x = a[0].c[i], y = a[1].c[i];

			if (IS("pow")) {
				if (is_k(y) && kfv(y) == 2.0f) out->c[i] = fop(I_FMUL, x, x);
				else if (is_k(y) && kfv(y) == 1.0f) out->c[i] = x;
				else if (is_k(y) && kfv(y) == 0.5f) out->c[i] = fop(I_FSQRT, x, -1);
				else if (is_k(x) && is_k(y)) out->c[i] = kf(powf(kfv(x), kfv(y)));
				else out->c[i] = f_exp2(fop(I_FMUL, y, f_log2(x)));
			} else if (IS("fmod")) {
				int q = f_floor(fop(I_FDIV, x, y), 1);

				out->c[i] = fop(I_FSUB, x, fop(I_FMUL, y, q));
			} else
				out->c[i] = f_atan2(x, y);
		}
		return 1;
	}
	if (IS("dot")) {
		NEED(2);
		float_args(a, 2, line);
		vk(out, scalar_t(B_FLOAT), f_dot(&a[0], &a[1]));
		return 1;
	}
	if (IS("length")) {
		NEED(1);
		float_args(a, 1, line);
		vk(out, scalar_t(B_FLOAT), a[0].n == 1 ? fop(I_FABS, a[0].c[0], -1) : fop(I_FSQRT, f_dot(&a[0], &a[0]), -1));
		return 1;
	}
	if (IS("distance")) {
		val d;

		NEED(2);
		float_args(a, 2, line);
		arith('-', &a[0], &a[1], &d, line);
		vk(out, scalar_t(B_FLOAT), d.n == 1 ? fop(I_FABS, d.c[0], -1) : fop(I_FSQRT, f_dot(&d, &d), -1));
		return 1;
	}
	if (IS("normalize")) {
		int s;

		NEED(1);
		out->t = float_args(a, 1, line), out->n = a[0].n;
		s = fop(I_FDIV, kf(1), fop(I_FSQRT, f_dot(&a[0], &a[0]), -1));
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(I_FMUL, a[0].c[i], s);
		return 1;
	}
	if (IS("cross")) {
		NEED(2);
		convert(&a[0], vec_t(B_FLOAT, 3), line);
		convert(&a[1], vec_t(B_FLOAT, 3), line);
		out->t = vec_t(B_FLOAT, 3), out->n = 3;
		for (i = 0; i < 3; i++) {
			int j = (i + 1) % 3, k = (i + 2) % 3;

			out->c[i] = fop(I_FSUB, fop(I_FMUL, a[0].c[j], a[1].c[k]), fop(I_FMUL, a[0].c[k], a[1].c[j]));
		}
		return 1;
	}
	if (IS("reflect")) {
		int d;

		NEED(2);
		out->t = float_args(a, 2, line), out->n = a[0].n;
		d = fop(I_FMUL, kf(2), f_dot(&a[0], &a[1]));
		for (i = 0; i < a[0].n; i++) out->c[i] = fop(I_FSUB, a[0].c[i], fop(I_FMUL, d, a[1].c[i]));
		return 1;
	}
	if (IS("refract")) {
		int d, k, eta, s, ok;

		NEED(3);
		convert(&a[2], scalar_t(B_FLOAT), line);
		eta = a[2].c[0];
		out->t = float_args(a, 2, line), out->n = a[0].n;
		d = f_dot(&a[1], &a[0]);
		k = fop(I_FSUB, kf(1), fop(I_FMUL, fop(I_FMUL, eta, eta), fop(I_FSUB, kf(1), fop(I_FMUL, d, d))));
		ok = fcmp(I_FLE, kf(0), k);
		s = fop(I_FADD, fop(I_FMUL, eta, d), fop(I_FSQRT, fop(I_FMAX, k, kf(0)), -1));
		for (i = 0; i < a[0].n; i++)
			out->c[i] = fsel(ok, fop(I_FSUB, fop(I_FMUL, eta, a[0].c[i]), fop(I_FMUL, s, a[1].c[i])), kf(0));
		return 1;
	}
	if (IS("faceforward")) {
		int d;

		NEED(3);
		out->t = float_args(a, 3, line), out->n = a[0].n;
		d = fcmp(I_FLT, f_dot(&a[2], &a[1]), kf(0));
		for (i = 0; i < a[0].n; i++) out->c[i] = fsel(d, a[0].c[i], fop(I_FNEG, a[0].c[i], -1));
		return 1;
	}
	if (IS("any") || IS("all")) {
		int r;

		NEED(1);
		convert(&a[0], (type){ B_BOOL, a[0].t.shape, a[0].t.rows, a[0].t.cols, 0, 0 }, line);
		r = a[0].c[0];
		for (i = 1; i < a[0].n; i++) r = iop(IS("any") ? I_OR : I_AND, r, a[0].c[i]);
		vk(out, scalar_t(B_BOOL), r);
		return 1;
	}
	if (IS("transpose")) {
		int r, c;

		NEED(1);
		if (a[0].t.shape != S_MAT) hl_fail(line, "transpose of a matrix");
		out->t = mat_t(a[0].t.base, a[0].t.cols, a[0].t.rows), out->n = a[0].n;
		for (r = 0; r < a[0].t.rows; r++)
			for (c = 0; c < a[0].t.cols; c++) out->c[c * a[0].t.rows + r] = a[0].c[r * a[0].t.cols + c];
		return 1;
	}
	if (IS("mul")) {
		val *x = &a[0], *y = &a[1];
		int r, c, k;

		NEED(2);
		if (x->t.base != B_FLOAT) { type t = x->t; t.base = B_FLOAT; convert(x, t, line); }
		if (y->t.base != B_FLOAT) { type t = y->t; t.base = B_FLOAT; convert(y, t, line); }
		if (x->n == 1 || y->n == 1) { arith('*', x, y, out, line); return 1; }
		if (x->t.shape != S_MAT && y->t.shape != S_MAT) { vk(out, scalar_t(B_FLOAT), f_dot(x, y)); return 1; }
		if (x->t.shape != S_MAT) {
			/* a row vector times a matrix */
			if (x->n != y->t.rows) hl_fail(line, "mul of a %d vector and %d rows", x->n, y->t.rows);
			out->t = vec_t(B_FLOAT, y->t.cols), out->n = y->t.cols;
			for (c = 0; c < y->t.cols; c++) {
				int s = fop(I_FMUL, x->c[0], y->c[c]);

				for (k = 1; k < x->n; k++) s = fop(I_FADD, s, fop(I_FMUL, x->c[k], y->c[k * y->t.cols + c]));
				out->c[c] = s;
			}
			return 1;
		}
		if (y->t.shape != S_MAT) {
			if (y->n != x->t.cols) hl_fail(line, "mul of %d columns and a %d vector", x->t.cols, y->n);
			out->t = vec_t(B_FLOAT, x->t.rows), out->n = x->t.rows;
			for (r = 0; r < x->t.rows; r++) {
				int s = fop(I_FMUL, x->c[r * x->t.cols], y->c[0]);

				for (k = 1; k < y->n; k++) s = fop(I_FADD, s, fop(I_FMUL, x->c[r * x->t.cols + k], y->c[k]));
				out->c[r] = s;
			}
			return 1;
		}
		if (x->t.cols != y->t.rows) hl_fail(line, "mul of matrices that do not fit");
		out->t = mat_t(B_FLOAT, x->t.rows, y->t.cols), out->n = x->t.rows * y->t.cols;
		for (r = 0; r < x->t.rows; r++)
			for (c = 0; c < y->t.cols; c++) {
				int s = fop(I_FMUL, x->c[r * x->t.cols], y->c[c]);

				for (k = 1; k < x->t.cols; k++) s = fop(I_FADD, s, fop(I_FMUL, x->c[r * x->t.cols + k], y->c[k * y->t.cols + c]));
				out->c[r * y->t.cols + c] = s;
			}
		return 1;
	}
	if (IS("determinant")) {
		val *m = &a[0];
		int d;

		NEED(1);
		if (m->t.shape != S_MAT || m->t.rows != m->t.cols || m->t.rows > 3) hl_fail(line, "determinant of a 2x2 or 3x3 matrix");
		if (m->t.rows == 2) d = fop(I_FSUB, fop(I_FMUL, m->c[0], m->c[3]), fop(I_FMUL, m->c[1], m->c[2]));
		else {
			val r0, r1, r2, x;

			r0.t = r1.t = r2.t = vec_t(B_FLOAT, 3), r0.n = r1.n = r2.n = 3;
			for (i = 0; i < 3; i++) r0.c[i] = m->c[i], r1.c[i] = m->c[3 + i], r2.c[i] = m->c[6 + i];
			x.t = vec_t(B_FLOAT, 3), x.n = 3;
			for (i = 0; i < 3; i++) {
				int j = (i + 1) % 3, k = (i + 2) % 3;

				x.c[i] = fop(I_FSUB, fop(I_FMUL, r1.c[j], r2.c[k]), fop(I_FMUL, r1.c[k], r2.c[j]));
			}
			d = f_dot(&r0, &x);
		}
		vk(out, scalar_t(B_FLOAT), d);
		return 1;
	}
	if (IS("asfloat") || IS("asint") || IS("asuint")) {
		int to = IS("asfloat") ? B_FLOAT : IS("asint") ? B_INT : B_UINT;

		NEED(1);
		copy_val(out, &a[0]);
		out->t.base = (uint8_t)to;
		for (i = 0; i < a[0].n; i++) {
			int x = a[0].c[i], d;

			if ((a[0].t.base == B_FLOAT) == (to == B_FLOAT)) continue;
			if (is_k(x)) { out->c[i] = konst_of(to == B_FLOAT ? C_F : C_X, kbits(x)); continue; }
			d = new_vreg(to == B_FLOAT ? C_F : C_X, 0);
			hc->vr[d].def = emit(to == B_FLOAT ? I_FMVWX : I_FMVXW, d, x, -1, 0);
			out->c[i] = d;
		}
		return 1;
	}
#undef IS
#undef NEED
	return 0;
}

/* ---- functions, inlined at every call ---- */

static int score(const func *f, const val *args, int nargs)
{
	int s = 0, k;

	for (k = 0; k < nargs; k++) {
		type p = f->params[k].ty, t = args[k].t;

		if (hl_type_eq(p, t)) s += 3;
		else if (hl_type_comps(p) == args[k].n && p.shape == t.shape) s += 2;
		else if (args[k].n == 1 || hl_type_comps(p) <= args[k].n) s += 1;
		else return -1;
	}
	return s;
}

static func *find_func(const char *name, int nargs, val *args)
{
	func *f, *best = 0;
	int bs = -1;

	for (f = hc->funcs; f; f = f->next)
		if (f->name == name && f->nparams == nargs) {
			int s = score(f, args, nargs);

			if (s > bs) bs = s, best = f;
		}
	if (!best) hl_fail(hc->line, "no %s for these arguments", name);
	return best;
}

static int count_returns(node *n)
{
	int c = 0;

	for (; n; n = n->next) c += (n->k == N_RETURN) + count_returns(n->a) + count_returns(n->b) + count_returns(n->c) + count_returns(n->d);
	return c;
}

static void call_user(func *f, node *call, val *args, int nargs, val *out)
{
	int scope = hc->nsyms, k, floor;
	val **pv = hl_alloc(sizeof(val *) * (nargs ? nargs : 1));
	inline_ctx *ic;
	node *last, *a;

	if (f->active) hl_fail(call->line, "%s calls itself: recursion is not supported", f->name);
	if (hc->ninl == 64) hl_fail(call->line, "calls too deep");
	if (f->ret.base != B_VOID && f->ret.shape == S_STRUCT && hl_type_comps(f->ret) > MAXC) hl_fail(call->line, "a return value too large");
	for (k = 0; k < nargs; k++) {
		param *p = &f->params[k];
		val v;

		if (p->flags & Q_OUT) {
			new_var(&v, p->ty);
			if (p->flags & Q_IN) store(&v, &args[k], call->line);
		} else {
			copy_val(&v, &args[k]);
			convert(&v, p->ty, call->line);
			if ((f->written >> k) & 1 || !bindable(&v)) {
				/* written in the body, or a variable's registers: copied */
				val c;

				if (!((f->written >> k) & 1)) {
					int i, ok = 1;

					/* a caller's variable not written in the body needs no copy (unless the body writes a static) */
					for (i = 0; i < v.n; i++)
						if (!is_k(v.c[i]) && hc->vr[v.c[i]].var == 2) ok = 0;
					if (ok) { pv[k] = heap_val(&v); continue; }
				}
				new_var(&c, p->ty);
				store(&c, &v, call->line);
				copy_val(&v, &c);
			}
		}
		pv[k] = heap_val(&v);
	}
	floor = hc->sym_floor;
	hc->sym_floor = hc->nsyms;
	for (k = 0; k < nargs; k++) push_sym(f->params[k].name, 0, pv[k], 0);
	ic = &hc->inl[hc->ninl++];
	memset(ic, 0, sizeof *ic);
	ic->rt = f->ret;
	ic->end = label_new();
	for (last = f->body->a; last && last->next; last = last->next) {}
	ic->direct = last && last->k == N_RETURN && count_returns(f->body->a) == 1;
	if (f->ret.base != B_VOID && !ic->direct) {
		val r;

		new_var(&r, f->ret);
		ic->ret = heap_val(&r);
	}
	f->active = 1;
	hc->depth++;
	gen_stmts(f->body->a);
	hc->depth--;
	f->active = 0;
	label(ic->end);
	if (f->ret.base == B_VOID) out->t = f->ret, out->n = 0;
	else if (ic->direct) copy_val(out, ic->direct_v);
	else copy_val(out, ic->ret);
	hc->ninl--;
	hc->nsyms = scope;
	hc->sym_floor = floor;
	/* out parameters written back */
	for (k = 0, a = call->a; k < nargs; k++, a = a->next)
		if (f->params[k].flags & Q_OUT) {
			val v;

			copy_val(&v, pv[k]);
			assign_to(a, &v, call->line);
		}
}

/* ---- statements ---- */

static int no_bind;	/* a for loop's own variables: its body is not in what follows them */

static void declare(node *d, node *rest)
{
	val v, init;
	int bind = 0;

	hc->line = d->line;
	if (d->ty.base == B_VOID) hl_fail(d->line, "a void variable");
	if (d->a) {
		if (d->a->k == N_LIST) list_values(d->a, d->ty, &init, d->line);
		else {
			gen(d->a, &init);
			convert(&init, d->ty, d->line);
		}
		/* a value never written again is the value itself: no registers of its own */
		if (bindable(&init) && !hl_assigns(rest, d->name) && !no_bind) bind = 1;
		if ((d->flags & Q_CONST) && hl_assigns(rest, d->name)) hl_fail(d->line, "constant %s is assigned to", d->name);
	}
	if (bind) {
		push_sym(d->name, 0, heap_val(&init), 0);
		return;
	}
	new_var(&v, d->ty);
	if (d->a) store(&v, &init, d->line);
	push_sym(d->name, 0, heap_val(&v), 0);
}

static int body_cost(node *n)
{
	int c = 0;

	for (; n; n = n->next) {
		c++;
		if (n->k == N_CALL) {
			func *f;

			for (f = hc->funcs; f; f = f->next)
				if (f->name == n->name) { c += f->cost; break; }
			if (!f && (!strcmp(n->name, "sin") || !strcmp(n->name, "cos") || !strcmp(n->name, "pow") || !strcmp(n->name, "exp") ||
				   !strcmp(n->name, "log") || !strcmp(n->name, "atan2") || !strcmp(n->name, "normalize")))
				c += 10;
		}
		c += body_cost(n->a) + body_cost(n->b) + body_cost(n->c) + body_cost(n->d);
	}
	return c;
}

/* A for loop's count, if it is known: `for (int i = A; i < B; i++)` and the like */
static int known_count(node *n, int32_t *from, int32_t *by)
{
	node *init = n->a, *c = n->b, *s = n->c;
	val v;
	int32_t lim, i, k = 0;
	int at = hc->ncode, nv = hc->nvr;

	if (!init || init->k != N_DECL || init->next || !init->a || init->ty.shape != S_SCALAR || init->ty.arr) return -1;
	if (init->ty.base != B_INT && init->ty.base != B_UINT) return -1;
	if (!c || c->k != N_BIN || c->a->k != N_ID || c->a->name != init->name) return -1;
	if (!s) return -1;
	if ((s->k == N_POST || s->k == N_PRE) && s->a->k == N_ID && s->a->name == init->name) *by = s->op == P_INC ? 1 : -1;
	else if (s->k == N_ASSIGN && (s->op == P_ADDEQ || s->op == P_SUBEQ) && s->a->k == N_ID && s->a->name == init->name) {
		gen(s->b, &v);
		if (v.n != 1 || !is_k(v.c[0]) || v.t.base == B_FLOAT) { hc->ncode = at, hc->nvr = nv; return -1; }
		*by = s->op == P_ADDEQ ? kiv(v.c[0]) : -kiv(v.c[0]);
	} else
		return -1;
	gen(init->a, &v);
	if (v.n != 1 || !is_k(v.c[0]) || v.t.base == B_FLOAT) { hc->ncode = at, hc->nvr = nv; return -1; }
	*from = kiv(v.c[0]);
	gen(c->b, &v);
	if (v.n != 1 || !is_k(v.c[0])) { hc->ncode = at, hc->nvr = nv; return -1; }
	lim = v.t.base == B_FLOAT ? (int32_t)kfv(v.c[0]) : kiv(v.c[0]);
	hc->ncode = at, hc->nvr = nv;
	if (hl_assigns(n->d, init->name)) return -1;
	for (i = *from; k <= 1024; i += *by, k++) {
		int go;

		switch (c->op) {
		case '<': go = i < lim; break;
		case P_LE: go = i <= lim; break;
		case '>': go = i > lim; break;
		case P_GE: go = i >= lim; break;
		case P_NE: go = i != lim; break;
		default: return -1;
		}
		if (!go) return k;
	}
	return -1;
}

static void loop_push(int brk, int cont)
{
	if (hc->nloops == 64) hl_fail(hc->line, "loops too deep");
	hc->loops[hc->nloops].brk = brk, hc->loops[hc->nloops].cont = cont;
	hc->nloops++;
}

static void yield_here(void) { emit(I_YIELD, -1, -1, -1, hc->yields++); }

static void gen_for(node *n)
{
	int scope = hc->nsyms, count, test, body, cont, exit_;
	int32_t from = 0, by = 1;

	count = known_count(n, &from, &by);
	if (count >= 0 && !(n->flags & A_LOOP) &&
	    ((n->flags & A_UNROLL) || (count <= 32 && count * body_cost(n->d) <= 640))) {
		/* unrolled: each turn the body with the counter a constant */
		int k;
		val iv;

		exit_ = label_new();
		for (k = 0; k < count; k++) {
			int s2 = hc->nsyms;

			vk(&iv, scalar_t(n->a->ty.base), ki(from + k * by));
			push_sym(n->a->name, 0, heap_val(&iv), 0);
			cont = label_new();
			loop_push(exit_, cont);
			gen_stmts(n->d);
			hc->nloops--;
			label(cont);
			hc->nsyms = s2;
		}
		label(exit_);
		return;
	}
	if (n->a) {
		no_bind = 1;
		gen_stmts(n->a);
		no_bind = 0;
	}
	test = label_new(), body = label_new(), cont = label_new(), exit_ = label_new();
	jump(test);
	label(body);
	loop_push(exit_, cont);
	gen_stmts(n->d);
	hc->nloops--;
	label(cont);
	if (n->c) { val v; gen(n->c, &v); }
	yield_here();
	label(test);
	if (n->b) branch_true(n->b, body);
	else jump(body);
	label(exit_);
	hc->nsyms = scope;
}

static void gen_stmt(node *n, node *rest)
{
	hc->line = n->line;
	switch (n->k) {
	case N_EMPTY:
		return;
	case N_BLOCK: {
		int scope = hc->nsyms;

		gen_stmts(n->a);
		hc->nsyms = scope;
		return;
	}
	case N_DECL:
		declare(n, rest);
		return;
	case N_EXPR: {
		val v;

		gen(n->a, &v);
		return;
	}
	case N_IF: {
		int scope = hc->nsyms, l1, l2;
		val c;
		int at = hc->ncode;

		/* a constant condition: only its side */
		gen(n->a, &c);
		if (c.n == 1 && is_k(c.c[0])) {
			int yes = (int)(kbits(cvt(c.c[0], c.t.base, B_BOOL)) != 0);

			if (yes) gen_stmt(n->b, 0);
			else if (n->c) gen_stmt(n->c, 0);
			hc->nsyms = scope;
			return;
		}
		hc->ncode = at;
		l1 = label_new();
		branch_false(n->a, l1);
		gen_stmt(n->b, 0);
		hc->nsyms = scope;
		if (n->c) {
			l2 = label_new();
			jump(l2);
			label(l1);
			gen_stmt(n->c, 0);
			hc->nsyms = scope;
			label(l2);
		} else
			label(l1);
		return;
	}
	case N_FOR:
		gen_for(n);
		return;
	case N_WHILE: {
		int test = label_new(), body = label_new(), exit_ = label_new(), scope = hc->nsyms;

		jump(test);
		label(body);
		loop_push(exit_, test);
		gen_stmt(n->d, 0);
		hc->nloops--;
		hc->nsyms = scope;
		yield_here();
		label(test);
		branch_true(n->a, body);
		label(exit_);
		return;
	}
	case N_DO: {
		int body = label_new(), cont = label_new(), exit_ = label_new(), scope = hc->nsyms;

		label(body);
		loop_push(exit_, cont);
		gen_stmt(n->d, 0);
		hc->nloops--;
		hc->nsyms = scope;
		label(cont);
		yield_here();
		branch_true(n->a, body);
		label(exit_);
		return;
	}
	case N_BREAK:
		if (!hc->nloops) hl_fail(n->line, "break outside a loop");
		jump(hc->loops[hc->nloops - 1].brk);
		return;
	case N_CONTINUE:
		if (!hc->nloops) hl_fail(n->line, "continue outside a loop");
		jump(hc->loops[hc->nloops - 1].cont);
		return;
	case N_RETURN: {
		inline_ctx *ic = &hc->inl[hc->ninl - 1];
		val v;

		if (!n->a) {
			if (ic->rt.base != B_VOID) hl_fail(n->line, "return without a value");
			jump(ic->end);
			return;
		}
		gen(n->a, &v);
		if (ic->rt.base == B_VOID) hl_fail(n->line, "a void function returns a value");
		convert(&v, ic->rt, n->line);
		if (ic->direct) {
			ic->direct_v = heap_val(&v);
			return;
		}
		store(ic->ret, &v, n->line);
		jump(ic->end);
		return;
	}
	case N_DISCARD:
		hl_fail(n->line, "discard is not supported: return a colour");
	}
	hl_fail(n->line, "not a statement");
}

static void gen_stmts(node *n)
{
	for (; n; n = n->next) gen_stmt(n, n->next);
}

/* ---- the entry: every pixel of the index ---- */

void hl_gen(const char *entry)
{
	func *f, *main_f = 0;
	global *g;
	int pixels = (hc->flags & HLSL_RAW) ? 1 : 4, top, P, idx, w, qx, qy, x, y;
	val pos, colour;
	node call;

	new_vreg(C_X, 0);	/* 0: x0 */
	for (f = hc->funcs; f; f = f->next)
		if (!strcmp(f->name, entry)) main_f = f;
	if (!main_f) hl_fail(1, "no function %s", entry);
	for (g = hc->globals; g; g = g->next) push_sym(g->name, (g->flags & Q_UNIFORM) ? 1 : 2, 0, g);

	P = new_vreg(C_X, 1);
	emit(I_ICONST, P, -1, -1, 0);
	hc->pix_vreg = P;
	top = label_new();
	label(top);
	yield_here();
	hc->hoist_at = hc->ncode;
	{
		/* the pixel's centre: x from the index and P, y from the index */
		idx = new_vreg(C_X, 0);
		emit(I_INDEX, idx, -1, -1, 0);
		w = new_vreg(C_X, 0);
		emit(I_LDU, w, -1, -1, 8);
		qx = iop(I_REMU, idx, w);
		qy = iop(I_DIVU, idx, w);
		x = cvt(iop(I_ADD, pixels == 4 ? iop(I_SHL, qx, ki(2)) : qx, P), B_INT, B_FLOAT);
		y = cvt(qy, B_INT, B_FLOAT);
		pos.t = vec_t(B_FLOAT, 4), pos.n = 4;
		pos.c[0] = fop(I_FADD, x, kf(0.5f)), pos.c[1] = fop(I_FADD, y, kf(0.5f)), pos.c[2] = kf(0), pos.c[3] = kf(1);
	}
	{
		/* the static globals that are not constants: variables of every pixel */
		int scope_mark = hc->nsyms;

		(void)scope_mark;
		for (g = hc->globals; g; g = g->next)
			if (!(g->flags & Q_UNIFORM) && !(g->flags & Q_CONST)) {
				val v, init;
				int i;

				new_var(&v, g->ty);
				for (i = 0; i < v.n; i++) hc->vr[v.c[i]].var = 2;
				if (g->init) {
					if (g->init->k == N_LIST) list_values(g->init, g->ty, &init, g->line);
					else { gen(g->init, &init); convert(&init, g->ty, g->line); }
					store(&v, &init, g->line);
				}
				push_sym(g->name, 0, heap_val(&v), 0);
			}
	}
	hc->nglobal_syms = hc->nsyms;
	hc->sym_floor = hc->nsyms;
	memset(&call, 0, sizeof call);
	call.k = N_CALL, call.name = main_f->name, call.line = main_f->line;
	{
		val args[1];

		if (main_f->nparams > 1) hl_fail(main_f->line, "%s takes the pixel's position only", entry);
		if (main_f->nparams == 1) {
			node arg;

			memset(&arg, 0, sizeof arg);
			arg.k = N_ID, arg.name = hl_intern("<pos>", 5);
			push_sym(arg.name, 0, heap_val(&pos), 0);
			call.a = &arg;
			copy_val(&args[0], &pos);
			call_user(main_f, &call, args, 1, &colour);
		} else
			call_user(main_f, &call, args, 0, &colour);
	}
	if (colour.t.base == B_VOID || colour.n < 1 || colour.n > 4 || colour.t.shape == S_STRUCT)
		hl_fail(main_f->line, "%s must return a colour: float4, float3 or float", entry);
	{
		type t = colour.t;

		t.base = B_FLOAT;
		convert(&colour, t, main_f->line);
	}
	if (hc->flags & HLSL_RAW) {
		int i;

		for (i = 0; i < 4; i++) emit(I_PIXF, -1, reg(i < colour.n ? colour.c[i] : kf(0)), -1, i);
		emit(I_PIX, -1, P, ZERO, 1);
	} else {
		int i, word = ki(0);

		for (i = 0; i < 3; i++) {
			int c = colour.c[colour.n == 1 ? 0 : i < colour.n ? i : 0], b;

			if (colour.n == 2 && i == 2) c = kf(0);
			c = fop(I_FMIN, fop(I_FMAX, c, kf(0)), kf(1));
			b = new_vreg(C_X, 0);
			emit(I_FCVTWUS, b, reg(fop(I_FMUL, c, kf(255.0f))), -1, 0);
			hc->code[hc->ncode - 1].rm = 0;
			word = iop(I_OR, word, i == 2 ? b : iop(I_SHL, b, ki(16 - 8 * i)));
		}
		emit(I_PIX, -1, P, reg(word), 0);
	}
	{
		int np = iop(I_ADD, P, ki(1));

		move(P, np);
	}
	emit(I_BNZ, -1, iop(I_SLT, P, ki(pixels)), -1, top);
	emit(I_RET, -1, -1, -1, 0);
}
