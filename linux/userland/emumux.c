/*
 * The console as several terminals (docs/console.md): a shell each on a pseudo-terminal, all
 * carried by the one serial console. Both ways, a byte 0x1e and a digit say which terminal
 * what follows belongs to. emuinit starts this when the host shows tabs (host flags, bit 1).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define TABS 4
#define SWITCH 0x1e

static int master[TABS] = {-1, -1, -1, -1};
static pid_t shell[TABS];

/* A shell on a terminal of its own, 80 x 30 as the host shows it. */
static void start(int n)
{
	struct winsize size = {30, 80, 0, 0};
	int m = posix_openpt(O_RDWR | O_NOCTTY);

	if (m < 0 || grantpt(m) || unlockpt(m)) {
		if (m >= 0)
			close(m);
		return;
	}
	ioctl(m, TIOCSWINSZ, &size);
	shell[n] = fork();
	if (shell[n] == 0) {
		int s;

		setsid();
		s = open(ptsname(m), O_RDWR);
		ioctl(s, TIOCSCTTY, 0);
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		for (s = 3; s < 32; s++)
			close(s);
		execl("/bin/sh", "sh", (char *)NULL);
		_exit(127);
	}
	master[n] = m;
}

int main(void)
{
	struct termios raw;
	char in[64], out[256], tag[2] = {SWITCH, '0'};
	int to = 0, from = -1, pending = 0, n, i;

	/* pseudo-terminals need their file system; then the console passes every byte as it is */
	mkdir("/dev/pts", 0755);
	mount("devpts", "/dev/pts", "devpts", 0, "ptmxmode=666");
	tcgetattr(0, &raw);
	cfmakeraw(&raw);
	tcsetattr(0, TCSANOW, &raw);
	start(0);
	for (;;) {
		struct pollfd fds[TABS + 1];

		fds[0].fd = 0;
		fds[0].events = POLLIN;
		for (i = 0; i < TABS; i++) {
			fds[i + 1].fd = master[i];	/* (a negative one is passed over) */
			fds[i + 1].events = POLLIN;
			fds[i + 1].revents = 0;
		}
		if (poll(fds, TABS + 1, -1) < 0)
			continue;
		if (fds[0].revents & POLLIN) {
			n = read(0, in, sizeof in);
			for (i = 0; i < n; i++) {
				if (pending && in[i] >= '0' && in[i] < '0' + TABS) {
					pending = 0;
					to = in[i] - '0';
					if (master[to] < 0)
						start(to);	/* a terminal's shell starts when the terminal is first asked for */
					continue;
				}
				if (!pending && in[i] == SWITCH) {
					pending = 1;
					continue;
				}
				pending = 0;
				if (master[to] >= 0)
					write(master[to], &in[i], 1);
			}
		}
		for (i = 0; i < TABS; i++) {
			if (master[i] < 0 || !(fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			n = read(master[i], out, sizeof out);
			if (n <= 0) {
				/* its shell has gone: another in its place, as init would give the console */
				close(master[i]);
				master[i] = -1;
				waitpid(shell[i], NULL, 0);
				start(i);
				continue;
			}
			if (from != i) {
				tag[1] = (char)('0' + i);
				write(1, tag, 2);
				from = i;
			}
			write(1, out, n);
		}
	}
}
