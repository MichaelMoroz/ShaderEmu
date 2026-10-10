/* ClassiCube's window and input on the ShaderEmu machine: a Nano-X window the GPU draws into. */
#include "Core.h"
#ifdef CC_BUILD_SHADEREMU
#include "_WindowBase.h"
#include "String_.h"
#include "Funcs.h"
#include "Bitmap.h"
#include "Errors.h"
#include "Options.h"
#include "Launcher.h"
#include "Chat.h"
#include "Utils.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#define MWINCLUDECOLORS
#include <nano-X.h>

#define CAPTION 22      /* the window manager's caption and frame (nanowm.h) */
#define FRAME   4
#define GPU_SIZE  0x00b00000u
#define REG_INPUT 0x20  /* the machine's input counters (docs/input.md) */
#define REG_CLOCK 0x34

static uint8_t* gpu;
static GR_WINDOW_ID window;
static GR_WINDOW_INFO window_info;
static cc_bool opened;
static int mouse_x, mouse_y;
static cc_bool mouse_known;

/* GPU memory from 0x87000000 (the device maps from 0x86000000), or NULL without the device. */
uint8_t* SE_Gpu(void) {
	int fd;
	void* map;
	if (gpu) return gpu;

	fd = open("/dev/gpu", O_RDWR);
	if (fd < 0) return NULL;
	{
		/* one program at a time draws with the GPU: the kernel says whose it is (programs/linux/gles.c) */
		unsigned other = 0;
		if (ioctl(fd, 0x80044705u, &other) < 0 && errno == EBUSY) {
			fprintf(stderr, "classicube: the GPU is drawing for another program (process %u): close it first\n", other);
			close(fd);
			exit(1);
		}
	}
	map = mmap(0, GPU_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
	if (map == MAP_FAILED) return NULL;
	return gpu = (uint8_t*)map;
}

/* The machine's clock in milliseconds: a load, where the system's is a call. */
uint32_t SE_Milliseconds(void) {
	return gpu ? ((volatile uint32_t*)gpu)[REG_CLOCK / 4] : 0;
}

/* Where the window's pixels are, for the GPU to copy a picture to. False while it has none. */
int SE_Surface(uint32_t* address, int* width, int* height, uint32_t* row) {
	static uint32_t buffers_seen;
	volatile uint32_t* regs = (volatile uint32_t*)gpu;
	if (!opened) return false;

	/* the window has another buffer (it was resized or moved): ask where */
	if (regs[0x64 / 4] != buffers_seen || !window_info.surface_address) {
		buffers_seen = regs[0x64 / 4];
		GrGetWindowInfo(window, &window_info);
	}
	*address = window_info.surface_address;
	*width   = window_info.width;
	*height  = window_info.height;
	*row     = window_info.surface_row;
	return window_info.realized && window_info.surface_address;
}

void Window_PreInit(void) { }

void Window_Init(void) {
	GR_SCREEN_INFO info;
	if (GrOpen() < 0) Process_Abort("no Nano-X server (start one: nano-X -p &)");
	GrGetScreenInfo(&info);
	SE_Gpu();

	DisplayInfo.Width  = info.cols;
	DisplayInfo.Height = info.rows;
	DisplayInfo.Depth  = 32;
	DisplayInfo.ScaleX = 1.0f;
	DisplayInfo.ScaleY = 1.0f;
	DisplayInfo.CursorVisible = true;
}

void Window_Free(void) { }

static void DoCreateWindow(int width, int height) {
	const char* s = getenv("CLASSICUBE_SIZE");
	int bare;
	if (s) { width = atoi(s); s = strchr(s, 'x'); if (s) height = atoi(s + 1); }
	if (width  > DisplayInfo.Width)  width  = DisplayInfo.Width;
	if (height > DisplayInfo.Height) height = DisplayInfo.Height;

	/* a window that leaves no room for a frame and caption has none, and sits in the corner */
	bare   = width + 2 * FRAME > DisplayInfo.Width || height + CAPTION + FRAME > DisplayInfo.Height;
	window = GrNewWindowEx(bare ? GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOAUTOMOVE : GR_WM_PROPS_APPWINDOW,
		"ClassiCube", GR_ROOT_WINDOW_ID, bare ? 0 : -1, bare ? 0 : -1, width, height, 0);
	GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE |
		GR_EVENT_MASK_FOCUS_IN | GR_EVENT_MASK_FOCUS_OUT);
	GrMapWindow(window);
	GrSetFocus(window);
	GrGetWindowInfo(window, &window_info);
	opened = true;

	Window_Main.Width    = width;
	Window_Main.Height   = height;
	Window_Main.Focused  = true;
	Window_Main.Exists   = true;
	Window_Main.UIScaleX = DEFAULT_UI_SCALE_X;
	Window_Main.UIScaleY = DEFAULT_UI_SCALE_Y;
	Window_Main.Handle.val = window;
}

