/* OpenTTD's screen in a Nano-X window (docs/games/openttd.md): the 8-bit blitter draws into a
 * paletted texture in GPU memory, which the GPU draws over the whole window. One thread: the
 * game's ticks and the drawing take turns, and a wait for the next one also waits for input. */

#include "../stdafx.h"
#include "../openttd.h"
#include "../gfx_func.h"
#include "../palette_func.h"
#include "../blitter/factory.hpp"
#include "../core/geometry_func.hpp"
#include "../core/math_func.hpp"
#include "../core/random_func.hpp"
#include "../framerate_type.h"
#include "../progress.h"
#include "../window_func.h"
#include "../timer/timer_game_tick.h"
#include "../network/network.h"
#include "../window_gui.h"
#include "../viewport_func.h"
#include "../tilehighlight_func.h"
#include "../vehicle_gui.h"
#include "../station_func.h"
#include "../station_base.h"
#include "../train.h"
#include "../zoom_func.h"
#include "../settings_type.h"
#include "../command_func.h"
#include "../company_func.h"
#include "../company_base.h"
#include "../genworld.h"
#include "../map_func.h"
#include "../vehicle_base.h"
#include "../vehicle_func.h"
#include "../timer/timer_game_calendar.h"
#include "../gui.h"
#include "../company_gui.h"
#include "shaderemu_v.h"
#include "shaderemu_gpu.h"
#include <unistd.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <utility>
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
}

#include "../safeguards.h"

static FVideoDriver_ShaderEmu iFVideoDriver_ShaderEmu;

static GR_WINDOW_ID _window;
static GLuint _texture;
static uint8_t *_picture;          ///< the screen's pixels, in GPU memory: the texture
static int _picture_w, _picture_h; ///< the most it holds
static int _screen_cols, _screen_rows;
static GR_EVENT _pending;          ///< an event that ended a wait, handled next
static bool _have_pending;
static int _draws;                 ///< pictures drawn, for OPENTTD_RATE
static int _paints;                ///< pictures painted, for the play lines
static bool _drawn;                ///< a list is with the GPU: settle before writing the picture
static int _pointer_x, _pointer_y; ///< where the pointer was last, for moves while OpenTTD holds it
static unsigned _keys_down;        ///< bit 0-3 the arrows (as _dirkeys), 4 Tab
static GR_KEYMOD _modifiers;

#define EVENTS (GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_EXPOSURE | \
		GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)

std::optional<std::string_view> VideoDriver_ShaderEmu::Start(const StringList &)
{
	/* the GPU draws the picture (docs/games/openttd.md, "The picture on the GPU"); OPENTTD_DRAW=cpu: the 8-bit blitter */
	static const char *draw = getenv("OPENTTD_DRAW");
	bool gpu = draw == nullptr || std::string_view(draw) != "cpu";
	if (gpu && BlitterFactory::SelectBlitter("8bpp-gpu") != nullptr) _blitter_autodetected = false;
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 8) return "only the 8-bit blitters";
	if (GrOpen() < 0) return "no Nano-X server (start one: nano-X -p &)";

	GR_SCREEN_INFO info;
	GrGetScreenInfo(&info);
	_screen_cols = info.cols;
	_screen_rows = info.rows;
	if (_cur_resolution.width == 0 || _cur_resolution.height == 0) _cur_resolution = { 640, 480 };
	_cur_resolution.width = Clamp<uint>(_cur_resolution.width, 640, _screen_cols);
	_cur_resolution.height = Clamp<uint>(_cur_resolution.height, 480, _screen_rows);

	_window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "OpenTTD", GR_ROOT_WINDOW_ID, -1, -1, _cur_resolution.width,
			_cur_resolution.height, 0);
	GrSelectEvents(_window, EVENTS);
	/* OpenTTD draws its own pointer, the tool's picture: the window system's is hidden over it */
	static const GR_BITMAP none[16] = {};
	GrSetCursor(_window, 16, 16, 0, 0, 0, 0, const_cast<GR_BITMAP *>(none), const_cast<GR_BITMAP *>(none));
	GrMapWindow(_window);
	GrSetFocus(_window);
	if (seglInit(_window) < 0) return "this machine has no GPU";
	/* the machine's clock in milliseconds, the word 0x34 of its control words (seglPalette() is the word 0x400):
	 * the frame rate window's measurements read it, not the system's clock (framerate_gui.cpp) */
	extern const volatile uint32_t *_se_clock_ms;
	_se_clock_ms = reinterpret_cast<const volatile uint32_t *>(reinterpret_cast<const char *>(seglPalette()) - 0x400 + 0x34);

	/* The game loads for a minute at 2 MIPS without a picture: the window says so, drawn at each
	 * exposure of its first second, which is also the window manager's time to frame it. */
	GR_GC_ID gc = GrNewGC();
	GrSetGCForeground(gc, GR_RGB(224, 224, 224));
	GrSetGCBackground(gc, GR_RGB(0, 0, 0));
	for (auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1); std::chrono::steady_clock::now() < until;) {
		GR_EVENT event;
		GrGetNextEventTimeout(&event, 100);
		if (event.type == GR_EVENT_TYPE_EXPOSURE) GrText(_window, gc, 16, 24, const_cast<char *>("Loading OpenTTD..."), -1, GR_TFASCII);
	}
	GrDestroyGC(gc);

	_picture_w = std::max(_screen_cols, 640);
	_picture_h = std::max(_screen_rows, 480);
	_picture_w = std::min(_picture_w, 2040);
	_picture_h = std::min(_picture_h, 2040);
	if (BlitterFactory::GetCurrentBlitter()->GetName() == "8bpp-gpu" && !SeGpuStart(_picture_w, _picture_h)) {
		/* (no GPU memory for its picture of words: the 8-bit blitter) */
		BlitterFactory::SelectBlitter("8bpp-optimized");
	}
	/* (with the GPU's picture, only addresses into this one are made: the blitter tells where by them) */
	_picture = SeGpuOn() ? new uint8_t[_picture_w * _picture_h]() : static_cast<uint8_t *>(seglMemory(_picture_w * _picture_h));
	if (_picture == nullptr) return "no GPU memory for the screen";
	/* (the GPU's memory is handed out from the same place to every program: it holds the last one's picture) */
	std::fill_n(_picture, static_cast<size_t>(_picture_w) * _picture_h, uint8_t{0});
	glGenTextures(1, &_texture);
	if (!this->Resize(_cur_resolution.width, _cur_resolution.height)) return "no screen";

	MarkWholeScreenDirty();
	this->is_game_threaded = false;
	return std::nullopt;
}

void VideoDriver_ShaderEmu::Stop()
{
	SeGpuSettle();
	if (_drawn) seglSettle();
	GrClose();
}

/** The picture is w x h pixels from now on: its rows are as long as it is wide. */
bool VideoDriver_ShaderEmu::Resize(int w, int h)
{
	w = Clamp(w, 64, _picture_w);
	h = Clamp(h, 64, _picture_h);
	if (_drawn) seglSettle(), _drawn = false;

	if (SeGpuOn()) {
		SeGpuResize(w, h);
	} else {
		glBindTexture(GL_TEXTURE_2D, _texture);
		seglTexturePointer(_picture, w, h, GL_COLOR_INDEX8_EXT);
	}
	_screen.width = w;
	_screen.height = h;
	_screen.pitch = w;
	_screen.dst_ptr = _picture;
	_cur_resolution = { static_cast<uint>(w), static_cast<uint>(h) };
	this->dirty_rect = {};

	CopyPalette(this->local_palette, true);
	BlitterFactory::GetCurrentBlitter()->PostResize();
	GameSizeChanged();
	return true;
}

void VideoDriver_ShaderEmu::MakeDirty(int left, int top, int width, int height)
{
	Rect r = { left, top, left + width, top + height };
	this->dirty_rect = BoundingRect(this->dirty_rect, r);
	SeGpuPointerDrawn(); /* (DrawMouseCursor's MakeDirty ends the pointer's sprites) */
}

