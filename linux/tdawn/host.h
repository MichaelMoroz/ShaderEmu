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
/*
 * A picture of bytes smaller than the screen, which the GPU stretches over the window: a
 * movie's frames (docs/ralert.md). The memory is the GPU's, and *physical is its address for
 * a worker core (docs/multicore.md). NULL when there is no room.
 */
unsigned char *host_picture(int width, int height, unsigned int *physical);
void host_show_picture(void);
/*
 * The map drawn by the GPU (docs/tdawn.md). host_scene_open() makes room for so many cells, or
 * returns -1 and the game draws the map itself. The atlas is 1,024 x 2,048 bytes and the masks
 * a bit for each of them; the shades are 128 x 256 words of 0xTTRRGGBB.
 */
enum { HOST_KEYED, HOST_MASK, HOST_SHADE };
int host_scene_open(int cells);
unsigned char *host_atlas(void);
unsigned char *host_masks(void);
unsigned int *host_shades(void);
/* A cell's kept quad: 24 pixels square at a pixel of the map, from the atlas (x < 0: none). */
void host_tile(int index, int map_x, int map_y, int atlas_x, int atlas_y);
void host_tiles_clear(void);
/* A frame's scene: where the map's corner is on the screen, then rectangles in their order. */
void host_scene_begin(int origin_x, int origin_y);
void host_sprite(int kind, int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour);
/* A rectangle of one colour, from 24 x 24 set bits in the masks at this place. */
void host_block(int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour);
/* The page goes over the scene, with holes of the key index inside this rectangle. */
void host_scene_end(int x, int y, int width, int height, int key);
/* No scene any more: the page is all there is to show. */
void host_scene_drop(void);
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
