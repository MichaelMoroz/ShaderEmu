// Vanilla Conquer's video, pointer and keyboard on the ShaderEmu machine: what video_sdl2.cpp
// and wwkeyboard_sdl2.cpp are for SDL, on top of host.c (docs/games/tdawn.md).
#include "gbuffer.h"
#include "palette.h"
#include "video.h"
#include "wwkeyboard.h"
#include "wwmouse.h"
#include "host.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern WWKeyboardClass* Keyboard;
extern bool &ShaderEmu_GL_Emitting, &ShaderEmu_GL_Touched; // gl.cpp: the map is the GPU's
extern bool InMainLoop;

class SurfaceMonitorClassDummy : public SurfaceMonitorClass
{
public:
    virtual void Restore_Surfaces()
    {
    }
    virtual void Set_Surface_Focus(bool in_focus)
    {
    }
    virtual void Release()
    {
    }
};

SurfaceMonitorClassDummy AllSurfacesDummy;
SurfaceMonitorClass& AllSurfaces = AllSurfacesDummy;

SurfaceMonitorClass::SurfaceMonitorClass()
{
    SurfacesRestored = false;
}

static int game_w, game_h;

bool Set_Video_Mode(int w, int h, int bits_per_pixel)
{
    game_w = w;
    game_h = h;
    return host_open(w, h) == 0;
}

void Reset_Video_Mode(void)
{
}

bool Is_Video_Fullscreen()
{
    return false;
}

void Toggle_Video_Fullscreen()
{
}

void Get_Video_Scale(float& x, float& y)
{
    x = 1;
    y = 1;
}

void Set_Video_Cursor_Clip(bool clipped)
{
}

void Move_Video_Mouse(float xrel, float yrel)
{
}

void Get_Video_Mouse(int& x, int& y)
{
    host_pointer(&x, &y);
}

void Set_Video_Cursor(void* cursor, int w, int h, int hotx, int hoty)
{
    host_cursor((const unsigned char*)cursor, w, h, hotx, hoty);
}

unsigned int Get_Free_Video_Memory(void)
{
    return 1000000000;
}

unsigned Get_Video_Hardware_Capabilities(void)
{
    return VIDEO_BLITTER;
}

void Wait_Vert_Blank(void)
{
}

void Wait_Blit(void)
{
}

// (Color_Cycle's say: the words from [0] to [1] and the last are all it changed in the palette it gave last)
int ShaderEmu_Palette_Few[2];

// The game's palette is 6 bits a colour; the machine's is a word of 0x00RRGGBB.
void Set_DD_Palette(void* palette)
{
    static unsigned last[192]; // the 768 bytes it was given last
    static bool set;
    static const void* last_from;
    static unsigned char few_turn;
    const unsigned char* from = (const unsigned char*)palette;
    unsigned int* to = host_palette();

    // (the game sets it every frame, to cycle a few of its colours: only those are made again)
    const unsigned* whole = ((uintptr_t)from & 3) == 0 && set ? (const unsigned*)palette : NULL;
    int few0 = 0, few1 = 192;
    if (ShaderEmu_Palette_Few[1] && last_from == palette && whole && ++few_turn) { // (one in 256 looks at them all)
        few0 = ShaderEmu_Palette_Few[0] & ~1, few1 = (ShaderEmu_Palette_Few[1] + 2) & ~1;
    }
    ShaderEmu_Palette_Few[1] = 0;
    last_from = palette;
    for (int word = few0; word < 192; word = word + 1 == few1 && few1 < 190 ? 190 : word + 1) {
        unsigned now;
        if (whole) {
            // (two words at a look: nearly all of them are as they were)
            if (!(word & 1) && whole[word] == last[word] && whole[word + 1] == last[word + 1]) {
                word++;
                continue;
            }
            now = whole[word];
        } else {
            memcpy(&now, from + word * 4, 4);
        }
        if (set && now == last[word]) {
            continue;
        }
        last[word] = now;
        host_settle(); // (the frame in flight is looked up in it)
        for (int i = word * 4 / 3; i <= (word * 4 + 3) / 3; i++) {
            const unsigned char* c = from + i * 3;
            to[i] = (unsigned)(c[0] << 2 | c[0] >> 4) << 16 | (unsigned)(c[1] << 2 | c[1] >> 4) << 8
                    | (unsigned)(c[2] << 2 | c[2] >> 4);
        }
    }
    set = true;
}