void VideoDriver_ShaderEmu::CheckPaletteAnim()
{
	if (!CopyPalette(this->local_palette)) return;
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

bool VideoDriver_ShaderEmu::LockVideoBuffer()
{
	if (_drawn && !SeGpuOn()) seglSettle(), _drawn = false;
	_screen.dst_ptr = _picture;
	return false;
}

void VideoDriver_ShaderEmu::UnlockVideoBuffer() {}

/* OPENTTD_CHECK=1: every picture is drawn again whole and the pixels where the two differ are
 * counted (a scroll or a band left wrong). OPENTTD_HOLD=N|bad|big: the program stops at picture N
 * of the play script, or at the first that differs (by over 1,000 pixels), and says where both
 * pictures are in memory. */
static int _play_frame = -1;     ///< pictures of OPENTTD_PLAY's script so far, -1 before it
static int _checked, _check_bad, _check_line, _check_part_line;
static long long _check_pixels;

static int Number(const char *s)
{
	if (s == nullptr || *s < '0' || *s > '9') return -2;
	int n = 0;
	for (; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
	return n;
}

/** OPENTTD_WINDOW_MS: how often what the tick changed in windows is drawn, 1,000 ms; in the machine's 5 kHz time. */
uint32_t SeWindowEvery()
{
	const int n = Number(getenv("OPENTTD_WINDOW_MS"));
	return (n >= 0 ? n : 1000) * 5;
}

static void HoldHere(const uint8_t *shown, int w, int h)
{
	fmt::print(stderr, "openttd: hold at picture {}: picture {:x} shown {:x} {}x{} palette {:x}\n", _play_frame,
			reinterpret_cast<uintptr_t>(_picture), reinterpret_cast<uintptr_t>(shown), w, h, reinterpret_cast<uintptr_t>(seglPalette()));
	for (;;) usleep(1000000);
}

void InvalidateDirtyCompanyFinances();
void NetworkDrawChatMessage();
void UpdateWindowsEarly();

static void CheckPicture()
{
	static const char *check_how = getenv("OPENTTD_CHECK");
	static const bool check = check_how != nullptr;
	static const bool keep = check && std::string_view(check_how) == "keep"; /* the picture as drawn is shown on, its errors kept */
	static const char *hold = getenv("OPENTTD_HOLD");
	int w = _screen.width, h = _screen.height;
	if (!check || _game_mode != GM_NORMAL) {
		if (hold != nullptr && _play_frame == Number(hold)) {
			if (SeGpuOn()) {
				/* (the GPU's picture, in the bytes the 8-bit blitter would have drawn) */
				static std::vector<uint8_t> drawn;
				SeGpuSubmit();
				SeGpuSettle();
				SeGpuPicture(drawn);
				std::copy(drawn.begin(), drawn.end(), _picture);
				/* OPENTTD_HOLD_WHO=x,y: the records that cover that pixel */
				if (const char *who = getenv("OPENTTD_HOLD_WHO"); who != nullptr) {
					const char *comma = strchr(who, ',');
					if (comma != nullptr) SeGpuWho(Number(who), Number(comma + 1));
				}
			}
			HoldHere(_picture, w, h);
		}
		return;
	}
	VpFlushFinish();
	UndrawMouseCursor();
	const bool worker = TwActive();
	if (worker) {
		TwHold(false);
		VehiclesSync(true); /* (what the tick said it moved since the picture's flush, as the next picture would) */
		InvalidateDirtyCompanyFinances(); /* (as the next picture's window timer would) */
		SeDirtyLaterDue(true);
		DrawDirtyBlocks();
	}
	static std::vector<uint8_t> shown;
	/* (every vehicle's copy made anew: one the tick changed and did not say so differs) */
	VehiclesChangedAll();
	VehiclesSync(worker);
	if (SeGpuOn()) {
		/* the GPU's picture as drawn, then drawn again whole */
		SeGpuSubmit();
		SeGpuSettle();
		SeGpuPicture(shown);
		MarkWholeScreenDirty();
		DrawDirtyBlocks();
		SeGpuSubmit();
		SeGpuSettle();
		static std::vector<uint8_t> whole;
		SeGpuPicture(whole);
		std::copy(whole.begin(), whole.end(), _picture);
	} else {
		shown.assign(_picture, _picture + w * h);
		MarkWholeScreenDirty();
		DrawDirtyBlocks();
	}
	int bad = 0, l = w, t = h, r = -1, b = -1, line = 0;
	static std::vector<int> down; /* differing pixels in a column so far, ending at this row */
	down.assign(w, 0);
	for (int y = 0; y < h; y++) {
		int across = 0;
		for (int x = 0; x < w; x++) {
			if (shown[y * w + x] == _picture[y * w + x]) {
				across = down[x] = 0;
				continue;
			}
			bad++;
			line = std::max({ line, ++across, ++down[x] });
			l = std::min(l, x), r = std::max(r, x), t = std::min(t, y), b = std::max(b, y);
		}
	}
	_checked++;
	if (bad != 0) {
		_check_bad++;
		_check_pixels += bad;
		_check_line = std::max(_check_line, line);
		if (_play_frame >= 2) _check_part_line = std::max(_check_part_line, line); /* (the first shows "paused") */
		fmt::print(stderr, "openttd: check: picture {} (play {}): {} pixels differ, in {},{} to {},{}, longest line {}\n",
				_checked, _play_frame, bad, l, t, r, b, line);
		Window *mw = std::string_view(check_how) == "list" ? FindWindowById(WC_MAIN_WINDOW, 0) : nullptr;
		if (mw != nullptr && mw->viewport != nullptr) { /* (the vehicles near the difference) */
			const Viewport &vp = *mw->viewport;
			for (const Vehicle *v : Vehicle::Iterate()) {
				if (v->coord.left == INVALID_COORD) continue;
				int vl = UnScaleByZoom(v->coord.left - vp.virtual_left, vp.zoom) + vp.left, vt = UnScaleByZoom(v->coord.top - vp.virtual_top, vp.zoom) + vp.top;
				int vr = UnScaleByZoom(v->coord.right - vp.virtual_left, vp.zoom) + vp.left, vb = UnScaleByZoom(v->coord.bottom - vp.virtual_top, vp.zoom) + vp.top;
				if (vr < l - 8 || vl > r + 8 || vb < t - 8 || vt > b + 8) continue;
				fmt::print(stderr, "openttd: near {} type {} sub {} screen {},{} to {},{} status {:x} hidden {} pos {},{},{} rev {}\n", v->index.base(), (int)v->type, v->subtype,
						vl, vt, vr, vb, v->vehstatus.base(), v->drawn.hidden, v->x_pos, v->y_pos, v->z_pos, v->sprite_cache.revalidate_before_draw);
			}
		}
		if (std::string_view(check_how) == "list") { /* where, and the colour shown and drawn whole */
			int n = 0;
			for (int i = 0; i < w * h && n < 12; i++) {
				if (shown[i] != _picture[i]) fmt::print(stderr, "  {},{} {} {}", i % w, i / w, shown[i], _picture[i]), n++;
			}
			fmt::print(stderr, "\n");
		}
	}
	if (hold != nullptr && (_play_frame == Number(hold) || (bad != 0 && _play_frame >= 0 && std::string_view(hold) == "bad") ||
			(bad > 1000 && _play_frame >= 0 && std::string_view(hold) == "big"))) HoldHere(shown.data(), w, h);
	if (keep && !SeGpuOn()) std::copy(shown.begin(), shown.end(), _picture);
	DrawMouseCursor();
	if (worker) TwRelease();
}

/** The picture over the whole window, submitted and not waited for: the next lock settles. */
void VideoDriver_ShaderEmu::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);
	static const GLfixed ONE = 65536;
	static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
	static const GLfixed xyz[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 };
	static const GLfixed uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };

	static const bool check_all = getenv("OPENTTD_CHECK") != nullptr; /* (a picture with nothing drawn checked too) */
	if (IsEmptyRect(this->dirty_rect) && this->local_palette.count_dirty == 0 && !(check_all && _play_frame >= 0)) return;
	if (_drawn) seglSettle(), _drawn = false;
	_draws++;
	_paints++;
	CheckPicture();
	if (SeGpuOn()) {
		/* the frame's records drawn by the GPU, then shown through the palette */
		static uint32_t words[256];
		for (int i = 0; i < 256; i++) {
			const Colour &c = this->local_palette.palette[i];
			words[i] = static_cast<uint32_t>(c.r) << 16 | static_cast<uint32_t>(c.g) << 8 | c.b;
		}
		this->local_palette.count_dirty = 0;
		SeGpuPaint(words, _window);
		this->dirty_rect = {};
		return;
	}

	if (this->local_palette.count_dirty != 0) {
		unsigned *words = seglPalette();
		for (int i = this->local_palette.first_dirty; i < this->local_palette.first_dirty + this->local_palette.count_dirty; i++) {
			const Colour &c = this->local_palette.palette[i];
			words[i] = static_cast<unsigned>(c.r) << 16 | static_cast<unsigned>(c.g) << 8 | c.b;
		}
		this->local_palette.count_dirty = 0;
	}

	int width, height;
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(xyz, uv, _texture, 255, 255, 0);
	seglSwapEarly();
	_drawn = true;
	this->dirty_rect = {};
}

