/* The IR to RISC-V (RV32IMF): moves folded away, dead code dropped, compares fused with their
 * branches, registers by linear scan over live intervals, and at every yield the values live
 * there saved to the index's output and loaded back when the next round goes on. */
#include <stdio.h>
#include <stdlib.h>
#include "hlsl_int.h"

/* registers: x5-x7 and f0-f2 are the back end's own; a0 the index's output, a1 the block,
 * a3 the index, a4 the constants (+ 2048) */
enum { X0 = 0, RA = 1, SP = 2, T0 = 5, T1 = 6, T2 = 7, OUT = 10, CTX = 11, A2 = 12, IDX = 13, POOLR = 14 };
static const uint8_t x_order[] = { 12, 15, 16, 17, 28, 29, 30, 31, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };
static const uint8_t f_order[] = { 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27 };
#define F(r) (32 + (r))
#define MARGIN 64	/* after a step returns, the library's own instructions in the pass */

static int new_vreg_back(int cls)
{
	vreg *v;

	if (hc->nvr == hc->vr_cap) {
		hc->vr = hl_grow(hc->vr, hc->vr_cap * sizeof(vreg), (hc->vr_cap * 2 + 256) * sizeof(vreg));
		hc->vr_cap = hc->vr_cap * 2 + 256;
	}
	v = &hc->vr[hc->nvr];
	memset(v, 0, sizeof *v);
	v->cls = (uint8_t)cls, v->def = -1, v->block = -2;
	return hc->nvr++;
}

static int writes(int op)
{
	switch (op) {
	case I_NOP: case I_LABEL: case I_JMP: case I_BNZ: case I_BZ: case I_YIELD: case I_PIX: case I_PIXF: case I_RET:
	case I_BEQ: case I_BNE: case I_BLT: case I_BGE: case I_BLTU: case I_BGEU:
		return 0;
	}
	return 1;
}

static int is_branch(int op) { return op == I_BNZ || op == I_BZ || (op >= I_BEQ && op <= I_BGEU); }
static int ends_block(int op) { return op == I_JMP || is_branch(op) || op == I_RET || op == I_YIELD; }

static void count_uses(void)
{
	int i;

	for (i = 0; i < hc->nvr; i++) hc->vr[i].uses = 0, hc->vr[i].defs = 0, hc->vr[i].def = -1;
	for (i = 0; i < hc->ncode; i++) {
		ins *c = &hc->code[i];

		if (c->op == I_NOP) continue;
		if (c->a > 0) hc->vr[c->a].uses++;
		if (c->b > 0) hc->vr[c->b].uses++;
		if (writes(c->op) && c->d > 0) {
			if (hc->vr[c->d].defs < 255) hc->vr[c->d].defs++;
			hc->vr[c->d].def = i;
		}
	}
}

static int reads(const ins *c, int v) { return c->a == v || c->b == v; }

/* t = ...; v = t  ->  v = ... */
static void coalesce(void)
{
	int i;

	count_uses();
	for (i = 0; i < hc->ncode; i++) {
		ins *c = &hc->code[i];
		int s = c->a, d = c->d, j, k, ok = 1;

		if ((c->op != I_MOV && c->op != I_FMOV) || s <= 0) continue;
		if (hc->vr[s].var || hc->vr[s].defs != 1 || hc->vr[s].uses != 1) continue;
		j = hc->vr[s].def;
		if (j < 0 || j >= i || i - j > 64) continue;
		for (k = j + 1; k < i && ok; k++) {
			ins *m = &hc->code[k];

			if (m->op == I_NOP) continue;
			if (m->op == I_LABEL || ends_block(m->op) || reads(m, d) || (writes(m->op) && m->d == d)) ok = 0;
		}
		if (!ok || reads(&hc->code[j], d)) continue;
		hc->code[j].d = d;
		c->op = I_NOP;
		hc->vr[s].uses = 0;
	}
}

static void dead_code(void)
{
	int *work, nw = 0, i, *first, *next;

	count_uses();
	work = hl_alloc(sizeof(int) * (hc->ncode + 1));
	/* each register's definitions, chained */
	first = hl_alloc(sizeof(int) * hc->nvr), next = hl_alloc(sizeof(int) * (hc->ncode + 1));
	for (i = 0; i < hc->nvr; i++) first[i] = -1;
	for (i = hc->ncode - 1; i >= 0; i--)
		if (hc->code[i].op != I_NOP && writes(hc->code[i].op) && hc->code[i].d > 0) next[i] = first[hc->code[i].d], first[hc->code[i].d] = i;
	for (i = 0; i < hc->ncode; i++)
		if (writes(hc->code[i].op) && hc->code[i].d > 0 && hc->vr[hc->code[i].d].uses == 0) work[nw++] = i;
	while (nw) {
		ins *c = &hc->code[work[--nw]];
		int k, src[2];

		if (c->op == I_NOP) continue;
		src[0] = c->a, src[1] = c->b;
		c->op = I_NOP;
		for (k = 0; k < 2; k++)
			if (src[k] > 0 && --hc->vr[src[k]].uses == 0) {
				int j;

				/* every definition of it is dead now */
				for (j = first[src[k]]; j >= 0; j = next[j])
					if (hc->code[j].op != I_NOP && hc->code[j].d == src[k]) work[nw++] = j;
			}
	}
}

static void compact(void)
{
	int i, n = 0;

	for (i = 0; i < hc->ncode; i++)
		if (hc->code[i].op != I_NOP) hc->code[n++] = hc->code[i];
	hc->ncode = n;
}

/* slt t, a, b; bnez t, L  ->  blt a, b, L */
static void fuse_branches(void)
{
	int i;

	count_uses();
	for (i = 1; i < hc->ncode; i++) {
		ins *b = &hc->code[i], *c = &hc->code[i - 1];
		int nz, op = -1;

		if (b->op != I_BNZ && b->op != I_BZ) continue;
		if (c->d != b->a || !writes(c->op) || hc->vr[b->a].uses != 1 || hc->vr[b->a].var || hc->vr[b->a].defs != 1) continue;
		nz = b->op == I_BNZ;
		switch (c->op) {
		case I_SLT: if (c->b >= 0) op = nz ? I_BLT : I_BGE; break;
		case I_SLTU: if (c->b >= 0) op = nz ? I_BLTU : I_BGEU; break;
		case I_SEQ: if (c->b >= 0) op = nz ? I_BEQ : I_BNE; break;
		case I_SNE: if (c->b >= 0) op = nz ? I_BNE : I_BEQ; break;
		case I_SEQZ: op = nz ? I_BEQ : I_BNE; c->b = 0; break;
		}
		if (op < 0) continue;
		b->op = (uint8_t)op, b->a = c->a, b->b = c->b;
		c->op = I_NOP;
	}
	/* a jump to the next instruction */
	for (i = 0; i < hc->ncode; i++)
		if (hc->code[i].op == I_JMP) {
			int k;

			for (k = i + 1; k < hc->ncode && (hc->code[k].op == I_LABEL || hc->code[k].op == I_NOP); k++)
				if (hc->code[k].op == I_LABEL && hc->code[k].imm == hc->code[i].imm) { hc->code[i].op = I_NOP; break; }
		}
}

