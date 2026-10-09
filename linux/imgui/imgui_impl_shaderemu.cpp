// Dear ImGui's platform and renderer for this machine (docs/imgui.md).
#include "imgui_impl_shaderemu.h"
#include <sys/time.h>
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
}

static GR_WINDOW_ID window;
static GLuint font_texture;
static struct timeval last_frame;

// ---- input ----

static ImGuiKey key_of(unsigned ch, bool ctrl)
{
    switch (ch) {
        case MWKEY_TAB: return ImGuiKey_Tab;
        case MWKEY_LEFT: return ImGuiKey_LeftArrow;
        case MWKEY_RIGHT: return ImGuiKey_RightArrow;
        case MWKEY_UP: return ImGuiKey_UpArrow;
        case MWKEY_DOWN: return ImGuiKey_DownArrow;
        case MWKEY_PAGEUP: return ImGuiKey_PageUp;
        case MWKEY_PAGEDOWN: return ImGuiKey_PageDown;
        case MWKEY_HOME: return ImGuiKey_Home;
        case MWKEY_END: return ImGuiKey_End;
        case MWKEY_INSERT: return ImGuiKey_Insert;
        case MWKEY_DELETE: return ImGuiKey_Delete;
        case MWKEY_BACKSPACE: return ImGuiKey_Backspace;
        case MWKEY_ENTER: return ImGuiKey_Enter;
        case MWKEY_ESCAPE: return ImGuiKey_Escape;
        case ' ': return ImGuiKey_Space;
        case MWKEY_LCTRL: return ImGuiKey_LeftCtrl;
        case MWKEY_RCTRL: return ImGuiKey_RightCtrl;
        case MWKEY_LSHIFT: return ImGuiKey_LeftShift;
        case MWKEY_RSHIFT: return ImGuiKey_RightShift;
        case MWKEY_LALT: return ImGuiKey_LeftAlt;
        case MWKEY_RALT: return ImGuiKey_RightAlt;
    }
    // the keyboard driver hands a letter with Ctrl over as its control character
    if (ctrl && ch >= 1 && ch <= 26) return (ImGuiKey)(ImGuiKey_A + ch - 1);
    if (ch >= 'a' && ch <= 'z') return (ImGuiKey)(ImGuiKey_A + ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return (ImGuiKey)(ImGuiKey_A + ch - 'A');
    if (ch >= '0' && ch <= '9') return (ImGuiKey)(ImGuiKey_0 + ch - '0');
    return ImGuiKey_None;
}

static void key_event(const GR_EVENT_KEYSTROKE& k, bool down)
{
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = (k.modifiers & MWKMOD_CTRL) != 0;
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, (k.modifiers & MWKMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (k.modifiers & MWKMOD_ALT) != 0);
    ImGuiKey key = key_of(k.ch, ctrl);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (down && !ctrl && k.ch >= 32 && k.ch < 127) io.AddInputCharacter(k.ch);
}

bool ImGui_ImplShaderEmu_Init(unsigned int nano_x_window)
{
    ImGuiIO& io = ImGui::GetIO();
    window = nano_x_window;
    io.BackendPlatformName = io.BackendRendererName = "imgui_impl_shaderemu";
    GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP |
                               GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION |
                               GR_EVENT_MASK_MOUSE_EXIT);
    // the font's picture: bytes of red, green, blue and alpha, which the library makes the device's words
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->SetTexID((ImTextureID)font_texture);
    io.Fonts->ClearTexData();
    gettimeofday(&last_frame, nullptr);
    return font_texture != 0;
}

bool ImGui_ImplShaderEmu_NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    GR_EVENT event;
    bool open = true;
    for (;;) {
        GrCheckNextEvent(&event);
        if (event.type == GR_EVENT_TYPE_NONE) break;
        switch (event.type) {
            case GR_EVENT_TYPE_CLOSE_REQ: open = false; break;
            case GR_EVENT_TYPE_UPDATE:
                if (event.update.utype == GR_UPDATE_SIZE) seglWindowChanged();
                break;
            case GR_EVENT_TYPE_MOUSE_POSITION: io.AddMousePosEvent((float)event.mouse.x, (float)event.mouse.y); break;
            case GR_EVENT_TYPE_MOUSE_EXIT: io.AddMousePosEvent(-FLT_MAX, -FLT_MAX); break;
            case GR_EVENT_TYPE_BUTTON_DOWN:
            case GR_EVENT_TYPE_BUTTON_UP: {
                bool down = event.type == GR_EVENT_TYPE_BUTTON_DOWN;
                // a notch of the wheel is a button going down (docs/input.md)
                if (event.button.buttons & GR_BUTTON_SCROLLUP) io.AddMouseWheelEvent(0, 1);
                if (event.button.buttons & GR_BUTTON_SCROLLDN) io.AddMouseWheelEvent(0, -1);
                io.AddMousePosEvent((float)event.button.x, (float)event.button.y);
                if (event.button.changebuttons & GR_BUTTON_L) io.AddMouseButtonEvent(0, down);
                if (event.button.changebuttons & GR_BUTTON_R) io.AddMouseButtonEvent(1, down);
                if (event.button.changebuttons & GR_BUTTON_M) io.AddMouseButtonEvent(2, down);
                break;
            }
            case GR_EVENT_TYPE_KEY_DOWN: key_event(event.keystroke, true); break;
            case GR_EVENT_TYPE_KEY_UP: key_event(event.keystroke, false); break;
        }
    }
    int width, height;
    seglSize(&width, &height);
    io.DisplaySize = ImVec2((float)width, (float)height);
    struct timeval now;
    gettimeofday(&now, nullptr);
    float passed = (float)(now.tv_sec - last_frame.tv_sec) + (float)(now.tv_usec - last_frame.tv_usec) * 1e-6f;
    io.DeltaTime = passed > 0 ? passed : 1.0f / 60;
    last_frame = now;
    return open;
}

