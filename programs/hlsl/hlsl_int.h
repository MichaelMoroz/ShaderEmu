/* The HLSL compiler's own declarations (hlsl.h is the interface). */
#ifndef HLSL_INT_H
#define HLSL_INT_H
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include "hlsl.h"

/* ---- memory: one arena a compilation, freed whole ---- */
typedef struct arena_block { struct arena_block *next; uint32_t used, size; } arena_block;
void *hl_alloc(uint32_t bytes);	/* zeroed */
void *hl_grow(void *old, uint32_t old_bytes, uint32_t new_bytes);

/* ---- tokens ---- */
enum { TK_EOF, TK_ID, TK_INT, TK_FLOAT, TK_P };
enum {	/* punctuators of more than one character (one character is itself) */
	P_SHLEQ = 256, P_SHREQ, P_ADDEQ, P_SUBEQ, P_MULEQ, P_DIVEQ, P_MODEQ, P_ANDEQ, P_OREQ, P_XOREQ,
	P_INC, P_DEC, P_SHL, P_SHR, P_LE, P_GE, P_EQ, P_NE, P_ANDAND, P_OROR, P_COLONCOLON
};
typedef struct {
	uint8_t kind, uns;	/* uns: an integer literal with u */
	int line;
	int p;			/* punctuator */
	const char *s;		/* identifier, interned */
	float f;
	uint32_t i;
} tok;

/* ---- types ---- */
enum { B_VOID, B_BOOL, B_INT, B_UINT, B_FLOAT, B_STRUCT };
enum { S_SCALAR, S_VEC, S_MAT, S_STRUCT };
typedef struct {
	uint8_t base, shape, rows, cols;	/* a vector is 1 x cols; a matrix rows x cols */
	int16_t st;		/* struct number */
	int16_t arr;		/* array length, 0 if none */
} type;

typedef struct node node;
typedef struct {
	const char *name;
	int nmembers;
	type *mt;
	const char **mname;
	int *moff;		/* components before the member */
	int comps;
} struct_def;

/* ---- syntax ---- */
enum {
	N_INT, N_FLOAT, N_BOOL, N_ID, N_BIN, N_ASSIGN, N_UN, N_PRE, N_POST, N_COND, N_CALL, N_CAST,
	N_MEMBER, N_INDEX, N_LIST, N_COMMA,
	N_BLOCK, N_DECL, N_EXPR, N_IF, N_FOR, N_WHILE, N_DO, N_BREAK, N_CONTINUE, N_RETURN, N_DISCARD, N_EMPTY
};
enum { Q_CONST = 1, Q_STATIC = 2, Q_UNIFORM = 4, Q_IN = 8, Q_OUT = 16, A_UNROLL = 32, A_LOOP = 64 };
struct node {
	uint8_t k;
	int16_t op;
	int line;
	int flags;
	const char *name;
	node *a, *b, *c, *d;
	node *next;
	type ty;		/* of a declaration or a cast */
	float f;
	int32_t i;
	int unroll;		/* [unroll(n)] */
};

typedef struct param { const char *name; type ty; int flags; } param;
typedef struct func {
	const char *name;
	type ret;
	int nparams;
	param *params;
	node *body;
	int line;
	int written;		/* bit p: parameter p is assigned in the body */
	int active;		/* being inlined: recursion */
	int cost;		/* nodes, for unrolling */
	struct func *next;
} func;

typedef struct global {
	const char *name;
	type ty;
	int flags;
	node *init;
	int offset;		/* a uniform's byte offset */
	int line;
	struct global *next;
} global;