/* Every constant and uniform loaded once a pixel, where the pixel starts: the register allocator
 * loads one again where it is used when it runs out of registers (REMAT). */
#define REMAT (-30000)

static void hoist_loads(void)
{
	int i, n = 0, k, *canon;
	ins *keys = hl_alloc(sizeof(ins) * 1024), *grown;

	canon = hl_alloc(sizeof(int) * 1024);
	count_uses();
	for (i = hc->hoist_at; i < hc->ncode; i++) {
		ins *c = &hc->code[i];
		int v;

		if (c->op != I_FCONST && c->op != I_LDU) continue;
		for (k = 0; k < n; k++)
			if (keys[k].op == c->op && keys[k].imm == c->imm && hc->vr[canon[k]].cls == hc->vr[c->d].cls) break;
		if (k == n) {
			if (n == 1024) continue;
			keys[n] = *c;
			canon[n] = new_vreg_back(hc->vr[c->d].cls);
			keys[n].d = canon[n];
			n++;
		}
		v = c->d;
		if (hc->vr[v].var || hc->vr[v].defs != 1) {
			/* into a variable: a move from the one load */
			c->op = (uint8_t)(hc->vr[v].cls == C_X ? I_MOV : I_FMOV), c->a = canon[k], c->b = -1, c->imm = 0;
			continue;
		}
		c->op = I_NOP;
		hc->vr[v].uses = -1;	/* (its uses are the load's now) */
		hc->vr[v].def = canon[k];
	}
	if (!n) return;
	for (i = 0; i < hc->ncode; i++) {
		ins *c = &hc->code[i];

		if (c->a > 0 && hc->vr[c->a].uses == -1) c->a = hc->vr[c->a].def;
		if (c->b > 0 && hc->vr[c->b].uses == -1) c->b = hc->vr[c->b].def;
	}
	grown = hl_alloc(sizeof(ins) * (hc->ncode + n + 1));
	memcpy(grown, hc->code, sizeof(ins) * hc->hoist_at);
	memcpy(grown + hc->hoist_at, keys, sizeof(ins) * n);
	memcpy(grown + hc->hoist_at + n, hc->code + hc->hoist_at, sizeof(ins) * (hc->ncode - hc->hoist_at));
	hc->code = grown, hc->ncode += n, hc->code_cap = hc->ncode + 1;
}

/* ---- blocks and liveness ---- */

typedef struct { int start, end, succ[2], nsucc; } block;
static block *blocks;
static int nblocks, *block_of, nglob, *glob_of, *glob_v, words;
static uint32_t *live_in, *live_out;
static int liveness_rounds;

#define BIT(s, g) ((s)[(g) >> 5] >> ((g) & 31) & 1)

static void liveness(void)
{
	int i, b, changed;
	uint32_t *use, *def;

	hc->label_at = hl_alloc(sizeof(int) * (hc->labels + 1));
	for (i = 0; i < hc->labels; i++) hc->label_at[i] = -1;
	for (i = 0; i < hc->ncode; i++)
		if (hc->code[i].op == I_LABEL) hc->label_at[hc->code[i].imm] = i;
	block_of = hl_alloc(sizeof(int) * (hc->ncode + 1));
	blocks = hl_alloc(sizeof(block) * (hc->ncode + 1));
	nblocks = 0;
	for (i = 0; i < hc->ncode; i++) {
		if (i == 0 || hc->code[i].op == I_LABEL || ends_block(hc->code[i - 1].op)) {
			if (nblocks) blocks[nblocks - 1].end = i;
			blocks[nblocks++].start = i;
		}
		block_of[i] = nblocks - 1;
	}
	blocks[nblocks - 1].end = hc->ncode;
	for (b = 0; b < nblocks; b++) {
		ins *last = &hc->code[blocks[b].end - 1];

		blocks[b].nsucc = 0;
		if (last->op == I_JMP || is_branch(last->op)) {
			int at = hc->label_at[last->imm];

			if (at < 0) hl_fail(0, "internal: a label never placed");
			blocks[b].succ[blocks[b].nsucc++] = block_of[at];
		}
		if (last->op != I_JMP && last->op != I_RET && b + 1 < nblocks) blocks[b].succ[blocks[b].nsucc++] = b + 1;
	}
	/* registers used in more than one block take part in the sets */
	for (i = 0; i < hc->nvr; i++) hc->vr[i].block = -2;
	for (i = 0; i < hc->ncode; i++) {
		ins *c = &hc->code[i];
		int r[3], k;

		r[0] = writes(c->op) ? c->d : -1, r[1] = c->a, r[2] = c->b;
		for (k = 0; k < 3; k++)
			if (r[k] > 0) {
				vreg *v = &hc->vr[r[k]];

				if (v->block == -2) v->block = block_of[i];
				else if (v->block != block_of[i]) v->block = -1;
			}
	}
	glob_of = hl_alloc(sizeof(int) * hc->nvr);
	glob_v = hl_alloc(sizeof(int) * hc->nvr);
	nglob = 0;
	for (i = 1; i < hc->nvr; i++) {
		glob_of[i] = -1;
		/* (a variable defined in a block and read before it there in a loop is live round the loop) */
		if (hc->vr[i].block == -1 || (hc->vr[i].var && hc->vr[i].block >= 0)) glob_v[nglob] = i, glob_of[i] = nglob++;
	}
	words = (nglob + 31) / 32 + 1;
	live_in = hl_alloc(sizeof(uint32_t) * words * nblocks);
	live_out = hl_alloc(sizeof(uint32_t) * words * nblocks);
	use = hl_alloc(sizeof(uint32_t) * words * nblocks);
	def = hl_alloc(sizeof(uint32_t) * words * nblocks);
	for (b = 0; b < nblocks; b++)
		for (i = blocks[b].start; i < blocks[b].end; i++) {
			ins *c = &hc->code[i];
			uint32_t *u = use + b * words, *d = def + b * words;
			int g;

			if (c->a > 0 && (g = glob_of[c->a]) >= 0 && !BIT(d, g)) u[g >> 5] |= 1u << (g & 31);
			if (c->b > 0 && (g = glob_of[c->b]) >= 0 && !BIT(d, g)) u[g >> 5] |= 1u << (g & 31);
			if (writes(c->op) && c->d > 0 && (g = glob_of[c->d]) >= 0) d[g >> 5] |= 1u << (g & 31);
		}
	do {
		changed = 0;
		for (b = nblocks - 1; b >= 0; b--) {
			uint32_t *in = live_in + b * words, *out = live_out + b * words;
			int w, s;

			for (s = 0; s < blocks[b].nsucc; s++) {
				uint32_t *sin = live_in + blocks[b].succ[s] * words;

				for (w = 0; w < words; w++) out[w] |= sin[w];
			}
			for (w = 0; w < words; w++) {
				uint32_t n = use[b * words + w] | (out[w] & ~def[b * words + w]);

				if (n != in[w]) in[w] = n, changed = 1;
			}
		}
		liveness_rounds++;
	} while (changed);
#ifdef HLSL_DEBUG
	if (getenv("HLSL_STATS")) fprintf(stderr, "ncode %d nvr %d blocks %d glob %d words %d rounds %d\n", hc->ncode, hc->nvr, nblocks, nglob, words, liveness_rounds);
#endif
}

