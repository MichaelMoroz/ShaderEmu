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

// Vertices the program has made compact itself (docs/gpu.md: x, y, z, then u | v << 16 in
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
// Triangles of tagged vertices (docs/gpu.md): the fourth word is u | v << 12 | tag << 24. With
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
// A layer of tiles for seglTexturePointer (docs/gpu.md, fragment mode 5): `pixels` is four
// words in memory from seglMemory(), the addresses (seglAddress()) of its cells, its tiles
// and its palette, and tile width | height << 8 | cells in a row << 16 | n << 28 (n not 0:
// the tiles are in pieces of 2^n, and their address is a table of the pieces'). A cell is 16 bits:
// a tile's number, bit 10 mirrors it across and bit 11 down, bits 12-15 a bank of 16 colours.
// A tile is width x height bytes; the palette is words of 0x00RRGGBB. The texture's width and
// height are the layer's in pixels; seglQuad draws it, keyed on a tile's byte.
#define SEGL_TILES 0x1F0B2
// A rectangle of a texture drawn over what the list drew before it, in the list's order: box is
// x0, y0, x1, y1 as seglQuad's corners are, texels the same corners in 1,024ths of the texture
// (texels, for one 1,024 wide). The colour is 0xTTRRGGBB, T being transparency; it tints an
// 8-bit or RGBA texture and is what a SEGL_BITS texture draws.
void seglSprite(const GLfixed* box, const int* texels, GLuint name, unsigned colour, int keyed);
// The command and the four vertices the last seglQuad or seglSprite wrote, and the address a
// command holds for memory from seglMemory(): for changing a list that is drawn again.
unsigned int* seglLastCommand(void);
unsigned int* seglLastVertices(void);
unsigned int seglAddress(const void* memory);
// Triangles in the window's own pixels (x right, y down), drawn over the frame in the order
// given and blended by alpha: a user interface. A vertex is sixteen of the library's numbers
// (docs/gpu.md): x, y at 0-1, texture coordinates at 8-9, red, green, blue, alpha (0 to 1) at
// 12-15. Room for vertices, and in *room how many fit; then how many were written of them.
void* seglScreenSpace(GLsizei* room);
void seglScreenUsed(GLsizei count, GLuint name);   // (a multiple of three; a texture of words, or 0)

// Forgets what was drawn since the last swap. seglSwapAgain() has the list of the last swap
// drawn once more, as its memory (textures, commands, vertices) is now.
void seglDiscard(void);
void seglSwapAgain(void);

// Commands that stay (docs/gpu.md): quads of compact vertices in memory from seglMemory(), and
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
