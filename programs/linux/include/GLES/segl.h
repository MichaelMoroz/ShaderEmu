// What ties programs/linux/gles.c to a window: where EGL would be. A program opens its window
// with Nano-X as usual and hands it over here.
#ifndef SHADEREMU_SEGL_H
#define SHADEREMU_SEGL_H

#include <GLES/gl.h>

// Draw into this Nano-X window from now on. 0, or -1 if the machine has no GPU device.
int seglInit(unsigned int nano_x_window);
// The window's size in pixels, as of the last seglSwap() (or seglInit()).
void seglSize(int* width, int* height);
// Shows what was drawn since the last swap, and returns once the GPU has drawn it.
void seglSwap(void);
// The same without the wait, which ends the machine's pass: the program goes on at once, and
// calls seglSettle() before it next writes memory the list reads (its textures, kept vertices,
// the palette: anything written through a pointer; the library's own calls settle themselves).
void seglSwapEarly(void);
void seglSettle(void);
// seglSwapEarly() for a frame drawn on a worker core: the list is handed to the first core,
// which submits it with seglSwapTake() (1 if it did; never a wait) and must call that often. A
// worker never takes the GPU's lock: it is the window system's too (docs/machine/gpu.md).
void seglSwapHand(void);
int seglSwapTake(void);
// The next swap shows a picture that is not a whole frame (one filling in): the caption's
// frames a second (fps_caption.h) does not count it.
void seglNotAFrame(void);
// The window was resized (a GR_UPDATE_SIZE event): learn its size now, not at the next swap.
void seglWindowChanged(void);
// Where a point of the window (an event's x, y) lies in a picture of width x height pixels
// stretched over the whole window, at the window's size now. Clamped to the picture.
void seglPicturePoint(int x, int y, int width, int height, int* px, int* py);

// Texture memory the program writes itself: a pointer to `bytes` of GPU memory (16-byte
// aligned), or NULL when there is none left. It lasts until the program ends.
void* seglMemory(unsigned int bytes);
// Makes the bound texture those pixels, with no copy: GL_COLOR_INDEX8_EXT bytes or GL_RGBA
// words (0xTTRRGGBB, T being transparency), rows top to bottom.
void seglTexturePointer(const void* pixels, GLsizei width, GLsizei height, GLenum internal);
// The 256-word palette of paletted textures (0x00RRGGBB), for writing directly.
unsigned int* seglPalette(void);
// How many bytes seglMemory() still has to give.
unsigned int seglMemoryLeft(void);
// More GPU memory for a program that needs it, below seglMemory()'s: the display's unused
// framebuffer while the desktop is drawn as layers. NULL (and 0 bytes) when there is none.
// One program may have it at a time; nothing hands it out or takes it back.
void* seglMemorySpare(unsigned int* bytes);

// What the host says of where the machine is shown (docs/world/holodeck.md): eight words the host
// writes every frame, or 0 before seglInit. All zero: a host that says nothing.
const volatile unsigned int* seglHostState(void);
// Which of the world's axes is up (1 x, 2 y, 3 z; 0: not said), for a host that shows the
// frame as a world round its visitor (docs/world/holodeck.md): it keeps that axis upright whatever
// the program's camera looks at. A program whose camera looks up and down says it.
void seglWorldUp(int axis);
// The frames from here on are a flat screen (a menu, a level loading) and no world: such a
// host shows the program's picture on a screen before its visitor, and no world round them.
void seglWorldFlat(int flat);
#define SEGL_HOST_KIND(s)      ((s)[0] & 15)            /* 1 the harness, 2 the world on a desktop, 3 in a headset */
#define SEGL_HOST_HOLODECK(s)  (((s)[0] >> 4) & 1)      /* its owner sits in a holodeck's seat */
#define SEGL_HOST_VISITORS(s)  (((s)[0] >> 8) & 255)
#define SEGL_HOST_NUMBER(s)    (((s)[0] >> 16) & 255)   /* this visitor's */
#define SEGL_HOST_HEAD_YAW(s)   ((short)((s)[1] & 0xffff))   /* 65,536 to a turn, to the right */
#define SEGL_HOST_HEAD_PITCH(s) ((short)((s)[1] >> 16))      /* upwards */
#define SEGL_HOST_HEAD_X(s)     ((short)((s)[2] & 0xffff))   /* millimetres from the seat's eye point: right, */
#define SEGL_HOST_HEAD_Y(s)     ((short)((s)[2] >> 16))      /* up, */
#define SEGL_HOST_HEAD_Z(s)     ((short)((s)[3] & 0xffff))   /* forward */
#define SEGL_HOST_HEAD_ROLL(s)  ((short)((s)[3] >> 16))   /* without hands; with them, the left hand's way: */
#define SEGL_HOST_LEFT_YAW(s)   ((signed char)((s)[3] >> 16) * 256)   /* in the head's units, 256 at a step */
#define SEGL_HOST_LEFT_PITCH(s) ((signed char)((s)[3] >> 24) * 128)   /* (its second word is the sticks) */
#define SEGL_HOST_HANDS(s)     (((s)[0] >> 5) & 1)      /* the hands are tracked: a program may aim by one */
/* a hand (0 left, 1 right): its place in units of 4 mm (right, up, forward), its buttons, and
   where it points: its turn to the right and its rise, 65,536 to a turn as the head's */
