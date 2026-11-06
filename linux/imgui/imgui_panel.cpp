// A program's panel by Dear ImGui over its OpenGL picture (imgui_panel.h; docs/apps/imgui.md, "A panel over a
// program's picture"). Its input, when it wants a frame, and the frame put again in between.
#include <string.h>
#include <time.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_shaderemu.h"
#include "imgui_panel.h"

static int frames_left;     // frames wanted after an input: a click and a release, a hover settling
static bool input;          // an input since the last frame: what it opens is laid out in one frame and drawn in the next
static unsigned last_ms;
static bool kept;           // a frame of ImGui's is kept to be put again
static bool kept_asked;

static unsigned now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

int imp_init(unsigned int window)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // (the disk is a ROM)
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // ImGui smooths a line by a texture's soft edge, and the device takes the nearest texel: the
    // same pixels as a plain line, whose quads the backend copies in half the instructions.
    style.AntiAliasedLines = false;
    style.WindowRounding = 0, style.WindowBorderSize = 0, style.WindowPadding = ImVec2(6, 6);
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    if (!ImGui_ImplShaderEmu_Init(window)) return 0;
    last_ms = now_ms();
    return 1;
}

// (from the font and the style: before the first frame ImGui's own font size is 0)
static float font_size()
{
    ImGuiIO& io = ImGui::GetIO();
    return ImGui::GetFontSize() > 0 ? ImGui::GetFontSize() : io.Fonts->Fonts.Size ? io.Fonts->Fonts[0]->FontSize : 13.0f;
}
int imp_line() { return (int)(font_size() + ImGui::GetStyle().FramePadding.y * 2); }
int imp_text_line() { return (int)(font_size() + ImGui::GetStyle().ItemSpacing.y); }

// A button's change waits until a frame has had the pointer where it is: a tab, which other items may lie over, takes
// a click only where the pointer was the frame before, and a scripted click (nxkey) moves and presses at once.
static int seen;                // frames since the pointer moved
static int pending[8], pending_count;
static float pointer_x = -1, pointer_y = -1;

void imp_pointer(int x, int y)
{
    if ((float)x == pointer_x && (float)y == pointer_y) return;
    pointer_x = (float)x, pointer_y = (float)y, seen = 0, input = true;
    ImGui::GetIO().AddMousePosEvent(pointer_x, pointer_y), frames_left = frames_left > 2 ? frames_left : 2;
}

// Presses queued and given to ImGui, and whether the last given found nothing of ImGui's under the pointer.
static int downs_queued, downs_given;
static bool down_free;

int imp_button(int button, int down)
{
    if (pending_count < 8) pending[pending_count++] = button << 1 | (down != 0), downs_queued += down != 0;
    frames_left = 3, input = true;
    return downs_queued;
}

int imp_press_given(int press, int* free)
{
    *free = down_free;
    return downs_given >= press;
}

void imp_wheel(int notches) { ImGui::GetIO().AddMouseWheelEvent(0, (float)notches), frames_left = frames_left > 2 ? frames_left : 2, input = true; }

void imp_key(int ch, int modifiers, int down)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = (modifiers & MWKMOD_CTRL) != 0;
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Shift, (modifiers & MWKMOD_SHIFT) != 0);
    ImGuiKey key = ImGuiKey_None;
    switch (ch) {
        case MWKEY_LEFT: key = ImGuiKey_LeftArrow; break;
        case MWKEY_RIGHT: key = ImGuiKey_RightArrow; break;
        case MWKEY_UP: key = ImGuiKey_UpArrow; break;
        case MWKEY_DOWN: key = ImGuiKey_DownArrow; break;
        case MWKEY_HOME: key = ImGuiKey_Home; break;
        case MWKEY_END: key = ImGuiKey_End; break;
        case MWKEY_DELETE: key = ImGuiKey_Delete; break;
        case MWKEY_BACKSPACE: key = ImGuiKey_Backspace; break;
        case MWKEY_ENTER: key = ImGuiKey_Enter; break;
        case MWKEY_ESCAPE: key = ImGuiKey_Escape; break;
        case MWKEY_TAB: key = ImGuiKey_Tab; break;
        default:
            if (ctrl && ch >= 1 && ch <= 26) key = (ImGuiKey)(ImGuiKey_A + ch - 1);
            else if (ch >= 'a' && ch <= 'z') key = (ImGuiKey)(ImGuiKey_A + ch - 'a');
    }
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down != 0);
    if (down && !ctrl && ch >= 32 && ch < 127) io.AddInputCharacter((unsigned)ch);
    frames_left = frames_left > 2 ? frames_left : 2, input = true;
}

