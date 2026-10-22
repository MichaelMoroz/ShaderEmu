/* Signals: raise, handlers by signal() and sigaction (SA_SIGINFO, the context), the mask, sigpending, alarm and
   pause, kill, SIGCHLD, an interrupted read, setjmp/longjmp and sigsetjmp/siglongjmp out of a handler. */
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "check.h"

static volatile sig_atomic_t got_usr1, got_usr2, got_alrm, got_chld, info_code, info_pid, ctx_ok;

static void on_usr1(int s) { got_usr1 += s == SIGUSR1; }
static void on_alrm(int s) { got_alrm++; }
static void on_chld(int s) { got_chld++; }

static void on_usr2(int s, siginfo_t *si, void *uc)
{
	got_usr2++;
	info_code = si->si_code;
	info_pid = si->si_pid;
	ucontext_t *u = uc;
	ctx_ok = u && u->uc_mcontext.__gregs[0] != 0 && u->uc_mcontext.__gregs[2] != 0;   /* a pc and an sp */
}

static jmp_buf jb;
static sigjmp_buf sjb;
static void jump_out(int s) { longjmp(jb, s); }
static void sjump_out(int s) { siglongjmp(sjb, s); }

static int depth(int n, int *count)
{
	(*count)++;
	if (n == 0) longjmp(jb, 77);
	return depth(n - 1, count) + 1;
}

int main(void)
{
	/* signal() and raise() */
	CHECK(signal(SIGUSR1, on_usr1) != SIG_ERR);
	CHECK_INT(raise(SIGUSR1), 0);
	CHECK_INT(got_usr1, 1);
	CHECK_INT(kill(getpid(), SIGUSR1), 0);
	CHECK_INT(got_usr1, 2);

	/* sigaction with SA_SIGINFO */
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_usr2;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	CHECK_INT(sigaction(SIGUSR2, &sa, 0), 0);
	CHECK_INT(kill(getpid(), SIGUSR2), 0);
	CHECK_INT(got_usr2, 1);
	CHECK_INT(info_code, SI_USER);
	CHECK_INT(info_pid, getpid());
	CHECK(ctx_ok);
	struct sigaction old;
	CHECK(sigaction(SIGUSR2, 0, &old) == 0 && old.sa_sigaction == on_usr2 && (old.sa_flags & SA_SIGINFO));

	/* the mask: blocked, pending, delivered when unblocked */
	sigset_t set, oldset, pend;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	CHECK_INT(sigprocmask(SIG_BLOCK, &set, &oldset), 0);
	raise(SIGUSR1);
	CHECK_INT(got_usr1, 2);
	CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1));
	CHECK_INT(sigprocmask(SIG_SETMASK, &oldset, 0), 0);
	CHECK_INT(got_usr1, 3);

	/* SIG_IGN */
	signal(SIGUSR1, SIG_IGN);
	raise(SIGUSR1);
	CHECK_INT(got_usr1, 3);

	/* alarm and pause */
	signal(SIGALRM, on_alrm);
	alarm(1);
	pause();
	CHECK_INT(got_alrm, 1);

	/* a read interrupted by a handler without SA_RESTART: EINTR */
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_alrm;
	sigaction(SIGALRM, &sa, 0);
	int p[2];
	pipe(p);
	struct itimerval it = { { 0, 0 }, { 0, 200000 } };
	setitimer(ITIMER_REAL, &it, 0);
	char c;
	errno = 0;
	CHECK(read(p[0], &c, 1) == -1 && errno == EINTR);
	CHECK_INT(got_alrm, 2);
	/* SIGCHLD: a vfork child writes a byte and exits (the parent goes on only after it) */
	signal(SIGCHLD, on_chld);
	pid_t pid = vfork();
	if (pid == 0) {
		struct timespec ts = { 0, 100000000 };
		nanosleep(&ts, 0);
		write(p[1], "x", 1);
		_exit(0);
	}
	CHECK_INT(read(p[0], &c, 1), 1);
	CHECK(c == 'x');
	int status;
	CHECK_INT(waitpid(pid, &status, 0), pid);
	CHECK_INT(got_chld, 1);
	close(p[0]);
	close(p[1]);

	/* setjmp and longjmp */
	volatile int count = 0;
	int r = setjmp(jb);
	if (r == 0) {
		int c2 = 0;
		depth(20, &c2);
		CHECK(0);
	}
	CHECK_INT(r, 77);
	r = setjmp(jb);
	if (r == 0) longjmp(jb, 0);
	CHECK_INT(r, 1);
	/* out of a handler with longjmp: the mask stays (SIGUSR1 blocked while its handler ran) */
	signal(SIGUSR1, jump_out);
	r = setjmp(jb);
	if (r == 0) { raise(SIGUSR1); CHECK(0); }
	CHECK_INT(r, SIGUSR1);
	sigprocmask(SIG_SETMASK, 0, &set);
	CHECK(sigismember(&set, SIGUSR1));
	sigprocmask(SIG_SETMASK, &oldset, 0);
	/* sigsetjmp(, 1) and siglongjmp: the mask comes back */
	signal(SIGUSR1, sjump_out);
	r = sigsetjmp(sjb, 1);
	if (r == 0) { count++; raise(SIGUSR1); CHECK(0); }
	CHECK_INT(r, SIGUSR1);
	CHECK_INT(count, 1);
	sigprocmask(SIG_SETMASK, 0, &set);
	CHECK(!sigismember(&set, SIGUSR1));

	/* sigsuspend */
	signal(SIGALRM, on_alrm);
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigprocmask(SIG_BLOCK, &set, &oldset);
	it.it_value.tv_usec = 100000;
	setitimer(ITIMER_REAL, &it, 0);
	sigset_t wait_set;
	sigemptyset(&wait_set);
	int before = got_alrm;
	sigsuspend(&wait_set);
	CHECK_INT(got_alrm, before + 1);
	sigprocmask(SIG_SETMASK, &oldset, 0);

	return DONE("signals");
}
