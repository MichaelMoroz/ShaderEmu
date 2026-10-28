// imdemo: a cube drawn with OpenGL and, over it, windows of Dear ImGui (docs/apps/imgui.md).
// IMDEMO_FRAMES=N ends after N frames, IMDEMO_SHOW=1 starts with ImGui's own demo window open,
// IMDEMO_SWAP=wait waits for each frame to be drawn before the next is begun. IMDEMO_HOLD=1
// draws a picture that the clock and the counts have no part in, for comparing two builds,
// and IMDEMO_SCROLL=N makes the demo window low and keeps it N pixels down its list (rows cut
// by its edges).
#include <stdio.h>
#include <stdlib.h>
#include "imgui.h"
#include "imgui_impl_shaderemu.h"
extern "C" {
#include <GLES/segl.h>
#include <nano-X.h>
}

#define HISTORY 90

// The processor's count of instructions run.
static unsigned instructions()
{
    unsigned n;
    __asm__ volatile("rdcycle %0" : "=r"(n));
    return n;
}

// A cube's six faces, four corners each, and a colour a face.
static const GLfloat cube[6][4][3] = {
    {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},     {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
    {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},     {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
    {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},     {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
};
static const GLfloat faces[6][3] = {{0.9f, 0.3f, 0.3f}, {0.3f, 0.8f, 0.4f}, {0.3f, 0.5f, 0.9f},
                                    {0.9f, 0.8f, 0.3f}, {0.8f, 0.4f, 0.9f}, {0.3f, 0.8f, 0.8f}};

static void draw_cube(float turn, float size, const float* background)
{
    int width, height;
    seglSize(&width, &height);
    float aspect = (float)width / (float)(height > 0 ? height : 1);
    glViewport(0, 0, width, height);
    glClearColor(background[0], background[1], background[2], 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustumf(-0.5f * aspect, 0.5f * aspect, -0.5f, 0.5f, 1, 20);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, 0, -5);
    glRotatef(turn, 0, 1, 0);
    glRotatef(turn * 0.7f, 1, 0, 0);
    glScalef(size, size, size);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, cube);
    for (int face = 0; face < 6; face++) {
        glColor4f(faces[face][0], faces[face][1], faces[face][2], 1);
        glDrawArrays(GL_QUADS, face * 4, 4);
    }
}

int main()
{
    if (GrOpen() < 0) {
        fprintf(stderr, "imdemo: no Nano-X server (start one: nano-X -p &)\n");
        return 1;
    }
    GR_WINDOW_ID window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char*)"Dear ImGui", GR_ROOT_WINDOW_ID, -1, -1, 640, 440, 0);
    GrMapWindow(window);
    if (seglInit(window) < 0) {
        fprintf(stderr, "imdemo: this machine has no GPU\n");
        return 1;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // the disk is a ROM: nothing to remember a layout in
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplShaderEmu_Init(window)) {
        fprintf(stderr, "imdemo: no GPU memory for the font\n");
        return 1;
    }

    float history[HISTORY] = {0}, background[3] = {0.10f, 0.12f, 0.16f}, speed = 40, size = 1, turn = 0;
    char text[64] = "type here";
    bool spin = true, show_demo = getenv("IMDEMO_SHOW") != nullptr;
    bool waits = getenv("IMDEMO_SWAP") != nullptr;   // IMDEMO_SWAP=wait: every list waited for, as it was
    bool held = getenv("IMDEMO_HOLD") != nullptr;
    int scrolled = getenv("IMDEMO_SCROLL") ? atoi(getenv("IMDEMO_SCROLL")) : 0;
    int oldest = 0;     // of the history, which goes round
    bool ahead = false;     // the frame made last is with the worker cores, and not shown yet
    int frames = 0, clicks = 0, most = getenv("IMDEMO_FRAMES") ? atoi(getenv("IMDEMO_FRAMES")) : 0;
    unsigned frame_start = instructions(), whole = 0, interface = 0, drawing = 0;

    // The frame before, when worker cores copy its triangles while this one is made (docs/apps/imgui.md,
    // "The copy on worker cores"): its commands, and it is shown, as soon as they have finished.
    unsigned finishing = 0;
    auto show = [&](bool wait) {
        if (!ahead || !(wait || ImGui_ImplShaderEmu_RenderReady())) return;
        unsigned from = instructions();
        ImGui_ImplShaderEmu_RenderFinish();
        if (waits) seglSwap();
        else seglSwapEarly();
        ahead = false;
        finishing += instructions() - from;
    };

    while (ImGui_ImplShaderEmu_NewFrame()) {
        unsigned interface_start = instructions();
        if (held) {
            turn = 30, spin = false;    // (the clock stays: keys repeat by it)
            whole = 290000, interface = 280000, drawing = 100000;
            for (int i = 0; i < HISTORY; i++) history[i] = (float)(250000 + (i * 7919) % 60000);
            oldest = 0;
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("ShaderEmu");
        ImGui::Text("Dear ImGui %s on a RISC-V", ImGui::GetVersion());
        ImGui::Text("computer in a pixel shader");
        ImGui::Separator();
        show(false);
        ImGui::Text("%.1f frames a second", held ? 10.0f : io.Framerate);
        ImGui::Text("a frame: %u,000 instructions", whole / 1000);
        ImGui::Text("ImGui's part: %u,000 (%d vertices)", interface / 1000, io.MetricsRenderVertices);
        ImGui::Text("of that, its triangles: %u,000", drawing / 1000);
        ImGui::PlotLines("##frame", history, HISTORY, oldest, "instructions a frame", 0, FLT_MAX, ImVec2(-1, 50));
        ImGui::Separator();
        show(false);
        ImGui::Checkbox("Spin the cube", &spin);
        ImGui::SliderFloat("speed", &speed, 0, 180, "%.0f degrees/s");
        ImGui::SliderFloat("size", &size, 0.2f, 2);
        ImGui::ColorEdit3("background", background);
        show(false);
        ImGui::InputText("text", text, sizeof text);
        if (ImGui::Button("Count")) clicks++;
        ImGui::SameLine();
        ImGui::Text("%d", clicks);
        ImGui::Checkbox("ImGui's own demo window", &show_demo);
        bool quit = ImGui::Button("Quit");
        ImGui::End();
        show(false);
        if (show_demo) {
            static bool placed;
            if (scrolled) ImGui::SetNextWindowScroll(ImVec2(0, (float)scrolled));
            ImGui::ShowDemoWindow(&show_demo);
            // it puts itself 650 pixels to the right, for a larger screen than this window
            if (!placed) {
                ImGui::SetWindowPos("Dear ImGui Demo", ImVec2(322, 12));
                ImGui::SetWindowSize("Dear ImGui Demo", ImVec2(306, scrolled ? 140 : 410));
                placed = true;
            }
        }
        ImGui::Render();

        // (the frame before, if the workers were still at it: waited for)
        show(true);
        unsigned drawing_start = instructions();
        if (spin) turn += speed * io.DeltaTime;
        draw_cube(turn, size, background);
        drawing_start = instructions();
        ahead = ImGui_ImplShaderEmu_RenderAhead(ImGui::GetDrawData());
        if (!ahead) ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
        drawing = finishing + instructions() - drawing_start;
        finishing = 0;
        interface = instructions() - interface_start;   // (the cube's few calls are in it too)
        // (not waited for: the next frame writes only its own triangles and table, in the other set)
        if (!ahead) {
            if (waits) seglSwap();
            else seglSwapEarly();
        }

        unsigned now = instructions();
        whole = now - frame_start;
        frame_start = now;
        history[oldest] = (float)whole;
        oldest = (oldest + 1) % HISTORY;
        if (++frames % 30 == 0)
            fprintf(stderr, "imstat: frame %d, %u instructions, %u of them the interface (%u its triangles), %d vertices\n", frames,
                    whole, interface, drawing, io.MetricsRenderVertices);
        if (quit || (most && frames >= most)) break;
    }
    if (ahead) ImGui_ImplShaderEmu_RenderFinish(), seglSwap();
    fprintf(stderr, "imdemo: done\n");
    GrClose();
    return 0;
}
