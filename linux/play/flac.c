/*
 * dr_flac (https://github.com/mackron/dr_libs, public domain or MIT No Attribution) for
 * nxplay: a FLAC file in memory, read as 16-bit samples. build.sh fetches the header.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include "dr_flac.h"
#include "flac.h"

static drflac *flac;

int
flac_open(const void *file, unsigned size, int *rate, int *channels, unsigned *frames)
{
	flac = drflac_open_memory(file, size, NULL);
	if (!flac)
		return 0;
	*rate = (int)flac->sampleRate;
	*channels = flac->channels;
	*frames = flac->totalPCMFrameCount > 0xfffffffful ? 0 : (unsigned)flac->totalPCMFrameCount;
	return 1;
}

int
flac_read(short *to, int frames)
{
	return (int)drflac_read_pcm_frames_s16(flac, (drflac_uint64)frames, to);
}

unsigned
flac_seek(unsigned frame)
{
	if (drflac_seek_to_pcm_frame(flac, frame))
		return frame;
	drflac_seek_to_pcm_frame(flac, 0);
	return 0;
}

void
flac_close(void)
{
	if (flac)
		drflac_close(flac);
	flac = NULL;
}