void Window_Create2D(int width, int height) { DoCreateWindow(width, height); Window_Main.Is3D = false; }
void Window_Create3D(int width, int height) { DoCreateWindow(width, height); Window_Main.Is3D = true;  }

void Window_Destroy(void) {
	if (opened) GrClose();
	opened = false;
}

void Window_SetTitle(const cc_string* title) { }
void Clipboard_GetText(cc_string* value) { }
void Clipboard_SetText(const cc_string* value) { }

int Window_GetWindowState(void) { return WINDOW_STATE_NORMAL; }
cc_result Window_EnterFullscreen(void) { return ERR_NOT_SUPPORTED; }
cc_result Window_ExitFullscreen(void)  { return ERR_NOT_SUPPORTED; }
int Window_IsObscured(void)            { return 0; }

void Window_Show(void) { }
void Window_SetSize(int width, int height) { }

void Window_RequestClose(void) {
	Event_RaiseVoid(&WindowEvents.Closing);
}


/*########################################################################################################################*
*----------------------------------------------------Input processing-----------------------------------------------------*
*#########################################################################################################################*/
static int MapKey(int ch) {
	static const char symbols[] = "`-=[]/;',.\\~_+{}?:\"<>|";
	static const cc_uint8 symbolKeys[] = {
		CCKEY_TILDE, CCKEY_MINUS, CCKEY_EQUALS, CCKEY_LBRACKET, CCKEY_RBRACKET, CCKEY_SLASH, CCKEY_SEMICOLON, CCKEY_QUOTE,
		CCKEY_COMMA, CCKEY_PERIOD, CCKEY_BACKSLASH,
		CCKEY_TILDE, CCKEY_MINUS, CCKEY_EQUALS, CCKEY_LBRACKET, CCKEY_RBRACKET, CCKEY_SLASH, CCKEY_SEMICOLON, CCKEY_QUOTE,
		CCKEY_COMMA, CCKEY_PERIOD, CCKEY_BACKSLASH,
	};
	static const char shifted[] = ")!@#$%^&*(";
	const char* at;

	if (ch >= 'a' && ch <= 'z') return CCKEY_A + (ch - 'a');
	if (ch >= 'A' && ch <= 'Z') return CCKEY_A + (ch - 'A');
	if (ch >= '0' && ch <= '9') return CCKEY_0 + (ch - '0');
	if (ch >= MWKEY_F1  && ch <= MWKEY_F11) return CCKEY_F1  + (ch - MWKEY_F1);
	if (ch >= MWKEY_KP0 && ch <= MWKEY_KP9) return CCKEY_KP0 + (ch - MWKEY_KP0);

	switch (ch) {
	case MWKEY_F12:       return CCKEY_F12;
	case MWKEY_LEFT:      return CCKEY_LEFT;
	case MWKEY_RIGHT:     return CCKEY_RIGHT;
	case MWKEY_UP:        return CCKEY_UP;
	case MWKEY_DOWN:      return CCKEY_DOWN;
	case MWKEY_ENTER: case '\n': return CCKEY_ENTER;
	case MWKEY_KP_ENTER:  return CCKEY_KP_ENTER;
	case MWKEY_ESCAPE:    return CCKEY_ESCAPE;
	case MWKEY_BACKSPACE: return CCKEY_BACKSPACE;
	case MWKEY_TAB:       return CCKEY_TAB;
	case ' ':             return CCKEY_SPACE;
	case MWKEY_INSERT:    return CCKEY_INSERT;
	case MWKEY_DELETE:    return CCKEY_DELETE;
	case MWKEY_HOME:      return CCKEY_HOME;
	case MWKEY_END:       return CCKEY_END;
	case MWKEY_PAGEUP:    return CCKEY_PAGEUP;
	case MWKEY_PAGEDOWN:  return CCKEY_PAGEDOWN;
	case MWKEY_LSHIFT:    return CCKEY_LSHIFT;
	case MWKEY_RSHIFT:    return CCKEY_RSHIFT;
	case MWKEY_LCTRL:     return CCKEY_LCTRL;
	case MWKEY_RCTRL:     return CCKEY_RCTRL;
	case MWKEY_LALT:      return CCKEY_LALT;
	case MWKEY_RALT:      return CCKEY_RALT;
	case MWKEY_PAUSE:     return CCKEY_PAUSE;
	case MWKEY_CAPSLOCK:  return CCKEY_CAPSLOCK;
	}
	if (ch > 0 && ch < 128) {
		if ((at = strchr(shifted, ch))) return CCKEY_0 + (int)(at - shifted);
		if ((at = strchr(symbols, ch))) return symbolKeys[at - symbols];
	}
	return INPUT_NONE;
}

