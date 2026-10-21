/* The compiler's entry: a program's source to its code, its uniforms and its block. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "hlsl_int.h"

hlsl_c *hc;
static arena_block *arena;

void *hl_alloc(uint32_t bytes)
{
	arena_block *b = arena;

	bytes = (bytes + 7) & ~7u;
	if (!b || b->used + bytes > b->size) {
		uint32_t size = bytes > 60000 ? bytes : 60000;

		b = malloc(sizeof(arena_block) + size);
		if (!b) hl_fail(0, "out of memory");
		b->next = arena, b->used = 0, b->size = size;
		arena = b;
	}
	{
		void *p = (char *)(b + 1) + b->used;

		b->used += bytes;
		memset(p, 0, bytes);
		return p;
	}
}

void *hl_grow(void *old, uint32_t old_bytes, uint32_t new_bytes)
{
	void *p = hl_alloc(new_bytes);

	if (old) memcpy(p, old, old_bytes < new_bytes ? old_bytes : new_bytes);
	return p;
}

static void arena_free(void)
{
	while (arena) {
		arena_block *n = arena->next;

		free(arena);
		arena = n;
	}
}

void hl_fail(int line, const char *fmt, ...)
{
	va_list ap;
	int n = 0;

	if (hc->err && hc->err_bytes > 0) {
		if (line > 0) n = snprintf(hc->err, hc->err_bytes, "line %d: ", line);
		if (n < 0 || n >= hc->err_bytes) n = 0;
		va_start(ap, fmt);
		vsnprintf(hc->err + n, hc->err_bytes - n, fmt, ap);
		va_end(ap);
	}
	longjmp(hc->fail, 1);
}

/* names in a table of 512 chains: each is a name's text, then the next's address */
const char *hl_intern(const char *s, int len)
{
	unsigned h = 2166136261u;
	int k;
	char *n;

	for (k = 0; k < len; k++) h = (h ^ (uint8_t)s[k]) * 16777619u;
	h &= 511;
	if (!hc->names) hc->names = hl_alloc(512 * sizeof(char *));
	for (n = (char *)hc->names[h]; n; n = *(char **)(n - sizeof(char *)))
		if (!memcmp(n, s, len) && !n[len]) return n;
	n = (char *)hl_alloc(sizeof(char *) + len + 1) + sizeof(char *);
	memcpy(n, s, len);
	*(const char **)(n - sizeof(char *)) = hc->names[h];
	hc->names[h] = n;
	return n;
}

typedef struct { char *name; int offset, comps; } uniform_info;

struct hlsl_program {
	uint8_t *code_mem;	/* the allocation */
	uint8_t *code;		/* 16-byte aligned */
	uint32_t code_bytes;
	uint32_t *ctx_mem, *ctx;
	uint32_t ctx_bytes;
	uniform_info *uniforms;
	int nuniforms;
	struct hlsl_stats stats;
	int flags;
	/* drawing (hlsl_draw.c) */
	uint32_t *steps_out;
	uint32_t steps_bytes;
	uint32_t *pixels;
	int width, height, stage;
};

