/*
 * nxsettings: the desktop's settings, a tab each. Desktop: what the desktop shows (a picture
 * of the image's, any picture file, or a colour). Look: windows classic or as Windows 7's, and
 * the text's font (ui_look.h). Screen: its size. Sound: how loud. Storage:
 * what the host keeps (docs/system/persist.md). System: what the machine is. Devices: each device and how it runs.
 * From a script: nxsettings apply | look seven|classic [smooth|classic] | size WxH | volume 0-100 |
 * choose [DIR] | system | devices.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <setjmp.h>
#include <signal.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#define UI_PICTURES	/* pictures of any kind decoded (media.h) */
#include "machine.h"
#include "ui.h"
#include "ui_files.h"
#include "ui_ask.h"
#include "shaderemu_sound.h"

#define WIDTH 600
#define HEIGHT 300
#define TAB_W 72
#define TOP 36			/* where a tab's own part begins */
#define MAX_CHOICES 64
#define SIZE_CHOICE "desktop.size"	/* the registry's key for the screen size the desktop starts with: WIDTHxHEIGHT (the nx script reads it) */

enum { TAB_DESKTOP, TAB_LOOK, TAB_SCREEN, TAB_SOUND, TAB_STORAGE, TAB_SYSTEM, TAB_DEVICES, TABS };
static const char *tab_names[TABS] = { "Desktop", "Look", "Screen", "Sound", "Storage", "System", "Devices" };
static const char *const storage_names[3] = { "Save now", "Load at start: on", "Clear the kept data" };
static int kept_bytes = -1, kept_most, kept_auto = 1;	/* what the host keeps of /persistence; -1: it keeps nothing */

static struct { char label[64], value[512]; } choices[MAX_CHOICES];
static struct ui_list pictures = { 8, TOP, WIDTH - 16, 196, 0, 0, -1 };
static const char *sizes[] = { "640x480", "800x600", "1024x576", "1024x768", "1280x720", "1600x900", "1920x1080" };
#define SIZES 7
static struct ui_list screens = { 8, TOP + 20, 170, SIZES * UI_ROW + 4, 0, 0, -1 };
static int fits[SIZES], most_w = 1280, most_h = 720;	/* the sizes this host can show, and its largest */
static int tab, have_sound = -1;	/* have_sound: not asked yet */
static int cores = 1, has_float = -1;	/* the machine's cores (its control words say); float instructions: not tried yet */
static unsigned speed_cycles, speed_ms, speed_now;	/* instructions and time at the last look; instructions a second since */
static int focus;	/* what the keyboard works: 0 the row of tabs, 1 and 2 the tab's controls in order */
static GR_WINDOW_ID window;
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ)

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

		/* (NAME@WxH is NAME made for a screen of that size: not a choice of its own) */
		if (strncmp(name, "wallpaper-", 10) != 0 || strchr(name, '@'))
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

/* The row of what the desktop shows now: a picture file of the user's gets a row of its own. */
static void
find_current(void)
{
	char now[256];
	int i;

	snprintf(now, sizeof now, "%s", reg_get(UI_WALLPAPER_CHOICE, UI_WALLPAPER_DEFAULT));
	for (i = 0; i < pictures.count; i++)
		if (!strcasecmp(choices[i].value, now)) {
			pictures.selected = i;
			ui_list_show(&pictures);
			return;
		}
	if (now[0] == '/' && access(now, R_OK) == 0) {
		char label[64];

		snprintf(label, sizeof label, "File: %s", strrchr(now, '/') + 1);
		add(label, now);
		pictures.selected = pictures.count - 1;
		ui_list_show(&pictures);
	}
}

/* Whether the desktop (nxfiles --desktop) is running: it draws a new choice itself. */
static int
desktop_running(void)
{
	DIR *proc = opendir("/proc");
	struct dirent *item;
	char path[64], line[64];
	int found = 0, fd, n;

	while (proc && !found && (item = readdir(proc)) != NULL) {
		if (item->d_name[0] < '1' || item->d_name[0] > '9')
			continue;
		snprintf(path, sizeof path, "/proc/%.20s/cmdline", item->d_name);
		if ((fd = open(path, O_RDONLY)) < 0)
			continue;
		n = (int)read(fd, line, sizeof line - 1);
		close(fd);
		/* (its words are parted by nothing: "nxfiles", then "--desktop") */
		found = n >= 18 && !memcmp(line + n - 18, "nxfiles\0--desktop\0", 18);
	}
	if (proc)
		closedir(proc);
	return found;
}

static void while_choosing(GR_EVENT *event);

/* What the desktop shows from now on, kept in the registry: a picture file itself (never a
 * decoded copy), or a colour. The desktop sees the key change and decodes the picture; with no
 * desktop running, it is drawn here, and the GPU's copy of it let go at once. */
static void
set_wallpaper(const char *value)
{
	static const char *const ok[] = { "OK" };
	char text[400];

	if (strlen(value) > 250) {
		snprintf(text, sizeof text, "%.120s...\ncannot be the desktop's picture: its path is longer than the\n250 letters the registry keeps. Move it to a folder with a shorter name.", value);
		ui_ask_icon = "error";
		ui_ask("Desktop picture", text, ok, 1, while_choosing);
		ui_ask_icon = NULL;
		return;
	}
	if (!reg_set(UI_WALLPAPER_CHOICE, value)) {
		snprintf(text, sizeof text, "The choice could not be kept:\n/persistence/registry could not be written (%s).\nThe desktop shows it until it is drawn again.", strerror(errno));
		ui_ask_icon = "error";
		ui_ask("Desktop picture", text, ok, 1, while_choosing);
		ui_ask_icon = NULL;
	}
	if (value[0] == '#' || !desktop_running()) {
		ui_wallpaper(value, 0);
#ifdef UI_PICTURES
		if (ui_decoded.pixmap)
			GrDestroyWindow(ui_decoded.pixmap);
		ui_decoded.pixmap = 0;
#endif
	}
}

