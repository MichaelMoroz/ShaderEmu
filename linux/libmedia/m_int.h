/* libmedia's own: what its parts share (docs/system/media.md). */
#ifndef SHADEREMU_MEDIA_INT_H
#define SHADEREMU_MEDIA_INT_H

#include <stdint.h>
#include <string.h>
#include "media.h"
#include "mcwx.h"

#define M_MOST_SIDE 8192	/* a picture larger than this either way is refused */

static inline uint32_t m_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint32_t m_le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static inline uint32_t m_le16(const uint8_t *p) { return (uint32_t)p[1] << 8 | p[0]; }
static inline uint8_t m_clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

/* Workers for one call: the program's own when it has them open, else opened here (and closed by m_workers_end). */
int m_workers_begin(int want);
void m_workers_end(void);
/* With workers: waits for a count another core keeps (looking after the workers on the first core). */
int m_wait(volatile uint32_t *count, uint32_t least, volatile uint32_t *failed);
/* fn over 0..n-1 on this core and up to `workers` more; a piece is whole 16 bytes of an output
 * whose items are out_bytes long (0: no output in rows). */
void m_for(uint32_t n, mcw_body fn, void *ctx, uint32_t out_bytes, int workers);
/* Items 0..n-1 of a pipeline, item i a core's in turn (core 0 has item 0), each run in order on
 * its core: an item may wait for lower ones. Busy workers are stepped round, never posted to. */
void m_stages(uint32_t n, mcw_fn stage, int workers);
/* The first core helps workers that stopped at a page (a worker never does). */
void m_look(void);
/* How far a decode is, for the program (media_progress), from the first core only. */
void m_progress(uint32_t done, uint32_t total);

/* Memory with its pages there, mapped (and unmapped by m_unmap); NULL when there is none. */
void *m_map(uint32_t bytes);
void m_unmap(void *memory, uint32_t bytes);
/* Memory kept from one call to the next, not cleared (one slot a use); m_scratch_done after. */
enum { M_PNG_RAW, M_PNG_ROWS, M_PNG_TABLES, M_JPEG_INTERVALS, M_JPEG_FOUND, M_JPEG_COUNTS, M_JPEG_STRIPS, M_SLOTS };
void *m_scratch(int slot, uint32_t bytes);
void m_scratch_done(int slot, void *memory, uint32_t bytes);

/* One deflate stream on every core, without its CRC-32 (m_inflate.c): what `made` says is all of it, or a reason. */
const char *m_inflate_stream_plain(const void *in, uint32_t in_size, void *out, uint32_t out_size, uint32_t *made, int workers);

/* The output's pixel for red, green, blue, alpha (3 or 4 channels, either order). */
static inline void
m_put(const struct media_image *o, uint8_t *to, int r, int g, int b, int a)
{
	if (o->channels == 4 && !(o->opaque && a != 255)) {
		if (o->bgr) {
			to[0] = (uint8_t)b, to[1] = (uint8_t)g, to[2] = (uint8_t)r, to[3] = (uint8_t)a;
		} else {
			to[0] = (uint8_t)r, to[1] = (uint8_t)g, to[2] = (uint8_t)b, to[3] = (uint8_t)a;
		}
		return;
	}
	if (a != 255) {
		int br = o->background >> 16 & 255, bg = o->background >> 8 & 255, bb = o->background & 255;

		r = (r * a + br * (255 - a) + 127) / 255;
		g = (g * a + bg * (255 - a) + 127) / 255;
		b = (b * a + bb * (255 - a) + 127) / 255;
	}
	if (o->channels == 4) {
		if (o->bgr)
			to[0] = (uint8_t)b, to[1] = (uint8_t)g, to[2] = (uint8_t)r, to[3] = 255;
		else
			to[0] = (uint8_t)r, to[1] = (uint8_t)g, to[2] = (uint8_t)b, to[3] = 255;
		return;
	}
	to[0] = (uint8_t)r, to[1] = (uint8_t)g, to[2] = (uint8_t)b;
}

/* The decoders. Each fills `out` (pixels there, of out->width x out->height). */
const char *m_png_probe(const uint8_t *f, uint32_t size, struct media_info *info);
const char *m_png_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers);
const char *m_jpeg_probe(const uint8_t *f, uint32_t size, struct media_info *info);
const char *m_jpeg_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers);
uint32_t m_jpeg_room(void);
uint32_t m_jpeg_parts_room(uint32_t size);
uint32_t m_jpeg_parts(const uint8_t *f, uint32_t size, struct media_image *out, void *room);
int m_jpeg_part(void *room, uint32_t from, uint32_t to, int clear);
const char *m_jpeg_decode_alone(const uint8_t *f, uint32_t size, struct media_image *out, void *room);
const char *m_jpeg_decode_shrunk(const uint8_t *f, uint32_t size, struct media_image *out, int workers, int shrink);
const char *m_gif_probe(const uint8_t *f, uint32_t size, struct media_info *info);
const char *m_gif_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers);
const char *m_simple_probe(const uint8_t *f, uint32_t size, struct media_info *info);
const char *m_simple_decode(const uint8_t *f, uint32_t size, struct media_image *out, int workers);
/* How long an animation's picture n is shown, in milliseconds (a film's clock: m_video.c). */
int m_anim_delay(const struct media_anim *a, int n);

#endif
