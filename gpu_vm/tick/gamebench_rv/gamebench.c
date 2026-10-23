/* The game bench's kernels (gpu_vm/tick/gamebench.h, GAMEBENCH.md) for the RISC-V machine, in C for its gcc: the same
 * data (the same generator, the same order), the same work and the same scenes, workers through programs/mc/mcw.c as
 * the games have them. Frames a second by the guest's clock:
 *
 *   gamebench NAME [SECONDS [FRAMES]]     NAME: g_logic g_verts g_ray1 g_q3 g_hl g_ray g_q3p
 *
 * prints "gamebench: NAME F frames in T s: X a second" every SECONDS (3 by default), three times after a first
 * second of warm-up. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mcw.h"

#include "gamebench_kern.h"

/* ---- the scenes: gamebench.h's g_scene_of */
typedef struct { int kind, size; } job;
typedef struct { const char *name; int E0, idle, rayW; job jobs[4]; int njobs; int rw, rh, rounds, spin; } scene;
static const scene scenes[] = {
	{"g_logic", 512, 0, 0, {{0}}, 0, 256, 192, 1, 0},
	{"g_verts", 0, 0, 0, {{0}}, 0, 256, 192, 1, 0},
	{"g_ray1", 0, 0, 0, {{0}}, 0, 64, 32, 1, 0},
	{"g_q3", 64, 2, 0, {{1, 256}, {2, 2048}, {1, 64}}, 3, 256, 192, 1, 0},
	{"g_q3p", 32, 2, 0, {{1, 16}, {2, 128}}, 2, 256, 192, 3, 2},
	{"g_hl", 512, 40, 0, {{1, 128}, {2, 2048}, {2, 2048}, {1, 32}}, 4, 256, 192, 1, 0},
	{"g_ray", 16, 0, 191, {{0}}, 0, 256, 192, 1, 0},
};
static uint32_t logic_job(uint32_t instance, uint32_t E) { return logic_frame(instance, E); }
static volatile uint32_t g_stop;
static uint32_t spin_job(uint32_t a, uint32_t b) { uint32_t n = 0; while (!g_stop) n++; return n + a + b; }   /* a spinner: a poll to the budget */

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	const scene *S = 0;
	for (unsigned i = 0; i < sizeof scenes / sizeof scenes[0]; i++)
		if (argc > 1 && !strcmp(argv[1], scenes[i].name)) S = &scenes[i];
	if (!S) { printf("gamebench NAME [SECONDS]: g_logic g_verts g_ray1 g_q3 g_hl g_ray\n"); return 1; }
	double every = argc > 2 ? atof(argv[2]) : 3;
	int nframes = argc > 3 ? atoi(argv[3]) : 0;   /* (a count of frames, then out: instructions a frame under qemu) */
	int lone = S->njobs == 0 && S->rayW == 0, maxE = S->E0, busy = S->njobs;
	for (int k = 0; k < busy; k++) if (S->jobs[k].kind == 1 && S->jobs[k].size > maxE) maxE = S->jobs[k].size;
	LP.E = S->E0;
	RW = S->rw; RH = S->rh;
	insts = (inst *)mcw_alloc(sizeof(inst) * (1 + busy));
	fb = (uint32_t *)mcw_alloc((unsigned)(RW * RH * 4));
	vout = (uint32_t (*)[4096][4])mcw_alloc(sizeof *vout * (1 + busy));
	memset(insts, 0, sizeof(inst) * (1 + busy));
	data(1 + busy, maxE ? maxE : 1);
	int want = busy + S->spin + S->idle + S->rayW, got = 0;
	if (want) {
		got = mcw_open(want);
		if (got < want) printf("gamebench: %d workers of %d\n", got, want);
		mcw_touch(insts, sizeof(inst) * (1 + busy));
		mcw_touch(fb, (unsigned)(RW * RH * 4));
		mcw_touch(vout, sizeof *vout * (1 + busy));
		mcw_touch_code((const void *)logic_frame, 4096);
	}
	ray_stride = S->rayW ? S->rayW : 1;
	for (int k = busy + 1; k <= busy + S->spin; k++) mcw_post(k, spin_job, 0, 0);
	double unit = S->E0 ? S->E0 * S->rounds : (double)(RW * RH / 32);
	if (!strcmp(S->name, "g_verts")) unit = 1;
	double t0 = now(), mark = t0 + 1;
	uint32_t at = 0, rounds = 0;
	for (uint32_t frame = 0;; frame++) {
		if (frame) {
			for (int k = 1; k <= S->rayW; k++) mcw_wait(k);
			for (int k = 1; k <= busy; k++) mcw_wait(k);
		}
		for (int k = 1; k <= busy; k++) {
			const job *j = &S->jobs[k - 1];
			if (j->kind == 1) { insts[k].frame = frame; mcw_post(k, logic_job, (uint32_t)(uintptr_t)&insts[k], (uint32_t)j->size); }
			else mcw_post(k, verts_job, (uint32_t)(uintptr_t)vout[k], (uint32_t)j->size);
		}
		for (int k = 1; k <= S->rayW; k++) mcw_post(k, ray_job, (uint32_t)(k - 1), frame);
		if (S->E0) { insts[0].frame = frame; logic_frame((uint32_t)(uintptr_t)&insts[0], (uint32_t)S->E0); units += (uint32_t)S->E0; }
		else if (!strcmp(S->name, "g_verts")) { verts_job((uint32_t)(uintptr_t)vout[0], 4096); units++; }
		else if (lone) { ray_stride = 1; ray_job(0, frame); units += (uint32_t)(RW * RH / 32); }
		if (nframes && frame + 1 >= (uint32_t)nframes) break;
		double t = now();
		if (t >= mark) {
			if (rounds) printf("gamebench: %s %.1f frames in %.2f s: %.2f a second\n", S->name, (units - at) / unit, t - t0, (units - at) / unit / (t - t0));
			fflush(stdout);
			if (++rounds > 3) break;
			at = units; t0 = t; mark = t + every;
		}
	}
	for (int k = 1; k <= S->rayW; k++) mcw_wait(k);
	for (int k = 1; k <= busy; k++) mcw_wait(k);
	g_stop = 1;
	for (int k = busy + 1; k <= busy + S->spin; k++) mcw_wait(k);
	printf("gamebench: done\n");
	return 0;
}
