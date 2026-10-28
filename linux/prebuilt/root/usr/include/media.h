/*
 * libmedia: pictures, sound and films for every program of the machine (docs/system/media.md),
 * decoded and encoded on the worker cores (mcw.h). Link with -lmedia (and mcw.c's workers).
 *
 * `workers` everywhere is the most worker cores a call may use: it opens them when the program
 * has none open and closes them again; a program that has its own open lends them (0: this core).
 */
#ifndef SHADEREMU_MEDIA_H
#define SHADEREMU_MEDIA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- pictures */

enum media_kind {
	MEDIA_UNKNOWN, MEDIA_PNG, MEDIA_JPEG, MEDIA_GIF, MEDIA_BMP, MEDIA_PNM, MEDIA_TGA,
	MEDIA_WAV, MEDIA_FLAC, MEDIA_MP3, MEDIA_OGG, MEDIA_MOD,
	MEDIA_MPEG, MEDIA_VQA, MEDIA_AVI,
};

struct media_info {
	enum media_kind kind;
	int width, height;	/* a picture's or film's */
	int frames;		/* an animation's pictures (1 for a still one) */
	int alpha;		/* it has see-through pixels */
	int parallel;		/* laid out to be decoded wholly on the workers (restart markers, segments) */
};

/* Pixels: 3 bytes a pixel (red, green, blue: a PPM's, what the GPU draws in fragment mode 4)
 * or 4 (and alpha), rows `stride` bytes apart. A stride a multiple of 16 lets every core write rows. */
struct media_image {
	int width, height, channels, stride;
	uint8_t *pixels;
	uint32_t background;	/* 0xRRGGBB that see-through pixels are laid over for 3 channels (white by default) */
	int bgr;		/* 4 channels as 0x00RRGGBB words (blue first): the window system's pixels */
	int opaque;		/* 4 channels laid over the background too, alpha 255 */
	int owned;		/* the library mapped the pixels (media_image_free unmaps them) */
};

const char *media_kind_name(enum media_kind kind);
/* What a file in memory is, from its first bytes and its headers. NULL, or why not. */
const char *media_probe(const void *data, uint32_t size, struct media_info *info);
/* The kind a file name's ending asks for (".png" ...); MEDIA_UNKNOWN for another. */
enum media_kind media_kind_of_name(const char *path);

/*
 * Decodes a picture (an animation's first frame). `out->channels` 3 or 4; with `out->pixels`
 * NULL the library maps them (stride width * channels rounded to 16). NULL, or what is wrong.
 */
const char *media_image_decode(const void *data, uint32_t size, struct media_image *out, int workers);
const char *media_image_load(const char *path, struct media_image *out, int channels, int workers);
/*
 * Decodes a picture to the size `out` has (pixels given): scaled to cover it, its middle kept,
 * when `cover` and its shape is within a third of out's, else scaled to fit inside on the background.
 */
const char *media_image_decode_fit(const void *data, uint32_t size, struct media_image *out, int cover, int workers);
/* Pixels in memory to the size `out` has, the same way (smaller: means of the pixels under each). */
void media_image_fit(const struct media_image *in, struct media_image *out, int cover, int workers);
void media_image_free(struct media_image *image);
/* A picture file decoded into a PPM file (in /tmp it is RAM, which the display draws from). */
const char *media_image_to_ppm(const char *path, const char *ppm_path, int *width, int *height, int workers);

/* Encoders. A file the library maps (free with media_free); its length in *size. A PNG's level:
 * 1 looks for one earlier match a place (quick), up to 9 down chains of 2 to 1,024. */
uint8_t *media_png_encode(const struct media_image *image, int level, uint32_t *size, int workers);
uint8_t *media_jpeg_encode(const struct media_image *image, int quality, uint32_t *size, int workers);
uint8_t *media_bmp_encode(const struct media_image *image, uint32_t *size);
uint8_t *media_ppm_encode(const struct media_image *image, uint32_t *size);
/* A GIF: the picture's own colours when it has 256 or fewer, else 252 fixed ones, dithered. */
uint8_t *media_gif_encode(const struct media_image *image, uint32_t *size, int workers);
/* An animated GIF: count frames of one size, each with its own colours, shown delays_ms[n] each
 * (NULL: 100), the whole played loops times (0: for ever). */
uint8_t *media_gif_anim_encode(const struct media_image *frames, int count, const int *delays_ms, int loops, uint32_t *size,
			       int workers);
