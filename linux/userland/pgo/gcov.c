/*
 * The compiler's counters for a profile-guided build of any program (-fprofile-generate), written to the console
 * as lines beginning `GC` (the toolchain's own libgcov is of another float ABI; an empty libgcov.a is put in the
 * toolchain's way at the link). linux/tdawn/profile.py sums the lines of runs into a profile and writes the .gcda
 * files a build reads (-fprofile-use). Tiberian Dawn's and Red Alert's host.c has the same functions; this one is
 * for the other games (QUAKE3_PROFILE, QUAKE_PROFILE, DOOM_PROFILE, HALFLIFE_PROFILE=count in their build scripts):
 *   - the counters are written when the program ends (atexit) and whenever it is sent SIGUSR2, so a game that never
 *     ends by itself is counted with `kill -USR2` after the part played;
 *   - the values met (indirect calls, top values) are kept in a pool of nodes, not by malloc: worker cores reach the
 *     counted code too (their counts are added without locks: a count may be lost; a node is taken atomically).
 */
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct gcov_ctr { unsigned int num; long long *values; };
struct gcov_fn { const void *key; unsigned int ident, lineno_checksum, cfg_checksum; struct gcov_ctr ctrs[1]; };
struct gcov_info {
	unsigned int version; struct gcov_info *next; unsigned int stamp, checksum; const char *filename;
	void (*merge[8])(long long *, unsigned int); unsigned int n_functions; const struct gcov_fn *const *functions;
};
struct gcov_kvp { long long value, count; struct gcov_kvp *next; };
static struct gcov_info *gcov_list;
__thread struct { void *callee; long long *counters; } __gcov_indirect_call;
long long __gcov_time_profiler_counter;

#define POOL 65536
static struct gcov_kvp pool[POOL];
static unsigned int pool_used;

void __gcov_exit(void);

static void
gcov_signal(int sig)
{
	(void)sig;
	__gcov_exit();
}

void
__gcov_init(struct gcov_info *info)
{
	if (!gcov_list) {
		atexit(__gcov_exit);
		signal(SIGUSR2, gcov_signal);
	}
	info->next = gcov_list, gcov_list = info;
}

void __gcov_merge_add(long long *counters, unsigned int count) { (void)counters, (void)count; }
void __gcov_merge_topn(long long *counters, unsigned int count) { (void)counters, (void)count; }
void __gcov_merge_time_profile(long long *counters, unsigned int count) { (void)counters, (void)count; }
void __gcov_merge_ior(long long *counters, unsigned int count) { (void)counters, (void)count; }

/* A value met: counted among the 32 kept for its place, as libgcov keeps them. */
static void
gcov_met(long long *counters, long long value)
{
	struct gcov_kvp *n, *last = NULL, *least = NULL;
	int k = 0;

	counters[0]++;
	for (n = (struct gcov_kvp *)(uintptr_t)counters[2]; n && k < 32; last = n, n = n->next, k++) {
		if (n->value == value) {
			n->count++;
			return;
		}
		if (!least || n->count < least->count)
			least = n;
	}
	if (counters[1] >= 32 || k >= 32) {
		if (least && --least->count < 1)
			least->value = value, least->count = 1;
		return;
	}
	k = __atomic_fetch_add(&pool_used, 1, __ATOMIC_RELAXED);
	if (k >= POOL)
		return;
	n = &pool[k];
	n->value = value, n->count = 1, n->next = NULL;
	if (last)
		last->next = n;
	else
		counters[2] = (uintptr_t)n;
	counters[1]++;
}

void __gcov_topn_values_profiler(long long *counters, long long value) { gcov_met(counters, value); }
void __gcov_ior_profiler(long long *counters, long long value) { *counters |= value; }
void __gcov_average_profiler(long long *counters, long long value) { counters[0] += value, counters[1]++; }
void __gcov_pow2_profiler(long long *counters, long long value) { counters[value && !(value & (value - 1))]++; }

void
__gcov_interval_profiler(long long *counters, long long value, int start, unsigned int steps)
{
	long long delta = value - start;

	counters[delta < 0 ? steps + 1 : delta >= steps ? steps : delta]++;
}

void
__gcov_indirect_call_profiler_v4(long long value, void *function)
{
	if (function == __gcov_indirect_call.callee)
		gcov_met(__gcov_indirect_call.counters, value);
	__gcov_indirect_call.callee = NULL;
}

/* (written with write(): from a signal the program's stdio may be in the middle of something) */
static char out[4096];
static unsigned int out_len;

static void
put(const char *fmt, ...)
{
	va_list ap;
	int n;

	if (out_len > sizeof out - 256) {
		write(2, out, out_len);
		out_len = 0;
	}
	va_start(ap, fmt);
	n = vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
	va_end(ap);
	if (n > 0)
		out_len += n;
}

void
__gcov_exit(void)
{
	struct gcov_info *i;
	unsigned int f, c, kind, kinds;

	for (i = gcov_list; i; i = i->next) {
		for (kinds = 0, kind = 0; kind < 8; kind++)
			kinds |= (i->merge[kind] != NULL) << kind;
		put("GC F %s %x %x %x %x\n", i->filename, i->version, i->stamp, i->checksum, kinds);
		for (f = 0; f < i->n_functions; f++) {
			const struct gcov_fn *fn = i->functions[f];
			const struct gcov_ctr *ctr;

			if (!fn || fn->key != i)
				continue;
			put("GC f %x %x %x\n", fn->ident, fn->lineno_checksum, fn->cfg_checksum);
			for (ctr = fn->ctrs, kind = 0; kind < 8; kind++) {
				if (!i->merge[kind])
					continue;
				put("GC k %u %u\n", kind, ctr->num);
				for (c = 0; c < ctr->num; c++) {
					if (kind == 3 || kind == 4) {
						/* (a place is three words: how often, how many values, the values) */
						const struct gcov_kvp *n = (const struct gcov_kvp *)(uintptr_t)ctr->values[c + 2];
						int k = 0;

						if (ctr->values[c])
							put("GC t %x %llx\n", c / 3, (unsigned long long)ctr->values[c]);
						for (; n && k < 32; n = n->next, k++)
							put("GC v %llx %llx\n", (unsigned long long)n->value, (unsigned long long)n->count);
						c += 2;
					} else if (ctr->values[c])
						put("GC c %x %llx\n", c, (unsigned long long)ctr->values[c]);
				}
				ctr++;
			}
		}
	}
	put("GC end\n");
	write(2, out, out_len);
	out_len = 0;
}

/* (the compiler sends a program's exec and fork calls through these, to write the counts first: a counted program that
   runs another loses nothing that matters here) */
int __gcov_execv(const char *path, char *const argv[]) { return execv(path, argv); }
int __gcov_execvp(const char *file, char *const argv[]) { return execvp(file, argv); }
int __gcov_execve(const char *path, char *const argv[], char *const envp[]) { return execve(path, argv, envp); }
pid_t __gcov_fork(void) { return fork(); }
