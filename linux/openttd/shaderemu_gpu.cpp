/* OpenTTD's picture drawn by the machine's GPU (docs/games/openttd.md, "The picture on the GPU").
 *
 * The blitter "8bpp-gpu" keeps each sprite's zoom levels as plain rows of indices in the sprite cache, where the
 * GPU reads them, and draws nothing itself: a sprite, a fill, a line's pixels, a remapped rectangle and a scroll
 * become records of the GPU's sprite command (CMD_SPRITES, docs/machine/gpu.md), a texel each, in the order the
 * game draws them. A list's records draw the picture's indices, the later nearer, into the GPU's target, which is
 * copied into a picture of words in GPU memory (red the index); the next list starts from that picture, and a
 * second list shows it through the palette in the window. A remap of what is there (the transparent buildings,
 * a shaded window) is drawn later in the list's blended pass and adds to green; whoever reads the picture next
 * makes the remaps (SPRITE_WORDS). Worker cores record the viewport's pieces into records of their own, which the
 * first core puts in the list after the round (SeGpuPiecesEnd). */

#include "../stdafx.h"
#include "../gfx_func.h"
#include "../zoom_func.h"
#include "../settings_type.h"
#include "../spritecache.h"
#include "../blitter/8bpp_base.hpp"
#include "../blitter/common.hpp"
#include "../blitter/factory.hpp"
#include "../core/math_func.hpp"
#include "shaderemu_gpu.h"
#include <sched.h>
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
void se_fps_caption(const GR_WINDOW_INFO *info, uint32_t now);   /* shaderemu_fps.c */
#include <mcw.h>
}

/** (getenv, before the safeguards) */
static const char *SeGpuEnv(const char *name) { return getenv(name); }
#include <cstdarg>
static void SeGpuLog(const char *f, ...) { va_list a; va_start(a, f); vfprintf(stderr, f, a); va_end(a); }

#include "../safeguards.h"

