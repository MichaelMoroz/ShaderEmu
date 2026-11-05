/*
 * nxperf's score (docs/apps/bench.md, "The score"): each result over the reference machine's, the
 * geometric mean of a part's results, times 1,000, and the overall score the geometric mean of the
 * parts. The reference is the machine as the card model has it (rvc_cpu --pass-model, 3 November
 * 2026, the doc's run 1): there, every part and the whole are 1,000. Beside a run's score stand
 * other machines' of the same tests (perf_machines): a 486DX-33 of 1990 from published numbers and
 * estimates, the machine on an RTX 3090 (the harness) and in the world on an RTX 5090, rvc_cpu by the
 * host's clock on a Ryzen 9 5900X (one thread and 24) and a Ryzen 9 9950X3D (one and 16), and the
 * 9950X3D running the tests natively (perf_host.c: no workers, lanes or GPU there).
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

/* The tests' values on nine machines: the reference (the card model), a 486DX-33, the card (the harness on an RTX
 * 3090), rvc_cpu on one and 24 threads of a Ryzen 9 5900X, a Ryzen 9 9950X3D natively (nxperf-host, 4 November 2026),
 * the world on an RTX 5090 (0 until a run with perf_run_test's repeats is written here: a machine with no values is not
 * shown), and rvc_cpu on one and 16 threads of the 9950X3D (4 November 2026). 0: not measured there. The 486's are
 * docs/apps/bench.md's "A 486DX-33": a few published, the rest estimated from the processor's cycle counts; the
 * others are a run each (the same section). */