/* ---- intervals and registers ---- */

static int *order, nintervals;	/* registers by the start of their interval */

static void intervals(void)
{
	int i, b, n = 0, *count, *at;

	for (i = 0; i < hc->nvr; i++) hc->vr[i].start = 0x7fffffff, hc->vr[i].end = -1;
	for (i = 0; i < hc->ncode; i++) {
		ins *c = &hc->code[i];
		int r[3], k;

		r[0] = writes(c->op) ? c->d : -1, r[1] = c->a, r[2] = c->b;
		for (k = 0; k < 3; k++)
			if (r[k] > 0) {
				vreg *v = &hc->vr[r[k]];

				if (i < v->start) v->start = i;
				if (i > v->end) v->end = i;
			}
	}
	{
		/* the first block a register is live into, and the last it is live out of: each once */
		uint32_t *seen = hl_alloc(sizeof(uint32_t) * words);
		int wd;

		for (b = 0; b < nblocks; b++)
			for (wd = 0; wd < words; wd++) {
				uint32_t in = live_in[b * words + wd] & ~seen[wd];

				seen[wd] |= in;
				for (; in; in &= in - 1) {
					vreg *v = &hc->vr[glob_v[32 * wd + __builtin_ctz(in)]];

					if (blocks[b].start < v->start) v->start = blocks[b].start;
				}
			}
		memset(seen, 0, sizeof(uint32_t) * words);
		for (b = nblocks - 1; b >= 0; b--)
			for (wd = 0; wd < words; wd++) {
				uint32_t out = live_out[b * words + wd] & ~seen[wd];

				seen[wd] |= out;
				for (; out; out &= out - 1) {
					vreg *v = &hc->vr[glob_v[32 * wd + __builtin_ctz(out)]];

					if (blocks[b].end - 1 > v->end) v->end = blocks[b].end - 1;
				}
			}
	}
	/* sorted by start; at one start, a definition there last */
	count = hl_alloc(sizeof(int) * (hc->ncode + 2));
	at = hl_alloc(sizeof(int) * (hc->ncode + 2));
	for (i = 1; i < hc->nvr; i++)
		if (hc->vr[i].end >= 0) count[hc->vr[i].start + 1]++, n++;
	for (i = 1; i <= hc->ncode; i++) count[i] += count[i - 1];
	order = hl_alloc(sizeof(int) * (n + 1));
	memcpy(at, count, sizeof(int) * (hc->ncode + 1));
	for (i = 1; i < hc->nvr; i++)
		if (hc->vr[i].end >= 0) order[at[hc->vr[i].start]++] = i;
	for (i = 0; i < hc->ncode; i++) {
		/* the definition of the instruction at a start goes after the others starting there */
		int lo = count[i], hi = count[i + 1], k;

		for (k = lo; k < hi - 1; k++)
			if (hc->code[i].d == order[k] && writes(hc->code[i].op)) {
				int t = order[k];

				order[k] = order[hi - 1], order[hi - 1] = t;
				break;
			}
	}
	nintervals = n;
}

static int used_regs[64];

/* a register whose one definition is a load of a constant or a uniform */
static int remat_load(int v)
{
	vreg *r = &hc->vr[v];

	return !r->var && r->defs == 1 && r->def >= 0 && (hc->code[r->def].op == I_FCONST || hc->code[r->def].op == I_LDU);
}

