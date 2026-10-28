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
#include "Entity.h"
#include "Camera.h"
#include "Game.h"
#include "ExtMath.h"
#include "World.h"
#include "Graphics.h"
#include "Event.h"
#include "Inventory.h"
#include "Gui.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include "../../programs/linux/fps_caption.h"
#include <GLES/segl.h>	/* (seglWorldEye, the host words) */

#define CAPTION 22      /* the window manager's caption and frame (nanowm.h) */
#define FRAME   4
#define GPU_SIZE  0x00b00000u
#define REG_INPUT 0x20  /* the machine's input counters (docs/machine/input.md) */
#define REG_CLOCK 0x34

static uint8_t* gpu;
static GR_WINDOW_ID window;
static GR_WINDOW_INFO window_info;
static cc_bool opened;
static int mouse_x, mouse_y;
static cc_bool mouse_known, mouse_inside = true;
static int origin_x, origin_y;	/* the window's corner on the display */
static cc_bool dragging;	/* the left button is held on the game: the pointer's travel turns the view */
static int drag_far, tapped;	/* how far it has gone; a press let go where it began digs once */
static uint32_t drag_ms;
/* A sitter in a holodeck's seat (docs/world/holodeck.md, "Played from the seat"): 1 with tracked
   hands, 2 with a mouse. The room is turned by `body`; the aim is the hand's from there. */
static int seated;
static int menu_on, menu_aim, menu_x, menu_y;	/* a seated player's menu: its pointer on the sheet (when aimed there), in the window's pixels */
static float body, aimed;
static uint32_t mouse_count;
#define HOST_STATE 0x360	/* the host's eight words (GLES/segl.h has their fields) */
static void WindowOrigin(void);

/* GPU memory from 0x81000000 (the device maps from 0x80000000), or NULL without the device. */
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
	if (!window_info.realized || !window_info.surface_address) return false;
	fps_caption(&window_info, regs[REG_CLOCK / 4]);	/* asked for once a frame, as it is shown */
	return true;
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
		GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_ENTER | GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ |
		GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_FOCUS_IN | GR_EVENT_MASK_FOCUS_OUT);
	GrMapWindow(window);
	GrSetFocus(window);
	GrGetWindowInfo(window, &window_info);
	WindowOrigin();
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

/* Where the window is on the display: the pointer is read in the display's pixels. */
static void WindowOrigin(void) {
	GR_WINDOW_INFO info;
	GR_WINDOW_ID id = window;
	int x = 0, y = 0, depth;

	for (depth = 0; depth < 8 && id && id != GR_ROOT_WINDOW_ID; depth++) {
		GrGetWindowInfo(id, &info);
		x += info.x; y += info.y;
		id = info.parent;
	}
	origin_x = x; origin_y = y;
}

/* The pointer, from the machine's input words themselves (docs/machine/input.md) and not from the
   server, whose word of every move was a question and an answer each. The server says when
   the pointer comes over the window and leaves it. */
void Gfx_SE_Own(void);
static void ReadPointer(const volatile uint32_t* input) {
	int x = (int)input[0] - origin_x, y = (int)input[1] - origin_y;
	if ((!mouse_inside && !dragging) || (mouse_known && x == mouse_x && y == mouse_y)) return;
	/* (an event waits for the frame's worker: what it changes may be what the list is made of) */
	Gfx_SE_Own();

	/* (a quarter of a degree a pixel at the game's own sensitivity) */
	if (mouse_known && Input.RawMode && dragging && !seated) {
		Event_RaiseRawMove(&PointerEvents.RawMoved, (x - mouse_x) * 2, (y - mouse_y) * 2);
		drag_far += abs(x - mouse_x) + abs(y - mouse_y);
	}
	mouse_x = x; mouse_y = y; mouse_known = true;
	Pointer_SetPosition(0, mouse_x, mouse_y);
}

