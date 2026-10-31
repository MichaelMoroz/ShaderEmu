/* nxnet: the other computers of the network and what their owners share (docs/system/lan.md,
 * "The Network program"). nxnet is the window; nxnet --serve answers the others and gives them
 * this computer's /root to read (nx starts it); --find, --ls ADDRESS PATH and --get ADDRESS
 * PATH FILE are the same from a script. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_glyph.h"

#define PORT 7474
#define SHARED "/root"
#define SWITCH "/tmp/nxshare.off"	/* there: this computer answers nobody */
#define HOSTING "/tmp/nxhosting"	/* the game this computer hosts, a word */
#define KEPT "/tmp/net"			/* files fetched to be opened */
#define MOST 200

struct item { char name[100]; long size; int folder; };

/* ---- both sides ---- */

/* This computer's address on the network, or 0 without a link. */
static in_addr_t
own_address(void)
{
	struct ifreq request;
	int s = socket(AF_INET, SOCK_DGRAM, 0), got;

	if (s < 0)
		return 0;
	memset(&request, 0, sizeof request);
	strcpy(request.ifr_name, "lan0");
	got = ioctl(s, SIOCGIFADDR, &request);
	close(s);
	return got == 0 ? ((struct sockaddr_in *)&request.ifr_addr)->sin_addr.s_addr : 0;
}

static void
seconds(int s, int n)
{
	struct timeval limit = { n, 0 };

	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit);
}

/* A line from a socket, without its end; its length, or -1. Read a byte at a time: what
 * follows a file's first line is the file. */
static int
line_from(int s, char *line, int size)
{
	int n = 0;

	while (n < size - 1 && read(s, line + n, 1) == 1 && line[n] != '\n')
		n++;
	line[n] = 0;
	return n < size - 1 ? n : -1;
}

static int
all_to(int s, const char *bytes, size_t n)
{
	ssize_t put;

	for (; n > 0; bytes += put, n -= (size_t)put)
		if ((put = write(s, bytes, n)) <= 0)
			return 0;
	return 1;
}

/* ---- the computer that shares ---- */

/* A request's path as a path of this computer, or false: it stays inside what is shared. */
static int
shared_path(const char *asked, char *path, int size)
{
	if (asked[0] != '/' || strstr(asked, "/.") || access(SWITCH, F_OK) == 0)
		return 0;
	snprintf(path, size, SHARED "%s", asked);
	return 1;
}

static void
serve(int s)
{
	char line[600], path[700], block[4096];
	struct stat info;
	struct dirent *item;
	DIR *folder;
	int file, n;

	seconds(s, 20);
	if (line_from(s, line, sizeof line) < 3 || line[1] != ' ' || !shared_path(line + 2, path, sizeof path) || stat(path, &info) != 0) {
		all_to(s, "NO\n", 3);
	} else if (line[0] == 'L' && (folder = opendir(path)) != NULL) {
		all_to(s, "OK\n", 3);
		while ((item = readdir(folder)) != NULL) {
			char full[1000];

			if (item->d_name[0] == '.' || strlen(item->d_name) >= sizeof ((struct item *)0)->name)
				continue;
			snprintf(full, sizeof full, "%s/%s", path, item->d_name);
			if (stat(full, &info) != 0)
				continue;
			n = snprintf(line, sizeof line, "%c\t%ld\t%s\n", S_ISDIR(info.st_mode) ? 'd' : 'f', (long)info.st_size, item->d_name);
			if (!all_to(s, line, (size_t)n))
				break;
		}
		closedir(folder);
	} else if (line[0] == 'G' && S_ISREG(info.st_mode) && (file = open(path, O_RDONLY)) >= 0) {
		n = snprintf(line, sizeof line, "OK %ld\n", (long)info.st_size);
		all_to(s, line, (size_t)n);
		while ((n = (int)read(file, block, sizeof block)) > 0 && all_to(s, block, (size_t)n))
			continue;
		close(file);
	} else {
		all_to(s, "NO\n", 3);
	}
	close(s);
}

