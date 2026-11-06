// nxray's panel by Dear ImGui (ray_panel.h): a bar under the picture of what the keys do, and a window over the
// picture with the figures: milliseconds a picture as they went, each worker's rays.
#include <stdio.h>
#include <string.h>
#include "imgui.h"
#include "imgui_panel.h"
extern "C" {
#include "ray_panel.h"
}

static int bar_height;

int ray_panel_init(unsigned int window)
{
    if (!imp_init(window)) return 0;
    imp_keep();     // (before nxray's pictures take the GPU's memory)
    // (two lines: the controls, and the figures in words)
    bar_height = imp_line() + imp_text_line() + 2 * (int)ImGui::GetStyle().WindowPadding.y;
    return 1;
}

int ray_panel_bar() { return bar_height; }

static ray_panel* now;
static void ask(int key)
{
    if (now->key_count < 8) now->keys[now->key_count++] = key;
}

static void bar(int wide, int high)
{
    ray_panel& p = *now;
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2(0, (float)(high - bar_height)));
    ImGui::SetNextWindowSize(ImVec2((float)wide, (float)bar_height));
    if (ImGui::Begin("##bar", nullptr, fixed)) {
        const float w = ImGui::GetContentRegionAvail().x;
        char text[48];
        // the picture's size
        snprintf(text, sizeof text, "%d x %d", p.width, p.height);
        ImGui::SetNextItemWidth(w * 0.17f);
        if (ImGui::BeginCombo("##size", text)) {
            for (int i = 0; i < p.sizes; i++) {
                snprintf(text, sizeof text, "%d x %d", p.size_list[i][0], p.size_list[i][1]);
                if (ImGui::Selectable(text, i == p.size_at)) p.size_asked = i;
            }
            ImGui::EndCombo();
        }
        // how often a ray is mirrored
        ImGui::SameLine();
        int mirrors = p.depth - 1;
        ImGui::SetNextItemWidth(w * 0.17f);
        if (ImGui::SliderInt("##mirrors", &mirrors, 0, p.depth_most - 1, mirrors == 1 ? "1 mirror" : mirrors ? "%d mirrors" : "no mirrors", ImGuiSliderFlags_NoInput)) {
            // (act's 'm' goes round: as many of them as reach the value)
            for (int d = p.depth; d != mirrors + 1; d = d % p.depth_most + 1) ask('m');
        }
        ImGui::SameLine();
        bool b = p.shadows != 0;
        if (ImGui::Checkbox("Shadows", &b)) ask('h');
        ImGui::SameLine();
        b = p.owners != 0;
        if (ImGui::Checkbox("Cores", &b)) ask('w');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("each share's first texels in its core's colour");
        ImGui::SameLine();
        if (ImGui::Button(p.stopped ? "Go" : "Stop", ImVec2(w * 0.08f, 0))) ask(' ');
        // the workers: none, one, ... all of them (the lanes' field where the machine has it)
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

static float took_at(void* data, int i) { return ((const float*)data)[i]; }

static void figures(int high)
{
    ray_panel& p = *now;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(236, 0), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.75f);
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_Once);
    if (ImGui::Begin("Figures", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        char text[64];
        float most = 0;
        for (int i = 0; i < p.took_count; i++) most = p.took[i] > most ? p.took[i] : most;
        if (p.took_count) snprintf(text, sizeof text, "%.0f ms the last picture", p.took[p.took_count - 1]);
        else snprintf(text, sizeof text, "ms a picture, as they come");
        ImGui::PlotLines("##took", took_at, (void*)p.took, p.took_count, 0, text, 0.0f, most * 1.25f + 1.0f, ImVec2(220, 50));
        if (p.field) {
            ImGui::TextDisabled("on the lanes' field: no worker's share");
        } else if (p.worker_count) {
            most = 0;
            for (int i = 0; i < p.worker_count; i++) most = p.worker_rays[i] > most ? p.worker_rays[i] : most;
            snprintf(text, sizeof text, "%d workers' rays", p.worker_count);
            ImGui::PlotHistogram("##rays", p.worker_rays, p.worker_count, 0, text, 0.0f, most * 1.15f + 0.01f, ImVec2(220, 50));
        } else {
            ImGui::TextDisabled("no workers: the first core traces it");
        }
    }
    ImGui::End();
    (void)high;
}

struct sizes { int wide, high; };
static void build(void* context)
{
    const sizes& s = *(const sizes*)context;
    bar(s.wide, s.high);
    figures(s.high - bar_height);
}

int ray_panel_put(ray_panel* p, int wide, int high)
{
    now = p;
    p->key_count = 0, p->size_asked = p->workers_asked = -1;
    sizes s = {wide, high};
    return imp_frame(wide, high, build, &s);
}
