/* xtime: the machine's time of a few things, by CLOCK_MONOTONIC (on gvm_cpu and the card: passes of 0.2 ms).
     xtime exec N PROG [ARGS...]    vfork, execve and wait N times: ms each
     xtime read FILE [BS]           the file read whole N times by read(BS, default 64 KB): ms and MB/s
     xtime copy KB                  memcpy of KB within the heap: ms and MB/s
     xtime touch KB                 malloc and write a byte a page */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "exec")) {
		int n = atoi(argv[2]);
		double t0 = now();
		for (int i = 0; i < n; i++) {
			pid_t pid = vfork();
			if (pid == 0) {
				int fd = open("/dev/null", O_WRONLY);
				dup2(fd, 1);
				execv(argv[3], argv + 3);
				_exit(127);
			}
			int st;
			waitpid(pid, &st, 0);
			if (!WIFEXITED(st) || WEXITSTATUS(st) == 127) { printf("xtime: %s failed\n", argv[3]); return 1; }
		}
		printf("xtime exec %s: %.1f ms each (%d)\n", argv[3], (now() - t0) / n, n);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "read")) {
		size_t bs = argc > 3 ? strtoul(argv[3], 0, 0) : 65536;
		char *buf = malloc(bs);
		double t0 = now();
		long total = 0;
		int fd = open(argv[2], O_RDONLY);
		if (fd < 0) { perror(argv[2]); return 1; }
		for (ssize_t r; (r = read(fd, buf, bs)) > 0;) total += r;
		double ms = now() - t0;
		printf("xtime read %s: %ld bytes in %.1f ms, %.2f MB/s (blocks of %zu)\n", argv[2], total, ms,
		       total / 1048576.0 / (ms / 1e3), bs);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "copy")) {
		size_t n = strtoul(argv[2], 0, 0) * 1024;
		char *a = malloc(n), *b = malloc(n);
		memset(a, 1, n);
		memset(b, 2, n);
		double t0 = now();
		for (int i = 0; i < 4; i++) memcpy(i & 1 ? a : b, i & 1 ? b : a, n);
		double ms = now() - t0;
		printf("xtime copy: 4 x %zu KB in %.1f ms, %.2f MB/s\n", n / 1024, ms, 4 * n / 1048576.0 / (ms / 1e3));
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "touch")) {
		size_t n = strtoul(argv[2], 0, 0) * 1024;
		double t0 = now();
		char *a = malloc(n);
		for (size_t i = 0; i < n; i += 4096) a[i] = 1;
		printf("xtime touch: %zu KB in %.1f ms\n", n / 1024, now() - t0);
		return 0;
	}
	fprintf(stderr, "usage: xtime exec N PROG [ARGS] | read FILE [BS] | copy KB | touch KB\n");
	return 2;
}
