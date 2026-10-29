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
#include "../framerate_type.h"
#include "../progress.h"
#include "../window_func.h"
#include "../timer/timer_game_tick.h"
#include "../network/network.h"
#include "../window_gui.h"
#include "../viewport_func.h"
#include "../vehicle_gui.h"
#include "../station_func.h"
#include "../station_base.h"
#include "../train.h"
#include "../zoom_func.h"
#include "../settings_type.h"
#include "shaderemu_v.h"
#include <unistd.h>
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
static bool _drawn;                ///< a list is with the GPU: settle before writing the picture
static int _pointer_x, _pointer_y; ///< where the pointer was last, for moves while OpenTTD holds it
static unsigned _keys_down;        ///< bit 0-3 the arrows (as _dirkeys), 4 Tab
static GR_KEYMOD _modifiers;

#define EVENTS (GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_EXPOSURE | \
		GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)

std::optional<std::string_view> VideoDriver_ShaderEmu::Start(const StringList &)
{
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
	_picture = static_cast<uint8_t *>(seglMemory(_picture_w * _picture_h));
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
	if (_drawn) seglSettle();
	GrClose();
}

/** The picture is w x h pixels from now on: its rows are as long as it is wide. */
bool VideoDriver_ShaderEmu::Resize(int w, int h)
{
	w = Clamp(w, 64, _picture_w);
	h = Clamp(h, 64, _picture_h);
	if (_drawn) seglSettle(), _drawn = false;

	glBindTexture(GL_TEXTURE_2D, _texture);
	seglTexturePointer(_picture, w, h, GL_COLOR_INDEX8_EXT);
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
}

