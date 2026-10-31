/* The game tick on a worker core of its own (docs/games/openttd.md, "The tick on a worker"): it runs
 * there on a stack and a heap of its own while the first core draws, waits where the first core asks,
 * and what it leaves the window system is made by the first core while it waits. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../gfx_func.h"
#include "../vehicle_func.h"
#include "../window_func.h"
#include "../news_func.h"
#include "../company_func.h"
#include "../settings_type.h"
#include "../timer/timer_game_tick.h"
#include "../transparency.h"
#include "shaderemu_v.h"
#include <cerrno>
extern "C" {
#include <mcwx.h>
}
/* (no safeguards.h: this file is the heap) */

void CallWindowGameTickEvent(); /* window.cpp, gfx.cpp */
void DoPaletteAnimations();
void YapfShipAheadService(); /* yapf_ship.cpp */
void YapfShipAheadStop();
void YapfShipAheadDrop();
void YapfShipAheadGo();
void YapfShipAheadCancel();
void YapfShipAheadInit();

extern "C" {
void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_calloc(size_t m, size_t n);
void *__real_realloc(void *p, size_t n);
void *__real_aligned_alloc(size_t align, size_t n);
size_t __real_malloc_usable_size(void *p);
}

struct TwRegs { uint32_t r[26]; }; ///< as shaderemu_v.cpp's TickRegs
extern "C" void ottd_tick_swap(TwRegs *save, const TwRegs *load);

uintptr_t _tw_stack;
int _tw_core;
void *_tw_dying;
bool _tw_running;
bool _tw_on;
TickWord _tw_req, _tw_ack;

/* the first core's words, and the tick's (each in 16 bytes of its own) */
static TickWord _tw_release, _tw_stop, _tw_excl_done, _tw_ring_tail, _th_answer_seq;
static TickWord _tw_where, _tw_excl, _tw_ticks, _tw_ended, _tw_ring_head, _th_ask_seq;
static TickWord _tw_site; ///< the tick's: where it last stopped in a tick (0 between two)
struct LongHold { uint32_t site, count, most; };
static LongHold _tw_long[16]; ///< holds over 40 ms, by where the tick stopped (printed at the end)
struct alignas(16) TwAsk { size_t bytes, align; };
static TwAsk _th_ask;          ///< the tick's: memory it asks the first core for
struct alignas(16) TwAnswer { void *p; };
static TwAnswer _th_answer;     ///< the first core's

/* ---- the tick's heap ---- */

/* Blocks of 16 bytes and more after a header of 16: up to 1 KB in steps of 16, then powers of two.
 * The tick alone takes and gives back here, and the first core while the tick waits; a block the
 * first core frees meanwhile is kept until then. Memory from musl that the tick frees goes back on a ring. */
static constexpr uint32_t TH_TAG = 0x7e4b1d00, TH_ALIGNED = 0x7e4b1eaa, TH_SMALL = 64, TH_CLASSES = TH_SMALL + 24;
struct alignas(16) ThHead { uint32_t tag, off, pad[2]; };
static uint8_t *_th_base, *_th_top, *_th_end;
static size_t _th_size;
static void *_th_free[TH_CLASSES];
static uint32_t _th_remote, _th_ring_frees; ///< the tick's: blocks it asked the first core for, and gave back on the ring
static std::vector<void *> *_th_pending; ///< the first core's frees of the tick's blocks, made at the next wait
static constexpr uint32_t RING = 4096;
alignas(16) static void *_th_ring[RING];

static inline size_t Pad(size_t n) { return ((n + 15) & ~size_t{15}) + 16; } ///< (musl's next header never in a texel of ours)
static inline bool Ours(const void *p) { return static_cast<size_t>(static_cast<const uint8_t *>(p) - _th_base) < _th_size; }
static inline uint32_t ThClass(size_t n)
{
	if (n <= 16 * TH_SMALL) return n == 0 ? 0 : static_cast<uint32_t>((n + 15) >> 4) - 1;
	uint32_t c = TH_SMALL;
	for (size_t cap = 2048; cap < n; cap <<= 1) c++;
	return c;
}
static inline size_t ThCap(uint32_t c) { return c < TH_SMALL ? size_t{c + 1} << 4 : size_t{2048} << (c - TH_SMALL); }

