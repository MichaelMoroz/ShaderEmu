/* /init for libc-test (test.sh libc-test): runs every program in /t in name order, each with a time limit (killed
   after LIMIT seconds of the machine's time), prints FAIL lines with the test's own output before them, and a
   summary; then powers off. A test passes when it exits 0 (libc-test's t_status). */
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LIMIT 20

static char *env[] = { "PATH=/bin", "HOME=/", "TMPDIR=/tmp", 0 };

static int cmp(const void *a, const void *b) { return strcmp(*(char **)a, *(char **)b); }

int main(void)
{
	static char *names[512];
	int n = 0, passed = 0;
	mount("proc", "/proc", "proc", 0, 0);
	mount("tmpfs", "/tmp", "tmpfs", 0, 0);
	mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
	mkdir("/dev/shm", 01777);
	mount("tmpfs", "/dev/shm", "tmpfs", 0, 0);
	setvbuf(stdout, 0, _IOLBF, 0);
	DIR *d = opendir("/t");
	struct dirent *e;
	while (d && (e = readdir(d)) && n < 512)
		if (e->d_name[0] != '.') names[n++] = strdup(e->d_name);
	qsort(names, n, sizeof *names, cmp);
	printf("libc-test: %d programs\n", n);
	for (int i = 0; i < n; i++) {
		char path[300];
		snprintf(path, sizeof path, "/t/%s", names[i]);
		char *argv[] = { path, 0 };
		chdir("/tmp");
		pid_t pid = vfork();
		if (pid == 0) {
			execve(path, argv, env);
			_exit(127);
		}
		int status = 0, waited = 0;
		while (waitpid(pid, &status, WNOHANG) == 0) {
			if (waited++ >= LIMIT * 20) {
				kill(pid, SIGKILL);
				waitpid(pid, &status, 0);
				status = -1;
				break;
			}
			nanosleep(&(struct timespec){ 0, 50000000 }, 0);
		}
		if (status == -1)
			printf("FAIL %s: timeout\n", names[i]);
		else if (WIFSIGNALED(status))
			printf("FAIL %s: signal %d\n", names[i], WTERMSIG(status));
		else if (WEXITSTATUS(status))
			printf("FAIL %s: exit %d\n", names[i], WEXITSTATUS(status));
		else
			passed++;
	}
	printf("libc-test: %d passed, %d failed\n", passed, n - passed);
	sync();
	reboot(RB_POWER_OFF);
	for (;;) pause();
}