static const struct { const char *name; float v[9]; } values[] = {
	/* model, 486DX-33, RTX 3090, rvc_cpu 1 thread and 24 (5900X), 9950X3D natively, the world (RTX 5090), rvc_cpu 1 and 16 (9950X3D) */
	{"int-alu",     { 4.93f,    25.0f,    6.51f,    262.0f,   267.8f,   8922.0f,  0,        454.8f,   457.9f }},
	{"int-mul",     { 5.26f,    1.6f,     5.45f,    311.4f,   314.0f,   5591.0f,  0,        451.5f,   454.3f }},
	{"int-div",     { 1.05f,    0.75f,    1.01f,    63.87f,   64.53f,   924.1f,   0,        94.92f,   94.23f }},
	{"branch",      { 0.952f,   6.0f,     1.10f,    51.95f,   53.38f,   5570.0f,  0,        80.24f,   80.63f }},
	{"fp-add",      { 6.97f,    3.3f,     6.98f,    259.5f,   262.7f,   11130.0f, 0,        360.7f,   362.7f }},
	{"fp-mul",      { 3.46f,    2.0f,     3.07f,    96.26f,   98.51f,   7358.0f,  0,        143.5f,   143.9f }},
	{"fp-div",      { 3.49f,    0.45f,    2.00f,    95.74f,   97.35f,   2236.0f,  0,        138.4f,   138.0f }},
	{"fp-sqrt",     { 1.04f,    0.37f,    0.846f,   28.01f,   30.21f,   1244.0f,  0,        41.84f,   42.51f }},
	{"load-4",      { 5.99f,    18.0f,    6.34f,    337.9f,   343.8f,   49420.0f, 0,        516.1f,   528.8f }},
	{"load-16",     { 5.98f,    14.0f,    5.21f,    338.1f,   344.0f,   37510.0f, 0,        514.4f,   527.7f }},
	{"load-64",     { 5.99f,    9.0f,     5.48f,    331.0f,   322.2f,   19490.0f, 0,        513.4f,   523.8f }},
	{"load-4k",     { 5.96f,    5.0f,     6.33f,    310.6f,   288.2f,   2873.0f,  0,        494.6f,   500.1f }},
	{"lat-4k",      { 946.0f,   90.0f,    904.8f,   16.74f,   16.53f,   0.8941f,  0,        11.55f,   11.61f }},
	{"lat-64k",     { 957.5f,   150.0f,   985.7f,   16.97f,   16.74f,   1.373f,   0,        11.45f,   11.35f }},
	{"lat-256k",    { 956.7f,   250.0f,   1031.0f,  18.22f,   17.40f,   2.386f,   0,        11.43f,   11.39f }},
	{"store-seq",   { 3.80f,    25.0f,    5.72f,    411.8f,   444.2f,   87450.0f, 0,        662.7f,   667.7f }},
	{"wcache-hot",  { 2.32f,    8.0f,     1.98f,    115.2f,   117.2f,   21210.0f, 0,        177.5f,   176.7f }},
	{"wcache-cold", { 0.252f,   4.0f,     0.574f,   69.83f,   71.34f,   5440.0f,  0,        102.3f,   102.9f }},
	{"copy-loop",   { 3.77f,    12.0f,    4.69f,    371.6f,   367.8f,   79590.0f, 0,        570.3f,   577.2f }},
	{"memcpy",      { 961.8f,   15.0f,    596.8f,   4162.0f,  4869.0f,  77890.0f, 0,        8059.0f,  8236.0f }},
	{"memset",      { 810.6f,   25.0f,    779.8f,   23380.0f, 23750.0f, 193500.0f,0,        124391.0f,126376.0f }},
	{"gather",      { 0.583f,   5.0f,     0.495f,   30.27f,   30.72f,   3788.0f,  0,        47.97f,   48.27f }},
	{"mc-copy",     { 120.3f,   15.0f,    144.6f,   369.5f,   476.4f,   0,        0,        586.8f,   838.2f }},
	{"mc-fill",     { 112.0f,   25.0f,    118.6f,   634.4f,   856.9f,   0,        0,        954.5f,   1293.0f }},
	{"mc-0",        { 0.866f,   4.0f,     0.950f,   49.89f,   50.02f,   1624.0f,  0,        75.36f,   75.42f }},
	{"mc-1",        { 1.08f,    4.0f,     0.892f,   47.49f,   47.69f,   0,        0,        71.18f,   71.43f }},
	{"mc-4",        { 4.19f,    4.0f,     3.59f,    55.50f,   99.14f,   0,        0,        84.95f,   207.7f }},
	{"mc-16",       { 14.78f,   4.0f,     14.32f,   59.72f,   271.0f,   0,        0,        90.57f,   593.1f }},
	{"mc-64",       { 39.64f,   4.0f,     56.07f,   60.89f,   521.6f,   0,        0,        91.31f,   863.6f }},
	{"mc-255",      { 68.46f,   4.0f,     151.9f,   61.20f,   624.5f,   0,        0,        91.55f,   1101.0f }},
	{"mc-for",      { 169.5f,   0,        157.9f,   2107.0f,  1785.0f,  0,        0,        3519.0f,  3175.0f }},
	{"mc-call",     { 721.2f,   0,        647.1f,   62683.0f, 62035.0f, 0,        0,        96798.0f, 96319.0f }},
	{"handoff",     { 821.2f,   0,        1775.0f,  244002.0f,246103.0f,0,        0,        357862.0f,355889.0f }},
	{"counters",    { 5.60f,    0,        10.32f,   4.13f,    22.47f,   0,        0,        6.06f,    22.0f }},
	{"hybrid-big",  { 1.93f,    0,        3.63f,    371.1f,   329.1f,   0,        0,        546.9f,   518.8f }},
	{"hybrid-small",{ 1.94f,    0,        3.61f,    359.5f,   312.9f,   0,        0,        546.6f,   539.3f }},
	{"lanes",       { 1.11f,    0,        2.49f,    1.35f,    11.56f,   0,        0,        2.07f,    16.78f }},
	{"field",       { 0.020f,   0,        2.90f,    1.13f,    1.09f,    0,        0,        1.71f,    1.69f }},
	{"shade",       { 2.77f,    0,        18.78f,   4.63f,    19.91f,   0,        0,        7.06f,    29.54f }},
	{"dot",         { 2.21f,    1.3f,     1.74f,    71.82f,   72.39f,   10460.0f, 0,        109.7f,   108.2f }},
	{"axpy",        { 1.67f,    1.0f,     1.22f,    61.89f,   61.98f,   10300.0f, 0,        93.23f,   91.94f }},
	{"gemv",        { 2.12f,    1.2f,     1.77f,    69.58f,   69.44f,   10600.0f, 0,        105.9f,   104.7f }},
	{"gemm",        { 1.78f,    1.2f,     1.38f,    60.37f,   55.43f,   10100.0f, 0,        91.47f,   91.8f }},
	{"gemm-mc",     { 112.8f,   1.2f,     145.4f,   61.22f,   600.9f,   0,        0,        104.4f,   1175.0f }},
	{"lu",          { 1.11f,    0.8f,     0.893f,   44.81f,   44.91f,   5025.0f,  0,        67.26f,   66.43f }},
	{"cholesky",    { 1.28f,    0.8f,     1.13f,    49.28f,   49.45f,   7399.0f,  0,        74.06f,   73.06f }},
	{"trsv",        { 1.48f,    0.8f,     1.24f,    56.13f,   55.30f,   6725.0f,  0,        83.81f,   82.0f }},
	{"qr",          { 1.30f,    0.8f,     0.910f,   48.67f,   48.98f,   6166.0f,  0,        72.97f,   72.35f }},
	{"fft",         { 1.29f,    0.9f,     1.05f,    51.72f,   52.05f,   9536.0f,  0,        73.74f,   73.13f }},
	{"sort",        { 0.046f,   0.9f,     0.044f,   2.33f,    2.28f,    37.27f,   0,        3.17f,    3.23f }},
	{"hash",        { 0.266f,   1.5f,     0.332f,   19.36f,   19.29f,   1631.0f,  0,        28.18f,   28.32f }},
	{"crc32",       { 0.957f,   4.0f,     1.06f,    52.55f,   52.21f,   3875.0f,  0,        85.08f,   85.13f }},
	{"inflate",     { 0.367f,   1.0f,     0.382f,   18.44f,   18.14f,   776.7f,   0,        25.61f,   25.97f }},
	{"png-dec",     { 0.335f,   0.15f,    0.322f,   3.85f,    5.73f,    0,        0,        5.96f,    10.12f }},
	{"jpeg-enc",    { 0.649f,   0.12f,    0.766f,   1.20f,    7.86f,    0,        0,        1.95f,    12.66f }},
	{"jpeg-dec",    { 0.235f,   0.15f,    0.238f,   1.38f,    3.47f,    0,        0,        2.09f,    5.42f }},
	{"gpu-list",    { 342.8f,   0,        2669.0f,  83647.0f, 83981.0f, 0,        0,        269721.0f,264989.0f }},
	{"gpu-tris",    { 0.085f,   0,        0.075f,   5.18f,    5.16f,    0,        0,        7.66f,    7.58f }},
	{"gpu-quads",   { 0.017f,   0,        0.016f,   0.871f,   0.878f,   0,        0,        1.26f,    1.26f }},
	{"gpu-fill",    { 263.2f,   0,        903.9f,   41887.0f, 42055.0f, 0,        0,        79969.0f, 81798.0f }},
	{"gpu-copy",    { 835.7f,   0,        285.6f,   4933.0f,  4900.0f,  0,        0,        8390.0f,  8492.0f }},
};
#define VALUES ((int)(sizeof values / sizeof values[0]))

const struct perf_machine perf_machines[PERF_MACHINES] = {
	{"486DX-33", "1990, estimated", 1},
	{"RTX 3090, harness", "the card in rvc_harness, 255 workers", 2},
	{"RTX 5090, world", "the world on this PC, 255 workers", 6},
	{"rvc_cpu, 5900X, 1 thread", "rvc_cpu on a Ryzen 9 5900X, one thread", 3},
	{"rvc_cpu, 5900X, 24 threads", "rvc_cpu on a Ryzen 9 5900X, 24 threads", 4},
	{"rvc_cpu, 9950X3D, 1 thread", "rvc_cpu on a Ryzen 9 9950X3D, one thread", 7},
	{"rvc_cpu, 9950X3D, 16 threads", "rvc_cpu on a Ryzen 9 9950X3D, 16 threads", 8},
	{"9950X3D, native", "a Ryzen 9 9950X3D running the tests natively: no workers, lanes or GPU", 5},
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