static void *ThRemote(size_t n, size_t align)
{
	_th_ask.bytes = n, _th_ask.align = align;
	const uint32_t s = _th_ask_seq.v + 1;
	_th_ask_seq.v = s;
	while (_th_answer_seq.v != s) mc_end_pass();
	_th_remote++;
	return _th_answer.p;
}

static void *ThAlloc(size_t n)
{
	const uint32_t c = ThClass(n);
	if (c >= TH_CLASSES) return ThRemote(n, 16);
	if (void *p = _th_free[c]; p != nullptr) {
		_th_free[c] = *static_cast<void **>(p);
		return p;
	}
	const size_t cap = ThCap(c);
	if (static_cast<size_t>(_th_end - _th_top) < cap + 16) return ThRemote(n, 16);
	ThHead *h = reinterpret_cast<ThHead *>(_th_top);
	h->tag = TH_TAG | c;
	_th_top += cap + 16;
	return h + 1;
}

static void ThFree(void *p)
{
	ThHead *h = static_cast<ThHead *>(p) - 1;
	if (h->tag == TH_ALIGNED) {
		p = static_cast<uint8_t *>(p) - h->off;
		h = static_cast<ThHead *>(p) - 1;
	}
	const uint32_t c = h->tag & 0xff;
	*static_cast<void **>(p) = _th_free[c];
	_th_free[c] = p;
}

static size_t ThUsable(void *p)
{
	const ThHead *h = static_cast<ThHead *>(p) - 1;
	if (h->tag == TH_ALIGNED) return ThCap((reinterpret_cast<const ThHead *>(static_cast<uint8_t *>(p) - h->off) - 1)->tag & 0xff) - h->off;
	return ThCap(h->tag & 0xff);
}

static void *ThAligned(size_t align, size_t n)
{
	if (align <= 16) return ThAlloc(n);
	uint8_t *raw = static_cast<uint8_t *>(ThAlloc(n + align));
	if (raw == nullptr || !Ours(raw)) return ThRemote(n, align);
	uint8_t *u = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(raw) + 16 + align - 1) & ~(uintptr_t{align} - 1));
	ThHead *h = reinterpret_cast<ThHead *>(u) - 1;
	h->tag = TH_ALIGNED;
	h->off = static_cast<uint32_t>(u - raw);
	return u;
}

/** On the tick's worker: musl's block, to be freed by the first core. */
static void ThRingPush(void *p)
{
	const uint32_t at = _tw_ring_head.v;
	while (at - _tw_ring_tail.v >= RING) mc_end_pass();
	_th_ring[at % RING] = p;
	_tw_ring_head.v = at + 1;
	_th_ring_frees++;
}

static bool _tw_held_now, _tw_held_between;

/** A block of the tick's freed by the first core. */
static void ThFreeHere(void *p)
{
	if (!_tw_on || _tw_held_now) return ThFree(p);
	if (_th_pending == nullptr) _th_pending = new std::vector<void *>();
	_th_pending->push_back(p);
}

/* ---- the first core's small blocks ---- */

/* Up to 1 KB, by size in steps of 16 after a header of 16, from 8 MB of its own, never given back but to their size's
 * list: musl's malloc and free were 8% of the first core's instructions, 157 and 110 a call (docs/games/openttd.md,
 * "The first core's small blocks"). Only the first core takes and gives back here; what the tick frees of it comes by the
 * ring. A block keeps its texels to itself, as Pad does. When the 8 MB are out, musl's. OPENTTD_MALLOC=musl: none. */
