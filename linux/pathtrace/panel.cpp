// nxpath's window by Dear ImGui (panel.h; docs/apps/raytrace.md, "The window"): the menus, the panel, the line of
// figures and the handles, one frame of ImGui over the view OpenGL drew. What a control asks for is handed back
// in the frame's `asked`, for nxpath.c to do after the frame.
#include <dirent.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_shaderemu.h"
#include "imgui_panel.h"
extern "C" {
#include <nano-X.h>
#include "panel.h"
#include "scene.h"
}

static int shown_tab = -1;  // the tab the frame before showed, and the list's row
static int shown_row = -1;

// (the input, the frames wanted and a press's owner are imgui_panel.cpp's, as nxray's and nxphys's)
int panel_init(unsigned int window) { return imp_init(window); }
int panel_bar() { return imp_line(); }
void panel_pointer(int x, int y) { imp_pointer(x, y); }
int panel_button(int button, int down) { return imp_button(button, down); }
int panel_press_given(int press, int* free) { return imp_press_given(press, free); }
void panel_wheel(int notches) { imp_wheel(notches); }
void panel_key(int ch, int modifiers, int down) { imp_key(ch, modifiers, down); }
int panel_wants_pointer() { return imp_wants_pointer(); }
int panel_busy() { return imp_busy(); }

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

// A control's name before it, and the control the rest of the line.
static const float NAMED = 62;
static void named(const char* name)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name);
    note(name);
    ImGui::SameLine(NAMED);
    ImGui::SetNextItemWidth(-1);
}

// What a control that edits one of the scene's numbers in place asks for: `id` while it changes (A_EDITED, the
// camera's A_CAMERA), A_EDIT_DONE when it is let go.
static void edited(bool changed, int id = A_EDITED)
{
    if (changed) ask(id);
    if (ImGui::IsItemDeactivatedAfterEdit()) ask(A_EDIT_DONE);
    note(nullptr);
}

// One of nxpath.c's sliders (samples, turns): steps, the value's words in it.
static void slider_line(int n, const char* name)
{
    char value[32];
    int v = path_slider(n, 0, 0);
    path_slider_text(n, value, sizeof value);
    named(name);
    ImGui::PushID(n);
    if (ImGui::SliderInt("##slider", &v, 0, path_slider_most(n), value, ImGuiSliderFlags_NoInput)) ask(A_SLIDER + n, v);
    if (ImGui::IsItemDeactivatedAfterEdit()) ask(A_SLIDE + n);
    note(value);
    ImGui::PopID();
}

static void nothing_selected()
{
    ImGui::TextWrapped("Nothing is selected: click a thing in the picture or in the list.");
    note(nullptr);
    ImGui::TextWrapped("Add has a plane, a box, a ball and a light; File imports a mesh.");
    note(nullptr);
}