/* A key as OpenTTD numbers it (WKC_*), and the character it types into *character */
static uint KeyOf(const GR_EVENT_KEYSTROKE &k, char32_t *character)
{
	static const char shifted[] = ")!@#$%^&*(";
	GR_KEY ch = k.ch;
	uint key = 0;
	bool printable = ch >= ' ' && ch < 0x7f;

	if (ch >= 'a' && ch <= 'z') key = ch - 'a' + 'A';
	else if (ch >= 'A' && ch <= 'Z') key = ch;
	else if (ch >= '0' && ch <= '9') key = ch;
	else if (ch >= MWKEY_F1 && ch <= MWKEY_F10) key = WKC_F1 + (ch - MWKEY_F1);
	else if (ch >= MWKEY_KP0 && ch <= MWKEY_KP9) key = '0' + (ch - MWKEY_KP0), printable = true, ch = key;
	else if (const char *s = ch > 0 && ch < 0x80 ? strchr(shifted, ch) : nullptr) key = '0' + (s - shifted);
	else {
		switch (ch) {
			case MWKEY_F11: key = WKC_F11; break;
			case MWKEY_F12: key = WKC_F12; break;
			case MWKEY_PAGEUP: key = WKC_PAGEUP; break;
			case MWKEY_PAGEDOWN: key = WKC_PAGEDOWN; break;
			case MWKEY_UP: key = WKC_UP; break;
			case MWKEY_DOWN: key = WKC_DOWN; break;
			case MWKEY_LEFT: key = WKC_LEFT; break;
			case MWKEY_RIGHT: key = WKC_RIGHT; break;
			case MWKEY_HOME: key = WKC_HOME; break;
			case MWKEY_END: key = WKC_END; break;
			case MWKEY_INSERT: key = WKC_INSERT; break;
			case MWKEY_DELETE: case 127: key = WKC_DELETE; printable = false; break;
			case MWKEY_ESCAPE: key = WKC_ESC; printable = false; break;
			case MWKEY_PAUSE: key = WKC_PAUSE; break;
			case MWKEY_BACKSPACE: key = WKC_BACKSPACE; break;
			case ' ': key = WKC_SPACE; break;
			case MWKEY_ENTER: case '\n': key = WKC_RETURN; break;
			case MWKEY_TAB: key = WKC_TAB; break;
			case MWKEY_KP_DIVIDE: key = WKC_NUM_DIV; break;
			case MWKEY_KP_MULTIPLY: key = WKC_NUM_MUL; break;
			case MWKEY_KP_MINUS: key = WKC_NUM_MINUS; break;
			case MWKEY_KP_PLUS: key = WKC_NUM_PLUS; break;
			case MWKEY_KP_ENTER: key = WKC_NUM_ENTER; break;
			case MWKEY_KP_PERIOD: key = WKC_NUM_DECIMAL; break;
			case '/': case '?': key = WKC_SLASH; break;
			case ';': case ':': key = WKC_SEMICOLON; break;
			case '=': case '+': key = WKC_EQUALS; break;
			case '[': case '{': key = WKC_L_BRACKET; break;
			case '\\': case '|': key = WKC_BACKSLASH; break;
			case ']': case '}': key = WKC_R_BRACKET; break;
			case '\'': case '"': key = WKC_SINGLEQUOTE; break;
			case ',': case '<': key = WKC_COMMA; break;
			case '-': case '_': key = WKC_MINUS; break;
			case '.': case '>': key = WKC_PERIOD; break;
			case '`': case '~': key = WKC_BACKQUOTE; break;
		}
	}
	if (k.modifiers & MWKMOD_META) key |= WKC_META;
	if (k.modifiers & MWKMOD_SHIFT) key |= WKC_SHIFT;
	if (k.modifiers & MWKMOD_CTRL) key |= WKC_CTRL;
	if (k.modifiers & MWKMOD_ALT) key |= WKC_ALT;
	*character = printable && !(k.modifiers & (MWKMOD_META | MWKMOD_CTRL | MWKMOD_ALT)) ? static_cast<char32_t>(ch) : WKC_NONE;
	return key;
}

static unsigned HeldBit(GR_KEY ch)
{
	switch (ch) {
		case MWKEY_LEFT: return 1;
		case MWKEY_UP: return 2;
		case MWKEY_RIGHT: return 4;
		case MWKEY_DOWN: return 8;
		case MWKEY_TAB: return 16;
		default: return 0;
	}
}

static void PointerAt(int x, int y)
{
	if (_cursor.fix_at) {
		/* OpenTTD holds the pointer still while the right button drags the view: the moves count */
		_cursor.UpdateCursorPositionRelative(x - _pointer_x, y - _pointer_y);
	} else {
		_cursor.UpdateCursorPosition(x, y);
	}
	_pointer_x = x;
	_pointer_y = y;
}

bool VideoDriver_ShaderEmu::PollEvent()
{
	GR_EVENT e;

	if (_have_pending) {
		e = _pending;
		_have_pending = false;
	} else {
		GrCheckNextEvent(&e);
	}
	switch (e.type) {
		case GR_EVENT_TYPE_NONE:
			return false;

		case GR_EVENT_TYPE_MOUSE_MOTION:
			_modifiers = e.mouse.modifiers;
			PointerAt(e.mouse.x, e.mouse.y);
			HandleMouseEvents();
			break;

		case GR_EVENT_TYPE_BUTTON_DOWN:
			_modifiers = e.button.modifiers;
			PointerAt(e.button.x, e.button.y);
			if (e.button.changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) {
				int notch = (e.button.changebuttons & GR_BUTTON_SCROLLUP) ? -1 : 1;
				_cursor.wheel += notch;
				_cursor.v_wheel += notch * 14.0f * _settings_client.gui.scrollwheel_multiplier;
				_cursor.wheel_moved = true;
				break;
			}
			if (e.button.changebuttons & GR_BUTTON_L) {
				if (_rightclick_emulate && (e.button.modifiers & MWKMOD_CTRL)) {
					_right_button_down = true;
					_right_button_clicked = true;
				} else {
					_left_button_down = true;
				}
			}
			if (e.button.changebuttons & GR_BUTTON_R) {
				_right_button_down = true;
				_right_button_clicked = true;
			}
			HandleMouseEvents();
			break;

		case GR_EVENT_TYPE_BUTTON_UP:
			_modifiers = e.button.modifiers;
			PointerAt(e.button.x, e.button.y);
			if (e.button.changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) break;
			if (_rightclick_emulate) {
				_right_button_down = false;
				_left_button_down = false;
				_left_button_clicked = false;
			} else {
				if (e.button.changebuttons & GR_BUTTON_L) _left_button_down = false, _left_button_clicked = false;
				if (e.button.changebuttons & GR_BUTTON_R) _right_button_down = false;
			}
			HandleMouseEvents();
			break;

		case GR_EVENT_TYPE_MOUSE_ENTER:
			_cursor.in_window = true;
			break;

		case GR_EVENT_TYPE_MOUSE_EXIT:
			UndrawMouseCursor();
			_cursor.in_window = false;
			break;

		case GR_EVENT_TYPE_KEY_DOWN: {
			char32_t character;
			_modifiers = e.keystroke.modifiers;
			_keys_down |= HeldBit(e.keystroke.ch);
			uint key = KeyOf(e.keystroke, &character);
			if (key != 0 || character != WKC_NONE) HandleKeypress(key, character);
			break;
		}

		case GR_EVENT_TYPE_KEY_UP:
			_modifiers = e.keystroke.modifiers;
			_keys_down &= ~HeldBit(e.keystroke.ch);
			break;

		case GR_EVENT_TYPE_EXPOSURE:
			this->MakeDirty(0, 0, _screen.width, _screen.height);
			break;

		case GR_EVENT_TYPE_UPDATE:
			if (e.update.wid == _window && e.update.utype == GR_UPDATE_SIZE) {
				seglWindowChanged();
				if (e.update.width != _screen.width || e.update.height != _screen.height) this->Resize(e.update.width, e.update.height);
			}
			break;

		case GR_EVENT_TYPE_CLOSE_REQ:
			HandleExitGameRequest();
			break;
	}
	return true;
}