static void HandleEvent(GR_EVENT* e) {
	int key, down;

	switch (e->type) {
	case GR_EVENT_TYPE_MOUSE_POSITION:
		if (mouse_known && Input.RawMode)
			Event_RaiseRawMove(&PointerEvents.RawMoved, e->mouse.x - mouse_x, e->mouse.y - mouse_y);
		mouse_x = e->mouse.x; mouse_y = e->mouse.y; mouse_known = true;
		Pointer_SetPosition(0, mouse_x, mouse_y);
		break;

	case GR_EVENT_TYPE_BUTTON_DOWN:
	case GR_EVENT_TYPE_BUTTON_UP:
		down = e->type == GR_EVENT_TYPE_BUTTON_DOWN;
		if (e->button.changebuttons & GR_BUTTON_L) Input_Set(CCMOUSE_L, down);
		if (e->button.changebuttons & GR_BUTTON_R) Input_Set(CCMOUSE_R, down);
		if (e->button.changebuttons & GR_BUTTON_M) Input_Set(CCMOUSE_M, down);
		if (down && (e->button.buttons & GR_BUTTON_SCROLLUP)) Mouse_ScrollVWheel(1.0f);
		if (down && (e->button.buttons & GR_BUTTON_SCROLLDN)) Mouse_ScrollVWheel(-1.0f);
		break;

	case GR_EVENT_TYPE_KEY_DOWN:
	case GR_EVENT_TYPE_KEY_UP:
		down = e->type == GR_EVENT_TYPE_KEY_DOWN;
		key  = MapKey(e->keystroke.ch);
		if (key) Input_Set(key, down);
		if (down && e->keystroke.ch >= 32 && e->keystroke.ch < 127)
			Event_RaiseInt(&InputEvents.Press, e->keystroke.ch);
		break;

	case GR_EVENT_TYPE_UPDATE:
		if (e->update.utype != GR_UPDATE_SIZE) break;
		GrGetWindowInfo(window, &window_info);
		if (window_info.width == Window_Main.Width && window_info.height == Window_Main.Height) break;
		Window_Main.Width  = window_info.width;
		Window_Main.Height = window_info.height;
		Event_RaiseVoid(&WindowEvents.Resized);
		break;

	case GR_EVENT_TYPE_FOCUS_IN:
	case GR_EVENT_TYPE_FOCUS_OUT:
		Window_Main.Focused = e->type == GR_EVENT_TYPE_FOCUS_IN;
		Event_RaiseVoid(&WindowEvents.FocusChanged);
		break;

	case GR_EVENT_TYPE_CLOSE_REQ:
		Window_Main.Exists = false;
		Window_RequestClose();
		break;
	}
}

/* Lines typed at the terminal the game was started from, which is how a test drives it through
   the machine's serial console: "down KEY", "up KEY", "turn DX DY", "point X Y", "hold" (the
   frame after next stays, for comparing pictures); anything else is said in chat. */
void SE_Hold(void);

static void TerminalLine(char* line) {
	cc_string text = String_FromReadonly(line), name;
	int key, x, y;

	if (!strncmp(line, "down ", 5) || !strncmp(line, "up ", 3)) {
		name = String_FromReadonly(strchr(line, ' ') + 1);
		key  = Utils_ParseEnum(&name, INPUT_NONE, Input_StorageNames, INPUT_COUNT);
		if (key) Input_Set(key, line[0] == 'd');
	} else if (sscanf(line, "turn %d %d", &x, &y) == 2) {
		Event_RaiseRawMove(&PointerEvents.RawMoved, x, y);
	} else if (sscanf(line, "point %d %d", &x, &y) == 2) {
		Pointer_SetPosition(0, x, y);
	} else if (!strcmp(line, "hold")) {
		SE_Hold();
	} else if (text.length) {
		Chat_Send(&text, false);
	}
}

