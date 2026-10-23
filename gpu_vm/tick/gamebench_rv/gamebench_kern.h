/* The game bench's kernels in C (gpu_vm/tick/GAMEBENCH.md), the work of gamebench.h's g_emit_logic, g_emit_verts and
 * g_emit_ray with its data: gamebench.c (the RISC-V machine, gcc) and gvm_lone.c (gpu_vm, clang) include it. The caller
 * sets insts, fb and vout before data(). */
#include <stdint.h>
#include <string.h>

#ifdef __riscv
#define FREG(x) "+f"(x)
#else
#define FREG(x) "+r"(x)   /* (gpu_vm: floats in the one register file) */
#endif
#define NOINLINE __attribute__((noinline))
typedef struct { uint64_t s; } rng_t;
static uint32_t rnext(rng_t *g) { g->s ^= g->s << 13; g->s ^= g->s >> 7; g->s ^= g->s << 17; return (uint32_t)g->s; }
static uint32_t rbelow(rng_t *g, uint32_t n) { return rnext(g) % n; }
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ---- the logic: entities of 128 bytes */
typedef struct ent { uint32_t type, flags; float pos[3], vel[3]; int32_t health; uint32_t counter; int32_t target; uint32_t key, trace, misc[19]; } ent;
typedef struct node { float nx, d; uint32_t child[2]; } node;
typedef struct hnode { uint32_t e, next, frame, key; } hnode;
struct inst;
typedef void (*think_fn)(struct inst *I, int e, ent *p);
typedef struct inst {
	ent ents[1024], snap[1024];
	uint32_t hash[1024];
	think_fn table[4];
	uint32_t frame, mask, pad[2];
	hnode heap[4096];
} inst;
typedef struct { int E, T, D, snap, ai; } logic_p;
static logic_p LP = {0, 1, 6, 4, 4};
static uint32_t g_zero;   /* a global the logic reads (gamebench.h's G_CONST + 28) */

static node world[4096];
static float cst[8];
static uint32_t spheres[16][8];
static uint32_t verts[4096][4];
static float bones[32][16];
static inst *insts;
static uint32_t *fb;
static uint32_t (*vout)[4096][4];
static volatile uint32_t g_sink;