// frames shown from the hidden page, and visible pages brought up to date after all (TDAWN_STATS)
unsigned ShaderEmu_Page_Flips, ShaderEmu_Page_Copies;

// A page of 8-bit pixels. The visible page and the first hidden one of its size are the two
// the GPU can show. The game finishes a frame by copying the hidden page to the visible one:
// that copy is put off (Stale), the hidden page is shown, and the copy is made only when
// something reads the visible page or draws on it.
class VideoSurfaceShaderEmu : public VideoSurface
{
public:
    VideoSurfaceShaderEmu(int w, int h, GBC_Enum flags)
        : Width(w)
        , Height(h)
        , Ours(false)
    {
        if ((flags & GBC_VISIBLE) && w == game_w && h == game_h && !Visible) {
            Pixels = host_page(0);
            Visible = this;
        } else if ((flags & GBC_VIDEOMEM) && w == game_w && h == game_h && !Hidden) {
            Pixels = host_page(1);
            Hidden = this;
        } else {
            Pixels = (unsigned char*)calloc(w, h);
            Ours = true;
        }
    }

    virtual ~VideoSurfaceShaderEmu()
    {
        if (Ours) {
            free(Pixels);
        }
        if (this == Visible || this == Hidden) {
            Catch_Up();
            (this == Visible ? Visible : Hidden) = nullptr;
        }
    }

    // makes the visible page what is shown again
    static void Catch_Up()
    {
        if (Stale && Visible && Hidden) {
            ShaderEmu_Page_Copies++;
            host_settle();
            memcpy(Visible->Pixels, Hidden->Pixels, (size_t)game_w * game_h);
        }
        Stale = false;
    }

    static VideoSurfaceShaderEmu* Visible;
    static VideoSurfaceShaderEmu* Hidden;
    static bool Stale;

    // a page is what the frame handed to the GPU shows: drawn before the page is written
    void Settle()
    {
        if (!Ours) {
            host_settle();
        }
    }

    virtual void* GetData() const
    {
        return Pixels;
    }
    virtual int GetPitch() const
    {
        return Width;
    }
    virtual bool IsAllocated() const
    {
        return false;
    }
    virtual void AddAttachedSurface(VideoSurface* surface)
    {
    }
    virtual bool IsReadyToBlit()
    {
        return true;
    }
    virtual bool LockWait()
    {
        Settle();
        if (this == Visible) {
            Catch_Up();
        } else if (this == Hidden && ShaderEmu_GL_Emitting) {
            ShaderEmu_GL_Touched = true;
        }
        return Pixels != nullptr;
    }
    virtual bool Unlock()
    {
        return true;
    }

    virtual void Blt(const Rect& destRect, VideoSurface* src, const Rect& srcRect, bool mask)
    {
        VideoSurfaceShaderEmu* from = (VideoSurfaceShaderEmu*)src;
        int sx = srcRect.X, sy = srcRect.Y, dx = destRect.X, dy = destRect.Y;
        int w = srcRect.Width, h = srcRect.Height;

        if (this == Visible && from == Hidden && !sx && !sy && !dx && !dy && w >= Width && h >= Height) {
            Stale = true;
            ShaderEmu_Page_Flips++;
            return;
        }
        if (this == Visible || from == Visible) {
            Catch_Up();
        }
        Settle();
        if (sx < 0) {
            w += sx, dx -= sx, sx = 0;
        }
        if (sy < 0) {
            h += sy, dy -= sy, sy = 0;
        }
        if (dx < 0) {
            w += dx, sx -= dx, dx = 0;
        }
        if (dy < 0) {
            h += dy, sy -= dy, dy = 0;
        }
        if (w > from->Width - sx) {
            w = from->Width - sx;
        }
        if (h > from->Height - sy) {
            h = from->Height - sy;
        }
        if (w > Width - dx) {
            w = Width - dx;
        }
        if (h > Height - dy) {
            h = Height - dy;
        }
        if (w <= 0 || h <= 0) {
            return;
        }
        // a page scrolled within itself: rows in the order that reads each before it is written
        bool down = from == this && dy > sy;
        for (int i = 0; i < h; i++) {
            int y = down ? h - 1 - i : i;
            memmove(Pixels + (dy + y) * Width + dx, from->Pixels + (sy + y) * from->Width + sx, w);
        }
    }