static int
serve_all(void)
{
	struct sockaddr_in here = { AF_INET, htons(PORT), { INADDR_ANY } }, from;
	int asked = socket(AF_INET, SOCK_DGRAM, 0), calls = socket(AF_INET, SOCK_STREAM, 0), one = 1, n, s;
	char text[64], name[40];
	socklen_t size;
	fd_set set;

	setsockopt(asked, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(calls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(asked, (struct sockaddr *)&here, sizeof here) != 0 || bind(calls, (struct sockaddr *)&here, sizeof here) != 0 || listen(calls, 4) != 0)
		return 1;	/* (another is serving already) */
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		FD_ZERO(&set);
		FD_SET(asked, &set);
		FD_SET(calls, &set);
		if (select((asked > calls ? asked : calls) + 1, &set, NULL, NULL, NULL) <= 0)
			continue;
		size = sizeof from;
		if (FD_ISSET(asked, &set) && (n = (int)recvfrom(asked, text, sizeof text - 1, 0, (struct sockaddr *)&from, &size)) >= 6 &&
		    !memcmp(text, "NXNET?", 6)) {
			/* its name: the one it was given (a line in /tmp/nxname), or its number on the network;
			 * and after it the game it hosts, [quake] (a word in /tmp/nxhosting: nxgame writes it) */
			FILE *given = fopen("/tmp/nxname", "r"), *game = fopen(HOSTING, "r");
			char hosts[16] = "";

			snprintf(name, sizeof name, "Computer %u", (unsigned)(ntohl(own_address()) & 0xffff));
			if (given) {
				if (fgets(name, 24, given))
					name[strcspn(name, "\r\n")] = 0;
				fclose(given);
			}
			if (game) {
				if (fscanf(game, "%12s", hosts) != 1)
					hosts[0] = 0;
				fclose(game);
			}
			/* (a computer that shares nothing still says that it hosts a game) */
			if (access(SWITCH, F_OK) != 0 || hosts[0]) {
				n = snprintf(text, sizeof text, hosts[0] ? "NXNET! %s [%s]" : "NXNET! %s", name, hosts);
				sendto(asked, text, (size_t)n, 0, (struct sockaddr *)&from, size);
			}
		}
		if (FD_ISSET(calls, &set) && (s = accept(calls, NULL, NULL)) >= 0) {
			/* a program of its own: `fork` is a vfork here, and with serve() in the child this one stopped for a
			 * whole transfer, or 20 s of a caller that says nothing: no other computer could find or reach it */
			if (fork() == 0) {
				dup2(s, 0);
				close(s);
				close(calls);
				close(asked);
				execlp("nxnet", "nxnet", "--one", (char *)NULL);
				_exit(127);
			}
			close(s);
		}
	}
}

/* ---- the computer that asks ---- */

static struct { in_addr_t address; char name[40]; } found[32];
static int found_count;
static struct item items[MOST];
static int item_count;
static void (*waiting)(void);	/* called while an answer is waited for: the window's events */

static int find_tenths = 40;	/* how long the answers are waited for: `--find SECONDS` asks for longer */

/* Asks who is there and takes the answers of the next four seconds: a computer busy with a
 * game, over the world's link, answered after a second and a half and was not seen. */
