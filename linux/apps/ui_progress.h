/* ui_progress.h: a small window that says what is being loaded or unpacked and how far it has
 * come, a bar and a line under it. It opens only after PROGRESS_LATER ms (a short job shows
 * nothing) and only where a window system answers. Closing it (its frame's button, Alt+F4)
 * takes it away at once and sets progress_closed: the job is then to stop. */
#ifndef SHADEREMU_APPS_UI_PROGRESS_H
#define SHADEREMU_APPS_UI_PROGRESS_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "nano-X.h"
#include "ui.h"

#define PROGRESS_LATER 700
#define PROGRESS_W 380
#define PROGRESS_H 78

static GR_WINDOW_ID progress_window;
static int progress_state;	/* 0 not begun, 1 waiting to show, 2 shown, -1 no window system */
static unsigned progress_began, progress_drawn = ~0u, progress_mapped;
static char progress_title[120], progress_line[80];
static int progress_closed, progress_connected;	/* closed by its user; a connection is open */
static void progress_end(void);

static unsigned
progress_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* The window's events, between steps of the job: uncovered, it is drawn whole; closed, it goes. */
static void
progress_poll(void)
{
	GR_EVENT event;

	while (progress_state == 2) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_NONE)
			break;
		if (event.type == GR_EVENT_TYPE_EXPOSURE)
			progress_drawn = ~0u;
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ) {
			progress_closed = 1;
			progress_end();
		}
	}
}

/* What the window will say it is doing ("Loading picture.png"). */
static void
progress_begin(const char *title)
{
	snprintf(progress_title, sizeof progress_title, "%s", title);
	progress_began = progress_ms();
	progress_state = 1;
}

/* How far: `done` of `total` (a total of 0: the bar is empty, the line says why). */
static void
progress_show(unsigned long long done, unsigned long long total, const char *line)
{
	unsigned share = total ? (unsigned)(done * 1000 / total) : 0;
	int bar = PROGRESS_W - 24;

	if (progress_state == 1 && progress_ms() - progress_began >= PROGRESS_LATER) {
		const GR_SCREEN_INFO *screen;

		progress_state = -1;
		if (GrOpen() < 0)
			return;
		progress_connected = 1;
		ui_init();
		screen = ui_screen();
		progress_window = GrNewWindowEx(UI_FIXED, progress_title, GR_ROOT_WINDOW_ID, (screen->cols - PROGRESS_W) / 2,
			(screen->rows - PROGRESS_H) / 3, PROGRESS_W, PROGRESS_H, UI_FACE);
		GrSelectEvents(progress_window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_CLOSE_REQ);
		GrMapWindow(progress_window);
		progress_mapped = progress_ms();
		progress_state = 2;
		progress_drawn = ~0u;
	}
	progress_poll();
	if (progress_state != 2)
		return;
	/* (drawn again for its first moments: the frame put round it after it was drawn cleared it) */
	if (progress_ms() - progress_mapped < 1500)
		progress_drawn = ~0u;
	if (share == progress_drawn && !strcmp(line, progress_line))
		return;
	snprintf(progress_line, sizeof progress_line, "%s", line);
	ui_fill(progress_window, 0, 0, PROGRESS_W, PROGRESS_H, UI_FACE);
	{	/* (cut with "..." to what fits: the font's widths as the window system measures them) */
		char cut[sizeof progress_title + 4];	/* (room for its "...") */
		int n = (int)strlen(progress_title), tw, th, tb;

		strcpy(cut, progress_title);
		GrSetGCFont(ui_gc, ui_var);
		GrGetGCTextSize(ui_gc, cut, n, GR_TFASCII, &tw, &th, &tb);
		while (n > 0 && tw > PROGRESS_W - 24) {
			strcpy(cut + --n, "...");
			GrGetGCTextSize(ui_gc, cut, n + 3, GR_TFASCII, &tw, &th, &tb);
		}
		ui_text(progress_window, 12, 8, cut, -1, BLACK, 0);
	}
	ui_bevel(progress_window, 12, 28, bar, 18, 1);
	ui_fill(progress_window, 13, 29, bar - 2, 16, WHITE);
	if (share)
		ui_fill(progress_window, 13, 29, (int)((bar - 2) * (unsigned long long)share / 1000), 16, GR_RGB(0, 0, 128));
	ui_text_fit(progress_window, 12, 54, PROGRESS_W - 24, progress_line, BLACK);
	GrFlush();
	progress_drawn = share;
}

static void
progress_end(void)
{
	if (progress_state == 2) {
		/* (the connection is kept for the program's next window: before the fix of 18 October
		   2026, one opened again after GrClose was sent no events, event_asked in client.c) */
		GrDestroyWindow(progress_window);
		GrFlush();
	}
	progress_state = 0;
}

/* "1.2 of 12.0 MB" */
static const char *
progress_megabytes(unsigned long long done, unsigned long long total)
{
	static char line[48];

	snprintf(line, sizeof line, "%llu.%llu of %llu.%llu MB", done >> 20, (done & 0xfffff) * 10 >> 20, total >> 20,
		 (total & 0xfffff) * 10 >> 20);
	return line;
}

#endif
