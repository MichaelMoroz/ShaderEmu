/*
 * nxperf's score (docs/apps/bench.md, "The score"): each result over the reference machine's, the
 * geometric mean of a part's results, times 1,000, and the overall score the geometric mean of the
 * parts. The reference is the machine as the card model has it (rvc_cpu --pass-model, 3 November
 * 2026, the doc's run 1): there, every part and the whole are 1,000. Beside a run's score stand
 * three machines' of the same tests (perf_machines): a 486DX-33 of 1990 from published numbers and
 * estimates, the machine on an RTX 3090, and rvc_cpu at full speed.
 */
#include <math.h>
#include <string.h>
#include "perf.h"

const char *const perf_parts[PERF_PARTS] = {"single core", "FPU", "memory", "multicore", "lanes", "numerical", "media", "GPU"};

/* A test's part: the cpu group's integer tests, its floats, then the groups as they are. */
int perf_part_of(int test)
{
	const struct perf_test *t = &perf_tests[test];
	static const char *const groups[] = {"cpu", "cpu", "memory", "cores", "lanes", "numbers", "media", "gpu"};
	int p;

	if (!strcmp(t->group, "cpu")) return strncmp(t->name, "fp-", 3) ? 0 : 1;
	for (p = 2; p < PERF_PARTS; p++)
		if (!strcmp(t->group, groups[p])) return p;
	return -1;
}

/* Smaller is better for a time an operation. */
static int lower_better(int test)
{
	return strncmp(perf_tests[test].unit, "ns/", 3) == 0;
}

/* The tests' values on four machines: the reference (the card model), a 486DX-33, the card, rvc_cpu at full speed.
 * 0: not measured there. The 486's are docs/apps/bench.md's "A 486DX-33": a few published, the rest estimated from
 * the processor's cycle counts; the card's and rvc_cpu's are a run each (the same section). */
static const struct { const char *name; float v[4]; } values[] = {
	/*                 model    486DX-33  RTX 3090  rvc_cpu */
	{"int-alu",     { 5.06f,    25.0f,    6.35f,    3.01f }},
	{"int-mul",     { 5.59f,    1.6f,     5.41f,    3.25f }},
	{"int-div",     { 1.07f,    0.75f,    1.01f,    0.649f }},
	{"branch",      { 0.976f,   6.0f,     1.09f,    0.586f }},
	{"fp-add",      { 7.25f,    3.3f,     6.10f,    4.27f }},
	{"fp-mul",      { 3.62f,    2.0f,     3.21f,    2.14f }},
	{"fp-div",      { 3.52f,    0.45f,    2.01f,    2.14f }},
	{"fp-sqrt",     { 1.04f,    0.37f,    0.821f,   0.649f }},
	{"load-4",      { 6.17f,    18.0f,    6.48f,    3.67f }},
	{"load-16",     { 6.17f,    14.0f,    5.56f,    3.70f }},
	{"load-64",     { 6.17f,    9.0f,     5.82f,    3.67f }},
	{"load-4k",     { 6.11f,    5.0f,     6.24f,    3.70f }},
	{"lat-4k",      { 932.3f,   90.0f,    924.7f,   1561.0f }},
	{"lat-64k",     { 932.3f,   150.0f,   984.2f,   1541.0f }},
	{"lat-256k",    { 932.3f,   250.0f,   1015.0f,  1559.0f }},
	{"store-seq",   { 3.52f,    25.0f,    5.79f,    5.19f }},
	{"wcache-hot",  { 2.37f,    8.0f,     1.99f,    1.42f }},
	{"wcache-cold", { 0.224f,   4.0f,     0.596f,   0.812f }},
	{"copy-loop",   { 3.52f,    12.0f,    4.65f,    4.68f }},
	{"memcpy",      { 187.2f,   15.0f,    5.82f,    998.6f }},
	{"memset",      { 749.0f,   25.0f,    7.41f,    2996.0f }},
	{"gather",      { 0.598f,   5.0f,     0.491f,   0.358f }},
	{"mc-copy",     { 127.1f,   15.0f,    125.6f,   121.2f }},
	{"mc-fill",     { 114.6f,   25.0f,    117.2f,   102.8f }},
	{"mc-0",        { 0.888f,   4.0f,     0.924f,   0.532f }},
	{"mc-1",        { 0.946f,   4.0f,     0.888f,   0.545f }},
	{"mc-4",        { 3.73f,    4.0f,     3.46f,    2.18f }},
	{"mc-16",       { 14.15f,   4.0f,     13.78f,   8.72f }},
	{"mc-64",       { 46.04f,   4.0f,     52.56f,   34.21f }},
	{"mc-255",      { 104.7f,   4.0f,     147.8f,   123.8f }},
	{"mc-for",      { 180.2f,   0,        160.9f,   119.9f }},
	{"mc-call",     { 338.6f,   0,        630.3f,   802.1f }},
	{"handoff",     { 712.6f,   0,        1734.0f,  2778.0f }},
	{"counters",    { 7.87f,    0,        9.41f,    8.47f }},
	{"hybrid-big",  { 1.74f,    0,        3.37f,    5.80f }},
	{"hybrid-small",{ 1.74f,    0,        3.78f,    5.77f }},
	{"lanes",       { 1.76f,    0,        2.79f,    1.73f }},
	{"field",       { 0.020f,   0,        1.80f,    0.012f }},
	{"shade",       { 3.31f,    0,        16.38f,   2.60f }},
	{"dot",         { 2.28f,    1.3f,     1.77f,    1.36f }},
	{"axpy",        { 1.70f,    1.0f,     1.23f,    1.12f }},
	{"gemv",        { 2.20f,    1.2f,     1.75f,    1.31f }},
	{"gemm",        { 1.83f,    1.2f,     1.38f,    1.09f }},
	{"gemm-mc",     { 152.0f,   1.2f,     147.9f,   136.6f }},
	{"lu",          { 1.14f,    0.8f,     0.901f,   0.690f }},
	{"cholesky",    { 1.30f,    0.8f,     1.06f,    0.785f }},
	{"trsv",        { 1.52f,    0.8f,     1.30f,    0.909f }},
	{"qr",          { 1.33f,    0.8f,     0.936f,   0.796f }},
	{"fft",         { 1.32f,    0.9f,     1.06f,    0.862f }},
	{"sort",        { 0.047f,   0.9f,     0.044f,   0.028f }},
	{"hash",        { 0.261f,   1.5f,     0.333f,   0.212f }},
	{"crc32",       { 0.977f,   4.0f,     1.09f,    0.585f }},
	{"inflate",     { 0.370f,   1.0f,     0.381f,   0.226f }},
	{"png-dec",     { 0.333f,   0.15f,    0.320f,   0.216f }},
	{"jpeg-enc",    { 0.731f,   0.12f,    0.742f,   0.570f }},
	{"jpeg-dec",    { 0.250f,   0.15f,    0.239f,   0.160f }},
	{"gpu-list",    { 490.2f,   0,        1653.0f,  1639.0f }},
	{"gpu-tris",    { 0.090f,   0,        0.079f,   0.059f }},
	{"gpu-quads",   { 0.019f,   0,        0.017f,   0.012f }},
	{"gpu-fill",    { 399.2f,   0,        771.0f,   683.9f }},
	{"gpu-copy",    { 187.2f,   0,        5.76f,    499.3f }},
};
#define VALUES ((int)(sizeof values / sizeof values[0]))