hlsl_program *hlsl_compile_ex(const char *source, const char *entry, int flags, char *error, int error_bytes)
{
	hlsl_c c;
	hlsl_program *p = 0;
	global *g;
	int n = 0;

	memset(&c, 0, sizeof c);
	hc = &c;
	c.err = error, c.err_bytes = error_bytes, c.flags = flags, c.src = source;
	if (error && error_bytes) error[0] = 0;
	if (setjmp(c.fail)) {
		if (p) hlsl_free(p);
		arena_free();
		hc = 0;
		return 0;
	}
	hl_lex();
	hl_parse();
	hl_gen(entry ? entry : "main");
	hl_back();
	p = calloc(1, sizeof *p);
	if (!p) hl_fail(0, "out of memory");
	p->flags = flags;
	p->code_bytes = (uint32_t)hc->nmc * 4;
	p->code_mem = malloc(p->code_bytes + 16);
	p->ctx_bytes = 16 + 2048;	/* (uniforms are at most 2 KB) */
	p->ctx_mem = malloc(p->ctx_bytes + 16);
	for (g = hc->globals; g; g = g->next)
		if (g->offset >= 0) n++;
	p->uniforms = calloc(n ? n : 1, sizeof(uniform_info));
	if (!p->code_mem || !p->ctx_mem || !p->uniforms) hl_fail(0, "out of memory");
	p->code = (uint8_t *)(((uintptr_t)p->code_mem + 15) & ~(uintptr_t)15);
	memcpy(p->code, hc->mc, p->code_bytes);
	p->ctx = (uint32_t *)(((uintptr_t)p->ctx_mem + 15) & ~(uintptr_t)15);
	memset(p->ctx, 0, p->ctx_bytes);
	for (g = hc->globals; g; g = g->next)
		if (g->offset >= 0) {
			uniform_info *u = &p->uniforms[p->nuniforms++];

			u->name = malloc(strlen(g->name) + 1);
			if (!u->name) hl_fail(0, "out of memory");
			strcpy(u->name, g->name);
			u->offset = g->offset, u->comps = hl_type_comps(g->ty);
		}
	p->stats.code_bytes = p->code_bytes;
	p->stats.constants = (uint32_t)hc->nks;
	p->stats.yields = (uint32_t)hc->yields;
	p->stats.texels = (uint32_t)hc->texels;
	p->stats.longest_step = (uint32_t)hc->longest;
	p->stats.spilled = (uint32_t)hc->spilled;
	p->stats.instructions = (uint32_t)hc->ncode;
	p->ctx[3] = 16 * (uint32_t)hc->texels;
	arena_free();
	hc = 0;
#if defined(__riscv)
	__asm__ volatile(".word 0x0000100f" ::: "memory");	/* fence.i: the new code fetched as written */
#endif
	return p;
}

hlsl_program *hlsl_compile(const char *source, const char *entry, char *error, int error_bytes)
{
	return hlsl_compile_ex(source, entry, 0, error, error_bytes);
}

void hlsl_free(hlsl_program *p)
{
	int k;

	if (!p) return;
	for (k = 0; k < p->nuniforms; k++) free(p->uniforms[k].name);
	free(p->uniforms);
	free(p->code_mem);
	free(p->ctx_mem);
	free(p);	/* (steps_out is the workers' memory, mcw_alloc: kept) */
}

int hlsl_uniform(const hlsl_program *p, const char *name, int *components)
{
	int k;

	for (k = 0; k < p->nuniforms; k++)
		if (!strcmp(p->uniforms[k].name, name)) {
			if (components) *components = p->uniforms[k].comps;
			return p->uniforms[k].offset - 16;
		}
	return -1;
}

static int set_words(hlsl_program *p, const char *name, const void *values, int count)
{
	int comps, at = hlsl_uniform(p, name, &comps);

	if (at < 0) return 0;
	if (count > comps) count = comps;
	memcpy((uint8_t *)p->ctx + 16 + at, values, 4 * (size_t)count);
	return 1;
}

int hlsl_set(hlsl_program *p, const char *name, const float *values, int count) { return set_words(p, name, values, count); }
int hlsl_set_int(hlsl_program *p, const char *name, const int32_t *values, int count) { return set_words(p, name, values, count); }

struct hlsl_stats hlsl_stats(const hlsl_program *p) { return p->stats; }
uint32_t *hlsl_ctx(hlsl_program *p) { return p->ctx; }
const uint8_t *hlsl_code(const hlsl_program *p, uint32_t *bytes) { if (bytes) *bytes = p->code_bytes; return p->code; }
uint32_t (*hlsl_step(const hlsl_program *p))(uint32_t, void *, uint32_t) { return (uint32_t (*)(uint32_t, void *, uint32_t))(uintptr_t)p->code; }

/* (hlsl_draw.c's view of the program) */
void *hlsl_internal(hlsl_program *p, uint32_t ***steps_out, uint32_t **steps_bytes, uint32_t ***pixels, int **wh, int **stage)
{
	*steps_out = &p->steps_out, *steps_bytes = &p->steps_bytes, *pixels = &p->pixels, *wh = &p->width, *stage = &p->stage;
	return p;
}