static NOINLINE uint32_t trace(float x, uint32_t *last)
{
	const node *n = world;
	uint32_t r = 0;
	for (int d = LP.D; d; d--) {
		float t = x * n->nx - n->d;
		uint32_t side = t < 0.0f;
		if (side) r ^= (uint32_t)d;
		uint32_t c = n->child[side];
		r += c;
		n = &world[c];
	}
	*last = (uint32_t)(uintptr_t)n;
	return r;
}
typedef struct { uint32_t w[8]; } trace_t;
static NOINLINE void pm_trace(float x, trace_t *tr)
{
	uint32_t last, r = trace(x, &last);
	tr->w[0] = r; tr->w[1] = fbits(x); tr->w[2] = last; tr->w[3] = (uint32_t)LP.D;
	tr->w[4] = r ^ fbits(x); tr->w[5] = last >> 4; tr->w[6] = r + 1; tr->w[7] = 0;
}
static NOINLINE void copy_words(uint32_t *d, const uint32_t *s, uint32_t bytes)
{
	do {
		uint32_t a = s[0], b = s[1], c = s[2], e = s[3];
		d[0] = a; d[1] = b; d[2] = c; d[3] = e;
		s += 4; d += 4; bytes -= 16;
	} while (bytes);
}
static NOINLINE void link(inst *I, int e, ent *p)
{
	uint32_t h = ((fbits(p->pos[0]) ^ fbits(p->pos[1])) * 0x9e3779b1u) >> 22;
	hnode *n = &I->heap[(uint32_t)e * 4 + (I->frame & 3)];
	n->e = (uint32_t)e; n->next = I->hash[h] + g_zero; n->frame = I->frame; n->key = h;
	I->hash[h] = (uint32_t)(uintptr_t)n;
}
static NOINLINE void think_phys(inst *I, int e, ent *p, int traces)
{
	const float *C = cst;
	float x = p->vel[0] * C[0] + p->pos[0], y = p->vel[1] * C[0] + p->pos[1], z = p->vel[2] * C[0] + p->pos[2];
	float vz = p->vel[2] - C[1];
	if (z < 0.0f) { z = 0.0f; vz = C[2]; }
	if (!(x < 512.0f)) x -= 1024.0f;
	p->pos[0] = x; p->pos[1] = y; p->pos[2] = z; p->vel[2] = vz;
	float vx = p->vel[0], vy = p->vel[1];
	__asm__("" : FREG(vx), FREG(vy));   /* (stored back as gamebench.h does) */
	p->vel[0] = vx; p->vel[1] = vy;
	uint32_t acc = 0;
	trace_t tr;
	for (int k = traces; k; k--) {
		pm_trace(x, &tr);
		for (int i = 0; i < 8; i += 2) { acc += tr.w[i]; acc ^= tr.w[i + 1]; }
	}
	p->trace = acc + fbits(x);
	uint32_t c = ++p->counter;
	if (!(c & 15)) p->health -= 3;
	(void)I; (void)e;
}
static NOINLINE void think0(inst *I, int e, ent *p) { think_phys(I, e, p, LP.T); }
static NOINLINE void think3(inst *I, int e, ent *p) { think_phys(I, e, p, 2 * LP.T); }
static NOINLINE void think1(inst *I, int e, ent *p)
{
	float own = p->pos[0], best = cst[3];
	int32_t target = -1;
	uint32_t junk = 0;
	for (int k = LP.ai; k; k--) {
		uint32_t j = ((uint32_t)k * 37 + (uint32_t)e) & I->mask;
		const ent *q = &I->ents[j];
		if (q->flags & 1) continue;
		float dx = __builtin_fabsf(q->pos[0] - own);
		if (dx < best) { best = dx; target = (int32_t)j; }
		junk += q->type;
	}
	g_sink = junk;
	p->target = target;
	if (p->health < 10) p->flags |= 4;
	p->counter++;
}
static NOINLINE void think2(inst *I, int e, ent *p)
{
	uint32_t c = ++p->counter;
	if (!(c & 63)) p->health += 25;
	(void)I; (void)e;
}
static NOINLINE void dispatch(inst *I, int e, ent *p)
{
	if ((p->flags | g_zero) & 1) return;
	char *pad = __builtin_alloca(64 * ((uint32_t)e & 3));   /* (the think's frame at a depth of e & 3: gamebench.h) */
	__asm__ volatile("" : : "r"(pad) : "memory");
	I->table[p->type](I, e, p);
	link(I, e, p);
	if (!(((uint32_t)e + I->frame) & (uint32_t)(LP.snap - 1))) copy_words((uint32_t *)&I->snap[e], (const uint32_t *)p, 128);
}
static uint32_t units;   /* entities, on the first core */
static NOINLINE uint32_t logic_frame(uint32_t instance, uint32_t E)
{
	inst *I = (inst *)(uintptr_t)instance;
	I->table[0] = think0; I->table[1] = think1; I->table[2] = think2; I->table[3] = think3;
	I->mask = E - 1;
	ent *p = I->ents;
	for (uint32_t e = 0; e < E; e++, p++) dispatch(I, (int)e, p);
	return 0;
}

/* ---- vertices */
static NOINLINE uint32_t verts_job(uint32_t out_, uint32_t count)
{
	uint32_t (*out)[4] = (uint32_t (*)[4])(uintptr_t)out_, clipped = 0;
	const uint32_t (*v)[4] = verts;
	for (uint32_t i = 0; i < count; i++, v++, out++) {
		float x, y, z;
		memcpy(&x, &(*v)[0], 4); memcpy(&y, &(*v)[1], 4); memcpy(&z, &(*v)[2], 4);
		const float *m = bones[(*v)[3]];
		float ox = m[0] * x + m[1] * y + m[2] * z + m[3];
		float oy = m[4] * x + m[5] * y + m[6] * z + m[7];
		float oz = m[8] * x + m[9] * y + m[10] * z + m[11];
		float l = ox * m[12] + oy * m[12];
		l = __builtin_fminf(__builtin_fmaxf(l, 0.0f), 255.0f);
		uint32_t col = (uint32_t)(int32_t)l;
		if (oz < 1.0f) { col |= 0x100; clipped++; }
		(*out)[0] = fbits(ox); (*out)[1] = fbits(oy); (*out)[2] = fbits(oz); (*out)[3] = col;
	}
	return clipped;
}

