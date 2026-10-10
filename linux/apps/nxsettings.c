/*
 * nxsettings: the desktop's settings, a tab each. Desktop: what the desktop shows (a picture
 * of the image's, any picture file, or a colour). Screen: its size. Sound: how loud. System:
 * what the machine is. From a script: nxsettings apply | size WxH | volume 0-100 | choose [DIR].
 */
#include <dirent.h>
#include <sys/utsname.h>
#include "ui.h"
#include "ui_files.h"
#include "shaderemu_sound.h"

#define WIDTH 380
#define HEIGHT 300
#define TAB_W 90
#define TOP 36			/* where a tab's own part begins */
#define MAX_CHOICES 64
#define SIZE_CHOICE "/tmp/nxsize"	/* the screen size the desktop starts with: WIDTHxHEIGHT (the nx script reads it) */

enum { TAB_DESKTOP, TAB_SCREEN, TAB_SOUND, TAB_SYSTEM, TABS };
static const char *tab_names[TABS] = { "Desktop", "Screen", "Sound", "System" };

static struct { char label[64], value[512]; } choices[MAX_CHOICES];
static struct ui_list pictures = { 8, TOP, WIDTH - 16, 196, 0, 0, -1 };
static const char *sizes[] = { "640x480", "800x600", "1024x576", "1024x768", "1280x720", "1600x900", "1920x1080" };
#define SIZES 7
static struct ui_list screens = { 8, TOP + 20, 170, SIZES * UI_ROW + 4, 0, 0, -1 };
static int fits[SIZES], most_w = 1280, most_h = 720;	/* the sizes this host can show, and its largest */
static int tab, have_sound = -1;	/* have_sound: not asked yet */
static int focus;	/* what the keyboard works: 0 the row of tabs, 1 and 2 the tab's controls in order */
static GR_WINDOW_ID window;

static void
add(const char *label, const char *value)
{
	if (pictures.count == MAX_CHOICES)
		return;
	snprintf(choices[pictures.count].label, sizeof choices[0].label, "%s", label);
	snprintf(choices[pictures.count].value, sizeof choices[0].value, "%s", value);
	pictures.count++;
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

static const char *
picture_label(int row)
{
	return choices[row].label;
}

/* The host's largest screen is in its flags, among the machine's control words (docs/gpu.md):
 * width and height in 16s of pixels. A host that does not say shows 1280 x 720. */
static void
find_sizes(void)
{
	int gpu = open("/dev/gpu", O_RDWR), i;
	const unsigned *words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000);

	if (words != MAP_FAILED && (words[0x3c / 4] >> 8 & 255)) {
		most_w = (words[0x3c / 4] >> 8 & 255) * 16;
		most_h = (words[0x3c / 4] >> 16 & 255) * 16;
	}
	if (words != MAP_FAILED)
		munmap((void *)words, 4096);
	if (gpu >= 0)
		close(gpu);
	for (i = 0; i < SIZES; i++)
		if (atoi(sizes[i]) <= most_w && atoi(strchr(sizes[i], 'x') + 1) <= most_h)
			fits[screens.count++] = i;
}

static const char *
size_label(int row)
{
	static char text[40];
	GR_SCREEN_INFO screen;
	int w = atoi(sizes[fits[row]]), h = atoi(strchr(sizes[fits[row]], 'x') + 1);

	GrGetScreenInfo(&screen);
	snprintf(text, sizeof text, "%d x %d%s", w, h, w == screen.cols && h == screen.rows ? "  (now)" : "");
	return text;
}

/* A number that follows `name` in a file of /proc. */
static long
field(const char *path, const char *name)
{
	FILE *file = fopen(path, "r");
	char line[160];
	long value = 0;

	while (file && fgets(line, sizeof line, file))
		if (!strncmp(line, name, strlen(name)))
			value = atol(line + strlen(name));
	if (file)
		fclose(file);
	return value;
}

/* The desktop again, with the size just written: the window system starts over and every
 * program on it closes, this one too. The script that does it must not die with them. */
static void
restart_desktop(const char *size)
{
	static int asked;
	FILE *file;

	if (asked)
		return;		/* (the button pressed again while the desktop is going) */
	asked = 1;
	file = fopen(SIZE_CHOICE, "w");
	if (file) {
		fputs(size, file);
		fclose(file);
	}
	if (fork() == 0) {
		setsid();
		execlp("nx", "nx", "restart", (char *)NULL);
		_exit(127);
	}
}

/* A short tone through the card, to hear the volume by. */
static void
beep(void)
{
	static short tone[4410];
	struct snd_format format = { 22050, 1 };
	int file = open("/dev/sound", O_WRONLY), i;

	if (file < 0)
		return;
	for (i = 0; i < 4410; i++) {
		int phase = i * 880 % 22050, up = phase * 4 / 22050;	/* a triangle wave of 880 a second */
		int v = (up == 0 ? phase : up == 3 ? phase - 22050 : 11025 - phase) * 4;

		tone[i] = (short)(v * (4410 - i) / 4410 / 4);
	}
	ioctl(file, SND_IOCTL_FORMAT, &format);
	write(file, tone, sizeof tone);
	close(file);	/* returns once it has been played */
}