static void HandleEvent(GR_EVENT* e) {
	int key, down;
	Gfx_SE_Own();

	switch (e->type) {
	case GR_EVENT_TYPE_MOUSE_ENTER:
		mouse_inside = true;
		mouse_known  = false; /* from where it is now */
		WindowOrigin();
		break;
	case GR_EVENT_TYPE_MOUSE_EXIT:
		mouse_inside = false;
		break;

	case GR_EVENT_TYPE_BUTTON_DOWN:
	case GR_EVENT_TYPE_BUTTON_UP:
		down = e->type == GR_EVENT_TYPE_BUTTON_DOWN;
		/* (a sitter breaks and places by the host's triggers, not the pointer's buttons: Seat) */
		if (seated && (Input.RawMode || menu_on)) e->button.changebuttons &= ~(GR_BUTTON_L | GR_BUTTON_R);
		/* In the game the left button drags the view round, as a finger on glass does; let go
		   where it was pressed, it is the left button's click. A sitter's buttons are the game's. */
		if ((e->button.changebuttons & GR_BUTTON_L) && Input.RawMode && !seated) {
			if (!down && dragging && drag_far < 6 && SE_Milliseconds() - drag_ms < 400) {
				Input_Set(CCMOUSE_L, true);
				tapped = 2;
			}
			dragging = down; drag_far = 0; drag_ms = SE_Milliseconds();
		} else if (e->button.changebuttons & GR_BUTTON_L) {
			dragging = false;
			Input_Set(CCMOUSE_L, down);
		}
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
		WindowOrigin();
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
	Gfx_SE_Own();

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
	static uint32_t asked_ms;
	char c;

	/* (a read with nothing to read is a system call: twenty times a second, not every frame) */
	if (gpu && SE_Milliseconds() - asked_ms < 50) return;
	asked_ms = SE_Milliseconds();
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

static void TurnTo(struct Entity* e, float yaw, float pitch) {
	struct LocationUpdate update;
	update.flags = LU_HAS_YAW | LU_HAS_PITCH;
	update.yaw   = yaw;
	update.pitch = Math_ClampAngle(pitch);
	if (update.pitch >= 90.0f && update.pitch <= 270.0f) update.pitch = update.pitch < 180.0f ? 90.0f : 270.0f;
	e->VTABLE->SetLocation(e, &update);
}

/* (segl.h's, for seglWorldEye: this game has no gles.c) */
const volatile unsigned int* seglHostState(void) { return gpu ? (const volatile unsigned int*)(gpu + HOST_STATE) : NULL; }

/* The player's eye over its floor, for the holodeck's default scale (game 3, linux/apps/gameid.h):
   a block is then about a metre, the sitter's own size. */
static void WorldEye(struct Entity* e) {
	static float said[4];	/* (said again only when it changes: two logarithms) */
	float now[4] = { Entity_GetEyeHeight(e), (float)Game_ViewDistance, Gfx.Projection.row1.x, Gfx.Projection.row2.y };
	if (now[2] <= 0.0f || !memcmp(now, said, sizeof(now))) return;
	memcpy(said, now, sizeof(now));
	seglWorldEye(3, now[0], 0.1f, now[1], now[2], now[3]);
}

static float Metre(const volatile uint32_t* host);
static int SheetAim(const volatile uint32_t* host, float* tx, float* ty);
static struct Matrix menu_view;		/* and the camera it opened under: the sheet stays there in the world */
#define SHEET_HALF 0.4f			/* the sheet's half height over its distance (Graphics_ShaderEmu.c) */

/* Chat or a text field being typed in: shown flat even from a seat, where W, A, S and D are the stick */
cc_bool Menus_SE_Typing(void);
static cc_bool Typing(void) { return Gui.InputGrab && (Gui.InputGrab == Gui_GetScreen(GUI_PRIORITY_CHAT) || Menus_SE_Typing()); }
float SE_SeatLift(void);

/* CLASSICUBE_SEAT's line of each block the player changes: where, and from what to what */
static void SaidBlock(void* obj, IVec3 at, BlockID was, BlockID now) {
	fprintf(stderr, "ccblock: %d %d %d %d -> %d\n", at.x, at.y, at.z, was, now);
}

/* A button held as play resumes is no press until it is let go */
static int Fresh(int* armed, int play, int down) {
	if (!play) *armed = 0; else if (!down) *armed = 1;
	return play && *armed && down;
}

/* From a holodeck's seat with tracked hands the right hand aims, and the walk and the room's
   view stay the body's; with a mouse its travel turns, and the head's tilt is the aim's rise. */
static void Seat(float delta) {
	const volatile uint32_t* host = (const volatile uint32_t*)(gpu + HOST_STATE);
	struct Entity* e = &Entities.CurPlayer->Base;
	int now = !(host[0] & 16) ? 0 : (host[0] & 32) ? 1 : (host[0] & 15) == 2 ? 2 : 0;
	static int said = -1;
	int breaking, clicking;

	if (World.Loaded) WorldEye(e);
	if (said < 0) said = getenv("CLASSICUBE_SEAT") != NULL;	/* a line every 20 frames: how it is seated, the aim, the body */
	if (said == 1) Event_Register_(&UserEvents.BlockChanged, NULL, SaidBlock);
	if (said && ++said % 20 == 0)
		fprintf(stderr, "seat: %d aim %.1f %.1f body %.1f drag %d at %.1f %.1f %.1f eye %08x picked %d %d %d %d block %.3f m lift %.1f slot %d holds %d raw %d menu %d at %d %d\n", seated, e->next.yaw, e->next.pitch,
			body, dragging, e->Position.x, e->Position.y, e->Position.z, ((volatile uint32_t*)(gpu + 0x6c))[0],
			Game_SelectedPos.valid, Game_SelectedPos.pos.x, Game_SelectedPos.pos.y, Game_SelectedPos.pos.z,
			0.004f / Metre(host), SE_SeatLift(), Inventory.SelectedIndex, Inventory_SelectedBlock, Input.RawMode, menu_on, menu_x, menu_y);
	if (now != seated) {
		seated = now;
		dragging = false;
		body = aimed = e->next.yaw;
		mouse_count = host[4] & 0xffff;
	}
	{	/* a sitter's jump: the jump button, or a desktop's Space, as the game's own key */
		static int jump, armed;
		int now = Fresh(&armed, seated && Input.RawMode, SEGL_HOST_JUMP(host));
		if (now != jump) {
			jump = now;
			Gfx_SE_Own();
			if (now) Input_SetPressed(CCKEY_SPACE);
			else Input_SetReleased(CCKEY_SPACE);
		}
	}
	{	/* its triggers: the right one (a desktop's left button or Ctrl) breaks, the left one
		   (a desktop's right button) places, each where its hand's beam is (Camera.c) */
		static int armed[2], right;
		int play = seated && Input.RawMode, now = Fresh(&armed[0], play, SEGL_HOST_HAND_TRIGGER(host, 0));
		breaking = Fresh(&armed[1], play, SEGL_HOST_HAND_TRIGGER(host, 1));
		if (now != right) { right = now; Gfx_SE_Own(); Input_Set(CCMOUSE_R, now); }
	}
	{	/* the block in hand: a desktop's wheel (its notches counted in the words), or the right
		   stick pushed up or down, a step and then one every 0.3 s; the left grip is the inventory */
		static int wheel = -1, grip, gripWas, key;
		static float pushed;
		int notches = SEGL_HOST_WHEEL(host), push = (signed char)(host[5] >> 24), g;
		int scrolls = Input.RawMode || menu_on;
		if (seated == 2 && wheel >= 0 && scrolls && notches != wheel) { Gfx_SE_Own(); Mouse_ScrollVWheel((float)(signed char)(notches - wheel)); }
		wheel = seated == 2 ? notches : -1;
		if (seated == 1 && scrolls && (push > 90 || push < -90)) {
			if (pushed <= 0.0f) { Gfx_SE_Own(); Mouse_ScrollVWheel(push > 0 ? 1.0f : -1.0f); pushed = 0.3f; }
			pushed -= delta;
		} else pushed = 0.0f;
		/* (by the grip's own press: one held as a menu closes opens nothing) */
		g = seated == 1 && SEGL_HOST_HAND_GRIP(host, 0);
		if (g && !gripWas && !grip && (Input.RawMode || Gui.InputGrab == Gui_GetScreen(GUI_PRIORITY_INVENTORY))) {
			key = KeyBind_Mappings[BIND_INVENTORY].button1 ? KeyBind_Mappings[BIND_INVENTORY].button1 : 'B';
			Gfx_SE_Own();
			Input_SetPressed(key);
			grip = 1;
		} else if (grip && !g) { Gfx_SE_Own(); Input_SetReleased(key); grip = 0; }
		gripWas = g;
	}
	{	/* a menu from the seat stays where it opened in the game's world, and the camera goes on
		   turning: its pointer is where the right hand's beam, or a desktop's view, meets the sheet
		   it is on (SheetAim); the right trigger clicks there, and nowhere off the sheet */
		static int was, ignore;
		int now = seated && !Input.RawMode && World.Loaded && !Typing(), trig = SEGL_HOST_HAND_TRIGGER(host, 1);
		menu_aim = 0;
		if (now) {
			float tx, ty, aspect = (float)Game.Width / Game.Height;
			/* (a trigger held as it opens is no click: it waits for the next press) */
			if (!was) ignore = trig;
			/* (core 0's camera: Gfx.View is the worker's, and the held block's for a while) */
			if (!was || menu_view.row4.w == 0.0f) menu_view = SE_Frame->view;
			if (SheetAim(host, &tx, &ty) && Math_AbsF(tx) < SHEET_HALF * aspect && Math_AbsF(ty) < SHEET_HALF) {
				int x = (int)((tx / (SHEET_HALF * aspect) + 1.0f) * 0.5f * Game.Width);
				int y = (int)((1.0f - ty / SHEET_HALF) * 0.5f * Game.Height);
				menu_aim = 1;
				if (x != menu_x || y != menu_y) { menu_x = x; menu_y = y; Gfx_SE_Own(); Pointer_SetPosition(0, x, y); }
			}
		}
		if (!trig) ignore = 0;
		clicking = now && menu_aim && trig && !ignore;
		was = menu_on = now;
	}
	{	/* the left button has one owner: the right trigger in play, its click on a menu */
		static int left;
		int want = breaking || clicking;
		if (want != left) { left = want; Gfx_SE_Own(); Input_Set(CCMOUSE_L, want); }
	}
	if (!Input.RawMode && !menu_on) return;
	if (seated == 1) {
		/* (what the keys turned the aim by since, the body turned) */
		float look = (signed char)(host[5] >> 16) / 127.0f;
		body += e->next.yaw - aimed;
		body += (Math_AbsF(look) < 0.15f ? 0.0f : look * Math_AbsF(look)) * 140.0f * delta;	/* the right stick */
		aimed = body + (short)(host[7] & 0xffff) * (360.0f / 65536.0f);
		TurnTo(e, aimed, -(short)(host[7] >> 16) * (360.0f / 65536.0f));
	} else if (seated == 2) {
		uint32_t count = host[4] & 0xffff;
		TurnTo(e, e->next.yaw + 0.066f * (short)(count - mouse_count), -(short)(host[1] >> 16) * (360.0f / 65536.0f));
		mouse_count = count;
	}
}

/* For the frame's words (Graphics_ShaderEmu.c): a menu, or a world not loaded yet, is a
   picture for a screen and not a world for a room. */
int SE_Flat(void) { return !World.Loaded || (!Input.RawMode && (!seated || Typing())); }

/* For the sheet's cursor (Graphics_ShaderEmu.c), on core 0: where a seated player's menu pointer is */
int SE_SeatCursor(int* x, int* y) { *x = menu_x; *y = menu_y; return menu_on && menu_aim; }

/* For the camera (Camera.c): whether the player sits with tracked hands, and then the yaw of
   the room's view. */
int SE_SeatBody(float* yaw) {
	if (seated != 1) return 0;
	*yaw = body;
	return 1;
}

/* For the frame (Game.c): whether it is drawn round a holodeck's seat. */
int SE_Seated(void) { return gpu && SEGL_HOST_HOLODECK((const volatile uint32_t*)(gpu + HOST_STATE)); }

/* For the chunks in view (Game.c): then the way the head looks, yaw and pitch as the game's. */
int SE_SeatHead(float* yaw, float* pitch) {
	const volatile uint32_t* host = (const volatile uint32_t*)(gpu + HOST_STATE);
	if (seated != 1) return 0;
	*yaw   = body + (short)(host[1] & 0xffff) * (360.0f / 65536.0f);
	*pitch = -(short)(host[1] >> 16) * (360.0f / 65536.0f);
	return 1;
}

/* For the walk (Entity.c): then the way the head looks, and the left stick added to the keys. */
int SE_SeatWalk(float* yaw, float* xMoving, float* zMoving) {
	const volatile uint32_t* host = (const volatile uint32_t*)(gpu + HOST_STATE);
	if (!seated) return 0;
	/* (a desktop's sitter walks the way they look, by the keys that come as the left stick) */
	if (seated == 1) *yaw = body + (short)(host[1] & 0xffff) * (360.0f / 65536.0f);
	if (Input.RawMode) {
		*xMoving += (signed char)(host[5]) / 127.0f;
		*zMoving -= (signed char)(host[5] >> 8) / 127.0f;
	}
	return 1;
}

/* Blocks in 4 mm of the room, at the room's scale. */
static float Metre(const volatile uint32_t* host) {
	static float metre, said[4];	/* (worked out again only when it changes: a power) */
	float now[4] = { (float)(host[0] >> 24), (float)Game_ViewDistance, Gfx.Projection.row1.x, Gfx.Projection.row2.y };
	if (memcmp(now, said, sizeof(now))) {
		/* blocks a metre: the room's plane, 1.28 by 0.8 m, is so far from the camera (GpuVolumePass.cginc) */
		float x = 1.28f * now[2], y = 0.8f * now[3], plane = 0.1f * powf(now[1] / 0.1f, now[0] / 255.0f);
		memcpy(said, now, sizeof(now));
		metre = 2.0f * plane / (x < y ? x : y) * 0.004f;
	}
	return metre;
}

/* For the 2D screen (Graphics_ShaderEmu.c), on core 0: how far before a sitter's camera it is
   drawn, in blocks (half a metre of the room, and twice the near plane); 0 when the room shows it flat. */
float SE_SeatSheet(void) {
	if (!seated || SE_Flat()) return 0.0f;
	float at = Metre((const volatile uint32_t*)(gpu + HOST_STATE)) * 125.0f;
	return at > 0.2f ? at : 0.2f;
}

static int HandAt(int word, float* at) {
	float metre = Metre((const volatile uint32_t*)(gpu + HOST_STATE));
	at[0] = ((int)((uint32_t)word << 22) >> 22) * metre;
	at[1] = ((int)((uint32_t)word << 12) >> 22) * metre;
	at[2] = ((int)((uint32_t)word << 2) >> 22) * metre;
	return 1;
}

/* For the pick and the held block (Camera.c, Game.c's SEFrame), on core 0: then also where the
   right hand is from the eye, in blocks: right, up, forward. */
int SE_SeatHand(float* at) {
	if (seated != 1) return 0;
	return HandAt(((const volatile uint32_t*)(gpu + HOST_STATE))[6], at);
}

/* A view matrix's inverse: its turn transposed, its move undone (row vectors, as ClassiCube's) */
void SE_RigidInverse(struct Matrix* out, const struct Matrix* m) {
	*out = Matrix_Identity;
	out->row1.x = m->row1.x; out->row1.y = m->row2.x; out->row1.z = m->row3.x;
	out->row2.x = m->row1.y; out->row2.y = m->row2.y; out->row2.z = m->row3.y;
	out->row3.x = m->row1.z; out->row3.y = m->row2.z; out->row3.z = m->row3.z;
	out->row4.x = -(m->row4.x * out->row1.x + m->row4.y * out->row2.x + m->row4.z * out->row3.x);
	out->row4.y = -(m->row4.x * out->row1.y + m->row4.y * out->row2.y + m->row4.z * out->row3.y);
	out->row4.z = -(m->row4.x * out->row1.z + m->row4.y * out->row2.z + m->row4.z * out->row3.z);
}

/* For the sheet (Graphics_ShaderEmu.c), on core 0: the camera a seated player's menu opened under */
static int MenuView(struct Matrix* view) {
	struct Matrix back;
	const struct Matrix* now = &SE_Frame->view;
	if (menu_view.row4.w == 0.0f || now->row4.w == 0.0f) return 0;
	/* turned as it opened, but where the camera is now: a fall or a teleport takes the sheet along */
	SE_RigidInverse(&back, now);
	*view = menu_view;
	view->row4.x = -(back.row4.x * view->row1.x + back.row4.y * view->row2.x + back.row4.z * view->row3.x);
	view->row4.y = -(back.row4.x * view->row1.y + back.row4.y * view->row2.y + back.row4.z * view->row3.y);
	view->row4.z = -(back.row4.x * view->row1.z + back.row4.y * view->row2.z + back.row4.z * view->row3.z);
	return 1;
}
int SE_SeatMenuView(struct Matrix* view) { return menu_on && MenuView(view); }

/* Where the aim meets the sheet, over its distance: the right hand's beam (the camera is the
   seat's eye, level and the seat's way), or a desktop's view, taken from the camera now through
   the world to the one the menu opened under, before which the sheet stands */
static int SheetAim(const volatile uint32_t* host, float* tx, float* ty) {
	struct Matrix back, to, opened;
	Vec3 o = Vec3_Create3(0.0f, 0.0f, 0.0f), w, d = Vec3_Create3(0.0f, 0.0f, -1.0f);
	float dist = SE_SeatSheet(), t;
	if (dist <= 0.0f || !MenuView(&opened)) return 0;
	if (seated == 1) {
		float at[3], yaw = (short)(host[7] & 0xffff) * (2.0f * MATH_PI / 65536.0f), pitch = (short)(host[7] >> 16) * (2.0f * MATH_PI / 65536.0f);
		HandAt(host[6], at);
		o = Vec3_Create3(at[0], at[1], -at[2]);
		d = Vec3_Create3(Math_SinF(yaw) * Math_CosF(pitch), Math_SinF(pitch), -Math_CosF(yaw) * Math_CosF(pitch));
	}
	SE_RigidInverse(&back, &SE_Frame->view);
	Matrix_Mul(&to, &back, &opened);
	Vec3_Transform(&w, &o, &to);
	o = w;
	w.x = d.x * to.row1.x + d.y * to.row2.x + d.z * to.row3.x;
	w.y = d.x * to.row1.y + d.y * to.row2.y + d.z * to.row3.y;
	w.z = d.x * to.row1.z + d.y * to.row2.z + d.z * to.row3.z;
	if (w.z > -0.05f) return 0;
	t = (-dist - o.z) / w.z;
	*tx = (o.x + t * w.x) / dist;
	*ty = (o.y + t * w.y) / dist;
	return 1;
}

/* For the place (Camera.c): then where the left hand is, as SE_SeatHand, and its aim in the
   game's degrees (yaw, pitch down) from word 3's high half (GLES/segl.h). */
int SE_SeatLeft(float* at, float* yaw, float* pitch) {
	const volatile uint32_t* host = (const volatile uint32_t*)(gpu + HOST_STATE);
	if (seated != 1) return 0;
	*yaw   = body + SEGL_HOST_LEFT_YAW(host) * (360.0f / 65536.0f);
	*pitch = -SEGL_HOST_LEFT_PITCH(host) * (360.0f / 65536.0f);
	return HandAt(host[4], at);
}

/* For the camera (Camera.c): how far over the player's eye the room's view is, in blocks. With
   small blocks the ground is LAP metres under the seat's eye, round the sitter's hands. */
#define LAP 0.6f
float SE_SeatLift(void) {
	float m, lift;
	if (seated != 1) return 0.0f;
	m = Metre((const volatile uint32_t*)(gpu + HOST_STATE)) * 250.0f;
	lift = LAP * m - Entity_GetEyeHeight(&Entities.CurPlayer->Base);
	return lift > 0.0f ? lift : 0.0f;
}

/* For the block in the right hand (HeldBlockRenderer.c): its model scale, HELD metres in the room
   whatever size the world is. */
#define HELD 0.04f
float SE_SeatHeld(void) {
	if (seated != 1) return 0.4f;
	return HELD * Metre((const volatile uint32_t*)(gpu + HOST_STATE)) * 250.0f;
}

/* Asking the server costs two system calls and two task switches. The machine's own input
   counters say when there can be anything: ask when they have moved, for a few calls after,
   and a few times a second. */
void Window_ProcessEvents(float delta) {
	static uint32_t seen[4], asked_ms, placed_ms;
	static int ask = 8;
	const volatile uint32_t* input;
	GR_EVENT e;
	int i;
	if (!opened) return;
	PollTerminal();
	KeysTurn(delta);
	if (tapped && !--tapped) Input_Set(CCMOUSE_L, false);
	if (gpu && Entities.CurPlayer) Seat(delta);

	if (gpu) {
		input =(const volatile uint32_t*)(gpu + REG_INPUT);
		ReadPointer(input);
		for (i = 2; i < 4; i++) { /* the buttons, and the count of keys */
			if (seen[i] != input[i]) { seen[i] = input[i]; ask = 8; }
		}
		if (!ask && SE_Milliseconds() - asked_ms >= 250) {
			ask = 1;
			/* (a window's frame can be moved without a word to the window) */
			if (SE_Milliseconds() - placed_ms >= 3000) { placed_ms = SE_Milliseconds(); WindowOrigin(); }
		}
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
