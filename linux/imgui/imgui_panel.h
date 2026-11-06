/*
 * A program's panel by Dear ImGui over what it draws with OpenGL (docs/apps/imgui.md, "A panel over a
 * program's picture"): nxpath's window, nxray's and nxphys's. The program feeds ImGui its own window's
 * events, and a frame of ImGui is made only when something changed: an input settling, or the program
 * saying what the panel shows is new (imp_wake). Between them the frame of ImGui's made last is put into
 * each of the program's frames again (ImGui_ImplShaderEmu_RenderAgain), a copy of words.
 */
#ifndef IMGUI_PANEL_H
#define IMGUI_PANEL_H
#include <nano-X.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Once, after seglInit() on the window: 0 when it cannot be. The height of a line of ImGui's controls. */
int imp_init(unsigned int window);
int imp_line(void);
int imp_text_line(void);	/* and of a line of words */
/* Input as the window has it: the pointer's place in the window, a button (0 left, 1 right, 2 middle)
 * going down or up (it returns the presses so far: a press's number), a notch of the wheel, a key. */
void imp_pointer(int x, int y);
int imp_button(int button, int down);
void imp_wheel(int notches);
void imp_key(int ch, int modifiers, int down);
/* A window's event as the four above: 1 when it was ImGui's (the pointer over ImGui's windows, a press
 * there or with a popup open, its release, a key while it types), 0 when it is the program's, and then
 * ImGui is not told of it (a move or a click in the program's picture wants no frame of ImGui's). */
int imp_event(const GR_EVENT *e);
/* Whether press number `press` has been given to ImGui in a frame, and then whether it found nothing of
 * ImGui's under the pointer (no window, no popup open): the press is the program's. */
int imp_press_given(int press, int *free);
int imp_wants_pointer(void);	/* after a frame: the pointer is over ImGui's */
int imp_typing(void);		/* a field has the keyboard */
/* For imp_frame(): the frames of ImGui's kept in GPU memory of their own, taken now (before the program
 * takes what is left of it); otherwise at the first imp_frame(), or copied into each frame if none is left. */
void imp_keep(void);
/* Frames wanted: what the panel shows changed (the program's figures), for so many frames of ImGui's. */
void imp_wake(int frames);
int imp_busy(void);		/* ImGui wants a frame (an input settling, imp_wake) */
/* The panel into the frame being built: a frame of ImGui's (`build` makes its windows, between
 * NewFrame and Render) when one is wanted or nothing is kept, else the one before again. `wide`
 * and `high` the window's. 1 when a frame of ImGui's was made. */
int imp_frame(int wide, int high, void (*build)(void *), void *context);

#ifdef __cplusplus
}
/* For nxpath, which makes a frame every time it draws: the frame's beginning (NewFrame, the input
 * given) and its end (Render, the triangles into the frame). */
void imp_begin(int wide, int high);
void imp_end();
#endif
#endif
