// What the port's files tell each other.
#ifndef AOE_PORT_H
#define AOE_PORT_H
extern int aoe_w, aoe_h;                       // the picture's size
extern unsigned char aoe_keys[256];
extern int aoe_mouse_x, aoe_mouse_y;
unsigned long aoe_clock_ms(void);
void aoe_idle(unsigned ms);
void aoe_sound_mapped(const void* at, long bytes, int fd);   // sound.cpp: a file mapped, which a voice can play where it lies
void aoe_lat(int kind, int a, int b);           // AOE_LAT: a line of what became of an input
void aoe_events(void);                         // the window system's and the script's events become messages
void aoe_window_make(int w, int h, const char* title);
void aoe_window_close(void);
void aoe_post(UINT m, WPARAM a, LPARAM b);
HWND aoe_main_window(void);
#endif