static constexpr size_t FH_BYTES = 8 * 1024 * 1024; /* (busy.sav takes 2.9 MB) */
static uint8_t *_fh_base, *_fh_top, *_fh_end;
static size_t _fh_size;
static bool _fh_tried;
static void *_fh_free[TH_SMALL];
static inline bool FhOurs(const void *p) { return static_cast<size_t>(static_cast<const uint8_t *>(p) - _fh_base) < _fh_size; }

static __attribute__((noinline)) void *FhNew(uint32_t c, size_t n)
{
	if (!_fh_tried) {
		_fh_tried = true;
		if (getenv("OPENTTD_MALLOC") == nullptr) {
			if (void *m = __real_aligned_alloc(16, FH_BYTES)) {
				_fh_base = _fh_top = static_cast<uint8_t *>(m);
				_fh_size = FH_BYTES;
				_fh_end = _fh_base + FH_BYTES;
			}
		}
	}
	const size_t cap = size_t{c + 1} << 4;
	if (static_cast<size_t>(_fh_end - _fh_top) < cap + 16) return __real_malloc(Pad(n));
	uint32_t *h = reinterpret_cast<uint32_t *>(_fh_top);
	h[0] = c;
	_fh_top += cap + 16;
	return h + 4;
}

extern "C" {
void *__wrap_malloc(size_t n)
{
	if (TickOnWorker()) return ThAlloc(n);
	if (n - 1 < 16 * TH_SMALL) {
		const uint32_t c = static_cast<uint32_t>(n - 1) >> 4;
		if (void *p = _fh_free[c]; p != nullptr) {
			_fh_free[c] = *static_cast<void **>(p);
			return p;
		}
		return FhNew(c, n);
	}
	return __real_malloc(Pad(n));
}

void __wrap_free(void *p)
{
	if (p == nullptr) return;
	if (TickOnWorker()) {
		if (p == _tw_dying) { _tw_dying = nullptr; return; } /* (a vehicle: the first core frees it at the next wait) */
		if (Ours(p)) ThFree(p);
		else ThRingPush(p);
		return;
	}
	if (FhOurs(p)) {
		const uint32_t c = (static_cast<uint32_t *>(p) - 4)[0];
		*static_cast<void **>(p) = _fh_free[c];
		_fh_free[c] = p;
		return;
	}
	if (Ours(p)) return ThFreeHere(p);
	__real_free(p);
}

void *__wrap_calloc(size_t m, size_t n)
{
	if (n != 0 && m > SIZE_MAX / n) {
		errno = ENOMEM;
		return nullptr;
	}
	/* What the kernel gives is cleared: the map's megabytes are not cleared twice */
	if (m * n >= 1u << 20 && !TickOnWorker()) return __real_calloc(Pad(m * n), 1);
	void *p = __wrap_malloc(m * n);
	if (p != nullptr) memset(p, 0, m * n);
	return p;
}

/* The tables of the unwinder: at a program's start libgcc reads every FDE of the program to file its range
 * (5 million instructions for OpenTTD, which throws nothing to play). They are filed when the first throw looks. */
void __real___register_frame_info(const void *begin, void *ob);
void *__real___deregister_frame_info(const void *begin);
const void *__real__Unwind_Find_FDE(void *pc, void *bases);
static const void *_eh_begin;
static void *_eh_ob;

void __wrap___register_frame_info(const void *begin, void *ob)
{
	_eh_begin = begin;
	_eh_ob = ob;
}

void *__wrap___deregister_frame_info(const void *begin)
{
	if (_eh_begin != nullptr) return nullptr; /* (never filed) */
	return __real___deregister_frame_info(begin);
}

const void *__wrap__Unwind_Find_FDE(void *pc, void *bases)
{
	if (_eh_begin != nullptr) {
		__real___register_frame_info(_eh_begin, _eh_ob);
		_eh_begin = nullptr;
	}
	return __real__Unwind_Find_FDE(pc, bases);
}

void *__wrap_realloc(void *p, size_t n)
{
	if (p == nullptr) return __wrap_malloc(n);
	if (FhOurs(p)) {
		const size_t cap = size_t{(static_cast<uint32_t *>(p) - 4)[0] + 1} << 4;
		if (n <= cap) return p;
		void *q = __wrap_malloc(n);
		if (q == nullptr) return nullptr;
		memcpy(q, p, cap);
		__wrap_free(p); /* (on the tick's worker, by the ring) */
		return q;
	}
	if (Ours(p)) {
		const size_t cap = ThUsable(p);
		if (n <= cap) return p;
		void *q = __wrap_malloc(n);
		if (q == nullptr) return nullptr;
		memcpy(q, p, cap);
		__wrap_free(p);
		return q;
	}
	if (!TickOnWorker()) return __real_realloc(p, Pad(n));
	const size_t cap = __real_malloc_usable_size(p) - 16;
	void *q = ThAlloc(n);
	if (q == nullptr) return nullptr;
	memcpy(q, p, std::min(cap, n));
	ThRingPush(p);
	return q;
}

void *__wrap_aligned_alloc(size_t align, size_t n)
{
	if (TickOnWorker()) return ThAligned(align, n);
	return __real_aligned_alloc(align, Pad(n));
}

void *__wrap_memalign(size_t align, size_t n) { return __wrap_aligned_alloc(align, n); }

int __wrap_posix_memalign(void **res, size_t align, size_t n)
{
	if (align < sizeof(void *) || (align & (align - 1)) != 0) return EINVAL;
	void *p = __wrap_aligned_alloc(align, n);
	if (p == nullptr) return ENOMEM;
	*res = p;
	return 0;
}

size_t __wrap_malloc_usable_size(void *p)
{
	if (p == nullptr) return 0;
	if (FhOurs(p)) return size_t{(static_cast<uint32_t *>(p) - 4)[0] + 1} << 4;
	if (Ours(p)) return ThUsable(p);
	return __real_malloc_usable_size(p) - 16;
}
}

