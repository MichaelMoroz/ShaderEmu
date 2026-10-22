/* The IR run as it is, for a machine the back end makes no code for (gpu_vm: gpu_vm/userland/apps/README.md). Each
 * operation does what hlsl_back.c's RISC-V code for it does (RV32IMF's division, shifts, conversions and min/max),
 * so a picture is the RISC-V machine's. A step runs whole: with no lanes, the steps are plain calls (as on rvc_cpu),
 * and every yield goes on at once. Built with HLSL_INTERP (on by default for __gpuvm__). */
#include <math.h>
#include <stdlib.h>
#include "hlsl_int.h"

#ifdef HLSL_INTERP

typedef struct hl_interp {
	ins *code;
	int ncode, nvr;
	int *label_at;		/* the instruction a label is */
} hl_interp;

/* the IR of the compilation that is ending, kept in memory of its own (the arena is freed) */
hl_interp *hl_interp_keep(void)
{
	hl_interp *k = calloc(1, sizeof *k);
	int i;

	if (!k) return 0;
	k->ncode = hc->ncode, k->nvr = hc->nvr;
	k->code = malloc(sizeof(ins) * (size_t)(hc->ncode + 1));
	k->label_at = malloc(sizeof(int) * (size_t)(hc->labels + 1));
	if (!k->code || !k->label_at) {
		free(k->code), free(k->label_at), free(k);
		return 0;
	}
	memcpy(k->code, hc->code, sizeof(ins) * (size_t)hc->ncode);
	for (i = 0; i <= hc->labels; i++) k->label_at[i] = hc->ncode;
	for (i = 0; i < hc->ncode; i++)
		if (hc->code[i].op == I_LABEL && hc->code[i].imm >= 0 && hc->code[i].imm <= hc->labels) k->label_at[hc->code[i].imm] = i;
	return k;
}

void hl_interp_free(hl_interp *k)
{
	if (!k) return;
	free(k->code), free(k->label_at), free(k);
}