namespace {

/** A record of the sprite command: a texel. */
struct Rec {
	uint32_t at;    ///< the bytes' address (the record's first pixel), or a colour
	uint32_t remap; ///< a table of 256 bytes, or what a shade adds to green
	uint32_t xy;    ///< x | y << 11 | (stride & 1023) << 22
	uint32_t wh;    ///< w | h << 11 | kind << 22 | (stride >> 10) << 26
};
static_assert(sizeof(Rec) == 16);

enum Kind : uint32_t { K_COPY = 0, K_REMAP = 1, K_BLACK = 2, K_SHADE = 3, K_FILL = 4, K_FILL_SHADE = 5, K_CHECKER = 6, K_WORDS = 7, K_RAW = 8 };

inline Rec MakeRec(uint32_t at, uint32_t remap, int x, int y, int w, int h, Kind kind, uint32_t stride)
{
	return { at, remap, static_cast<uint32_t>(x) | static_cast<uint32_t>(y) << 11 | (stride & 1023) << 22,
			static_cast<uint32_t>(w) | static_cast<uint32_t>(h) << 11 | kind << 22 | (stride >> 10) << 26 };
}
inline int RecX(const Rec &r) { return r.xy & 0x7ff; }
inline int RecY(const Rec &r) { return (r.xy >> 11) & 0x7ff; }
inline int RecW(const Rec &r) { return r.wh & 0x7ff; }
inline int RecH(const Rec &r) { return (r.wh >> 11) & 0x7ff; }
inline Kind RecKind(const Rec &r) { return static_cast<Kind>((r.wh >> 22) & 15); }
inline bool Shades(Kind k) { return k == K_SHADE || k == K_FILL_SHADE; }

constexpr uint32_t CMD_SPRITES = 5;
constexpr int MAX_COMMANDS = 4096;
constexpr uint32_t MAX_VERTICES = 196608;
constexpr uint32_t MAX_RECORDS = MAX_VERTICES / 6;
constexpr uint32_t MAX_ORDER = (1u << 22) - 2 * MAX_RECORDS;
constexpr int LISTS = 4;
constexpr uint32_t CHUNK = 16384;        ///< records of a chunk of the first core's
constexpr uint32_t SIDE_CHUNK = 65536;   ///< bytes of its pixel runs and remaps
constexpr uint32_t WORKER_RECORDS = 8192;
constexpr uint32_t WORKER_SIDE = 4096;

/* the machine's GPU registers (docs/machine/gpu.md) */
enum : int { REG_SUBMIT = 0x10 / 4, REG_LIST = 0x14 / 4, REG_COUNT = 0x18 / 4, REG_DRAWN = 0x1c / 4, REG_CLOCK = 0x34 / 4, REG_COPIES = 0x38 / 4,
	REG_INTO = 0x50 / 4, REG_COPY = 0x70 / 4, REG_LOCK = 0x60 / 4, REG_BUFFERS = 0x64 / 4 };

template <class T> T *AlignedNew(size_t n)
{
	return static_cast<T *>(::operator new(n * sizeof(T), std::align_val_t{64}));
}
template <class T> void AlignedDelete(T *p)
{
	::operator delete(p, std::align_val_t{64});
}

/** A core's records. Only that core writes them; the first core reads them once the round is over. */
struct alignas(64) Stream {
	Rec *recs;
	uint32_t n, cap;
	uint8_t *side;    ///< bytes the records point at: pixel runs, remaps copied
	uint32_t side_n, side_cap;
	uint32_t shades;  ///< shade records made so far, to tell a range has any
	bool overflow;    ///< (a worker) it had no room: its piece is given up, and it has more room next frame
	/* the first core's */
	uint32_t run;     ///< its first record not yet in a command
	uint32_t run_shades;
};

/** A piece of the viewport: its records, on the core that drew it. In a texel of its own. */
struct alignas(16) Piece {
	Rec *recs;
	uint32_t count;
	uint16_t shades;
	uint8_t state;    ///< 0 none, 1 drawn on a worker, 2 put in the list by the first core
};

struct List {
	uint32_t *cmds;   ///< MAX_COMMANDS commands of 16 words
	int n;
	uint32_t vertices, order, passes;
	uint32_t ta, tb;  ///< the two remaps its shades count in green (made a, then b times: the first's 0x0f, the second's 0xf0)
	bool content;     ///< anything but the picture it starts from
	Rec *bg;          ///< that picture as a record
	uint32_t frame;   ///< (debugging) the frame it was made in
};
uint32_t _frame;

bool _on;
Stream _streams[VP_CORES];
std::vector<Rec *> _chunks_used, _chunks_free;   ///< the first core's, this frame and spare
std::vector<uint8_t *> _sides_used, _sides_free;
std::vector<Piece, std::allocator<Piece>> _pieces_v;
Piece *_pieces;
int _piece_count;
uint32_t _pieces_base;   ///< the order the pieces of a flush share
int _pieces_list = -1;   ///< in this list
bool _in_piece;          ///< the first core draws a piece: its runs go at the pieces' order
uint32_t _piece_offset;

List _lists[LISTS];
int _cur = -1;            ///< the list being made, -1 between frames
int _last = -1;           ///< the list whose picture is in _words
List _shows[LISTS];       ///< the lists that show the picture
int _show = 0;
uint32_t *_palettes;      ///< a palette for each of them

uint32_t *_words;         ///< the picture: a word a pixel, red the index, green the remaps still to make
int _width, _height, _row;
volatile uint32_t *_regs;
uint32_t _drawn_at, _copies_at;   ///< the GPU's counters our last list waits for: drawn, and its copy made
bool _pending;
GR_WINDOW_INFO _window_info;
uint32_t _buffers_seen;
bool _window_known;

/* remaps of what is there: the first table is green 1, the second 16 (a list of the first's after the second's
 * is not told apart from the other order) */
const uint8_t *_shade_a, *_shade_b;

/* the pointer: what is drawn between CopyToBuffer and the driver's MakeDirty, shown over the picture */
Rec *_overlay;
uint8_t *_overlay_side;
uint32_t _overlay_n, _overlay_side_n;
bool _capturing;
constexpr uint32_t OVERLAY_RECORDS = 64, OVERLAY_SIDE = 32768;

inline uint32_t Address(const void *p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

void Rest()
{
	mcw_rest();
}

/** The GPU is free: our last list is drawn (and its copy made once the copies counter moves). */
bool Drawn()
{
	return !_pending || static_cast<int32_t>(_regs[REG_DRAWN] - _drawn_at) > 0;
}

void WaitDrawn()
{
	while (!Drawn()) Rest();
}

/** A list to the GPU, its picture copied into (into, w, h, row): it waits for the lists before it. */
void Submit(const uint32_t *cmds, int n, uint32_t passes, uint32_t into, int w, int h, int row)
{
	for (;;) {
		while (__atomic_exchange_n(&_regs[REG_LOCK], 1u, __ATOMIC_ACQUIRE)) sched_yield();
		if (_regs[REG_SUBMIT] == 0) break;
		__atomic_store_n(&_regs[REG_LOCK], 0u, __ATOMIC_RELEASE);
		Rest();
	}
	_drawn_at = _regs[REG_DRAWN];
	_copies_at = _regs[REG_COPIES] + (_regs[REG_COPY] != 0 ? 1 : 0);   /* (a copy still to make is another's) */
	_regs[REG_INTO] = into;
	_regs[REG_INTO + 1] = w;
	_regs[REG_INTO + 2] = h;
	_regs[REG_INTO + 3] = row;
	_regs[REG_LIST] = Address(cmds);
	_regs[REG_COUNT] = n;
	_regs[REG_SUBMIT] = 1 | 4 | (passes & 0xfe) << 8;
	__atomic_store_n(&_regs[REG_LOCK], 0u, __ATOMIC_RELEASE);
	_pending = true;
}

/** A command of `count` records at `order` in the list being made; false when it has no room. */
bool Command(List &l, const Rec *recs, uint32_t count, uint32_t order, bool shades, uint32_t palette, uint32_t ta = 0, uint32_t tb = 0)
{
	if (l.n == MAX_COMMANDS || l.vertices + 6 * count > MAX_VERTICES) return false;
	uint32_t *c = l.cmds + 16 * l.n++;
	c[0] = CMD_SPRITES;
	c[1] = Address(recs);
	c[2] = count;
	c[3] = l.vertices;
	c[4] = 1 | (shades ? 4 : 0);
	c[5] = order;
	c[6] = palette;
	c[7] = ta;
	c[8] = tb;
	l.vertices += 6 * count;
	if (shades) l.passes |= 4;
	return true;
}

/** The two remaps the picture's green counts: the last list's. */
inline uint32_t LastA() { return _last >= 0 ? _lists[_last].ta : 0; }
inline uint32_t LastB() { return _last >= 0 ? _lists[_last].tb : 0; }

/** The list a frame's drawing goes on in: it starts from the picture as the last list left it. */
void StartList()
{
	_cur = (_last + 1) % LISTS;
	List &l = _lists[_cur];
	l.n = 0;
	l.vertices = 0;
	l.order = 1;
	l.passes = 1;
	l.content = false;
	l.frame = _frame;
	*l.bg = MakeRec(Address(_words), 0, 0, 0, _width, _height, K_WORDS, _row * 4);
	Command(l, l.bg, 1, 0, false, 0, LastA(), LastB());
}

/** The list being made to the GPU, into the picture; the next starts from it. */
void SubmitList()
{
	List &l = _lists[_cur];
	l.ta = Address(_shade_a);
	l.tb = Address(_shade_b);
	Submit(l.cmds, l.n, l.passes, Address(_words), _width, _height, _row);
	_last = _cur;
	_cur = -1;
}

void Break()
{
	SubmitList();
	StartList();
}

/**
 * Records on the list from `order` on, across lists while they do not fit: in a new list they go on at its
 * order, which is all the list has drawn. Whatever the list draws later lies nearer than they do.
 */
void Put(const Rec *recs, uint32_t count, uint32_t &order, bool shades)
{
	while (count != 0) {
		List &l = _lists[_cur];
		uint32_t room = std::min<uint32_t>((MAX_VERTICES - l.vertices) / 6, count);
		if (l.n == MAX_COMMANDS || room == 0 || order + room >= MAX_ORDER) {
			Break();
			order = _lists[_cur].order;
			continue;
		}
		Command(l, recs, room, order, shades, 0, 0);
		l.content = true;
		order += room;
		recs += room;
		count -= room;
		l.order = std::max(l.order, order);
	}
}

/** The order the pieces of a flush share in the list being made (made again in a list a piece broke into). */
uint32_t PieceBase()
{
	if (_pieces_list != _cur) {
		List &l = _lists[_cur];
		if (l.order + MAX_RECORDS >= MAX_ORDER) Break();
		_pieces_list = _cur;
		_pieces_base = _lists[_cur].order;
		_lists[_cur].order += MAX_RECORDS;
		_piece_offset = 0;
	}
	return _pieces_base;
}

/** The first core's records not yet in a command, put in the list (in a piece, at the pieces' order). */
void EmitRun()
{
	Stream &s = _streams[0];
	if (s.n == s.run) return;
	bool shades = s.shades != s.run_shades;
	if (_in_piece) {
		uint32_t base = PieceBase();
		int list = _cur;
		uint32_t order = base + _piece_offset;
		Put(s.recs + s.run, s.n - s.run, order, shades);
		/* (a piece that broke the list goes on after what of it the new list has) */
		if (_cur == list) _piece_offset = order - base;
		else _pieces_list = -1;
	} else {
		uint32_t order = _lists[_cur].order;
		Put(s.recs + s.run, s.n - s.run, order, shades);
	}
	s.run = s.n;
	s.run_shades = s.shades;
}

/** The first core's next chunk of records (the last stays alive until the frame is drawn). */
void NewChunk(Stream &s)
{
	EmitRun();
	if (s.recs != nullptr) _chunks_used.push_back(s.recs);
	if (_chunks_free.empty()) _chunks_free.push_back(AlignedNew<Rec>(CHUNK));
	s.recs = _chunks_free.back();
	_chunks_free.pop_back();
	s.n = s.run = 0;
	s.cap = CHUNK;
	s.run_shades = s.shades;
}

void NewSide(Stream &s)
{
	if (s.side != nullptr) _sides_used.push_back(s.side);
	if (_sides_free.empty()) _sides_free.push_back(AlignedNew<uint8_t>(SIDE_CHUNK));
	s.side = _sides_free.back();
	_sides_free.pop_back();
	s.side_n = 0;
	s.side_cap = SIDE_CHUNK;
}

/** A frame's first drawing: the frame before is drawn, the records start again. */
void FrameBegin()
{
	WaitDrawn();
	_frame++;
	Stream &s = _streams[0];
	for (Rec *c : _chunks_used) _chunks_free.push_back(c);
	_chunks_used.clear();
	for (uint8_t *c : _sides_used) _sides_free.push_back(c);
	_sides_used.clear();
	s.n = s.run = 0;
	s.side_n = 0;
	s.shades = s.run_shades = 0;
	for (int k = 1; k < VP_CORES; k++) {
		Stream &w = _streams[k];
		if (w.overflow || w.recs == nullptr) {
			/* (here, on the first core: a worker may not allocate) */
			uint32_t cap = w.recs == nullptr ? WORKER_RECORDS : w.cap * 2;
			uint32_t side = w.side == nullptr ? WORKER_SIDE : w.side_cap * 2;
			if (w.recs != nullptr) AlignedDelete(w.recs);
			if (w.side != nullptr) AlignedDelete(w.side);
			w.recs = AlignedNew<Rec>(cap);
			w.side = AlignedNew<uint8_t>(side);
			/* (their pages stored by this core first: docs/machine/multicore.md) */
			std::fill_n(reinterpret_cast<uint8_t *>(w.recs), cap * sizeof(Rec), 0);
			std::fill_n(w.side, side, 0);
			w.cap = cap;
			w.side_cap = side;
			w.overflow = false;
		}
		w.n = w.side_n = w.shades = 0;
	}
	StartList();
}

inline void EnsureFrame()
{
	if (_cur < 0) FrameBegin();
}

/** Room for a record on this core: false on a worker that has none (its piece is given up). */
inline Rec *NewRec(Stream &s, int k)
{
	if (s.n == s.cap) {
		if (k != 0) {
			s.overflow = true;
			VpRefuse();
			return nullptr;
		}
		NewChunk(s);
	}
	return &s.recs[s.n++];
}

/** Bytes beside the records (16-aligned): nullptr on a worker that has no room. */
inline uint8_t *NewSide(Stream &s, int k, uint32_t bytes)
{
	uint32_t at = (s.side_n + 15) & ~15u;
	if (at + bytes > s.side_cap) {
		if (k != 0) {
			s.overflow = true;
			VpRefuse();
			return nullptr;
		}
		NewSide(s);
		at = 0;
	}
	s.side_n = at + bytes;
	return s.side + at;
}

/** What a shade by `table` adds to green; 0 on a worker that meets a table not yet known (its piece is given up). */
uint32_t ShadeCode(const uint8_t *table, int k)
{
	if (table == _shade_a) return 1;
	if (table == _shade_b) return 16;
	if (k != 0) {
		VpRefuse();
		return 0;
	}
	if (_shade_a == nullptr) {
		_shade_a = table;
		return 1;
	}
	if (_shade_b != nullptr) {
		/* a third table: the list so far is made with the two, the next has this one second */
		EmitRun();
		Break();
	}
	_shade_b = table;
	return 16;
}

/** Where a pointer into the screen is; false for any other buffer. */
inline bool Where(const void *video, int &x, int &y)
{
	ptrdiff_t d = static_cast<const uint8_t *>(video) - static_cast<const uint8_t *>(_screen.dst_ptr);
	if (d < 0 || d >= static_cast<ptrdiff_t>(_screen.pitch) * _screen.height) return false;
	y = static_cast<int>(d / _screen.pitch);
	x = static_cast<int>(d % _screen.pitch);
	return true;
}

/** A remap table as the GPU will read it: a table a core may change again is copied (only its first entries: a
 * font's sprites have indices 0 to 2). */
inline const uint8_t *Snapshot(Stream &s, int k, const uint8_t *remap)
{
	const uint8_t *cores = reinterpret_cast<const uint8_t *>(_vp_core);
	if (remap < cores || remap >= cores + sizeof(_vp_core)) return remap;
	uint8_t *copy = NewSide(s, k, 16);
	if (copy == nullptr) return nullptr;
	std::copy_n(remap, 16, copy);
	return copy;
}

/** A pixel of colour c at (x, y) of the screen: joined to the row of pixels before it when it can be. */
void PutPixel(int x, int y, uint8_t c)
{
	if (x < 0 || y < 0 || x >= _width || y >= _height) return;
	int k = VpCore();
	if (k == 0) EnsureFrame();
	Stream &s = _streams[k];
	uint32_t first = s.run; /* (a pixel joins only a record of this piece, or of core 0's run: not one at another's order) */
	if (s.n > first) {
		Rec &r = s.recs[s.n - 1];
		Kind kind = RecKind(r);
		if (RecH(r) == 1 && RecY(r) == y) {
			int rx = RecX(r), rw = RecW(r);
			/* a row of pixels goes on */
			if (kind == K_RAW && x == rx + rw && r.at == Address(s.side) + s.side_n - rw && s.side_n < s.side_cap && rw < 2047) {
				s.side[s.side_n++] = c;
				r.wh += 1;
				return;
			}
			/* a checker's row (FILLRECT_CHECKER) goes on */
			uint8_t had = kind == K_RAW ? *reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(r.at)) : static_cast<uint8_t>(r.at);
			if (x == rx + rw + 1 && had == c && rw < 2046 && ((kind == K_RAW && rw == 1) || kind == K_CHECKER)) {
				r = MakeRec(c | ((rx + y) & 1) << 8, 0, rx, y, rw + 2, 1, K_CHECKER, 0);
				return;
			}
		}
	}
	uint8_t *b = NewSide(s, k, 1);
	if (b == nullptr) return;
	*b = c;
	/* (a run's bytes are joined only while they are the last: NewSide pads to 16, so the next is apart) */
	Rec *r = NewRec(s, k);
	if (r == nullptr) return;
	*r = MakeRec(Address(b), 0, x, y, 1, 1, K_RAW, 1);
}

/** Records not in the picture: a buffer of the game's own (a screenshot, a sprite made into RGBA). */
void DrawToMemory(Blitter::BlitterParams *bp, BlitterMode mode, const uint8_t *src, uint32_t stride)
{
	uint8_t *dst_line = static_cast<uint8_t *>(bp->dst) + bp->top * bp->pitch + bp->left;
	for (int y = 0; y < bp->height; y++, dst_line += bp->pitch, src += stride) {
		for (int x = 0; x < bp->width; x++) {
			uint8_t s = src[x];
			if (s == 0) continue;
			switch (mode) {
				case BlitterMode::ColourRemap:
				case BlitterMode::CrashRemap:
					if (bp->remap[s] != 0) dst_line[x] = bp->remap[s];
					break;
				case BlitterMode::BlackRemap: dst_line[x] = 0; break;
				case BlitterMode::Transparent:
				case BlitterMode::TransparentRemap: dst_line[x] = bp->remap[dst_line[x]]; break;
				default: dst_line[x] = s; break;
			}
		}
	}
}

} // namespace

