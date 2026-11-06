/*
 * nxpath's window drawn by Dear ImGui (panel.cpp, docs/apps/raytrace.md, "The window"): the menus along the
 * top, the panel on the right, the line of figures along the bottom and the handles over the view, one frame
 * of ImGui over what OpenGL drew of the view. What the controls ask for is handed back to nxpath.c, which
 * does it after the frame (a dialog, a scene changed): what the frame shows is read from nxpath.c's
 * functions below.
 */
#ifndef PANEL_H
#define PANEL_H
#ifdef __cplusplus
extern "C" {
#endif

#define PANEL 250		/* the panel's width, right of the picture */
#define STATUS 22		/* the line of figures along the bottom */
#define ROWS 6			/* of the scene's list */
enum { TAB_OBJECT, TAB_MATERIAL, TAB_RENDER, TAB_VIEW, TABS };

/* The sliders of nxpath.c's own settings by number: a step from 0 to its most. The scene's numbers (a place, a
 * colour, a material's, the camera's) the panel edits where they lie in `sc` (scene.h). */
enum { S_SAMPLES, S_TURNS, SLIDERS };

/* What a control of the panel or a row of a menu does. */
enum {
	A_NONE, A_IMPORT, A_EXPORT, A_CLOSE, A_PLANE, A_BOX, A_BALL, A_LIGHT, A_DUPLICATE, A_DELETE, A_SCENE, A_PICTURE, A_WHOLE, A_RENDER,
	A_STOP, A_LIVE, A_SMOOTH, A_TAB, A_DETAIL = A_TAB + TABS, A_WHAT, A_MATERIAL = A_WHAT + 3, A_SLIDER = A_MATERIAL + 6,
	A_SLIDE = A_SLIDER + SLIDERS, A_EDITED = A_SLIDE + SLIDERS, A_EDIT_DONE, A_CAMERA, A_RENAMED, A_OPEN, A_SAVE, A_FILES, A_EXAMPLE,
	A_CHOOSE = A_EXAMPLE + 64
	/* A_DETAIL: its value the detail chosen; A_SLIDER + slider: its value the step; A_SLIDE + slider: let go (its
	 * settings kept); A_EDITED: a number of the scene changed in place (the picture is of another scene); A_EDIT_DONE:
	 * such a number let go; A_CAMERA: the camera's changed; A_RENAMED: a thing's name typed; A_FILES: a file chosen
	 * (panel_files_path), its value the action that asked for it; A_CHOOSE + row: a row of the list clicked */
};

/* ---- nxpath.c, for the frame ---- */
int path_rows(void);			/* the scene's list: its rows, a row's words, the one selected (-1: none) */
const char *path_row(int row);
int path_selected(void);
int path_object(void);			/* the object or the light selected, or -1 */
int path_light(void);
int path_tab(void);			/* the panel's tab, what the handles do */
int path_what(void);
int path_slider(int n, int set, int value);	/* a slider's step now, or set to `value` */
int path_slider_most(int n);
void path_slider_text(int n, char *text, int size);
const char *path_detail(int i);		/* the Render tab: detail i's words (-1: the one chosen, NULL past the last), the */
int path_detail_at(void);		/* one chosen, the picture's size, what the two buttons say */
void path_picture_size(int *w, int *h);
int path_live(void);
int path_smooth(void);			/* the picture shown smoothed while it has few samples (the lanes' job) */
int path_tracing(void);			/* shares out and not stopped: the button says Stop */
int path_showing(void);			/* the view shows a traced picture: the button shows the scene */
int path_greyed(int id);		/* a menu's row that cannot be done now */
const char *path_example(int i);	/* File, Examples: a scene's name, NULL past the last */
/* The Render tab's figures: thousands of rays a second at each step of the samples (their count), the picture's
 * pixels by their light, a share of them in each of `count` bins (0: no picture yet), the scene's figures, the workers. */
int path_rates(const float **values);
int path_light_shares(float *bins, int count, float *most);
void path_figures(char *text, int size);
int path_workers(void);
const char *path_status(void);		/* the line of figures, and the picture's bar: its words and how much of it is filled */
void path_bar(char *text, int size, float *fill);

/* ---- panel.cpp ---- */
/* Once, after seglInit on the window: false when it cannot be. The menu bar's height after it. */
int panel_init(unsigned int window);
int panel_bar(void);
/* Input as the window has it: the pointer's place in the window, a button (0 left, 1 right, 2 middle) going down or
 * up, a notch of the wheel, a key while ImGui types (panel_typing()). */
void panel_pointer(int x, int y);
int panel_button(int button, int down);	/* the presses so far, a press's number */
void panel_wheel(int notches);
void panel_key(int ch, int modifiers, int down);
/* After a frame: whether the pointer is ImGui's (over a menu, the panel, the line of figures or a popup), whether
 * a text field has the keyboard, and whether ImGui wants another frame (its input queue, a click settling). */
int panel_wants_pointer(void);
/* Whether press number `press` has been given to ImGui in a frame, and then whether it found nothing of ImGui's
 * under the pointer (no window, no popup open): the press is the view's. */
int panel_press_given(int press, int *free);
int panel_typing(void);
int panel_busy(void);
/* A file to be chosen in ImGui's window over the panel (the window system's dialogs before): its title, the folder it
 * starts in, the endings shown (".obj .ply"), and for one to be written its name to start with (NULL: one to read).
 * When it is chosen, a frame asks for A_FILES with `kind` as its value; the path is panel_files_path()'s. */
void panel_files(int kind, const char *title, const char *folder, const char *endings, const char *name);
const char *panel_files_path(void);

struct panel_frame {
	int wide, high, view_w, view_h;		/* the window, and the view under the menu bar */
	/* the handles of what is selected (gizmo.cpp), when `handles`: the view's matrices, the selected thing's matrix, what
	 * they do, and whether a drag of the view holds the left button (the handles do not see it then) */
	int handles, what, drag;
	const float *view, *projection;
	float *matrix;
	/* out: the handles' (1 a drag changed the matrix, 2 the pointer is over one or holds it), what the controls asked for
	 * in their order (A_*; a slider's change is A_SLIDER + n with its value), and, for a test, the panel's controls */
	int moved;
	int asked[16], values[16], asked_count;
	int controls, over, cut, lowest;
};
/* One frame of ImGui over what OpenGL drew (before the swap). */
void panel_draw(struct panel_frame *f);

/* ---- gizmo.cpp ---- */
enum { GIZMO_MOVE, GIZMO_TURN, GIZMO_SIZE };
/* The handles over the view at (0, top) of `width` by `height` in the window, inside panel_draw()'s frame: 1 when a
 * drag changed `matrix`, 2 while the pointer is over a handle or holds one. `blind`: the left button is the view's. */
int gizmo_draw(const float *view, const float *projection, float *matrix, int what, int top, int width, int height, int blind);

#ifdef __cplusplus
}
#endif
#endif
