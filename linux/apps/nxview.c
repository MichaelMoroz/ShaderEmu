/*
 * nxview: a picture viewer. nxview FILE shows a PPM, PGM, BMP, GIF or XPM picture, as large
 * as fits the window without changing its shape.
 */
#include "ui.h"

static GR_WINDOW_ID window;
static GR_IMAGE_ID image;
static GR_IMAGE_INFO info;
static const char *ppm;		/* the file, when the display draws it itself */
static int width, height;

static void
draw(void)
{
	int w = width, h = height;

	if (ppm) {
		/* whole, as large as fits */
		w = info.width * height > info.height * width ? width : info.width * height / info.height;
		h = w * info.height / info.width;
		ui_fill(window, 0, 0, width, height, MWRGB(40, 40, 40));
		GrDrawImageFromFile(window, ui_gc, (width - w) / 2, (height - h) / 2, w, h, (char *)ppm, 0);
		return;
	}
	ui_fill(window, 0, 0, width, height, MWRGB(40, 40, 40));
	if (!image) {
		ui_text(window, 10, 10, "cannot show this file", -1, WHITE, 0);
		return;
	}
	/* the largest size of the picture's shape that fits */
	if (info.width * height > info.height * width)
		h = info.height * width / info.width;
	else
		w = info.width * height / info.height;
	GrDrawImageToFit(window, ui_gc, (width - w) / 2, (height - h) / 2, w, h, image);
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	GR_SCREEN_INFO screen;
	char title[300];
	int pw, ph;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	GrGetScreenInfo(&screen);
	width = 320;
	height = 240;
	if (argc > 1 && ui_picture_size(argv[1], &pw, &ph)) {
		ppm = argv[1];
		info.width = pw;
		info.height = ph;
	} else if (argc > 1 && (image = GrLoadImageFromFile(argv[1], 0)) != 0) {
		GrGetImageInfo(image, &info);
	}
	if (ppm || image) {
		/* the picture's own size, halved until it leaves room on the screen */
		width = info.width;
		height = info.height;
		while (width > screen.cols - 40 || height > screen.rows - 80) {
			width /= 2;
			height /= 2;
		}
	}
	snprintf(title, sizeof title, "%s - Viewer", argc > 1 ? argv[1] : "no file");
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, title, GR_ROOT_WINDOW_ID, -1, -1, width, height, MWRGB(40, 40, 40));
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE) {
				width = event.update.width;
				height = event.update.height;
				draw();
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