/** The 8-bit blitter whose drawing the GPU makes. */
class Blitter_8bppGpu final : public Blitter_8bppBase {
public:
	/** A sprite's zoom levels as rows of indices, 0 a hole. */
	struct SpriteData {
		SpriteCollMap<uint32_t> offset; ///< from data
		SpriteCollMap<uint16_t> width;  ///< a row's bytes
		uint8_t data[];
	};

	void Draw(Blitter::BlitterParams *bp, BlitterMode mode, ZoomLevel zoom) override
	{
		const SpriteData *sd = static_cast<const SpriteData *>(bp->sprite);
		uint32_t stride = sd->width[zoom];
		const uint8_t *src = sd->data + sd->offset[zoom] + bp->skip_top * stride + bp->skip_left;
		int x, y;
		if (!Where(bp->dst, x, y)) {
			DrawToMemory(bp, mode, src, stride);
			return;
		}
		x += bp->left;
		y += bp->top;
		int k = VpCore();
		if (k == 0 && _capturing) {
			/* the pointer's sprites, copied: they are shown for as long as it stands there */
			uint32_t bytes = static_cast<uint32_t>(bp->width) * bp->height;
			if (_overlay_n == OVERLAY_RECORDS || mode == BlitterMode::Transparent || mode == BlitterMode::TransparentRemap) return;
			if (_overlay_side_n + 256 + bytes > OVERLAY_SIDE) return;
			uint32_t remap = 0;
			if (mode == BlitterMode::ColourRemap || mode == BlitterMode::CrashRemap) {
				std::copy_n(bp->remap, 256, _overlay_side + _overlay_side_n);
				remap = Address(_overlay_side + _overlay_side_n);
				_overlay_side_n += 256;
			}
			uint8_t *pixels = _overlay_side + _overlay_side_n;
			for (int r = 0; r < bp->height; r++) std::copy_n(src + r * stride, bp->width, pixels + r * bp->width);
			_overlay_side_n = (_overlay_side_n + bytes + 15) & ~15u;
			_overlay[_overlay_n++] = MakeRec(Address(pixels), remap, x, y, bp->width, bp->height,
					mode == BlitterMode::Normal ? K_COPY : mode == BlitterMode::BlackRemap ? K_BLACK : K_REMAP, bp->width);
			return;
		}
		if (k == 0) EnsureFrame();
		Stream &s = _streams[k];
		Kind kind;
		uint32_t remap = 0;
		switch (mode) {
			case BlitterMode::ColourRemap:
			case BlitterMode::CrashRemap: {
				const uint8_t *table = Snapshot(s, k, bp->remap);
				if (table == nullptr) return;
				kind = K_REMAP;
				remap = Address(table);
				break;
			}
			case BlitterMode::BlackRemap: kind = K_BLACK; break;
			case BlitterMode::Transparent:
			case BlitterMode::TransparentRemap:
				kind = K_SHADE;
				remap = ShadeCode(bp->remap, k);
				if (remap == 0) return;
				break;
			default: kind = K_COPY; break;
		}
		Rec *r = NewRec(_streams[k], k);
		if (r == nullptr) return;
		*r = MakeRec(Address(src), remap, x, y, bp->width, bp->height, kind, stride);
		if (kind == K_SHADE) _streams[k].shades++;
	}