/* ---- the IR: operations on numbered virtual registers ---- */
enum {
	I_NOP, I_ICONST, I_FCONST, I_MOV, I_FMOV,
	I_ADD, I_SUB, I_MUL, I_DIV, I_DIVU, I_REM, I_REMU, I_AND, I_OR, I_XOR, I_SHL, I_SHR, I_SHRU,
	I_SLT, I_SLTU, I_SEQ, I_SNE, I_SEQZ,
	I_FADD, I_FSUB, I_FMUL, I_FDIV, I_FSQRT, I_FMIN, I_FMAX, I_FNEG, I_FABS, I_FSGNJ, I_FSGNJX,
	I_FEQ, I_FLT, I_FLE,
	I_FCVTWS, I_FCVTWUS, I_FCVTSW, I_FCVTSWU, I_FMVXW, I_FMVWX,
	I_LDU,			/* d = the uniform word at imm */
	I_INDEX,		/* d = the index */
	I_LABEL, I_JMP, I_BNZ, I_BZ,	/* a: the condition; imm: the label */
	I_YIELD,		/* imm: the yield's number */
	I_PIX,			/* the pixel a (its number) is b (a word); with HLSL_RAW, b is the first of four floats */
	I_PIXF,			/* HLSL_RAW: the float a is word imm of the pixel */
	I_RET,
	I_BEQ, I_BNE, I_BLT, I_BGE, I_BLTU, I_BGEU,	/* a and b compared; imm: the label */
	I_COUNT
};
typedef struct {
	uint8_t op, rm;
	int d, a, b;
	int32_t imm;
} ins;

enum { C_X, C_F };
typedef struct {
	uint8_t cls, var, defs;
	int16_t loc;		/* register, or -1 - spill slot */
	int def;		/* where a single definition is */
	int uses;
	int start, end;		/* its interval */
	int block;		/* its block, if it is used in one only; -1: more */
} vreg;

#define MAXC 64		/* components of one value */
typedef struct {
	type t;
	int n;
	int c[MAXC];		/* >= 0 a register; <= -2 the constant -2 - c */
} val;

typedef struct { uint8_t cls; uint32_t bits; } konst;

typedef struct sym {
	const char *name;
	int kind;		/* 0 a value (v), 1 a uniform (g), 2 a constant global (g, its value made at a use) */
	val *v;
	global *g;
} sym;

typedef struct {
	int brk, cont;		/* labels */
} loop_ctx;

typedef struct {
	val *ret;		/* where returns go */
	type rt;
	int end;		/* label */
	int direct;		/* one return, the last statement: its value is the call's */
	val *direct_v;
} inline_ctx;

typedef struct hlsl_c {
	jmp_buf fail;
	char *err;
	int err_bytes;
	int flags;
	/* source and tokens */
	const char *src;
	tok *toks;
	int ntoks, at;
	/* names */
	const char **names;
	int nnames, names_cap;
	/* the program */
	struct_def *structs;
	int nstructs;
	func *funcs;
	global *globals;
	int uniform_bytes;
	/* generation */
	ins *code;
	int ncode, code_cap;
	vreg *vr;
	int nvr, vr_cap;
	konst *ks;
	int nks, ks_cap;
	int *khash;		/* 1024 heads */
	int *knext;
	int labels;
	sym *syms;
	int nsyms, syms_cap;
	loop_ctx loops[64];
	int nloops;
	inline_ctx inl[64];
	int ninl;
	int yields;
	int pix_vreg;
	int hoist_at;		/* where a pixel's constants and uniforms are loaded */
	int sym_floor, nglobal_syms;	/* a function's names from sym_floor; the globals' below nglobal_syms */
	int depth;
	/* the back end */
	int *label_at;		/* the instruction a label is */
	uint32_t *mc;		/* machine code */
	int nmc, mc_cap;
	uint32_t *pool;
	int npool;
	int spill_slots;
	int state_words;	/* the most saved at a yield */
	int texels;
	int longest;
	int spilled;
	int line;
} hlsl_c;

extern hlsl_c *hc;
void hl_fail(int line, const char *fmt, ...) __attribute__((noreturn));
const char *hl_intern(const char *s, int len);

/* hlsl_parse.c */
void hl_lex(void);
void hl_parse(void);
int hl_type_comps(type t);
int hl_type_eq(type a, type b);
const char *hl_type_name(type t, char *buf);
int hl_parse_type_name(const char *s, type *t);

/* hlsl_gen.c */
void hl_gen(const char *entry);

/* hlsl_back.c */
void hl_back(void);

/* hlsl_interp.c: the IR run as it is where the back end makes no code (gpu_vm, which has no RISC-V) */
#if defined(__gpuvm__) && !defined(HLSL_INTERP)
#define HLSL_INTERP 1
#endif
#ifdef HLSL_INTERP
typedef struct hl_interp hl_interp;
hl_interp *hl_interp_keep(void);
void hl_interp_free(hl_interp *k);
uint32_t hl_interp_step(uint32_t index, void *ctx, uint32_t round);
#endif

#endif