/* ---- the tick on its worker ---- */

static TwRegs _tw_job_regs, _tw_regs;
static uint32_t _tw_seq;              ///< the first core's: its last request
static CompanyID _tw_gui_company;     ///< the first core's _current_company, as it let the tick go
static mcw_handle _tw_handle;
static std::vector<TickTask *> _tw_tasks; ///< the tick's, made by the first core at a wait
static bool _tw_excl_have;            ///< the tick's: no worker draws until it next waits
static uint32_t _tw_next;             ///< the tick's: when its next tick is due (the machine's 5 kHz time)
static uint64_t _tw_seen_ticks;       ///< the first core's: the game ticks it made the windows' parts of
/* for the play lines: the first core's waits and holds, the tick's time in ticks and waiting */
uint32_t _tw_hold_time, _tw_holds, _tw_hold_most;
static uint32_t _tw_tick_time, _tw_park_time, _tw_tick_instret, _tw_idle_time, _tw_excl_time, _tw_loops;

void TickDeferTask(TickTask *task)
{
	if (!TickOnWorker()) {
		task->Run();
		delete task;
		return;
	}
	_tw_tasks.push_back(task);
}

struct TrapRec { const void *at; uint32_t count; };
static TrapRec _tw_traps[64];
void TickTrap(const void *caller)
{
	for (TrapRec &t : _tw_traps) {
		if (t.at == caller || t.at == nullptr) {
			t.at = caller;
			t.count++;
			return;
		}
	}
}

/** On the tick's worker: wait here until the first core lets go (request r); mid: in a tick. */
static void TwPark(uint32_t r, bool mid)
{
	const CompanyID tick_company = _current_company;
	_current_company = _tw_gui_company;
	_tw_excl_have = false;
	const uint32_t t = VpTime();
	_tw_ack.v = r;
	while (_tw_release.v != r) mc_end_pass();
	_tw_park_time += VpTime() - t;
	if (mid) _current_company = tick_company;
}