static void
find_all(void)
{
	struct sockaddr_in to = { AF_INET, htons(PORT), { htonl(INADDR_BROADCAST) } }, from;
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1, turn, n, i;
	in_addr_t own = own_address();
	int self = getenv("NXNET_SELF") != NULL;	/* a test with one computer: it lists itself */
	struct timeval limit;
	char text[64];
	socklen_t size;
	fd_set set;

	found_count = 0;
	if (s < 0 || (!own && !self))
		return;
	if (self)
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK), own = 0;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	for (turn = 0; turn < find_tenths; turn++) {
		if (turn % 10 == 0 && turn < find_tenths - 10)	/* asked again: a packet may be lost */
			sendto(s, "NXNET?", 6, 0, (struct sockaddr *)&to, sizeof to);
		FD_ZERO(&set);
		FD_SET(s, &set);
		limit.tv_sec = 0, limit.tv_usec = 100000;
		size = sizeof from;
		if (select(s + 1, &set, NULL, NULL, &limit) > 0 && (n = (int)recvfrom(s, text, sizeof text - 1, 0, (struct sockaddr *)&from, &size)) > 7 &&
		    !memcmp(text, "NXNET! ", 7) && from.sin_addr.s_addr != own) {
			text[n] = 0;
			for (i = 0; i < found_count && found[i].address != from.sin_addr.s_addr; i++)
				continue;
			/* (its latest answer is its name: what it hosts may have come since the first) */
			if (i < 32) {
				found[i].address = from.sin_addr.s_addr;
				snprintf(found[i].name, sizeof found[i].name, "%s", text + 7);
				found_count += i == found_count;
			}
		}
		if (waiting)
			waiting();
	}
	close(s);
}

/* A connection to a computer with a request sent and its answer's first line read into
 * `line`; -1, with why in `line`, when there is none or it says no. */
static int
ask(in_addr_t address, char what, const char *path, char *line, int size)
{
	struct sockaddr_in to = { AF_INET, htons(PORT), { address } };
	int s = socket(AF_INET, SOCK_STREAM, 0);
	char request[600];

	seconds(s, 15);
	if (s < 0 || connect(s, (struct sockaddr *)&to, sizeof to) != 0) {
		snprintf(line, size, "it does not answer");
	} else if (!all_to(s, request, (size_t)snprintf(request, sizeof request, "%c %.500s\n", what, path)) ||
		   line_from(s, line, size) < 2 || memcmp(line, "OK", 2)) {
		snprintf(line, size, "it does not give that");
	} else {
		return s;
	}
	if (s >= 0)
		close(s);
	return -1;
}

static int
by_kind(const void *a, const void *b)
{
	const struct item *x = a, *y = b;

	return x->folder != y->folder ? y->folder - x->folder : strcasecmp(x->name, y->name);
}

/* A folder's entries into `items`; false, and why, when they cannot be had. */
static int
list_of(in_addr_t address, const char *path, char *why, int size)
{
	char line[300], kind;
	int s = ask(address, 'L', path, why, size), n;

	if (s < 0)
		return 0;
	item_count = 0;
	while (item_count < MOST && line_from(s, line, sizeof line) > 4) {
		struct item *e = &items[item_count];

		if (sscanf(line, "%c\t%ld\t%n", &kind, &e->size, &n) < 2 || strlen(line + n) >= sizeof e->name)
			continue;
		strcpy(e->name, line + n);
		e->folder = kind == 'd';
		item_count++;
		if (waiting && item_count % 16 == 0)
			waiting();
	}
	close(s);
	qsort(items, (size_t)item_count, sizeof items[0], by_kind);
	return 1;
}

static int stop;	/* set by the window while a file comes: it is given up */

/* A file of a computer into a file here; `coming` hears how far it is. False, and why. */
static int
file_of(in_addr_t address, const char *path, const char *to, char *why, int size, void (*coming)(long have, long all))
{
	char block[2048];
	int s = ask(address, 'G', path, why, size), out;
	long all, have = 0;
	ssize_t n;

	if (s < 0)
		return 0;
	all = atol(why + 2);
	if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		snprintf(why, size, "it cannot be written here (%s)", strerror(errno));
		close(s);
		return 0;
	}
	stop = 0;
	while (have < all && !stop && (n = read(s, block, sizeof block)) > 0 && write(out, block, (size_t)n) == n) {
		have += n;
		if (coming)
			coming(have, all);
	}
	close(s);
	close(out);
	if (have < all) {
		unlink(to);
		snprintf(why, size, stop ? "given up" : "the link broke at %ld of %ld bytes", have, all);
		return 0;
	}
	return 1;
}

