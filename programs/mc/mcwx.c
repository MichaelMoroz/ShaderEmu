/*
 * The second layer of the worker library: mcwx.h. Like mcw.c it uses nothing of the C library,
 * so that a program with a runtime of its own can link it.
 */
#include <stdarg.h>
#include "mcwx.h"

#define LOG_BYTES 2048u
#define DEFERS 64u
#define SCRATCH_CHUNK 0x10000u
#define CALLS 256

static long call6(long number, long a, long b, long c, long d, long e, long f)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	__asm__ volatile(MC_ECALL : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
	return a0;
}

/* ---- a jump out of a job: the registers a call keeps ---- */

int mcwx_setjmp(uint32_t *at) __attribute__((returns_twice));
void mcwx_longjmp(uint32_t *at) __attribute__((noreturn));
#ifdef __gpuvm__
/* gpu_vm: ra, sp and s0-s23 are what a call keeps (26 words, as RISC-V's 14 and 12 floats) */
#define FLOAT_SAVES(op) \
	"	" op " s12, 56(a0)\n	" op " s13, 60(a0)\n	" op " s14, 64(a0)\n	" op " s15, 68(a0)\n" \
	"	" op " s16, 72(a0)\n	" op " s17, 76(a0)\n	" op " s18, 80(a0)\n	" op " s19, 84(a0)\n" \
	"	" op " s20, 88(a0)\n	" op " s21, 92(a0)\n	" op " s22, 96(a0)\n	" op " s23, 100(a0)\n"
#elif defined __riscv_flen
#define FLOAT_SAVES(op) \
	"	" op " fs0, 56(a0)\n	" op " fs1, 60(a0)\n	" op " fs2, 64(a0)\n	" op " fs3, 68(a0)\n" \
	"	" op " fs4, 72(a0)\n	" op " fs5, 76(a0)\n	" op " fs6, 80(a0)\n	" op " fs7, 84(a0)\n" \
	"	" op " fs8, 88(a0)\n	" op " fs9, 92(a0)\n	" op " fs10, 96(a0)\n	" op " fs11, 100(a0)\n"
#else
#define FLOAT_SAVES(op) ""
#endif
#define INT_SAVES(op) \
	"	" op " ra, 0(a0)\n	" op " sp, 4(a0)\n	" op " s0, 8(a0)\n	" op " s1, 12(a0)\n" \
	"	" op " s2, 16(a0)\n	" op " s3, 20(a0)\n	" op " s4, 24(a0)\n	" op " s5, 28(a0)\n" \
	"	" op " s6, 32(a0)\n	" op " s7, 36(a0)\n	" op " s8, 40(a0)\n	" op " s9, 44(a0)\n" \
	"	" op " s10, 48(a0)\n	" op " s11, 52(a0)\n"
#ifdef __gpuvm__
__asm__(".text\n"
	".globl mcwx_setjmp\n"
	".p2align 4\n"
	"mcwx_setjmp:\n" INT_SAVES("sw") FLOAT_SAVES("sw")
	"	li a0, 0\n"
	"	ret\n"
	".globl mcwx_longjmp\n"
	".p2align 4\n"
	"mcwx_longjmp:\n" INT_SAVES("lw") FLOAT_SAVES("lw")
	"	li a0, 1\n"
	"	ret\n");
#else
__asm__(".text\n"
	".globl mcwx_setjmp\n"
	"mcwx_setjmp:\n" INT_SAVES("sw") FLOAT_SAVES("fsw")
	"	li a0, 0\n"
	"	ret\n"
	".globl mcwx_longjmp\n"
	"mcwx_longjmp:\n" INT_SAVES("lw") FLOAT_SAVES("flw")
	"	li a0, 1\n"
	"	ret\n");
#endif
#define JUMP_WORDS 26

/* ---- what a core keeps of a job's side effects ---- */

typedef struct chunk { struct chunk *next; uint32_t bytes, pad[2]; } chunk;	/* then its memory */

/* A core's: written by it during a job, read and cleared by the first core afterwards. */
typedef struct {
	uint32_t *jump;		/* where mcw_fail() goes: a job's start, on this core's stack */
	uint32_t failed, value;
	const char *what;
	uint32_t log_used, defers, used, pad;
	chunk *first, *at;	/* scratch memory, and the chunk being used */
	uint32_t pad2[2];
	char log[LOG_BYTES];
	struct { uint32_t fn, a0, a1, pad; } defer[DEFERS];
} side;