	Sprite *Encode(SpriteType sprite_type, const SpriteLoader::SpriteCollection &sprite, SpriteAllocator &allocator) override
	{
		ZoomLevel zoom_min, zoom_max;
		if (sprite_type == SpriteType::Font) {
			zoom_min = ZoomLevel::Min;
			zoom_max = ZoomLevel::Min;
		} else {
			zoom_min = _settings_client.gui.zoom_min;
			zoom_max = static_cast<ZoomLevel>(std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
			if (zoom_max == zoom_min) zoom_max = ZoomLevel::Max;
		}
		size_t bytes = sizeof(SpriteData);
		for (ZoomLevel i = zoom_min; i <= zoom_max; i++) bytes += static_cast<size_t>(sprite[i].width) * sprite[i].height;
		Sprite *dest = allocator.Allocate<Sprite>(sizeof(Sprite) + bytes);
		const auto &root = sprite.Root();
		dest->height = root.height;
		dest->width = root.width;
		dest->x_offs = root.x_offs;
		dest->y_offs = root.y_offs;
		SpriteData *sd = reinterpret_cast<SpriteData *>(dest->data);
		sd->offset = {};
		sd->width = {};
		uint32_t at = 0;
		for (ZoomLevel i = zoom_min; i <= zoom_max; i++) {
			const SpriteLoader::Sprite &z = sprite[i];
			sd->offset[i] = at;
			sd->width[i] = z.width;
			const SpriteLoader::CommonPixel *p = z.data;
			for (size_t n = static_cast<size_t>(z.width) * z.height; n != 0; n--) sd->data[at++] = p++->m;
		}
		return dest;
	}

	void DrawColourMappingRect(void *dst, int width, int height, PaletteID pal) override
	{
		int x, y;
		const uint8_t *table = GetNonSprite(pal, SpriteType::Recolour) + 1;
		if (!Where(dst, x, y)) {
			uint8_t *p = static_cast<uint8_t *>(dst);
			for (int r = 0; r < height; r++, p += _screen.pitch) {
				for (int i = 0; i < width; i++) p[i] = table[p[i]];
			}
			return;
		}
		int k = VpCore();
		if (k == 0) EnsureFrame();
		uint32_t code = ShadeCode(table, k);
		if (code == 0 || width <= 0 || height <= 0) return;
		Rec *r = NewRec(_streams[k], k);
		if (r == nullptr) return;
		*r = MakeRec(0, code, x, y, width, height, K_FILL_SHADE, 0);
		_streams[k].shades++;
	}

	void SetPixel(void *video, int x, int y, PixelColour colour) override
	{
		int vx, vy;
		if (!Where(video, vx, vy)) {
			*(static_cast<uint8_t *>(video) + x + y * _screen.pitch) = colour.p;
			return;
		}
		PutPixel(vx + x, vy + y, colour.p);
	}

	void DrawLine(void *video, int x, int y, int x2, int y2, int screen_width, int screen_height, PixelColour colour, int width, int dash) override
	{
		int vx, vy;
		if (!Where(video, vx, vy)) {
			this->DrawLineGeneric(x, y, x2, y2, screen_width, screen_height, width, dash, [=](int px, int py) {
				*(static_cast<uint8_t *>(video) + px + py * _screen.pitch) = colour.p;
			});
			return;
		}
		this->DrawLineGeneric(x, y, x2, y2, screen_width, screen_height, width, dash, [=](int px, int py) {
			PutPixel(vx + px, vy + py, colour.p);
		});
	}

	void DrawRect(void *video, int width, int height, PixelColour colour) override
	{
		int x, y;
		if (!Where(video, x, y)) {
			uint8_t *p = static_cast<uint8_t *>(video);
			for (int r = 0; r < height; r++, p += _screen.pitch) std::fill_n(p, width, colour.p);
			return;
		}
		if (width <= 0 || height <= 0) return;
		int k = VpCore();
		if (k == 0) EnsureFrame();
		Rec *r = NewRec(_streams[k], k);
		if (r == nullptr) return;
		*r = MakeRec(colour.p, 0, x, y, width, height, K_FILL, 0);
	}

	/** The pointer is put back (or a chat message taken away): the GPU never drew it into the picture. */
	void CopyFromBuffer(void *, const void *, int, int) override
	{
		_overlay_n = 0;
		_overlay_side_n = 0;
	}

	/** What is under the pointer kept: its sprites, drawn next, are kept apart and shown over the picture. */
	void CopyToBuffer(const void *, void *, int, int) override
	{
		WaitDrawn(); /* (the lists that show the pointer read them) */
		_overlay_n = 0;
		_overlay_side_n = 0;
		_capturing = true;
	}

	void CopyImageToBuffer(const void *video, void *dst, int width, int height, int dst_pitch) override
	{
		int x, y;
		if (!Where(video, x, y)) {
			Blitter_8bppBase::CopyImageToBuffer(video, dst, width, height, dst_pitch);
			return;
		}
		SeGpuSettle();
		std::vector<uint8_t> picture;
		SeGpuPicture(picture);
		for (int r = 0; r < height; r++) {
			std::copy_n(picture.data() + (y + r) * _width + x, width, static_cast<uint8_t *>(dst) + r * dst_pitch);
		}
	}

	void ScrollBuffer(void *video, int &left, int &top, int &width, int &height, int scroll_x, int scroll_y) override
	{
		/* (as upstream's: the rectangle moved to, which left, top, width and height become) */
		if (scroll_y > 0) {
			top += scroll_y;
			height -= scroll_y;
		} else {
			height += scroll_y;
		}
		assert(height > 0);
		if (scroll_x >= 0) {
			left += scroll_x;
			width -= scroll_x;
		}
		else {
			width += scroll_x;
		}
		int vx, vy;
		if (!Where(video, vx, vy) || width <= 0) return;
		EnsureFrame();
		/* the picture as the last list left it, moved: whatever this list drew so far goes first */
		EmitRun();
		if (_lists[_cur].content) Break();
		int x = vx + left, y = vy + top, sx = x - scroll_x, sy = y - scroll_y;
		Stream &s = _streams[0];
		Rec *r = NewRec(s, 0);
		*r = MakeRec(Address(_words + sy * _row + sx), 0, x, y, width, height, K_WORDS, _row * 4);
		/* (its remaps are the last list's: a command of its own) */
		List &l = _lists[_cur];
		if (!Command(l, r, 1, l.order, false, 0, LastA(), LastB())) {
			Break();
			List &m = _lists[_cur];
			Command(m, r, 1, m.order, false, 0, LastA(), LastB());
		}
		_lists[_cur].order++;
		_lists[_cur].content = true;
		s.run = s.n;
		s.run_shades = s.shades;
	}

	std::string_view GetName() override { return "8bpp-gpu"; }
};

/** Factory for the blitter whose drawing the GPU makes. */
class FBlitter_8bppGpu : public BlitterFactory {
public:
	FBlitter_8bppGpu() : BlitterFactory("8bpp-gpu", "8bpp drawn by the machine's GPU (ShaderEmu)") {}
	std::unique_ptr<Blitter> CreateInstance() override { return std::make_unique<Blitter_8bppGpu>(); }
};
static FBlitter_8bppGpu iFBlitter_8bppGpu;

bool SeGpuOn()
{
	return _on;
}

bool SeGpuStart(int max_w, int max_h)
{
	_regs = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(seglPalette()) - 0x400);
	size_t bytes = static_cast<size_t>(max_w) * max_h * 4;
	{
		unsigned int spare = 0;
		void *p = seglMemorySpare(&spare);
		if (p != nullptr && spare >= bytes) _words = static_cast<uint32_t *>(p);
	}
	if (_words == nullptr) _words = static_cast<uint32_t *>(seglMemory(bytes));
	_palettes = static_cast<uint32_t *>(seglMemory(LISTS * 256 * 4));
	if (_words == nullptr || _palettes == nullptr) return false;
	std::fill_n(_words, static_cast<size_t>(max_w) * max_h, 0u);
	for (List &l : _lists) {
		l.cmds = AlignedNew<uint32_t>(MAX_COMMANDS * 16);
		l.bg = AlignedNew<Rec>(1);
	}
	for (List &l : _shows) {
		l.cmds = AlignedNew<uint32_t>(2 * 16);
		l.bg = AlignedNew<Rec>(1);
	}
	_overlay = AlignedNew<Rec>(OVERLAY_RECORDS);
	_overlay_side = AlignedNew<uint8_t>(OVERLAY_SIDE);
	_streams[0].recs = nullptr;
	NewChunk(_streams[0]);
	NewSide(_streams[0]);
	_on = true;
	return true;
}