/* ---- the window ---- */

#define WIDTH 560
#define HEIGHT 340
#define BAR 28
#define STATUS 18
#define KEYS_Y (HEIGHT - STATUS - 30)
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ)

static GR_WINDOW_ID window;
static struct ui_list computers = { 8, BAR + 20, 170, KEYS_Y - BAR - 28, 0, 0, -1 };
static struct ui_list entries = { 186, BAR + 20, WIDTH - 194, KEYS_Y - BAR - 28, 0, 0, -1 };
static char folder[400] = "/", notice[200];
static int focus = 0;		/* the list the keys are for: the computers, or the entries */
static int busy;

static const char *
computer_label(int row)
{
	static char text[80];
	struct in_addr a = { found[row].address };

	snprintf(text, sizeof text, "%s  (%s)", found[row].name, inet_ntoa(a));
	return text;
}

static const char *
entry_label(int row)
{
	static char text[140];

	if (items[row].folder)
		snprintf(text, sizeof text, "[%s]", items[row].name);
	else if (items[row].size < 10000)
		snprintf(text, sizeof text, "%s   %ld bytes", items[row].name, items[row].size);
	else
		snprintf(text, sizeof text, "%s   %ld KB", items[row].name, (items[row].size + 512) / 1024);
	return text;
}

static void
draw_status(void)
{
	ui_fill(window, 0, HEIGHT - STATUS, WIDTH, STATUS, UI_FACE);
	ui_bevel(window, 0, HEIGHT - STATUS, WIDTH, STATUS, 1);
	ui_text_fit(window, 6, HEIGHT - STATUS + 3, WIDTH - 12, notice, BLACK);
}

static void
draw(void)
{
	struct in_addr own = { own_address() };
	int sharing = access(SWITCH, F_OK) != 0, chosen = computers.selected >= 0;
	char text[500];

	ui_fill(window, 0, 0, WIDTH, HEIGHT - STATUS, UI_FACE);
	ui_tool(window, 6, 3, 28, 22, UI_G_AGAIN, 0);
	ui_tool(window, 37, 3, 28, 22, UI_G_UP, chosen && folder[1] ? 0 : 2);
	if (own.s_addr)
		snprintf(text, sizeof text, "This computer is %s.   My Home is %s", inet_ntoa(own), sharing ? "shared: the others may read it" : "not shared");
	else
		snprintf(text, sizeof text, "This computer is not on a network.");
	ui_text_fit(window, 74, 8, WIDTH - 74 - 118, text, BLACK);
	ui_button(window, WIDTH - 112, 3, 106, 22, sharing ? "Stop sharing" : "Share my Home", 0);
	ui_text(window, 8, BAR + 3, "Computers", -1, UI_SHADOW, 0);
	if (chosen)
		snprintf(text, sizeof text, "%s: %s", found[computers.selected].name, folder);
	ui_text_fit(window, 186, BAR + 3, WIDTH - 194, chosen ? text : "Choose a computer to see what it shares", UI_SHADOW);
	ui_list_draw(window, &computers, computer_label);
	ui_list_draw(window, &entries, entry_label);
	ui_focus(window, focus ? entries.x + 2 : computers.x + 2, BAR + 22, (focus ? entries.width : computers.width) - 4, entries.height - 4);
	ui_button(window, 186, KEYS_Y, 70, 24, "   Open", 0);
	ui_button(window, 262, KEYS_Y, 150, 24, "Copy to my Home", 0);
	draw_status();
}

static void
say(const char *text)
{
	snprintf(notice, sizeof notice, "%s", text);
	if (window)
		draw_status(), GrFlush();
}