static void allocate(void)
{
	int *active = hl_alloc(sizeof(int) * (hc->nvr + 1)), nact = 0, k, n = nintervals, min_end = 0x7fffffff;
	int owner[64];

	for (k = 0; k < 64; k++) owner[k] = 0, used_regs[k] = 0;
	hc->spill_slots = 0, hc->spilled = 0;
	for (k = 0; k < n; k++) {
		int v = order[k], cls = hc->vr[v].cls, p = hc->vr[v].start, j, r = -1, hint = -1;
		int defined_here = hc->code[p].d == v && writes(hc->code[p].op);
		const uint8_t *ord = cls == C_X ? x_order : f_order;
		int nord = cls == C_X ? (int)sizeof x_order : (int)sizeof f_order;

		/* the intervals that ended free their registers (one that ends where this is defined, too) */
		if (min_end <= (defined_here ? p : p - 1)) for (j = 0, min_end = 0x7fffffff; j < nact;) {
			vreg *a = &hc->vr[active[j]];

			if (a->end < p || (defined_here && a->end == p)) {
				owner[a->loc] = 0;
				if (defined_here && a->end == p && (hc->code[p].op == I_MOV || hc->code[p].op == I_FMOV) && hc->code[p].a == active[j]) hint = a->loc;
				active[j] = active[--nact];
			} else {
				if (a->end < min_end) min_end = a->end;
				j++;
			}
		}
		if (hint >= 0 && !owner[hint]) r = hint;
		for (j = 0; j < nord && r < 0; j++) {
			int reg = cls == C_X ? ord[j] : F(ord[j]);

			if (!owner[reg]) r = reg;
		}
		if (r < 0) {
			/* a load that ends last is loaded again where used; else the interval that ends last gives
			 * its register, or this one goes to the stack */
			int far = -1, fe = hc->vr[v].end, rem = -1, re = -1;

			for (j = 0; j < nact; j++)
				if (hc->vr[active[j]].cls == cls && remat_load(active[j]) && hc->vr[active[j]].end > re) re = hc->vr[active[j]].end, rem = j;
			if (remat_load(v) && hc->vr[v].end >= re) {
				hc->vr[v].loc = REMAT;
				continue;
			}
			if (rem >= 0) {
				r = hc->vr[active[rem]].loc;
				hc->vr[active[rem]].loc = REMAT;
				active[rem] = active[--nact];
				goto got;
			}
			for (j = 0; j < nact; j++)
				if (hc->vr[active[j]].cls == cls && hc->vr[active[j]].end > fe) fe = hc->vr[active[j]].end, far = j;
			if (far < 0) {
				hc->vr[v].loc = (int16_t)(-1 - hc->spill_slots++);
				hc->spilled++;
				continue;
			}
			r = hc->vr[active[far]].loc;
			hc->vr[active[far]].loc = (int16_t)(-1 - hc->spill_slots++);
			hc->spilled++;
			active[far] = active[--nact];
		}
	got:
		hc->vr[v].loc = (int16_t)r;
		owner[r] = v;
		used_regs[r] = 1;
		active[nact++] = v;
		if (hc->vr[v].end < min_end) min_end = hc->vr[v].end;
	}
	hc->vr[0].loc = 0;
}

/* ---- machine code ---- */

typedef struct { int at, target, kind, site; } fixup;	/* kind 0 branch, 1 jal, 2 auipc + addi */
static fixup *fix;
static int nfix, fix_cap, *target_at, ntargets, branch_no, *long_branch, long_cap;
static int nsaved, frame, saved_at[64];
static int *ir_off;
static int **state;	/* each yield's saved registers */
static int *nstate;
static int *yield_pos, *bound_slot, pool_floats;
static uint32_t *pool_bits;
static int npool_bits;

static void w(uint32_t x)
{
	if (hc->nmc == hc->mc_cap) {
		hc->mc = hl_grow(hc->mc, hc->mc_cap * 4, (hc->mc_cap * 2 + 1024) * 4);
		hc->mc_cap = hc->mc_cap * 2 + 1024;
	}
	hc->mc[hc->nmc++] = x;
}

static uint32_t R(int f7, int rs2, int rs1, int f3, int rd, int op)
{
	return (uint32_t)f7 << 25 | (uint32_t)(rs2 & 31) << 20 | (uint32_t)(rs1 & 31) << 15 | (uint32_t)f3 << 12 | (uint32_t)(rd & 31) << 7 | (uint32_t)op;
}
static uint32_t I(int imm, int rs1, int f3, int rd, int op)
{
	return ((uint32_t)imm & 0xfff) << 20 | (uint32_t)(rs1 & 31) << 15 | (uint32_t)f3 << 12 | (uint32_t)(rd & 31) << 7 | (uint32_t)op;
}
static uint32_t S(int imm, int rs2, int rs1, int f3, int op)
{
	return ((uint32_t)imm >> 5 & 0x7f) << 25 | (uint32_t)(rs2 & 31) << 20 | (uint32_t)(rs1 & 31) << 15 | (uint32_t)f3 << 12 | ((uint32_t)imm & 31) << 7 | (uint32_t)op;
}
static uint32_t b_imm(int off)
{
	uint32_t o = (uint32_t)off;

	return (o >> 12 & 1) << 31 | (o >> 5 & 0x3f) << 25 | (o >> 1 & 0xf) << 8 | (o >> 11 & 1) << 7;
}
static uint32_t j_imm(int off)
{
	uint32_t o = (uint32_t)off;

	return (o >> 20 & 1) << 31 | (o >> 1 & 0x3ff) << 21 | (o >> 11 & 1) << 20 | (o >> 12 & 0xff) << 12;
}

static void addi(int rd, int rs, int imm) { w(I(imm, rs, 0, rd, 0x13)); }
static void lw(int rd, int off, int base) { w(I(off, base, 2, rd, 0x03)); }
static void sw(int rs, int off, int base) { w(S(off, rs, base, 2, 0x23)); }
static void flw(int rd, int off, int base) { w(I(off, base, 2, rd, 0x07)); }
static void fsw(int rs, int off, int base) { w(S(off, rs, base, 2, 0x27)); }

static void li(int rd, int32_t k)
{
	if (k >= -2048 && k <= 2047) { addi(rd, X0, k); return; }
	{
		uint32_t hi = ((uint32_t)k + 0x800) >> 12;
		int lo = (int)((uint32_t)k - (hi << 12));

		w(hi << 12 | (uint32_t)(rd & 31) << 7 | 0x37);
		if (lo) addi(rd, rd, lo);
	}
}

static void add_fix(int target, int kind)
{
	if (nfix == fix_cap) fix = hl_grow(fix, fix_cap * sizeof *fix, (fix_cap * 2 + 256) * sizeof *fix), fix_cap = fix_cap * 2 + 256;
	fix[nfix].at = hc->nmc, fix[nfix].target = target, fix[nfix].kind = kind, fix[nfix].site = branch_no;
	nfix++;
}

static int is_long(int site) { return site < long_cap && long_branch[site]; }

/* a conditional branch: f3 its kind; far ones as the opposite branch over a jump */
static void branch(int f3, int rs1, int rs2, int target)
{
	int site = branch_no++;

	if (is_long(site)) {
		w(R(0, rs2, rs1, f3 ^ 1, 0, 0x63) | b_imm(8));
		add_fix(target, 1);
		w(0x6f);
		return;
	}
	add_fix(target, 0);
	fix[nfix - 1].site = site;
	w(R(0, rs2, rs1, f3, 0, 0x63));
}

static void jump_to(int target)
{
	add_fix(target, 1);
	w(0x6f);
}

static void mark(int target) { target_at[target] = hc->nmc; }

static int pool_index(uint32_t bits);
static int pool_off(int k);

