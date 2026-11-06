// nxpath's window by Dear ImGui (panel.h; docs/apps/raytrace.md, "The window"): the menus, the panel, the line of
// figures and the handles, one frame of ImGui over the view OpenGL drew. What a control asks for is handed back
// in the frame's `asked`, for nxpath.c to do after the frame.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_shaderemu.h"
extern "C" {
#include <nano-X.h>
#include "panel.h"
}

static int frames_left;     // frames wanted after an input: a click and a release, a hover settling
static unsigned last_ms;
static int shown_tab = -1;  // the tab the frame before showed, and the list's row
static int shown_row = -1;

static unsigned now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

int panel_init(unsigned int window)
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

int panel_bar() { return (int)ImGui::GetFrameHeight(); }

// A button's change waits until a frame has had the pointer where it is: a tab, which other items may lie over, takes
// a click only where the pointer was the frame before, and a scripted click (nxkey) moves and presses at once.
static int seen;                // frames since the pointer moved
static int pending[8], pending_count;
static float pointer_x = -1, pointer_y = -1;

void panel_pointer(int x, int y)
{
    if ((float)x == pointer_x && (float)y == pointer_y) return;
    pointer_x = (float)x, pointer_y = (float)y, seen = 0;
    ImGui::GetIO().AddMousePosEvent(pointer_x, pointer_y), frames_left = 2;
}

// Presses queued and given to ImGui, and whether the last given found nothing of ImGui's under the pointer.
static int downs_queued, downs_given;
static bool down_free;

int panel_button(int button, int down)
{
    if (pending_count < 8) pending[pending_count++] = button << 1 | (down != 0), downs_queued += down != 0;
    frames_left = 3;
    return downs_queued;
}

int panel_press_given(int press, int* free)
{
    *free = down_free;
    return downs_given >= press;
}
void panel_wheel(int notches) { ImGui::GetIO().AddMouseWheelEvent(0, (float)notches), frames_left = 2; }

void panel_key(int ch, int modifiers, int down)
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
    frames_left = 2;
}

int panel_wants_pointer() { return ImGui::GetIO().WantCaptureMouse; }
int panel_busy() { return frames_left > 0 || pending_count > 0 || ImGui::GetCurrentContext()->InputEventsQueue.Size > 0; }

// ---- a frame ----

static panel_frame* frame;

static void ask(int id, int value = 0)
{
    if (frame->asked_count < 16) frame->values[frame->asked_count] = value, frame->asked[frame->asked_count++] = id;
}

// For a test (NXPATH_PRINT): the panel's controls, those over another, those whose words are wider than they are,
// and where the lowest ends (as the panel of the window system's had it counted).
#define NOTED 128
static ImRect noted[NOTED];
static void note(const char* words)
{
    const ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (words && ImGui::CalcTextSize(words, nullptr, true).x > r.GetWidth()) frame->cut++;
    for (int i = 0; i < frame->controls && i < NOTED; i++)
        frame->over += r.Min.x < noted[i].Max.x && noted[i].Min.x < r.Max.x && r.Min.y < noted[i].Max.y && noted[i].Min.y < r.Max.y;
    if (frame->controls < NOTED) noted[frame->controls] = r;
    frame->controls++;
    if ((int)r.Max.y > frame->lowest) frame->lowest = (int)r.Max.y;
}

static bool button(const char* words, float w, float h = 0, bool pressed = false)
{
    if (pressed) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(words, ImVec2(w, h));
    if (pressed) ImGui::PopStyleColor();
    note(words);
    return clicked;
}

static void head(const char* words)
{
    ImGui::SeparatorText(words);
    note(nullptr);
}

// A slider with its name before it and its value in it, as the panel's lines had them.
static void slider_line(int n, const char* name)
{
    char value[32], format[40];
    int v = path_slider(n, 0, 0);
    if (v < 0) return;
    path_slider_text(n, value, sizeof value);
    // (the value is the slider's words: a '%' in them is written twice)
    int k = 0;
    for (const char* c = value; *c && k < (int)sizeof format - 2; c++) {
        if (*c == '%') format[k++] = '%';
        format[k++] = *c;
    }
    format[k] = 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    note(name);
    ImGui::SameLine(58);
    ImGui::SetNextItemWidth(-1);
    ImGui::PushID(n);
    if (ImGui::SliderInt("##slider", &v, 0, path_slider_most(n), format)) ask(A_SLIDER + n, v);
    if (ImGui::IsItemDeactivatedAfterEdit()) ask(A_SLIDE + n);
    note(value);
    ImGui::PopID();
}