void TickWorkerYield()
{
	if (!TickOnWorker()) return;
	const uint32_t r = _tw_req.v;
	if ((r & 1) != 0 || r == _tw_ack.v) return; /* (a wait between two ticks is made at the end of this one) */
	_tw_site.v = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
	TwPark(r, true);
}

void TickExclusive()
{
	if (!TickOnWorker() || _tw_excl_have) return;
	const uint32_t s = _tw_excl.v + 1, t = VpTime();
	_tw_excl.v = s;
	while (_tw_excl_done.v != s) {
		const uint32_t r = _tw_req.v;
		if (r != _tw_ack.v && (r & 1) == 0) TwPark(r, true);
		else mc_end_pass();
	}
	_tw_excl_have = true;
	_tw_excl_time += VpTime() - t;
}

static TickWord _tw_pace; ///< the first core's: the least time between two ticks while it scrolls late (TwPace)

static uint32_t TwInterval()
{
	/* OPENTTD_TICK_MS=N: no more than a tick every N ms */
	static const uint32_t least = [] { const char *e = getenv("OPENTTD_TICK_MS"); return e != nullptr ? static_cast<uint32_t>(atoi(e)) * 5 : 0; }();
	if (_pause_mode.Any() || _game_speed == 0) return MILLISECONDS_PER_TICK * 5;
	return std::max<uint32_t>({least, _tw_pace.v, MILLISECONDS_PER_TICK * 5 * 100 / _game_speed});
}

/**
 * The first core, after a picture of work (5 kHz time) over a target: while a viewport scrolls and pictures
 * are late the ticks are spaced out, to a tick a OPENTTD_PACE ms at most (1000; 0: never).
 */
void TwPace(uint32_t work, uint32_t target, bool scrolling)
{
	static const uint32_t most = [] { const char *e = getenv("OPENTTD_PACE"); return (e != nullptr ? static_cast<uint32_t>(atoi(e)) : 1000) * 5; }();
	static uint32_t avg = 0, quiet = 0;
	if (!_tw_on || most == 0) return;
	avg = (avg * 3 + work) / 4;
	quiet = scrolling ? 0 : quiet + 1;
	uint32_t pace = _tw_pace.v;
	if (quiet > 8) {
		pace = 0;
	} else if (avg > target) {
		/* (from half the most, a quarter more a picture: a tick a third cheaper than before filled the slower ramp of
		 * before, from 200 ms an eighth more a picture, with twice the ticks while zoomed out, and every tick's moves
		 * are parts drawn again: 1.84 pictures a second while played fell to 1.47) */
		pace = std::min(most, pace == 0 ? most / 2 : pace + pace / 4 + 25);
	} else if (avg < target * 7 / 8) {
		pace = pace * 7 / 8;
		if (pace < MILLISECONDS_PER_TICK * 5) pace = 0;
	}
	_tw_pace.v = pace;
	static const bool log = getenv("OPENTTD_PACE_LOG") != nullptr;
	static uint32_t n = 0;
	if (log && ++n % 20 == 0) fmt::print(stderr, "openttd: pace: work {:.1f} ms (target {:.1f}), a tick every {:.0f} ms at least\n", avg / 5.0, target / 5.0, pace / 5.0);
}