/* the place of register v, or v loaded into the scratch register s */
static int src(int v, int s)
{
	int loc;

	if (v == 0) return X0;
	loc = hc->vr[v].loc;
	if (loc >= 0) return loc < 32 ? loc : loc - 32;
	if (loc == REMAT) {
		ins *c = &hc->code[hc->vr[v].def];

		if (c->op == I_LDU) {
			if (hc->vr[v].cls == C_X) lw(s, c->imm, CTX);
			else flw(s, c->imm, CTX);
		} else if (c->imm == 0)
			w(R(0x78, 0, X0, 0, s, 0x53));
		else
			flw(s, pool_off(pool_index((uint32_t)c->imm)), POOLR);
		return s;
	}
	if (hc->vr[v].cls == C_X) lw(s, frame - 4 - 4 * (-1 - loc) - 4 * nsaved, SP);
	else flw(s, frame - 4 - 4 * (-1 - loc) - 4 * nsaved, SP);
	return s;
}

static int slot_off(int loc) { return frame - 4 - 4 * nsaved - 4 * (-1 - loc); }

static int dst(int v) { int loc = hc->vr[v].loc; return loc >= 0 ? (loc < 32 ? loc : loc - 32) : hc->vr[v].cls == C_X ? T0 : 0; }

static void dst_done(int v, int r)
{
	int loc = hc->vr[v].loc;

	if (loc >= 0) return;
	if (hc->vr[v].cls == C_X) sw(r, slot_off(loc), SP);
	else fsw(r, slot_off(loc), SP);
}

static int pool_index(uint32_t bits)
{
	int k;

	for (k = 0; k < npool_bits; k++)
		if (pool_bits[k] == bits && k < pool_floats) return k;
	if (npool_bits >= 1024) hl_fail(0, "more than 1024 constants");
	pool_bits = hl_grow(pool_bits, npool_bits * 4, (npool_bits + 1) * 4);
	pool_bits[npool_bits] = bits;
	return npool_bits++;
}

static int pool_off(int k) { return 4 * k - 2048; }

static void emit_ins(int i)
{
	ins *c = &hc->code[i];
	int a, b, d;
	static const struct { uint8_t op; uint8_t f7, f3; } xr[] = {
		{ I_ADD, 0, 0 }, { I_SUB, 0x20, 0 }, { I_MUL, 1, 0 }, { I_DIV, 1, 4 }, { I_DIVU, 1, 5 }, { I_REM, 1, 6 }, { I_REMU, 1, 7 },
		{ I_AND, 0, 7 }, { I_OR, 0, 6 }, { I_XOR, 0, 4 }, { I_SHL, 0, 1 }, { I_SHR, 0x20, 5 }, { I_SHRU, 0, 5 }, { I_SLT, 0, 2 },
		{ I_SLTU, 0, 3 } };
	static const struct { uint8_t op, f7, f3; } fr[] = {
		{ I_FADD, 0x00, 0 }, { I_FSUB, 0x04, 0 }, { I_FMUL, 0x08, 0 }, { I_FDIV, 0x0c, 0 }, { I_FMIN, 0x14, 0 }, { I_FMAX, 0x14, 1 },
		{ I_FSGNJ, 0x10, 0 }, { I_FSGNJX, 0x10, 2 } };
	int k;

	if (writes(c->op) && c->d > 0 && hc->vr[c->d].loc == REMAT) return;	/* (loaded where it is used) */
	switch (c->op) {
	case I_LABEL: mark(c->imm); return;
	case I_ICONST: d = dst(c->d); li(d, c->imm); dst_done(c->d, d); return;
	case I_FCONST:
		d = dst(c->d);
		if (c->imm == 0) w(R(0x78, 0, X0, 0, d, 0x53));
		else flw(d, pool_off(pool_index((uint32_t)c->imm)), POOLR);
		dst_done(c->d, d);
		return;
	case I_MOV: a = src(c->a, T1); d = dst(c->d); if (a != d) addi(d, a, 0); dst_done(c->d, d); return;
	case I_FMOV: a = src(c->a, 1); d = dst(c->d); if (a != d) w(R(0x10, a, a, 0, d, 0x53)); dst_done(c->d, d); return;
	case I_LDU:
		d = dst(c->d);
		if (hc->vr[c->d].cls == C_X) lw(d, c->imm, CTX);
		else flw(d, c->imm, CTX);
		dst_done(c->d, d);
		return;
	case I_INDEX: d = dst(c->d); addi(d, IDX, 0); dst_done(c->d, d); return;
	case I_SEQ: case I_SNE:
		a = src(c->a, T1), b = src(c->b, T2), d = dst(c->d);
		w(R(0, b, a, 4, d, 0x33));
		if (c->op == I_SEQ) w(I(1, d, 3, d, 0x13));
		else w(R(0, d, X0, 3, d, 0x33));
		dst_done(c->d, d);
		return;
	case I_SEQZ: a = src(c->a, T1), d = dst(c->d); w(I(1, a, 3, d, 0x13)); dst_done(c->d, d); return;
	case I_FSQRT: a = src(c->a, 1), d = dst(c->d); w(R(0x2c, 0, a, 0, d, 0x53)); dst_done(c->d, d); return;
	case I_FNEG: a = src(c->a, 1), d = dst(c->d); w(R(0x10, a, a, 1, d, 0x53)); dst_done(c->d, d); return;
	case I_FABS: a = src(c->a, 1), d = dst(c->d); w(R(0x10, a, a, 2, d, 0x53)); dst_done(c->d, d); return;
	case I_FEQ: case I_FLT: case I_FLE:
		a = src(c->a, 1), b = src(c->b, 2), d = dst(c->d);
		w(R(0x50, b, a, c->op == I_FEQ ? 2 : c->op == I_FLT ? 1 : 0, d, 0x53));
		dst_done(c->d, d);
		return;
	case I_FCVTWS: case I_FCVTWUS:
		a = src(c->a, 1), d = dst(c->d);
		w(R(0x60, c->op == I_FCVTWUS, a, c->rm, d, 0x53));
		dst_done(c->d, d);
		return;
	case I_FCVTSW: case I_FCVTSWU:
		a = src(c->a, T1), d = dst(c->d);
		w(R(0x68, c->op == I_FCVTSWU, a, 0, d, 0x53));
		dst_done(c->d, d);
		return;
	case I_FMVXW: a = src(c->a, 1), d = dst(c->d); w(R(0x70, 0, a, 0, d, 0x53)); dst_done(c->d, d); return;
	case I_FMVWX: a = src(c->a, T1), d = dst(c->d); w(R(0x78, 0, a, 0, d, 0x53)); dst_done(c->d, d); return;
	case I_JMP: jump_to(c->imm); return;
	case I_BNZ: case I_BZ: a = src(c->a, T1); branch(c->op == I_BNZ ? 1 : 0, a, X0, c->imm); return;
	case I_BEQ: case I_BNE: case I_BLT: case I_BGE: case I_BLTU: case I_BGEU: {
		static const uint8_t f3[] = { 0, 1, 4, 5, 6, 7 };

		a = src(c->a, T1), b = src(c->b, T2);
		branch(f3[c->op - I_BEQ], a, b, c->imm);
		return;
	}
	case I_YIELD: {
		int y = c->imm;

		w(I(0x0ba, 0, 2, T0, 0x73));		/* csrr t0, 0x0ba */
		addi(T0, T0, -1);
		lw(T1, pool_off(bound_slot[y]), POOLR);
		branch(6, T0, T1, ntargets - 2 * hc->yields + 2 * y);	/* bltu: to the save */
		mark(ntargets - 3 * hc->yields + y);	/* the resume */
		return;
	}
	case I_PIX:
		if (c->imm == 1) { sw(X0, 16, OUT); return; }
		a = src(c->a, T1);
		w(I(2, a, 1, T0, 0x13));	/* slli */
		w(R(0, OUT, T0, 0, T0, 0x33));
		b = src(c->b, T2);
		sw(b, 0, T0);
		return;
	case I_PIXF: a = src(c->a, 1); fsw(a, 4 * c->imm, OUT); return;
	case I_RET: jump_to(ntargets - 3 * hc->yields - 3); return;	/* ret0 */
	}
	for (k = 0; k < (int)(sizeof xr / sizeof *xr); k++)
		if (xr[k].op == c->op) {
			a = src(c->a, T1), d = dst(c->d);
			if (c->b < 0) {
				int imm = c->imm;

				if (c->op == I_SHL || c->op == I_SHRU) imm &= 31;
				if (c->op == I_SHR) imm = (imm & 31) | 0x400;
				w(I(imm, a, xr[k].f3, d, 0x13));
			} else {
				b = src(c->b, T2);
				w(R(xr[k].f7, b, a, xr[k].f3, d, 0x33));
			}
			dst_done(c->d, d);
			return;
		}
	for (k = 0; k < (int)(sizeof fr / sizeof *fr); k++)
		if (fr[k].op == c->op) {
			a = src(c->a, 1), b = src(c->b, 2), d = dst(c->d);
			w(R(fr[k].f7, b, a, fr[k].f3, d, 0x53));
			dst_done(c->d, d);
			return;
		}
	hl_fail(0, "internal: no code for operation %d", c->op);
}