static side first_side __attribute__((aligned(16)));
static side *side_of[MC_MAX_CORES];	/* worker k's, made by the first core before k's first job here */

static void write_out(const char *text, uint32_t bytes) { call6(64, 1, (long)text, (long)bytes, 0, 0, 0); }
static void (*sink)(const char *, uint32_t) = write_out;
void mcw_on_print(void (*to)(const char *, uint32_t)) { sink = to ? to : write_out; }

static side *here(void)
{
	int k = mcw_self();

	return k ? side_of[k] : &first_side;
}

static side *side_for(int k)
{
	if (!side_of[k]) {
		side_of[k] = mcw_alloc(sizeof(side));
		if (side_of[k]) mcw_touch(side_of[k], sizeof(side));
	}
	return side_of[k];
}

/* ---- printing ---- */

static uint32_t put_number(char *out, uint32_t room, uint32_t v, unsigned base, int negative, int width, char pad)
{
	char digits[12];
	uint32_t n = 0, i = 0;

	do digits[n++] = "0123456789abcdef"[v % base]; while ((v /= base) != 0);
	if (negative && pad == '0' && i < room) out[i++] = '-', width--;
	else if (negative) digits[n++] = '-';
	for (; (int)n < width && i < room; width--) out[i++] = pad;
	while (n && i < room) out[i++] = digits[--n];
	return i;
}

static uint32_t format(char *out, uint32_t room, const char *f, va_list ap)
{
	uint32_t at = 0;

	for (; *f && at < room; f++) {
		int width = 0;
		char pad = ' ';

		if (*f != '%') {
			out[at++] = *f;
			continue;
		}
		if (*++f == '0') pad = '0', f++;
		while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		while (*f == 'l') f++;
		switch (*f) {
		case 'd': case 'i': {
			int32_t v = va_arg(ap, int32_t);
			at += put_number(out + at, room - at, v < 0 ? 0u - (uint32_t)v : (uint32_t)v, 10, v < 0, width, pad);
			break;
		}
		case 'u': at += put_number(out + at, room - at, va_arg(ap, uint32_t), 10, 0, width, pad); break;
		case 'x': case 'X': case 'p': at += put_number(out + at, room - at, va_arg(ap, uint32_t), 16, 0, width, pad); break;
		case 'c': out[at++] = (char)va_arg(ap, int); break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			uint32_t len = 0;

			if (!s) s = "(null)";
			while (s[len]) len++;
			for (; (int)len < width && at < room; width--) out[at++] = ' ';
			while (*s && at < room) out[at++] = *s++;
			break;
		}
		case 0: return at;
		default: out[at++] = *f;
		}
	}
	return at;
}

void mcw_print(const char *f, ...)
{
	char line[256];
	side *s = here();
	uint32_t n;
	va_list ap;

	va_start(ap, f);
	n = format(line, sizeof line, f, ap);
	va_end(ap);
	if (s == &first_side) {
		sink(line, n);
		return;
	}
	if (!s || s->log_used + n > LOG_BYTES) {
		/* (a job not of this layer, or a full log: written now, by a system call core 0 makes) */
		if (s && s->log_used) write_out(s->log, s->log_used), s->log_used = 0;
		if (!s || n > LOG_BYTES) {
			write_out(line, n);
			return;
		}
	}
	for (uint32_t i = 0; i < n; i++) s->log[s->log_used + i] = line[i];
	s->log_used += n;
}

void mcw_print_now(void)
{
	side *s = here();

	if (s && s != &first_side && s->log_used) write_out(s->log, s->log_used), s->log_used = 0;
}

/* ---- failures, deferred calls, scratch ---- */

static void say_failed(int worker, const char *what, uint32_t value)
{
	char line[160];
	uint32_t n = 0;
	const char *head = "mcw: a job failed on worker ";

	while (*head) line[n++] = *head++;
	n += put_number(line + n, 8, (uint32_t)worker, 10, 0, 0, ' ');
	line[n++] = ':';
	line[n++] = ' ';
	while (what && *what && n < 140) line[n++] = *what++;
	line[n++] = ' ';
	n += put_number(line + n, 8, value, 16, 0, 0, ' ');
	line[n++] = '\n';
	sink(line, n);
	call6(94, 70, 0, 0, 0, 0, 0);	/* exit_group */
}
static mcw_failure_fn failure = say_failed;
void mcw_on_failure(mcw_failure_fn fn) { failure = fn ? fn : say_failed; }