// ---- drawing ----

// A corner as ImGui has it (ImDrawVert's layout).
struct corner { float x, y, u, v; ImU32 colour; };

static ImU32 known;      // the colour written last, and
static float shade[4];   // the device's four numbers for it: most vertices repeat the last one's

// Sixteen numbers a vertex: position, an unused four, texture coordinates, colour. The first
// two pairs are ImGui's floats as they are, so they go over as words.
static inline ImU32* put(ImU32* to, const ImU32* from)
{
    if (from[4] != known) {
        known = from[4];
        for (int k = 0; k < 4; k++) shade[k] = (float)(known >> (8 * k) & 255) * (1.0f / 255);
    }
    const ImU32* tint = (const ImU32*)shade;
    to[0] = from[0], to[1] = from[1], to[2] = 0;
    to[8] = from[2], to[9] = from[3];
    to[12] = tint[0], to[13] = tint[1], to[14] = tint[2], to[15] = tint[3];
    return to + 16;
}
static inline ImU32* put(ImU32* to, const corner& c) { return put(to, (const ImU32*)&c); }

static corner between(const corner& a, const corner& b, float t)
{
    corner c = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t, a.colour};
    if (a.colour != b.colour) {
        c.colour = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            float from = (float)(a.colour >> shift & 255), to = (float)(b.colour >> shift & 255);
            c.colour |= (ImU32)(from + (to - from) * t + 0.5f) << shift;
        }
    }
    return c;
}

// The part of a polygon on the inner side of one edge of the clip rectangle: x or y at least
// (or at most) `limit`. The device has no scissor, so a triangle that crosses one is cut here.
static int cut(const corner* in, int count, corner* out, bool is_y, bool at_least, float limit)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const corner &a = in[i], &b = in[(i + 1) % count];
        float pa = is_y ? a.y : a.x, pb = is_y ? b.y : b.x;
        bool a_in = at_least ? pa >= limit : pa <= limit, b_in = at_least ? pb >= limit : pb <= limit;
        if (a_in) out[n++] = a;
        if (a_in != b_in) out[n++] = between(a, b, (limit - pa) / (pb - pa));
    }
    return n;
}

static inline float least(float a, float b, float c)
{
    float m = a < b ? a : b;
    return m < c ? m : c;
}

void ImGui_ImplShaderEmu_RenderDrawData(ImDrawData* draw_data)
{
    GLsizei room = 0, used = 0;
    ImU32* to = (ImU32*)seglScreenSpace(&room);
    static_assert(sizeof(ImDrawVert) == sizeof(corner), "a corner is an ImDrawVert");
    for (int n = 0; n < draw_data->CmdListsCount && to; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        const ImDrawVert* vertices = list->VtxBuffer.Data;
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) cmd.UserCallback(list, &cmd);
                continue;
            }
            const ImVec4 clip = cmd.ClipRect;
            const ImDrawIdx* index = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* base = vertices + cmd.VtxOffset;
            for (unsigned i = 0; i + 2 < cmd.ElemCount && used + 21 <= room; i += 3) {
                const corner *p = (const corner*)&base[index[i]], *q = (const corner*)&base[index[i + 1]],
                             *r = (const corner*)&base[index[i + 2]];
                // the triangle's box against the clip rectangle: nearly every one is all inside
                float x0 = least(p->x, q->x, r->x), x1 = -least(-p->x, -q->x, -r->x);
                float y0 = least(p->y, q->y, r->y), y1 = -least(-p->y, -q->y, -r->y);
                if (x0 >= clip.x && x1 <= clip.z && y0 >= clip.y && y1 <= clip.w) {
                    to = put(put(put(to, *p), *q), *r);
                    used += 3;
                } else if (x1 >= clip.x && x0 <= clip.z && y1 >= clip.y && y0 <= clip.w) {
                    // cut by each edge in turn: up to seven corners, drawn as a fan
                    corner tri[3] = {*p, *q, *r}, a[8], b[8];
                    int count = cut(tri, 3, a, false, true, clip.x);
                    count = cut(a, count, b, false, false, clip.z);
                    count = cut(b, count, a, true, true, clip.y);
                    count = cut(a, count, b, true, false, clip.w);
                    for (int k = 1; k + 1 < count; k++) {
                        to = put(put(put(to, b[0]), b[k]), b[k + 1]);
                        used += 3;
                    }
                }
            }
            // a command of the device's for every one of ImGui's that drew: each has one texture
            seglScreenUsed(used, (GLuint)cmd.TextureId);
            if (used) to = (ImU32*)seglScreenSpace(&room);
            used = 0;
        }
    }
}