/* While an answer is waited for: the window is drawn when asked, and Escape gives a file up. */
static void
pump(void)
{
	GR_EVENT event;

	for (GrCheckNextEvent(&event); event.type != GR_EVENT_TYPE_NONE; GrCheckNextEvent(&event)) {
		if (event.type == GR_EVENT_TYPE_EXPOSURE)
			draw();
		else if (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == MWKEY_ESCAPE)
			stop = 1;
		else if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			exit(0);
	}
}

static void
coming(long have, long all)
{
	static long said;
	char text[120];

	if (have - said < 8192 && have < all && have > said)
		return;
	said = have;
	snprintf(text, sizeof text, "%ld of %ld KB have come (Escape gives it up)", have / 1024, (all + 1023) / 1024);
	say(text);
	pump();
}

/* Shows a folder of the chosen computer. */
static void
show(const char *path)
{
	char why[120];

	if (computers.selected < 0)
		return;
	say("asking it...");
	if (list_of(found[computers.selected].address, path, why, sizeof why)) {
		snprintf(folder, sizeof folder, "%s", path);
		entries.count = item_count;
		entries.top = 0;
		entries.selected = item_count ? 0 : -1;
		snprintf(notice, sizeof notice, "%d entries", item_count);
	} else {
		/* (what the list holds is the computer's before, and Open would ask this one for it) */
		entries.count = 0;
		entries.selected = -1;
		say(why);
	}
	draw();
}

static void
refresh(void)
{
	say("asking who is there...");
	find_all();
	computers.count = found_count;
	computers.top = 0;
	computers.selected = -1;
	entries.count = 0, entries.selected = -1;
	strcpy(folder, "/");
	snprintf(notice, sizeof notice, found_count ? "%d other computers answer" : own_address() ? "No other computer answers (one that shares does)" :
		 "No network here", found_count);
	draw();
}

static void
up(void)
{
	char above[400];
	char *slash;

	snprintf(above, sizeof above, "%s", folder);
	if ((slash = strrchr(above, '/')) != NULL)
		slash[slash == above ? 1 : 0] = 0;
	if (folder[1])
		show(above);
}

/* The selected entry: a folder is gone into; a file is fetched and opened, or (`keep`) kept
 * in /root under a name that is free there. */
static void
take(int keep)
{
	const struct item *e = &items[entries.selected];
	char from[600], to[300], why[160];
	struct in_addr a;
	int n;

	if (computers.selected < 0 || entries.selected < 0 || busy)
		return;
	snprintf(from, sizeof from, "%s%s%s", folder, folder[1] ? "/" : "", e->name);
	if (e->folder) {
		if (!keep)
			show(from);
		else
			say("A file is copied, not a folder");
		return;
	}
	a.s_addr = found[computers.selected].address;
	if (keep) {
		const char *dot = strrchr(e->name, '.');
		int stem = dot && dot != e->name ? (int)(dot - e->name) : (int)strlen(e->name);

		for (n = 1; n < 100; n++) {
			if (n == 1)
				snprintf(to, sizeof to, "/root/%s", e->name);
			else
				snprintf(to, sizeof to, "/root/%.*s-%d%s", stem, e->name, n, e->name + stem);
			if (access(to, F_OK) != 0)
				break;
		}
	} else {
		mkdir(KEPT, 0755);
		snprintf(to, sizeof to, KEPT "/%s", inet_ntoa(a));
		mkdir(to, 0755);
		snprintf(to, sizeof to, KEPT "/%s/%s", inet_ntoa(a), e->name);
	}
	busy = 1;
	if (!file_of(a.s_addr, from, to, why, sizeof why, coming)) {
		say(why);
	} else if (keep) {
		snprintf(notice, sizeof notice, "It is %.150s now", to);
		draw_status();
	} else {
		say("opening it");
		ui_run("open", to);
	}
	busy = 0;
}

