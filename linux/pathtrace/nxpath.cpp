// nxpath: a path tracer of a Cornell box, its tiles traced by every worker core the machine
// has, with its settings in a Dear ImGui panel (docs/raytrace.md, docs/multicore.md,
// docs/imgui.md). Core 0 traces nothing: it draws the panel and the picture as it clears,
// and hands a tile to each core that has none.
//
// For tests: NXPATH_AUTO=1 presses Render at the start, NXPATH_EXIT=1 leaves when the picture
// is done, NXPATH_SAMPLES, NXPATH_BOUNCES, NXPATH_SIZE=WxH and NXPATH_CORES=N (0 to 3: which
// of the panel's layouts of the cores) set the panel. Every picture's line (pathstat:) has its
// time, its rays and a sum of its pixels, which is the same whichever cores traced it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "imgui.h"
#include "imgui_impl_shaderemu.h"
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
#include "mcw.h"
#include "tracer.h"
extern char __DATA_BEGIN__[], _end[];
}

#define MOST_W 320
#define MOST_H 240

static unsigned now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

static void pass()
{
    __asm__ volatile(".word 0x0100000f");   // pause: the machine's pass ends
}

// The layouts of the cores the panel offers (the machine's geometry: docs/multicore.md).
static const char* const layout_names[] = {"15 of the smallest", "15 small", "7 large", "this core alone"};
static const char* const layout_notes[] = {"0.75 KB of stores a pass each", "1.5 KB each", "6 KB each, as a program's workers are", "no worker cores"};
static const unsigned char layouts[3][15] = {
    {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
    {6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 0, 0, 0, 0, 0},
};
static int workers = 0, layout_set = -1;

// The workers given back, the cores laid out, and as many taken as there are.
static void take_workers(int layout)
{
    if (layout == layout_set) return;
    mcw_close();
    workers = 0;
    layout_set = layout;
    if (layout >= 3) return;
    int n = 0;
    while (n < 15 && layouts[layout][n]) n++;
    mcw_shape(layouts[layout], n);   // (a machine that cannot be laid out keeps the cores it has)
    workers = mcw_open(n);
    if (workers) mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
}

int main()
{
    if (GrOpen() < 0) {
        fprintf(stderr, "nxpath: no Nano-X server (start one: nano-X -p &)\n");
        return 1;
    }
    GR_WINDOW_ID window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char*)"Path tracer", GR_ROOT_WINDOW_ID, -1, -1, 780, 520, 0);
    GrMapWindow(window);
    if (seglInit(window) < 0) {
        fprintf(stderr, "nxpath: this machine has no GPU\n");
        return 1;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplShaderEmu_Init(window)) {
        fprintf(stderr, "nxpath: no GPU memory for the font\n");
        return 1;
    }
    // the picture: a texture where it lies, in GPU memory, and the samples' sums in memory
    // that begins on 16 bytes (a worker's own to write, tile by tile)
    pt.pixels = (uint32_t*)seglMemory(MOST_W * MOST_H * 4);
    pt.sums = (float*)mcw_alloc(MOST_W * MOST_H * 3 * sizeof(float));
    if (!pt.pixels || !pt.sums) {
        fprintf(stderr, "nxpath: no memory for the picture\n");
        return 1;
    }
    memset(pt.pixels, 0, MOST_W * MOST_H * 4);
    mcw_touch(pt.sums, MOST_W * MOST_H * 3 * sizeof(float));
    GLuint texture;
    glGenTextures(1, &texture);
    atexit(mcw_close);

    // the panel's settings
    static const int sizes[3][2] = {{160, 120}, {240, 180}, {320, 240}};
    static const char* const size_names[] = {"160 x 120", "240 x 180", "320 x 240"};
    static const char* const materials[] = {"matt", "mirror", "glass"};
    int size = 0, samples = 16, bounces = 5, layout = 0, ball[2] = {PT_MIRROR, PT_GLASS};
    float light = 12.0f;
    bool owners = false, leave_when_done = getenv("NXPATH_EXIT") != nullptr, press = getenv("NXPATH_AUTO") != nullptr;
    const char* text;
    if ((text = getenv("NXPATH_SAMPLES")) != nullptr) samples = atoi(text);
    if ((text = getenv("NXPATH_BOUNCES")) != nullptr) bounces = atoi(text);
    if ((text = getenv("NXPATH_CORES")) != nullptr) layout = atoi(text) & 3;
    if ((text = getenv("NXPATH_SIZE")) != nullptr)
        for (int i = 0; i < 3; i++)
            if (atoi(text) == sizes[i][0]) size = i;

    // the picture being traced
    bool tracing = false, drawn_once = false;
    int tiles = 0, sample = 0, next = 0, out = 0, busy[16], pictures = 0, shown_w = 160, shown_h = 120, of_samples = 0;
    unsigned began = 0, took = 0, rays = 0, drew = 0;
    for (int k = 0; k < 16; k++) busy[k] = -1;

    for (;;) {
        // ---- the tiles: every core that has none gets the next ----
        bool idle = true;
        if (tracing) {
            if (workers == 0 && next < tiles) {
                rays += pt_tile((uint32_t)next++, (uint32_t)sample);   // this core alone: a tile, then the panel
                idle = false;
            }
            for (int k = 1; k <= workers; k++) {
                if (busy[k] >= 0 && mcw_done(k)) {
                    rays += mcw_result(k);
                    busy[k] = -1;
                    out--;
                }
                if (busy[k] < 0 && next < tiles) {
                    busy[k] = next;
                    mcw_post(k, pt_tile, (uint32_t)next++, (uint32_t)sample | (uint32_t)k << 24);
                    out++;
                    idle = false;
                }
            }
            if (next >= tiles && out == 0) {
                // a sample of every pixel is in: the next, or the picture is done
                took = now_ms() - began;
                if (++sample >= of_samples) {
                    unsigned sum = 0;
                    for (int i = 0; i < pt.width * pt.height; i++) sum = sum * 31 + pt.pixels[i];
                    tracing = false;
                    fprintf(stderr, "pathstat: picture %d, %d x %d, %d samples, %d turns: %u ms with %d workers (%s), %u rays, %u thousand rays a second, sum %08x\n",
                            pictures++, pt.width, pt.height, of_samples, pt.bounces, took, workers, layout_names[layout_set < 0 ? 3 : layout_set], rays,
                            took ? rays / took : 0, sum);
                    if (leave_when_done) break;
                } else {
                    next = 0;
                }
                idle = false;
            }
        }
        // ---- the panel, ten times a second (it is 200 thousand instructions of this core) ----
        unsigned now = now_ms();
        if (drawn_once && now - drew < (tracing ? 150u : 50u)) {
            if (idle) pass();
            continue;
        }
        drew = now;
        drawn_once = true;
        if (!ImGui_ImplShaderEmu_NewFrame()) break;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(270, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Path tracer");
        ImGui::TextWrapped("A Cornell box, traced by every worker core of the machine.");
        ImGui::Separator();
        ImGui::BeginDisabled(tracing);
        ImGui::Combo("picture", &size, size_names, 3);
        ImGui::SliderInt("samples", &samples, 1, 256, "%d a pixel", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("turns", &bounces, 1, 8, "%d of a path");
        ImGui::SliderFloat("lamp", &light, 2.0f, 30.0f, "%.0f");
        ImGui::Combo("left ball", &ball[0], materials, 3);
        ImGui::Combo("right ball", &ball[1], materials, 3);
        ImGui::Checkbox("show which core traced what", &owners);
        ImGui::Separator();
        ImGui::TextUnformatted("The cores:");
        for (int i = 0; i < 4; i++) {
            ImGui::RadioButton(layout_names[i], &layout, i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", layout_notes[i]);
        }
        ImGui::TextDisabled("%s", layout_notes[layout]);
        ImGui::EndDisabled();
        ImGui::Separator();
        bool render = ImGui::Button(tracing ? "Tracing..." : "Render", ImVec2(120, 28)) && !tracing;
        ImGui::SameLine();
        bool stop = ImGui::Button("Stop", ImVec2(60, 28)) && tracing;
        if (tracing || pictures) {
            int all = tiles * (of_samples > 0 ? of_samples : 1), have = tracing ? sample * tiles + next - out : all;
            ImGui::ProgressBar(all ? (float)have / (float)all : 0.0f, ImVec2(-1, 0));
            unsigned ms = tracing ? now - began : took;
            ImGui::Text("%d of %d samples, %u.%u s", tracing ? sample : of_samples, of_samples, ms / 1000, ms % 1000 / 100);
            ImGui::Text("%u,%03u rays, %u thousand a second", rays / 1000, rays % 1000, ms ? rays / ms : 0);
            ImGui::Text("%d worker cores", workers);
        }
        bool quit = ImGui::Button("Quit");
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(286, 8), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(486, 500), ImGuiCond_FirstUseEver);
        ImGui::Begin("Picture");
        {
            // as large as fits
            ImVec2 room = ImGui::GetContentRegionAvail();
            float by = room.x / (float)shown_w < room.y / (float)shown_h ? room.x / (float)shown_w : room.y / (float)shown_h;
            if (by < 1.0f) by = 1.0f;
            if (pictures || tracing) ImGui::Image((ImTextureID)(intptr_t)texture, ImVec2((float)shown_w * by, (float)shown_h * by));
            else ImGui::TextUnformatted("Render traces it.");
        }
        ImGui::End();
        ImGui::Render();
        glViewport(0, 0, (GLsizei)io.DisplaySize.x, (GLsizei)io.DisplaySize.y);
        glClearColor(0.08f, 0.09f, 0.11f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
        seglSwap();
        if (quit) break;

        if (stop) {
            // the tiles still out are waited for (their cores must be free), and that is the picture
            for (int k = 1; k <= workers; k++)
                if (busy[k] >= 0) {
                    mcw_wait(k);
                    rays += mcw_result(k);
                    busy[k] = -1;
                }
            out = 0;
            took = now_ms() - began;
            of_samples = sample > 0 ? sample : 1;
            tracing = false;
            pictures++;
        }
        if (render || press) {
            press = false;
            take_workers(layout);
            pt.width = shown_w = sizes[size][0];
            pt.height = shown_h = sizes[size][1];
            pt.bounces = bounces;
            pt.material[0] = ball[0];
            pt.material[1] = ball[1];
            pt.owners = owners;
            pt.light = light;
            pt.seed = 0x9e3779b9u;
            glBindTexture(GL_TEXTURE_2D, texture);
            seglTexturePointer(pt.pixels, pt.width, pt.height, GL_RGBA);
            memset(pt.pixels, 0, (size_t)(pt.width * pt.height * 4));
            tiles = ((pt.width + PT_TILE - 1) / PT_TILE) * ((pt.height + PT_TILE - 1) / PT_TILE);
            of_samples = samples;
            sample = next = out = 0;
            rays = 0;
            began = now_ms();
            tracing = true;
        }
    }
    fprintf(stderr, "nxpath: done\n");
    GrClose();
    return 0;
}