/* OPENTTD_PLAY=1, for measuring play: a script of pictures once a game is up, the same views at any
 * speed (docs/games/openttd.md, "Speed"): scrolling, zooming out and in, then three trains' windows
 * and a station's open with the main view following a train. It prints each part's pictures a
 * second and exits; OPENTTD_PLAY=N repeats the script N times. OPENTTD_PLAY_SHORT=1: 30 pictures
 * scrolling, 20 zoomed out and 20 with the windows, for a run on the card. */
/* OPENTTD_PLAY_PART=scroll|follow: that part alone, the scroll's 60 pictures or 60 following a train */
static const int PLAY_SCROLL[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
		30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 199 };
static const int PLAY_FOLLOW[] = { 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
		220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
		240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255, 256, 257, 258, 259, 199 };
/* OPENTTD_PLAY_PART=window: 60 pictures of the view standing still, with OPENTTD_PLAY_SCENE=none (no window), one (the
 * finances), many (finances, a station list, a station, the towns) or vehicle (a train's window, following it) */
static const int PLAY_WINDOW[] = { 300, 301, 302, 303, 304, 305, 306, 307, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319,
		320, 321, 322, 323, 324, 325, 326, 327, 328, 329, 330, 331, 332, 333, 334, 335, 336, 337, 338, 339,
		340, 341, 342, 343, 344, 345, 346, 347, 348, 349, 350, 351, 352, 353, 354, 355, 356, 357, 358, 359, 199 };
static const int PLAY_SHORT[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
		60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 80, 81, 82, 83, 84, 85, 86, 87, 88, 100,
		110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 199 };
extern uint32_t _vp_stat_jobs, _vp_stat_pieces, _vp_stat_again, _vp_stat_redrawn, _vp_stat_loaded; /* viewport.cpp */
extern uint64_t _vp_stat_spent, _vp_stat_most;
extern uint32_t _vp_stat_flushes;
extern uint32_t _vp_stat_tiles[4];

struct PlayPart {
	const char *name;
	int from;
	std::chrono::steady_clock::time_point start;
	uint64_t ticks;
	uint64_t instructions, tick_instructions; ///< core 0's at its start, and in game ticks
	uint32_t phase[VP_PHASES];
	uint32_t tw_tick, tw_park, tw_instret, tw_hold, tw_holds; ///< the tick's worker's (TwTimes), the first core's waits for it
	int paints;
};

static inline uint64_t Instructions()
{
	uint32_t hi, lo, again;
	do {
		asm volatile("rdinstreth %0" : "=r"(hi));
		asm volatile("rdinstret %0" : "=r"(lo));
		asm volatile("rdinstreth %0" : "=r"(again));
	} while (hi != again);
	return static_cast<uint64_t>(hi) << 32 | lo;
}
static uint64_t _tick_instructions; ///< core 0's in game ticks so far

/* The game tick in slices (docs/games/openttd.md, "The tick in slices"): ::GameLoop runs on a stack of
 * its own and gives way at TickYieldPoint() once its slice of a picture is spent; the drawing between
 * changes no game state, and a command first finishes the tick. */
struct TickRegs { uint32_t r[26]; }; ///< ra, sp, s0 to s11, fs0 to fs11
extern "C" void ottd_tick_swap(TickRegs *save, const TickRegs *load);
asm(R"(
	.text
	.globl ottd_tick_swap
	.type ottd_tick_swap, @function
ottd_tick_swap:
	sw ra, 0(a0); sw sp, 4(a0); sw s0, 8(a0); sw s1, 12(a0); sw s2, 16(a0); sw s3, 20(a0); sw s4, 24(a0)
	sw s5, 28(a0); sw s6, 32(a0); sw s7, 36(a0); sw s8, 40(a0); sw s9, 44(a0); sw s10, 48(a0); sw s11, 52(a0)
	fsw fs0, 56(a0); fsw fs1, 60(a0); fsw fs2, 64(a0); fsw fs3, 68(a0); fsw fs4, 72(a0); fsw fs5, 76(a0)
	fsw fs6, 80(a0); fsw fs7, 84(a0); fsw fs8, 88(a0); fsw fs9, 92(a0); fsw fs10, 96(a0); fsw fs11, 100(a0)
	lw ra, 0(a1); lw sp, 4(a1); lw s0, 8(a1); lw s1, 12(a1); lw s2, 16(a1); lw s3, 20(a1); lw s4, 24(a1)
	lw s5, 28(a1); lw s6, 32(a1); lw s7, 36(a1); lw s8, 40(a1); lw s9, 44(a1); lw s10, 48(a1); lw s11, 52(a1)
	flw fs0, 56(a1); flw fs1, 60(a1); flw fs2, 64(a1); flw fs3, 68(a1); flw fs4, 72(a1); flw fs5, 76(a1)
	flw fs6, 80(a1); flw fs7, 84(a1); flw fs8, 88(a1); flw fs9, 92(a1); flw fs10, 96(a1); flw fs11, 100(a1)
	ret
	.size ottd_tick_swap, .-ottd_tick_swap
)");

enum class TickState : uint8_t { Between, Running, Out }; ///< Out: given way in the middle
static TickState _tick_state = TickState::Between;
static TickRegs _tick_main, _tick_regs;
static uint8_t *_tick_stack;
static constexpr size_t TICK_STACK = 512 * 1024;
static CompanyID _tick_company, _gui_company; ///< _current_company of the tick, and of the drawing
static bool _tick_whole; ///< OPENTTD_TICK=whole: every tick at once, as upstream's
bool _tick_slicing;
uint32_t _tick_yield_at;
static uint64_t _tick_budgets, _tick_slices, _tick_finishes; ///< for the play lines: slices, their budgets, ticks finished at once
static float _tick_draw_sum; ///< the pictures but for the tick, summed at each slice

void TickYield()
{
	if (_tick_state != TickState::Running || _generating_world || RecursiveCommandCounter::Depth() != 0 || _game_mode == GM_BOOTSTRAP) return;
	_tick_company = _current_company;
	_current_company = _gui_company;
	_tick_state = TickState::Out;
	ottd_tick_swap(&_tick_regs, &_tick_main);
	_gui_company = _current_company;
	_current_company = _tick_company;
	_tick_state = TickState::Running;
}

/** OPENTTD_SUM_AT=N: at the end of tick N, null_v.cpp's sum and one of every vehicle's place, speed and cargo. */
static void TickSumAt()
{
	static const uint64_t at = std::max(Number(getenv("OPENTTD_SUM_AT")), 0);
	static const uint64_t base = TimerGameTick::counter - 1;
	if (at == 0 || TimerGameTick::counter - base != at) return;
	uint32_t h = 2166136261u, hv = 2166136261u;
	auto add = [](uint32_t &h, uint32_t v) { for (int k = 0; k < 4; k++) h = (h ^ ((v >> (8 * k)) & 0xff)) * 16777619u; };
	for (uint t = 0; t < Map::Size(); t++) {
		Tile tile(t);
		add(h, tile.type() | tile.height() << 8 | tile.m1() << 16 | tile.m3() << 24);
		add(h, tile.m2() | tile.m4() << 16 | tile.m5() << 24);
		add(h, tile.m6() | tile.m7() << 8 | tile.m8() << 16);
	}
	add(h, _random.state[0]);
	add(h, _random.state[1]);
	add(h, TimerGameCalendar::date.base());
	for (const Company *c : Company::Iterate()) {
		add(h, static_cast<uint32_t>(static_cast<int64_t>(c->money)));
		add(h, static_cast<uint32_t>(static_cast<int64_t>(c->money) >> 32));
	}
	for (const Vehicle *v : Vehicle::Iterate()) {
		add(hv, v->index.base());
		add(hv, static_cast<uint32_t>(v->x_pos));
		add(hv, static_cast<uint32_t>(v->y_pos));
		add(hv, static_cast<uint32_t>(v->z_pos) | v->cur_speed << 16);
		add(hv, v->cargo.StoredCount() | v->progress << 24);
	}
	std::string line = fmt::format("openttd: tick {}, sum {:08x}, vehicles {} sum {:08x}\n", at, h, Vehicle::GetNumItems(), hv);
	TickDefer([line] { fmt::print(stderr, "{}", line); });
}

