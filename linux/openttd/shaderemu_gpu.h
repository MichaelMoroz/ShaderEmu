/* OpenTTD's picture drawn by the machine's GPU (docs/games/openttd.md, "The picture on the GPU"): the blitter
 * "8bpp-gpu" records every sprite, fill, line and pixel as a record of the GPU's sprite command, and the GPU
 * draws the indices into a picture of its own, which a second list shows through the palette. */

#ifndef VIDEO_SHADEREMU_GPU_H
#define VIDEO_SHADEREMU_GPU_H

#include <cstdint>
#include <vector>

/** For OPENTTD_PLAY's lines: lists to the GPU, records, and the rectangles and window paints of the dirty blocks. */
struct SeGpuCounts { uint32_t lists, records, rects, pixels, paints; };
extern SeGpuCounts _se_gpu_counts;
extern uint32_t _se_paints_of[256];

/** Whether the GPU draws the picture (the blitter is "8bpp-gpu"). */
bool SeGpuOn();
/** The video driver's part: the picture's size, and where its rows of words lie in GPU memory. */
bool SeGpuStart(int max_w, int max_h);
void SeGpuResize(int w, int h);
/** The frame's lists to the GPU, then the picture through the palette (256 words 0x00RRGGBB) into the window. */
void SeGpuPaint(const uint32_t *palette, unsigned window);
/** The frame drawn so far to the GPU, into the picture (no showing): SeGpuSettle, then SeGpuPicture reads it. */
void SeGpuSubmit();
/** A list that found the GPU busy, posted if it is free now. */
void SeGpuPoll();
/** Every list of ours drawn and its picture copied out. */
void SeGpuSettle();
/** The picture's indices as the GPU drew them (its remaps made), after a settle: w x h bytes. */
void SeGpuPicture(std::vector<uint8_t> &out);
/** (debugging) the records of the lists still held that cover pixel (x, y), printed. */
void SeGpuWho(int x, int y);
/** The mouse pointer's sprites are drawn: what was captured since CopyToBuffer is the pointer. */
void SeGpuPointerDrawn();

/* The viewport's pieces (viewport.cpp's ViewportFlush): each is drawn on some core into a range of its records,
 * and the pieces of a flush, which do not overlap, are put in the list together after the round. */
void SeGpuPiecesBegin(int pieces);
void SeGpuPieceBegin(int piece);
void SeGpuPieceEnd(int piece, bool failed);
void SeGpuPiecesEnd();

#endif /* VIDEO_SHADEREMU_GPU_H */