static void
draw(void)
{
	struct utsname system;
	GR_SCREEN_INFO screen;
	char text[160];
	int i;

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	for (i = 0; i < TABS; i++)
		ui_button(window, 8 + i * (TAB_W + 2), 6, TAB_W, 22, tab_names[i], i == tab);
	if (focus == 0)
		ui_focus(window, 8 + tab * (TAB_W + 2) + 4, 6 + 4, TAB_W - 8, 22 - 8);
	GrGetScreenInfo(&screen);
	switch (tab) {
	case TAB_DESKTOP:
		ui_list_draw(window, &pictures, picture_label);
		ui_button(window, 8, TOP + 204, 180, 22, "Choose a picture file...", 0);
		if (focus == 1)
			ui_focus(window, pictures.x, pictures.y, pictures.width, pictures.height);
		else if (focus == 2)
			ui_focus(window, 8 + 4, TOP + 204 + 4, 180 - 8, 22 - 8);
		ui_text(window, 8, HEIGHT - 26, "A picture is a PPM file; others can be made into one on the host.", -1, UI_SHADOW, 0);
		break;
	case TAB_SCREEN:
		snprintf(text, sizeof text, "The screen is %d x %d now.", screen.cols, screen.rows);
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		ui_list_draw(window, &screens, size_label);
		ui_button(window, 190, TOP + 20, 180, 22, "Use the selected size", 0);
		if (focus == 1)
			ui_focus(window, screens.x, screens.y, screens.width, screens.height);
		else if (focus == 2)
			ui_focus(window, 190 + 4, TOP + 20 + 4, 180 - 8, 22 - 8);
		ui_text(window, 190, TOP + 52, "The desktop starts again:", -1, BLACK, 0);
		ui_text(window, 190, TOP + 68, "every open program closes.", -1, BLACK, 0);
		snprintf(text, sizeof text, "This machine's display shows %d x %d at most.", most_w, most_h);
		ui_text(window, 8, HEIGHT - 26, text, -1, UI_SHADOW, 0);
		break;
	case TAB_SOUND:
		if (have_sound <= 0) {
			ui_text(window, 8, TOP, "This machine has no sound card (or its host plays none).", -1, BLACK, 0);
			break;
		}
		snprintf(text, sizeof text, "Volume of everything the machine plays: %d%%", (int)(SND_MASTER * 100 / 256));
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		ui_slider(window, 8, TOP + 22, WIDTH - 16, (int)SND_MASTER, 256);
		ui_button(window, 8, TOP + 52, 90, 22, "Play a tone", 0);
		if (focus == 1)
			ui_focus(window, 8, TOP + 22, WIDTH - 16, 18);
		else if (focus == 2)
			ui_focus(window, 8 + 4, TOP + 52 + 4, 90 - 8, 22 - 8);
		snprintf(text, sizeof text, "The card plays %u samples a second.", (unsigned)SND_RATE);
		ui_text(window, 8, TOP + 90, text, -1, UI_SHADOW, 0);
		ui_text(window, 8, TOP + 106, "How loud the host plays it is set on the host.", -1, UI_SHADOW, 0);
		break;
	case TAB_SYSTEM:
		uname(&system);
		snprintf(text, sizeof text, "%s %s", system.sysname, system.release);
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "Processor: %s, one core, no floating point unit", system.machine);
		ui_text(window, 8, TOP + 18, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "Memory: %ld MB, %ld MB free", field("/proc/meminfo", "MemTotal:") / 1024,
			 field("/proc/meminfo", "MemAvailable:") / 1024);
		ui_text(window, 8, TOP + 36, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "Screen: %d x %d", screen.cols, screen.rows);
		ui_text(window, 8, TOP + 54, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "Running for %ld minutes", field("/proc/uptime", "") / 60);
		ui_text(window, 8, TOP + 72, text, -1, BLACK, 0);
		ui_text(window, 8, TOP + 100, "Settings last until the machine is switched off: its disk", -1, UI_SHADOW, 0);
		ui_text(window, 8, TOP + 116, "is a ROM, and what is written lives in memory.", -1, UI_SHADOW, 0);
		ui_text(window, 8, HEIGHT - 26, "ShaderEmu: a RISC-V computer in a pixel shader", -1, UI_SHADOW, 0);
		break;
	}
}

/* Events of this window that come while the file chooser is up. */
static void
while_choosing(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE)
		draw();
}

static void
choose_picture(void)
{
	char path[512], label[64];
	const char *name;

	if (!ui_choose_file("Choose a picture", "/usr/share", ".ppm", path, sizeof path, while_choosing))
		return;
	name = strrchr(path, '/') + 1;
	snprintf(label, sizeof label, "File: %s", name);
	add(label, path);
	pictures.selected = pictures.count - 1;
	ui_list_show(&pictures);
	ui_wallpaper(path, 1);
}

