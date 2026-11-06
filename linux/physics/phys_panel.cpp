// nxphys's panel by Dear ImGui (phys_panel.h): a bar under the picture of what the keys do, and a window over the
// picture with the gravity, the heap's boxes, and the figures as they went: substeps a second, the boxes' energy, their speeds.
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "imgui_panel.h"
extern "C" {
#include "phys_panel.h"
}

static int bar_height;

int phys_panel_init(unsigned int window)
{
    if (!imp_init(window)) return 0;
    imp_keep();
    bar_height = imp_line() + imp_text_line() + 2 * (int)ImGui::GetStyle().WindowPadding.y;
    return 1;
}

int phys_panel_bar() { return bar_height; }

static phys_panel* now;
static void ask(int key)
{
    if (now->key_count < 8) now->keys[now->key_count++] = key;
}

static void bar(int wide, int high)
{
    phys_panel& p = *now;
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2(0, (float)(high - bar_height)));
    ImGui::SetNextWindowSize(ImVec2((float)wide, (float)bar_height));
    if (ImGui::Begin("##bar", nullptr, fixed)) {
        static const char* const scenes[3] = {"A wall", "Towers", "A heap"};
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w * 0.16f);
        if (ImGui::BeginCombo("##scene", p.scene >= 1 && p.scene <= 3 ? scenes[p.scene - 1] : "A test's")) {
            for (int i = 0; i < 3; i++)
                if (ImGui::Selectable(scenes[i], p.scene == i + 1)) ask('1' + i);
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Again", ImVec2(w * 0.09f, 0))) ask('r');
        ImGui::SameLine();
        if (ImGui::Button("Throw", ImVec2(w * 0.09f, 0))) ask(' ');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("the heavy box thrown at them");
        ImGui::SameLine();
        if (ImGui::Button(p.stopped ? "Go" : "Stop", ImVec2(w * 0.08f, 0))) ask('p');
        ImGui::SameLine();
        bool b = p.owners != 0;
        if (ImGui::Checkbox("Cores", &b)) ask('c');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("each box in its core's colour");
        // the workers: none, one, ... all of them
        ImGui::SameLine();
        static int wanted = -1;
        static bool dragging;
        if (!dragging) wanted = p.wanted;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const char* format = wanted >= p.most_workers ? "all the workers" : wanted == 1 ? "one worker" : wanted ? "%d workers" : "no workers";
        if (ImGui::SliderInt("##workers", &wanted, 0, p.most_workers, format)) dragging = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) p.workers_asked = wanted, dragging = false;
        else if (!ImGui::IsItemActive()) dragging = false;
        ImGui::TextUnformatted(p.status);
    }
    ImGui::End();
}

static void boxes()
{
    phys_panel& p = *now;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(236, 0), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("The boxes", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        const float label = 60, width = 220 - label;
        char text[64];
        // the gravity, given to the boxes at the next substep they all wait at
        static float gravity;
        static bool dragging;
        if (!dragging) gravity = p.gravity;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Gravity");
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderFloat("##gravity", &gravity, 0.0f, 20.0f, "%.2f m/s2")) dragging = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) p.gravity_asked = gravity, dragging = false;
        else if (!ImGui::IsItemActive()) dragging = false;
        // the heap's boxes, a new heap when let go
        static int heap;
        static bool heaping;
        if (!heaping) heap = p.heap_boxes;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Heap");
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("##heap", &heap, 2, p.most_boxes, "%d boxes", ImGuiSliderFlags_Logarithmic)) heaping = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) p.heap_asked = heap, heaping = false;
        else if (!ImGui::IsItemActive()) heaping = false;
        // what they did, a second a point
        float most = 0;
        for (int i = 0; i < p.figure_count; i++) most = p.rates[i] > most ? p.rates[i] : most;
        if (p.figure_count) snprintf(text, sizeof text, "%.0f substeps a second", p.rates[p.figure_count - 1]);
        else snprintf(text, sizeof text, "substeps a second, as they go");
        ImGui::PlotLines("##rates", p.rates, p.figure_count, 0, text, 0.0f, most * 1.25f + 1.0f, ImVec2(220, 44));
        most = 0;
        for (int i = 0; i < p.figure_count; i++) most = p.energy[i] > most ? p.energy[i] : most;
        if (p.figure_count) snprintf(text, sizeof text, "%.0f J moving", p.energy[p.figure_count - 1]);
        else snprintf(text, sizeof text, "their energy");
        ImGui::PlotLines("##energy", p.energy, p.figure_count, 0, text, 0.0f, most * 1.25f + 1.0f, ImVec2(220, 44));
        most = 0;
        for (int i = 0; i < PHYS_SPEEDS; i++) most = p.speeds[i] > most ? p.speeds[i] : most;
        snprintf(text, sizeof text, "%d boxes, 0 to %.0f m/s", p.boxes, p.speed_most);
        ImGui::PlotHistogram("##speeds", p.speeds, PHYS_SPEEDS, 0, text, 0.0f, most * 1.15f + 1.0f, ImVec2(220, 44));
    }
    ImGui::End();
}

struct sizes { int wide, high; };
static void build(void* context)
{
    const sizes& s = *(const sizes*)context;
    bar(s.wide, s.high);
    boxes();
}

int phys_panel_put(phys_panel* p, int wide, int high)
{
    now = p;
    p->key_count = 0, p->workers_asked = p->heap_asked = -1, p->gravity_asked = -1;
    sizes s = {wide, high};
    return imp_frame(wide, high, build, &s);
}
