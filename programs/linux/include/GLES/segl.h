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