/* NULL if the file is a picture the desktop can show, or why not. */
static const char *
not_a_picture(const char *path, char *why, int size)
{
	struct media_info info;
	struct stat st;
	const char *said;
	void *data;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0) {
		snprintf(why, size, "it cannot be read: %s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return why;
	}
	if (st.st_size < 16 || (data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		close(fd);
		snprintf(why, size, st.st_size < 16 ? "it is empty, or too short to be a picture" : "it could not be read into memory: %s", strerror(errno));
		return why;
	}
	close(fd);
	said = media_probe(data, (uint32_t)st.st_size, &info);
	munmap(data, (size_t)st.st_size);
	if (said)
		snprintf(why, size, "%s", said);
	else if (info.kind < MEDIA_PNG || info.kind > MEDIA_TGA)
		snprintf(why, size, "it is %s, not a picture", media_kind_name(info.kind));
	else
		return NULL;
	return why;
}

static const char *
picture_label(int row)
{
	return choices[row].label;
}

/* The host's largest screen is in its flags, among the machine's control words (docs/machine/gpu.md):
 * width and height in 16s of pixels. A host that does not say shows 1280 x 720. */
static void
find_sizes(void)
{
	int gpu = open("/dev/gpu", O_RDWR), i;
	const unsigned *words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000);

	/* how many cores the machine has, or could have (docs/machine/multicore.md: the geometry's words) */
	if (words != MAP_FAILED && (words[0x3cc / 4] || words[0x3c0 / 4]))
		cores = (int)(words[0x3cc / 4] ? words[0x3cc / 4] : words[0x3c0 / 4]);
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
	int w = atoi(sizes[fits[row]]), h = atoi(strchr(sizes[fits[row]], 'x') + 1);

	snprintf(text, sizeof text, "%d x %d%s", w, h, w == ui_screen()->cols && h == ui_screen()->rows ? "  (now)" : "");
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

static sigjmp_buf no_float;

static void
float_trap(int signal_number)
{
	(void)signal_number;
	siglongjmp(no_float, 1);
}

/* Whether the processor has float instructions: one is run, and it is an illegal one or not.
 * (Linux's own word on it, /proc/cpuinfo, is its device tree's, which does not name them.) */
static int
float_unit(void)
{
	void (*before)(int) = signal(SIGILL, float_trap);
	volatile int there = 0;

	if (!sigsetjmp(no_float, 1)) {
#ifndef __gpuvm__	/* (gpu_vm's floats are always there) */
		__asm__ volatile(".word 0xd0007053");	/* fcvt.s.w ft0, zero */
#endif
		there = 1;
	}
	signal(SIGILL, before);
	return there;
}

static unsigned
cycles_now(void)
{
	unsigned n;

#ifdef __gpuvm__
	__asm__ volatile("mfsr %0, ops" : "=r"(n));
#else
	__asm__ volatile("rdcycle %0" : "=r"(n));
#endif
	return n;
}

static unsigned
ms_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* The first core's instructions a second since this was last asked (it counts them: rdcycle). */
static void
measure_speed(void)
{
	unsigned cycles = cycles_now(), ms = ms_now();

	if (speed_ms && ms - speed_ms >= 200)
		speed_now = (unsigned)((unsigned long long)(cycles - speed_cycles) * 1000 / (ms - speed_ms));
	if (!speed_ms || ms - speed_ms >= 200) {
		speed_cycles = cycles;
		speed_ms = ms;
	}
}

/* The network device's address, or NULL while it has none. */
static const char *
network_address(void)
{
	static char text[32];
	struct ifreq request;
	int s = socket(AF_INET, SOCK_DGRAM, 0), got;

	if (s < 0)
		return NULL;
	memset(&request, 0, sizeof request);
	strcpy(request.ifr_name, "lan0");
	got = ioctl(s, SIOCGIFADDR, &request);
	close(s);
	if (got != 0)
		return NULL;
	snprintf(text, sizeof text, "%s", inet_ntoa(((struct sockaddr_in *)&request.ifr_addr)->sin_addr));
	return text;
}

/* What the machine is, a line each: every one asked of it as it is now. Returns how many. */
static int
system_lines(char lines[][160], int screen_w, int screen_h)
{
	struct utsname system;
	const char *address;
	FILE *memory = fopen("/proc/meminfo", "r");
	char line[160];
	long total = 0, available = 0;
	int n = 0;

	/* (both numbers in one reading of the file: the kernel writes it out for each) */
	while (memory && fgets(line, sizeof line, memory))
		if (!strncmp(line, "MemTotal:", 9))
			total = atol(line + 9);
		else if (!strncmp(line, "MemAvailable:", 13))
			available = atol(line + 13);
	if (memory)
		fclose(memory);
	uname(&system);
	if (has_float < 0)
		has_float = float_unit();
	if (have_sound < 0)
		have_sound = snd_open() == 0;
	measure_speed();
	snprintf(lines[n++], 160, "%s %s, %s", system.sysname, system.release, machine_mmu() ? "with an MMU (Sv32 paging)" : "without an MMU");
	snprintf(lines[n++], 160, "Processor: %s (%s), %d core%s", system.machine, machine_isa(), cores, cores == 1 ? "" : "s");
	if (cores > 1)
		snprintf(lines[n++], 160, "Linux runs on the first; the other %d are programs' workers", cores - 1);
	else
		snprintf(lines[n++], 160, "Linux and every program run on it");
	if (speed_now)
		snprintf(lines[n++], 160, "The first core ran %u.%02u million instructions a second just now", speed_now / 1000000,
			 speed_now / 10000 % 100);
	else
		snprintf(lines[n++], 160, "The first core's speed: measuring...");
	snprintf(lines[n++], 160, "Memory: %ld MB, %ld MB free", total / 1024, available / 1024);
	snprintf(lines[n++], 160, "Disk: a ROM of %ld MB; what is written is kept in memory", field("/sys/class/mtd/mtd0/size", "") >> 20);
	if (screen_w)
		snprintf(lines[n++], 160, "Screen: %d x %d (this host shows %d x %d at most)", screen_w, screen_h, most_w, most_h);
	else
		snprintf(lines[n++], 160, "Screen: this host shows %d x %d at most", most_w, most_h);
	if (have_sound > 0)
		snprintf(lines[n++], 160, "Sound card: %u samples a second, volume %d%%", (unsigned)SND_RATE, (int)(SND_MASTER * 100 / 256));
	else
		snprintf(lines[n++], 160, "Sound card: none");
	address = network_address();
	snprintf(lines[n++], 160, "Network: %s", address ? address : "not connected");
	snprintf(lines[n++], 160, "Running for %ld minutes", field("/proc/uptime", "") / 60);
	return n;
}

/* The desktop again, with another size: the window system starts over and every program on it
 * closes, this one too. The script that does it must not die with them; with `ask` it asks
 * first (and is the one to refuse a second press while it does). */
static void
restart_desktop(const char *size, int ask)
{
	if (!ask)
		reg_set(SIZE_CHOICE, size);
	if (fork() == 0) {
		setsid();
		execlp("nx", "nx", "restart", ask ? "ask" : (char *)NULL, size, (char *)NULL);
		_exit(127);
	}
}

/* The volume, as the next session will have it once the folder is saved. */
static void
keep_volume(void)
{
	reg_set_int("sound.volume", (int)(SND_MASTER * 100 / 256));
}

/* nxpersist with a word or two, waited for; its exit status. */
static int
persist(const char *what, const char *more)
{
	int status = -1;
	pid_t child = fork();

	if (child == 0) {
		execlp("nxpersist", "nxpersist", what, more, (char *)NULL);
		_exit(127);
	}
	return child > 0 && waitpid(child, &status, 0) == child ? status : -1;
}

/* What the host keeps, asked of it (nxpersist state). */
static void
read_kept(void)
{
	FILE *from = popen("nxpersist state", "r");
	char line[64];

	kept_bytes = -1;
	while (from && fgets(line, sizeof line, from)) {
		sscanf(line, "stored %d", &kept_bytes);
		sscanf(line, "limit %d", &kept_most);
		sscanf(line, "auto %d", &kept_auto);
	}
	if (from)
		pclose(from);
}

/* The Storage tab's three buttons. */
static void
storage(int which)
{
	static const char *const answers[] = { "Clear", "Cancel" }, *const ok[] = { "OK" };
	int status = 0;
	char text[200];

	if (which == 0)
		status = persist("save", NULL);
	else if (which == 1)
		status = persist("auto", kept_auto ? "0" : "1");
	else if (ui_ask("Storage", "Clear what the host keeps of /persistence?\nThe folder itself stays until the machine is switched off.", answers, 2, NULL) == 0)
		status = persist("clear", NULL);
	/* (a save that the host refused says why in nxpersist's own window) */
	if (status == -1 || (WIFEXITED(status) && WEXITSTATUS(status) == 127))
		snprintf(text, sizeof text, "nxpersist, which talks to the host, could not be run.");
	else if (status && which)
		snprintf(text, sizeof text, "The host did not take it (nxpersist %s: status %d).\nThis machine's host may keep nothing between sessions.",
			 which == 1 ? "auto" : "clear", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	if ((status == -1 || (WIFEXITED(status) && WEXITSTATUS(status) == 127)) || (status && which)) {
		ui_ask_icon = "error";
		ui_ask("Storage", text, ok, 1, while_choosing);
		ui_ask_icon = NULL;
	}
	read_kept();
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

/* ---- the Devices tab: the machine's devices and what each is now (docs/system/nanox.md, "Settings") ---- */

enum { DEV_CPU, DEV_MEMORY, DEV_STORAGE, DEV_GPU, DEV_SOUND, DEV_NETWORK, DEV_INPUT, DEV_CLOCK, DEV_CONSOLE, DEV_HOST, DEVICES };
static const char *device_names[DEVICES] = { "Processor", "Memory", "Storage", "GPU and display", "Sound card", "Network",
	"Keyboard and pointer", "Clock", "Console", "Host" };
static struct ui_list devices = { 8, TOP, 150, HEIGHT - TOP - 8, DEVICES, 0, 0 };
static char device_shown[24][96];	/* the selected device's lines as they were last drawn */
static int device_count = -1;
static const volatile unsigned *dev_words, *dev_more;	/* the control words, and the page of every core's count */

static const char *
device_label(int row)
{
	return device_names[row];
}

static void
device_words(void)
{
	static int tried;
	int gpu;
	void *at;

	if (tried)
		return;
	tried = 1;
	if ((gpu = open("/dev/gpu", O_RDWR)) < 0)
		return;
	if ((at = mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000)) != MAP_FAILED)
		dev_words = at;
	if ((at = mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000 + 0x6ba000)) != MAP_FAILED)
		dev_more = at;
}

/* A line of a file (its first, or the one that begins with `name`), without its end. */
static const char *
file_line(const char *path, const char *name)
{
	static char line[160];
	FILE *file = fopen(path, "r");
	int found = 0;

	line[0] = 0;
	while (file && !found && fgets(line, sizeof line, file))
		found = !name || !strncmp(line, name, strlen(name));
	if (file)
		fclose(file);
	if (!found)
		line[0] = 0;
	line[strcspn(line, "\n")] = 0;
	return line;
}

static long
file_number(const char *path)
{
	return atol(file_line(path, NULL));
}

/* What a device is and how it runs, a line each; returns how many. */
static int
device_lines(int which, char lines[][96])
{
	int n = 0;

	device_words();
	measure_speed();
	switch (which) {
	case DEV_CPU: {
		const char *isa = file_line("/proc/cpuinfo", "isa");
		unsigned now = dev_words ? dev_words[0x3c0 / 4] : 1, most = dev_words ? dev_words[0x3cc / 4] : 1, c, working = 0, asleep = 0;

		if (has_float < 0)
			has_float = float_unit();
#ifdef __gpuvm__
		snprintf(lines[n++], 96, "Architecture: GPU-VM, 32 bits (bundles of two ops)");
		snprintf(lines[n++], 96, "Units: integers, memory, floats, the card's special functions");
#else
		snprintf(lines[n++], 96, "Architecture: RISC-V, 32 bits (RV32I)");
		snprintf(lines[n++], 96, "Extensions: M (multiply), A (atomics)%s, Zicsr, Zifencei", has_float ? ", F (float)" : "");
		if (machine_zb())
			snprintf(lines[n++], 96, "And Zba, Zbb: address and bit instructions");
#endif
		snprintf(lines[n++], 96, "No compressed (C) instructions: every one is 32 bits");
		snprintf(lines[n++], 96, "Linux says: %s", isa[0] ? strchr(isa, ':') ? strchr(isa, ':') + 2 : isa : "-");
		snprintf(lines[n++], 96, "Modes: machine (the firmware, in the GPU), supervisor, user");
		if (machine_mmu())
			snprintf(lines[n++], 96, "Memory translation: Sv32, 4 KB pages");
		else {
			snprintf(lines[n++], 96, "Memory translation: none (no MMU; satp is always 0)");
			snprintf(lines[n++], 96, "Programs: flat (bFLT), each where it was loaded");
		}
		snprintf(lines[n++], 96, "Cores: %u laid out now, %u at most", now ? now : 1, most ? most : 1);
		snprintf(lines[n++], 96, "Core 0 runs Linux; the others are workers lent to programs");
		if (dev_words && now > 1) {
			for (c = 1; c < now && c < 256; c++) {
				unsigned runs = c < 32 ? dev_words[0x3c4 / 4] >> c : dev_more ? dev_more[0x400 / 4 + 4 * (c / 32)] >> (c % 32) : 0;
				unsigned sleeps = c < 32 ? dev_words[0x3c8 / 4] >> c : dev_more ? dev_more[0x404 / 4 + 4 * (c / 32)] >> (c % 32) : 0;

				working += runs & 1 && !(sleeps & 1);
				asleep += sleeps & 1;
			}
			snprintf(lines[n++], 96, "Workers: %u running, %u asleep, %u parked", working, asleep, now - 1 - working - asleep);
		}
		snprintf(lines[n++], 96, "Core 0: %u.%02u million instructions a second just now", speed_now / 1000000, speed_now / 10000 % 100);
		snprintf(lines[n++], 96, "A pass of the GPU runs at most 8,192 instructions a core");
		snprintf(lines[n++], 96, "Each core: its registers and a write cache in the GPU's state");
		break;
	}
	case DEV_MEMORY: {
		long total = atol(file_line("/proc/meminfo", "MemTotal:") + 9), free = atol(file_line("/proc/meminfo", "MemAvailable:") + 13);

		snprintf(lines[n++], 96, "RAM: %ld MB, %ld MB free", total / 1024, free / 1024);
#ifdef __gpuvm__
		if (1) {
			snprintf(lines[n++], 96, "Where: 0x02000000 up, Linux's from its start");
			snprintf(lines[n++], 96, "No MMU: a program's memory is taken whole as it starts");
		} else
#endif
		if (machine_mmu())
			snprintf(lines[n++], 96, "Where: 0x82000000 up, Linux's from its start");
		else {
			snprintf(lines[n++], 96, "Where: 0x82000000 to 0x92000000, Linux's from its start");
			snprintf(lines[n++], 96, "No MMU: a program's memory is taken whole as it starts");
		}
		snprintf(lines[n++], 96, "Kept in: the GPU's state texture, 16 bytes a texel");
		snprintf(lines[n++], 96, "Page cache: %ld MB", atol(file_line("/proc/meminfo", "Cached:") + 7) / 1024);
#ifdef __gpuvm__
		snprintf(lines[n++], 96, "GPU memory: 0x00010000 to 0x01000000, 16 MB (the GPU's)");
#else
		snprintf(lines[n++], 96, "GPU memory: 0x80000000 to 0x81000000, 16 MB (the GPU's)");
#endif
		break;
	}
	case DEV_STORAGE: {
		struct statvfs root;

		snprintf(lines[n++], 96, "ROM: %ld MB, read only (/dev/mtd0, %s)", file_number("/sys/class/mtd/mtd0/size") >> 20,
			 file_line("/sys/class/mtd/mtd0/name", NULL));
		snprintf(lines[n++], 96, "Its files: romfs, read where they lie in the ROM");
		if (statvfs("/tmp", &root) == 0)
			snprintf(lines[n++], 96, "Written files: in memory, %lu MB free of it", (unsigned long)(root.f_bavail * root.f_frsize >> 20));
		snprintf(lines[n++], 96, "Kept between sessions: /persistence, by the host");
		if (kept_bytes >= 0)
			snprintf(lines[n++], 96, "The host keeps %d KB of it (%d KB at most)", (kept_bytes + 999) / 1000, kept_most / 1000);
		break;
	}
	case DEV_GPU: {
		unsigned mode = dev_words ? dev_words[0] : 0;
		static const char *modes[] = { "off", "32-bit colour", "8-bit indexed", "the GPU's picture", "layers (the desktop)" };

#ifdef __gpuvm__
		snprintf(lines[n++], 96, "Device: /dev/gpu, its words at 0x01000000");
#else
		snprintf(lines[n++], 96, "Device: /dev/gpu, its words at 0x81000000");
#endif
		snprintf(lines[n++], 96, "Draws: triangle lists into pictures in GPU memory");
		snprintf(lines[n++], 96, "Textures: in GPU memory, or files of the ROM where they lie");
		snprintf(lines[n++], 96, "Display mode: %s", mode < 5 ? modes[mode] : "?");
		if (dev_words && dev_words[1])
			snprintf(lines[n++], 96, "Screen: %u x %u", dev_words[1], dev_words[2]);
		snprintf(lines[n++], 96, "The largest this host shows: %d x %d", most_w, most_h);
		snprintf(lines[n++], 96, "Frame buffer device: /dev/fb0");
		break;
	}
	case DEV_SOUND:
		if (have_sound < 0)
			have_sound = snd_open() == 0;
		if (have_sound <= 0) {
			snprintf(lines[n++], 96, "None (or its host plays nothing)");
			break;
		}
		snprintf(lines[n++], 96, "Device: /dev/sound, mixed by the host");
		snprintf(lines[n++], 96, "Rate: %u samples a second", (unsigned)SND_RATE);
		snprintf(lines[n++], 96, "Volume: %d%%", (int)(SND_MASTER * 100 / 256));
		break;
	case DEV_NETWORK: {
		const char *address = network_address();

		snprintf(lines[n++], 96, "Device: lan0, a link through the host");
		snprintf(lines[n++], 96, "Address: %s", address ? address : "none");
		snprintf(lines[n++], 96, "Hardware address: %s", file_line("/sys/class/net/lan0/address", NULL)[0] ? file_line("/sys/class/net/lan0/address", NULL) : "none");
		snprintf(lines[n++], 96, "Largest packet: %ld bytes", file_number("/sys/class/net/lan0/mtu"));
		snprintf(lines[n++], 96, "Received: %ld packets, %ld KB", file_number("/sys/class/net/lan0/statistics/rx_packets"),
			 file_number("/sys/class/net/lan0/statistics/rx_bytes") / 1024);
		snprintf(lines[n++], 96, "Sent: %ld packets, %ld KB", file_number("/sys/class/net/lan0/statistics/tx_packets"),
			 file_number("/sys/class/net/lan0/statistics/tx_bytes") / 1024);
		break;
	}
	case DEV_INPUT:
		snprintf(lines[n++], 96, "Keyboard: /dev/input/event0, Linux key codes");
		snprintf(lines[n++], 96, "Pointer: /dev/input/event1, the display's pixels");
		if (dev_words) {
			snprintf(lines[n++], 96, "Key events so far: %u", dev_words[0x2c / 4]);
			snprintf(lines[n++], 96, "Pointer at %u, %u; buttons %u", dev_words[0x20 / 4], dev_words[0x24 / 4], dev_words[0x28 / 4]);
			snprintf(lines[n++], 96, "The keyboard's owner: %s", dev_words[0x30 / 4] == 0x6b657973 ? "a program" : "the console");
		}
		break;
	case DEV_CLOCK: {
		time_t now = time(NULL);
		char when[40];

		strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
		snprintf(lines[n++], 96, "Clock chip: DS1742 at 0x03000000, set by the host");
		snprintf(lines[n++], 96, "Now: %s", when);
		snprintf(lines[n++], 96, "Timer: the machine's own, 100 ticks a second");
		snprintf(lines[n++], 96, "Running for %ld minutes", field("/proc/uptime", "") / 60);
		break;
	}
	case DEV_CONSOLE:
		snprintf(lines[n++], 96, "Console: /dev/hvc0, a line to the host (the firmware's)");
		snprintf(lines[n++], 96, "Terminals: hvc0 to hvc7, the host's console tabs");
		break;
	case DEV_HOST: {
		static const char *kinds[] = { "says nothing", "the harness", "the world, on a desktop", "the world, in a headset" };
		unsigned word = dev_words ? dev_words[0x360 / 4] : 0;
		const char *user = getenv("USER");

		snprintf(lines[n++], 96, "Host: %s", (word & 15) < 4 ? kinds[word & 15] : "?");
		snprintf(lines[n++], 96, "Player: %s", user && *user ? user : "player");
		if ((word & 15) >= 2) {
			snprintf(lines[n++], 96, "Visitors here: %u", word >> 8 & 255);
			snprintf(lines[n++], 96, "In a holodeck's seat: %s", word & 16 ? "yes" : "no");
		}
		break;
	}
	}
	return n;
}

static void
draw_devices(int whole)
{
	static char lines[24][96];
	int n = device_lines(devices.selected < 0 ? 0 : devices.selected, lines), i, x = devices.x + devices.width + 10;

	if (whole)
		ui_list_draw(window, &devices, device_label);
	for (i = 0; i < n || i < device_count; i++) {
		if (!whole && i < n && i < device_count && !strcmp(lines[i], device_shown[i]))
			continue;
		ui_fill(window, x, TOP + i * 18, WIDTH - x, 18, UI_FACE);
		if (i < n)
			ui_text_fit(window, x, TOP + i * 18, WIDTH - x - 6, lines[i], BLACK);
	}
	memcpy(device_shown, lines, sizeof lines);
	device_count = n;
}

static char system_shown[12][160];	/* the System tab's lines as they were last drawn */
static int system_count;

/* The System tab's lines again, a second later: the ones that read otherwise are drawn. */
static void
draw_system_changes(void)
{
	static char lines[12][160];
	int n = system_lines(lines, ui_screen()->cols, ui_screen()->rows), i;

	for (i = 0; i < n || i < system_count; i++) {
		if (i < n && i < system_count && !strcmp(lines[i], system_shown[i]))
			continue;
		ui_fill(window, 0, TOP + i * 18, WIDTH, 18, UI_FACE);
		if (i < n)
			ui_text(window, 8, TOP + i * 18, lines[i], -1, BLACK, 0);
	}
	memcpy(system_shown, lines, sizeof lines);
	system_count = n;
}

/* The Look tab: two choices of two, a button each, the one in use pressed. */
static const char *const look_names[4] = { "Windows 7", "Classic", "Smooth (Selawik)", "Classic (bitmap)" };
#define LOOK_X(i) (8 + (i) % 2 * 168)
#define LOOK_Y(i) (TOP + 20 + (i) / 2 * 64)

/* Every window's frame drawn again in the look now set: the window manager paints a frame at its exposure. */
static void
frames_again(void)
{
	GR_WINDOW_ID parent, *children = NULL;
	GR_WINDOW_INFO info;
	GR_COUNT n = 0, i;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n; i++) {
		GrGetWindowInfo(children[i], &info);
		if (!(info.props & GR_WM_PROPS_NODECORATE) && info.mapped)
			GrClearWindow(children[i], GR_TRUE);
	}
	free(children);
}