void TickAfter() { TickSumAt(); }

void VideoDriver_ShaderEmu::TickEntry()
{
	for (;;) {
		try {
			static_cast<VideoDriver_ShaderEmu *>(VideoDriver::GetInstance())->GameLoop();
		} catch (...) {
			fmt::print(stderr, "openttd: an exception out of a game tick\n");
			std::abort();
		}
		TickSumAt();
		_tick_state = TickState::Between;
		ottd_tick_swap(&_tick_regs, &_tick_main);
	}
}

/** Runs the tick (a new one when none is out) until it is through (true) or its slice of budget is spent. */
bool VideoDriver_ShaderEmu::TickRun(uint32_t budget)
{
	if (_tick_stack == nullptr) {
		_tick_stack = static_cast<uint8_t *>(::operator new(TICK_STACK, std::align_val_t{16}));
		_tick_regs = {};
		_tick_regs.r[0] = reinterpret_cast<uint32_t>(&VideoDriver_ShaderEmu::TickEntry);
		_tick_regs.r[1] = reinterpret_cast<uint32_t>(_tick_stack + TICK_STACK - 16);
	}
	if (_tick_state == TickState::Between) _gui_company = _current_company;
	uint32_t now;
	asm volatile("rdinstret %0" : "=r"(now));
	_tick_yield_at = now + budget;
	_tick_slicing = budget != UINT32_MAX;
	_tick_state = TickState::Running;
	ottd_tick_swap(&_tick_main, &_tick_regs);
	_tick_slicing = false;
	return _tick_state == TickState::Between;
}

void TickFinish()
{
	VpFlushFinish(); /* (a command or a new game: nothing drawn is out) */
	TwAfterFlush();
	if (TwActive()) {
		if (!TickOnWorker()) TwHold(true);
		return;
	}
	if (_tick_state != TickState::Out) return;
	uint64_t from = Instructions();
	VideoDriver_ShaderEmu::TickRun(UINT32_MAX);
	_tick_instructions += Instructions() - from;
	_tick_finishes++;
}

/** A play line written whole (the console's descriptor may be one that does not wait). */
static void PlayLine(const std::string &line)
{
	const char *p = line.data();
	size_t left = line.size();
	while (left > 0) {
		ssize_t w = write(2, p, left);
		if (w > 0) p += w, left -= w;
		else if (w < 0 && (errno == EAGAIN || errno == EINTR)) usleep(2000);
		else break;
	}
}

static void PlayScroll(int dx, int dy)
{
	Window *w = GetMainWindow();
	if (w == nullptr || w->viewport == nullptr) return;
	w->viewport->follow_vehicle = VehicleID::Invalid();
	w->viewport->dest_scrollpos_x += ScaleByZoom(dx, w->viewport->zoom);
	w->viewport->dest_scrollpos_y += ScaleByZoom(dy, w->viewport->zoom);
}