/* Writes a picture as its name's ending asks (.png .jpg .jpeg .gif .bmp .ppm; another: PPM). */
const char *media_image_save(const char *path, const struct media_image *image, int quality, int workers);
void media_free(void *memory, uint32_t size);
/* Called on the first core while a picture is decoded, when not NULL: `done` of `total`, or a
 * total of 0 where how far is not known (it is still at work). Often: the program draws no more
 * than it likes. */
extern void (*media_progress)(uint32_t done, uint32_t total);

/* Deflate in segments ("MDZ1", total, piece, count, count + 1 offsets, then raw deflate streams of
 * `piece` bytes each but the last): the inflated size, 0 for another file; inflated on the workers
 * into out (16-byte aligned: a segment a job). NULL, or what is wrong. */
uint32_t media_inflated_size(const void *file, uint32_t size);
const char *media_inflate(const void *file, uint32_t size, void *out, uint32_t out_size, int workers);
/* One raw deflate stream (a gzip member's, a zip entry's) inflated into out[0..out_size) on up to
 * `workers` more cores: the bytes made, the compressed bytes taken and the output's CRC-32 into
 * *made, *used and *crc (each may be NULL). NULL, or what is wrong. */
const char *media_inflate_stream(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used,
				 uint32_t *crc, int workers);
/* The same on this core alone (a worker's job too), in room of media_inflate_room() bytes, 16-aligned. */
uint32_t media_inflate_room(void);
const char *media_inflate_one(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, uint32_t *used,
			      uint32_t *crc, void *room);
/* CRC-32 (gzip's and zip's) of more bytes after crc (0 at first); of two pieces joined. */
uint32_t media_crc32(uint32_t crc, const void *data, uint32_t bytes);
uint32_t media_crc32_join(uint32_t crc1, uint32_t crc2, uint32_t bytes2);

/* ---------------------------------------------------------------- animations (GIF) */

struct media_anim;
/* Takes an animation apart and decodes every picture of it on the workers, ahead. */
struct media_anim *media_anim_open(const void *data, uint32_t size, int workers, const char **why);
int media_anim_frames(const struct media_anim *anim);
int media_anim_width(const struct media_anim *anim);
int media_anim_height(const struct media_anim *anim);
/* Picture n as a whole canvas, 4 bytes a pixel (rows width * 4 apart), and how long it is shown. */
const uint8_t *media_anim_frame(struct media_anim *anim, int n, int *delay_ms);
int media_anim_loops(const struct media_anim *anim);	/* 0: for ever */
void media_anim_close(struct media_anim *anim);

/* ---------------------------------------------------------------- sound */

struct media_audio;
struct media_audio_info {
	enum media_kind kind;
	int rate, channels;	/* the samples are 16-bit, `channels` a frame */
	uint32_t frames;	/* in all; 0 when the file does not say */
	uint32_t pieces;	/* pieces any core decodes by itself (0: only media_audio_read, in order) */
	int half;		/* an MP3 file decoded at half its rate (media_audio_half) */
};

/* WAV, FLAC, MP3, Ogg Vorbis, MOD. The file is read where it lies, until media_audio_close. */
struct media_audio *media_audio_open(const void *data, uint32_t size, const char **why);
void media_audio_get_info(const struct media_audio *audio, struct media_audio_info *info);
/* In order on this core: the next frames (fewer at the end); a seek says where it then is. */
int media_audio_read(struct media_audio *audio, int16_t *to, int frames);
uint32_t media_audio_seek(struct media_audio *audio, uint32_t frame);
/*
 * Pieces, for the workers. media_audio_decoders() on the first core makes `n` decoders (one a
 * core that decodes: decoder 0 for this one, k for worker k). Then on any core, any order:
 * a piece's first frame and frame count, and the piece decoded with decoder `d`.
 */
int media_audio_decoders(struct media_audio *audio, int n);
uint32_t media_audio_piece_first(const struct media_audio *audio, uint32_t piece);
uint32_t media_audio_piece_frames(const struct media_audio *audio, uint32_t piece);
int media_audio_piece(struct media_audio *audio, int d, uint32_t piece, int16_t *to);
/* The same a step at a time (an MP3 frame, a FLAC block...): from frame `done` of the piece
 * (0 at first) on; how many of its frames are done then. A job that may be dropped uses it. */
uint32_t media_audio_piece_step(struct media_audio *audio, int d, uint32_t piece, int16_t *to, uint32_t done);
/* The piece a frame is in. */
uint32_t media_audio_piece_of(const struct media_audio *audio, uint32_t frame);
/* Pieces about `frames` long (whole MP3 frames or FLAC blocks, whole 16 bytes): how many then. */
uint32_t media_audio_set_piece(struct media_audio *audio, uint32_t frames);
/* An MP3 file at half its rate (its lower 16 bands; the info and pieces halved): whether it took. */
int media_audio_half(struct media_audio *audio, int on);
/* The rate a tracker tune is mixed at (22,050 Hz unless asked): the rate it has then. */
int media_audio_mix_rate(struct media_audio *audio, int rate);
/* Every piece on the workers into `to` (frames * channels samples); the frames decoded. */
uint32_t media_audio_decode_all(struct media_audio *audio, int16_t *to, uint32_t most_frames, int workers);
void media_audio_close(struct media_audio *audio);