    // the rectangle's far edges are inside it, as the other backends take them
    virtual void FillRect(const Rect& rect, unsigned char color)
    {
        int x0 = rect.X < 0 ? 0 : rect.X, y0 = rect.Y < 0 ? 0 : rect.Y;
        int x1 = rect.X + rect.Width + 1, y1 = rect.Y + rect.Height + 1;

        Settle();
        if (this == Visible) {
            Catch_Up();
        } else if (this == Hidden && ShaderEmu_GL_Emitting) {
            ShaderEmu_GL_Touched = true;
        }
        if (x1 > Width) {
            x1 = Width;
        }
        if (y1 > Height) {
            y1 = Height;
        }
        for (int y = y0; y < y1 && x1 > x0; y++) {
            memset(Pixels + y * Width + x0, color, x1 - x0);
        }
    }

private:
    unsigned char* Pixels;
    int Width;
    int Height;
    bool Ours;
};

VideoSurfaceShaderEmu* VideoSurfaceShaderEmu::Visible;
VideoSurfaceShaderEmu* VideoSurfaceShaderEmu::Hidden;
bool VideoSurfaceShaderEmu::Stale;

// what a frame's showing cost, for the figures conquer.cpp prints (TDAWN_STATS)
unsigned ShaderEmu_Show_Cycles;
unsigned ShaderEmu_Shows;

void Video_Render_Frame()
{
    unsigned began = host_cycles();

    host_present(VideoSurfaceShaderEmu::Stale ? 1 : 0, !Get_Mouse_State());
    ShaderEmu_Show_Cycles += host_cycles() - began;
    ShaderEmu_Shows++;
}

Video::Video()
{
}

Video::~Video()
{
}

Video& Video::Shared()
{
    static Video video;
    return video;
}

VideoSurface* Video::CreateSurface(int w, int h, GBC_Enum flags)
{
    return new VideoSurfaceShaderEmu(w, h, flags);
}

class WWKeyboardClassShaderEmu : public WWKeyboardClass
{
public:
    virtual void Fill_Buffer_From_System(void);
    virtual KeyASCIIType To_ASCII(unsigned short key);
};

/*
** The right button held over the map and moved drags it (docs/games/tdawn.md, "Scrolling"). The
** game gets that button's press at its release, and only if the pointer stayed in place.
*/
int ShaderEmu_Drag_Slack(int x, int y); // the game's: how far is in place there, 0 where no drag begins
static int Drag_State, Drag_X, Drag_Y, Drag_Slack, Drag_DX, Drag_DY; // state 1: pressed, 2: dragging
static int Press_X, Press_Y;

static void Drag_Follow(int x, int y)
{
    if (Drag_State == 1 && abs(x - Drag_X) + abs(y - Drag_Y) > Drag_Slack) {
        Drag_State = 2;
    }
    if (Drag_State == 2) {
        Drag_DX += x - Drag_X;
        Drag_DY += y - Drag_Y;
        Drag_X = x;
        Drag_Y = y;
    }
}

// The pointer is followed while the machine's own word says the button is down: the window
// system's word of its release comes later, and the map must not go on until then.
static void Drag_Poll(void)
{
    if (Drag_State && (host_buttons() & 2)) {
        int x, y;
        host_pointer(&x, &y);
        Drag_Follow(x, y);
    }
}

