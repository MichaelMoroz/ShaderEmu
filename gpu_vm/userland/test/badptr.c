/* System calls given pointers outside RAM must fail with EFAULT, not fault in the kernel (an oops that kills the
   caller): gpu_vm/linux/arch/gpuvm/include/asm/uaccess.h's access_ok. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>

static int n, ok;

static void expect(const char *what, long r)
{
	n++;
	if (r == -1 && errno == EFAULT) ok++;
	else printf("badptr: %s gave %ld (errno %d), not EFAULT\n", what, r, r == -1 ? errno : 0);
}

int main(void)
{
	void *past_ram = (void *)0x0a000000, *boot = (void *)0x100, *dev = (void *)0x016b8000, *ops = (void *)0xf0000000;
	int fd = open("/etc/hostname", O_RDONLY);
	expect("write from past RAM", syscall(SYS_write, 1, past_ram, 4));
	expect("write from the device window", syscall(SYS_write, 1, dev, 4));
	expect("read into the boot area", syscall(SYS_read, fd, boot, 4));
	expect("read into the operations region", syscall(SYS_read, fd, ops, 4));
	expect("open of a name past RAM", syscall(SYS_openat, AT_FDCWD, past_ram, O_RDONLY));
	expect("statx into NULL", syscall(SYS_statx, AT_FDCWD, "/", 0, 0x7ff, (void *)0));
	expect("uname into 8", syscall(SYS_uname, (void *)8));
	expect("clock_gettime into past RAM", syscall(SYS_clock_gettime64, CLOCK_MONOTONIC, past_ram));
	expect("pipe into the boot area", syscall(SYS_pipe2, boot, 0));
	/* and the good ones still work */
	char buf[16] = { 0 };
	struct utsname u;
	int good = pread(fd, buf, sizeof buf - 1, 0) > 0 && uname(&u) == 0 && !strcmp(u.machine, "gpuvm");
	printf("badptr: %d of %d bad pointers gave EFAULT; good pointers %s\n", ok, n, good ? "work" : "FAIL");
	return ok == n && good ? 0 : 1;
}
