/*
 * nxsettings: the desktop's settings. Picks what the desktop shows (a picture from
 * /usr/share/wallpaper-*.ppm, or a plain colour) and says what the machine is.
 */
#include <dirent.h>
#include <sys/utsname.h>
#include "ui.h"

#define WIDTH 300
#define HEIGHT (24 + count * ROW + 60)	/* the list, then three lines about the machine */
#define ROW 20
#define MAX_CHOICES 19	/* what a 480-row screen has room for */

static struct { char label[40], value[200]; } choices[MAX_CHOICES];
static int count, chosen = -1;
static GR_WINDOW_ID window;

static void
add(const char *label, const char *value)
{
	if (count == MAX_CHOICES)
		return;
	snprintf(choices[count].label, sizeof choices[0].label, "%s", label);
	snprintf(choices[count].value, sizeof choices[0].value, "%s", value);
	count++;
}

/* The pictures in the image, named by what follows "wallpaper-", then some colours. */
static void
find_choices(void)
{
	struct dirent **names;
	int n = scandir("/usr/share", &names, NULL, alphasort), i;
	char label[64], path[300];

	for (i = 0; i < n; i++) {
		const char *name = names[i]->d_name;
		char *dot;

		if (strncmp(name, "wallpaper-", 10) != 0)
			continue;
		snprintf(label, sizeof label, "Picture: %s", name + 10);
		if ((dot = strrchr(label, '.')) != NULL)
			*dot = 0;
		snprintf(path, sizeof path, "/usr/share/%s", name);
		add(label, path);
	}
	add("Colour: teal", "#008080");
	add("Colour: slate", "#304050");
	add("Colour: black", "#000000");
}

static void
draw(void)
{
	struct utsname system;
	GR_SCREEN_INFO screen;
	char text[120];
	int i;

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	ui_text(window, 8, 6, "Desktop", -1, BLACK, 0);
	for (i = 0; i < count; i++)
		ui_button(window, 8, 24 + i * ROW, WIDTH - 16, ROW - 2, choices[i].label, i == chosen);
	GrGetScreenInfo(&screen);
	uname(&system);
	snprintf(text, sizeof text, "Screen %d x %d", screen.cols, screen.rows);
	ui_text(window, 8, HEIGHT - 52, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "%s %s, %s", system.sysname, system.release, system.machine);
	ui_text(window, 8, HEIGHT - 36, text, -1, BLACK, 0);
	ui_text(window, 8, HEIGHT - 20, "ShaderEmu: a RISC-V computer in a pixel shader", -1, UI_SHADOW, 0);
}

int
main(int argc, char **argv)
{
	GR_EVENT event;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	/* nxsettings apply: put up the desktop as last chosen, and go (the nx script does this) */
	if (argc > 1 && !strcmp(argv[1], "apply")) {
		ui_wallpaper_restore();
		GrFlush();
		GrClose();
		return 0;
	}
	find_choices();
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Settings", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		int i;

		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event))
				break;
			i = (event.button.y - 24) / ROW;
			if (event.button.y >= 24 && i < count) {
				chosen = i;
				draw();
				ui_wallpaper(choices[i].value, 1);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
