// The handles of what is selected (scene.h): ImGuizmo, drawn by Dear ImGui over the view, inside the
// window's frame of ImGui (panel.cpp): its pointer and buttons are that frame's.
#include "imgui.h"
#include "imgui_internal.h"
#include "ImGuizmo.h"
extern "C" {
#include "scene.h"
#include "panel.h"
}

#define HANDLE 80.0f    // an axis's length in pixels, whatever the picture's size

// ImGuizmo's sines and cosines (build.sh names them for it): the C library's are worked out
// in doubles, which the machine has not, and a turning handle asks for 400 a frame.
// Quarter turns off, then a series: right to the sixth place.
static float sine(float angle, int quarter)
{
    const int turns = (int)(angle * 0.63661977f + (angle < 0 ? -0.5f : 0.5f));
    const float r = angle - (float)turns * 1.57079633f, s = r * r;
    quarter += turns;
    const float value = quarter & 1 ? 1 + s * (-0.5f + s * (0.041666667f + s * (-0.0013888889f + s * 0.0000248016f)))
                                    : r * (1 + s * (-0.16666667f + s * (0.0083333333f + s * -0.00019841270f)));
    return quarter & 2 ? -value : value;
}

extern "C" float gizmo_sin(float angle) { return sine(angle, 0); }
extern "C" float gizmo_cos(float angle) { return sine(angle, 1); }

int gizmo_draw(const float* view, const float* projection, float* matrix, int what, int top, int width, int height, int blind)
{
    if (width <= 0 || height <= 0) return 0;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    // (a drag of the view holds the left button: the handles do not see it)
    const bool down = g.IO.MouseDown[0], clicked = g.IO.MouseClicked[0], released = g.IO.MouseReleased[0];
    if (blind) g.IO.MouseDown[0] = g.IO.MouseClicked[0] = g.IO.MouseReleased[0] = false;
    // (a window of its own, which takes no input: the handles are drawn into its list)
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetRect(0, (float)top, (float)width, (float)height);
    ImGuizmo::SetGizmoSizeClipSpace(2.0f * HANDLE / (float)width);
    const ImGuizmo::OPERATION operation = what == GIZMO_TURN ? ImGuizmo::ROTATE : what == GIZMO_SIZE ? ImGuizmo::SCALE : ImGuizmo::TRANSLATE;
    float before[16];
    memcpy(before, matrix, sizeof before);
    // moved along the world's axes, turned about the thing's own (and sized along them, always)
    ImGuizmo::Manipulate(view, projection, operation, what == GIZMO_MOVE ? ImGuizmo::WORLD : ImGuizmo::LOCAL, matrix);
    const bool held = ImGuizmo::IsOver(operation) || ImGuizmo::IsUsing();
    g.IO.MouseDown[0] = down, g.IO.MouseClicked[0] = clicked, g.IO.MouseReleased[0] = released;
    return memcmp(before, matrix, sizeof before) ? 1 : held ? 2 : 0;
}
