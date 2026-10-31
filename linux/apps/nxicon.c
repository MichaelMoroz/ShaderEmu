/*
 * nxicon: gives a program an icon of its own, at the end of its file, where the desktop, the
 * Start menu, the bar and a window's caption look for it (ui_icons.h).
 *
 *	nxicon PROGRAM PICTURE		the picture (any kind libmedia reads, any size; magenta and
 *					what is see-through are clear) becomes its icon
 *	nxicon PROGRAM			says whether it has one
 *	nxicon PROGRAM --none		takes it away
 *
 * The picture is brought to 32 x 32 and to 16 x 16 by taking the nearest pixel, so one drawn
 * at 32 x 32 is kept as it is.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "media.h"

#define MARK "NXICON1"
#define ICON_BYTES (32 * 32 * 3 + 16 * 16 * 3 + 8)

static int
has_icon(int fd, long *size)
{
	char mark[8];

	*size = lseek(fd, 0, SEEK_END);
	return *size >= ICON_BYTES && pread(fd, mark, 8, *size - 8) == 8 && !memcmp(mark, MARK, 8);
}

int
main(int argc, char **argv)
{
	unsigned char icon[ICON_BYTES], *to = icon;
	struct media_image picture;
	const char *why;
	int fd, w, h, size, x, y;
	long length;

	if (argc < 2) {
		fprintf(stderr, "nxicon PROGRAM [PICTURE | --none]: a program's own icon, at the end of its file\n");
		return 1;
	}
	if ((fd = open(argv[1], argc > 2 ? O_RDWR : O_RDONLY)) < 0) {
		perror(argv[1]);
		return 1;
	}
	if (argc < 3) {
		printf("%s has %s\n", argv[1], has_icon(fd, &length) ? "an icon of its own" : "no icon of its own");
		return 0;
	}
	/* (the old icon goes only when there is a new one to put) */
	if (strcmp(argv[2], "--none") && (why = media_image_load(argv[2], &picture, 4, 0)) != NULL) {
		fprintf(stderr, "nxicon: %s: %s\n", argv[2], why);
		return 1;
	}
	if (has_icon(fd, &length) && ftruncate(fd, length -= ICON_BYTES) != 0) {
		perror(argv[1]);
		return 1;
	}
	if (!strcmp(argv[2], "--none"))
		return 0;
	w = picture.width, h = picture.height;
	for (size = 32; size >= 16; size /= 2)
		for (y = 0; y < size; y++)
			for (x = 0; x < size; x++, to += 3) {
				const unsigned char *p = picture.pixels + (y * 2 + 1) * h / (size * 2) * picture.stride + (x * 2 + 1) * w / (size * 2) * 4;

				/* (see-through is the key colour, which the desktop leaves out) */
				if (p[3] < 128)
					to[0] = 255, to[1] = 0, to[2] = 255;
				else
					to[0] = p[0], to[1] = p[1], to[2] = p[2];
			}
	memcpy(to, MARK, 8);
	if (pwrite(fd, icon, sizeof icon, length) != (int)sizeof icon) {
		perror(argv[1]);
		return 1;
	}
	return 0;
}