/* the yields' saved registers: what is live after each, but constants (made again) */
static void yield_state(void)
{
	int i, g;

	state = hl_alloc(sizeof(int *) * (hc->yields + 1));
	nstate = hl_alloc(sizeof(int) * (hc->yields + 1));
	yield_pos = hl_alloc(sizeof(int) * (hc->yields + 1));
	for (i = 0; i < hc->yields; i++) yield_pos[i] = -1;
	hc->state_words = 0;
	count_uses();
	for (i = 0; i < hc->ncode; i++)
		if (hc->code[i].op == I_YIELD) {
			int y = hc->code[i].imm, b = block_of[i], n = 0;
			uint32_t *out = live_out + b * words;

			yield_pos[y] = i;
			state[y] = hl_alloc(sizeof(int) * (nglob + 1));
			for (g = 0; g < nglob; g++)
				if (BIT(out, g)) state[y][n++] = glob_v[g];
			nstate[y] = n;
			{
				int words_saved = 0, k;

				for (k = 0; k < n; k++) {
					vreg *v = &hc->vr[state[y][k]];

					if (!(v->defs == 1 && v->def >= 0 && (hc->code[v->def].op == I_ICONST || hc->code[v->def].op == I_FCONST ||
									     hc->code[v->def].op == I_LDU)))
						words_saved++;
				}
				if (words_saved > hc->state_words) hc->state_words = words_saved;
			}
		}
}

static int is_remat(int v)
{
	vreg *r = &hc->vr[v];

	return r->defs == 1 && r->def >= 0 && (hc->code[r->def].op == I_ICONST || hc->code[r->def].op == I_FCONST || hc->code[r->def].op == I_LDU);
}

static int state_base(void) { return (hc->flags & HLSL_RAW) ? 20 : 16; }

static void save_stub(int y)
{
	int k, j = 0, P = hc->pix_vreg;

	for (k = 0; k < nstate[y]; k++) {
		int v = state[y][k], r, at;

		if (is_remat(v)) continue;
		at = state_base() + 4 * j++;
		if (hc->vr[v].cls == C_X) { r = src(v, T1); sw(r, at, OUT); }
		else { r = src(v, 1); fsw(r, at, OUT); }
	}
	w(((0xFF000u) << 12) | (uint32_t)T2 << 7 | 0x37);	/* lui t2, 0xff000 */
	addi(T2, T2, y);
	if (hc->flags & HLSL_RAW) sw(T2, 16, OUT);
	else {
		int r = src(P, T1);

		w(I(2, r, 1, T0, 0x13));
		w(R(0, OUT, T0, 0, T0, 0x33));
		sw(T2, 0, T0);
	}
	jump_to(ntargets - 3 * hc->yields - 2);	/* ret1 */
}

static void restore_stub(int y)
{
	int k, j = 0;

	for (k = 0; k < nstate[y]; k++) {
		int v = state[y][k], loc = hc->vr[v].loc, at;

		if (is_remat(v)) {
			emit_ins(hc->vr[v].def);
			continue;
		}
		at = state_base() + 4 * j++;
		if (hc->vr[v].cls == C_X) {
			if (loc >= 0) lw(loc, at, OUT);
			else { lw(T1, at, OUT); sw(T1, slot_off(loc), SP); }
		} else {
			if (loc >= 0) flw(loc - 32, at, OUT);
			else { flw(1, at, OUT); fsw(1, slot_off(loc), SP); }
		}
	}
	jump_to(ntargets - 3 * hc->yields + y);
}

static int epi_words, ret_words;

