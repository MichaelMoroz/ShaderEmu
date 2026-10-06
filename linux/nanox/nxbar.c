/*
 * nxbar: a bar along the bottom of the screen with a Start button, a menu of the programs
 * in the image, and a clock.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"

#define BAR_HEIGHT 26
#define START_WIDTH 58
#define ITEM_HEIGHT 20
#define MENU_WIDTH 130
#define CLOCK_WIDTH 52
#define FACE MWRGB(192, 192, 192)
#define SHADOW MWRGB(128, 128, 128)
#define SELECTED MWRGB(0, 0, 128)

static const struct { const char *label, *program; } items[] = {
	{ "Terminal", "nxterm" },
	{ "Calculator", "nxcalc" },
	{ "Clock", "nxclock" },
	{ "Eyes", "nxeyes" },
	{ "Tetris", "nxtetris" },
	{ "Mines", "nxmine" },
	{ "Roaches", "nxroach" },
	{ "Gears (OpenGL)", "glxgears" },
};
#define ITEMS ((int)(sizeof(items) / sizeof(items[0])))

static GR_WINDOW_ID bar, menu;
static GR_GC_ID gc;
static GR_SCREEN_INFO screen;
static int menu_open, hover = -1;

/* A rectangle with a lit top-left and a shaded bottom-right edge, or the reverse. */
static void
bevel(GR_WINDOW_ID w, int x, int y, int width, int height, int pressed)
{
	GrSetGCForeground(gc, pressed ? SHADOW : WHITE);
	GrLine(w, gc, x, y, x + width - 1, y);
	GrLine(w, gc, x, y, x, y + height - 1);
	GrSetGCForeground(gc, pressed ? WHITE : SHADOW);
	GrLine(w, gc, x, y + height - 1, x + width - 1, y + height - 1);
	GrLine(w, gc, x + width - 1, y, x + width - 1, y + height - 1);
}

static void
draw_bar(void)
{
	char text[16];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	GrSetGCForeground(gc, FACE);
	GrFillRect(bar, gc, 0, 0, screen.cols, BAR_HEIGHT);
	GrSetGCForeground(gc, WHITE);
	GrLine(bar, gc, 0, 0, screen.cols - 1, 0);
	bevel(bar, 3, 3, START_WIDTH, BAR_HEIGHT - 6, menu_open);
	GrSetGCForeground(gc, BLACK);
	GrText(bar, gc, 16, 7, (void *)"Start", 5, GR_TFASCII | GR_TFTOP);
	bevel(bar, screen.cols - CLOCK_WIDTH - 3, 3, CLOCK_WIDTH, BAR_HEIGHT - 6, 1);
	snprintf(text, sizeof(text), "%02d:%02d", tm->tm_hour, tm->tm_min);
	GrSetGCForeground(gc, BLACK);
	GrText(bar, gc, screen.cols - CLOCK_WIDTH + 8, 7, text, strlen(text), GR_TFASCII | GR_TFTOP);
}

static void
draw_menu(void)
{
	int i;

	GrSetGCForeground(gc, FACE);
	GrFillRect(menu, gc, 0, 0, MENU_WIDTH, ITEMS * ITEM_HEIGHT + 4);
	bevel(menu, 0, 0, MENU_WIDTH, ITEMS * ITEM_HEIGHT + 4, 0);
	for (i = 0; i < ITEMS; i++) {
		if (i == hover) {
			GrSetGCForeground(gc, SELECTED);
			GrFillRect(menu, gc, 2, 2 + i * ITEM_HEIGHT, MENU_WIDTH - 4, ITEM_HEIGHT);
		}
		GrSetGCForeground(gc, i == hover ? WHITE : BLACK);
		GrText(menu, gc, 10, 5 + i * ITEM_HEIGHT, (void *)items[i].label, strlen(items[i].label),
			GR_TFASCII | GR_TFTOP);
	}
}

static void
show_menu(int open)
{
	menu_open = open;
	hover = -1;
	if (open) {
		GrMapWindow(menu);
		GrRaiseWindow(menu);
	} else
		GrUnmapWindow(menu);
	draw_bar();
}

static void
launch(const char *program)
{
	if (fork() == 0) {
		execlp(program, program, (char *)NULL);
		_exit(127);
	}
}

int
main(void)
{
	GR_EVENT event;
	int minute = -1;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxbar: cannot reach the Nano-X server\n");
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);	/* started programs are not ours to wait for */
	GrGetScreenInfo(&screen);
	gc = GrNewGC();
	GrSetGCUseBackground(gc, GR_FALSE);

	bar = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NOFOCUS,
		"nxbar", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT, screen.cols, BAR_HEIGHT, FACE);
	menu = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NOFOCUS,
		"nxbar menu", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT - ITEMS * ITEM_HEIGHT - 4,
		MENU_WIDTH, ITEMS * ITEM_HEIGHT + 4, FACE);
	GrSelectEvents(bar, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN);
	GrSelectEvents(menu, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_MOUSE_MOTION |
		GR_EVENT_MASK_MOUSE_EXIT);
	GrMapWindow(bar);

	for (;;) {
		time_t now;

		GrGetNextEventTimeout(&event, 5000);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == bar)
				draw_bar();
			else
				draw_menu();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid == bar) {
				if (event.button.x < START_WIDTH + 6)
					show_menu(!menu_open);
				else if (menu_open)
					show_menu(0);
			} else {
				int i = (event.button.y - 2) / ITEM_HEIGHT;

				show_menu(0);
				if (i >= 0 && i < ITEMS)
					launch(items[i].program);
			}
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (event.mouse.wid == menu && hover != (event.mouse.y - 2) / ITEM_HEIGHT) {
				hover = (event.mouse.y - 2) / ITEM_HEIGHT;
				draw_menu();
			}
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			if (menu_open && event.general.wid == menu)
				show_menu(0);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
		now = time(NULL);
		if (localtime(&now)->tm_min != minute) {
			minute = localtime(&now)->tm_min;
			draw_bar();
		}
	}
}