static void PlayStep()
{
	static const char *play = getenv("OPENTTD_PLAY");
	static int rounds = play != nullptr ? std::max(1, Number(play)) : 0;
	static PlayPart cur;
	static bool in_part = false;
	static int waited = 0;
	static std::chrono::steady_clock::time_point began;
	static uint64_t began_ticks;
	if (rounds == 0 || _game_mode != GM_NORMAL) return;
	if (_play_frame < 0) {
		if (++waited < 5) return; /* the first pictures of a loaded game draw everything */
		_play_frame = 0;
		/* OPENTTD_PAUSE=1: the game paused, so that two builds draw the same views */
		if (getenv("OPENTTD_PAUSE") != nullptr) {
			_pause_mode.Set(PauseMode::Normal);
			MarkWholeScreenDirty(); /* (the status bar says so) */
		}
		/* no autosave: one is minutes of LZMA at 2 MIPS, in whichever part it falls */
		_settings_client.gui.autosave_interval = 0;
		ChangeAutosaveFrequency(true);
		/* OPENTTD_PLAY_JUMP=1: a scroll goes the whole way at once, so that a picture's view is its number's
		 * whatever the speed (two ways of drawing compared by their sums) */
		if (getenv("OPENTTD_PLAY_JUMP") != nullptr) _settings_client.gui.smooth_scroll = false;
		/* OPENTTD_PLAY_FRAMERATE=1: the frame rate window open through the play, to weigh what it costs */
		if (getenv("OPENTTD_PLAY_FRAMERATE") != nullptr) ShowFramerateWindow();
	}
	static const bool short_play = getenv("OPENTTD_PLAY_SHORT") != nullptr;
	static const char *play_part = getenv("OPENTTD_PLAY_PART");
	static const int *list = short_play ? PLAY_SHORT : play_part == nullptr ? nullptr :
			std::string_view(play_part) == "follow" ? PLAY_FOLLOW : std::string_view(play_part) == "window" ? PLAY_WINDOW : PLAY_SCROLL;
	static const int len = list == PLAY_SHORT ? std::size(PLAY_SHORT) : list == PLAY_SCROLL ? std::size(PLAY_SCROLL) :
			list == PLAY_FOLLOW ? std::size(PLAY_FOLLOW) : list == PLAY_WINDOW ? std::size(PLAY_WINDOW) : 200;
	/* OPENTTD_PICTURE_LOG=1: each picture's ms, the first core's instructions, ticks, and its phases' ms */
	static const bool picture_log = getenv("OPENTTD_PICTURE_LOG") != nullptr;
	if (picture_log) {
		static uint32_t last_t, last_phase[VP_PHASES];
		static uint64_t last_i, last_ticks;
		const uint32_t now = VpTime();
		const uint64_t i = Instructions();
		std::string line = fmt::format("openttd: picture {} {:.1f} ms, {}k, {} ticks:", _play_frame, (now - last_t) / 5.0,
				(i - last_i) / 1000, TimerGameTick::counter - last_ticks);
		for (int k = 0; k < VP_PHASES; k++) line += fmt::format(" {:.1f}", (_vp_phase[k] - last_phase[k]) / 5.0), last_phase[k] = _vp_phase[k];
		extern uint32_t _odd_stat[3];
		static uint32_t last_odd[3];
		line += fmt::format("; odd moves {}, {:.1f} ms to their wait, {:.1f} waited", _odd_stat[0] - last_odd[0],
				(_odd_stat[1] - last_odd[1]) / 5.0, (_odd_stat[2] - last_odd[2]) / 5.0);
		std::copy_n(_odd_stat, 3, last_odd);
		extern uint32_t _veh_synced, _veh_ahead;
		static uint32_t last_synced, last_ahead;
		line += fmt::format("; vehicles synced {}, as the tick made them {}", _veh_synced - last_synced, _veh_ahead - last_ahead);
		last_synced = _veh_synced, last_ahead = _veh_ahead;
		PlayLine(line + "\n");
		last_t = now, last_i = i, last_ticks = TimerGameTick::counter;
	}
	const int at = _play_frame++ % len;
	const int f = list != nullptr ? list[at] : at;
	/* OPENTTD_PLAY_TIMES=1: each picture's time, to see where a part's time goes */
	static const bool times = getenv("OPENTTD_PLAY_TIMES") != nullptr;
	if (times) {
		static auto last = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		/* (=sums: and the last picture's sum, to compare two builds' pictures) */
		static const bool sums = std::string_view(getenv("OPENTTD_PLAY_TIMES")) == "sums";
		uint32_t sum = 2166136261u;
		if (sums && SeGpuOn()) {
			/* (the GPU's picture as drawn and copied out, its remaps made) */
			static std::vector<uint8_t> drawn;
			SeGpuSettle();
			SeGpuPicture(drawn);
			for (uint8_t b : drawn) sum = (sum ^ b) * 16777619u;
		} else if (sums && _picture != nullptr) {
			for (int i = 0; i < _screen.width * _screen.height; i++) sum = (sum ^ _picture[i]) * 16777619u;
		}
		fmt::print(stderr, "openttd: picture {} took {:.1f} ms, sum {:08x}\n", f, std::chrono::duration<double, std::milli>(now - last).count(), sum);
		last = now;
	}
	if (at == 0) {
		/* the rounds before the last warm the sprite cache and the ships' water regions */
		if (_play_frame / len + 1 == rounds) PlayLine(fmt::format("openttd: play PL-GO\n"));
		began = std::chrono::steady_clock::now();
		began_ticks = TimerGameTick::counter;
		_checked = _check_bad = _check_line = 0, _check_pixels = 0;
	}
	auto begin_part = [&](const char *name) {
		auto now = std::chrono::steady_clock::now();
		if (in_part) {
			const PlayPart &p = cur;
			double s = std::chrono::duration<double>(now - p.start).count();
			const int pictures = _play_frame - 1 - p.from;
			auto ms = [&](int k) { return (_vp_phase[k] - p.phase[k]) / 5.0 / std::max(pictures, 1); };
			PlayLine(fmt::format("openttd: play {}: {} pictures in {:.2f} s, {:.2f} a second, {} ticks; viewport {} parts in {} pieces, "
					"{} made again by workers ({} sprites loaded), {} by the first core; longest line differing {}; "
					"core 0 {}k instructions a picture, {}k in game ticks\n",
					p.name, pictures, s, pictures / s, TimerGameTick::counter - p.ticks,
					_vp_stat_jobs, _vp_stat_pieces, _vp_stat_again, _vp_stat_loaded, _vp_stat_redrawn, _check_part_line,
					(Instructions() - p.instructions) / 1000 / std::max(pictures, 1),
					(_tick_instructions - p.tick_instructions) / 1000 / std::max(pictures, 1)));
			{
				static SeGpuCounts last;
				const SeGpuCounts counts = _se_gpu_counts;
				const int n = std::max(pictures, 1);
				PlayLine(fmt::format("openttd: play {} a picture: {:.2f} painted, {:.2f} GPU lists, {:.0f} records; {:.1f} rects of {:.0f} pixels, {:.1f} window paints\n", p.name,
						static_cast<double>(_paints - p.paints) / n, static_cast<double>(counts.lists - last.lists) / n,
						static_cast<double>(counts.records - last.records) / n,
						static_cast<double>(counts.rects - last.rects) / n, static_cast<double>(counts.pixels - last.pixels) / n,
						static_cast<double>(counts.paints - last.paints) / n));
				last = counts;
				if (getenv("OPENTTD_PLAY_WHO") != nullptr) {
					std::string who = fmt::format("openttd: play {} paints by window class:", p.name);
					for (int c = 0; c < 256; c++) {
						if (_se_paints_of[c] != 0) who += fmt::format(" {}:{}", c, _se_paints_of[c]);
						_se_paints_of[c] = 0;
					}
					PlayLine(who + "\n");
				}
			}
			/* (a line of its own: the console loses the end of a long one) */
			PlayLine(fmt::format("openttd: play {} ms a picture: flush {:.1f} (pieces {:.1f}, again {:.1f}, "
					"first core {:.1f}, copy {:.1f}, texts {:.1f}), move {:.1f}, windows {:.1f}, paint {:.1f}, tick {:.1f}; "
					"{:.2f} flushes a picture, a piece {}k, the dearest of a flush {}k; tiles a piece: drawn {} ({} known), "
					"passed {}, not learned {}\n", p.name,
					ms(VP_PHASE_FLUSH), ms(VP_PHASE_PIECES), ms(VP_PHASE_AGAIN), ms(VP_PHASE_FIRST), ms(VP_PHASE_COPY), ms(VP_PHASE_TEXTS),
					ms(VP_PHASE_MOVE), ms(VP_PHASE_WINDOWS), ms(VP_PHASE_PAINT), ms(VP_PHASE_TICK),
					static_cast<double>(_vp_stat_flushes) / std::max(pictures, 1), _vp_stat_spent / 1000 / std::max<uint64_t>(_vp_stat_pieces, 1),
					_vp_stat_most / 1000 / std::max<uint32_t>(_vp_stat_flushes, 1),
					_vp_stat_tiles[0] / std::max<uint32_t>(_vp_stat_pieces, 1), _vp_stat_tiles[1] / std::max<uint32_t>(_vp_stat_pieces, 1),
					_vp_stat_tiles[2] / std::max<uint32_t>(_vp_stat_pieces, 1), _vp_stat_tiles[3] / std::max<uint32_t>(_vp_stat_pieces, 1)));
		}
		if (in_part && TwActive()) {
			const PlayPart &p = cur;
			uint32_t tick, park, instret;
			TwTimes(tick, park, instret);
			const double s = std::chrono::duration<double>(now - p.start).count();
			const uint64_t ticks = TimerGameTick::counter - p.ticks;
			PlayLine(fmt::format("openttd: play {} tick on a worker: {:.2f} ticks a second, {:.0f} ms of the worker's a tick, waiting {:.0f}% "
					"of the time, {:.2f} MIPS in ticks; the first core waited {:.1f} ms a picture for it ({:.1f} holds, the longest {:.1f} ms)\n",
					p.name, ticks / s, (tick - p.tw_tick) / 5.0 / std::max<uint64_t>(ticks, 1), (park - p.tw_park) / 5.0 / s / 10.0,
					(instret - p.tw_instret) / std::max((tick - p.tw_tick) / 5.0, 1.0) / 1000.0,
					(_tw_hold_time - p.tw_hold) / 5.0 / std::max(_play_frame - 1 - p.from, 1),
					static_cast<double>(_tw_holds - p.tw_holds) / std::max(_play_frame - 1 - p.from, 1), _tw_hold_most / 5.0));
		} else if (in_part) {
			const PlayPart &p = cur;
			PlayLine(fmt::format("openttd: play {} tick: {} slices, {}k instructions given each, the picture but the tick {:.1f} ms; finished at once {} times\n",
					p.name, _tick_slices, _tick_budgets / 1000 / std::max<uint64_t>(_tick_slices, 1),
					_tick_draw_sum / std::max<uint64_t>(_tick_slices, 1), _tick_finishes));
		}
		_tick_draw_sum = 0;
		_tw_hold_most = 0;
		_tick_budgets = _tick_slices = _tick_finishes = 0;
		_check_part_line = 0;
		_vp_stat_jobs = _vp_stat_pieces = _vp_stat_again = _vp_stat_redrawn = _vp_stat_loaded = 0;
		_vp_stat_spent = _vp_stat_most = 0;
		_vp_stat_flushes = 0;
		std::fill_n(_vp_stat_tiles, 4, 0);
		in_part = name != nullptr;
		if (in_part) {
			cur = { name, _play_frame - 1, now, TimerGameTick::counter, Instructions(), _tick_instructions, {}, 0, 0, 0, _tw_hold_time, _tw_holds, _paints };
			std::copy_n(_vp_phase, VP_PHASES, cur.phase);
			TwTimes(cur.tw_tick, cur.tw_park, cur.tw_instret);
		}
	};
	Window *main = GetMainWindow();
	static std::vector<VehicleID> trains;
	if (f == 60 || f == 80 || f == 100 || f == 110 || f == 170 || f == 200) VpFlushFinish(); /* (zooms, windows: nothing out) */
	if (f == 0) begin_part("scroll"), PlayLine(fmt::format("openttd: play highlight {}\n", static_cast<int>(_thd.drawstyle)));
	/* OPENTTD_PLAY_ODD=1: the way back a pixel longer a step, a move the machine's copy does not make */
	static const int odd = getenv("OPENTTD_PLAY_ODD") != nullptr;
	if (f < 60) PlayScroll(f < 30 ? 24 : -24 - odd, f < 30 ? 12 : -12);
	if (f == 60) begin_part("zoomed out"), DoZoomInOutWindow(ZOOM_OUT, main);
	if (f == 80) DoZoomInOutWindow(ZOOM_OUT, main);
	if (f >= 60 && f < 100) PlayScroll(f < 80 ? 16 : -16, f < 80 ? -8 : 8);
	if (f == 100) DoZoomInOutWindow(ZOOM_IN, main), DoZoomInOutWindow(ZOOM_IN, main);
	if (f == 110) {
		begin_part("windows");
		trains.clear();
		for (const Train *t : Train::Iterate()) {
			if (t->IsFrontEngine() && trains.size() < 3) trains.push_back(t->index);
		}
		for (VehicleID id : trains) ShowVehicleViewWindow(Vehicle::Get(id));
		for (const Station *st : Station::Iterate()) { ShowStationViewWindow(st->index); break; }
		if (!trains.empty() && main != nullptr && main->viewport != nullptr) main->viewport->follow_vehicle = trains[0];
	}
	if (f == 170) {
		begin_part("windows closed");
		CloseAllNonVitalWindows();
		if (main != nullptr && main->viewport != nullptr) main->viewport->follow_vehicle = VehicleID::Invalid();
	}
	if (f >= 170 && f < 200) PlayScroll(f < 185 ? -24 : 24, 0);
	if (f == 200) {
		begin_part("follow");
		for (const Train *t : Train::Iterate()) {
			if (t->IsFrontEngine() && t->cur_speed > 0 && main != nullptr && main->viewport != nullptr) {
				main->viewport->follow_vehicle = t->index;
				break;
			}
		}
	}
	if (f == 300) {
		VpFlushFinish();
		static const char *scene = getenv("OPENTTD_PLAY_SCENE");
		const std::string_view s = scene != nullptr ? scene : "none";
		if (s == "one" || s == "many") {
			for (const Company *c : Company::Iterate()) {
				ShowCompanyFinances(c->index);
				if (s == "many") {
					ShowCompanyStations(c->index);
					for (const Station *st : Station::Iterate()) { ShowStationViewWindow(st->index); break; }
					ShowTownDirectory();
				}
				break;
			}
		}
		if (s == "vehicle") {
			const Train *any = nullptr;
			for (const Train *t : Train::Iterate()) {
				if (!t->IsFrontEngine()) continue;
				if (any == nullptr || (any->cur_speed == 0 && t->cur_speed > 0)) any = t;
			}
			if (any != nullptr) ShowVehicleViewWindow(any);
		}
	}
	if (f == 305) begin_part("window");
	/* OPENTTD_PLAY_MOVE=1: at 320 the windows move as a drag moves them, at 340 they close (with OPENTTD_CHECK: each picture so) */
	static const bool play_move = getenv("OPENTTD_PLAY_MOVE") != nullptr;
	if (play_move && f == 320) {
		VpFlushFinish();
		for (Window *w : Window::Iterate()) {
			if (w->window_class == WC_MAIN_WINDOW || w->window_class == WC_MAIN_TOOLBAR || w->window_class == WC_STATUS_BAR) continue;
			w->SetDirty();
			if (w->viewport != nullptr) w->viewport->left += 40, w->viewport->top += 24;
			w->left += 40, w->top += 24;
			w->SetDirty();
		}
	}
	if (play_move && f == 340) {
		VpFlushFinish();
		CloseAllNonVitalWindows();
	}
	if (f == 199) {
		begin_part(nullptr);
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
		PlayLine(fmt::format("openttd: play round {}: {} pictures in {:.2f} s, {:.2f} a second, {} ticks; checked {}, {} differ ({} pixels, longest line {})\n",
				_play_frame / len, len, s, len / s, TimerGameTick::counter - began_ticks, _checked, _check_bad, _check_pixels, _check_line));
		if (short_play) CloseAllNonVitalWindows(); /* (the next round opens them again) */
		if (_play_frame / len < rounds) return;
		PlayLine(fmt::format("openttd: play PL-DONE\n"));
		_exit_game = true;
	}
}