static void epilogue(void)
{
	int k, at = hc->nmc;

	for (k = 0; k < nsaved; k++) {
		int r = saved_at[k];

		if (r < 32) lw(r, frame - 4 - 4 * k, SP);
		else flw(r - 32, frame - 4 - 4 * k, SP);
	}
	addi(SP, SP, frame);
	w(I(0, RA, 0, X0, 0x67));	/* ret */
	epi_words = hc->nmc - at;
}

static int resolve(void)
{
	int k, ok = 1;

	for (k = 0; k < nfix; k++) {
		fixup *f = &fix[k];
		int off = 4 * (target_at[f->target] - f->at);

		if (target_at[f->target] < 0) hl_fail(0, "internal: a target never placed (%d)", f->target);
		if (f->kind == 0) {
			if (off < -4096 || off > 4094) {
				if (f->site >= long_cap) {
					int nc = f->site + 256;

					long_branch = hl_grow(long_branch, long_cap * 4, nc * 4);
					long_cap = nc;
				}
				long_branch[f->site] = 1;
				ok = 0;
				continue;
			}
			hc->mc[f->at] |= b_imm(off);
		} else if (f->kind == 1) {
			if (off < -(1 << 20) || off >= (1 << 20)) hl_fail(0, "the code is longer than a jump reaches");
			hc->mc[f->at] |= j_imm(off);
		} else {
			uint32_t hi = ((uint32_t)off + 0x800) >> 12;
			int lo = (int)((uint32_t)off - (hi << 12));

			hc->mc[f->at] |= hi << 12;
			hc->mc[f->at + 1] |= (uint32_t)(lo & 0xfff) << 20;
		}
	}
	return ok;
}

static int pool_target;

static void emit_all(void)
{
	int i, y, p, prologue_end;

	hc->nmc = 0, nfix = 0, branch_no = 0;
	for (i = 0; i < ntargets; i++) target_at[i] = -1;
	/* prologue */
	addi(SP, SP, -frame);
	for (i = 0; i < nsaved; i++) {
		int r = saved_at[i];

		if (r < 32) sw(r, frame - 4 - 4 * i, SP);
		else fsw(r - 32, frame - 4 - 4 * i, SP);
	}
	addi(IDX, OUT, 0);
	add_fix(pool_target, 2);
	w((uint32_t)POOLR << 7 | 0x17);		/* auipc a4 */
	addi(POOLR, POOLR, 0);
	lw(OUT, 0, CTX);
	lw(T0, 4, CTX);
	w(R(0x20, T0, IDX, 0, T0, 0x33));	/* sub t0, index, first */
	lw(T1, 12, CTX);
	w(R(1, T1, T0, 0, T0, 0x33));		/* mul */
	w(R(0, T0, OUT, 0, OUT, 0x33));
	branch(1, A2, X0, ntargets - 3 * hc->yields - 1);	/* bnez round: the dispatch */
	prologue_end = hc->nmc;
	(void)prologue_end;
	for (i = 0; i < hc->ncode; i++) {
		ir_off[i] = hc->nmc;
		emit_ins(i);
	}
	ir_off[hc->ncode] = hc->nmc;
	/* ret0, the epilogue, ret1 */
	mark(ntargets - 3 * hc->yields - 3);
	addi(OUT, X0, 0);
	epilogue();
	mark(ntargets - 3 * hc->yields - 2);
	addi(OUT, X0, 1);
	{
		/* (back to the epilogue: it is just before) */
		int back = target_at[ntargets - 3 * hc->yields - 3] + 1;

		w(0x6f | (uint32_t)0);
		{
			int off = 4 * (back - (hc->nmc - 1));

			hc->mc[hc->nmc - 1] |= j_imm(off);
		}
	}
	ret_words = 1;
	/* the dispatch: the first word of the index's pixels that says where a step stopped */
	mark(ntargets - 3 * hc->yields - 1);
	{
		int found = ntargets - 3 * hc->yields - 4;	/* (a target of its own) */

		if (hc->flags & HLSL_RAW) {
			lw(T0, 16, OUT);
			w(I(24, T0, 5, T1, 0x13));
			addi(T1, T1, -255);
			branch(0, T1, X0, found);
		} else
			for (p = 0; p < 4; p++) {
				lw(T0, 4 * p, OUT);
				w(I(24, T0, 5, T1, 0x13));	/* srli */
				addi(T1, T1, -255);
				branch(0, T1, X0, found);
			}
		jump_to(ntargets - 3 * hc->yields - 3);
		mark(found);
		w(I(8, T0, 1, T0, 0x13));
		w(I(8, T0, 5, T0, 0x13));
		for (y = 0; y < hc->yields; y++) {
			if (yield_pos[y] < 0) continue;
			addi(T1, T0, -y);
			branch(0, T1, X0, ntargets - 2 * hc->yields + 2 * y + 1);
		}
		jump_to(ntargets - 3 * hc->yields - 3);
	}
	for (y = 0; y < hc->yields; y++) {
		if (yield_pos[y] < 0) continue;
		mark(ntargets - 2 * hc->yields + 2 * y);
		save_stub(y);
		mark(ntargets - 2 * hc->yields + 2 * y + 1);
		restore_stub(y);
	}
	/* the constants: a4 points 2048 bytes in */
	mark(pool_target);
	{
		int base = hc->nmc;

		for (i = 0; i < npool_bits; i++) w(pool_bits[i]);
		target_at[pool_target] = base + 512;	/* + 2048 bytes */
	}
}

/* ---- the longest stretch to the next yield, from each place ---- */

static int *longest;

static int term_cost(int i)
{
	ins *c = &hc->code[i];

	if (c->op == I_RET) return 1 + epi_words;
	if (c->op == I_YIELD) {
		int y = c->imm, k, n = 0;

		for (k = 0; k < nstate[y]; k++)
			if (!is_remat(state[y][k])) n++;
		/* the save, its stores, the mark, ret1 and the epilogue */
		return n * 2 + 8 + 2 + epi_words;
	}
	return 0;
}