static void TwMain()
{
	_tw_next = VpTime();
	for (;;) {
		if (_tw_stop.v != 0) break;
		const uint32_t r = _tw_req.v;
		if (r != _tw_ack.v) {
			_tw_where.v = 0;
			TwPark(r, false);
			continue;
		}
		const uint32_t now = VpTime();
		if (static_cast<int32_t>(now - _tw_next) < 0) {
			YapfShipAheadGo(); /* (the map is still until the tick's next) */
			mc_end_pass();
			_tw_idle_time += VpTime() - now;
			continue;
		}
		_tw_loops++;
		const uint32_t interval = TwInterval();
		_tw_next += interval;
		if (static_cast<int32_t>(now - _tw_next) > static_cast<int32_t>(interval * 5)) _tw_next = now; /* (upstream's ALLOWED_DRIFT) */
		_tw_where.v = 1;
		uint32_t i0;
		asm volatile("rdinstret %0" : "=r"(i0));
		const uint32_t park0 = _tw_park_time;
		if (_pause_mode.Any()) YapfShipAheadDrop();
		YapfShipAheadStop();
		try {
			StateGameLoop();
		} catch (...) {
			fmt::print(stderr, "openttd: an exception out of a game tick on its worker\n");
			std::abort();
		}
		TickAfter();
		uint32_t i1;
		asm volatile("rdinstret %0" : "=r"(i1));
		_tw_tick_instret += i1 - i0;
		_tw_tick_time += VpTime() - now - (_tw_park_time - park0);
		_tw_where.v = 0;
		_tw_ticks.v = _tw_ticks.v + 1;
	}
	_tw_ended.v = 1;
	ottd_tick_swap(&_tw_regs, &_tw_job_regs);
}

static uint32_t TwJob(uint32_t, uint32_t)
{
	_tw_core = mcw_self();
	ottd_tick_swap(&_tw_job_regs, &_tw_regs);
	return 0;
}

/* ---- the first core's side ---- */

static void ThDrainRing()
{
	uint32_t tail = _tw_ring_tail.v;
	const uint32_t head = _tw_ring_head.v;
	if (tail == head) return;
	for (; tail != head; tail++) __wrap_free(_th_ring[tail % RING]);
	_tw_ring_tail.v = tail;
}

void SeLinkGraphService(); /* linkgraphjob.cpp */
bool SeLinkGraphInit();
void SeLinkGraphStats();

/** What the tick's worker waits for: its stops, its frees, memory it asked for; the link graph's workers. */
void TwService()
{
	SeLinkGraphService();
	YapfShipAheadService();
	if (!_tw_on) return;
	if (mcw_ready(_tw_handle) && _tw_ended.v == 0) {
		fmt::print(stderr, "openttd: the game tick's worker ended\n");
		std::abort();
	}
	if (!VpFlushOut()) ThDrainRing(); /* (what it freed may be what a piece out reads) */
	if (_th_ask_seq.v != _th_answer_seq.v) {
		const size_t n = Pad(_th_ask.bytes);
		void *p = _th_ask.align > 16 ? __real_aligned_alloc(_th_ask.align, n) : __real_malloc(n);
		if (p == nullptr) {
			fmt::print(stderr, "openttd: no memory for the game tick ({} bytes)\n", _th_ask.bytes);
			std::abort();
		}
		mcw_touch(p, static_cast<unsigned>(n));
		_th_answer.p = p;
		_th_answer_seq.v = _th_ask_seq.v;
	}
}

/** The first core will not draw until the tick next waits: a delete the tick asked to make alone is made. */
void TwGrant()
{
	if (_tw_on && _tw_excl.v != _tw_excl_done.v && !VpFlushOut()) _tw_excl_done.v = _tw_excl.v;
}

bool TwExclusiveWanted() { return _tw_on && _tw_excl.v != _tw_excl_done.v; }

/** The frees the tick left, made once no flush is out (the tick waiting). */
void TwAfterFlush()
{
	if (VpFlushOut()) return;
	if (_th_pending != nullptr && (_tw_held_now || !_tw_on)) {
		for (void *p : *_th_pending) ThFree(p);
		_th_pending->clear();
	}
	ThDrainRing();
}

