/* /init of gpu_vm's Linux for the real-work bench (gpu_vm/bench_real/README.md): runs each program of the bench
   between two console lines "BENCH-START name" and "BENCH-END name", which the host stamps with its wall clock
   (stamp.py), and the guest's own clock beside them. Then "BENCH-DONE" and it waits. */
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* runs argv with stdin and stdout from files (NULL: the console's) */
static int run(const char *name, char *const argv[], const char *in, const char *out) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in) posix_spawn_file_actions_addopen(&fa, 0, in, O_RDONLY, 0);
    if (out) posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    printf("BENCH-START %s (guest %.3f s)\n", name, now());
    fflush(stdout);
    double t0 = now();
    pid_t pid;
    int st = -1;
    if (posix_spawn(&pid, argv[0], &fa, NULL, argv, environ) == 0) waitpid(pid, &st, 0);
    double t1 = now();
    printf("BENCH-END %s: exit %d, guest %.3f s\n", name, WIFEXITED(st) ? WEXITSTATUS(st) : -1, t1 - t0);
    fflush(stdout);
    posix_spawn_file_actions_destroy(&fa);
    return st;
}

static long size_of(const char *p) {
    struct stat s;
    return stat(p, &s) ? -1 : (long)s.st_size;
}

static int same(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int r = fa && fb;
    while (r) {
        int x = getc(fa), y = getc(fb);
        if (x != y) r = 0;
        if (x == EOF) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return r;
}

int main(void) {
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    printf("BENCH-INIT (guest %.3f s)\n", now());
    fflush(stdout);
    char *cm1[] = {"/bin/coremark", "0x0", "0x0", "0x66", ITER1, NULL};
    char *cm2[] = {"/bin/coremark", "0x0", "0x0", "0x66", ITER2, NULL};
    char *gz[] = {"/bin/minigzip", NULL};
    char *gunz[] = {"/bin/minigzip", "-d", NULL};
    run("coremark-" ITER1, cm1, NULL, NULL);
    run("coremark-" ITER2, cm2, NULL, NULL);
    run("gzip", gz, "/bench/input", "/tmp/input.gz");
    run("gunzip", gunz, "/tmp/input.gz", "/tmp/output");
    printf("BENCH gzip: %ld bytes to %ld, back %s\n", size_of("/bench/input"), size_of("/tmp/input.gz"),
           same("/bench/input", "/tmp/output") ? "the same" : "DIFFERENT");
    printf("BENCH-DONE\n");
    fflush(stdout);
    for (;;) pause();
}