/* ---------------------------------------------------------------- films */

/*
 * MPEG-1 (closed groups of pictures, no B pictures: docs/apps/video.md), Westwood's VQA, MJPEG in
 * AVI and GIF animations (docs/system/media.md, "Films"). One MPEG-1 film is open at a time. While a film is open
 * its workers are its own: the program posts nothing to them.
 */
struct media_video;
struct media_video_info {
	enum media_kind kind;
	int width, height;
	int fps_milli;		/* pictures in a thousand seconds (an animation's: on the mean) */
	int frames;
	int format;		/* MEDIA_FRAME_* below */
	uint32_t frame_bytes;	/* one picture as kept, a multiple of 16 */
	int rate, channels;	/* the sound's; 0 without one */
	uint32_t sound_frames;
	int workers;		/* the workers it decodes on */
	int least_places;	/* media_video_places() wants at least so many */
};
/* RGBA: 4 bytes a pixel (red, green, blue, alpha). YUV420: luma, then blue and red chroma at
 * half the size each way (the GPU's SEGL_YUV420). PAL8: a byte a pixel, with a palette.
 * WORDS: 0x00RRGGBB words, the GPU's opaque GL_RGBA texels (MJPEG in AVI). */
enum { MEDIA_FRAME_RGBA, MEDIA_FRAME_YUV420, MEDIA_FRAME_PAL8, MEDIA_FRAME_WORDS };

struct media_video *media_video_open(const void *data, uint32_t size, int workers, const char **why);
void media_video_get_info(const struct media_video *video, struct media_video_info *info);
/*
 * Where pictures are to be put (memory a texture is, say), before the first picture is asked
 * for: picture n is made in places[n % slots], once the picture there has been let go. Without
 * it the library keeps them. Not for RGBA (an animation's picture is its canvas). NULL, or why not.
 */
const char *media_video_places(struct media_video *video, uint8_t *const *places, int slots);
/*
 * Picture n, decoded ahead on the workers: NULL while it is not ready yet. A PAL8 picture's
 * palette (256 x 0x00RRGGBB) in *palette. Pictures before n are let go; one before those let go
 * is decoded again from where it can be (media_video_seek).
 */
const uint8_t *media_video_frame(struct media_video *video, int n, const uint32_t **palette);
/* The last picture ready with none missing after the last asked for, and whether those being
 * decoded will be ready when a film played on from now comes to them (*in_time). */
int media_video_ready(struct media_video *video, int *in_time);
/* Drops what is decoded and decodes from picture n's group: the group's first picture. */
int media_video_seek(struct media_video *video, int n);
/* When picture n begins, in milliseconds from the film's start; the picture due at `ms`. */
uint32_t media_video_time(const struct media_video *video, int n);
int media_video_at(const struct media_video *video, uint32_t ms);
/* Frames of 16-bit sound from `first` decoded so far: how many could be copied into `to`. */
uint32_t media_video_sound(struct media_video *video, uint32_t first, int16_t *to, uint32_t most);
/* Lets the workers go on with the decoding (the first core calls it between frames). */
void media_video_pump(struct media_video *video);
void media_video_close(struct media_video *video);

/* ---------------------------------------------------------------- pieces for a game's own players */

/* Westwood's VQA 4x4 blocks: a frame's block rows over every core (red alert). */
void media_vq4_rows(const uint8_t *codebook, const uint8_t *pointers, uint8_t *out, unsigned blocks_per_row,
		    unsigned num_rows, unsigned out_width, int workers);
/* The same for 4x2 blocks, solid at a high pointer of 15 (tiberian dawn). */
void media_vq2_rows(const uint8_t *codebook, const uint8_t *pointers, uint8_t *out, unsigned blocks_per_row,
		    unsigned num_rows, unsigned out_width, int workers);
/* Westwood's LCW ("format 80"): what it unpacked. */
uint32_t media_lcw(const uint8_t *source, uint8_t *dest, uint32_t length);

/* A sum of bytes (FNV-1a), for tests that compare two machines' decodings. */
uint32_t media_sum(const void *data, uint32_t bytes, uint32_t sum);

#ifdef __cplusplus
}
#endif
#endif
