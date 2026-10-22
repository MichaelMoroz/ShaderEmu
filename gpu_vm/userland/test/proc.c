/* Processes without an MMU: vfork and execve, waitpid and exit status, a pipe to a child, posix_spawn, getpid. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "check.h"

extern char **environ;

int main(int argc, char **argv)
{
	/* a child: `proc child CODE` prints its arguments and exits with CODE */
	if (argc > 1 && !strcmp(argv[1], "child")) {
		printf("child %d of %d: %s %s\n", (int)getpid(), (int)getppid(), argc > 2 ? argv[2] : "", getenv("GVM_CHILD") ? getenv("GVM_CHILD") : "-");
		fflush(stdout);
		return argc > 2 ? atoi(argv[2]) : 0;
	}
	if (argc > 1 && !strcmp(argv[1], "cat")) {   /* stdin to stdout, upper case */
		char buf[64];
		int n;
		while ((n = read(0, buf, sizeof buf)) > 0) {
			for (int i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
			write(1, buf, n);
		}
		return 0;
	}

	pid_t me = getpid();
	CHECK(me > 1);

	/* vfork + execve, and the exit code */
	char *cargv[] = { argv[0], "child", "7", 0 };
	char *cenv[] = { "GVM_CHILD=from-env", 0 };
	pid_t pid = vfork();
	if (pid == 0) {
		execve("/bin/proc", cargv, cenv);
		_exit(127);
	}
	CHECK(pid > 0);
	int status = -1;
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);

	/* vfork + _exit with no exec */
	pid = vfork();
	if (pid == 0) _exit(42);
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);

	/* exec of what does not exist */
	pid = vfork();
	if (pid == 0) {
		execve("/bin/nothing", cargv, cenv);
		_exit(errno == ENOENT ? 99 : 98);
	}
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 99);

	/* a pipe to a child and one back */
	int to[2], from[2];
	CHECK(pipe(to) == 0 && pipe(from) == 0);
	char *catv[] = { argv[0], "cat", 0 };
	pid = vfork();
	if (pid == 0) {
		dup2(to[0], 0);
		dup2(from[1], 1);
		close(to[0]); close(to[1]); close(from[0]); close(from[1]);
		execve("/bin/proc", catv, environ);
		_exit(127);
	}
	close(to[0]);
	close(from[1]);
	CHECK_INT(write(to[1], "shout this", 10), 10);
	close(to[1]);
	char buf[64] = { 0 };
	int got = 0, n;
	while ((n = read(from[0], buf + got, sizeof buf - 1 - got)) > 0) got += n;
	close(from[0]);
	CHECK_STR(buf, "SHOUT THIS");
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	/* posix_spawn (a CLONE_VM | CLONE_VFORK child on a stack of its own) */
	char *sargv[] = { argv[0], "child", "3", 0 };
	CHECK_INT(posix_spawn(&pid, "/bin/proc", 0, 0, sargv, environ), 0);
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 3);

	/* a child killed by a signal */
	pid = vfork();
	if (pid == 0) {
		char *kargv[] = { argv[0], "child", "0", 0 };
		execve("/bin/proc", kargv, cenv);
		_exit(127);
	}
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFEXITED(status));
	pid = vfork();
	if (pid == 0) {
		kill(getpid(), SIGTERM);   /* the child dies here (not raise: it would take the parent's tid) */
		_exit(1);
	}
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

	/* fork does not exist without an MMU */
	CHECK(fork() == -1 && errno == ENOSYS);

	CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
	return DONE("proc");
}