/** What the tick left while it ran, made now that it waits. */
static void TwDrain()
{
	_se_tick_marks = true; /* (what the tick says of a window's widgets is drawn at most every OPENTTD_WINDOW_MS) */
	for (TickTask *t : _tw_tasks) {
		t->Run();
		delete t;
	}
	_se_tick_marks = false;
	_tw_tasks.clear();
	TwAfterFlush();
	VehiclesSync(true);
}

static void TwLetGo()
{
	_tw_gui_company = _current_company;
	_tw_held_now = false;
	_tw_running = true;
	_tw_release.v = _tw_req.v;
}

void TwHold(bool between)
{
	if (!_tw_on) return;
	if (_tw_held_now) {
		if (!between || _tw_held_between) return;
		TwLetGo();
	}
	_tw_seq += 2;
	const uint32_t r = _tw_seq | (between ? 1 : 0);
	_tw_req.v = r;
	const uint32_t t = VpTime();
	while (_tw_ack.v != r) {
		TwService();
		TwGrant();
		mcw_rest();
	}
	_tw_hold_time += VpTime() - t;
	_tw_hold_most = std::max(_tw_hold_most, VpTime() - t);
	if (VpTime() - t > 200) {
		const uint32_t site = _tw_where.v == 0 ? 0 : _tw_site.v;
		for (LongHold &h : _tw_long) {
			if (h.count != 0 && h.site != site) continue;
			h.site = site, h.count++, h.most = std::max(h.most, VpTime() - t);
			break;
		}
	}
	YapfShipAheadStop(); /* (the first core may change the map while it holds the tick: no ship's path made ahead reads it) */
	_tw_holds++;
	_tw_held_now = true;
	_tw_held_between = between || _tw_where.v == 0;
	_tw_running = false;
	TwDrain();
}

void TwRelease()
{
	if (_tw_on && _tw_held_now) TwLetGo();
}

void TickReleaseForFlush()
{
	static const bool late = getenv("OPENTTD_TW_LATE") != nullptr; /* (a test: held through the flush) */
	if (!late) TwRelease();
}

bool TwHeld() { return _tw_held_now; }

/** Each game tick's part of the window system since the last, made now (the tick waits). */
void TwSync()
{
	if (!_tw_on) return;
	const uint64_t now = TimerGameTick::counter;
	uint64_t n = now - _tw_seen_ticks;
	_tw_seen_ticks = now;
	for (n = std::min<uint64_t>(n, 4); n > 0; n--) {
		CallWindowGameTickEvent();
		NewsLoop();
		if (_pause_mode.None() && HasBit(_display_opt, DO_FULL_ANIMATION)) DoPaletteAnimations();
	}
}

int VpWorkersOpen();
void WaterRegionsMakeAll();