// TDAWN_DRAG=N, for a test: from the Nth call in a mission the map is dragged a pixel a call, so
// that the view comes to every place of a cell: each way and back, a cell's 24 x 24 places
// row by row, then 120 pixels each way and back (the map's edges). False when it is over.
static bool Drag_Script(int* dx, int* dy)
{
    static const signed char way[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    static int from = -1, calls, first;

    if (from < 0) {
        const char* text = getenv("TDAWN_DRAG"); // (N,S: from step S of the script)
        from = text ? atoi(text) : 0;
        first = text && strchr(text, ',') ? atoi(strchr(text, ',') + 1) : 0;
    }
    int step = from && InMainLoop ? calls++ - from : -1;
    step = step < 0 ? -1 : step + first;
    if (step < 0 || step >= 240 + 600 + 480) {
        return false;
    }
    if (step < 240) {
        *dx = way[step / 30][0], *dy = way[step / 30][1];
    } else if (step < 840) {
        int row = (step - 240) / 25, at = (step - 240) % 25;
        *dx = at == 24 ? 0 : row & 1 ? -1 : 1, *dy = at == 24;
    } else {
        *dx = way[(step - 840) / 120][0], *dy = way[(step - 840) / 120][1];
    }
    return true;
}

// How far the pointer has dragged the map since the last call, in the game's pixels.
bool ShaderEmu_Drag(int* dx, int* dy)
{
    Drag_Poll();
    *dx = Drag_DX;
    *dy = Drag_DY;
    Drag_DX = Drag_DY = 0;
    return Drag_Script(dx, dy) || Drag_State == 2 || *dx || *dy;
}

bool ShaderEmu_Auto_Press; // a won mission's screens: TDAWN_AUTO presses Return through them too

void WWKeyboardClassShaderEmu::Fill_Buffer_From_System(void)
{
    static const char* automatic = getenv("TDAWN_AUTO");
    static int calls;
    struct host_event event;

    // TDAWN_AUTO=1 presses Return through the menus, for runs nobody is at
    if (automatic && (!InMainLoop || ShaderEmu_Auto_Press) && (++calls & 63) == 0) {
        Put_Key_Message(VK_RETURN, false);
        Put_Key_Message(VK_RETURN, true);
    }
    Drag_Poll();
    while (!Is_Buffer_Full() && host_event(&event)) {
        switch (event.type) {
        case HOST_KEY:
            Put_Key_Message(event.key, !event.down);
            break;
        case HOST_BUTTON:
            if (event.key == VK_RBUTTON && event.down) {
                Drag_Slack = ShaderEmu_Drag_Slack(event.x, event.y);
                if (Drag_Slack) {
                    // (where the pointer is now: an event's place is up to a tenth of a second old)
                    Drag_State = 1;
                    host_pointer(&Drag_X, &Drag_Y);
                    Press_X = event.x;
                    Press_Y = event.y;
                    break;
                }
            } else if (event.key == VK_RBUTTON && Drag_State) {
                bool dragged = Drag_State == 2;
                Drag_State = 0;
                if (dragged) {
                    break;
                }
                Put_Mouse_Message(VK_RBUTTON, Press_X, Press_Y, false);
            }
            Put_Mouse_Message(event.key, event.x, event.y, !event.down);
            break;
        case HOST_QUIT:
            exit(0);
        }
    }
}

KeyASCIIType WWKeyboardClassShaderEmu::To_ASCII(unsigned short key)
{
    static const char plain[] = ";=,-./`", shifted[] = ":+<_>?~", digits[] = ")!@#$%^&*(";
    int code = key & 0xff;
    bool shift = (key & WWKEY_SHIFT_BIT) != 0;

    if ((key & (WWKEY_RLS_BIT | WWKEY_BTN_BIT)) || (key & (WWKEY_CTRL_BIT | WWKEY_ALT_BIT))) {
        return KA_NONE;
    }
    if (code >= 'A' && code <= 'Z') {
        return (KeyASCIIType)(shift ? code : code - 'A' + 'a');
    }
    if (code >= '0' && code <= '9') {
        return (KeyASCIIType)(shift ? digits[code - '0'] : code);
    }
    if (code == VK_SPACE || code == VK_RETURN || code == VK_ESCAPE || code == VK_BACK || code == VK_TAB) {
        return (KeyASCIIType)code;
    }
    if (code >= 0xba && code <= 0xc0) {
        return (KeyASCIIType)(shift ? shifted[code - 0xba] : plain[code - 0xba]);
    }
    return KA_NONE;
}

WWKeyboardClass* CreateWWKeyboardClass(void)
{
    return new WWKeyboardClassShaderEmu;
}