static void longest_paths(void)
{
	int *stack, sp = 0, i, *st;

	longest = hl_alloc(sizeof(int) * (hc->ncode + 1));
	st = hl_alloc(sizeof(int) * (hc->ncode + 1));
	stack = hl_alloc(sizeof(int) * (hc->ncode + 2) * 2);
	for (i = 0; i <= hc->ncode; i++) longest[i] = -1;
	longest[hc->ncode] = 0;
	for (i = hc->ncode - 1; i >= 0; i--) {
		if (st[i]) continue;
		stack[sp++] = i;
		while (sp) {
			int v = stack[sp - 1], op = hc->code[v].op, succ[2], ns = 0, k, all = 1, best = 0;
			int cost = ir_off[v + 1] - ir_off[v];

			if (st[v] == 2) { sp--; continue; }
			st[v] = 1;
			if (op == I_RET || op == I_YIELD) {
				longest[v] = cost + term_cost(v);
				st[v] = 2, sp--;
				continue;
			}
			if (op == I_JMP) succ[ns++] = hc->label_at[hc->code[v].imm];
			else {
				succ[ns++] = v + 1;
				if (is_branch(op)) succ[ns++] = hc->label_at[hc->code[v].imm];
			}
			for (k = 0; k < ns; k++) {
				int s = succ[k];

				if (s >= hc->ncode) continue;
				if (st[s] == 1) hl_fail(0, "internal: a loop with no place to stop");
				if (st[s] == 0) { all = 0; stack[sp++] = s; }
				else if (longest[s] > best) best = longest[s];
			}
			if (!all) continue;
			longest[v] = cost + best;
			st[v] = 2, sp--;
		}
	}
}

void hl_back(void)
{
	int i, y, passes = 0, k;

	fix = 0, nfix = 0, fix_cap = 0;	/* (the arena of the last compilation is gone) */
	hoist_loads();
	coalesce();
	dead_code();
	compact();
	fuse_branches();
	compact();
	liveness();
	intervals();
	allocate();
	yield_state();
	{
		int most = (hc->flags & HLSL_RAW) ? 19 : 20;

		if (hc->state_words > most)
			hl_fail(0, "%d values are live where a loop may stop, and an index keeps %d: fewer variables across loops",
				hc->state_words, most);
		hc->texels = (hc->flags & HLSL_RAW) ? (5 + hc->state_words + 3) / 4 : 1 + (hc->state_words + 3) / 4;
		if (hc->texels < 1) hc->texels = 1;
	}
	/* the frame: spill slots, then the callee-saved registers this code uses */
	nsaved = 0;
	for (k = 8; k < 28; k++)
		if ((k == 8 || k == 9 || k >= 18) && used_regs[k]) saved_at[nsaved++] = k;
	for (k = 8; k < 28; k++)
		if ((k == 8 || k == 9 || k >= 18) && used_regs[F(k)]) saved_at[nsaved++] = F(k);
	frame = (4 * (nsaved + hc->spill_slots) + 15) & ~15;
	if (frame > 2000) hl_fail(0, "too many values for the registers (%d on the stack)", hc->spill_slots);
	/* targets: labels, then the pool, found, ret0, ret1, dispatch, resumes, saves and restores */
	ntargets = hc->labels + 5 + 3 * hc->yields;
	pool_target = hc->labels;
	target_at = hl_alloc(sizeof(int) * ntargets);
	ir_off = hl_alloc(sizeof(int) * (hc->ncode + 1));
	/* the constants: the floats first, then a bound for each yield */
	pool_bits = 0, npool_bits = 0, pool_floats = 1024;
	for (i = 0; i < hc->ncode; i++)
		if (hc->code[i].op == I_FCONST && hc->code[i].imm) pool_index((uint32_t)hc->code[i].imm);
	pool_floats = npool_bits;
	bound_slot = hl_alloc(sizeof(int) * (hc->yields + 1));
	for (y = 0; y < hc->yields; y++) {
		pool_bits = hl_grow(pool_bits, npool_bits * 4, (npool_bits + 1) * 4);
		pool_bits[npool_bits] = 0;
		bound_slot[y] = npool_bits++;
	}
	long_branch = 0, long_cap = 0;
	do {
		if (++passes > 20) hl_fail(0, "internal: branches do not settle");
		emit_all();
	} while (!resolve());
	/* each yield's bound: the longest way from it on, to a yield's save or the end */
	longest_paths();
	hc->longest = 0;
	for (y = 0; y < hc->yields; y++) {
		int at = yield_pos[y], b;

		if (at < 0) continue;
		b = longest[at + 1] + MARGIN;
		hc->mc[target_at[pool_target] - 512 + bound_slot[y]] = (uint32_t)b;
		/* a step that starts here: the prologue, the dispatch, the restore, the way on */
		if (b + 40 + 2 * nstate[y] > hc->longest) hc->longest = b + 40 + 2 * nstate[y];
	}
	if (longest[0] + 20 > hc->longest) hc->longest = longest[0] + 20;
#ifdef HLSL_DEBUG
	{
		void hl_dump(void);

		if (getenv("HLSL_DUMP")) hl_dump();
	}
#endif
}

#ifdef HLSL_DEBUG
static const char *const op_names[] = { "nop", "iconst", "fconst", "mov", "fmov", "add", "sub", "mul", "div", "divu", "rem", "remu",
	"and", "or", "xor", "shl", "shr", "shru", "slt", "sltu", "seq", "sne", "seqz", "fadd", "fsub", "fmul", "fdiv", "fsqrt", "fmin",
	"fmax", "fneg", "fabs", "fsgnj", "fsgnjx", "feq", "flt", "fle", "fcvtws", "fcvtwus", "fcvtsw", "fcvtswu", "fmvxw", "fmvwx",
	"ldu", "index", "label", "jmp", "bnz", "bz", "yield", "pix", "pixf", "ret", "beq", "bne", "blt", "bge", "bltu", "bgeu" };

void hl_dump(void)
{
	int i;

	for (i = 0; i < hc->ncode; i++) {
		ins *c = &hc->code[i];
		char r[3][24];
		int k, v[3] = { c->d, c->a, c->b };

		for (k = 0; k < 3; k++) {
			if (v[k] < 0) { r[k][0] = 0; continue; }
			if (v[k] == 0) { strcpy(r[k], "zero"); continue; }
			if (hc->vr[v[k]].loc >= 32) sprintf(r[k], "v%d(f%d)", v[k], hc->vr[v[k]].loc - 32);
			else if (hc->vr[v[k]].loc >= 0) sprintf(r[k], "v%d(x%d)", v[k], hc->vr[v[k]].loc);
			else sprintf(r[k], "v%d(s%d)", v[k], -1 - hc->vr[v[k]].loc);
		}
		fprintf(stderr, "%4d %-7s %-12s %-12s %-12s imm %d rm %d\n", i, op_names[c->op], writes(c->op) ? r[0] : "", r[1], r[2], c->imm, c->rm);
	}
}
#endif
