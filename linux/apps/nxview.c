/*
 * nxview: a picture viewer. nxview FILE shows a PPM, PNG, JPEG, PGM, BMP, GIF or XPM picture,
 * as large as fits the window without changing its shape. A PNG or JPEG picture is decoded
 * on the worker cores into a PPM file in memory (ui_image.h), which the display then draws
 * as it draws any PPM. NXVIEW_WORKERS=N asks for N of them (3; 0 decodes on this core).
 * nxview --decode FILE OUT.ppm decodes without a window and says how long it took.
 */
#include <sys/time.h>
#include "ui.h"
#include "ui_image.h"

static GR_WINDOW_ID window;
static GR_IMAGE_ID image;
static GR_IMAGE_INFO info;
static const char *ppm;		/* the file, when the display draws it itself */
static const char *wrong;	/* why a PNG or JPEG picture cannot be shown */
static char decoded[64];	/* the PPM file made of one */
static int width, height;

static void
forget(void)
{
	if (decoded[0])
		unlink(decoded);
}

/* Decodes a PNG or JPEG file into a PPM file and says what that took. */
static const char *
decode(const char *from, const char *to, int *pw, int *ph)
{
	const char *asked = getenv("NXVIEW_WORKERS"), *why;
	int workers = asked ? atoi(asked) : 3;
	struct timeval then, now;
	unsigned then_n, now_n;

	gettimeofday(&then, NULL);
	__asm__ volatile("rdcycle %0" : "=r"(then_n));
	why = ui_image_to_ppm(from, to, pw, ph, workers);
	__asm__ volatile("rdcycle %0" : "=r"(now_n));
	gettimeofday(&now, NULL);
	if (!why) {
		/* a sum of the pixels, for a test to compare with the same decoder's on another machine */
		unsigned sum = 0, n = 0;
		FILE *file = getenv("NXVIEW_SUM") ? fopen(to, "rb") : NULL;
		int c;

		while (file && (c = fgetc(file)) != EOF)
			sum = (sum << 5 | sum >> 27) ^ (unsigned)c, n++;
		if (file)
			fclose(file);
		fprintf(stderr, "nxview: %s, %d x %d: decoded in %ld ms, %u thousand instructions on this core, asking for %d workers",
			from, *pw, *ph, (long)((now.tv_sec - then.tv_sec) * 1000 + (now.tv_usec - then.tv_usec) / 1000), (now_n - then_n) / 1000, workers);
		if (file)
			fprintf(stderr, "; sum %08x of %u bytes", sum, n);
		fprintf(stderr, "\n");
	}
	return why;
}

static int
is_coded(const char *name)
{
	size_t n = strlen(name);

	return (n > 4 && (!strcasecmp(name + n - 4, ".png") || !strcasecmp(name + n - 4, ".jpg"))) || (n > 5 && !strcasecmp(name + n - 5, ".jpeg"));
}

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
		ui_text(window, 10, 10, wrong ? "This is" : "cannot show this file", -1, WHITE, 0);
		if (wrong)
			ui_text(window, 10, 28, wrong, -1, WHITE, 0);
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

	if (argc > 3 && !strcmp(argv[1], "--decode")) {
		const char *why = decode(argv[2], argv[3], &pw, &ph);

		if (why)
			fprintf(stderr, "nxview: %s: %s\n", argv[2], why);
		return why != NULL;
	}
	if (GrOpen() < 0)
		return 1;
	ui_init();
	GrGetScreenInfo(&screen);
	width = 320;
	height = 240;
	if (argc > 1 && is_coded(argv[1])) {
		snprintf(decoded, sizeof decoded, "/tmp/nxview-%d.ppm", (int)getpid());
		atexit(forget);
		wrong = decode(argv[1], decoded, &pw, &ph);
		if (wrong) {
			decoded[0] = 0;
		} else {
			ppm = decoded;
			info.width = pw;
			info.height = ph;
		}
	} else if (argc > 1 && ui_picture_size(argv[1], &pw, &ph)) {
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
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
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
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.ch != MWKEY_ESCAPE && event.keystroke.ch != 'q')
				break;
			/* Escape and q close it */
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			forget();
			return 0;
		}
	}
}
