/* The tests' /init: runs each test program (vfork and execve), prints whether it passed, then a summary line, and
   powers the machine off (gvm_cpu exits). gpu_vm/userland/test.sh reads the summary. */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char *env[] = { "PATH=/bin", "HOME=/", "GVM_TEST=yes", "TERM=dumb", 0 };

static int run(char **argv)
{
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	pid_t pid = vfork();
	if (pid == 0) {
		execve(argv[0], argv, env);
		_exit(127);
	}
	int status = -1;
	if (pid < 0 || waitpid(pid, &status, 0) != pid) {
		printf("== %s: could not run it\n", argv[0]);
		return 0;
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	long ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
	int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	if (ok)
		printf("== %s: PASS (%ld ms)\n", argv[0], ms);
	else if (WIFSIGNALED(status))
		printf("== %s: FAIL, killed by signal %d\n", argv[0], WTERMSIG(status));
	else
		printf("== %s: FAIL, exit %d\n", argv[0], WEXITSTATUS(status));
	fflush(stdout);
	return ok;
}

int main(int argc, char **argv)
{
	static char *tests[][5] = {
		{ "/bin/hello", "first", 0 },
		{ "/bin/libc", 0 },
		{ "/bin/files", 0 },
		{ "/bin/proc", 0 },
		{ "/bin/signals", 0 },
		{ "/bin/timeenv", "one", "two words", "", 0 },
		{ "/bin/threads", 0 },
		{ "/bin/math", 0 },
		{ "/bin/cxx_eh", 0 },
	};
	int n = sizeof tests / sizeof *tests, passed = 0;
	mount("proc", "/proc", "proc", 0, 0);
	setvbuf(stdout, 0, _IOLBF, 0);
	printf("gvm-userland-test: %d programs\n", n);
	for (int i = 0; i < n; i++) passed += run(tests[i]);
	printf("gvm-userland-test: %d passed, %d failed\n", passed, n - passed);
	fflush(stdout);
	sync();
	reboot(RB_POWER_OFF);
	for (;;) pause();
}
