/*
 * nxbench: times the drawing a window system spends its life on (fills, text, scrolling),
 * through the Nano-X server. Prints milliseconds for each.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"

#define WIDTH 480
#define HEIGHT 320

static GR_WINDOW_ID window;
static GR_GC_ID gc;

static long
now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* Returns once the server has handled everything sent so far. */
static void
finish(void)
{
	GR_WINDOW_INFO info;

	GrGetWindowInfo(window, &info);
}

/* A panel of buttons, as a calculator or a dialog draws: face, four edges, a label. */
static void
buttons(int count)
{
	int i;

	for (i = 0; i < count; i++) {
		int x = 8 + (i % 7) * 66, y = 8 + (i / 7 % 10) * 30;

		GrSetGCForeground(gc, LTGRAY);
		GrFillRect(window, gc, x, y, 60, 24);
		GrSetGCForeground(gc, WHITE);
		GrLine(window, gc, x, y, x + 59, y);
		GrLine(window, gc, x, y, x, y + 23);
		GrSetGCForeground(gc, GRAY);
		GrLine(window, gc, x, y + 23, x + 59, y + 23);
		GrLine(window, gc, x + 59, y, x + 59, y + 23);
		GrSetGCForeground(gc, BLACK);
		GrText(window, gc, x + 20, y + 6, (void *)"sin", 3, GR_TFASCII | GR_TFTOP);
		if (i % 20 == 19)
			finish();
	}
	finish();
}

/* `nxbench hold` only puts the panel up and keeps it painted, as something to drag about. */
int
main(int argc, char **argv)
{
	static const char line[] = "The quick brown fox jumps over the lazy dog 0123456789";
	GR_EVENT event;
	long start;
	int i;

	if (GrOpen() < 0) {
		printf("nxbench: cannot reach the Nano-X server\n");
		return 1;
	}
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "nxbench", GR_ROOT_WINDOW_ID, 40, 40, WIDTH, HEIGHT, WHITE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE);
	GrMapWindow(window);
	gc = GrNewGC();
	do
		GrGetNextEvent(&event);
	while (event.type != GR_EVENT_TYPE_EXPOSURE);

	if (argc > 1) {
		GrSetGCUseBackground(gc, GR_FALSE);
		buttons(70);
		printf("nxbench: holding\n");
		for (;;) {
			GrGetNextEvent(&event);
			if (event.type == GR_EVENT_TYPE_EXPOSURE)
				buttons(70);
		}
	}

	start = now_ms();
	for (i = 0; i < 25; i++) {
		GrSetGCForeground(gc, (i & 1) ? WHITE : LTGRAY);
		GrFillRect(window, gc, 0, 0, WIDTH, HEIGHT);
		finish();
	}
	printf("nxbench: 25 fills of %dx%d: %ld ms\n", WIDTH, HEIGHT, now_ms() - start);

	GrSetGCForeground(gc, BLACK);
	GrSetGCUseBackground(gc, GR_FALSE);
	start = now_ms();
	for (i = 0; i < 100; i++) {
		GrText(window, gc, 4, (i % 24) * 13, (void *)line, strlen(line), GR_TFASCII | GR_TFTOP);
		if (i % 24 == 23)
			finish();
	}
	finish();
	printf("nxbench: 100 lines of text: %ld ms\n", now_ms() - start);

	start = now_ms();
	for (i = 0; i < 15; i++) {
		GrCopyArea(window, gc, 0, 0, WIDTH, HEIGHT - 13, window, 0, 13, MWROP_COPY);
		GrSetGCForeground(gc, WHITE);
		GrFillRect(window, gc, 0, HEIGHT - 13, WIDTH, 13);
		GrSetGCForeground(gc, BLACK);
		GrText(window, gc, 4, HEIGHT - 13, (void *)line, strlen(line), GR_TFASCII | GR_TFTOP);
		finish();
	}
	printf("nxbench: 15 scrolls by one line: %ld ms\n", now_ms() - start);
	start = now_ms();
	buttons(200);
	printf("nxbench: 200 buttons: %ld ms\n", now_ms() - start);

	usleep(300000);		/* let the server finish drawing; then leave the result up for a while */
	printf("nxbench: done\n");
	sleep(3);
	GrClose();
	return 0;
}