/* ---- rays: chunks of 32 pixels */
static int RW = 256, RH = 192, RS = 16, ray_stride = 1;
static NOINLINE uint32_t ray_job(uint32_t first, uint32_t frame)
{
	uint32_t chunks = (uint32_t)(RW * RH / 32), sum = 0, lw2 = 0;
	volatile float rec[2];
	float sink = 0, step = 4.0f / (float)RW, cy0 = 2.0f * (float)RH / (float)RW;
	while ((1 << lw2) < RW) lw2++;
	for (uint32_t ch = first; ch < chunks; ch += (uint32_t)ray_stride) {
		for (uint32_t p = ch * 32, n = 32; n; n--, p++) {
			float dx = (float)(int32_t)(p & (uint32_t)(RW - 1)) * step - 2.0f;
			float dy = (float)(int32_t)(p >> lw2) * step - cy0;
			float a = dx * dx + dy * dy + 1.0f, best = cst[3];
			uint32_t color = 0, v;
			const uint32_t *s = spheres[0];
			for (int k = RS; k; k--, s += 8) {
				float cx, cy, cz, cc;
				memcpy(&cx, &s[0], 4); memcpy(&cy, &s[1], 4); memcpy(&cz, &s[2], 4); memcpy(&cc, &s[3], 4);
				float b = cx * dx + cy * dy + cz;
				float disc = b * b - a * cc;
				if (disc < 0.0f) continue;
				sink += __builtin_sqrtf(disc);
				float t = b - disc * cst[6];
				rec[0] = t; rec[1] = b;
				if (t < best) { best = t; color = s[4]; }
			}
			if (color && best < cst[3]) {
				sink += 1.0f / __builtin_sqrtf(a);
				float f = __builtin_fmaxf(__builtin_fminf(best * 16.0f, 255.0f), 0.0f);
				v = ((uint32_t)(int32_t)f * color) >> 8;
			} else v = (p ^ frame) & 255;
			fb[p] = v;
			sum += v;
		}
	}
	g_sink = (uint32_t)sink;
	return sum;
}

/* ---- the data: gamebench.h's g_data, call for call */
static void data(int instances, int E)
{
	rng_t g = {0x5eed1234u};
	for (uint32_t i = 0; i < 4096; i++) {
		float nx = (float)((int)rbelow(&g, 33) - 16) / 16.0f, d = (float)((int)rbelow(&g, 801) - 400);
		world[i].nx = nx; world[i].d = d; world[i].child[0] = (2 * i + 1) & 4095; world[i].child[1] = (2 * i + 2) & 4095;
	}
	const float c[8] = {1.0f / 32, 0.75f, 20.0f, 1e30f, 1.0f / 64, 2.0f, 0.125f, 0};
	memcpy(cst, c, sizeof c);
	for (int s = 0; s < 16; s++) {
		float cx = (float)((int)rbelow(&g, 65) - 32) / 8.0f;
		float cy = (float)((int)rbelow(&g, 49) - 24) / 16.0f;
		float cz = (float)(3 + rbelow(&g, 6));
		float r = (float)(16 + rbelow(&g, 17)) / 8.0f;
		float cc = cx * cx + cy * cy + cz * cz - r * r;
		spheres[s][0] = fbits(cx); spheres[s][1] = fbits(cy); spheres[s][2] = fbits(cz); spheres[s][3] = fbits(cc);
		spheres[s][4] = 0x10101u * (1 + rbelow(&g, 255));
	}
	for (uint32_t v = 0; v < 4096; v++) {
		for (int k = 0; k < 3; k++) verts[v][k] = fbits((float)((int)rbelow(&g, 201) - 100) / 8);
		verts[v][3] = rbelow(&g, 32);
	}
	for (uint32_t b = 0; b < 32; b++)
		for (int k = 0; k < 16; k++) bones[b][k] = (float)((int)rbelow(&g, 17) - 8) / 8;
	for (int in = 0; in < instances; in++)
		for (int e = 0; e < E; e++) {
			uint32_t w[32];
			for (int k = 0; k < 32; k++) w[k] = rnext(&g);
			w[0] = rbelow(&g, 4); w[1] = rbelow(&g, 8) == 0 ? 1 : 0;
			for (int k = 2; k <= 4; k++) w[k] = fbits((float)((int)rbelow(&g, 1001) - 500) / 4);
			w[4] = fbits((float)rbelow(&g, 400) / 4);
			for (int k = 5; k <= 7; k++) w[k] = fbits((float)((int)rbelow(&g, 201) - 100) / 4);
			w[8] = 50 + rbelow(&g, 100); w[9] = rbelow(&g, 64);
			memcpy(&insts[in].ents[e], w, 128);
		}
}