static void swatch_head(const char* words)
{
    // (its words and the colour beside them, then the line: the swatch is not over the heading's line)
    const float* c = path_colour();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(words);
    note(words);
    if (c) {
        ImGui::SameLine(right - 26);
        ImGui::ColorButton("##colour", ImVec4(c[0], c[1], c[2], 1), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(26, 16));
        note(nullptr);
    }
    ImGui::Separator();
}

static void object_tab(int o, int l)
{
    static const char* const lines[3] = {"Place", "Turn", "Size"};
    static const char* const what_names[3] = {"Move", "Turn", "Size"};
    const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    if (o < 0 && l < 0) {
        ImGui::TextWrapped("Nothing is selected: click a thing in the picture or in the list.");
        note(nullptr);
        ImGui::TextWrapped("Add has a plane, a box, a ball and a light; File imports a mesh.");
        note(nullptr);
        return;
    }
    head(path_row(path_selected()) + (l >= 0 ? 2 : 0));
    if (o >= 0) {
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            if (button(what_names[i], third, 0, path_what() == i)) ask(A_WHAT + i);
        }
    }
    // the three numbers of each line with a button down and up
    const float x0 = 46, step = (ImGui::GetContentRegionAvail().x - x0) / 3;
    ImGui::Dummy(ImVec2(0, 0));
    for (int k = 0; k < 3; k++) {
        ImGui::SameLine(x0 + k * step + step / 2 - 3);
        ImGui::TextUnformatted(k == 0 ? "x" : k == 1 ? "y" : "z");
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));    // (narrow buttons: a number between each two)
    for (int i = 0; i < (o >= 0 ? 3 : 1); i++) {
        float st, *v = path_line(i, &st);
        if (!v) continue;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(lines[i]);
        note(lines[i]);
        for (int k = 0; k < 3; k++) {
            char text[24];
            const float f = v[k] < 0 ? -v[k] : v[k];
            int whole = (int)f, part = (int)((f - (float)whole) * 100.0f + 0.5f);
            if (part == 100) whole++, part = 0;
            snprintf(text, sizeof text, "%s%d.%02d", v[k] < 0 ? "-" : "", whole, part);
            ImGui::PushID(i * 3 + k);
            ImGui::SameLine(x0 + k * step);
            if (ImGui::SmallButton("-")) ask(A_LESS + i * 3 + k);
            note("-");
            const float w = ImGui::CalcTextSize(text).x;
            ImGui::SameLine(x0 + k * step + (step - w) / 2);
            ImGui::TextUnformatted(text);
            note(nullptr);
            ImGui::SameLine(x0 + (k + 1) * step - 13);
            if (ImGui::SmallButton("+")) ask(A_MORE + i * 3 + k);
            note("+");
            ImGui::PopID();
        }
    }
    ImGui::PopStyleVar();
    if (l >= 0) {
        slider_line(S_RADIUS, "Radius");
        slider_line(S_POWER, "Power");
        swatch_head("Colour");
        slider_line(S_RED, "Red");
        slider_line(S_GREEN, "Green");
        slider_line(S_BLUE, "Blue");
    }
    ImGui::Spacing();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    if (button("Duplicate", half)) ask(A_DUPLICATE);
    ImGui::SameLine();
    if (button("Delete", half)) ask(A_DELETE);
}