/* Shows a tab; the keyboard stays on the row of tabs, or goes there. */
static void
show_tab(int which)
{
	tab = which;
	focus = 0;
	if (tab == TAB_SOUND && have_sound < 0)
		have_sound = snd_open() == 0;
	draw();
}

/* How many places the keyboard can be on the tab shown: the row of tabs, then its controls. */
static int
controls(void)
{
	return tab == TAB_SYSTEM || (tab == TAB_SOUND && have_sound <= 0) ? 1 : 3;
}

/* Tab goes round the row of tabs and the controls of the tab shown; arrows change the tab,
 * the list's row or the slider; Enter or Space presses a button. */
static void
key(const GR_EVENT *event)
{
	int ch = event->keystroke.ch, value;

	if (ui_tab(event, &focus, controls())) {
		draw();
		return;
	}
	if (focus == 0) {
		if (ch == MWKEY_LEFT || ch == MWKEY_RIGHT)
			show_tab((tab + (ch == MWKEY_LEFT ? TABS - 1 : 1)) % TABS);
		return;
	}
	switch (tab) {
	case TAB_DESKTOP:
		if (focus == 2 && ui_press_key(ch))
			choose_picture();
		else if (focus == 1 && (ui_list_key(&pictures, ch) || ui_press_key(ch)) && pictures.selected >= 0)
			ui_wallpaper(choices[pictures.selected].value, 1);
		else
			break;
		draw();
		break;
	case TAB_SCREEN:
		if (ui_press_key(ch) && screens.selected >= 0)
			restart_desktop(sizes[fits[screens.selected]]);
		else if (focus == 1 && ui_list_key(&screens, ch))
			draw();
		break;
	case TAB_SOUND:
		if (focus == 2 && ui_press_key(ch)) {
			beep();
			break;
		}
		value = (int)SND_MASTER;
		value = ch == MWKEY_LEFT ? value - 8 : ch == MWKEY_RIGHT ? value + 8 : ch == MWKEY_HOME ? 0 : ch == MWKEY_END ? 256 : -1;
		if (focus == 1 && value != -1) {
			SND_MASTER = value < 0 ? 0 : value > 256 ? 256 : value;
			draw();
		}
		break;
	}
}

static void
press(GR_EVENT *event)
{
	int down = event->type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(event);
	int x = down ? event->button.x : event->mouse.x, y = down ? event->button.y : event->mouse.y, how;

	if (down && y >= 6 && y < 28 && x >= 8 && (x - 8) / (TAB_W + 2) < TABS) {
		show_tab((x - 8) / (TAB_W + 2));
		return;
	}
	switch (tab) {
	case TAB_DESKTOP:
		how = ui_list_event(&pictures, event);
		if (how == 1 && event->type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(event) && pictures.selected >= 0 &&
		    x < pictures.x + pictures.width - UI_SCROLL_W - 2)
			ui_wallpaper(choices[pictures.selected].value, 1);
		if (how) {
			focus = 1;
			draw();
		} else if (down && ui_inside(x, y, 8, TOP + 204, 180, 22)) {
			choose_picture();
			draw();
		}
		break;
	case TAB_SCREEN:
		if (ui_list_event(&screens, event)) {
			focus = 1;
			draw();
		} else if (down && ui_inside(x, y, 190, TOP + 20, 180, 22) && screens.selected >= 0)
			restart_desktop(sizes[fits[screens.selected]]);
		break;
	case TAB_SOUND:
		if (have_sound <= 0)
			break;
		/* the slider follows a held button as well as a click */
		if ((down || (event->type == GR_EVENT_TYPE_MOUSE_MOTION && (event->mouse.buttons & GR_BUTTON_L))) &&
		    ui_inside(x, y, 8, TOP + 18, WIDTH - 16, 26)) {
			SND_MASTER = ui_slider_value(x, 8, WIDTH - 16, 256);
			draw();
		} else if (down && ui_inside(x, y, 8, TOP + 52, 90, 22)) {
			beep();
		}
		break;
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	char path[512];

	/* nxsettings volume N: how loud, without the desktop */
	if (argc > 2 && !strcmp(argv[1], "volume")) {
		if (snd_open())
			return 1;
		SND_MASTER = atoi(argv[2]) * 256 / 100;
		return 0;
	}
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
	if (argc > 2 && !strcmp(argv[1], "size")) {
		restart_desktop(argv[2]);
		return 0;
	}
	/* nxsettings choose [FOLDER]: the file chooser by itself; prints what was chosen */
	if (argc > 1 && !strcmp(argv[1], "choose")) {
		if (ui_choose_file("Choose a file", argc > 2 ? argv[2] : "/", NULL, path, sizeof path, NULL))
			printf("%s\n", path);
		GrClose();
		return 0;
	}
	find_choices();
	find_sizes();
	if (getenv("NXSETTINGS_TAB")) {		/* the tab to open on, for tests */
		tab = atoi(getenv("NXSETTINGS_TAB")) % TABS;
		if (tab == TAB_SOUND)
			have_sound = snd_open() == 0;
	}
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Settings", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_MOUSE_MOTION |
		GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_MOUSE_MOTION:
			press(&event);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(&event);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