static void object_tab(int o, int l)
{
    static const char* const what_names[3] = {"Move", "Turn", "Size"};
    if (o < 0 && l < 0) return nothing_selected();
    named("Name");
    if (ImGui::InputText("##name", o >= 0 ? sc.objects[o].name : sc.lights[l].name, SC_NAME)) ask(A_RENAMED);
    note(nullptr);
    if (o >= 0) {
        sc_object& ob = sc.objects[o];
        named("Handles");
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(what_names[i], path_what() == i)) ask(A_WHAT + i);
            note(what_names[i]);
        }
        head("Where it is");
        named("Place");
        edited(ImGui::DragFloat3("##place", ob.place, 0.01f, -1000.0f, 1000.0f, "%.2f"));
        named("Turn");
        edited(ImGui::DragFloat3("##turn", ob.turn, 0.5f, -360.0f, 360.0f, "%.0f"));
        named("Size");
        const float largest = fmaxf(ob.size[0], fmaxf(ob.size[1], ob.size[2]));
        edited(ImGui::DragFloat3("##size", ob.size, fmaxf(largest * 0.01f, 0.0001f), 0.0001f, 1000.0f, "%.3g", ImGuiSliderFlags_AlwaysClamp));
        if (ob.mesh >= 0 && ob.mesh < sc.mesh_count) {
            ImGui::TextDisabled("%s, %d triangles", sc.meshes[ob.mesh].name, sc.meshes[ob.mesh].triangles);
            note(nullptr);
        }
    } else {
        sc_light& li = sc.lights[l];
        head("Where it is");
        named("Place");
        edited(ImGui::DragFloat3("##place", li.place, 0.01f, -1000.0f, 1000.0f, "%.2f"));
        named("Radius");
        edited(ImGui::SliderFloat("##radius", &li.radius, 0.02f, 1.0f, "%.2f"));
        head("Its light");
        named("Power");
        edited(ImGui::SliderFloat("##power", &li.power, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic));
        named("Colour");
        edited(ImGui::ColorEdit3("##colour", li.colour, ImGuiColorEditFlags_PickerHueWheel));
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
    if (o < 0 && l < 0) return nothing_selected();
    if (o < 0) {
        ImGui::TextWrapped("A light has no material: its size, power and colour are under Light.");
        note(nullptr);
        return;
    }
    sc_object& ob = sc.objects[o];
    named("Kind");
    if (ImGui::BeginCombo("##kind", "a kind of thing...")) {
        for (int i = 0; i < 6; i++)
            if (ImGui::Selectable(kinds[i])) ask(A_MATERIAL + i);
        ImGui::EndCombo();
    }
    note(nullptr);
    named("Colour");
    edited(ImGui::ColorEdit3("##colour", ob.colour, ImGuiColorEditFlags_PickerHueWheel));
    head("What it is made of");
    named("Rough");
    edited(ImGui::SliderFloat("##rough", &ob.rough, 0.0f, 1.0f, "%.2f"));
    named("Metal");
    edited(ImGui::SliderFloat("##metal", &ob.metal, 0.0f, 1.0f, "%.2f"));
    named("Glass");
    edited(ImGui::SliderFloat("##glass", &ob.glass, 0.0f, 1.0f, "%.2f"));
    named("Index");
    edited(ImGui::SliderFloat("##ior", &ob.ior, 1.0f, 2.5f, "%.2f"));
    named("Density");
    edited(ImGui::SliderFloat("##density", &ob.density, 0.0f, 10.0f, "%.2f"));
    named("Glow");
    edited(ImGui::SliderFloat("##glow", &ob.glow, 0.0f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic));
    // how much its coat or glass mirrors, from straight on to a grazing look (Schlick's, by its index)
    head("Mirrored by the angle");
    float mirrored[32];
    const float r0 = ((ob.ior - 1) / (ob.ior + 1)) * ((ob.ior - 1) / (ob.ior + 1));
    for (int i = 0; i < 32; i++) {
        const float c = cosf((float)i / 31.0f * 1.5707963f), m = 1 - c, m5 = m * m * m * m * m;
        mirrored[i] = r0 + (1 - r0) * m5;
    }
    char words[40];
    snprintf(words, sizeof words, "%.0f%% straight on", r0 * 100.0f);
    ImGui::PlotLines("##mirrored", mirrored, 32, 0, words, 0.0f, 1.0f, ImVec2(-1, 44));
    note(nullptr);
}