void mcw_fail(const char *what, uint32_t value)
{
	side *s = here();

	if (s && s->jump) {
		s->failed = 1;
		s->what = what;
		s->value = value;
		mcwx_longjmp(s->jump);
	}
	/* (outside a job of this layer: said at once, and the program ends) */
	say_failed(mcw_self(), what, value);
	for (;;) {}
}

void mcw_defer(mcw_fn fn, uint32_t a0, uint32_t a1)
{
	side *s = here();

	if (s == &first_side) {
		fn(a0, a1);
		return;
	}
	if (!s || s->defers >= DEFERS) mcw_fail("mcw: too many deferred calls in one job", DEFERS);
	s->defer[s->defers].fn = (uint32_t)fn;
	s->defer[s->defers].a0 = a0;
	s->defer[s->defers].a1 = a1;
	s->defers++;
}

/* What worker k's job left for the first core, made now (k 0: what the first core's own job did). */
static void settle(int k)
{
	side *s = k ? side_of[k] : &first_side;
	uint32_t i;

	if (!s) return;
	if (s->log_used) {
		sink(s->log, s->log_used);
		s->log_used = 0;
	}
	for (i = 0; i < s->defers; i++) ((mcw_fn)s->defer[i].fn)(s->defer[i].a0, s->defer[i].a1);
	s->defers = 0;
	if (s->failed) {
		s->failed = 0;
		failure(k, s->what, s->value);
	}
}

void *mcw_scratch(uint32_t bytes)
{
	side *s = here();

	if (!s) mcw_fail("mcw: scratch in a job not of mcwx", bytes);
	bytes = (bytes + 15) & ~15u;
	for (;;) {
		chunk *c;
		uint32_t size;
		long r;

		if (s->at && s->used + bytes <= s->at->bytes) {
			void *m = (uint8_t *)(s->at + 1) + s->used;
			s->used += bytes;
			return m;
		}
		if (s->at && s->at->next) {
			s->at = s->at->next;
			s->used = 0;
			continue;
		}
		size = bytes > SCRATCH_CHUNK - 16 ? bytes : SCRATCH_CHUNK - 16;
		r = call6(222, 0, (long)((size + 16 + 4095) & ~4095u), 3, 0x22, -1, 0);	/* mmap2 */
		if (r < 0 && r > -4096) mcw_fail("mcw: no memory for scratch", bytes);
		c = (chunk *)r;
		c->next = 0;
		c->bytes = size;
		if (s->at) s->at->next = c;
		else s->first = c;
		s->at = c;
		s->used = 0;
	}
}

void mcw_scratch_reset(void)
{
	side *s = here();

	if (!s) return;
	s->at = s->first;
	s->used = 0;
}

/* fn(a0, a1) with mcw_fail() coming back here; 0 then. */
static uint32_t guarded(mcw_fn fn, uint32_t a0, uint32_t a1)
{
	side *s = here();
	uint32_t jump[JUMP_WORDS], *was = s->jump;
	volatile uint32_t result = 0;

	s->jump = jump;
	if (!mcwx_setjmp(jump)) result = fn(a0, a1);
	s->jump = was;
	return result;
}

/* ---- the loop's team ---- */

/* The round: the first core's, the round's number written last. */
typedef struct { volatile uint32_t round, fn, ctx, n, grain, shares, linger, pad; } round_t;
typedef struct { volatile uint32_t round, share, pad[2]; } texel_t;
static round_t *team;
static texel_t *plan;		/* the first core's: worker k's share of a round */
static texel_t *done_of;	/* worker k's: the last round it did */
static uint8_t in_team[MC_MAX_CORES];	/* a team job is posted to worker k and has not ended */
static uint8_t busy[MC_MAX_CORES];	/* worker k has a call out */
static int linger = 16;
static int in_round;	/* a round is out: a loop asked meanwhile (from core 0's piece) is one call */