void SeGpuResize(int w, int h)
{
	SeGpuSettle();
	_width = w;
	_height = h;
	_row = w;
	_last = -1; /* (the picture's rows moved: the next list draws everything, GameSizeChanged marks it all dirty) */
}

void SeGpuPointerDrawn()
{
	_capturing = false;
}

void SeGpuPaint(const uint32_t *palette, unsigned window)
{
	if (_cur >= 0) {
		EmitRun();
		if (_lists[_cur].content) SubmitList();
		else _cur = -1;
	}
	/* where the window's pixels are, asked again when the window system gives it another buffer */
	if (!_window_known || _regs[REG_BUFFERS] != _buffers_seen) {
		_buffers_seen = _regs[REG_BUFFERS];
		GrGetWindowInfo(window, &_window_info);
		_window_known = true;
	}
	if (!_window_info.realized || _window_info.surface_address == 0) return;
	_show = (_show + 1) % LISTS;
	List &l = _shows[_show];
	uint32_t *pal = _palettes + 256 * _show;
	std::copy_n(palette, 256, pal);
	l.n = 0;
	l.vertices = 0;
	l.passes = 1;
	int w = std::min<int>(_width, _window_info.width), h = std::min<int>(_height, _window_info.height);
	*l.bg = MakeRec(Address(_words), 0, 0, 0, w, h, K_WORDS, _row * 4);
	Command(l, l.bg, 1, 0, false, Address(pal), LastA(), LastB());
	if (_overlay_n != 0 && !_capturing) Command(l, _overlay, _overlay_n, 1, false, Address(pal), 0);
	Submit(l.cmds, l.n, l.passes, _window_info.surface_address, _window_info.width, _window_info.height, _window_info.surface_row);
	if (!_capturing) se_fps_caption(&_window_info, _regs[REG_CLOCK]);   /* (the frames shown, as the machine's OpenGL counts its own) */
}

