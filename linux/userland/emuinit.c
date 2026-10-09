/*
 * The image's init: what upstream's /rvcinit shell script does (a writeable overlay over the
 * read-only root, /proc and /sys, a shell on the console), as one static program. The script
 * starts a dozen processes, each of which maps the C library a page fault at a time.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void say(const char* text) { write(1, text, strlen(text)); }

/* What the host asks for at boot: bits 0 and 1 of the machine's host flags (the word at
 * 0x8700003c, docs/gpu.md), the desktop and the console as tabs. A machine without the GPU
 * device has no such word and neither. */
static int host_wants(void) {
    int fd = open("/dev/gpu", O_RDWR), wanted = 0;
    const unsigned* words;

    if (fd < 0) {
        return 0;
    }
    words = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);   /* the control words */
    if (words != MAP_FAILED) {
        wanted = words[0x3c / 4] & 3;   /* bit 1: the host shows the console as tabs (emumux) */
        munmap((void*)words, 4096);
    }
    close(fd);
    return wanted;
}

static void must(int result, const char* what) {
    if (result < 0) {
        perror(what);
    }
}

#ifdef MARKS
static void mark(const char* what) {
    char line[64];
    int fd = open("/proc/uptime", O_RDONLY), n = read(fd, line, sizeof line - 1);
    close(fd);
    line[n > 0 ? n : 0] = 0;
    line[strcspn(line, " ")] = 0;
    say("MARK ");
    say(what);
    say(" ");
    say(line);
    say("\n");
}
#else
#define mark(what)
#endif

static const char banner[] =
    "                  _ _\n"
    "                 | (_)\n"
    " _ ____   _____  | |_ _ __  _   ___  __\n"
    "| '__\\ \\ / / __| | | | '_ \\| | | \\ \\/ /\n"
    "| |   \\ V / (__  | | | | | | |_| |>  <\n"
    "|_|    \\_/ \\___| |_|_|_| |_|\\__,_/_/\\_\\ \n"
    "\n"
    "\n"
    "                              by _pi_\n"
    "\n";

int main(void) {
    char line[64];
    int fd, n;

    say("\n> Welcome to userland!\n> Setting up overlay mount for writeable root\n");
    must(mount("tmpfs", "/tmp", "tmpfs", 0, NULL), "mount /tmp");
    mkdir("/tmp/upper", 0755);
    mkdir("/tmp/work", 0755);
    mkdir("/tmp/newroot", 0755);
    must(mount("overlay", "/tmp/newroot", "overlay", 0, "lowerdir=/,upperdir=/tmp/upper,workdir=/tmp/work"), "mount overlay");
    must(mount("/dev", "/tmp/newroot/dev", NULL, MS_BIND, NULL), "bind /dev");
    must(chroot("/tmp/newroot"), "chroot");
    must(chdir("/"), "chdir");

    say("> Entered overlay chroot, mounting /proc & /sys\n");
    must(mount("proc", "/proc", "proc", 0, NULL), "mount /proc");
    must(mount("sys", "/sys", "sysfs", 0, NULL), "mount /sys");

    fd = open("/proc/uptime", O_RDONLY);
    n = fd < 0 ? 0 : read(fd, line, sizeof line - 1);
    close(fd);
    line[n > 0 ? n : 0] = 0;
    line[strcspn(line, " \n")] = 0;
    say("> Startup took: ");
    say(line);
    say("s\n");
    say(banner);
    say("> Starting login shell on TTY /dev/hvc0\n");

    setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin:/usr/local/bin", 1);
    setenv("HOME", "/root", 1);   // not "/": the shell would show the prompt there as "~ # "
    setenv("TERM", "vt100", 0);
    int wants = host_wants();
    const char* shell = wants & 2 && access("/usr/bin/emumux", X_OK) == 0 ? "/usr/bin/emumux" : "/bin/sh";
    if (wants & 1 && access("/usr/bin/nx", X_OK) == 0 && fork() == 0) {
        /* the window system, the bar and a terminal; the console keeps its shell */
        setsid();
        execl("/bin/sh", "sh", "/usr/bin/nx", (char*)NULL);
        _exit(127);
    }
    for (;;) {
        mark("before fork");
        pid_t pid = fork();
        if (pid == 0) {
            mark("child");
            /* a session of its own with the console as its terminal, as getty would set up */
            setsid();
            mark("after setsid");
            fd = open("/dev/hvc0", O_RDWR);
            mark("after open");
            ioctl(fd, TIOCSCTTY, 0);
            dup2(fd, 0);
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) {
                close(fd);
            }
            mark("before exec");
            execl(shell, shell, (char*)NULL);
            _exit(127);
        }
        while (wait(NULL) != pid) {
        }
        say("> Restarting login shell\n");
    }
}