static uint32_t team_job(uint32_t unused, uint32_t k);
static uint32_t run_share(uint32_t share, uint32_t unused);
static uint32_t guarded(mcw_fn fn, uint32_t a0, uint32_t a1);
static uint32_t call_job(uint32_t rec_at, uint32_t unused);

static int setup(void)
{
	if (team) return 1;
	if (mcw_self()) return 0;
	team = mcw_alloc(sizeof(round_t));
	plan = mcw_alloc(sizeof(texel_t) * MC_MAX_CORES);
	done_of = mcw_alloc(sizeof(texel_t) * MC_MAX_CORES);
	if (!team || !plan || !done_of) {
		team = 0;
		return 0;
	}
	mcw_touch_code((const void *)team_job, 256);
	mcw_touch_code((const void *)run_share, 256);
	mcw_touch_code((const void *)guarded, 256);
	mcw_touch_code((const void *)call_job, 64);
	mcw_touch_code((const void *)here, 64);
	mcw_touch_code((const void *)mcwx_setjmp, 256);
	mcw_touch_code((const void *)mcw_self, 64);
	mcw_touch(team, sizeof(round_t));
	mcw_touch(plan, sizeof(texel_t) * MC_MAX_CORES);
	mcw_touch(done_of, sizeof(texel_t) * MC_MAX_CORES);
	return 1;
}

static uint32_t run_share(uint32_t share, uint32_t unused)
{
	mcw_body fn = (mcw_body)team->fn;
	void *ctx = (void *)team->ctx;
	uint64_t n = team->n, grain = team->grain, step = grain * team->shares, from;

	(void)unused;
	for (from = share * grain; from < n; from += step) fn(ctx, (uint32_t)from, (uint32_t)(from + grain < n ? from + grain : n));
	return 0;
}

static uint32_t team_job(uint32_t unused, uint32_t k)
{
	uint32_t last = done_of[k].round, idle = 0;

	(void)unused;
	/* (posted to a worker with no core: its share is the first core's, taken in turn by post_team) */
	if (!mcw_on_worker()) return 0;
	for (;;) {
		uint32_t r = team->round;

		if (r != last) {
			if (!team->fn) break;
			if (plan[k].round == r) guarded(run_share, plan[k].share, 0);
			done_of[k].round = r;
			last = r;
			idle = 0;
		} else if (++idle > team->linger) {
			break;
		}
		mc_end_pass();
	}
	return 0;
}

/* The team's job posted to worker k; false where k had no core. Its share is then run here by
 * the caller, never inside the post: shares may wait for lower ones (m_stages' pipelines). */
static int post_team(int k)
{
	in_team[k] = 1;
	mcw_post(k, team_job, 0, (uint32_t)k);
	if (mcw_bound(k)) return 1;
	in_team[k] = 0;
	return 0;
}

/* Worker k's share of round r, run by the first core: only once every lower share is done or
 * out on a core, so that it waits for nothing that waits for it. */
static void run_missed(int k, uint32_t r)
{
	guarded(run_share, plan[k].share, 0);
	done_of[k].round = r;
}

void mcw_for_linger(int passes) { linger = passes < 0 ? 0 : passes; }

static uint32_t gcd(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}

static int out_on;	/* a round begun by mcw_for_begin() is out */
static int out_from, out_calls;	/* (its wait: round_through) */
static int round_through(const int *list, int members, uint32_t r, int *from, int *calls);
void mcw_for_wait(void);

