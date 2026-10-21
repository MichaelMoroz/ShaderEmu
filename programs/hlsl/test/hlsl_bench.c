/*
 * hlsl_bench: a shader compiled in the machine and drawn into memory, its sum and its times.
 * The sum is the host's (hlsl_test with the same arguments) where floats are exact (rvc_cpu).
 *   hlsl_bench FILE.hlsl WIDTH HEIGHT PICTURES [uniform=v,v,v ...]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hlsl.h"
#include "mcw.h"

static unsigned ms_now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)t.tv_sec * 1000u + (unsigned)t.tv_nsec / 1000000u;
}

int main(int argc, char **argv)
{
	FILE *f;
	long n;
	char *src, err[256];
	int w, h, pictures, a, k, workers = 0;
	uint32_t *pixels, sum = 2166136261u, i;
	hlsl_program *p;
	struct hlsl_stats st;
	unsigned t0;

	if (argc < 5) { printf("hlsl_bench FILE WIDTH HEIGHT PICTURES [name=v,...]\n"); return 1; }
	if (!(f = fopen(argv[1], "rb"))) { printf("hlsl_bench: no %s\n", argv[1]); return 1; }
	fseek(f, 0, SEEK_END), n = ftell(f), fseek(f, 0, SEEK_SET);
	src = malloc(n + 1);
	if (fread(src, 1, n, f) != (size_t)n) return 1;
	src[n] = 0;
	fclose(f);
	w = atoi(argv[2]), h = atoi(argv[3]), pictures = atoi(argv[4]);
	t0 = ms_now();
	p = hlsl_compile(src, "main", err, sizeof err);
	if (!p) { printf("hlsl_bench: %s\n", err); return 1; }
	st = hlsl_stats(p);
	printf("hlsl_bench: compiled in %u ms: %u bytes, %u IR, %u yields, %u texels, longest step %u, %u spilled\n", ms_now() - t0,
	       st.code_bytes, st.instructions, st.yields, st.texels, st.longest_step, st.spilled);
	for (a = 5; a < argc; a++) {
		char name[64];
		float v[16];
		int c = 0;
		const char *s = strchr(argv[a], '=');

		if (!s) continue;
		snprintf(name, sizeof name, "%.*s", (int)(s - argv[a]), argv[a]);
		for (s++; *s && c < 16; c++) {
			v[c] = strtof(s, (char **)&s);
			if (*s == ',') s++;
		}
		if (!hlsl_set(p, name, v, c)) printf("hlsl_bench: no uniform %s\n", name);
	}
	if (!mcw_field_lanes() && getenv("HLSL_WORKERS")) workers = mcw_open(atoi(getenv("HLSL_WORKERS")));
	printf("hlsl_bench: the field's %d lanes, %d workers\n", mcw_field_lanes(), workers);
	pixels = mcw_alloc((unsigned)(w * h * 4));
	mcw_touch(pixels, (unsigned)(w * h * 4));
	for (k = 0; k < pictures; k++) {
		t0 = ms_now();
		hlsl_draw(p, pixels, w, h);
		if (mcw_field_lanes()) {
			struct mcw_steps_report r = mcw_field_steps_report();

			printf("hlsl_bench: picture %d in %u ms (the field: %u rounds, %u steps too long)\n", k, ms_now() - t0, r.rounds,
			       r.too_long);
		} else
			printf("hlsl_bench: picture %d in %u ms\n", k, ms_now() - t0);
	}
	for (i = 0; i < (uint32_t)(w * h); i++) sum = (sum ^ pixels[i]) * 16777619u;
	printf("hlsl_bench: sum %08x\n", sum);
	if (getenv("HLSL_PPM") && (f = fopen(getenv("HLSL_PPM"), "wb"))) {	/* the picture, to look at */
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (i = 0; i < (uint32_t)(w * h); i++) fputc(pixels[i] >> 16 & 255, f), fputc(pixels[i] >> 8 & 255, f), fputc(pixels[i] & 255, f);
		fclose(f);
	}
	if (workers) mcw_close();
	return 0;
}