#define SEGL_HOST_HAND_X(s, h)   ((int)((s)[4 + 2 * (h)] << 22) >> 22)
#define SEGL_HOST_HAND_Y(s, h)   ((int)((s)[4 + 2 * (h)] << 12) >> 22)
#define SEGL_HOST_HAND_Z(s, h)   ((int)((s)[4 + 2 * (h)] << 2) >> 22)
#define SEGL_HOST_HAND_YAW(s, h)   ((short)((s)[5 + 2 * (h)] & 0xffff))
#define SEGL_HOST_HAND_PITCH(s, h) ((short)((s)[5 + 2 * (h)] >> 16))
#define SEGL_HOST_HAND_TRIGGER(s, h) (((s)[4 + 2 * (h)] >> 30) & 1)
#define SEGL_HOST_HAND_GRIP(s, h)    (((s)[4 + 2 * (h)] >> 31) & 1)
/* The left hand's second word is the sitter's sticks instead, a signed byte each, 127 pushed
   home: 0 the left one to the right, 1 forward, 2 the right one to the right, 3 up. A desktop's
   sitter has the keys they walk by as the left one, and fire as the right trigger */
#define SEGL_HOST_STICK(s, n)  ((signed char)((s)[5] >> (8 * (n))))
/* the room's scale, 255ths: the plane 1.28 by 0.8 m of the room is near * (far / near) ^ this */
#define SEGL_HOST_PLANE(s)     ((s)[0] >> 24)
/* with no hands (a desktop's sitter): how far their mouse has gone to the right, a counter of
   16 bits that wraps; the head's pitch is the aim's rise */
#define SEGL_HOST_MOUSE_X(s)   ((s)[4] & 0xffff)
/* with no hands, while a game is played from the seat: the wheel's notches up, 8 bits that wrap */
#define SEGL_HOST_WHEEL(s)     (((s)[5] >> 16) & 255)
/* the jump button is held (a headset's, or a desktop sitter's Space): no trigger jumps */
#define SEGL_HOST_JUMP(s)      (((s)[0] >> 6) & 1)
/* For the holodeck's default scale (docs/world/holodeck.md, "The world's size"): the player's eye over
   its floor in the program's units, its near and far planes and its projection's x and y scales.
   `game` is linux/apps/gameid.h's number: the host takes the word only while that game is played. */
static inline void seglWorldEye(int game, float eye, float znear, float zfar, float fx, float fy)
{
	volatile unsigned int* word = (volatile unsigned int*)seglHostState();
	float x = 1.28f * fx, y = 0.8f * fy, unit = 2 * znear / (x < y ? x : y), spread = __builtin_logf(zfar / znear);
	int a = (int)(__builtin_logf(eye / unit) / spread * 4096), b = (int)(4096 / spread);
	unsigned int said;

	if (!word || spread <= 0)
		return;
	a = a < -32768 ? -32768 : a > 32767 ? 32767 : a;
	b = b > 4095 ? 4095 : b;
	said = (unsigned)(a & 0xffff) | (unsigned)b << 16 | (unsigned)(game & 15) << 28;
	word += (0x6c - 0x360) / 4;	/* the control word 0x8100006c, written only when it changes */
	if (*word != said)
		*word = said;
}

