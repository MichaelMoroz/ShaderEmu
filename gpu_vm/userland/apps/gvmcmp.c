/*
 * gvmcmp: how close a program's output is to its reference (gpu_vm/userland/apps/README.md, "Checks"), for the checks
 * of float results that the card's special functions make other bits of than the reference machine:
 *   gvmcmp ppm A.ppm B.ppm        PSNR of two binary PPM pictures of one size, and the largest difference of a channel
 *   gvmcmp pcm16 A.raw B.raw      SNR of two 16-bit sounds (B the reference), and the largest difference of a sample
 * prints "gvmcmp: N.N dB, at most M apart" (99.0 dB: the same), or a line saying why not.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load(const char *path, long *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *d;

	if (!f) return 0;
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	d = malloc(*n ? *n : 1);
	if (d && fread(d, 1, *n, f) != (size_t)*n) free(d), d = 0;
	fclose(f);
	return d;
}

/* the pixels of a binary PPM: their offset, or -1 */
static long ppm_pixels(const unsigned char *d, long n, int *w, int *h)
{
	long at = 2;
	int field[3], k;

	if (n < 2 || d[0] != 'P' || d[1] != '6') return -1;
	for (k = 0; k < 3; k++) {
		while (at < n && (d[at] == ' ' || d[at] == '\n' || d[at] == '\r' || d[at] == '\t' || d[at] == '#')) {
			if (d[at] == '#') while (at < n && d[at] != '\n') at++;
			else at++;
		}
		field[k] = 0;
		while (at < n && d[at] >= '0' && d[at] <= '9') field[k] = field[k] * 10 + (d[at++] - '0');
	}
	*w = field[0], *h = field[1];
	return at + 1;
}

int main(int argc, char **argv)
{
	unsigned char *a, *b;
	long na, nb, i, count, offa = 0, offb = 0;
	double se = 0, energy = 0;
	long most = 0;
	int pcm;

	if (argc != 4 || (strcmp(argv[1], "ppm") && strcmp(argv[1], "pcm16"))) {
		fprintf(stderr, "usage: gvmcmp ppm|pcm16 OUTPUT REFERENCE\n");
		return 2;
	}
	pcm = !strcmp(argv[1], "pcm16");
	if (!(a = load(argv[2], &na)) || !(b = load(argv[3], &nb))) {
		printf("gvmcmp: cannot read %s\n", a ? argv[3] : argv[2]);
		return 1;
	}
	if (!pcm) {
		int wa, ha, wb, hb;

		offa = ppm_pixels(a, na, &wa, &ha), offb = ppm_pixels(b, nb, &wb, &hb);
		if (offa < 0 || offb < 0 || wa != wb || ha != hb || na - offa != nb - offb) {
			printf("gvmcmp: not two pictures of one size\n");
			return 1;
		}
	} else if (na != nb) {
		printf("gvmcmp: %ld bytes against %ld\n", na, nb);
		return 1;
	}
	count = pcm ? na / 2 : na - offa;
	for (i = 0; i < count; i++) {
		long x = pcm ? (short)(a[2 * i] | a[2 * i + 1] << 8) : a[offa + i];
		long y = pcm ? (short)(b[2 * i] | b[2 * i + 1] << 8) : b[offb + i];
		long d = x > y ? x - y : y - x;

		se += (double)d * d;
		energy += pcm ? (double)y * y : 255.0 * 255.0;
		if (d > most) most = d;
	}
	printf("gvmcmp: %.1f dB, at most %ld apart\n", se == 0 ? 99.0 : 10 * log10(energy / se), most);
	return 0;
}