static void render_tab()
{
    char text[80];
    int w, h;
    head("The picture");
    path_picture_size(&w, &h);
    snprintf(text, sizeof text, "%s, %d x %d", path_detail(-1), w, h);
    named("Detail");
    if (ImGui::BeginCombo("##detail", text)) {
        for (int i = 0; path_detail(i); i++)
            if (ImGui::Selectable(path_detail(i), i == path_detail_at())) ask(A_DETAIL, i);
        ImGui::EndCombo();
    }
    note(nullptr);
    slider_line(S_SAMPLES, "Samples");
    slider_line(S_TURNS, "Turns");
    bool live = path_live() != 0;
    if (ImGui::Checkbox("Traced when the view is still", &live)) ask(A_LIVE);
    note("Traced when the view is still");
    bool smooth = path_smooth() != 0;
    if (ImGui::Checkbox("Smoothed at few samples", &smooth)) ask(A_SMOOTH);
    note("Smoothed at few samples");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    const bool going = path_tracing() != 0, shown = path_showing() != 0;
    if (button(going ? "Stop" : "Render", half, 26, going)) ask(going ? A_STOP : A_RENDER);
    ImGui::SameLine();
    if (button(shown ? "Show the scene" : "Show the picture", half, 26)) ask(shown ? A_SCENE : A_PICTURE);
    // the rays a second as the samples went, the picture's pixels by their light, the scene's figures
    head("How it goes");
    const float* rates;
    const int n = path_rates(&rates);
    float most = 0;
    for (int i = 0; i < n; i++) most = rates[i] > most ? rates[i] : most;
    if (n) snprintf(text, sizeof text, "%.0f thousand rays a second", rates[n - 1]);
    else snprintf(text, sizeof text, "rays a second, as it traces");
    ImGui::PlotLines("##rates", rates, n, 0, text, 0.0f, most * 1.25f + 1.0f, ImVec2(-1, 52));
    note(nullptr);
    float bins[32];
    if (!path_light_shares(bins, 32, &most)) memset(bins, 0, sizeof bins), most = 0;
    ImGui::PlotHistogram("##light", bins, 32, 0, most > 0 ? "its pixels, dark to light" : "its pixels: no picture yet", 0.0f, most * 1.25f + 0.01f,
                         ImVec2(-1, 52));
    note(nullptr);
    path_figures(text, sizeof text);
    ImGui::TextDisabled("%s, %d workers", text, path_workers());
    note(nullptr);
}

// The camera and the background's light.
static void view_tab()
{
    sc_camera& c = sc.camera;
    head("The camera");
    named("Looks at");
    edited(ImGui::DragFloat3("##target", c.target, 0.01f, -1000.0f, 1000.0f, "%.2f"), A_CAMERA);
    named("Round");
    edited(ImGui::DragFloat("##turn", &c.turn, 0.5f, -3600.0f, 3600.0f, "%.0f degrees"), A_CAMERA);
    named("Up");
    edited(ImGui::SliderFloat("##tilt", &c.tilt, -89.0f, 89.0f, "%.0f degrees"), A_CAMERA);
    named("Away");
    edited(ImGui::DragFloat("##far", &c.far, 0.02f, 0.1f, 200.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic), A_CAMERA);
    named("Lens");
    edited(ImGui::SliderFloat("##lens", &c.lens, 0.1f, 2.0f, "%.2f"), A_CAMERA);
    if (button("The whole scene in view", -1)) ask(A_WHOLE);
    head("The background's light");
    named("Colour");
    edited(ImGui::ColorEdit3("##sky", sc.sky, ImGuiColorEditFlags_PickerHueWheel));
    named("Power");
    edited(ImGui::SliderFloat("##sky_power", &sc.sky_power, 0.0f, 2.0f, "%.2f"));
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
    imp_wake(3);
}

const char* panel_files_path() { return files.path; }
int panel_typing() { return imp_typing() || files.kind; }   // (the chooser has the keys: a name, Escape)

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
            static const char* const names[TABS] = {"Object", "Material", "Render", "View"};
            for (int k = 0; k < TABS; k++) {
                // (a tab chosen by the program, not by a click: as the panel says)
                const ImGuiTabItemFlags flags = tab != shown_tab && k == tab ? ImGuiTabItemFlags_SetSelected : 0;
                const char* name = k == TAB_OBJECT && o < 0 && l >= 0 ? "Light###Object" : names[k];
                if (ImGui::BeginTabItem(name, nullptr, flags)) {
                    if (k != tab && tab == shown_tab) ask(A_TAB + k);
                    if (k == TAB_OBJECT) object_tab(o, l);
                    else if (k == TAB_MATERIAL) material_tab(o, l);
                    else if (k == TAB_RENDER) render_tab();
                    else view_tab();
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
    frame = f;
    f->moved = 0, f->asked_count = 0, f->controls = f->over = f->cut = f->lowest = 0;
    imp_begin(f->wide, f->high);
    const int bar = panel_bar();
    if (f->handles) f->moved = gizmo_draw(f->view, f->projection, f->matrix, f->what, bar, f->view_w, f->view_h, f->drag);
    menus();
    panel(f, bar);
    status_line(f);
    files_window(f);
    imp_end();
}