void SeGpuSubmit()
{
	if (_cur < 0) return;
	EmitRun();
	if (_lists[_cur].content) SubmitList();
	else _cur = -1;
}

void SeGpuSettle()
{
	if (!_on) return;
	WaitDrawn();
	/* the copy of the last list's picture is made in the commit after it is drawn */
	while (_pending && static_cast<int32_t>(_regs[REG_COPIES] - _copies_at) <= 0) Rest();
	_pending = false;
}

void SeGpuPicture(std::vector<uint8_t> &out)
{
	out.resize(static_cast<size_t>(_width) * _height);
	const uint8_t *ta = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(LastA()));
	const uint8_t *tb = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(LastB()));
	for (int y = 0; y < _height; y++) {
		for (int x = 0; x < _width; x++) {
			uint32_t w = _words[y * _row + x];
			uint32_t index = (w >> 16) & 0xff, later = (w >> 8) & 0xff;
			for (uint32_t n = 0; n < (later & 15) && ta != nullptr; n++) index = ta[index];
			for (uint32_t n = 0; n < (later >> 4) && tb != nullptr; n++) index = tb[index];
			out[y * _width + x] = static_cast<uint8_t>(index);
		}
	}
	/* the pointer over it, as the 8-bit blitter has it in its picture */
	if (_capturing) return;
	for (uint32_t i = 0; i < _overlay_n; i++) {
		const Rec &r = _overlay[i];
		const uint8_t *src = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(r.at));
		const uint8_t *remap = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(r.remap));
		Kind kind = RecKind(r);
		for (int y = 0; y < RecH(r); y++) {
			for (int x = 0; x < RecW(r); x++) {
				int px = RecX(r) + x, py = RecY(r) + y;
				uint8_t s = src[y * RecW(r) + x];
				if (s == 0 || px >= _width || py >= _height) continue;
				uint8_t m = kind == K_REMAP ? remap[s] : kind == K_BLACK ? 0 : s;
				if (kind == K_REMAP && m == 0) continue;
				out[py * _width + px] = m;
			}
		}
	}
}

