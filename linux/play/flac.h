/* A FLAC file in memory (flac.c, over dr_flac): one at a time. */
#ifndef NXPLAY_FLAC_H
#define NXPLAY_FLAC_H

/* True if it is one; `frames` is 0 when the file does not say how long it is. */
int flac_open(const void *file, unsigned size, int *rate, int *channels, unsigned *frames);
/* The next frames, as many 16-bit samples each as the file has channels; fewer at the end. */
int flac_read(short *to, int frames);
/* Where it then is. */
unsigned flac_seek(unsigned frame);
void flac_close(void);

#endif
