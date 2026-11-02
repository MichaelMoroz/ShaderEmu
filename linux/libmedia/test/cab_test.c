/* cab.h's unpacking on the build computer (workers as threads, test/mcw_host.c), for comparing with 7-Zip:
 *   cab_test INSTALLER OUT [WORKERS [BATCH]]     every file of the cabinet into OUT, a line each: size, CRC-32, name
 * (linux/libmedia/build.sh makes $WORK/src/libmedia/cab_test-host; docs/system/install.md, "Checked"). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include "../../apps/cab.h"

static void
make_dirs(char *path)
{
	char *s;

	for (s = path + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(path, 0755);
			*s = '/';
		}
}

int
main(int argc, char **argv)
{
	struct cab c;
	struct stat st;
	struct timespec t0, t1;
	const char *why;
	uint8_t *f;
	uint32_t i;
	int fd;

	if (argc < 3) {
		fprintf(stderr, "usage: cab_test INSTALLER OUT [WORKERS [BATCH]]\n");
		return 2;
	}
	if ((fd = open(argv[1], O_RDONLY)) < 0 || fstat(fd, &st) ||
	    (f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		return 2;
	}
	if ((why = cab_open(&c, f, (uint32_t)st.st_size))) {
		fprintf(stderr, "%s: %s\n", argv[1], why);
		return 2;
	}
	c.workers = argc > 3 ? atoi(argv[3]) : 8;
	if (argc > 4)
		c.batch = (uint32_t)atoi(argv[4]);
	c.sums = 1;
	mcw_open(c.workers);
	printf("cabinet at %u, %u bytes, %u folders, %u files\n", c.at, c.length, c.nfolders, c.nfiles);
	for (i = 0; i < c.nfiles; i++) {
		char path[1024], *s;

		snprintf(path, sizeof path, "%s/%s", argv[2], c.files[i].name[0] == '\\' ? c.files[i].name + 1 : c.files[i].name);
		for (s = path; *s; s++)
			if (*s == '\\')
				*s = '/';
		make_dirs(path);
		if ((c.files[i].fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			return 2;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	why = cab_unpack(&c);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	for (i = 0; i < c.nfiles; i++) {
		close(c.files[i].fd);
		printf("%10u %08x %s\n", c.files[i].size, c.files[i].crc, c.files[i].name);
	}
	printf("%s: %llu bytes in %u rounds, %u of %u block sums checked, %.3f s%s%s\n", why ? "FAILED" : "unpacked",
	       (unsigned long long)c.made, c.rounds, c.sums_seen - c.bad_sums, c.sums_seen,
	       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, why ? ": " : "", why ? why : "");
	return why ? 1 : 0;
}
