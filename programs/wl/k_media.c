/*
 * media_inflate, media_jpeg: what loading and the desktop's pictures spend on, by the machine's own libmedia
 * (linux/libmedia), linked from each image's libmedia.a as its programs link it. The picture is made here
 * (160 x 120: gradients, shapes and some noise, as a photo-like screenshot) and encoded in setup by
 * libmedia's own encoders: media_png_encode (level 6) and media_jpeg_encode (quality 85, its restart
 * intervals). A unit of media_inflate: the PNG's zlib stream inflated whole by media_inflate_one, on the core
 * that calls (the unfiltering is not in it). A unit of media_jpeg: the JPEG decoded whole by m_jpeg_decode_alone.
 */
#include <stdlib.h>
#include <string.h>
#include "wl.h"
#include "media.h"

uint32_t m_jpeg_room(void);   /* (m_int.h) */
const char *m_jpeg_decode_alone(const uint8_t *f, uint32_t size, struct media_image *out, void *room);

#define WIDTH 160
#define HEIGHT 120

static uint8_t *zlib_stream, *jpeg_file;
static uint32_t zlib_size, jpeg_size, raw_size;

static void make_picture(struct media_image *image)
{
    uint32_t seed = 0x9f07;
    image->width = WIDTH, image->height = HEIGHT, image->channels = 3, image->stride = WIDTH * 3;
    image->pixels = malloc(WIDTH * HEIGHT * 3);
    for (int y = 0; y < HEIGHT; y++)
        for (int x = 0; x < WIDTH; x++) {
            uint8_t *p = image->pixels + y * WIDTH * 3 + x * 3;
            int r = x * 255 / WIDTH, g = y * 255 / HEIGHT, b = (x + y) & 255;
            int dx = x - 80, dy = y - 60;
            if (dx * dx + dy * dy < 35 * 35) r = 230, g = 200 - dy, b = 40;
            if (x > 15 && x < 60 && y > 75 && y < 110) r = g = b = 60 + (x / 8 % 2) * 120;
            int noise = (int)(wl_rand(&seed) % 17) - 8;
            p[0] = (uint8_t)(r + noise < 0 ? 0 : r + noise > 255 ? 255 : r + noise);
            p[1] = (uint8_t)(g + noise < 0 ? 0 : g + noise > 255 ? 255 : g + noise);
            p[2] = (uint8_t)b;
        }
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void setup_inflate(void)
{
    struct media_image image;
    uint32_t png_size;
    memset(&image, 0, sizeof image);
    make_picture(&image);
    uint8_t *png = media_png_encode(&image, 6, &png_size, 0);
    /* the IDAT chunks joined: one zlib stream; its two bytes of header off, a raw deflate stream */
    zlib_stream = wl_alloc(png_size);
    zlib_size = 0;
    for (uint32_t at = 8; at + 12 <= png_size;) {
        uint32_t len = be32(png + at);
        if (!memcmp(png + at + 4, "IDAT", 4)) memcpy(zlib_stream + zlib_size, png + at + 8, len), zlib_size += len;
        at += 12 + len;
    }
    raw_size = (WIDTH * 3 + 1) * HEIGHT;
    free(image.pixels);
}

static void *inst_inflate(void)
{
    uint8_t *s = wl_alloc(raw_size + 64 + media_inflate_room());
    return s;
}

static uint32_t run_inflate(void *ctx)
{
    uint8_t *out = ctx, *room = out + ((raw_size + 63) & ~15u);
    uint32_t made = 0, used = 0, crc = 0;
    const char *why = media_inflate_one(zlib_stream + 2, zlib_size - 2, out, raw_size, &made, &used, &crc, room);
    return crc ^ made ^ (why ? 0xdead : 0);
}

static void setup_jpeg(void)
{
    struct media_image image;
    memset(&image, 0, sizeof image);
    make_picture(&image);
    uint8_t *jpeg = media_jpeg_encode(&image, 85, &jpeg_size, 0);
    jpeg_file = wl_alloc(jpeg_size);
    memcpy(jpeg_file, jpeg, jpeg_size);
    free(image.pixels);
    (void)m_jpeg_room();   /* (its colour tables made here, on the first core) */
}

struct jpeg_scratch { struct media_image out; uint8_t *room; };

static void *inst_jpeg(void)
{
    struct jpeg_scratch *s = wl_alloc(sizeof *s);
    s->out.width = WIDTH, s->out.height = HEIGHT, s->out.channels = 4, s->out.stride = WIDTH * 4;
    s->out.pixels = wl_alloc(WIDTH * HEIGHT * 4);
    s->room = wl_alloc(m_jpeg_room());
    return s;
}

static uint32_t run_jpeg(void *ctx)
{
    struct jpeg_scratch *s = ctx;
    uint32_t sum = 0;
    const char *why = m_jpeg_decode_alone(jpeg_file, jpeg_size, &s->out, s->room);
    for (int i = 0; i < WIDTH * HEIGHT * 4; i += 97) sum = sum * 31 + s->out.pixels[i];
    return sum ^ (why ? 0xdead : 0);
}

const wl_kernel wl_media_inflate = {"media_inflate", "libmedia: media_inflate_one of a PNG's 58 KB (loading, unarc)", setup_inflate, inst_inflate, run_inflate};
const wl_kernel wl_media_jpeg = {"media_jpeg", "libmedia: m_jpeg_decode_alone of a 160 x 120 JPEG", setup_jpeg, inst_jpeg, run_jpeg};