// Vertices the program has made compact itself (docs/machine/gpu.md: x, y, z, then u | v << 16 in
// 1,024ths), drawn as glDrawArrays draws: GL_TRIANGLES, a strip, a fan or GL_QUADS. x, y and z
// are the kind of number the library was built for: floats with SEGL_FLOAT, else 16.16.
void seglCompact(GLenum mode, const unsigned int* vertices, GLsizei count);
// The same for triangles the program writes in place: room for `count` vertices (a multiple
// of three) of a draw made now, or NULL when the frame has none left.
unsigned int* seglCompactSpace(GLsizei count);
// Gives back the last `unused` vertices of the room seglCompactSpace() or seglPoints() just gave.
void seglCompactTrim(GLsizei unused);
// Room for `count` points, a compact vertex each, drawn as triangles that face the eye: the
// point, and corners `size` above it and to its right, times 1 + growth x depth from depth
// `from` on. Those three are the library's own numbers. NULL when the frame has no room.
unsigned int* seglPoints(GLsizei count, const void* size_growth_from);
// And for triangles that are in memory from seglMemory() already: a draw of them where they
// are, which the program may ask for again in any later frame. -1 when the frame is full.
int seglCompactAt(const void* vertices, GLsizei count);
// Triangles of tagged vertices (docs/machine/gpu.md): the fourth word is u | v << 12 | tag << 24. With
// a `table` (256 words in memory from seglMemory()) a vertex's colour is the word its tag
// names. `vertices` are where they are, as seglCompactAt()'s, or NULL for room in the frame.
// Returns where the vertices are (to be written, when the room is the frame's), NULL for none.
unsigned int* seglTagged(const void* vertices, GLsizei count, const unsigned int* table);
// The same from packed vertices, a word each (x | y << 8 | z << 16 | tag << 24, whole numbers)
// with u | v << 12 a word each in `coords`; all of it in memory from seglMemory(). -1: frame full.
int seglPacked(const void* vertices, const void* coords, GLsizei count, const unsigned int* table);
// The matrix on top of the GL_MODELVIEW or GL_PROJECTION stack as sixteen of the library's
// own numbers, and the current matrix set from sixteen such: a copy, with no conversion.
void seglGetMatrix(GLenum mode, void* matrix);
void seglSetMatrix(const void* matrix);

// Makes a texture name the window's own pixels, as the last frame left them (no copy), and
// says how far across that texture the window's width reaches (its rows may be longer).
// -1 when the window has no pixels of its own.
int seglWindowTexture(GLuint name, GLfixed* across);

// One quad of a texture in one call: four corners (x, y, z) and their texture
// coordinates, a grey and an alpha (below 255 it is blended), and whether the key's texels are
// holes. Uses the matrices and the depth test in force, and none of the other state.
void seglQuad(const GLfixed* xyz, const GLfixed* uv, GLuint name, unsigned grey, int alpha, int keyed);

