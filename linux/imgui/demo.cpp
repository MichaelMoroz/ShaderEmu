// imdemo: a cube drawn with OpenGL and, over it, windows of Dear ImGui (docs/imgui.md).
// IMDEMO_FRAMES=N ends after N frames, IMDEMO_SHOW=1 starts with ImGui's own demo window open.
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
    int frames = 0, clicks = 0, most = getenv("IMDEMO_FRAMES") ? atoi(getenv("IMDEMO_FRAMES")) : 0;
    unsigned frame_start = instructions(), whole = 0, interface = 0, drawing = 0;

    while (ImGui_ImplShaderEmu_NewFrame()) {
        unsigned interface_start = instructions();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
        ImGui::Begin("ShaderEmu");
        ImGui::Text("Dear ImGui %s on a RISC-V", ImGui::GetVersion());
        ImGui::Text("computer in a pixel shader");
        ImGui::Separator();
        ImGui::Text("%.1f frames a second", io.Framerate);
        ImGui::Text("a frame: %u,000 instructions", whole / 1000);
        ImGui::Text("ImGui's part: %u,000 (%d vertices)", interface / 1000, io.MetricsRenderVertices);
        ImGui::Text("of that, its triangles: %u,000", drawing / 1000);
        ImGui::PlotLines("##frame", history, HISTORY, 0, "instructions a frame", 0, FLT_MAX, ImVec2(-1, 50));
        ImGui::Separator();
        ImGui::Checkbox("Spin the cube", &spin);
        ImGui::SliderFloat("speed", &speed, 0, 180, "%.0f degrees/s");
        ImGui::SliderFloat("size", &size, 0.2f, 2);
        ImGui::ColorEdit3("background", background);
        ImGui::InputText("text", text, sizeof text);
        if (ImGui::Button("Count")) clicks++;
        ImGui::SameLine();
        ImGui::Text("%d", clicks);
        ImGui::Checkbox("ImGui's own demo window", &show_demo);
        bool quit = ImGui::Button("Quit");
        ImGui::End();
        if (show_demo) {
            static bool placed;
            ImGui::ShowDemoWindow(&show_demo);
            // it puts itself 650 pixels to the right, for a larger screen than this window
            if (!placed) {
                ImGui::SetWindowPos("Dear ImGui Demo", ImVec2(322, 12));
                ImGui::SetWindowSize("Dear ImGui Demo", ImVec2(306, 410));
                placed = true;
            }
        }
        ImGui::Render();

        if (spin) turn += speed * io.DeltaTime;
        draw_cube(turn, size, background);
        unsigned drawing_start = instructions();
        ImGui_ImplShaderEmu_RenderDrawData(ImGui::GetDrawData());
        drawing = instructions() - drawing_start;
        interface =instructions() - interface_start;   // (the cube's few calls are in it too)
        seglSwap();

        unsigned now = instructions();
        whole = now - frame_start;
        frame_start = now;
        for (int i = 0; i + 1 < HISTORY; i++) history[i] = history[i + 1];
        history[HISTORY - 1] = (float)whole;
        if (++frames % 30 == 0)
            fprintf(stderr, "imstat: frame %d, %u instructions, %u of them the interface (%u its triangles), %d vertices\n", frames,
                    whole, interface, drawing, io.MetricsRenderVertices);
        if (quit || (most && frames >= most)) break;
    }
    fprintf(stderr, "imdemo: done\n");
    GrClose();
    return 0;
}
