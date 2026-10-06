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

// Texture memory the program writes itself: a pointer to `bytes` of GPU memory (16-byte
// aligned), or NULL when there is none left. It lasts until the program ends.
void* seglMemory(unsigned int bytes);
// Makes the bound texture those pixels, with no copy: GL_COLOR_INDEX8_EXT bytes or GL_RGBA
// words (0xTTRRGGBB, T being transparency), rows top to bottom.
void seglTexturePointer(const void* pixels, GLsizei width, GLsizei height, GLenum internal);
// The 256-word palette of paletted textures (0x00RRGGBB), for writing directly.
unsigned int* seglPalette(void);

#endif
