/*
 * What the desktop's small programs share: the grey button look, the two fonts, starting
 * another program, and putting a picture or a colour on the desktop.
 */
#ifndef SHADEREMU_APPS_UI_H
#define SHADEREMU_APPS_UI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"

#define UI_FACE MWRGB(192, 192, 192)
#define UI_SHADOW MWRGB(128, 128, 128)
#define UI_SELECTED MWRGB(0, 0, 128)
#define UI_WALLPAPER_CHOICE "/tmp/nxwallpaper"	/* what the desktop shows: a picture's path, or #RRGGBB */
#define UI_WALLPAPER_DEFAULT "/usr/share/wallpaper-fox.ppm"

static GR_GC_ID ui_gc;
static GR_FONT_ID ui_fixed, ui_var;
static int ui_fw, ui_fh;	/* a character of the fixed font */

static void
ui_init(void)
{
	GR_FONT_INFO info;

	ui_gc = GrNewGC();
	GrSetGCUseBackground(ui_gc, GR_FALSE);
	ui_var = GrCreateFontEx(GR_FONT_SYSTEM_VAR, 0, 0, NULL);
	ui_fixed = GrCreateFontEx(GR_FONT_SYSTEM_FIXED, 0, 0, NULL);
	GrGetFontInfo(ui_fixed, &info);
	ui_fw = info.maxwidth;
	ui_fh = info.height;
}

static void
ui_fill(GR_DRAW_ID w, int x, int y, int width, int height, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	GrFillRect(w, ui_gc, x, y, width, height);
}

/* Text with its top left corner at x, y; `fixed` picks the font. */
static void
ui_text(GR_DRAW_ID w, int x, int y, const char *text, int count, GR_COLOR colour, int fixed)
{
	GrSetGCFont(ui_gc, fixed ? ui_fixed : ui_var);
	GrSetGCForeground(ui_gc, colour);
	GrText(w, ui_gc, x, y, (void *)text, count < 0 ? (int)strlen(text) : count, GR_TFASCII | GR_TFTOP);
}

/* A rectangle with a lit top-left and a shaded bottom-right edge, or the reverse. */
static void
ui_bevel(GR_DRAW_ID w, int x, int y, int width, int height, int pressed)
{
	GrSetGCForeground(ui_gc, pressed ? UI_SHADOW : WHITE);
	GrLine(w, ui_gc, x, y, x + width - 1, y);
	GrLine(w, ui_gc, x, y, x, y + height - 1);
	GrSetGCForeground(ui_gc, pressed ? WHITE : UI_SHADOW);
	GrLine(w, ui_gc, x, y + height - 1, x + width - 1, y + height - 1);
	GrLine(w, ui_gc, x + width - 1, y, x + width - 1, y + height - 1);
}

static void
ui_button(GR_DRAW_ID w, int x, int y, int width, int height, const char *label, int pressed)
{
	ui_fill(w, x, y, width, height, UI_FACE);
	ui_bevel(w, x, y, width, height, pressed);
	ui_text(w, x + 6, y + (height - 12) / 2, label, -1, BLACK, 0);
}

static int
ui_inside(int px, int py, int x, int y, int width, int height)
{
	return px >= x && px < x + width && py >= y && py < y + height;
}

/* Starts a program with one argument (or none), without a shell. */
static void
ui_run(const char *program, const char *argument)
{
	if (fork() == 0) {
		execlp(program, program, argument, (char *)NULL);
		_exit(127);
	}
}

/* The size a PPM file says it has; 0 when it is not one. */
static int
ui_picture_size(const char *path, int *pw, int *ph)
{
	FILE *file = fopen(path, "rb");
	int most = 0, ok;

	if (!file)
		return 0;
	ok = fscanf(file, "P6 %d %d %d", pw, ph, &most) == 3 && *pw > 0 && *ph > 0;
	fclose(file);
	return ok;
}

/*
 * Draws a PPM file over a whole window of that size. A picture near the window's shape is
 * cropped to it (drawn larger, the window cutting it); another is shown whole, on `around`.
 */
static int
ui_picture(GR_DRAW_ID w, const char *path, int width, int height, GR_COLOR around)
{
	int pw, ph, dw = width, dh = height;

	if (!ui_picture_size(path, &pw, &ph))
		return 0;
	if (pw * height * 10 > ph * width * 13 || ph * width * 10 > pw * height * 13) {
		if (pw * height > ph * width)
			dh = ph * width / pw;
		else
			dw = pw * height / ph;
		ui_fill(w, 0, 0, width, height, around);
	} else if (pw * height > ph * width) {
		dw = pw * height / ph;
	} else {
		dh = ph * width / pw;
	}
	GrDrawImageFromFile(w, ui_gc, (width - dw) / 2, (height - dh) / 2, dw, dh, (char *)path, 0);
	return 1;
}

/* Shows a picture file, or a colour written #RRGGBB, as the desktop, and remembers the choice. */
static void
ui_wallpaper(const char *choice, int remember)
{
	GR_SCREEN_INFO screen;
	FILE *file;

	GrGetScreenInfo(&screen);
	if (choice[0] == '#') {
		unsigned long rgb = strtoul(choice + 1, NULL, 16);

		ui_fill(GR_ROOT_WINDOW_ID, 0, 0, screen.cols, screen.rows, MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
	} else {
		ui_picture(GR_ROOT_WINDOW_ID, choice, screen.cols, screen.rows, BLACK);
	}
	if (remember && (file = fopen(UI_WALLPAPER_CHOICE, "w")) != NULL) {
		fputs(choice, file);
		fclose(file);
	}
}

/* The desktop as last chosen, or the default picture. */
static void
ui_wallpaper_restore(void)
{
	char choice[200] = UI_WALLPAPER_DEFAULT;
	FILE *file = fopen(UI_WALLPAPER_CHOICE, "r");

	if (file) {
		if (fgets(choice, sizeof choice, file))
			choice[strcspn(choice, "\r\n")] = 0;
		fclose(file);
	}
	if (choice[0] != '#' && access(choice, R_OK) != 0)
		strcpy(choice, "#008080");	/* the picture is not in this image */
	ui_wallpaper(choice, 0);
}

#endif