/** The tick goes to a worker of its own, waiting there at once (false: none to be had). */
bool TwStart()
{
	if (_tw_on) return true;
	if (VpWorkersOpen() < 2) return false;
	/* (a loaded game has none of its water regions: ships' first paths made them one by one in the tick) */
	WaterRegionsMakeAll();
	YapfShipAheadCancel(); /* (the paths made ahead go by the tick's place: every job ends) */
	YapfShipAheadInit(); /* (and their slots made here, by the first core) */
	SeLinkGraphInit(); /* (the link graph's slots made here: the tick's worker hands its jobs to them) */
	if (_tw_stack == 0) {
		uint8_t *s = static_cast<uint8_t *>(__real_malloc(TW_STACK + 16));
		const char *mb = getenv("OPENTTD_TICK_HEAP");
		size_t size = static_cast<size_t>(mb != nullptr && atoi(mb) > 0 ? atoi(mb) : 4) << 20; /* (it takes 136 KB; memory in one piece is what a machine without paging runs out of) */
		uint8_t *h = nullptr;
		for (; size >= (4u << 20) && h == nullptr; size /= 2) h = static_cast<uint8_t *>(__real_malloc(size + 16));
		if (s == nullptr || h == nullptr) return false;
		size *= 2;
		const uint32_t t = VpTime();
		mcw_touch(s, TW_STACK + 16);
		mcw_touch(h, static_cast<unsigned>(size + 16));
		_tw_stack = (reinterpret_cast<uintptr_t>(s) + 15) & ~uintptr_t{15};
		_th_base = _th_top = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(h) + 15) & ~uintptr_t{15});
		_th_size = size;
		_th_end = _th_base + size;
		fmt::print(stderr, "openttd: the game tick on a worker: a heap of {} MB, made present in {:.1f} ms\n", size >> 20, (VpTime() - t) / 5.0);
	}
	_tw_regs = {};
	_tw_regs.r[0] = reinterpret_cast<uint32_t>(&TwMain);
	_tw_regs.r[1] = static_cast<uint32_t>(_tw_stack + TW_STACK - 16);
	_tw_stop.v = 0;
	_tw_ended.v = 0;
	_tw_gui_company = _current_company;
	/* (it starts waiting: the first core is in the middle of a picture) */
	_tw_seq += 2;
	_tw_req.v = _tw_seq;
	mcw_for_end();
	_tw_handle = mcw_try_call(TwJob, 0, 0);
	if (_tw_handle == 0) return false;
	_tw_on = true;
	while (_tw_ack.v != _tw_req.v) {
		TwService();
		mcw_rest();
	}
	_tw_held_now = _tw_held_between = true;
	_tw_running = false;
	_tw_seen_ticks = TimerGameTick::counter;
	return true;
}

/** The tick back on the first core, between two ticks. */
void TwStop()
{
	if (!_tw_on) return;
	VpFlushFinish();
	TwHold(true);
	YapfShipAheadCancel(); /* (the paths made ahead go by the tick's place: every job ends) */
	_tw_stop.v = 1;
	TwLetGo();
	while (_tw_ended.v == 0) {
		TwService();
		mcw_rest();
	}
	mcw_join(_tw_handle);
	_tw_on = false;
	_tw_running = false;
	_tw_held_now = false;
	TwDrain();
	static const bool traps = getenv("OPENTTD_TRAPS") != nullptr;
	if (traps) {
		fmt::print(stderr, "openttd: tick traps (TickFinish at {}):", reinterpret_cast<const void *>(&TickFinish));
		for (const TrapRec &t : _tw_traps) {
			if (t.at != nullptr) fmt::print(stderr, " {}x{}", t.at, t.count);
		}
		fmt::print(stderr, "\n");
	}
	fmt::print(stderr, "openttd: the tick's worker: {} ticks begun, ms ticking {}, waiting {}, idle {}, waiting to delete alone {}\n",
			_tw_loops, _tw_tick_time / 5, _tw_park_time / 5, _tw_idle_time / 5, _tw_excl_time / 5);
	fmt::print(stderr, "openttd: the tick's heap: {} KB taken, {} blocks of musl's asked for, {} given back\n",
			(_th_top - _th_base) >> 10, _th_remote, _th_ring_frees);
	fmt::print(stderr, "openttd: the first core's small blocks: {} KB of {} MB taken\n", (_fh_top - _fh_base) >> 10, _fh_size >> 20);
	std::string holds;
	for (const LongHold &h : _tw_long) {
		if (h.count != 0) holds += fmt::format(" {:x}: {} (the longest {} ms)", h.site, h.count, h.most / 5);
	}
	if (!holds.empty()) fmt::print(stderr, "openttd: holds over 40 ms, by where the tick stopped:{}\n", holds);
	SeLinkGraphStats();
	extern void YapfShipAheadStats();
	YapfShipAheadStats();
}

bool TwActive() { return _tw_on; }

/** For the play lines: the tick's ms in ticks and waiting so far, and its instructions in ticks. */
void TwTimes(uint32_t &tick, uint32_t &park, uint32_t &instret)
{
	tick = _tw_tick_time, park = _tw_park_time, instret = _tw_tick_instret;
}