// One bit a pixel for seglTexturePointer (rows of whole bytes, leftmost pixel in the highest
// bit): where a bit is set the colour is drawn, elsewhere nothing.
#define SEGL_BITS 0x1F0B1
// A layer of tiles for seglTexturePointer (docs/machine/gpu.md, fragment mode 5): `pixels` is four
// words in memory from seglMemory(), the addresses (seglAddress()) of its cells, its tiles
// and its palette, and tile width | height << 8 | cells in a row << 16 | n << 28 (n not 0:
// the tiles are in pieces of 2^n, and their address is a table of the pieces'). A cell is 16 bits:
// a tile's number, bit 10 mirrors it across and bit 11 down, bits 12-15 a bank of 16 colours.
// A tile is width x height bytes; the palette is words of 0x00RRGGBB. The texture's width and
// height are the layer's in pixels; seglQuad draws it, keyed on a tile's byte.
#define SEGL_TILES 0x1F0B2
// A video's frame for seglTexturePointer (docs/machine/gpu.md, fragment mode 6): width x height bytes
// of luma, then the blue and the red chroma planes, each half the size each way; both sizes even.
#define SEGL_YUV420 0x1F0B3
// A rectangle of a texture drawn over what the list drew before it, in the list's order: box is
// x0, y0, x1, y1 as seglQuad's corners are, texels the same corners in 1,024ths of the texture
// (texels, for one 1,024 wide). The colour is 0xTTRRGGBB, T being transparency; it tints an
// 8-bit or RGBA texture and is what a SEGL_BITS texture draws.
void seglSprite(const GLfixed* box, const int* texels, GLuint name, unsigned colour, int keyed);
// Many sprites as seglSprite draws them one by one, in one call (a box, texels, texture, colour, key each).
typedef struct { GLfixed box[4]; int texels[4]; GLuint name; unsigned colour; int keyed; } SEGLsprite;
void seglSprites(const SEGLsprite* sprites, int count);
// Many sprites of one texture at once: the program writes them at seglSpriteSpace() (`room` of
// them fit), 16 words each: four corners of x, y, 0 and texels (u | v << 16), x and y being
// seglNumber() of a corner; seglSpritesUsed() then draws so many as seglSprite would have.
unsigned int* seglSpriteSpace(GLsizei* room);
unsigned int seglNumber(GLfixed value);
void seglSpritesUsed(GLsizei quads, GLuint name, unsigned colour, int keyed);
// The command and the four vertices the last seglQuad or seglSprite wrote, and the address a
// command holds for memory from seglMemory(): for changing a list that is drawn again.
unsigned int* seglLastCommand(void);
unsigned int* seglLastVertices(void);
unsigned int seglAddress(const void* memory);
// Triangles in the window's own pixels (x right, y down), drawn over the frame in the order
// given and blended by alpha: a user interface. A vertex is sixteen of the library's numbers
// (docs/machine/gpu.md): x, y at 0-1, texture coordinates at 8-9, red, green, blue, alpha (0 to 1) at
// 12-15. Room for vertices, and in *room how many fit; then how many were written of them.
void* seglScreenSpace(GLsizei* room);
void seglScreenUsed(GLsizei count, GLuint name);   // (a multiple of three; a texture of words, or 0)
// The same in a quarter of the memory, as tagged vertices with a table (docs/machine/gpu.md): a vertex
// is four words, x and y in pixels (the library's numbers), 0, and u | v << 12 | tag << 24 in
// 1,024ths of the texture; its colour is the table's word for its tag, transparency << 24 |
// red << 16 | green << 8 | blue. seglScreenTable() gives a table of 256 words in the frame
// (NULL: no room); then room and what was written, as above, `quads` for four corners a quad
// (drawn as corners 0 1 2 and 0 2 3) in place of three a triangle.
unsigned int* seglScreenTable(void);
unsigned int* seglScreenCompactSpace(GLsizei* room);
void seglScreenCompactUsed(GLsizei count, int quads, GLuint name, const unsigned int* table);
// For a program whose worker cores write such vertices while it does other things
// (docs/apps/imgui.md, "The copy on worker cores"): seglFrameSpace() is room in the frame, whole
// 16 bytes, that the program fills as it likes (NULL: not that much; seglFrameLeft() says how
// much there is), and seglScreenCompactAt() draws `count` vertices that lie in it, with a
// table that does, as seglScreenCompactUsed() draws those written last.
void* seglFrameSpace(unsigned int bytes);
unsigned int seglFrameLeft(void);
void seglScreenCompactAt(const unsigned int* vertices, GLsizei count, int quads, GLuint name, const unsigned int* table);

// Forgets what was drawn since the last swap. seglSwapAgain() has the list of the last swap
// drawn once more, as its memory (textures, commands, vertices) is now.
void seglDiscard(void);
void seglSwapAgain(void);
// The same without the wait: seglSettle() before the program next writes what that list reads.
void seglSwapAgainEarly(void);

// Commands that stay (docs/machine/gpu.md): quads of compact vertices in memory from seglMemory(), and
// a command per run of them with one texture and one grey, at the head of every frame's list.
// seglKeep(n, quads) says how many (0: none; -1 if they do not fit), before a frame's drawing;
// seglKeptMatrices() gives them the matrices in force, once a frame before anything is drawn.
int seglKeep(int count, int quads);
void seglKeptQuads(int index, const void* vertices, int first_quad, int quads, GLuint name, unsigned grey, int keyed);
// A kept command is drawn with no blending and a paletted texture unless this says otherwise:
// the kind of blending as glBlendFunc has them (1 by alpha, 2 added, 3 multiplied; all with
// the depth test), and whether its texture is words (0xTTRRGGBB) instead of palette indices.
void seglKeptBlend(int index, int kind, int words);
void seglKeptShown(int index, int quads);   // how many of its quads are drawn: 0 hides it
void seglKeptGrey(int index, unsigned grey);
// The same in one of the two sets only (seglSet() is the one the frame being built is in): a
// program that changes many greys a frame keeps count for each set, and stores half as much.
int seglSet(void);
void seglKeptGreyIn(int set, int index, unsigned grey);
void seglKeptTexture(int index, GLuint name);
// Lays its texture on the picture instead of on its surface (a sky that still hides what is
// behind): the texture coordinates at the picture's top left, and how far they go in 1,024 pixels.
void seglKeptLaid(int index, GLfixed u, GLfixed v, GLfixed du, GLfixed dv);
void seglKeptMatrices(void);

#endif