void SeGpuWho(int x, int y)
{
	for (int li = 0; li < LISTS; li++) {
		const List &l = _lists[li];
		if (l.frame != _frame) continue;
		for (int c = 0; c < l.n; c++) {
			const uint32_t *cmd = l.cmds + 16 * c;
			SeGpuLog("gpu who: list %d cmd %d: %u records from %08x, order %u, passes %u\n", li, c, cmd[2], cmd[1], cmd[5], cmd[4]);
			const Rec *recs = reinterpret_cast<const Rec *>(static_cast<uintptr_t>(cmd[1]));
			for (uint32_t k = 0; k < cmd[2]; k++) {
				const Rec &r = recs[k];
				if (x < RecX(r) || y < RecY(r) || x >= RecX(r) + RecW(r) || y >= RecY(r) + RecH(r)) continue;
				uint32_t stride = (r.xy >> 22) | (r.wh >> 26) << 10;
				int byte = -1;
				Kind kind = RecKind(r);
				if (kind == K_COPY || kind == K_REMAP || kind == K_BLACK || kind == K_SHADE || kind == K_RAW) {
					byte = *reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(r.at + (y - RecY(r)) * stride + (x - RecX(r))));
				}
				SeGpuLog("gpu who %d,%d: list %d%s cmd %d rec %u order %u kind %u at %08x remap %08x %d,%d %dx%d stride %u byte %d\n",
					x, y, li, li == _cur ? " (cur)" : li == _last ? " (last)" : "", c, k, cmd[5] + k, kind, r.at, r.remap,
					RecX(r), RecY(r), RecW(r), RecH(r), stride, byte);
			}
		}
	}
}