/* Sets the look, keeps it in the registry, and draws what this program and the window manager draw in it. */
static void
set_look(int seven, int smooth)
{
	reg_set(UI_LOOK_STYLE, seven ? "seven" : "classic");
	reg_set(UI_LOOK_TEXT, smooth ? "smooth" : "classic");
	ui_look_set(seven ? UI_STYLE_SEVEN : UI_STYLE_CLASSIC, smooth);
	if (window) {
		ui_restyle();
		frames_again();
	}
}

/* A style brings its own text (classic is the old look whole); the text's buttons then change only that. */
static void
look_choose(int which)
{
	set_look(which < 2 ? which == 0 : ui_seven, which < 2 ? which == 0 : which == 2);
}

static void
draw(void)
{
	GR_SCREEN_INFO screen = *ui_screen();
	char text[160];
	int i;

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	for (i = 0; i < TABS; i++)
		ui_button(window, 8 + i * (TAB_W + 2), 6, TAB_W, 22, tab_names[i], i == tab);
	if (focus == 0)
		ui_focus(window, 8 + tab * (TAB_W + 2) + 4, 6 + 4, TAB_W - 8, 22 - 8);
	switch (tab) {
	case TAB_DESKTOP:
		ui_list_draw(window, &pictures, picture_label);
		ui_button(window, 8, TOP + 204, 180, 22, "Choose a picture file...", 0);
		if (focus == 1)
			ui_focus(window, pictures.x, pictures.y, pictures.width, pictures.height);
		else if (focus == 2)
			ui_focus(window, 8 + 4, TOP + 204 + 4, 180 - 8, 22 - 8);
		ui_text(window, 8, HEIGHT - 26, "A picture file is a JPEG, PNG, GIF, BMP, TGA or PPM: the desktop keeps the file itself.", -1, UI_SHADOW, 0);
		break;
	case TAB_LOOK:
		ui_text(window, 8, TOP, "Windows", -1, BLACK, 0);
		ui_text(window, 8, TOP + 64, "Text", -1, BLACK, 0);
		for (i = 0; i < 4; i++) {
			ui_button(window, LOOK_X(i), LOOK_Y(i), 160, 22, look_names[i], i < 2 ? (i == 0) == ui_seven : (i == 2) == ui_smooth);
			if (focus == i + 1)
				ui_focus(window, LOOK_X(i) + 4, LOOK_Y(i) + 4, 160 - 8, 22 - 8);
		}
		ui_text(window, 360, LOOK_Y(0) + 4, ui_seven ? "Pale glass frames, a dark bar." : "Grey frames, a blue caption.", -1, UI_SHADOW, 0);
		ui_text(window, 360, LOOK_Y(2) + 4, ui_smooth ? "Selawik, smoothed." : "The window system's own.", -1, UI_SHADOW, 0);
		ui_text(window, 8, HEIGHT - 44, "Frames, the bar and the desktop change at once; a program", -1, UI_SHADOW, 0);
		ui_text(window, 8, HEIGHT - 26, "already open keeps its own look until it is opened again.", -1, UI_SHADOW, 0);
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
	case TAB_STORAGE:
		if (kept_bytes < 0)
			snprintf(text, sizeof text, "This machine's host keeps nothing between sessions.");
		else if (kept_bytes == 0)
			snprintf(text, sizeof text, "The host keeps nothing of /persistence yet.");
		else
			snprintf(text, sizeof text, "The host keeps %d KB of /persistence (%d at most).", (kept_bytes + 999) / 1000, kept_most / 1000);
		ui_text(window, 8, TOP, text, -1, BLACK, 0);
		for (i = 0; i < 3; i++) {
			ui_button(window, 8, TOP + 24 + i * 28, 170, 22, i == 1 && !kept_auto ? "Load at start: off" : storage_names[i], 0);
			if (focus == i + 1)
				ui_focus(window, 8 + 4, TOP + 24 + i * 28 + 4, 170 - 8, 22 - 8);
		}
		ui_text(window, 8, HEIGHT - 44, "/persistence holds settings and saved games. It is kept", -1, UI_SHADOW, 0);
		ui_text(window, 8, HEIGHT - 26, "only when saved: here, or with the button by the computer.", -1, UI_SHADOW, 0);
		break;
	case TAB_SYSTEM: {
		static char lines[12][160];
		int n = system_lines(lines, screen.cols, screen.rows);

		for (i = 0; i < n; i++)
			ui_text(window, 8, TOP + i * 18, lines[i], -1, BLACK, 0);
		memcpy(system_shown, lines, sizeof lines);
		system_count = n;
		}
		ui_text(window, 8, HEIGHT - 44, "Settings are kept when saved (the Storage tab).", -1, UI_SHADOW, 0);
#ifdef __gpuvm__
		ui_text(window, 8, HEIGHT - 26, "ShaderEmu: a GPU-VM computer in a pixel shader", -1, UI_SHADOW, 0);
#else
		ui_text(window, 8, HEIGHT - 26, "ShaderEmu: a RISC-V computer in a pixel shader", -1, UI_SHADOW, 0);
#endif
		break;
	case TAB_DEVICES:
		draw_devices(1);
		if (focus == 1)
			ui_focus(window, devices.x, devices.y, devices.width, devices.height);
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
	static const char *const ok[] = { "OK" };
	char path[512], label[64], why[160], text[400];
	const char *name;
	int i;

	/* (every kind libmedia decodes: docs/system/media.md) */
	if (!ui_choose_file("Choose a picture", access("/root/Pictures", R_OK) == 0 ? "/root/Pictures" : "/usr/share",
			    ".jpg .jpeg .png .gif .bmp .tga .ppm .pgm .pbm .pnm", path, sizeof path, while_choosing))
		return;
	if (not_a_picture(path, why, sizeof why)) {
		snprintf(text, sizeof text, "%.120s cannot be the desktop's picture:\n%s.", strrchr(path, '/') + 1, why);
		ui_ask_icon = "error";
		ui_ask("Desktop picture", text, ok, 1, while_choosing);
		ui_ask_icon = NULL;
		return;
	}
	name = strrchr(path, '/') + 1;
	snprintf(label, sizeof label, "File: %s", name);
	for (i = 0; i < pictures.count && strcmp(choices[i].value, path); i++)
		continue;
	if (i == pictures.count)
		add(label, path);
	pictures.selected = i < pictures.count ? i : pictures.count - 1;
	ui_list_show(&pictures);
	set_wallpaper(path);
}

/* Shows a tab; the keyboard stays on the row of tabs, or goes there. */
static void
show_tab(int which)
{
	tab = which;
	focus = 0;
	if (tab == TAB_SOUND && have_sound < 0)
		have_sound = snd_open() == 0;
	if (tab == TAB_STORAGE || tab == TAB_DEVICES)
		read_kept();
	device_count = -1;
	draw();
}

/* How many places the keyboard can be on the tab shown: the row of tabs, then its controls. */
static int
controls(void)
{
	return tab == TAB_SYSTEM || (tab == TAB_SOUND && have_sound <= 0) ? 1 : tab == TAB_STORAGE ? 4 : tab == TAB_LOOK ? 5 :
		tab == TAB_DEVICES ? 2 : 3;
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
			set_wallpaper(choices[pictures.selected].value);
		else
			break;
		draw();
		break;
	case TAB_LOOK:
		if (ui_press_key(ch)) {
			look_choose(focus - 1);
			draw();
		}
		break;
	case TAB_SCREEN:
		if (ui_press_key(ch) && screens.selected >= 0)
			restart_desktop(sizes[fits[screens.selected]], 1);
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
			keep_volume();
			draw();
		}
		break;
	case TAB_STORAGE:
		if (ui_press_key(ch)) {
			storage(focus - 1);
			draw();
		}
		break;
	case TAB_DEVICES:
		if (focus == 1 && ui_list_key(&devices, ch)) {
			device_count = -1;
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
			set_wallpaper(choices[pictures.selected].value);
		if (how) {
			focus = 1;
			draw();
		} else if (down && ui_inside(x, y, 8, TOP + 204, 180, 22)) {
			choose_picture();
			draw();
		}
		break;
	case TAB_LOOK:
		for (how = 0; down && how < 4; how++)
			if (ui_inside(x, y, LOOK_X(how), LOOK_Y(how), 160, 22)) {
				focus = how + 1;
				look_choose(how);
				draw();
			}
		break;
	case TAB_SCREEN:
		if (ui_list_event(&screens, event)) {
			focus = 1;
			draw();
		} else if (down && ui_inside(x, y, 190, TOP + 20, 180, 22) && screens.selected >= 0)
			restart_desktop(sizes[fits[screens.selected]], 1);
		break;
	case TAB_SOUND:
		if (have_sound <= 0)
			break;
		/* the slider follows a held button as well as a click */
		if ((down || (event->type == GR_EVENT_TYPE_MOUSE_MOTION && (event->mouse.buttons & GR_BUTTON_L))) &&
		    ui_inside(x, y, 8, TOP + 18, WIDTH - 16, 26)) {
			SND_MASTER = ui_slider_value(x, 8, WIDTH - 16, 256);
			keep_volume();
			draw();
		} else if (down && ui_inside(x, y, 8, TOP + 52, 90, 22)) {
			beep();
		}
		break;
	case TAB_STORAGE:
		for (how = 0; down && how < 3; how++)
			if (ui_inside(x, y, 8, TOP + 24 + how * 28, 170, 22)) {
				focus = how + 1;
				storage(how);
				draw();
			}
		break;
	case TAB_DEVICES:
		if (ui_list_event(&devices, event)) {
			focus = 1;
			device_count = -1;
			draw();
		}
		break;
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	char path[512];

	/* nxsettings system: what the System tab says, printed (it needs no desktop) */
	if (argc > 1 && !strcmp(argv[1], "system")) {
		static char lines[12][160];
		int i, n;

		find_sizes();
		system_lines(lines, 0, 0);
		usleep(300000);		/* (a speed is two looks at the count, some time apart) */
		n = system_lines(lines, 0, 0);
		for (i = 0; i < n; i++)
			printf("%s\n", lines[i]);
		return 0;
	}
	/* nxsettings devices: what the Devices tab says of each device, printed */
	if (argc > 1 && !strcmp(argv[1], "devices")) {
		static char lines[24][96];
		int d, i, n;

		find_sizes();
		measure_speed();
		usleep(300000);
		for (d = 0; d < DEVICES; d++) {
			n = device_lines(d, lines);
			printf("%s\n", device_names[d]);
			for (i = 0; i < n; i++)
				printf("    %s\n", lines[i]);
		}
		return 0;
	}
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
	/* nxsettings look seven|classic [smooth|classic]: the look, as the Look tab sets it */
	if (argc > 2 && !strcmp(argv[1], "look")) {
		set_look(strcmp(argv[2], "classic") != 0, argc > 3 ? strcmp(argv[3], "classic") != 0 : ui_smooth);
		frames_again();
		GrClose();
		return 0;
	}
	signal(SIGCHLD, SIG_DFL);	/* (the decoder is waited for: see ui_run in ui.h) */
	/* nxsettings apply: put up the desktop as last chosen, and go (the nx script does this) */
	if (argc > 1 && !strcmp(argv[1], "apply")) {
		ui_wallpaper_restore();
		GrFlush();
		GrClose();
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "size")) {
		restart_desktop(argv[2], 0);
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
	find_current();
	find_sizes();
	if (getenv("NXSETTINGS_TAB")) {		/* the tab to open on, for tests */
		tab = atoi(getenv("NXSETTINGS_TAB")) % TABS;
		if (tab == TAB_SOUND)
			have_sound = snd_open() == 0;
		if (tab == TAB_STORAGE)
			read_kept();
	}
	if (getenv("NXSETTINGS_DEVICE"))	/* and the device shown there */
		devices.selected = atoi(getenv("NXSETTINGS_DEVICE")) % DEVICES;
	window = GrNewWindowEx(UI_FIXED, "Settings", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	for (;;) {
		/* only the System and Devices tabs have something to say by themselves, once a second */
		GrGetNextEventTimeout(&event, tab == TAB_SYSTEM || tab == TAB_DEVICES ? 1000 : 0);
		if (event.type == GR_EVENT_TYPE_TIMEOUT && tab == TAB_SYSTEM)
			draw_system_changes();
		if (event.type == GR_EVENT_TYPE_TIMEOUT && tab == TAB_DEVICES)
			draw_devices(0);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_MOUSE_MOTION:
			/* the pointer's moves while a button is held (a slider, a list's bar), and not otherwise */
			if (event.type == GR_EVENT_TYPE_BUTTON_DOWN ? !ui_wheel(&event) : !event.mouse.buttons)
				ui_moves(window, EVENTS, event.type == GR_EVENT_TYPE_BUTTON_DOWN);
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
