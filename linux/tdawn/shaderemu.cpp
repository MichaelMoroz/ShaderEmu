// Vanilla Conquer's video, pointer and keyboard on the ShaderEmu machine: what video_sdl2.cpp
// and wwkeyboard_sdl2.cpp are for SDL, on top of host.c (docs/tdawn.md).
#include "gbuffer.h"
#include "palette.h"
#include "video.h"
#include "wwkeyboard.h"
#include "wwmouse.h"
#include "host.h"
#include <cstdlib>
#include <cstring>

extern WWKeyboardClass* Keyboard;
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

// The game's palette is 6 bits a colour; the machine's is a word of 0x00RRGGBB.
void Set_DD_Palette(void* palette)
{
    const unsigned char* from = (const unsigned char*)palette;
    unsigned int* to = host_palette();

    for (int i = 0; i < 256; i++, from += 3) {
        to[i] = (unsigned)(from[0] << 2 | from[0] >> 4) << 16 | (unsigned)(from[1] << 2 | from[1] >> 4) << 8
                | (unsigned)(from[2] << 2 | from[2] >> 4);
    }
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
            memcpy(Visible->Pixels, Hidden->Pixels, (size_t)game_w * game_h);
        }
        Stale = false;
    }

    static VideoSurfaceShaderEmu* Visible;
    static VideoSurfaceShaderEmu* Hidden;
    static bool Stale;

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
        if (this == Visible) {
            Catch_Up();
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

        if (this == Visible) {
            Catch_Up();
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

void WWKeyboardClassShaderEmu::Fill_Buffer_From_System(void)
{
    static const char* automatic = getenv("TDAWN_AUTO");
    static int calls;
    struct host_event event;

    // TDAWN_AUTO=1 presses Return through the menus, for runs nobody is at
    if (automatic && !InMainLoop && (++calls & 63) == 0) {
        Put_Key_Message(VK_RETURN, false);
        Put_Key_Message(VK_RETURN, true);
    }
    while (!Is_Buffer_Full() && host_event(&event)) {
        switch (event.type) {
        case HOST_KEY:
            Put_Key_Message(event.key, !event.down);
            break;
        case HOST_BUTTON:
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