static int
window_main(void)
{
	GR_EVENT event;
	int how, ch;

	if (GrOpen() < 0)
		return 1;
	signal(SIGCHLD, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	ui_init();
	window = GrNewWindowEx(UI_FIXED, "Network", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	waiting = pump;
	draw();
	refresh();
	for (;;) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if ((how = ui_list_event(&computers, &event)) != 0) {
				focus = 0;
				if (!ui_wheel(&event) && computers.selected >= 0)
					show("/");
				else
					draw();
			} else if ((how = ui_list_event(&entries, &event)) != 0) {
				focus = 1;
				draw();
				if (how == 2)
					take(0);
			} else if (ui_wheel(&event) || !(event.button.changebuttons & GR_BUTTON_L)) {
				break;
			} else if (ui_inside(event.button.x, event.button.y, 6, 3, 28, 22)) {
				refresh();
			} else if (ui_inside(event.button.x, event.button.y, 37, 3, 28, 22)) {
				up();
			} else if (ui_inside(event.button.x, event.button.y, WIDTH - 112, 3, 106, 22)) {
				/* the switch is a file: the computer that serves looks for it at every question */
				if (access(SWITCH, F_OK) == 0)
					unlink(SWITCH);
				else
					close(open(SWITCH, O_WRONLY | O_CREAT, 0644));
				draw();
			} else if (ui_inside(event.button.x, event.button.y, 186, KEYS_Y, 70, 24)) {
				take(0);
			} else if (ui_inside(event.button.x, event.button.y, 262, KEYS_Y, 150, 24)) {
				take(1);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			ch = event.keystroke.ch;
			if (ch == MWKEY_ESCAPE || ch == 'q') {
				GrClose();
				return 0;
			} else if (ch == MWKEY_TAB || ch == MWKEY_LEFT || ch == MWKEY_RIGHT) {
				focus = ch == MWKEY_TAB ? !focus : ch == MWKEY_RIGHT;
				draw();
			} else if (ch == MWKEY_F5) {
				refresh();
			} else if (ch == MWKEY_BACKSPACE) {
				up();
			} else if (ch == MWKEY_ENTER) {
				if (focus)
					take(0);
				else if (computers.selected >= 0)
					show("/"), focus = 1, draw();
			} else if (ui_list_key(focus ? &entries : &computers, ch)) {
				if (!focus)
					entries.count = 0, entries.selected = -1;	/* (another computer: its list is yet to be asked for) */
				draw();
			}
			break;
		}
	}
}

int
main(int argc, char **argv)
{
	char why[160];
	struct in_addr a;
	int i;

	if (argc > 1 && !strcmp(argv[1], "--serve"))
		return serve_all();
	if (argc > 1 && !strcmp(argv[1], "--one")) {
		serve(0);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--find")) {
		if (argc > 2 && atoi(argv[2]) > 0)
			find_tenths = atoi(argv[2]) * 10;
		find_all();
		for (i = 0; i < found_count; i++) {
			a.s_addr = found[i].address;
			printf("%s %s\n", inet_ntoa(a), found[i].name);
		}
		printf("nxnet: %d computers answer\n", found_count);
		return 0;
	}
	if (argc > 3 && !strcmp(argv[1], "--ls")) {
		if (!list_of(inet_addr(argv[2]), argv[3], why, sizeof why)) {
			printf("nxnet: %s\n", why);
			return 1;
		}
		for (i = 0; i < item_count; i++)
			printf("%c %8ld %s\n", items[i].folder ? 'd' : 'f', items[i].size, items[i].name);
		return 0;
	}
	if (argc > 4 && !strcmp(argv[1], "--get")) {
		if (!file_of(inet_addr(argv[2]), argv[3], argv[4], why, sizeof why, NULL)) {
			printf("nxnet: %s\n", why);
			return 1;
		}
		return 0;
	}
	return window_main();
}
