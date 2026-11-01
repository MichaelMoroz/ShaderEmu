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
void host_palette_set(int index, unsigned int word); /* a colour, stored when the list in flight is drawn */
unsigned int *host_palette(void);
/* The pointer's picture (index 0 is a hole); the pixels are copied. */
void host_cursor(const unsigned char *pixels, int width, int height, int hot_x, int hot_y);
/* Shows a page as it is now, with the pointer on it or not. */
void host_present(int page, int with_cursor);
/* The frame shown last is drawn by the time this returns: before a store to a page it shows. */
void host_settle(void);
/*
 * A picture of bytes smaller than the screen, which the GPU stretches over the window: a
 * movie's frames (docs/games/ralert.md). The memory is the GPU's, and *physical is its address for
 * a worker core (docs/machine/multicore.md). NULL when there is no room.
 */
unsigned char *host_picture(int width, int height, unsigned int *physical);
void host_show_picture(void);
/*
 * The map drawn by the GPU (docs/games/tdawn.md). host_scene_open() makes room for so many cells, or
 * returns -1 and the game draws the map itself. The atlas is 1,024 x 2,048 bytes and the masks
 * a bit for each of them; the shades are 256 x 256 words of 0xTTRRGGBB.
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
/* A scene has three parts, drawn in this order: 0 under the objects, 1 the objects (begun
 * empty every scene), 2 over them. host_scene_part() says whose the next rectangles are; with
 * keep 0 the part's own are forgotten first. Parts 0 and 2 stay from scene to scene. */
void host_scene_part(int part, int keep);
void host_sprite(int kind, int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour);
/* A rectangle of one colour, from 24 x 24 set bits in the masks at this place. */
void host_block(int x, int y, int width, int height, int atlas_x, int atlas_y, unsigned int colour);
/*
 * Rectangles the caller keeps, by where they are from the scene's origin: put where that is
 * now, those that touch the view (x0, y0 to x1, y1 of the screen). Only noted here: the memory
 * must stay as it is until host_scene_settle() or the next host_scene_begin() has returned.
 */
struct host_quad { short x, y, w, h, ax, ay; unsigned char kind; unsigned int colour; };
void host_scene_view(int x0, int y0, int x1, int y1);
void host_quads(const struct host_quad *quads, int count);
void host_scene_settle(void);
/* For a test: the scene as noted, drawn here: an index and a sum of what darkens it, a pixel of the screen. */
void host_scene_picture(unsigned char *index, unsigned int *dark);
/* For a test: the window's picture as the GPU left it, a word a pixel in rows of *row (NULL: it has no buffer). */
const unsigned int *host_window_picture(int *width, int *height, int *row);
/*
 * A frame of a shape file as a worker core has made it ready: the part of it that has pixels,
 * rows of `w`, and in the 32 bytes before them a bit for every index among them. NULL while
 * it has not (the file is asked for then, a run of frames from this one on): the caller has
 * the game build the frame, which gives the same pixels. After the pixels are its rows' spans
 * (HOST_FRAME_SPANS): each row's first and last pixel from the rectangle's left, bit 15 of the
 * first set when a hole lies between (a row with none: last < first).
 */
enum { HOST_FRAME_READY = 1, HOST_FRAME_EMPTY = 2, HOST_FRAME_SPANS = 4 };
struct host_frame { const unsigned char *pixels; short left, top, w, h; int state; };	/* 16 bytes */
struct host_span { unsigned short first, last; };
const struct host_frame *host_frame(const void *file, int frame);
static inline const struct host_span *
host_frame_spans(const struct host_frame *r)
{
	return (const struct host_span *)(r->pixels + ((r->w * r->h + 3) & ~3));
}
/* A file that will be asked for is unpacked ahead, from this frame on. */
void host_frames_ahead(const void *file, int frame);
/* What was put off for the scene's core is done by the time this returns (it looks at shapes). */
void host_frames_settle(void);
/* A unit's path searches on other cores: how many can be made now, one begun on core k (1 up; 0 where no worker is free), waited for. */
int host_path_cores(void);
int host_path_post(int k, unsigned int (*search)(unsigned int, unsigned int), unsigned int object, unsigned int what);
void host_path_wait(int k);
/* And cores that ask about the map's cells ahead of a search, k from 0. */
int host_pre_cores(void);
int host_pre_post(int k, unsigned int (*ask)(unsigned int, unsigned int), unsigned int object, unsigned int what);
void host_pre_wait(int k);
void host_frames_forget(void);	/* the files are not what they were: before they go */
extern unsigned int host_frames_bytes;	/* of the frames made ready so far */
/*
 * Work for whichever core makes the next scene, done before that scene's commands: an atlas's
 * pixels that only the scene looks at. What it is given must stay as it is until then.
 */
typedef void (*host_fill_fn)(void *what, const void *from);
void host_fill(host_fill_fn fn, void *what, const void *from);
/* The page goes over the scene, with holes of the key index inside this rectangle. */
void host_scene_end(int x, int y, int width, int height, int key);
/* The same, after draw() has noted the objects' part, both on a worker (the objects drawn a
 * frame behind: gl.cpp); whether a worker would take it now. joined() is called on this core
 * once draw() has run, at the join or at once. */
void host_scene_behind(void (*draw)(void), int x, int y, int width, int height, int key, void (*joined)(void));
int host_scene_behind_ready(void);
/* No scene any more: the page is all there is to show. */
void host_scene_drop(void);
/* The scene's rectangles are made by a worker core when the machine has one (host.c). This
 * gives the cores back, for another use of them in the program; and how many scenes one made. */
void host_workers_close(void);
/* fn over items 0..n-1 in pieces on the worker cores and this one, each item writing out_bytes in a row of `out`. */
void host_for(unsigned int n, void (*fn)(void *ctx, unsigned int from, unsigned int to), void *ctx, void *out,
	      unsigned int out_bytes);
extern unsigned int host_scene_jobs;
/* TDAWN_MARK's line of what the workers' jobs ran a frame, said before the second mark (ends 1). */
void host_workers_mark(int ends, int frames);
/* The next event, or 0 when there is none. Asks the server only when input has moved. */
int host_event(struct host_event *event);
void host_pointer(int *x, int *y);
/* The buttons as the machine has them now (bit 0 left, 1 right): an event of one comes later. */
int host_buttons(void);
/* The host's clock in milliseconds, and instructions run so far. */
unsigned int host_ms(void);
unsigned int host_cycles(void);
/* Ends the machine's frame without showing anything. */
void host_wait(void);

/*
 * The first byte of a row that is not 0 (n if none) and the last (-1): a word at a time,
 * since most of a shape's frame is nothing.
 */
typedef unsigned int __attribute__((may_alias)) host_word;
static inline int
host_first_pixel(const unsigned char *row, int n)
{
	int i = 0;

	for (; i < n && ((unsigned long)(row + i) & 3); i++)
		if (row[i])
			return i;
	while (i + 4 <= n && !*(const host_word *)(row + i))
		i += 4;
	while (i < n && !row[i])
		i++;
	return i;
}

static inline int
host_last_pixel(const unsigned char *row, int n)
{
	int i = n;

	for (; i > 0 && ((unsigned long)(row + i) & 3); i--)
		if (row[i - 1])
			return i - 1;
	while (i >= 4 && !*(const host_word *)(row + i - 4))
		i -= 4;
	while (i > 0 && !row[i - 1])
		i--;
	return i - 1;
}

#ifdef __cplusplus
}
#endif
#endif