void mcw_for_opt(uint32_t n, mcw_body fn, void *ctx, const mcw_for_opts *o)
{
	int list[MC_MAX_CORES], members = 0, workers = mcw_count(), want, first, i, k, from, calls;
	uint32_t shares, grain, r;

	if (out_on) mcw_for_wait();
	if (!n) return;
	if (o && o->touch) mcw_touch(o->touch, o->touch_bytes);
	if (workers == 0 || mcw_self() || in_round || !setup()) {
		fn(ctx, 0, n);	/* alone, asked on a worker, or inside a round */
		return;
	}
	want = o && o->workers > 0 && o->workers < workers ? o->workers : workers;
	for (k = 1; k <= workers && members < want; k++) {
		if (busy[k]) continue;
		/* (one in the team is taken as it is: a team job that ended is seen in the wait, round_through) */
		/* (one with no core would run its piece here and now, before the others had theirs) */
		if (!in_team[k] && (!mcw_bound(k) || !side_for(k))) continue;
		list[members++] = k;
	}
	first = !(o && o->first_idle) || members == 0;
	shares = (uint32_t)(members + first);
	grain = o && o->grain ? o->grain : (n + shares * 4 - 1) / (shares * 4);
	if (o && o->out_bytes) {
		uint32_t unit = 16 / gcd(o->out_bytes, 16);
		grain = (grain + unit - 1) / unit * unit;
	}
	if (!grain) grain = 1;
	team->fn = (uint32_t)fn;
	team->ctx = (uint32_t)ctx;
	team->n = n;
	team->grain = grain;
	team->shares = shares;
	team->linger = (uint32_t)linger;
	r = team->round + 1;
	for (i = 0; i < members; i++) {
		plan[list[i]].share = (uint32_t)(i + first);
		plan[list[i]].round = r;
	}
	team->round = r;	/* (last: a worker that sees it sees the rest) */
	for (i = 0; i < members; i++) {
		k = list[i];
		if (!in_team[k]) post_team(k);	/* (one that lost its core is taken below, after share 0) */
	}
	in_round = 1;
	if (first) guarded(run_share, 0, 0);
	for (from = 0, calls = 0; !round_through(list, members, r, &from, &calls);) mcw_rest();
	in_round = 0;
	settle(0);
	for (i = 0; i < members; i++) settle(list[i]);
}

void mcw_for(uint32_t n, mcw_body fn, void *ctx) { mcw_for_opt(n, fn, ctx, 0); }

/* ---- a round begun and not waited for ---- */

static int out_list[MC_MAX_CORES], out_members;
static uint32_t out_round;

void mcw_for_begin(uint32_t n, mcw_body fn, void *ctx, const mcw_for_opts *o)
{
	int workers = mcw_count(), want, first, i, k;
	uint32_t shares, grain, r;

	if (out_on) mcw_for_wait();
	if (!n) return;
	if (o && o->touch) mcw_touch(o->touch, o->touch_bytes);
	if (workers == 0 || mcw_self() || in_round || !setup()) {
		fn(ctx, 0, n);
		return;
	}
	want = o && o->workers > 0 && o->workers < workers ? o->workers : workers;
	out_members = 0;
	for (k = 1; k <= workers && out_members < want; k++) {
		if (busy[k]) continue;
		if (!in_team[k] && (!mcw_bound(k) || !side_for(k))) continue;
		out_list[out_members++] = k;
	}
	first = !(o && o->first_idle) || out_members == 0;
	shares = (uint32_t)(out_members + first);
	grain = o && o->grain ? o->grain : (n + shares * 4 - 1) / (shares * 4);
	if (o && o->out_bytes) {
		uint32_t unit = 16 / gcd(o->out_bytes, 16);
		grain = (grain + unit - 1) / unit * unit;
	}
	if (!grain) grain = 1;
	team->fn = (uint32_t)fn;
	team->ctx = (uint32_t)ctx;
	team->n = n;
	team->grain = grain;
	team->shares = shares;
	team->linger = (uint32_t)linger;
	r = team->round + 1;
	for (i = 0; i < out_members; i++) {
		plan[out_list[i]].share = (uint32_t)(i + first);
		plan[out_list[i]].round = r;
	}
	team->round = r;
	for (i = 0; i < out_members; i++) {
		k = out_list[i];
		if (!in_team[k]) post_team(k);
	}
	out_round = r;
	out_from = out_calls = 0;
	out_on = 1;
	if (first) {
		in_round = 1;
		guarded(run_share, 0, 0);
		in_round = 0;
	}
}

/*
 * Whether every member of round r is through: the members before *from are, and a look goes
 * on from there. Asking a worker (mcw_done) is some 50 instructions, 255 of them a pass's
 * worth of this core: one is asked a call, the first not through, and every one only at
 * every eighth call (a worker that stopped at a page or whose team job ended is seen to then).
 */