int imp_wants_pointer() { return ImGui::GetIO().WantCaptureMouse; }
int imp_typing() { return ImGui::GetIO().WantTextInput; }
void imp_keep() { ImGui_ImplShaderEmu_KeepFrames(true), kept_asked = true; }
void imp_wake(int frames) { frames_left = frames_left > frames ? frames_left : frames; }
int imp_busy() { return frames_left > 0 || pending_count > 0 || ImGui::GetCurrentContext()->InputEventsQueue.Size > 0; }

// (whether the pointer at x, y is over one of ImGui's windows as the last frame laid them out, or a popup is open)
static bool over_imgui(int x, int y)
{
    ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.OpenPopupStack.Size) return true;
    for (ImGuiWindow* w : g.Windows)
        if (w->WasActive && !w->Hidden && w->OuterRectClipped.Contains(ImVec2((float)x, (float)y))) return true;
    return false;
}

static bool held;           // a press ImGui took is held: its release and the moves till then are ImGui's
static bool pointer_ours;   // ImGui was last told of the pointer over its own

// (a move or a press over the program's picture is not told to ImGui, which would want frames for it: a move that
// leaves ImGui's windows is, once, so that nothing stays lit)
int imp_event(const GR_EVENT* e)
{
    switch (e->type) {
    case GR_EVENT_TYPE_MOUSE_MOTION:
    case GR_EVENT_TYPE_MOUSE_POSITION: {
        const bool ours = held || over_imgui(e->mouse.x, e->mouse.y);
        if (ours || pointer_ours) imp_pointer(e->mouse.x, e->mouse.y);
        pointer_ours = ours;
        return ours;
    }
    case GR_EVENT_TYPE_BUTTON_DOWN:
        // (over ImGui's, or anywhere with a popup open: a press there closes it)
        if (!over_imgui(e->button.x, e->button.y)) return 0;
        imp_pointer(e->button.x, e->button.y);
        if (e->button.changebuttons & GR_BUTTON_L) imp_button(0, 1);
        if (e->button.changebuttons & GR_BUTTON_R) imp_button(1, 1);
        if (e->button.changebuttons & GR_BUTTON_M) imp_button(2, 1);
        held = pointer_ours = true;
        return 1;
    case GR_EVENT_TYPE_BUTTON_UP:
        if (!held) return 0;
        imp_pointer(e->button.x, e->button.y);
        if (e->button.changebuttons & GR_BUTTON_L) imp_button(0, 0);
        if (e->button.changebuttons & GR_BUTTON_R) imp_button(1, 0);
        if (e->button.changebuttons & GR_BUTTON_M) imp_button(2, 0);
        held = false;
        return 1;
    case GR_EVENT_TYPE_KEY_DOWN:
    case GR_EVENT_TYPE_KEY_UP:
        if (!imp_typing()) return 0;
        imp_key(e->keystroke.ch, e->keystroke.modifiers, e->type == GR_EVENT_TYPE_KEY_DOWN);
        return 1;
    }
    return 0;
}

// (`always`: a frame for any change is followed by one more, as nxpath's are; else only one for an input)
static void begin(int wide, int high, bool always)
{
    ImGuiIO& io = ImGui::GetIO();
    const unsigned now = now_ms();
    io.DisplaySize = ImVec2((float)wide, (float)high);
    io.DeltaTime = now != last_ms ? (float)(now - last_ms) * 0.001f : 0.001f;
    last_ms = now;
    if (frames_left > 0) frames_left--;
    int downs = 0;
    if (seen > 0) {
        for (int i = 0; i < pending_count; i++) io.AddMouseButtonEvent(pending[i] >> 1, pending[i] & 1), downs += pending[i] & 1;
        pending_count = 0;
    }
    seen++;
    // (a frame for a change is followed by one more: ImGui lays out what appears, a tab bar, in one frame and draws it in the next)
    static bool settling;
    if (settling) settling = false;
    else if (!frames_left && (always || input)) settling = true, frames_left = 1;
    input = false;
    ImGui::NewFrame();
    // (a press given now: whether it is the program's, as ImGui saw the pointer with the popups open before it)
    if (downs) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        down_free = g.HoveredWindow == nullptr && g.OpenPopupStack.Size == 0;
        downs_given += downs;
    }
}

void imp_begin(int wide, int high) { begin(wide, high, true); }

void imp_end()
{
    ImGui::Render();
    ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
    kept = true;
}

int imp_frame(int wide, int high, void (*build)(void*), void* context)
{
    // (kept from the first such frame on: a program that makes a frame of ImGui's every time copies nothing more)
    if (!kept_asked) ImGui_ImplShaderEmu_KeepFrames(true), kept_asked = true;
    const ImVec2 size((float)wide, (float)high);
    if (!imp_busy() && kept && ImGui::GetIO().DisplaySize.x == size.x && ImGui::GetIO().DisplaySize.y == size.y &&
        ImGui_ImplShaderEmu_RenderAgain())
        return 0;
    begin(wide, high, false);
    build(context);
    imp_end();
    return 1;
}