static void material_tab(int o, int l)
{
    static const char* const kinds[6] = {"Matt", "Plastic", "Metal", "Mirror", "Glass", "Glow"};
    static const char* const made[6] = {"Rough", "Metal", "Glass", "Index", "Density", "Glow"};
    if (o < 0 && l < 0) {
        ImGui::TextWrapped("Nothing is selected: click a thing in the picture or in the list.");
        note(nullptr);
        return;
    }
    head(path_row(path_selected()) + (l >= 0 ? 2 : 0));
    if (o < 0) {
        ImGui::TextWrapped("A light has no material: its size, power and colour are under Light.");
        note(nullptr);
        return;
    }
    const float third = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3;
    for (int i = 0; i < 6; i++) {
        if (i % 3) ImGui::SameLine();
        if (button(kinds[i], third)) ask(A_MATERIAL + i);
    }
    swatch_head("Colour");
    slider_line(S_RED, "Red");
    slider_line(S_GREEN, "Green");
    slider_line(S_BLUE, "Blue");
    head("What it is made of");
    for (int i = 0; i < 6; i++) slider_line(S_ROUGH + i, made[i]);
}

static void render_tab()
{
    char text[40];
    int w, h;
    head("The picture");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Detail");
    note("Detail");
    ImGui::SameLine(58);
    if (button(path_detail(), 86)) ask(A_DETAIL);
    path_picture_size(&w, &h);
    snprintf(text, sizeof text, "%d x %d", w, h);
    ImGui::SameLine();
    ImGui::TextUnformatted(text);
    note(text);
    slider_line(S_SAMPLES, "Samples");
    slider_line(S_TURNS, "Turns");
    head("The background's light");
    slider_line(S_SKY_RED, "Red");
    slider_line(S_SKY_GREEN, "Green");
    slider_line(S_SKY_BLUE, "Blue");
    slider_line(S_SKY, "Power");
    ImGui::Spacing();
    bool live = path_live() != 0;
    if (ImGui::Checkbox("Traced when the view is still", &live)) ask(A_LIVE);
    note("Traced when the view is still");
    bool smooth = path_smooth() != 0;
    if (ImGui::Checkbox("Smoothed while it has few samples", &smooth)) ask(A_SMOOTH);
    note("Smoothed while it has few samples");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    const bool going = path_tracing() != 0, shown = path_showing() != 0;
    if (button(going ? "Stop" : "Render", half, 26, going)) ask(going ? A_STOP : A_RENDER);
    ImGui::SameLine();
    if (button(shown ? "Show the scene" : "Show the picture", half, 26)) ask(shown ? A_SCENE : A_PICTURE);
    if (button("Export...", half)) ask(A_EXPORT);
}

// ---- a file chosen (panel_files): ImGui's window over the panel, in place of the window system's dialogs ----

#define ENTRIES 512
struct entry { char name[64]; bool folder; };
static struct {
    int kind;                   // the action that asked, 0: none open
    bool opening, write;        // (OpenPopup at the next frame) a file to be written, with a name typed
    char title[64], folder[256], endings[32], name[128], path[400];
    entry entries[ENTRIES];
    int count, chosen;
} files;

static bool wanted(const char* name)
{
    // (an ending of `endings`, as ".obj .ply")
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    const size_t n = strlen(dot);
    for (const char* e = files.endings; *e;) {
        const size_t k = strcspn(e, " ");
        if (k == n && !strncasecmp(e, dot, n)) return true;
        e += k + (e[k] == ' ');
    }
    return false;
}

static int by_name(const void* a, const void* b)
{
    const entry *x = (const entry*)a, *y = (const entry*)b;
    return x->folder != y->folder ? (x->folder ? -1 : 1) : strcasecmp(x->name, y->name);
}

// The folder's folders and wanted files, the folders first, each by name.
static void list_folder()
{
    files.count = 0, files.chosen = -1;
    DIR* dir = opendir(files.folder);
    if (!dir) return;
    while (struct dirent* d = readdir(dir)) {
        if (d->d_name[0] == '.' || files.count == ENTRIES || strlen(d->d_name) >= sizeof files.entries[0].name) continue;
        char path[400];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", strcmp(files.folder, "/") ? files.folder : "", d->d_name);
        const bool folder = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        if (!folder && !wanted(d->d_name)) continue;
        entry& e = files.entries[files.count++];
        snprintf(e.name, sizeof e.name, "%s", d->d_name);
        e.folder = folder;
    }
    closedir(dir);
    qsort(files.entries, (size_t)files.count, sizeof(entry), by_name);
}