void VideoDriver_ShaderEmu::CheckPaletteAnim()
{
	if (!CopyPalette(this->local_palette)) return;
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

bool VideoDriver_ShaderEmu::LockVideoBuffer()
{
	if (_drawn) seglSettle(), _drawn = false;
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

static void HoldHere(const uint8_t *shown, int w, int h)
{
	fmt::print(stderr, "openttd: hold at picture {}: picture {:x} shown {:x} {}x{} palette {:x}\n", _play_frame,
			reinterpret_cast<uintptr_t>(_picture), reinterpret_cast<uintptr_t>(shown), w, h, reinterpret_cast<uintptr_t>(seglPalette()));
	for (;;) usleep(1000000);
}

static void CheckPicture()
{
	static const char *check_how = getenv("OPENTTD_CHECK");
	static const bool check = check_how != nullptr;
	static const bool keep = check && std::string_view(check_how) == "keep"; /* the picture as drawn is shown on, its errors kept */
	static const char *hold = getenv("OPENTTD_HOLD");
	int w = _screen.width, h = _screen.height;
	if (!check || _game_mode != GM_NORMAL) {
		if (hold != nullptr && _play_frame == Number(hold)) HoldHere(_picture, w, h);
		return;
	}
	UndrawMouseCursor();
	static std::vector<uint8_t> shown;
	shown.assign(_picture, _picture + w * h);
	MarkWholeScreenDirty();
	DrawDirtyBlocks();
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
	if (keep) std::copy(shown.begin(), shown.end(), _picture);
	DrawMouseCursor();
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
	CheckPicture();

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
static const int PLAY_SHORT[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
		60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 80, 81, 82, 83, 84, 85, 86, 87, 88, 100,
		110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 199 };
extern uint32_t _vp_stat_jobs, _vp_stat_pieces, _vp_stat_again, _vp_stat_redrawn, _vp_stat_loaded; /* viewport.cpp */

struct PlayPart {
	const char *name;
	int from;
	std::chrono::steady_clock::time_point start;
	uint64_t ticks;
};

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
	}
	static const bool short_play = getenv("OPENTTD_PLAY_SHORT") != nullptr;
	const int len = short_play ? std::size(PLAY_SHORT) : 200;
	int f = _play_frame++ % len;
	if (short_play) f = PLAY_SHORT[f];
	/* OPENTTD_PLAY_TIMES=1: each picture's time, to see where a part's time goes */
	static const bool times = getenv("OPENTTD_PLAY_TIMES") != nullptr;
	if (times) {
		static auto last = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();
		/* (=sums: and the last picture's sum, to compare two builds' pictures) */
		static const bool sums = std::string_view(getenv("OPENTTD_PLAY_TIMES")) == "sums";
		uint32_t sum = 2166136261u;
		if (sums && _picture != nullptr) {
			for (int i = 0; i < _screen.width * _screen.height; i++) sum = (sum ^ _picture[i]) * 16777619u;
		}
		fmt::print(stderr, "openttd: picture {} took {:.1f} ms, sum {:08x}\n", f, std::chrono::duration<double, std::milli>(now - last).count(), sum);
		last = now;
	}
	if (f == 0) {
		/* the rounds before the last warm the sprite cache and the ships' water regions */
		if (_play_frame / len + 1 == rounds) fmt::print(stderr, "openttd: play PL-GO\n");
		began = std::chrono::steady_clock::now();
		began_ticks = TimerGameTick::counter;
		_checked = _check_bad = _check_line = 0, _check_pixels = 0;
	}
	auto begin_part = [&](const char *name) {
		auto now = std::chrono::steady_clock::now();
		if (in_part) {
			const PlayPart &p = cur;
			double s = std::chrono::duration<double>(now - p.start).count();
			fmt::print(stderr, "openttd: play {}: {} pictures in {:.2f} s, {:.2f} a second, {} ticks; viewport {} parts in {} pieces, "
					"{} made again by workers ({} sprites loaded), {} by the first core; longest line differing {}\n",
					p.name, _play_frame - 1 - p.from, s, (_play_frame - 1 - p.from) / s, TimerGameTick::counter - p.ticks,
					_vp_stat_jobs, _vp_stat_pieces, _vp_stat_again, _vp_stat_loaded, _vp_stat_redrawn, _check_part_line);
		}
		_check_part_line = 0;
		_vp_stat_jobs = _vp_stat_pieces = _vp_stat_again = _vp_stat_redrawn = _vp_stat_loaded = 0;
		in_part = name != nullptr;
		if (in_part) cur = { name, _play_frame - 1, now, TimerGameTick::counter };
	};
	Window *main = GetMainWindow();
	static std::vector<VehicleID> trains;
	if (f == 0) begin_part("scroll");
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
	if (f >= 170) PlayScroll(f < 185 ? -24 : 24, 0);
	if (f == 199) {
		begin_part(nullptr);
		double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
		fmt::print(stderr, "openttd: play round {}: {} pictures in {:.2f} s, {:.2f} a second, {} ticks; checked {}, {} differ ({} pixels, longest line {})\n",
				_play_frame / len, len, s, len / s, TimerGameTick::counter - began_ticks, _checked, _check_bad, _check_pixels, _check_line);
		if (short_play) CloseAllNonVitalWindows(); /* (the next round opens them again) */
		if (_play_frame / len < rounds) return;
		fmt::print(stderr, "openttd: play PL-DONE\n");
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

void VideoDriver_ShaderEmu::MainLoop()
{
	/* OPENTTD_RATE=1: pictures drawn and game ticks run, every ten seconds */
	bool rate = getenv("OPENTTD_RATE") != nullptr;
	auto rate_from = std::chrono::steady_clock::now();
	uint64_t ticks_from = TimerGameTick::counter;
	while (!_exit_game) {
		this->Tick();
		if (rate && std::chrono::steady_clock::now() - rate_from >= std::chrono::seconds(10)) {
			fmt::print(stderr, "openttd: {} draws, {} ticks in 10 s\n", _draws, TimerGameTick::counter - std::min(ticks_from, TimerGameTick::counter));
			rate_from += std::chrono::seconds(10);
			ticks_from = TimerGameTick::counter;
			_draws = 0;
		}

		/* until the next tick or draw, or an event */
		auto next = std::min(this->next_draw_tick, this->next_game_tick);
		auto now = std::chrono::steady_clock::now();
		if (next > now && !_have_pending) {
			long ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
			if (ms > 0) {
				GrGetNextEventTimeout(&_pending, ms);
				_have_pending = _pending.type != GR_EVENT_TYPE_NONE && _pending.type != GR_EVENT_TYPE_TIMEOUT;
			}
		}
	}
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
