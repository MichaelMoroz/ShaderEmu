/*
 * minimp3 (https://github.com/lieff/minimp3, CC0) for nxplay, compiled twice: as it is
 * (mp3full_frame) and with MP3D_HALF (mp3half_frame), which is minimp3.patch's half rate.
 * build.sh fetches the header. No SIMD: the machine has scalar single floats.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#ifdef MP3D_HALF
#define mp3dec_init mp3half_init
#define mp3dec_decode_frame mp3half_frame
#else
#define mp3dec_init mp3full_init
#define mp3dec_decode_frame mp3full_frame
#endif
#include "minimp3.h"