void panel_files(int kind, const char* title, const char* folder, const char* endings, const char* name)
{
    files.kind = kind, files.opening = true, files.write = name != nullptr;
    snprintf(files.title, sizeof files.title, "%s", title);
    snprintf(files.folder, sizeof files.folder, "%s", folder && *folder ? folder : "/");
    snprintf(files.endings, sizeof files.endings, "%s", endings);
    snprintf(files.name, sizeof files.name, "%s", name ? name : "");
    list_folder();
    frames_left = 3;
}

const char* panel_files_path() { return files.path; }
int panel_typing() { return ImGui::GetIO().WantTextInput || files.kind; }   // (the chooser has the keys: a name, Escape)

static void files_into(const char* name)
{
    char folder[256];
    if (!strcmp(name, "..")) {
        char* slash = strrchr(files.folder, '/');
        if (!slash) return;
        snprintf(folder, sizeof folder, "%.*s", slash == files.folder ? 1 : (int)(slash - files.folder), files.folder);
    } else
        snprintf(folder, sizeof folder, "%s/%s", strcmp(files.folder, "/") ? files.folder : "", name);
    snprintf(files.folder, sizeof files.folder, "%s", folder);
    list_folder();
}

static void files_window(const panel_frame* f)
{
    if (!files.kind) return;
    char id[80];
    snprintf(id, sizeof id, "%s###files", files.title);
    if (files.opening) ImGui::OpenPopup(id), files.opening = false;
    ImGui::SetNextWindowSize(ImVec2(420, 330));
    ImGui::SetNextWindowPos(ImVec2((float)f->wide * 0.5f, (float)f->high * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        files.kind = 0;     // (closed by ImGui)
        return;
    }
    bool done = false, cancel = false;
    ImGui::TextUnformatted(files.folder);
    ImGui::SameLine(ImGui::GetWindowWidth() - 46);
    if (ImGui::Button("Up") && strcmp(files.folder, "/")) files_into("..");
    if (ImGui::BeginChild("##entries", ImVec2(0, files.write ? -56 : -30), ImGuiChildFlags_Borders)) {
        for (int i = 0; i < files.count; i++) {
            char words[80];
            snprintf(words, sizeof words, "%s%s", files.entries[i].name, files.entries[i].folder ? "/" : "");
            ImGui::PushID(i);
            if (ImGui::Selectable(words, i == files.chosen, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (files.entries[i].folder) {
                    char name[64];
                    snprintf(name, sizeof name, "%s", files.entries[i].name);
                    ImGui::PopID();
                    files_into(name);
                    break;
                }
                files.chosen = i;
                snprintf(files.name, sizeof files.name, "%s", files.entries[i].name);
                done = ImGui::IsMouseDoubleClicked(0);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (files.write) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", files.name, sizeof files.name, ImGuiInputTextFlags_EnterReturnsTrue)) done = true;
    }
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    ImGui::BeginDisabled(!files.name[0]);
    if (ImGui::Button(files.write ? "Save" : "Open", ImVec2(half, 0))) done = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(half, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) cancel = true;
    if (done && files.name[0]) {
        snprintf(files.path, sizeof files.path, "%s/%s", strcmp(files.folder, "/") ? files.folder : "", files.name);
        ask(A_FILES, files.kind);
    }
    if ((done && files.name[0]) || cancel) ImGui::CloseCurrentPopup(), files.kind = 0;
    ImGui::EndPopup();
}

static void item(const char* words, const char* keys, int id, bool checked = false)
{
    if (ImGui::MenuItem(words, keys, checked, !path_greyed(id))) ask(id);
}

static void menus()
{
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        item("Open a scene...", nullptr, A_OPEN);
        item("Save the scene...", nullptr, A_SAVE);
        if (ImGui::BeginMenu("Examples")) {
            for (int i = 0; path_example(i); i++) item(path_example(i), nullptr, A_EXAMPLE + i);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        item("Import a mesh...", nullptr, A_IMPORT);
        item("Export the picture...", nullptr, A_EXPORT);
        ImGui::Separator();
        item("Close", nullptr, A_CLOSE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        item("Plane", nullptr, A_PLANE);
        item("Box", nullptr, A_BOX);
        item("Ball", nullptr, A_BALL);
        item("Light", nullptr, A_LIGHT);
        ImGui::Separator();
        item("Duplicate", "Ctrl+D", A_DUPLICATE);
        item("Delete", "Del", A_DELETE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        item("The scene", nullptr, A_SCENE, !path_showing());
        item("The picture", nullptr, A_PICTURE, path_showing() != 0);
        ImGui::Separator();
        item("The whole scene in view", "Home", A_WHOLE);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Render")) {
        item("Render", "F5", A_RENDER);
        item("Stop", "Esc", A_STOP);
        ImGui::Separator();
        item("Traced when the view is still", "L", A_LIVE, path_live() != 0);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

static void panel(const panel_frame* f, int bar)
{
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2((float)(f->wide - PANEL), (float)bar));
    ImGui::SetNextWindowSize(ImVec2((float)PANEL, (float)(f->high - bar - STATUS)));
    if (ImGui::Begin("##panel", nullptr, fixed)) {
        head("The scene");
        const int rows = path_rows(), selected = path_selected();
        if (ImGui::BeginListBox("##scene", ImVec2(-1, ROWS * ImGui::GetTextLineHeightWithSpacing() + 4))) {
            for (int r = 0; r < rows; r++) {
                ImGui::PushID(r);
                if (ImGui::Selectable(path_row(r), r == selected)) ask(A_CHOOSE + r);
                if (r == selected && selected != shown_row) ImGui::SetScrollHereY();     // (a thing picked in the view: its row seen)
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        shown_row = selected;
        note(nullptr);
        const int o = path_object(), l = path_light(), tab = path_tab();
        if (ImGui::BeginTabBar("##tabs")) {
            static const char* const names[3] = {"Object", "Material", "Render"};
            for (int k = 0; k < 3; k++) {
                // (a tab chosen by the program, not by a click: as the panel says)
                const ImGuiTabItemFlags flags = tab != shown_tab && k == tab ? ImGuiTabItemFlags_SetSelected : 0;
                const char* name = k == TAB_OBJECT && o < 0 && l >= 0 ? "Light###Object" : names[k];
                if (ImGui::BeginTabItem(name, nullptr, flags)) {
                    if (k != tab && tab == shown_tab) ask(A_TAB + k);
                    if (k == TAB_OBJECT) object_tab(o, l);
                    else if (k == TAB_MATERIAL) material_tab(o, l);
                    else render_tab();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        shown_tab = tab;
    }
    ImGui::End();
}

static void status_line(const panel_frame* f)
{
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2(0, (float)(f->high - STATUS)));
    ImGui::SetNextWindowSize(ImVec2((float)f->wide, (float)STATUS));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
    if (ImGui::Begin("##status", nullptr, fixed)) {
        char text[48];
        float fill;
        path_bar(text, sizeof text, &fill);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(at, ImVec2((float)(f->wide - PANEL), at.y + STATUS), true);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(path_status());
        ImGui::PopClipRect();
        ImGui::SameLine((float)(f->wide - PANEL + 6));
        ImGui::ProgressBar(fill, ImVec2(PANEL - 12, (float)(STATUS - 4)), text);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void panel_draw(panel_frame* f)
{
    ImGuiIO& io = ImGui::GetIO();
    const unsigned now = now_ms();
    frame = f;
    f->moved = 0, f->asked_count = 0, f->controls = f->over = f->cut = f->lowest = 0;
    io.DisplaySize = ImVec2((float)f->wide, (float)f->high);
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
    else if (!frames_left) settling = true, frames_left = 1;
    ImGui::NewFrame();
    // (a press given now: whether it is the view's, as ImGui saw the pointer with the popups open before it)
    if (downs) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        down_free = g.HoveredWindow == nullptr && g.OpenPopupStack.Size == 0;
        downs_given += downs;
    }
    const int bar = panel_bar();
    if (f->handles) f->moved = gizmo_draw(f->view, f->projection, f->matrix, f->what, bar, f->view_w, f->view_h, f->drag);
    menus();
    panel(f, bar);
    status_line(f);
    files_window(f);
    ImGui::Render();
    ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
}