void VideoDriver_ShaderEmu::InputLoop()
{
	bool old_ctrl_pressed = _ctrl_pressed;

	_ctrl_pressed = (_modifiers & MWKMOD_CTRL) != 0;
	_shift_pressed = (_modifiers & MWKMOD_SHIFT) != 0;
	/* Tab held runs the game fast, as on other systems */
	this->fast_forward_key_pressed = (_keys_down & 16) && !(_modifiers & MWKMOD_ALT);
	_dirkeys = _keys_down & 15;
	/* OPENTTD_SCROLL=1, for measuring: the arrows held down and right, then up and left, two seconds each */
	static const bool scroll_test = getenv("OPENTTD_SCROLL") != nullptr;
	if (scroll_test && _game_mode == GM_NORMAL) {
		auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		_dirkeys |= (s / 2) % 2 ? 4 | 8 : 1 | 2;
	}
	/* OPENTTD_HOSTING=FILE: "openttd" written there once this server's game runs (nxgame, docs/system/lan.md) */
	static const char *hosting = getenv("OPENTTD_HOSTING");
	if (hosting != nullptr && _network_server && _game_mode == GM_NORMAL) {
		if (FILE *f = fopen(hosting, "w"); f != nullptr) fmt::print(f, "openttd\n"), fclose(f);
		hosting = nullptr;
	}

	PlayStep();
	if (old_ctrl_pressed != _ctrl_pressed) HandleCtrlChanged();
}

/** Whether the game tick runs on a worker of its own (OPENTTD_TICK=whole|slices: on the first core). */
static bool WantWorkerTick()
{
	static const char *how = getenv("OPENTTD_TICK");
	static const bool first_core = how != nullptr && (std::string_view(how) == "whole" || std::string_view(how) == "slices");
	return !first_core && _game_mode == GM_NORMAL && !_networking && _switch_mode == SM_NONE && !HasModalProgress();
}

/** A picture with the tick on its worker: it waits while the first core reads and changes the game, and
 * goes on while the viewports are drawn (docs/games/openttd.md, "The tick on a worker"). */
void VideoDriver_ShaderEmu::WorkerTick()
{
	static const float frame_ms = [] { int n = Number(getenv("OPENTTD_FRAME_MS")); return n > 0 ? n : 66.0f; }();
	auto now = std::chrono::steady_clock::now();
	if (now < this->next_draw_tick) return;
	const auto interval = std::chrono::microseconds(static_cast<int>(frame_ms * 1000));
	this->next_draw_tick += interval;
	if (this->next_draw_tick < now - ALLOWED_DRIFT * interval) this->next_draw_tick = now;

	uint32_t t = VpTime();
	const uint32_t work_from = t;
	extern uint32_t _vp_scrolls;
	const uint32_t scrolls = _vp_scrolls;
	TwHold(false);
	TwSync();
	::GameLoop(); /* (the first core's parts: the tick itself is the worker's) */
	_vp_phase[VP_PHASE_TICK] += VpTime() - t;
	if (!WantWorkerTick()) TwStop();
	this->LockVideoBuffer();
	InteractiveRandom();
	this->DrainCommandQueue();
	while (this->PollEvent()) {}
	this->InputLoop();
	if (this->fast_forward_key_pressed && !_networking && _game_mode != GM_MENU) {
		ChangeGameSpeed(true);
		this->fast_forward_via_key = true;
	} else if (this->fast_forward_via_key) {
		ChangeGameSpeed(false);
		this->fast_forward_via_key = false;
	}
	::InputLoop();
	VehiclesSync(TwActive());
	/* the last picture's flush, left out while all this ran, made whole, and that picture shown */
	static bool paint_owed = false;
	t = VpTime();
	if (_game_mode == GM_BOOTSTRAP || _switch_mode == SM_NONE || HasModalProgress()) UpdateWindowsEarly();
	if (!VpFlushReady()) { /* (the tick runs while its pieces are drawn, and waits again for the drawing) */
		TwRelease();
		VpFlushFinish();
		TwHold(false);
		VehiclesSync(TwActive());
	}
	VpFlushFinish();
	TwAfterFlush();
	_vp_phase[VP_PHASE_WINDOWS] += VpTime() - t;
	if (std::exchange(paint_owed, false)) {
		NetworkDrawChatMessage();
		DrawMouseCursor();
		t = VpTime();
		this->Paint();
		_vp_phase[VP_PHASE_PAINT] += VpTime() - t;
	}
	t = VpTime();
	if (_game_mode == GM_BOOTSTRAP || _switch_mode == SM_NONE || HasModalProgress()) ::UpdateWindows();
	_vp_phase[VP_PHASE_WINDOWS] += VpTime() - t;
	TwRelease(); /* (the viewports' last flush let it go, if there was one) */
	this->PopulateSystemSprites();
	this->CheckPaletteAnim();
	if (VpFlushOut()) {
		paint_owed = true;
	} else {
		t = VpTime();
		this->Paint();
		_vp_phase[VP_PHASE_PAINT] += VpTime() - t;
	}
	this->UnlockVideoBuffer();
	TwService();
	TwGrant();
	/* (OPENTTD_PACE_MS: the picture's work the pace keeps to while it scrolls, 90 ms: ten pictures a second) */
	static const uint32_t pace_ms = [] { int n = Number(getenv("OPENTTD_PACE_MS")); return n > 0 ? n : 90; }();
	TwPace(VpTime() - work_from, pace_ms * 5, _vp_scrolls != scrolls);
}

