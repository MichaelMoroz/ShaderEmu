/* The window's side of the port (window.c), for the side that has windows.h (host.cpp). */
#ifndef AOE_WINDOW_H
#define AOE_WINDOW_H
#ifdef __cplusplus
extern "C" {
#endif
enum { AOE_KEY_DOWN = 1, AOE_KEY_UP, AOE_BUTTON_DOWN, AOE_BUTTON_UP, AOE_MOVE, AOE_CLOSE };
struct aoe_event { int kind, a, b, c; };
unsigned char *aoe_win_open(int w, int h, const char *title);
void aoe_win_palette(const unsigned char *rgbx);
void aoe_win_present(void);
void aoe_win_not_a_frame(void);   /* the next present shows nothing new: not counted in the caption's rate */
void aoe_win_settle(void);
void aoe_win_origin(int *x, int *y);
void aoe_win_map(int *x, int *y);
void aoe_win_size(int *w, int *h);   /* the window's size now */   /* a point of the window now as a pixel of the picture, which is stretched over it */
void aoe_win_close(void);
int aoe_win_event(struct aoe_event *e);
void aoe_win_rest(void);
unsigned aoe_win_clock(void);
int aoe_win_buttons(void);
void aoe_win_pointer(int *x, int *y);
#ifdef __cplusplus
}
#endif
#endif
