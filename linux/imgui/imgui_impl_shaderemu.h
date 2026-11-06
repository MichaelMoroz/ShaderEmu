// Dear ImGui on this machine (docs/apps/imgui.md): a Nano-X window for its input, and the GPU
// device, through programs/linux/gles.c, for its triangles. An OpenGL program opens its
// window and calls seglInit() as usual, then these around its own drawing.
#pragma once
#include "imgui.h"

bool ImGui_ImplShaderEmu_Init(unsigned int nano_x_window);
// Takes the window's events that are waiting, and starts a frame's input. False: asked to close.
bool ImGui_ImplShaderEmu_NewFrame();
// Adds the frame's triangles to what OpenGL drew; seglSwap() then shows both.
void ImGui_ImplShaderEmu_RenderDrawData(ImDrawData* draw_data);
// The same by worker cores, while the program goes on to its next frame (docs/apps/imgui.md, "The
// copy on worker cores"): call it where RenderDrawData() would be called, after what OpenGL
// draws under the interface. True: the frame is not whole yet, and RenderFinish() makes it so
// (the program calls it, then seglSwap(), before it draws its next frame, and ImGui may make
// its next frame meanwhile). False (no workers, little to copy, a callback among the
// commands): nothing was done, and RenderDrawData() is to be called. IMGUI_COPY=inline: never.
bool ImGui_ImplShaderEmu_RenderAhead(ImDrawData* draw_data);
void ImGui_ImplShaderEmu_RenderFinish();
// For a program that draws its frames more often than ImGui's change (a picture under a panel): with
// KeepFrames(true) RenderDrawData() keeps what it gave the device, and RenderAgain() puts the same
// triangles into the frame being built, with no frame of ImGui's: drawn where they are kept, in two
// blocks of GPU memory KeepFrames() takes (96 KB each; call it before the program takes the rest), or
// copied into the frame when they do not fit there. False: nothing kept
// (none drawn yet, or more than it could keep, or no room in the frame): a frame of ImGui's is wanted.
void ImGui_ImplShaderEmu_KeepFrames(bool keep);
bool ImGui_ImplShaderEmu_RenderAgain();
// Whether the workers have finished, so that RenderFinish() would not wait: a program asks now
// and then while it makes its next frame, and shows the frame before as soon as it can.
bool ImGui_ImplShaderEmu_RenderReady();