static int round_through(const int *list, int members, uint32_t r, int *from, int *calls)
{
	int i, k, out = 0, all = (++*calls & 7) == 0;

	while (*from < members && done_of[list[*from]].round == r) (*from)++;
	for (i = *from; i < members; i++) {
		k = list[i];
		if (done_of[k].round == r) continue;
		out = 1;
		if (!all && i != *from) break;
		if (in_team[k] && !mcw_done(k)) continue;
		/* (its team job ended before it saw the round, or never went out: posted again) */
		in_team[k] = 0;
		if (done_of[k].round == r || post_team(k)) continue;
		in_round = 1;
		run_missed(k, r);
		in_round = 0;
	}
	return !out;
}

int mcw_for_ready(void)
{
	if (!out_on) return 1;
	return round_through(out_list, out_members, out_round, &out_from, &out_calls);
}

void mcw_for_wait(void)
{
	int i;

	if (!out_on) return;
	while (!mcw_for_ready()) mcw_rest();
	out_on = 0;
	settle(0);
	for (i = 0; i < out_members; i++) settle(out_list[i]);
}

void mcw_for_end(void)
{
	int k, any = 0;

	if (out_on) mcw_for_wait();
	if (!team) return;
	for (k = 1; k <= mcw_count(); k++)
		if (in_team[k]) any = 1;
	if (!any) return;
	team->fn = 0;
	team->round = team->round + 1;
	for (;;) {
		for (any = 0, k = 1; k <= mcw_count(); k++)
			if (in_team[k] && !mcw_done(k)) any = 1;
			else in_team[k] = 0;
		if (!any) break;
		mcw_rest();
	}
}

/* ---- a picture made as a fragment shader makes it ---- */

/* What the lanes read: the picture and its function (the first core's, in memory of its own). */
typedef struct { uint32_t *pixels; int width, height; mcw_pixel_fn fn; void *ctx; uint32_t pad[3]; } shade_t;
static shade_t *shading;

/* Texel t of the picture, four pixels side by side: the lanes' index. */
static void shade_texel(uint32_t t, void *ctx)
{
	const shade_t *s = ctx;
	uint32_t p = 4 * t, x = p % (uint32_t)s->width, y = p / (uint32_t)s->width, *out = s->pixels + p;
	uint32_t a = s->fn((int)x, (int)y, s->ctx), b = s->fn((int)x + 1, (int)y, s->ctx);
	uint32_t c = s->fn((int)x + 2, (int)y, s->ctx), d = s->fn((int)x + 3, (int)y, s->ctx);

	out[0] = a, out[1] = b, out[2] = c, out[3] = d;
}

/* A core's piece of the picture: on its lanes where it has them. */
static void shade_piece(void *ctx, uint32_t from, uint32_t to)
{
	shade_t *s = ctx;

	mcw_lanes(shade_texel, s, from, to - from, s->pixels + 4 * from, 1);
}

/* The round: a piece a worker (the first core takes none: its pixels would be one at a time),
 * all from one lanes job, so a worker pays for one job's start and end a picture. */
static int shade_setup(uint32_t *pixels, int width, int height, mcw_pixel_fn fn, void *ctx, mcw_for_opts *o)
{
	uint32_t texels = (uint32_t)(width * height) / 4, workers = (uint32_t)(mcw_count() > 0 ? mcw_count() : 1);

	if (!shading) {
		static shade_t one __attribute__((aligned(16)));

		shading = &one;
		mcw_touch(shading, sizeof(shade_t));
		mcw_touch_code((const void *)shade_texel, 128);
		mcw_touch_code((const void *)shade_piece, 64);
	}
	mcw_touch_code((const void *)fn, 4);
	shading->pixels = pixels, shading->width = width, shading->height = height, shading->fn = fn, shading->ctx = ctx;
	o->grain = (texels + workers - 1) / workers;
	o->out_bytes = 16;
	o->touch = 0, o->touch_bytes = 0;
	o->workers = 0;
	o->first_idle = mcw_count() > 0;
	return texels != 0;
}

static int shade_field;	/* the picture out is the field's job */

