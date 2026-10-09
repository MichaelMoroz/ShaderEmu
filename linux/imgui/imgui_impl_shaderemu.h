// Dear ImGui on this machine (docs/imgui.md): a Nano-X window for its input, and the GPU
// device, through programs/linux/gles.c, for its triangles. An OpenGL program opens its
// window and calls seglInit() as usual, then these around its own drawing.
#pragma once
#include "imgui.h"

bool ImGui_ImplShaderEmu_Init(unsigned int nano_x_window);
// Takes the window's events that are waiting, and starts a frame's input. False: asked to close.
bool ImGui_ImplShaderEmu_NewFrame();
// Adds the frame's triangles to what OpenGL drew; seglSwap() then shows both.
void ImGui_ImplShaderEmu_RenderDrawData(ImDrawData* draw_data);
