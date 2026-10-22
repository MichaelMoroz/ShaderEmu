/* File I/O on the initramfs (rootfs) and a tmpfs: open, read, write, lseek, stat, directories, rename, unlink,
   stdio files, pipes and dup. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

static void tree(const char *dir)
{
	char path[128], buf[4096];
	snprintf(path, sizeof path, "%s/gvm_test", dir);
	CHECK_INT(mkdir(path, 0755), 0);
	struct stat st;
	CHECK(stat(path, &st) == 0 && S_ISDIR(st.st_mode));

	snprintf(path, sizeof path, "%s/gvm_test/a.txt", dir);
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	CHECK(fd >= 0);
	for (int i = 0; i < (int)sizeof buf; i++) buf[i] = 'a' + i % 26;
	for (int k = 0; k < 10; k++) CHECK_INT(write(fd, buf, sizeof buf), sizeof buf);
	CHECK_INT(close(fd), 0);
	CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 40960 && (st.st_mode & 0777) == 0644);

	fd = open(path, O_RDWR);
	CHECK(fd >= 0);
	CHECK_INT(lseek(fd, 12345, SEEK_SET), 12345);
	char c[4];
	CHECK_INT(read(fd, c, 3), 3);
	CHECK(c[0] == 'a' + 12345 % 4096 % 26 && c[2] == 'a' + 12347 % 4096 % 26);
	CHECK_INT(lseek(fd, -1, SEEK_END), 40959);
	CHECK_INT(write(fd, "XYZ", 3), 3);
	CHECK_INT(lseek(fd, 0, SEEK_CUR), 40962);
	CHECK_INT(pwrite(fd, "Q", 1, 5), 1);
	CHECK_INT(pread(fd, c, 1, 5), 1);
	CHECK(c[0] == 'Q');
	CHECK_INT(ftruncate(fd, 100), 0);
	CHECK(fstat(fd, &st) == 0 && st.st_size == 100);
	close(fd);

	/* stdio */
	snprintf(path, sizeof path, "%s/gvm_test/b.txt", dir);
	FILE *f = fopen(path, "w");
	CHECK(f != 0);
	for (int i = 0; i < 100; i++) fprintf(f, "line %d %s\n", i, i % 2 ? "odd" : "even");
	CHECK_INT(fclose(f), 0);
	f = fopen(path, "r");
	CHECK(f != 0);
	char line[64];
	int n = 0, ok = 1;
	while (fgets(line, sizeof line, f)) {
		char want[64];
		snprintf(want, sizeof want, "line %d %s\n", n, n % 2 ? "odd" : "even");
		if (strcmp(line, want)) ok = 0;
		n++;
	}
	CHECK(ok && n == 100);
	CHECK(feof(f));
	fclose(f);
	f = fopen(path, "a+");
	fputs("appended\n", f);
	fseek(f, -9, SEEK_END);
	CHECK(fgets(line, sizeof line, f) && !strcmp(line, "appended\n"));
	fclose(f);

	/* directory listing */
	snprintf(path, sizeof path, "%s/gvm_test", dir);
	DIR *d = opendir(path);
	CHECK(d != 0);
	int seen = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, "a.txt")) seen |= 1;
		if (!strcmp(e->d_name, "b.txt")) seen |= 2;
	}
	closedir(d);
	CHECK_INT(seen, 3);

	/* rename, link, symlink, unlink, rmdir */
	char p2[128], p3[128];
	snprintf(path, sizeof path, "%s/gvm_test/a.txt", dir);
	snprintf(p2, sizeof p2, "%s/gvm_test/c.txt", dir);
	CHECK_INT(rename(path, p2), 0);
	CHECK(access(path, F_OK) == -1 && errno == ENOENT);
	CHECK_INT(access(p2, R_OK), 0);
	snprintf(p3, sizeof p3, "%s/gvm_test/l", dir);
	CHECK_INT(symlink("c.txt", p3), 0);
	char target[64] = { 0 };
	CHECK_INT(readlink(p3, target, sizeof target - 1), 5);
	CHECK_STR(target, "c.txt");
	CHECK(stat(p3, &st) == 0 && st.st_size == 100);
	CHECK_INT(unlink(p3), 0);
	CHECK_INT(unlink(p2), 0);
	snprintf(path, sizeof path, "%s/gvm_test/b.txt", dir);
	CHECK_INT(unlink(path), 0);
	snprintf(path, sizeof path, "%s/gvm_test", dir);
	CHECK_INT(rmdir(path), 0);
	CHECK(stat(path, &st) == -1 && errno == ENOENT);
}

int main(void)
{
	/* the initramfs is the root: a ramfs */
	tree("");
	/* a tmpfs */
	mkdir("/tmp", 01777);
	CHECK_INT(mount("tmpfs", "/tmp", "tmpfs", 0, "size=1m"), 0);
	tree("/tmp");
	CHECK_INT(umount("/tmp"), 0);

	/* a file the initramfs carries */
	FILE *f = fopen("/etc/test.txt", "r");
	CHECK(f != 0);
	char line[64];
	CHECK(f && fgets(line, sizeof line, f) && !strcmp(line, "a file of the initramfs\n"));
	if (f) fclose(f);

	/* getcwd, chdir */
	char cwd[64];
	CHECK_INT(chdir("/etc"), 0);
	CHECK(getcwd(cwd, sizeof cwd) && !strcmp(cwd, "/etc"));
	CHECK(access("test.txt", R_OK) == 0);
	chdir("/");

	/* pipe, dup2 */
	int p[2];
	CHECK_INT(pipe(p), 0);
	CHECK_INT(write(p[1], "through the pipe", 16), 16);
	char buf[32] = { 0 };
	CHECK_INT(read(p[0], buf, sizeof buf), 16);
	CHECK_STR(buf, "through the pipe");
	int d = dup(p[1]);
	CHECK(d > p[1]);
	CHECK_INT(dup2(p[0], 20), 20);
	close(p[1]);
	close(d);
	CHECK_INT(read(20, buf, sizeof buf), 0);   /* every writer closed: end of file */
	close(p[0]);
	close(20);
	CHECK(fcntl(1, F_GETFD) >= 0);
	CHECK(open("/no/such/file", O_RDONLY) == -1 && errno == ENOENT);

	return DONE("files");
}