void SeGpuPiecesBegin(int pieces)
{
	EnsureFrame();
	EmitRun();
	if (static_cast<int>(_pieces_v.size()) < pieces) _pieces_v.resize(pieces * 2);
	_pieces = _pieces_v.data();
	_piece_count = pieces;
	for (int i = 0; i < pieces; i++) _pieces[i] = {};
	_pieces_list = -1;
	PieceBase();
}

void SeGpuPieceBegin(int piece)
{
	int k = VpCore();
	Stream &s = _streams[k];
	if (k == 0) {
		EmitRun();
		_in_piece = true;
		_piece_offset = 0; /* (the pieces share an order: they do not overlap) */
		return;
	}
	/* (a worker: the piece's records from here; its shades so far kept in its count until the end) */
	_pieces[piece].recs = s.recs + s.n;
	_pieces[piece].count = s.shades;
	s.run = s.n;
}

void SeGpuPieceEnd(int piece, bool failed)
{
	int k = VpCore();
	Stream &s = _streams[k];
	Piece &p = _pieces[piece];
	if (k == 0) {
		if (failed) s.n = s.run;
		else EmitRun();
		_in_piece = false;
		p.state = failed ? 0 : 2;
		return;
	}
	uint32_t from = static_cast<uint32_t>(p.recs - s.recs);
	if (failed) {
		s.n = from;
		p.state = 0;
		return;
	}
	p.shades = static_cast<uint16_t>(s.shades != p.count);
	p.count = s.n - from;
	p.state = 1;
}

void SeGpuPiecesEnd()
{
	for (int i = 0; i < _piece_count; i++) {
		Piece &p = _pieces[i];
		if (p.state != 1 || p.count == 0) continue;
		uint32_t base = PieceBase();
		int list = _cur;
		uint32_t order = base;
		Put(p.recs, p.count, order, p.shades != 0);
		if (_cur != list) _pieces_list = -1;
	}
	_piece_count = 0;
	_pieces_list = -1;
}
