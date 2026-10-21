/*
 * hlslc: an HLSL shader compiled for the lanes and nothing run (docs/system/hlsl.md). A mistake is
 * said as the C compiler says one, "FILE:LINE: error: what", for the IDE (docs/apps/ide.md).
 *   hlslc FILE.hlsl [ENTRY]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hlsl.h"

int main(int argc, char **argv)
{
	const char *entry = argc > 2 ? argv[2] : "main";
	char error[256], *source;
	struct hlsl_stats st;
	hlsl_program *p;
	FILE *f;
	long n;

	if (argc < 2) { fprintf(stderr, "hlslc FILE.hlsl [ENTRY]\n"); return 2; }
	if (!(f = fopen(argv[1], "rb"))) { printf("%s:0: error: cannot be read\n", argv[1]); return 1; }
	fseek(f, 0, SEEK_END), n = ftell(f), fseek(f, 0, SEEK_SET);
	source = malloc((size_t)n + 1);
	if (!source || fread(source, 1, (size_t)n, f) != (size_t)n) { printf("%s:0: error: cannot be read\n", argv[1]); return 1; }
	source[n] = 0;
	fclose(f);
	if (!(p = hlsl_compile(source, entry, error, sizeof error))) {
		/* the compiler's "line N: what" */
		if (!strncmp(error, "line ", 5) && strstr(error, ": "))
			printf("%s:%d: error: %s\n", argv[1], atoi(error + 5), strstr(error, ": ") + 2);
		else
			printf("%s:1: error: %s\n", argv[1], error);
		return 1;
	}
	st = hlsl_stats(p);
	printf("%s: %u bytes of RISC-V, %u places a lane may stop, %u texels an index, the longest step %u instructions\n", argv[1],
	       st.code_bytes, st.yields, st.texels, st.longest_step);
	hlsl_free(p);
	return 0;
}