void mcw_shade_begin(uint32_t *pixels, int width, int height, mcw_pixel_fn fn, void *ctx)
{
	mcw_for_opts o;

	mcw_shade_wait();
	if (!shade_setup(pixels, width, height, fn, ctx, &o)) return;
	/* the lanes' field where the machine has one (the whole picture one job, none of it given out
	 * to the workers); else a piece a worker, each on its own lanes */
	if (!mcw_self() && mcw_field_lanes() > 0) {
		shade_field = 1;
		mcw_field_begin(shade_texel, shading, 0, (uint32_t)(width * height) / 4, pixels, 1);
		return;
	}
	mcw_for_begin((uint32_t)(width * height) / 4, shade_piece, shading, &o);
}

void mcw_shade_wait(void)
{
	if (shade_field) {
		mcw_field_wait();
		shade_field = 0;
	}
	mcw_for_wait();
}

void mcw_shade(uint32_t *pixels, int width, int height, mcw_pixel_fn fn, void *ctx)
{
	mcw_shade_begin(pixels, width, height, fn, ctx);
	mcw_shade_wait();
}

/* ---- calls ---- */

typedef struct { uint32_t fn, a0, a1, k; } call_rec;
static call_rec calls[CALLS] __attribute__((aligned(16)));
static uint8_t call_state[CALLS];	/* 0 free, 1 out on a worker, 2 made here */
static uint32_t call_answer[CALLS];
static int next_worker = 1;

static uint32_t call_job(uint32_t rec_at, uint32_t unused)
{
	const call_rec *c = (const call_rec *)rec_at;

	(void)unused;
	return guarded((mcw_fn)c->fn, c->a0, c->a1);
}

mcw_handle mcw_call(mcw_fn fn, uint32_t a0, uint32_t a1)
{
	int h, k = 0, tries, workers = mcw_count();
	call_rec *c;

	for (h = 0; h < CALLS && call_state[h]; h++) {}
	if (h == CALLS) mcw_fail("mcw: more calls out than there are handles", CALLS);
	c = &calls[h];
	c->fn = (uint32_t)fn;
	c->a0 = a0;
	c->a1 = a1;
	if (workers && !mcw_self() && setup())
		for (tries = 0; tries < workers && !k; tries++) {
			int w = next_worker;

			next_worker = next_worker % workers + 1;
			if (busy[w] || !mcw_bound(w)) continue;
			if (in_team[w]) {
				if (!mcw_done(w)) continue;
				in_team[w] = 0;
			}
			if (mcw_done(w) && side_for(w)) k = w;
		}
	c->k = (uint32_t)k;
	if (k) {
		busy[k] = 1;
		call_state[h] = 1;
		mcw_post(k, call_job, (uint32_t)c, 0);
	} else {
		call_answer[h] = call_job((uint32_t)c, 0);
		call_state[h] = 2;
	}
	return h + 1;
}

/* mcw_call() on a worker with nothing to do, or nothing at all (0): for work only a worker pays for. */
mcw_handle mcw_try_call(mcw_fn fn, uint32_t a0, uint32_t a1)
{
	int h, k = 0, tries, workers = mcw_count();

	if (!workers || mcw_self() || !setup()) return 0;
	for (h = 0; h < CALLS && call_state[h]; h++) {}
	if (h == CALLS) return 0;
	for (tries = 0; tries < workers && !k; tries++) {
		int w = next_worker;

		next_worker = next_worker % workers + 1;
		if (busy[w] || !mcw_bound(w)) continue;
		if (in_team[w]) {
			if (!mcw_done(w)) continue;
			in_team[w] = 0;
		}
		if (mcw_done(w) && side_for(w)) k = w;
	}
	if (!k) return 0;
	calls[h].fn = (uint32_t)fn;
	calls[h].a0 = a0;
	calls[h].a1 = a1;
	calls[h].k = (uint32_t)k;
	busy[k] = 1;
	call_state[h] = 1;
	mcw_post(k, call_job, (uint32_t)&calls[h], 0);
	return h + 1;
}

int mcw_ready(mcw_handle h)
{
	if (h < 1 || h > CALLS || !call_state[h - 1]) return 1;
	return call_state[h - 1] == 2 || mcw_done((int)calls[h - 1].k);
}

void mcw_rest_on(const mcw_handle *h, int n)
{
	int workers[16], m = 0, i;

	for (i = 0; i < n && m < 16; i++)
		if (h[i] >= 1 && h[i] <= CALLS && call_state[h[i] - 1] == 1 && calls[h[i] - 1].k)
			workers[m++] = (int)calls[h[i] - 1].k;
	mcw_rest_for(workers, m);
}