const struct perf_machine perf_machines[PERF_MACHINES] = {
	{"486DX-33", "1990, estimated", 1},
	{"RTX 3090", "the card, 255 workers", 2},
	{"rvc_cpu", "full speed, all threads", 3},
};

static float value_on(int test, int column)
{
	int k;

	for (k = 0; k < VALUES; k++)
		if (!strcmp(values[k].name, perf_tests[test].name)) return values[k].v[column];
	return 0;
}

/* One result against the reference: above 1 is faster. 0: none. */
float perf_ratio(int test, float value)
{
	float ref = value_on(test, 0);

	if (ref <= 0 || value <= 0) return 0;
	return lower_better(test) ? ref / value : value / ref;
}

/* The parts and the whole from the values given (0 for a test not run or whose check failed). A part with no
 * result is 0, and the whole is of the parts there are; `counted` (may be NULL) how many results each part had. */
float perf_score(const float *value, float *parts, int *counted)
{
	float logs[PERF_PARTS] = {0}, all = 0;
	int n[PERF_PARTS] = {0}, i, p, have = 0;

	for (i = 0; i < perf_count; i++) {
		float r = perf_ratio(i, value[i]);

		p = perf_part_of(i);
		if (p < 0 || r <= 0) continue;
		logs[p] += logf(r), n[p]++;
	}
	for (p = 0; p < PERF_PARTS; p++) {
		parts[p] = n[p] ? 1000.0f * expf(logs[p] / (float)n[p]) : 0;
		if (counted) counted[p] = n[p];
		if (n[p]) all += logf(parts[p]), have++;
	}
	return have ? expf(all / (float)have) : 0;
}

/* A machine's of perf_machines: its parts and whole by the same arithmetic. */
float perf_machine_score(int m, float *parts, int *counted)
{
	float v[128];
	int i;

	for (i = 0; i < perf_count && i < 128; i++) v[i] = value_on(i, perf_machines[m].column);
	return perf_score(v, parts, counted);
}