static float fl(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t bi(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static int isnan_bits(uint32_t b) { return (b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu); }

/* fmin/fmax as RISC-V's: a NaN gives the other, two give the canonical NaN, -0 is below +0 */
static uint32_t fminmax(uint32_t a, uint32_t b, int max)
{
	if (isnan_bits(a) && isnan_bits(b)) return 0x7fc00000u;
	if (isnan_bits(a)) return b;
	if (isnan_bits(b)) return a;
	if ((a << 1) == 0 && (b << 1) == 0) return max ? (a & b) : (a | b);	/* (zeros: the sign bit decides) */
	return (fl(a) < fl(b)) == !max ? a : b;
}

/* fcvt.w.s / fcvt.wu.s with a rounding mode (0 nearest even, 1 toward zero, 2 down, 3 up, 4 nearest away) */
static uint32_t to_int(uint32_t b, int rm, int uns)
{
	float f = fl(b), r;

	if (isnan_bits(b)) return uns ? 0xffffffffu : 0x7fffffffu;
	r = rm == 1 ? truncf(f) : rm == 2 ? floorf(f) : rm == 3 ? ceilf(f) : rm == 4 ? roundf(f) : rintf(f);
	if (uns) {
		if (r <= -1.0f || (r < 0.0f)) return 0;
		if (r >= 4294967296.0f) return 0xffffffffu;
		return (uint32_t)r;
	}
	if (r >= 2147483648.0f) return 0x7fffffffu;
	if (r < -2147483648.0f) return 0x80000000u;
	return (uint32_t)(int32_t)r;
}

static uint32_t interp_run(const hl_interp *k, uint32_t index, uint32_t *ctx, uint32_t *x)
{
	const ins *code = k->code;
	uint8_t *out = (uint8_t *)(uintptr_t)(ctx[0] + (index - ctx[1]) * ctx[3]);
	int pc = 0, guard = 0;

	x[0] = 0;
	while (pc < k->ncode) {
		const ins *c = &code[pc++];
		uint32_t a = c->a > 0 ? x[c->a] : 0, b = c->b > 0 ? x[c->b] : c->b < 0 ? (uint32_t)c->imm : 0, v = 0;

		(void)guard;
		switch (c->op) {
		case I_NOP: case I_LABEL: case I_YIELD: continue;
		case I_ICONST: case I_FCONST: v = (uint32_t)c->imm; break;
		case I_MOV: case I_FMOV: case I_FMVXW: case I_FMVWX: v = a; break;
		case I_LDU: v = *(const uint32_t *)((const uint8_t *)ctx + c->imm); break;
		case I_INDEX: v = index; break;
		case I_ADD: v = a + b; break;
		case I_SUB: v = a - b; break;
		case I_MUL: v = a * b; break;
		case I_DIV: v = b == 0 ? 0xffffffffu : (a == 0x80000000u && b == 0xffffffffu) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
		case I_DIVU: v = b == 0 ? 0xffffffffu : a / b; break;
		case I_REM: v = b == 0 ? a : (a == 0x80000000u && b == 0xffffffffu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
		case I_REMU: v = b == 0 ? a : a % b; break;
		case I_AND: v = a & b; break;
		case I_OR: v = a | b; break;
		case I_XOR: v = a ^ b; break;
		case I_SHL: v = a << (b & 31); break;
		case I_SHR: v = (uint32_t)((int32_t)a >> (b & 31)); break;
		case I_SHRU: v = a >> (b & 31); break;
		case I_SLT: v = (int32_t)a < (int32_t)b; break;
		case I_SLTU: v = a < b; break;
		case I_SEQ: v = a == b; break;
		case I_SNE: v = a != b; break;
		case I_SEQZ: v = a == 0; break;
		case I_FADD: v = bi(fl(a) + fl(b)); break;
		case I_FSUB: v = bi(fl(a) - fl(b)); break;
		case I_FMUL: v = bi(fl(a) * fl(b)); break;
		case I_FDIV: v = bi(fl(a) / fl(b)); break;
		case I_FSQRT: v = bi(sqrtf(fl(a))); break;
		case I_FMIN: v = fminmax(a, b, 0); break;
		case I_FMAX: v = fminmax(a, b, 1); break;
		case I_FNEG: v = a ^ 0x80000000u; break;
		case I_FABS: v = a & 0x7fffffffu; break;
		case I_FSGNJ: v = (a & 0x7fffffffu) | (b & 0x80000000u); break;
		case I_FSGNJX: v = a ^ (b & 0x80000000u); break;
		case I_FEQ: v = fl(a) == fl(b); break;
		case I_FLT: v = fl(a) < fl(b); break;
		case I_FLE: v = fl(a) <= fl(b); break;
		case I_FCVTWS: v = to_int(a, c->rm, 0); break;
		case I_FCVTWUS: v = to_int(a, c->rm, 1); break;
		case I_FCVTSW: v = bi((float)(int32_t)a); break;
		case I_FCVTSWU: v = bi((float)a); break;
		case I_JMP: pc = k->label_at[c->imm]; continue;
		case I_BNZ: if (a) pc = k->label_at[c->imm]; continue;
		case I_BZ: if (!a) pc = k->label_at[c->imm]; continue;
		case I_BEQ: if (a == b) pc = k->label_at[c->imm]; continue;
		case I_BNE: if (a != b) pc = k->label_at[c->imm]; continue;
		case I_BLT: if ((int32_t)a < (int32_t)b) pc = k->label_at[c->imm]; continue;
		case I_BGE: if ((int32_t)a >= (int32_t)b) pc = k->label_at[c->imm]; continue;
		case I_BLTU: if (a < b) pc = k->label_at[c->imm]; continue;
		case I_BGEU: if (a >= b) pc = k->label_at[c->imm]; continue;
		case I_PIX:
			if (c->imm == 1) { *(uint32_t *)(out + 16) = 0; continue; }
			*(uint32_t *)(out + 4 * a) = c->b > 0 ? x[c->b] : 0;
			continue;
		case I_PIXF: *(uint32_t *)(out + 4 * c->imm) = a; continue;
		case I_RET: return 0;
		default: continue;
		}
		if (c->d > 0) x[c->d] = v;
	}
	return 0;
}

/* The step: the program's IR is in the word below the block (hlsl.c). Every round is the whole index. */
uint32_t hl_interp_step(uint32_t index, void *ctx, uint32_t round)
{
	uint32_t *block = ctx;
	const hl_interp *k = (const hl_interp *)(uintptr_t)block[-1];
	uint32_t small[3072], *x = small, r;

	(void)round;
	if (k->nvr + 1 > 3072 && !(x = malloc(sizeof(uint32_t) * (size_t)(k->nvr + 1)))) return 0;
	r = interp_run(k, index, block, x);
	if (x != small) free(x);
	return r;
}

#endif