static void PollTerminal(void) {
	static char line[128];
	static int length, opened_terminal;
	char c;

	if (!opened_terminal) {
		fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
		opened_terminal = true;
	}
	while (read(0, &c, 1) == 1) {
		if (c == '\r') continue;
		if (c != '\n') { if (length < (int)sizeof(line) - 1) line[length++] = c; continue; }
		line[length] = '\0';
		length = 0;
		TerminalLine(line);
	}
}

/* The arrow keys turn the view while the game has the pointer: it is not held in the window,
   and stops turning the view where the window ends. */
static void KeysTurn(float delta) {
	float dx, dy;
	if (!Input.RawMode) return;

	dx = (float)(Input.Pressed[CCKEY_RIGHT] - Input.Pressed[CCKEY_LEFT]);
	dy = (float)(Input.Pressed[CCKEY_DOWN]  - Input.Pressed[CCKEY_UP]);
	if (dx != 0.0f || dy != 0.0f) Event_RaiseRawMove(&PointerEvents.RawMoved, dx * 600.0f * delta, dy * 400.0f * delta);
}

/* Asking the server costs two system calls and two task switches. The machine's own input
   counters say when there can be anything: ask when they have moved, for a few calls after,
   and a few times a second. */
void Window_ProcessEvents(float delta) {
	static uint32_t seen[4], asked_ms;
	static int ask = 8;
	const volatile uint32_t* input;
	GR_EVENT e;
	int i;
	if (!opened) return;
	PollTerminal();
	KeysTurn(delta);

	if (gpu) {
		input =(const volatile uint32_t*)(gpu + REG_INPUT);
		for (i = 0; i < 4; i++) {
			if (seen[i] != input[i]) { seen[i] = input[i]; ask = 8; }
		}
		if (!ask && SE_Milliseconds() - asked_ms >= 250) ask = 1;
	}

	while (ask) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE) {
			ask--;
			asked_ms = SE_Milliseconds();
			return;
		}
		HandleEvent(&e);
	}
}

void Gamepads_PreInit(void) { }
void Gamepads_Init(void) { }
void Gamepads_Process(float delta) { }

static void Cursor_GetRawPos(int* x, int* y) { *x = mouse_x; *y = mouse_y; }
void Cursor_SetPosition(int x, int y) { }
static void Cursor_DoSetVisible(cc_bool visible) { }

/* The pointer is not held: its travel over the window turns the view. */
void Window_EnableRawMouse(void)  { Input.RawMode = true;  }
void Window_UpdateRawMouse(void)  { }
void Window_DisableRawMouse(void) { Input.RawMode = false; }


/*########################################################################################################################*
*-------------------------------------------------------Misc/Other--------------------------------------------------------*
*#########################################################################################################################*/
static void ShowDialogCore(const char* title, const char* msg) {
	Platform_LogConst(title);
	Platform_LogConst(msg);
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) { return ERR_NOT_SUPPORTED; }
cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs* args) { return ERR_NOT_SUPPORTED; }

void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height) {
	bmp->scan0  = (BitmapCol*)Mem_Alloc(width * height, BITMAPCOLOR_SIZE, "window pixels");
	bmp->width  = width;
	bmp->height = height;
}
void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) { }
void Window_FreeFramebuffer(struct Bitmap* bmp) { Mem_Free(bmp->scan0); }

int SE_WorldSeed(int seed) {
	const char* s = getenv("CLASSICUBE_SEED");
	return s ? atoi(s) : seed;
}

/* What the game itself uses of the launcher, which is not built */
static char hashBuffer[STRING_SIZE];
cc_string Launcher_AutoHash = String_FromArray(hashBuffer);

void LauncherTheme_Load(struct LauncherTheme* theme) {
	Mem_Set(theme, 0, sizeof(*theme));
	theme->ButtonForeColor = BitmapColor_RGB(111, 111, 111);
}

void OnscreenKeyboard_Open(struct OpenKeyboardArgs* args) { }
void OnscreenKeyboard_SetText(const cc_string* text) { }
void OnscreenKeyboard_Close(void) { }
void Window_LockLandscapeOrientation(cc_bool lock) { }
#endif