/** Upstream's Tick, with core 0's instructions in game ticks counted. */
void VideoDriver_ShaderEmu::DriverTick()
{
	static bool operational = false;
	if (!operational) { /* (upstream's marks the driver working at its first picture) */
		auto was = this->next_draw_tick;
		this->Tick();
		operational = this->next_draw_tick != was;
		return;
	}
	static bool no_worker = false;
	if (WantWorkerTick() && !no_worker) {
		if (!TwActive()) {
			TickFinish(); /* (a tick in slices is through first) */
			no_worker = !TwStart();
			if (no_worker) fmt::print(stderr, "openttd: no worker for the game tick: it runs in slices\n");
		}
		if (TwActive()) {
			this->WorkerTick();
			return;
		}
	} else if (TwActive()) {
		TwStop();
	}
	/* (float, not double: the machine has single precision only, a double's operation is 200 instructions) */
	static float frame_ms = [] { int n = Number(getenv("OPENTTD_FRAME_MS")); return n > 0 ? n : 100.0f; }();
	static float least_ms = [] { int n = Number(getenv("OPENTTD_TICK_LEAST")); return n > 0 ? n : 10.0f; }(); ///< the least slice, its overrun in it
	static float draw_ms = 0, tick_ms = 0, rate = 2000; ///< the last picture's but for the tick, the tick's since, core 0's instructions a ms in it
	static float over_ms = 0; ///< how far a slice runs past its budget, on average (it gives way only at its yield points)
	static uint32_t last_from = 0;
	auto now = std::chrono::steady_clock::now();
	if (_tick_whole) {
		if (now >= this->next_game_tick) {
			uint64_t from = Instructions();
			this->GameLoop();
			_tick_instructions += Instructions() - from;
			TickSumAt();
		}
	} else if (_tick_state == TickState::Out || now >= this->next_game_tick) {
		const uint32_t t = VpTime();
		/* the tick's slice: what the picture leaves of frame_ms, at least OPENTTD_TICK_LEAST ms */
		const uint32_t budget = static_cast<uint32_t>(rate * std::max({frame_ms - draw_ms - over_ms, least_ms - over_ms, 2.0f}));
		const uint64_t from = Instructions();
		bool started = false; /* (one new tick a picture, as upstream's Tick) */
		for (;;) {
			const uint64_t spent = Instructions() - from;
			if (spent >= budget) break;
			if (_tick_state != TickState::Out && (started || std::chrono::steady_clock::now() < this->next_game_tick)) break;
			started |= _tick_state == TickState::Between;
			if (!TickRun(static_cast<uint32_t>(budget - spent))) break;
		}
		const uint32_t spent = static_cast<uint32_t>(Instructions() - from);
		const float ms = (VpTime() - t) / 5.0f; /* (the machine's 5 kHz time: the clock's is coarser) */
		if (ms >= 2 && spent > 0) rate = std::clamp(0.75f * rate + 0.25f * (spent / ms), 200.0f, 50000.0f);
		over_ms = 0.75f * over_ms + 0.25f * (spent > budget ? (spent - budget) / rate : 0.0f);
		_tick_instructions += spent;
		tick_ms += ms;
		_tick_budgets += budget, _tick_slices++, _tick_draw_sum += draw_ms;
		_vp_phase[VP_PHASE_TICK] += VpTime() - t;
	}

	now = std::chrono::steady_clock::now();
	if (now < this->next_draw_tick) return;
	this->next_draw_tick += this->GetDrawInterval();
	if (this->next_draw_tick < now - ALLOWED_DRIFT * this->GetDrawInterval()) this->next_draw_tick = now;

	const uint32_t draw_from = VpTime();
	/* (an average over pictures: a cheap one after dear ones leaves the tick less) */
	if (last_from != 0) draw_ms = 0.75f * draw_ms + 0.25f * std::max((draw_from - last_from) / 5.0f - tick_ms, 0.0f);
	last_from = draw_from, tick_ms = 0;
	this->LockVideoBuffer();
	InteractiveRandom();
	this->DrainCommandQueue();
	while (this->PollEvent()) {}
	this->InputLoop();
	if (this->fast_forward_key_pressed && !_networking && _game_mode != GM_MENU) {
		ChangeGameSpeed(true);
		this->fast_forward_via_key = true;
	} else if (this->fast_forward_via_key) {
		ChangeGameSpeed(false);
		this->fast_forward_via_key = false;
	}
	::InputLoop();
	VehiclesSync(false); /* (what the viewports draw of the vehicles: their copies) */
	uint32_t t = VpTime();
	if (_game_mode == GM_BOOTSTRAP || _switch_mode == SM_NONE || HasModalProgress()) ::UpdateWindows();
	_vp_phase[VP_PHASE_WINDOWS] += VpTime() - t;
	this->PopulateSystemSprites();
	this->CheckPaletteAnim();
	t = VpTime();
	this->Paint();
	_vp_phase[VP_PHASE_PAINT] += VpTime() - t;
	this->UnlockVideoBuffer();
}

void VideoDriver_ShaderEmu::MainLoop()
{
	/* OPENTTD_RATE=1: pictures drawn and game ticks run, every ten seconds */
	bool rate = getenv("OPENTTD_RATE") != nullptr;
	const char *how = getenv("OPENTTD_TICK");
	_tick_whole = how != nullptr && std::string_view(how) == "whole";
	auto rate_from = std::chrono::steady_clock::now();
	uint64_t ticks_from = TimerGameTick::counter;
	while (!_exit_game) {
		this->DriverTick();
		if (rate && std::chrono::steady_clock::now() - rate_from >= std::chrono::seconds(10)) {
			fmt::print(stderr, "openttd: {} draws, {} ticks in 10 s\n", _draws, TimerGameTick::counter - std::min(ticks_from, TimerGameTick::counter));
			rate_from += std::chrono::seconds(10);
			ticks_from = TimerGameTick::counter;
			_draws = 0;
		}

		/* until the next tick or draw, or an event */
		auto next = TwActive() ? this->next_draw_tick : std::min(this->next_draw_tick, this->next_game_tick);
		auto now = std::chrono::steady_clock::now();
		if (next > now && !_have_pending && _tick_state != TickState::Out) {
			long ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
			if (TwActive()) ms = std::min(ms, 5L), TwService(), TwGrant();
			if (ms > 0) {
				GrGetNextEventTimeout(&_pending, ms);
				_have_pending = _pending.type != GR_EVENT_TYPE_NONE && _pending.type != GR_EVENT_TYPE_TIMEOUT;
			}
		}
	}
	TwStop();
	TickFinish();
}

/**
 * The program has said it goes (PostMainLoop): whatever it does from here on, it goes. A process that stays holds the
 * GPU's drawing, and the next program that draws is told to close a program with no window (docs/games/openttd.md, "At exit").
 * SIGALRM is one no handler here takes.
 */
void SeExitWatch()
{
	signal(SIGALRM, SIG_DFL);
	alarm(60);
}

/** The drivers are stopped: the kernel takes everything back at once, workers first (shaderemu_gpu_exiting). */
void SeExitNow()
{
	fflush(nullptr);
	_exit(0);
}

bool VideoDriver_ShaderEmu::ChangeResolution(int w, int h)
{
	GrResizeWindow(_window, w, h);
	return this->Resize(w, h);
}

bool VideoDriver_ShaderEmu::ToggleFullscreen(bool)
{
	return false;
}

bool VideoDriver_ShaderEmu::AfterBlitterChange()
{
	return BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 8 && this->Resize(_screen.width, _screen.height);
}

Dimension VideoDriver_ShaderEmu::GetScreenSize() const
{
	return { static_cast<uint>(_screen_cols), static_cast<uint>(_screen_rows) };
}