uint32_t mcw_join(mcw_handle h)
{
	uint32_t answer;
	int k;

	if (h < 1 || h > CALLS || !call_state[h - 1]) return 0;
	h--;
	if (call_state[h] == 2) {
		answer = call_answer[h];
		k = 0;
	} else {
		k = (int)calls[h].k;
		mcw_wait(k);
		answer = mcw_result(k);
		busy[k] = 0;
	}
	call_state[h] = 0;
	settle(0);	/* (a job its worker's core was taken from ran here) */
	if (k) settle(k);
	return answer;
}

/* ---- counters ---- */

static volatile uint32_t *counts;	/* MC_COUNT_PHYS, mapped */
static volatile uint32_t *head;		/* MC_COUNT_HEAD_PHYS */
static uint32_t words[MC_COUNTERS];	/* the counters of a program without workers */
static int count_base = -1, counting;
/* What worker k (0: the first core) added since the merge it last saw, a counter at a time. */
typedef struct { uint32_t merges, added; } since_t;
static since_t since[MC_MAX_CORES][MC_COUNTERS] __attribute__((aligned(16)));

#define REGION(k) (MC_COUNT_REGION_AT(k) / 4)

int mcw_counters_merged(void) { return count_base >= 0 && head[1] == MC_COUNT_MERGED; }

int mcw_counters(void)
{
	int slot = mcw_slot(), i;

	if (counting || mcw_self()) return MC_COUNTERS;
	if (slot < 0) return MC_COUNTERS;	/* (no workers: words; asked again after mcw_open()) */
	counting = 1;
	if (!(counts = mcw_map(MC_COUNT_GPU_OFFSET, MC_COUNT_BYTES)) ||
	    !(head = mcw_map((MC_COUNT_HEAD_PHYS & ~4095u) - 0x86000000u, 4096)))
		mcw_fail("mcw: the counters' memory could not be mapped", 0);
	head += (MC_COUNT_HEAD_PHYS & 4095u) / 4;
	mcw_touch((void *)counts, MC_COUNT_BYTES);
	mcw_touch((void *)head, 16);
	mcw_touch(since, sizeof since);
	count_base = slot * MC_COUNTERS;
	mcw_counters_settle();	/* (what a program before this one in the slot added, merged first) */
	for (i = 0; i < MC_COUNTERS; i++) mcw_set(i, 0);
	return MC_COUNTERS;
}

void mcw_add(int i, uint32_t value)
{
	int k;
	uint32_t merges;
	since_t *own;

	if (count_base < 0) {
		if (mcw_self()) mcw_fail("mcw: a counter added to on a worker before mcw_counters()", (uint32_t)i);
		words[i] += value;
		return;
	}
	k = mcw_self();
#ifdef __gpuvm__
	__asm__ volatile("amoadd zero, %1, (%0)" : : "r"(counts + REGION(k) + count_base + i), "r"(value) : "memory");
#else
	__asm__ volatile("amoadd.w zero, %1, (%0)" : : "r"(counts + REGION(k) + count_base + i), "r"(value) : "memory");
#endif
	head[0] = 1;
	merges = head[2];
	own = &since[k][i];
	if (own->merges != merges) own->merges = merges, own->added = 0;
	own->added += value;
}

uint32_t mcw_get(int i)
{
	uint32_t g, total;
	const since_t *own;
	int k;

	if (count_base < 0) return words[i];
	g = (uint32_t)(count_base + i);
	if (mcw_counters_merged()) {
		own = &since[mcw_self()][i];
		return counts[g] + (own->merges == head[2] ? own->added : 0);
	}
	/* (a machine that does not merge: every region is added here) */
	for (total = 0, k = 0; k < MC_MAX_CORES; k++) total += counts[REGION(k) + g];
	return total;
}

void mcw_counters_settle(void)
{
	/* (a merge after this takes every add made before; with none to merge there is none) */
	uint32_t merges;

	if (!mcw_counters_merged() || !head[0]) return;
	merges = head[2];
	while (head[2] == merges) mcw_rest();
}

void mcw_set(int i, uint32_t value)
{
	if (count_base < 0) words[i] = value;
	else mcw_add(i, value - mcw_get(i));
}
