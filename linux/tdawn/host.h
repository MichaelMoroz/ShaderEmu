/* What Tiberian Dawn asks of this machine: a Nano-X window shown by the GPU, and its input. */
#ifndef TDAWN_HOST_H
#define TDAWN_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

enum { HOST_NONE, HOST_KEY, HOST_BUTTON, HOST_QUIT };

struct host_event {
	int type;
	int key;	/* a Windows virtual key code, which is what the game's tables hold */
	int down;
	int x, y;	/* of a button event, in the game's pixels */
};

/* Opens the window. 0, or -1 with no server or no GPU. */
int host_open(int width, int height);
/* The two 8-bit pages the GPU can show (0 and 1): the game draws straight into them. */
unsigned char *host_page(int page);
/* 256 words of 0x00RRGGBB the screen and the cursor are looked up in. */
unsigned int *host_palette(void);
/* The pointer's picture (index 0 is a hole); the pixels are copied. */
void host_cursor(const unsigned char *pixels, int width, int height, int hot_x, int hot_y);
/* Shows a page as it is now, with the pointer on it or not. */
void host_present(int page, int with_cursor);
/* The next event, or 0 when there is none. Asks the server only when input has moved. */
int host_event(struct host_event *event);
void host_pointer(int *x, int *y);
/* The host's clock in milliseconds, and instructions run so far. */
unsigned int host_ms(void);
unsigned int host_cycles(void);
/* Ends the machine's frame without showing anything. */
void host_wait(void);

#ifdef __cplusplus
}
#endif
#endif
