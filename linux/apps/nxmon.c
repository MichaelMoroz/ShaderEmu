/* nxmon: the monitor, a task manager laid out as Windows 7's (docs/system/nanox.md): Applications,
 * Processes, Performance and Networking. The graphs and meters are shaded by worker cores
 * straight into the window's buffer once a sample (docs/machine/multicore.md), or by this core without
 * them; samples come at the speed View chooses, and what no tab shows is not read. */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include "machine.h"
#include "registry.h"
#include "ui.h"
#include "ui_ask.h"
#include "ui_menu.h"
#include "mcw.h"

#define WIDTH 600
#define HEIGHT 500
#define MENU_H 20
#define TAB_Y 24
#define TAB_H 22
#define PANEL_Y (TAB_Y + TAB_H)
#define STATUS_H 22
#define STATUS_Y (HEIGHT - STATUS_H)
#define PANEL_BOTTOM (STATUS_Y - 4)
#define BUTTON_H 24
#define BUTTON_Y (PANEL_BOTTOM - 8 - BUTTON_H)
#define HEAD_H 20
#define LIST_X 12
#define LIST_W (WIDTH - 24)
#define LIST_Y (PANEL_Y + 10 + HEAD_H)
#define LIST_H (BUTTON_Y - 8 - LIST_Y)

/* Performance: two rows of a meter and a history, then three boxes of numbers */
#define CPU_BOX_Y (PANEL_Y + 8)
#define CPU_BOX_H 192
#define MEM_BOX_Y (CPU_BOX_Y + CPU_BOX_H + 6)
#define MEM_BOX_H 100
#define INFO_Y (MEM_BOX_Y + MEM_BOX_H + 6)
#define INFO_H (PANEL_BOTTOM - 4 - INFO_Y)
#define METER_BOX_W 112
#define METER_X 24
#define METER_W 88
#define CPU_METER_H 140
#define MEM_METER_H 56
#define HIST_X 132
#define HIST_W (WIDTH - 12 - HIST_X)
#define PLOT_X (HIST_X + (HIST_W - CPU_PLOT_W) / 2)
#define CPU_PLOT_Y (CPU_BOX_Y + 20)
#define CPU_PLOT_W 420
#define CPU_PLOT_H 160
#define MEM_PLOT_Y (MEM_BOX_Y + 18)
#define MEM_PLOT_H 72
/* Networking: the graph, then a row an adapter */
#define NET_BOX_Y (PANEL_Y + 8)
#define NET_BOX_H 216
#define NET_PLOT_X 30
#define NET_PLOT_Y (NET_BOX_Y + 20)
#define NET_PLOT_W 540
#define NET_PLOT_H 180
#define NET_LIST_Y (NET_BOX_Y + NET_BOX_H + 10 + HEAD_H)
#define MAPPED_ROWS (MEM_PLOT_Y + MEM_PLOT_H + 2)	/* the window's rows the workers may write */
#define MAX_PLOT_W NET_PLOT_W
#define MAX_PLOT_PIXELS (NET_PLOT_W * NET_PLOT_H)

#define HISTORY 60		/* samples a graph */
#define GRID_PX 12		/* the graphs' grid, which moves with the samples */
#define GRID 16			/* cores a row of the per-core grid */
#define CELL_W 26
#define CELL_H 10
#define CELL_SAMPLES (CELL_W - 1)	/* a pixel a sample */
#define MAX_CORES 256
#define MAX_PROGRAMS 96
#define MAX_TASKS 32
#define MAX_NETS 4
#define TAGS 4			/* worker tags a program may have: one an open of /dev/gpu that asked */
#define COLUMNS 6
#define CELL 48			/* a column's text */
#define VISIBLE 32		/* rows a list shows at most */
#define SLOTS 96

#define GRID_LINE 0xff00602eu
#define CORE0_COLOUR MWRGB(32, 224, 64)
#define WORKER_COLOUR MWRGB(48, 160, 255)
#define MEMORY_COLOUR MWRGB(48, 200, 240)
#define FILES_COLOUR MWRGB(240, 220, 90)
#define RECEIVED_COLOUR MWRGB(240, 220, 60)
#define SENT_COLOUR MWRGB(240, 64, 64)
#define LCD_TEXT MWRGB(0, 255, 0)

enum { APPLICATIONS, PROCESSES, PERFORMANCE, NETWORKING, TABS };
enum { CPU, MEMORY, NET, PLOTS };
enum { BY_NAME, BY_PID, BY_CPU, BY_WORKERS, BY_GPU, BY_MEMORY };
enum { HIGH, NORMAL, LOW, PAUSED };
enum { R_PLOT, R_GRID, R_METER };
enum {
	C_NEW = 1, C_EXIT, C_REFRESH, C_HIGH, C_NORMAL, C_LOW, C_PAUSED, C_ONE_GRAPH, C_PER_CORE, C_KERNEL, C_ABOUT,
	C_SWITCH, C_FRONT, C_MINIMISE, C_END_TASK, C_GO_PROCESS, C_END_PROCESS, C_END_TREE,
	C_PRI_HIGH, C_PRI_ABOVE, C_PRI_NORMAL, C_PRI_BELOW, C_PRI_LOW
};

static const int speed_ms[4] = { 500, 1000, 4000, 0 };
static const char *const tab_names[TABS] = { "Applications", "Processes", "Performance", "Networking" };

/* A graph: two values a sample, the second stacked on the first, `top` at the graph's top. */
static struct plot {
	unsigned low[HISTORY], high[HISTORY], top;
} plots[PLOTS] = { { {0}, {0}, 1000000 }, { {0}, {0}, 1 }, { {0}, {0}, 1024 } };
static const uint32_t shades[PLOTS][2] = { {0x20e040, 0x30a0ff}, {0x30c8f0, 0xf0dc5a}, {0xf0dc3c, 0xf04040} };
static unsigned char core_load[MAX_CORES][CELL_SAMPLES];	/* each core's last samples, 0 to 100 */
static unsigned char core_running[MAX_CORES];
static unsigned meter[2];	/* the meters: core 0's and memory's percent */
static unsigned samples_taken;	/* the grid's lines move a sample's width with each */

/* What the workers shade: places of the window, each a graph, the per-core grid or a meter. */
static struct region {
	int kind, which, x, y, w, h;
} regions[4];
static int region_count, region_rows;

static GR_WINDOW_ID window;
static int hidden;		/* minimised: samples are kept, nothing is drawn or shaded */
static int tab = PERFORMANCE, per_core, show_kernel, speed = NORMAL, has_network;

/* The window's own buffer in GPU memory (docs/system/nanox.md), which GrGetWindowInfo locates.
 * Workers given different rows of it never store to the same 16 bytes. */
#define SHADERS 8
static int gpu = -1;
static uint32_t *surface;	/* the window's first pixel; 0 until it is known */
static int surface_row;		/* pixels from one row of it to the next */
static uint32_t *checked;
static int workers;		/* lent to this program: another that asks has them (mcw_asked) */
static int wait_samples;	/* without them: samples to go before they are asked for again */
static int shading;		/* they are at a frame */
static int need_frame;		/* the regions are to be shaded again */
static int whole = 1, rescaled[PLOTS];	/* without workers: shaded whole, not moved a sample */
static int no_workers, check, print;
static unsigned frames_shaded;

/* The machine's control words (docs/machine/multicore.md, "Counters"): what each core ran, cores 16 on in
 * the network's row (0x876ba000); and a word a core with its program's tag (WORKERS_OWNER). */
static const volatile unsigned *core_words, *core_more, *owners;
static unsigned core_count = 1, core_before[MAX_CORES], core_rate[MAX_CORES];
static unsigned cores_working, cores_idle, cores_asleep, core_busiest;
static unsigned capacity;	/* instructions a second a core runs when it is never idle */
static unsigned speed_now, workers_rate, busy, kernel_busy;	/* the last sample: core 0, the workers, Linux's % */
static unsigned lists_rate, copies_rate;
static long memory_total, memory_used, memory_files, memory_cached, memory_slab, memory_free_block;	/* kB */
static long switches, interrupts;	/* a second */

static struct program {
	int pid, ppid, seen, share, workers, draws, kernel, nice;	/* kernel: a kernel thread */
	unsigned long ticks;
	unsigned ticks_at;	/* ms when `ticks` was read */
	unsigned worker_rate, lists, lists_before, lists_rate, tags[TAGS];
	long memory;		/* kB */
	char name[16];
} programs[MAX_PROGRAMS];
static int program_count, user_processes;
static unsigned programs_at;	/* ms when they were last read whole */
static struct ui_list list = { LIST_X, LIST_Y, LIST_W, LIST_H, 0, 0, -1 };
static int listed[MAX_PROGRAMS];	/* the process list's rows: programs, in the chosen order */
static int sort_by = BY_CPU, sort_down = 1;
static int chosen_pid;
static int ending_pid;		/* asked to end: made to if it is still there a while later */
static unsigned ending_at;
static char ending_name[16];	/* (a pid used again meanwhile is another program's) */

/* The Applications tab: a window of the desktop's each */
static struct task {
	GR_WINDOW_ID frame, client;
	int pid, hidden;
	char title[CELL];
} tasks[MAX_TASKS];
static int task_count, windows_dirty = 1, windows_looks;
static struct ui_list task_list = { LIST_X, LIST_Y, LIST_W, LIST_H, 0, 0, -1 };
static GR_WINDOW_ID chosen_frame;

static struct net {
	char name[16], address[20];
	unsigned long long rx, tx;
	unsigned long rx_packets, tx_packets;
	unsigned rx_rate, tx_rate;
	int up, known;
} nets[MAX_NETS];
static int net_count;
static struct ui_list net_list = { LIST_X, NET_LIST_Y, LIST_W, PANEL_BOTTOM - 8 - NET_LIST_Y, 0, 0, -1 };

/* The processor's count of instructions run (its low 32 bits; it wraps every few minutes). */
static unsigned
instructions(void)
{
	unsigned n;

#ifdef __gpuvm__
	__asm__ volatile("mfsr %0, cycle" : "=r"(n));
#else
	__asm__ volatile("rdcycle %0" : "=r"(n));
#endif
	return n;

}

/* A whole small file of /proc; the number of bytes read. */
static int
read_file(const char *name, char *text, int size)
{
	int file = open(name, O_RDONLY), n = 0, got;

	if (file < 0)
		return text[0] = 0;
	while (n < size - 1 && (got = read(file, text + n, size - 1 - n)) > 0)
		n += got;
	close(file);
	text[n] = 0;
	return n;
}

/* A process's /proc/PID file that comes whole with one read (a second one only said so). */
static int
read_once(const char *name, char *text, int size)
{
	int file = open(name, O_RDONLY), n = file < 0 ? 0 : (int)read(file, text, (size_t)size - 1);

	if (file >= 0)
		close(file);
	text[n > 0 ? n : 0] = 0;
	return n > 0 ? n : 0;
}

/* The number that follows `name` in a text, or 0; `from`, when given, is where to look from
 * and is left after the name (the files' fields come in one order). */
static long
field(const char *text, const char *name, const char **from)
{
	const char *at = strstr(from && *from ? *from : text, name);

	if (at && from)
		*from = at + strlen(name);
	return at ? atol(at + strlen(name)) : 0;
}

/* "3.07": a count of millions with two places. */
static const char *
millions(char *to, unsigned n)
{
	sprintf(to, "%u.%02u", n / 1000000, n % 1000000 / 10000);
	return to;
}

/* Bytes as a person reads them: "512 B", "3.4 KB", "1.2 MB". */
static const char *
bytes(char *to, unsigned long long n)
{
	if (n < 1024)
		sprintf(to, "%u B", (unsigned)n);
	else if (n < 1024 * 1024)
		sprintf(to, "%u.%u KB", (unsigned)(n / 1024), (unsigned)(n % 1024 * 10 / 1024));
	else
		sprintf(to, "%u.%u MB", (unsigned)(n >> 20), (unsigned)((n & 0xfffff) * 10 >> 20));
	return to;
}

/* Milliseconds: the machine's clock word when it is mapped (a load), else the kernel's. */
static unsigned
ms_now(void)
{
	struct timeval now;

	if (core_words)
		return core_words[0x34 / 4];
	gettimeofday(&now, NULL);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_usec / 1000);
}

/* ---- samples ---- */

/* Linux's time since the last sample by what it went to, and its counts of events: /proc/stat. */
static void
sample_linux(long ms)
{
	static long before[8], before_switches, before_interrupts;
	static char text[4096];
	long v[8] = {0}, d[8], all = 0, now_switches, now_interrupts;
	const char *at = NULL;
	char *next;
	int i;

	read_file("/proc/stat", text, sizeof text);
	for (i = 0, next = text + 3; i < 8 && !strncmp(text, "cpu ", 4); i++) {
		char *number = next;

		v[i] = strtol(number, &next, 10);
		if (next == number)
			break;
	}
	for (i = 0; i < 8; i++) {
		d[i] = v[i] - before[i];
		all += d[i];
		before[i] = v[i];
	}
	/* user and nice are programs; system and the two interrupt times the kernel */
	kernel_busy = all > 0 ? (unsigned)(100 * (d[2] + d[5] + d[6]) / all) : 0;
	busy = all > 0 ? (unsigned)(100 * (d[0] + d[1]) / all) + kernel_busy : 0;
	now_interrupts = field(text, "\nintr ", &at);
	now_switches = field(text, "\nctxt ", &at);
	switches = (now_switches - before_switches) * 1000 / ms;
	interrupts = (now_interrupts - before_interrupts) * 1000 / ms;
	before_switches = now_switches;
	before_interrupts = now_interrupts;
}

/* What each worker core ran since the last sample, a second's worth; the sum of them, and
 * how many of them work (10,000 instructions a second or more), wait or sleep. */
static unsigned
sample_cores(long ms)
{
	unsigned c, now, sum = 0;

	if (!core_words)
		return 0;
	core_count = core_words[0x3c0 / 4] < 1 ? 1 : core_words[0x3c0 / 4] > MAX_CORES ? MAX_CORES : core_words[0x3c0 / 4];
	if (core_count > 16 && !core_more)
		core_count = 16;
	cores_working = cores_idle = cores_asleep = core_busiest = 0;
	for (c = 1; c < core_count; c++) {
		unsigned runs = c < 32 ? core_words[0x3c4 / 4] >> c : core_more[0x400 / 4 + 4 * (c / 32)] >> (c % 32);
		unsigned sleeps = c < 32 ? core_words[0x3c8 / 4] >> c : core_more[0x404 / 4 + 4 * (c / 32)] >> (c % 32);

		now = c < 16 ? core_words[0x380 / 4 + c] : core_more[c];
		/* (a geometry laid out anew starts the counts again: one that went down is all new) */
		unsigned ran = now < core_before[c] ? now : now - core_before[c];

		/* (most cores ran nothing, and a small count needs no division of 64 bits) */
		core_rate[c] = !ran ? 0 : ran < 4000000u ? ran * 1000u / (unsigned)ms : (unsigned)((unsigned long long)ran * 1000 / ms);
		core_before[c] = now;
		core_running[c] = runs & 1;
		sum += core_rate[c];
		if (core_rate[c] > core_busiest)
			core_busiest = core_rate[c];
		if (core_rate[c] >= 10000)
			cores_working++;
		else if (sleeps & 1)
			cores_asleep++;
		else if (runs & 1)
			cores_idle++;
	}
	return sum;
}

/* The largest run of free memory the kernel has as one block: the highest order /proc/buddyinfo
 * counts any of, in kB. Without an MMU a program's memory must be such a run. */
static long
largest_free_block(void)
{
	static char text[1024];
	long most = 0;
	char *line, *at;

	read_file("/proc/buddyinfo", text, sizeof text);
	for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
		int order = 0;

		at = strstr(line, "zone");
		if (!at || (strchr(line, '\n') && at > strchr(line, '\n')))
			continue;
		for (at += 4; *at == ' '; at++)
			;
		while (*at && *at != ' ')
			at++;	/* the zone's name */
		for (;;) {
			char *number = at;
			long count = strtol(number, &at, 10);

			if (at == number)
				break;
			if (count > 0 && (4L << order) > most)
				most = 4L << order;
			order++;
		}
	}
	return most;
}

static void
sample_memory(int details)
{
	static char text[2048];
	const char *at = NULL;

	read_file("/proc/meminfo", text, sizeof text);
	memory_total = field(text, "MemTotal:", &at);
	memory_used = memory_total - field(text, "MemAvailable:", &at);
	memory_cached = field(text, "\nCached:", &at);
	memory_files = field(text, "Shmem:", &at);	/* files written since the start live in memory */
	at = NULL;
	memory_slab = field(text, "\nSlab:", &at);
	if (details)
		memory_free_block = largest_free_block();
	plots[MEMORY].top = memory_total > 0 ? (unsigned)memory_total : 1;
	plots[MEMORY].low[HISTORY - 1] = memory_used > memory_files ? (unsigned)(memory_used - memory_files) : 0;
	plots[MEMORY].high[HISTORY - 1] = (unsigned)memory_files;
	meter[1] = memory_total > 0 ? (unsigned)(memory_used * 100 / memory_total) : 0;
}

/* The network's adapters from /proc/net/dev (not the loopback); with `details` whether each is
 * up and its address. True if there is one. */
static int
sample_net(long ms, int details)
{
	static char text[2048];
	char *line;
	int i, sock = -1;

	read_file("/proc/net/dev", text, sizeof text);
	for (i = 0; i < net_count; i++)
		nets[i].known = 0;
	for (line = strchr(text, '\n'); line && (line = strchr(line + 1, '\n')) && line[1];) {
		char name[16], *colon = strchr(line + 1, ':');
		unsigned long long rx, tx;
		unsigned long rxp, txp;
		struct net *n;
		int k = 0;

		if (!colon)
			break;
		for (line++; *line == ' '; line++)
			;
		while (line < colon && k < 15)
			name[k++] = *line++;
		name[k] = 0;
		line = colon;
		if (!strcmp(name, "lo") || sscanf(colon + 1, "%llu %lu %*u %*u %*u %*u %*u %*u %llu %lu", &rx, &rxp, &tx, &txp) != 4)
			continue;
		for (i = 0; i < net_count && strcmp(nets[i].name, name); i++)
			;
		if (i == MAX_NETS)
			continue;
		n = &nets[i];
		if (i == net_count) {
			memset(n, 0, sizeof *n);
			snprintf(n->name, sizeof n->name, "%s", name);
			n->rx = rx, n->tx = tx;
			net_count++;
		}
		n->rx_rate = ms > 0 && rx >= n->rx ? (unsigned)((rx - n->rx) * 1000 / (unsigned long long)ms) : 0;
		n->tx_rate = ms > 0 && tx >= n->tx ? (unsigned)((tx - n->tx) * 1000 / (unsigned long long)ms) : 0;
		n->rx = rx, n->tx = tx, n->rx_packets = rxp, n->tx_packets = txp;
		n->known = 1;
		if (details) {
			char path[64], state[16];
			struct ifreq request;

			snprintf(path, sizeof path, "/sys/class/net/%s/operstate", name);
			read_file(path, state, sizeof state);
			n->up = !strncmp(state, "up", 2) || !strncmp(state, "unknown", 7);
			if (sock < 0)
				sock = socket(AF_INET, SOCK_DGRAM, 0);
			memset(&request, 0, sizeof request);
			snprintf(request.ifr_name, sizeof request.ifr_name, "%s", name);
			if (sock >= 0 && ioctl(sock, SIOCGIFADDR, &request) == 0)
				snprintf(n->address, sizeof n->address, "%s",
					 inet_ntoa(((struct sockaddr_in *)&request.ifr_addr)->sin_addr));
			else
				snprintf(n->address, sizeof n->address, "none");
		}
	}
	if (sock >= 0)
		close(sock);
	plots[NET].low[HISTORY - 1] = plots[NET].high[HISTORY - 1] = 0;
	for (i = 0; i < net_count; i++) {
		plots[NET].low[HISTORY - 1] += nets[i].rx_rate;
		plots[NET].high[HISTORY - 1] += nets[i].tx_rate;
	}
	return net_count > 0;
}

static struct program *
program_of(int pid)
{
	int i;

	for (i = 0; i < program_count; i++)
		if (programs[i].pid == pid)
			return &programs[i];
	return NULL;
}

/* Who has the GPU open (/proc/gpu, a line an open): workers held, their tags, lists drawn;
 * then what each program's cores ran, by the cores' owner words. */
static void
sample_gpu_users(long ms)
{
	static char text[2048];
	char *line;
	unsigned c;
	int i;

	for (i = 0; i < program_count; i++) {
		programs[i].workers = programs[i].draws = 0;
		programs[i].worker_rate = programs[i].lists = 0;
		memset(programs[i].tags, 0, sizeof programs[i].tags);
	}
	read_file("/proc/gpu", text, sizeof text);
	for (line = strchr(text, '\n'); line && line[1]; line = strchr(line + 1, '\n')) {
		unsigned pid, held, wanted, lent, tag, draws, lists;
		struct program *p;

		if (sscanf(line + 1, "%u %u %u %u %u %u %u", &pid, &held, &wanted, &lent, &tag, &draws, &lists) != 7)
			continue;
		p = program_of((int)pid);
		if (!p)
			continue;
		p->workers += (int)held;
		p->draws |= (int)draws;
		p->lists += lists + 1;	/* (+1: an open with no list yet still counts as the GPU's) */
		for (i = 0; i < TAGS; i++)
			if (!p->tags[i] || p->tags[i] == tag) {
				p->tags[i] = tag;
				break;
			}
	}
	for (c = 1; owners && c < core_count; c++) {
		unsigned tag = owners[0x400 / 4 + c];

		for (i = 0; tag && i < program_count; i++)
			if (programs[i].tags[0] == tag || programs[i].tags[1] == tag || programs[i].tags[2] == tag || programs[i].tags[3] == tag) {
				programs[i].worker_rate += core_rate[c];
				break;
			}
	}
	for (i = 0; i < program_count; i++) {
		struct program *p = &programs[i];

		p->lists_rate = p->lists_before && p->lists > p->lists_before ? (unsigned)((p->lists - p->lists_before) * 1000 / ms) : 0;
		p->lists_before = p->lists;
	}
}

/* Reads /proc/PID/stat into p: its parent, its ticks and whether it is a kernel thread. */
static int
read_stat(struct program *p, unsigned now)
{
	char name[40], text[400], *open_at, *close_at, *at;
	unsigned long user, kernel;
	int skip, n;

	snprintf(name, sizeof name, "/proc/%d/stat", p->pid);
	read_once(name, text, sizeof text);
	open_at = strchr(text, '(');
	close_at = strrchr(text, ')');
	if (!open_at || !close_at || close_at[1] != ' ')
		return 0;
	n = (int)(close_at - open_at - 1);
	n = n < (int)sizeof p->name - 1 ? n : (int)sizeof p->name - 1;
	memcpy(p->name, open_at + 1, (size_t)n);
	p->name[n] = 0;
	p->ppid = atoi(close_at + 4);	/* after ") S " */
	/* after the name: the state and ten numbers, then the time as a program and in the kernel */
	for (at = close_at + 1, skip = 0; skip < 11 && *at; skip++) {
		while (*at == ' ')
			at++;
		while (*at && *at != ' ')
			at++;
	}
	if (skip < 11)
		return 0;
	user = strtoul(at, &at, 10);
	kernel = strtoul(at, &at, 10);
	strtol(at, &at, 10), strtol(at, &at, 10), strtol(at, &at, 10);	/* the children's two times, the priority */
	p->nice = (int)strtol(at, &at, 10);
	strtol(at, &at, 10), strtol(at, &at, 10), strtoull(at, &at, 10);	/* threads, a timer, the start */
	p->memory = (long)(strtoul(at, &at, 10) / 1024);	/* (without an MMU what it was given is what it has) */
	p->kernel = p->pid == 2 || p->ppid == 2;
	/* a tick is a hundredth of a second */
	p->share = p->ticks_at && now != p->ticks_at ? (int)((user + kernel - p->ticks) * 1000 / (now - p->ticks_at)) : 0;
	p->share = p->share > 100 ? 100 : p->share < 0 ? 0 : p->share;
	p->ticks = user + kernel;
	p->ticks_at = now;
	return 1;
}

/* The processes. `full`: each one's share of the processor since it was last looked at, its
 * memory (its stat says that too) and its GPU; otherwise only what is new is read, for the count. */
static void
sample_programs(int full)
{
	DIR *folder = opendir("/proc");
	struct dirent *item;
	unsigned now = ms_now();
	int i, n;

	if (full)
		programs_at = now;
	for (i = 0; i < program_count; i++)
		programs[i].seen = 0;
	while (folder && (item = readdir(folder)) != NULL) {
		int pid = atoi(item->d_name);
		struct program *p;

		if (pid <= 0)
			continue;
		p = program_of(pid);
		if (!p) {
			if (program_count == MAX_PROGRAMS)
				continue;
			p = &programs[program_count];
			memset(p, 0, sizeof *p);
			p->pid = pid;
			if (!read_stat(p, now))
				continue;
			program_count++;
			p->share = 0;
		} else if (full && (!p->kernel || show_kernel) && !read_stat(p, now))
			continue;
		p->seen = 1;
	}
	if (folder)
		closedir(folder);
	for (i = n = user_processes = 0; i < program_count; i++)
		if (programs[i].seen) {
			user_processes += !programs[i].kernel;
			programs[n++] = programs[i];
		}
	program_count = n;
	if (full) {
		static unsigned gpu_at;

		sample_gpu_users(gpu_at && now != gpu_at ? (long)(now - gpu_at) : 1000);
		gpu_at = now;
	}
}

/* The desktop's windows: every frame the window manager made round a program's window, with
 * its title (asked again only when `titles`). True if anything shown changed. */
static int
find_windows(int titles)
{
	static struct task was[MAX_TASKS];
	GR_WINDOW_ID parent, *children = NULL, *inside;
	GR_COUNT n = 0, m, i;
	int was_count = task_count, k, changed;

	memcpy(was, tasks, sizeof tasks);
	task_count = 0;
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n && task_count < MAX_TASKS; i++) {
		struct task *t = &tasks[task_count];
		GR_WINDOW_INFO info;

		GrGetWindowInfo(children[i], &info);
		if (info.props & GR_WM_PROPS_NODECORATE)
			continue;
		for (k = 0; k < was_count && was[k].frame != children[i]; k++)
			;
		t->frame = children[i];
		t->hidden = !info.mapped;
		t->pid = info.processid;
		t->client = k < was_count ? was[k].client : 0;
		if (!t->client) {
			inside = NULL;
			m = 0;
			GrQueryTree(children[i], &parent, &inside, &m);
			t->client = m > 0 ? inside[0] : 0;
			free(inside);
			if (!t->client)
				continue;	/* (no frame: not a program's window) */
		}
		if (titles || k == was_count) {
			GR_WM_PROPERTIES props;

			props.title = NULL;
			GrGetWMProperties(t->client, &props);
			snprintf(t->title, sizeof t->title, "%s", props.title && props.title[0] ? (char *)props.title : "(no title)");
			free(props.title);
		} else
			memcpy(t->title, was[k].title, sizeof t->title);
		task_count++;
	}
	free(children);
	changed = task_count != was_count;
	for (i = 0; i < task_count && !changed; i++)
		changed = was[i].frame != tasks[i].frame || was[i].hidden != tasks[i].hidden || strcmp(was[i].title, tasks[i].title);
	if (changed && print)
		for (i = 0; i < task_count; i++)
			printf("nxmon: window \"%s\" of process %d%s\n", tasks[i].title, tasks[i].pid, tasks[i].hidden ? ", minimised" : "");
	task_list.count = task_count;
	task_list.selected = -1;
	for (i = 0; i < task_count; i++)
		if (tasks[i].frame == chosen_frame)
			task_list.selected = i;
	if (task_list.top > task_count - ui_list_rows(&task_list))
		task_list.top = task_count > ui_list_rows(&task_list) ? task_count - ui_list_rows(&task_list) : 0;
	return changed;
}

/* ---- shading: what a worker does, and this core without them ---- */

/* Colour b over colour a, k 256ths of it. */
static uint32_t
blend(uint32_t a, uint32_t b, unsigned k)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - k) + (b & 0xff00ff) * k) >> 8 & 0xff00ff;
	uint32_t g = ((a & 0x00ff00) * (256 - k) + (b & 0x00ff00) * k) >> 8 & 0x00ff00;

	return 0xff000000u | rb | g;
}

/* Rows y0 to y1, columns x0 to x1 of a graph: its two areas, the second on the first, straight
 * from one sample to the next, brighter towards the top, with the grid behind them. */
static void
shade_plot(const struct region *r, int x0, int x1, int y0, int y1, uint32_t *to, int stride)
{
	const struct plot *p = &plots[r->which];
	unsigned low[HISTORY], all[HISTORY], most = (unsigned)r->h * 256;
	unsigned short low_at[MAX_PLOT_W], all_at[MAX_PLOT_W];
	int step = r->w / HISTORY, scroll = (int)(samples_taken % GRID_PX) * step % GRID_PX, i, x, y;

	for (i = 0; i < HISTORY; i++) {
		unsigned l = !p->low[i] ? 0 : (unsigned)((unsigned long long)p->low[i] * most / p->top);
		unsigned t = !p->high[i] ? 0 : (unsigned)((unsigned long long)p->high[i] * most / p->top);

		low[i] = l = l > most ? most : l;
		all[i] = l + t > most ? most : l + t;
	}
	for (x = x0; x < x1; x++) {
		int u = (x + 1) * 256 / step - 256, f;

		u = u < 0 ? 0 : u > (HISTORY - 1) * 256 ? (HISTORY - 1) * 256 : u;
		i = u >> 8;
		f = u & 255;
		if (i > HISTORY - 2) {
			i = HISTORY - 2;
			f = 256;
		}
		low_at[x] = (unsigned short)((low[i] * (unsigned)(256 - f) + low[i + 1] * (unsigned)f) >> 8);
		all_at[x] = (unsigned short)((all[i] * (unsigned)(256 - f) + all[i + 1] * (unsigned)f) >> 8);
	}
	for (y = y0; y < y1; y++) {
		uint32_t *row = to + y * stride;
		int base = (r->h - 1 - y) * 256;
		unsigned light = 150 + 105 * (unsigned)(r->h - y) / (unsigned)r->h;
		uint32_t low_c = blend(0, shades[r->which][0], light), high_c = blend(0, shades[r->which][1], light);
		uint32_t behind = (r->h - 1 - y) % GRID_PX == GRID_PX - 1 ? GRID_LINE : 0xff000000u;

		for (x = x0; x < x1; x++) {
			int below = low_at[x] - base, under = all_at[x] - base;
			uint32_t c = (x + scroll) % GRID_PX == GRID_PX - 1 ? GRID_LINE : behind;

			if (under >= 256)
				c = high_c;
			else if (under > 0)
				c = blend(c, high_c, (unsigned)under);
			if (below >= 256)
				c = low_c;
			else if (below > 0)
				c = blend(c, low_c, (unsigned)below);
			row[x] = c;
		}
	}
}

/* Rows y0 to y1 of the per-core grid: a cell a core, its last samples' load as an area, the
 * first core's in its own colour; a core the machine has not laid out is darker, one it lacks
 * darker still. */
static void
shade_grid(const struct region *r, int y0, int y1, uint32_t *to, int stride)
{
	int x, y;

	for (y = y0; y < y1; y++) {
		uint32_t *row = to + y * stride;
		int cell_y = y % CELL_H, base = (CELL_H - 2 - cell_y) * 256;
		unsigned light = 150 + 105 * (unsigned)(CELL_H - 1 - cell_y) / (CELL_H - 1);
		uint32_t worker = blend(0, shades[CPU][1], light), first = blend(0, shades[CPU][0], light);

		for (x = 0; x < r->w; x++) {
			unsigned core = (unsigned)(y / CELL_H * GRID + x / CELL_W), cell_x = (unsigned)(x % CELL_W);
			uint32_t c;
			int height;

			if (cell_x == CELL_W - 1 || cell_y == CELL_H - 1 || x >= GRID * CELL_W) {
				row[x] = GRID_LINE;
				continue;
			}
			if (core >= core_count) {
				row[x] = 0xff080808u;
				continue;
			}
			c = core && !core_running[core] ? 0xff141414u : 0xff000000u;
			height = core_load[core][cell_x] * (CELL_H - 1) * 256 / 100 - base;
			if (height >= 256)
				c = core ? worker : first;
			else if (height > 0)
				c = blend(c, core ? worker : first, (unsigned)height);
			row[x] = c;
		}
	}
}

/* Rows y0 to y1 of a meter: two columns of bars, lit from the bottom to its percent. */
static void
shade_meter(const struct region *r, int y0, int y1, uint32_t *to, int stride)
{
	int bars = r->h / 4, lit = ((int)meter[r->which] * bars + 50) / 100, left = (r->w - 56) / 2, x, y;

	for (y = y0; y < y1; y++) {
		uint32_t *row = to + y * stride;
		int from_bottom = r->h - 1 - y, bar = from_bottom / 4;
		uint32_t on = from_bottom % 4 == 3 || bar >= bars ? 0xff000000u : bar < lit ? 0xff00ff00u : 0xff00501eu;

		for (x = 0; x < r->w; x++)
			row[x] = (x >= left && x < left + 24) || (x >= left + 32 && x < left + 56) ? on : 0xff000000u;
	}
}

static void
shade_region(const struct region *r, int y0, int y1, uint32_t *to, int stride)
{
	if (r->kind == R_PLOT)
		shade_plot(r, 0, r->w, y0, y1, to, stride);
	else if (r->kind == R_GRID)
		shade_grid(r, y0, y1, to, stride);
	else
		shade_meter(r, y0, y1, to, stride);
}

static uint32_t *
region_at(const struct region *r)
{
	return surface + r->y * surface_row + r->x;
}

/* A worker's job: rows `rows & 0xffff` up to `rows >> 16` of the regions, one after another. */
static uint32_t
shade_rows(uint32_t rows, uint32_t unused)
{
	int a = (int)(rows & 0xffff), b = (int)(rows >> 16), from = 0, i;

	(void)unused;
	for (i = 0; i < region_count; i++) {
		const struct region *r = &regions[i];
		int y0 = a > from ? a - from : 0, y1 = b - from < r->h ? b - from : r->h;

		if (y0 < y1)
			shade_region(r, y0, y1, region_at(r), surface_row);
		from += r->h;
	}
	return (uint32_t)(b - a);
}

/* What the tab shows that is shaded, not drawn. */
static void
set_regions(void)
{
	static const struct region performance[4] = {
		{ R_PLOT, CPU, PLOT_X, CPU_PLOT_Y, CPU_PLOT_W, CPU_PLOT_H },
		{ R_PLOT, MEMORY, PLOT_X, MEM_PLOT_Y, CPU_PLOT_W, MEM_PLOT_H },
		{ R_METER, 0, METER_X, CPU_PLOT_Y, METER_W, CPU_METER_H },
		{ R_METER, 1, METER_X, MEM_PLOT_Y, METER_W, MEM_METER_H },
	};
	static const struct region networking = { R_PLOT, NET, NET_PLOT_X, NET_PLOT_Y, NET_PLOT_W, NET_PLOT_H };
	int i;

	region_count = 0;
	if (tab == PERFORMANCE) {
		memcpy(regions, performance, sizeof performance);
		if (per_core)
			regions[0].kind = R_GRID;
		region_count = 4;
	} else if (tab == NETWORKING) {
		regions[0] = networking;
		region_count = 1;
	}
	for (i = region_rows = 0; i < region_count; i++)
		region_rows += regions[i].h;
}

/* Finds the window's buffer and maps it, and takes the workers: once the window is there. */
static void
locate(void)
{
	GR_WINDOW_INFO info;
	uint32_t offset, size;
	void *memory;

	if (surface || gpu < 0)
		return;
	GrGetWindowInfo(window, &info);
	if (!info.surface_address)
		return;
	surface_row = info.surface_row ? (int)info.surface_row : (int)info.width;
#ifdef __gpuvm__
	offset = info.surface_address;	/* (gpu_vm: the device window starts at 0) */
#else
	offset = info.surface_address - 0x86000000u;
#endif
	size = (offset & 4095) + (uint32_t)surface_row * MAPPED_ROWS * 4;
	memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, gpu, offset & ~4095u);
	if (memory == MAP_FAILED)
		return;
	surface = (uint32_t *)((char *)memory + (offset & 4095));
	mcw_touch(memory, size);
	mcw_lend();
	workers = no_workers ? 0 : mcw_open(SHADERS);
}

/* The workers' frame, if they are at one, finished: before what they read or write changes. */
static void
finish_frame(void)
{
	int k;

	if (!shading)
		return;
	for (k = 1; k <= workers; k++)
		mcw_wait(k);
	shading = 0;
	frames_shaded++;
}

/* Another program is waiting for the workers: it has them, and this core shades meanwhile. */
static void
give_workers(void)
{
	finish_frame();
	mcw_close();
	workers = 0;
	whole = 1;
	wait_samples = 5;
	if (print)
		puts("nxmon: the workers are given to a program that asked");
}

/* Without workers, at a sample: they are taken again once some have been nobody's for a while. */
static void
take_workers(void)
{
	unsigned nobodys = core_count - 1 - cores_working - cores_asleep - cores_idle;

	if (no_workers || !surface)
		return;
	if (!nobodys)
		wait_samples = 5;
	else if (--wait_samples <= 0)
		workers = mcw_open(SHADERS);
}

static int
frame_done(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		if (!mcw_done(k))
			return 0;
	return 1;
}

/* What the workers have just shaded, shaded again on this core and compared (a test). */
static void
check_frame(void)
{
	int i, x, y;

	for (i = 0; i < region_count; i++) {
		const struct region *r = &regions[i];
		int differ = 0;

		shade_region(r, 0, r->h, checked, r->w);
		for (y = 0; y < r->h; y++)
			for (x = 0; x < r->w; x++)
				differ += checked[y * r->w + x] != region_at(r)[y * surface_row + x];
		if (i == 0)
			printf("nxmon: %d of %d pixels differ from this core's\n", differ, r->w * r->h);
		else
			printf("nxmon: %d of %d pixels of region %d differ\n", differ, r->w * r->h, i);
	}
}

/* The window system draws by taking the window's buffer to the GPU and copying it back whole
 * (docs/system/nanox.md), which would undo what is written meanwhile: after asking it to draw, a
 * pixel is read, which is answered when all of that is done. */
static void
settle(void)
{
	GR_PIXELVAL pixel;

	GrReadArea(window, 0, 0, 1, 1, &pixel);
}

static void
post_frame(void)
{
	int k;

	for (k = 1; k <= workers; k++)
		mcw_post(k, shade_rows, (uint32_t)(region_rows * (k - 1) / workers) | (uint32_t)(region_rows * k / workers) << 16, 0);
	shading = 1;
}

/* Without workers: each graph moved a sample to the left and its newest columns shaded; the
 * rest whole (and a graph whose scale changed, or the window was drawn). */
static void
shade_here(void)
{
	int i, y;

	for (i = 0; i < region_count; i++) {
		const struct region *r = &regions[i];
		int step = r->w / HISTORY;
		uint32_t *at = region_at(r);

		if (r->kind != R_PLOT || whole || rescaled[r->which]) {
			shade_region(r, 0, r->h, at, surface_row);
			continue;
		}
		for (y = 0; y < r->h; y++)
			memmove(at + y * surface_row, at + y * surface_row + step, (size_t)(r->w - step) * sizeof at[0]);
		shade_plot(r, r->w - step, r->w, 0, r->h, at, surface_row);
	}
	whole = 0;
	memset(rescaled, 0, sizeof rescaled);
}

/* ---- drawing ---- */

static char shown[SLOTS][64];

/* What text_at last drew is forgotten: everything is drawn again. */
static void
forget_texts(void)
{
	int i;

	for (i = 0; i < SLOTS; i++)
		strcpy(shown[i], "\1");
}

static int
text_width(const char *text)
{
	return ui_width(&ui_var_widths, ui_var, text, -1);
}

/* A line of text in a box `width` wide, drawn again only when it reads otherwise than it did.
 * `slot` names the place; align 0 is left, 1 right, 2 the middle. */
static void
text_at(int slot, int x, int y, int width, const char *text, GR_COLOR colour, GR_COLOR ground, int align)
{
	int tw;

	if (width < 1 || !strcmp(shown[slot], text))
		return;
	snprintf(shown[slot], sizeof shown[slot], "%s", text);
	ui_fill(window, x, y, width, 15, ground);
	tw = text_width(text);
	if (tw > width)	/* (cut to its box: what is drawn past it is never cleared) */
		ui_text_fit(window, x, y, width, text, colour);
	else
		ui_text(window, align == 1 ? x + width - tw : align == 2 ? x + (width - tw) / 2 : x, y, text, -1, colour, 0);
}

/* A box round a group of things, its title in its top edge. */
static void
group(int x, int y, int w, int h, const char *title)
{
	if (ui_seven) {
		GrSetGCForeground(ui_gc, MWRGB(213, 223, 229));
		GrRect(window, ui_gc, x, y + 7, w, h - 7);
	} else {
		ui_edges(window, x, y + 7, w, h - 7, UI_SHADOW, WHITE);
		ui_edges(window, x + 1, y + 8, w - 2, h - 9, WHITE, UI_SHADOW);
	}
	ui_fill(window, x + 6, y, text_width(title) + 4, 15, UI_FACE);
	ui_text(window, x + 8, y, title, -1, BLACK, 0);
}

/* Small squares of a graph's colours and what they stand for, from x on; where they end. */
static int
legend(int x, int y, GR_COLOR colour, const char *text)
{
	ui_fill(window, x - 2, y, text_width(text) + 18, 15, UI_FACE);
	ui_fill(window, x, y + 4, 8, 8, colour);
	ui_text(window, x + 12, y, text, -1, BLACK, 0);
	return x + 12 + text_width(text) + 10;
}

static const char *const menu_names[3] = { "File", "View", "Help" };
static int menu_x[4], tab_x[TABS + 1];

static int
tab_count(void)
{
	return has_network ? TABS : TABS - 1;
}

static void
draw_menu_bar(void)
{
	int i, x = 2;

	ui_fill(window, 0, 0, WIDTH, MENU_H, UI_FACE);
	for (i = 0; i < 3; i++) {
		menu_x[i] = x;
		ui_text(window, x + 8, 3, menu_names[i], -1, BLACK, 0);
		x += text_width(menu_names[i]) + 16;
	}
	menu_x[3] = x;
}

/* The tabs, the chosen one joined to the page under it, and the page's edge. */
static void
draw_tabs(void)
{
	int i, x = 6;

	ui_fill(window, 0, MENU_H, WIDTH, PANEL_Y - MENU_H, UI_FACE);
	ui_bevel(window, 4, PANEL_Y, WIDTH - 8, PANEL_BOTTOM - PANEL_Y, 0);
	for (i = 0; i < tab_count(); i++) {
		int w = text_width(tab_names[i]) + 20, up = i == tab ? 2 : 0;
		GR_COLOR face = i == tab || !ui_seven ? UI_FACE : MWRGB(225, 225, 225);

		tab_x[i] = x;
		ui_fill(window, x, TAB_Y - up, w, TAB_H + up, face);
		if (ui_seven)
			ui_edges(window, x, TAB_Y - up, w, TAB_H + up + 1, MWRGB(137, 140, 149), MWRGB(137, 140, 149));
		else
			ui_edges(window, x, TAB_Y - up, w, TAB_H + up + 1, WHITE, UI_SHADOW);
		if (i == tab)
			ui_fill(window, x + 1, PANEL_Y, w - 2, 1, UI_FACE);
		ui_text(window, x + 10, TAB_Y + 4 - up / 2, tab_names[i], -1, BLACK, 0);
		x += w + (i == tab ? 0 : 1);
	}
	tab_x[i] = x;
}

/* The status bar: processes, core 0's use and memory's, a pane each. */
static void
draw_status(int again)
{
	static const int x[4] = { 4, 140, 300, WIDTH - 4 };
	char text[3][48];
	int i;

	if (again) {
		ui_fill(window, 0, STATUS_Y, WIDTH, STATUS_H, UI_FACE);
		for (i = 0; i < 3; i++)
			ui_bevel(window, x[i], STATUS_Y + 2, x[i + 1] - x[i] - 2, STATUS_H - 4, 1);
	}
	snprintf(text[0], 48, "Processes: %d", user_processes);
	snprintf(text[1], 48, "CPU Usage: %u%%", busy > 100 ? 100 : busy);
	snprintf(text[2], 48, "Physical Memory: %u%%", meter[1]);
	for (i = 0; i < 3; i++)
		text_at(i, x[i] + 6, STATUS_Y + 4, x[i + 1] - x[i] - 14, text[i], BLACK, UI_FACE, 0);
}

/* A list's titles: a cell a column, the one it is in the order of with an arrow. */
static void
draw_head(const struct ui_list *l, int columns, const int *x, const char *const *names, int sorted, int down)
{
	int c, y = l->y - HEAD_H;

	for (c = 0; c < columns; c++) {
		int x0 = l->x + x[c] - 6, x1 = c + 1 < columns ? l->x + x[c + 1] - 6 : l->x + l->width;

		if (ui_seven) {
			ui_fill(window, x0, y, x1 - x0, HEAD_H, WHITE);
			ui_fill(window, x1 - 1, y + 2, 1, HEAD_H - 4, MWRGB(229, 229, 229));
			ui_fill(window, x0, y + HEAD_H - 1, x1 - x0, 1, MWRGB(213, 213, 213));
		} else
			ui_button(window, x0, y, x1 - x0, HEAD_H, "", 0);
		ui_text(window, x0 + 6, y + 4, names[c], -1, BLACK, 0);
		if (c == sorted) {
			int k, cx = ui_seven ? (x0 + x1) / 2 : x1 - 12, cy = ui_seven ? y + 1 : y + 8;

			GrSetGCForeground(ui_gc, ui_seven ? MWRGB(151, 165, 176) : BLACK);
			for (k = 0; k < 4; k++)
				GrLine(window, ui_gc, cx - (down ? 3 - k : k), cy + k, cx + (down ? 3 - k : k), cy + k);
		}
	}
}

/* A list's rows, a column each at x; a row is drawn again only when it reads otherwise. */
static void
draw_rows(const struct ui_list *l, int columns, const int *x, void (*row)(int n, char text[COLUMNS][CELL]), int full)
{
	static char cache[VISIBLE][COLUMNS][CELL];
	static int cache_state[VISIBLE], cache_top = -1, cache_count = -1;
	int rows = ui_list_rows(l), bar = l->count > rows ? UI_SCROLL_W : 0, i, c;
	char text[COLUMNS][CELL];

	rows = rows > VISIBLE ? VISIBLE : rows;
	if (full || (cache_count > rows) != (l->count > rows)) {
		full = 1;
		ui_fill(window, l->x, l->y, l->width, l->height, WHITE);
		ui_bevel(window, l->x, l->y, l->width, l->height, 1);
		memset(cache_state, 0, sizeof cache_state);
	}
	for (i = 0; i < rows; i++) {
		int n = l->top + i, y = l->y + 2 + i * UI_ROW, state = n >= l->count ? 1 : n == l->selected ? 3 : 2, same;

		memset(text, 0, sizeof text);
		if (n < l->count)
			row(n, text);
		same = cache_state[i] == state;
		for (c = 0; c < columns && same; c++)
			same = !strcmp(cache[i][c], text[c]);
		if (same)
			continue;
		ui_fill(window, l->x + 2, y, l->width - 4 - bar, UI_ROW, state == 3 ? UI_SELECTED : WHITE);
		for (c = 0; c < columns && state != 1; c++) {
			int w = (c + 1 < columns ? x[c + 1] : l->width - bar) - x[c] - 6;

			ui_text_fit(window, l->x + x[c], y + 3, w, text[c], state == 3 ? WHITE : BLACK);
		}
		memcpy(cache[i], text, sizeof text);
		cache_state[i] = state;
	}
	if (bar && (full || cache_top != l->top || cache_count != l->count))
		ui_scrollbar(window, l->x + l->width - 2 - UI_SCROLL_W, l->y + 2, l->height - 4, l->count, rows, l->top);
	cache_top = l->top;
	cache_count = l->count;
}

/* ---- Applications ---- */

static const int task_x[2] = { 6, 460 };
static const char *const task_names[2] = { "Task", "Status" };
#define TASK_BUTTON_W 92

static void
task_row(int n, char text[COLUMNS][CELL])
{
	snprintf(text[0], CELL, "%s", tasks[n].title);
	snprintf(text[1], CELL, "%s", tasks[n].hidden ? "Minimised" : "Running");
}

static void
draw_applications(int full)
{
	static const char *const labels[3] = { "End Task", "Switch To", "New Task..." };
	int i;

	if (full) {
		draw_head(&task_list, 2, task_x, task_names, -1, 0);
		for (i = 0; i < 3; i++)
			ui_button(window, WIDTH - 12 - (3 - i) * (TASK_BUTTON_W + 6) + 6, BUTTON_Y, TASK_BUTTON_W, BUTTON_H, labels[i], 0);
	}
	draw_rows(&task_list, 2, task_x, task_row, full);
}

/* ---- Processes ---- */

static const int process_x[COLUMNS] = { 6, 160, 216, 266, 410, 496 };
static const char *const process_names[COLUMNS] = { "Image Name", "PID", "CPU", "Workers (busy)", "GPU", "Memory" };
#define KERNEL_BUTTON_W 150
#define END_W 100

static void
process_row(int n, char text[COLUMNS][CELL])
{
	const struct program *p = &programs[listed[n]];
	char m[16];

	snprintf(text[BY_NAME], CELL, "%s", p->name);
	snprintf(text[BY_PID], CELL, "%d", p->pid);
	snprintf(text[BY_CPU], CELL, "%02d", p->share);
	if (p->workers)
		snprintf(text[BY_WORKERS], CELL, "%d (%u%%, %s M/s)", p->workers,
			 capacity ? (unsigned)((unsigned long long)p->worker_rate * 100 / ((unsigned long long)capacity * (unsigned)p->workers)) : 0,
			 millions(m, p->worker_rate));
	else
		snprintf(text[BY_WORKERS], CELL, "-");
	if (p->lists)
		snprintf(text[BY_GPU], CELL, "%u lists/s%s", p->lists_rate, p->draws ? " *" : "");
	else
		snprintf(text[BY_GPU], CELL, "-");
	if (p->kernel)
		snprintf(text[BY_MEMORY], CELL, "-");
	else
		snprintf(text[BY_MEMORY], CELL, "%ld K", p->memory);
}

static void
draw_processes(int full)
{
	if (full) {
		draw_head(&list, COLUMNS, process_x, process_names, sort_by, sort_down);
		ui_button(window, LIST_X, BUTTON_Y, KERNEL_BUTTON_W, BUTTON_H, "Show kernel threads", show_kernel);
		ui_button(window, WIDTH - 12 - END_W, BUTTON_Y, END_W, BUTTON_H, "End Process", 0);
	}
	draw_rows(&list, COLUMNS, process_x, process_row, full);
}

/* Program a against b in the chosen order. */
static int
compare(const struct program *a, const struct program *b)
{
	long x = 0, y = 0;

	switch (sort_by) {
	case BY_NAME: return strcmp(a->name, b->name);
	case BY_PID: x = a->pid, y = b->pid; break;
	case BY_CPU: x = a->share, y = b->share; break;
	case BY_WORKERS: x = (long)a->workers * 1000000 + a->worker_rate / 1000, y = (long)b->workers * 1000000 + b->worker_rate / 1000; break;
	case BY_GPU: x = (long)a->lists_rate * 2 + (a->lists != 0), y = (long)b->lists_rate * 2 + (b->lists != 0); break;
	case BY_MEMORY: x = a->memory, y = b->memory; break;
	}
	return x < y ? -1 : x > y;
}

static int
before(const struct program *a, const struct program *b)
{
	int c = compare(a, b);

	if (sort_down)
		c = -c;
	return c ? c < 0 : a->pid < b->pid;
}

/* The processes in the list's order, the chosen one still chosen if it is still there. */
static void
list_programs(void)
{
	int i, k;

	list.count = 0;
	for (i = 0; i < program_count; i++) {
		if (programs[i].kernel && !show_kernel)
			continue;
		for (k = list.count++; k > 0 && before(&programs[i], &programs[listed[k - 1]]); k--)
			listed[k] = listed[k - 1];
		listed[k] = i;
	}
	list.selected = -1;
	for (k = 0; k < list.count; k++)
		if (programs[listed[k]].pid == chosen_pid)
			list.selected = k;
	if (list.selected < 0)
		chosen_pid = 0;
	if (list.top > list.count - ui_list_rows(&list))
		list.top = list.count > ui_list_rows(&list) ? list.count - ui_list_rows(&list) : 0;
}

/* ---- Performance ---- */

#define INFO_ROWS 6
static const char *const info_titles[3] = { "Physical Memory (MB)", "System", "Cores and GPU" };
static const char *const info_names[3][INFO_ROWS] = {
	{ "Total", "In use", "Available", "Files read", "Files written", "Largest free" },
	{ "Processes", "Up time", "Load", "Task switches", "Interrupts", "Busy, kernel" },
	{ "Core 0", "All cores", "Workers busy", "Asleep, idle", "Parked", "GPU lists" },
};
static const int info_x[4] = { 12, 204, 396, WIDTH - 12 };
static int scale_x[2];	/* where the graphs' legends end: their scales go right of them */

static void
draw_performance(void)
{
	int i, k, x;

	group(12, CPU_BOX_Y, METER_BOX_W, CPU_BOX_H, "CPU Usage");
	group(HIST_X, CPU_BOX_Y, HIST_W, CPU_BOX_H, "CPU Usage History");
	group(12, MEM_BOX_Y, METER_BOX_W, MEM_BOX_H, "Memory");
	group(HIST_X, MEM_BOX_Y, HIST_W, MEM_BOX_H, "Physical Memory Usage History");
	x = HIST_X + 16 + text_width("CPU Usage History");
	x = legend(x, CPU_BOX_Y, CORE0_COLOUR, "core 0");
	scale_x[0] = legend(x, CPU_BOX_Y, WORKER_COLOUR, per_core ? "workers, a cell a core" : "workers");
	x = HIST_X + 16 + text_width("Physical Memory Usage History");
	x = legend(x, MEM_BOX_Y, MEMORY_COLOUR, "in use");
	scale_x[1] = legend(x, MEM_BOX_Y, FILES_COLOUR, "files");
	/* black where the workers shade, and under the meters for their numbers */
	ui_fill(window, METER_X, CPU_PLOT_Y, METER_W, CPU_METER_H + 18, BLACK);
	ui_fill(window, METER_X, MEM_PLOT_Y, METER_W, MEM_METER_H + 18, BLACK);
	ui_fill(window, PLOT_X, CPU_PLOT_Y, CPU_PLOT_W, CPU_PLOT_H, BLACK);
	ui_fill(window, PLOT_X, MEM_PLOT_Y, CPU_PLOT_W, MEM_PLOT_H, BLACK);
	ui_bevel(window, PLOT_X - 1, CPU_PLOT_Y - 1, CPU_PLOT_W + 2, CPU_PLOT_H + 2, 1);
	ui_bevel(window, PLOT_X - 1, MEM_PLOT_Y - 1, CPU_PLOT_W + 2, MEM_PLOT_H + 2, 1);
	for (i = 0; i < 3; i++) {
		group(info_x[i], INFO_Y, info_x[i + 1] - info_x[i] - 6, INFO_H, info_titles[i]);
		for (k = 0; k < INFO_ROWS; k++)
			ui_text(window, info_x[i] + 10, INFO_Y + 18 + k * 15, info_names[i][k], -1, BLACK, 0);
	}
}

/* The numbers of the Performance tab as they are now. */
static void
performance_values(void)
{
	struct sysinfo info;
	char v[3][INFO_ROWS][40], a[16];
	unsigned all = speed_now + workers_rate, parked = core_count - 1 - cores_working - cores_asleep - cores_idle;
	int i, k, x;

	sysinfo(&info);
	snprintf(v[0][0], 40, "%ld", memory_total / 1024);
	snprintf(v[0][1], 40, "%ld", memory_used / 1024);
	snprintf(v[0][2], 40, "%ld", (memory_total - memory_used) / 1024);
	snprintf(v[0][3], 40, "%ld", (memory_cached - memory_files) / 1024);
	snprintf(v[0][4], 40, "%ld.%ld", memory_files / 1024, memory_files % 1024 * 10 / 1024);
	snprintf(v[0][5], 40, "%ld KB", memory_free_block);
	snprintf(v[1][0], 40, "%d", user_processes);
	snprintf(v[1][1], 40, "%ld:%02ld:%02ld", (long)info.uptime / 3600, (long)info.uptime / 60 % 60, (long)info.uptime % 60);
	snprintf(v[1][2], 40, "%lu.%02lu  %lu.%02lu", info.loads[0] >> 16, (info.loads[0] & 0xffff) * 100 >> 16,
		 info.loads[1] >> 16, (info.loads[1] & 0xffff) * 100 >> 16);
	snprintf(v[1][3], 40, "%ld/s", switches);
	snprintf(v[1][4], 40, "%ld/s", interrupts);
	snprintf(v[1][5], 40, "%u%%, %u%%", busy, kernel_busy);
	snprintf(v[2][0], 40, "%s M/s", millions(a, speed_now));
	snprintf(v[2][1], 40, "%s M/s", millions(a, all));
	snprintf(v[2][2], 40, "%u of %u", cores_working, core_count - 1);
	snprintf(v[2][3], 40, "%u, %u", cores_asleep, cores_idle);
	snprintf(v[2][4], 40, "%u", parked);
	snprintf(v[2][5], 40, "%u/s", lists_rate);
	for (i = 0; i < 3; i++) {
		int left = 0, right = info_x[i + 1] - 14;	/* the values start past the box's widest name */

		for (k = 0; k < INFO_ROWS; k++)
			if (text_width(info_names[i][k]) > left)
				left = text_width(info_names[i][k]);
		left += info_x[i] + 10 + 6;
		for (k = 0; k < INFO_ROWS; k++)
			text_at(10 + i * INFO_ROWS + k, left, INFO_Y + 18 + k * 15, right - left, v[i][k], BLACK, UI_FACE, 1);
	}
	snprintf(v[0][0], 40, "%u %%", busy > 100 ? 100 : busy);
	text_at(30, METER_X, CPU_PLOT_Y + CPU_METER_H + 1, METER_W, v[0][0], LCD_TEXT, BLACK, 2);
	snprintf(v[0][0], 40, "%ld MB", memory_used / 1024);
	text_at(31, METER_X, MEM_PLOT_Y + MEM_METER_H + 1, METER_W, v[0][0], LCD_TEXT, BLACK, 2);
	if (per_core)
		snprintf(v[0][0], 40, "%s M/s a core", millions(a, capacity));
	else
		snprintf(v[0][0], 40, "%s M/s", millions(a, plots[CPU].top));
	x = scale_x[0] > HIST_X + HIST_W - 140 ? scale_x[0] : HIST_X + HIST_W - 140;
	text_at(32, x, CPU_BOX_Y, HIST_X + HIST_W - 8 - x, v[0][0], UI_SHADOW, UI_FACE, 1);
	snprintf(v[0][0], 40, "%ld MB", memory_total / 1024);
	x = scale_x[1] > HIST_X + HIST_W - 140 ? scale_x[1] : HIST_X + HIST_W - 140;
	text_at(33, x, MEM_BOX_Y, HIST_X + HIST_W - 8 - x, v[0][0], UI_SHADOW, UI_FACE, 1);
	if (print && core_count > 1)	/* (for a test: the workers' line) */
		printf("%u working, %u asleep, %u idle, %u parked; a core's most %u a second\n", cores_working, cores_asleep,
		       cores_idle, parked, capacity);
}

/* ---- Networking ---- */

static const int net_x[COLUMNS] = { 6, 76, 176, 286, 386, 476 };
static const char *const net_names[COLUMNS] = { "Adapter", "State", "Address", "Received", "Sent", "Packets" };

static void
net_row(int n, char text[COLUMNS][CELL])
{
	char a[16];

	snprintf(text[0], CELL, "%s", nets[n].name);
	snprintf(text[1], CELL, "%s", nets[n].up ? "Connected" : "Not connected");
	snprintf(text[2], CELL, "%s", nets[n].address);
	snprintf(text[3], CELL, "%s/s", bytes(a, nets[n].rx_rate));
	snprintf(text[4], CELL, "%s/s", bytes(a, nets[n].tx_rate));
	snprintf(text[5], CELL, "%lu / %lu", nets[n].rx_packets, nets[n].tx_packets);
}

static void
draw_networking(int full)
{
	char title[40], a[16];
	int x;

	if (full) {
		snprintf(title, sizeof title, "Network: %s", net_count ? nets[0].name : "none");
		group(12, NET_BOX_Y, WIDTH - 24, NET_BOX_H, title);
		x = 12 + 16 + text_width(title);
		x = legend(x, NET_BOX_Y, RECEIVED_COLOUR, "received");
		legend(x, NET_BOX_Y, SENT_COLOUR, "sent");
		ui_fill(window, NET_PLOT_X, NET_PLOT_Y, NET_PLOT_W, NET_PLOT_H, BLACK);
		ui_bevel(window, NET_PLOT_X - 1, NET_PLOT_Y - 1, NET_PLOT_W + 2, NET_PLOT_H + 2, 1);
		draw_head(&net_list, COLUMNS, net_x, net_names, -1, 0);
	}
	net_list.count = net_count;
	snprintf(title, sizeof title, "%s/s", bytes(a, plots[NET].top));
	text_at(34, WIDTH - 12 - 140, NET_BOX_Y, 128, title, UI_SHADOW, UI_FACE, 1);
	draw_rows(&net_list, COLUMNS, net_x, net_row, full);
}

/* The tab's own part again: `full` when the window is drawn whole. */
static void
draw_page(int full)
{
	switch (tab) {
	case APPLICATIONS: draw_applications(full); break;
	case PROCESSES: draw_processes(full); break;
	case PERFORMANCE:
		if (full)
			draw_performance();
		performance_values();
		break;
	case NETWORKING: draw_networking(full); break;
	}
	draw_status(full);
}

static void
draw(void)
{
	finish_frame();
	forget_texts();
	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	draw_menu_bar();
	draw_tabs();
	set_regions();
	draw_page(1);
	GrFlush();
	locate();
	whole = 1;
	need_frame = region_count > 0;
}

/* ---- a sample ---- */

static void
sample(long ms)
{
	static unsigned lists_before, copies_before, passes_before, count_then;
	unsigned count_now = instructions(), now = ms_now(), passes, top, c;
	int i, k;

	if (ms <= 0)
		ms = 1;
	for (i = 0; i < PLOTS; i++) {
		memmove(plots[i].low, plots[i].low + 1, (HISTORY - 1) * sizeof plots[i].low[0]);
		memmove(plots[i].high, plots[i].high + 1, (HISTORY - 1) * sizeof plots[i].high[0]);
	}
	speed_now = count_then ? (unsigned)((unsigned long long)(count_now - count_then) * 1000 / (unsigned long long)ms) : 0;
	count_then = count_now;
	workers_rate = sample_cores(ms);
	sample_linux(ms);
	sample_memory(tab == PERFORMANCE);
	if (has_network && tab == NETWORKING)
		sample_net(ms, 1);
	/* a core's most: 8,192 instructions a pass the machine made; without that word, core 0's
	   speed over how busy Linux kept it */
	passes = core_words ? core_words[0x32c / 4] : 0;
	if (passes && passes_before && passes != passes_before)
		capacity = (unsigned)((unsigned long long)(passes - passes_before) * 8192u * 1000u / (unsigned)ms);
	else if (busy >= 20) {
		unsigned estimate = (unsigned)((unsigned long long)speed_now * 100 / busy);

		capacity = capacity ? (capacity * 3 + estimate) / 4 : estimate;
	}
	passes_before = passes;
	if (capacity < speed_now)
		capacity = speed_now;
	if (capacity < core_busiest)
		capacity = core_busiest;
	plots[CPU].low[HISTORY - 1] = speed_now;
	plots[CPU].high[HISTORY - 1] = workers_rate;
	if (core_words) {
		unsigned lists = core_words[0x1c / 4], copies = core_words[0x38 / 4];

		lists_rate = lists_before ? (unsigned)((lists - lists_before) * 1000u / (unsigned)ms) : 0;
		copies_rate = copies_before ? (unsigned)((copies - copies_before) * 1000u / (unsigned)ms) : 0;
		lists_before = lists, copies_before = copies;
	}
	/* a graph is as tall as its busiest sample needs, in doublings */
	for (i = 0; i < PLOTS; i++) {
		if (i == MEMORY)
			continue;
		for (top = i == CPU ? 1000000 : 1024, k = 0; k < HISTORY; k++)
			while (plots[i].low[k] + plots[i].high[k] > top && top < 0x80000000u)
				top *= 2;
		rescaled[i] |= top != plots[i].top;
		plots[i].top = top;
	}
	/* every core's load: core 0's is Linux's */
	for (c = 0; c < MAX_CORES; c++) {
		unsigned load = c == 0 ? busy : capacity ? (unsigned)((unsigned long long)core_rate[c] * 100 / capacity) : 0;

		memmove(core_load[c], core_load[c] + 1, CELL_SAMPLES - 1);
		core_load[c][CELL_SAMPLES - 1] = (unsigned char)(load > 100 ? 100 : load);
	}
	meter[0] = busy > 100 ? 100 : busy;
	samples_taken++;
	/* the processes are the dearest to read: the list at most every other second, the count
	   every fourth sample */
	if (tab == PROCESSES && !hidden ? now - programs_at >= 1900 : samples_taken % 4 == 0) {
		sample_programs(tab == PROCESSES && !hidden);
		if (tab == PROCESSES)
			list_programs();
	}
	/* windows come and go by the window system's word (CHLD_UPDATE); titles are asked anew */
	if (tab == APPLICATIONS && !hidden && ++windows_looks % 5 == 0)
		find_windows(1);
	if (!hidden)
		draw_page(0);
	if (!workers)
		take_workers();
	if (print && workers)
		printf("nxmon: %u frames of the graphs from %d workers\n", frames_shaded, workers);
	frames_shaded = 0;
	need_frame = region_count > 0 && !hidden;
}

/* ---- what the person does ---- */

static void
while_asking(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw();
}

static int
ask(const char *icon, const char *text, const char *yes, const char *no)
{
	const char *answers[2] = { yes, no };

	finish_frame();
	ui_ask_icon = icon;
	return ui_ask("Monitor", text, answers, no ? 2 : 1, while_asking);
}

static int
desktop_part(const char *name)
{
	return !strcmp(name, "nano-X") || !strcmp(name, "nxbar") || !strcmp(name, "emuinit");
}

/* Whether pid started `ancestor`'s line: the parent of its parent ... is it. */
static int
descends(int pid, int ancestor)
{
	int depth;

	for (depth = 0; pid > 1 && depth < 32; depth++) {
		const struct program *p = program_of(pid);

		if (!p)
			return 0;
		if (p->ppid == ancestor)
			return 1;
		pid = p->ppid;
	}
	return 0;
}

/* End Process, asked first; with `tree` every process it started too. Asked to go
 * (SIGTERM), and made to (SIGKILL) if it is still there two seconds later. */
static void
end_process(int tree)
{
	const struct program *p = list.selected >= 0 ? &programs[listed[list.selected]] : NULL;
	char text[300];
	int pid, i;

	if (!p) {
		ask("information", "Choose a process in the list first.", "OK", NULL);
		return;
	}
	pid = p->pid;
	if (pid == 1 || p->kernel) {
		ask("error", "That process cannot be ended:\nthe machine is made of it.", "OK", NULL);
		return;
	}
	snprintf(text, sizeof text, "Do you want to end %s%s (process %d)?\n%s", tree ? "the process tree of " : "", p->name, pid,
		 desktop_part(p->name) ? "It is part of the desktop itself: ending it closes\nthe Start menu and bar, or every open program." :
		 "If it has open windows, what is not saved in them is lost.");
	if (ask("warning", text, tree ? "End process tree" : "End process", "Cancel") != 0)
		return;
	if (tree)
		for (i = 0; i < program_count; i++)
			if (descends(programs[i].pid, pid))
				kill(programs[i].pid, SIGTERM);
	if (kill(pid, SIGTERM) != 0) {
		ask("error", "The process could not be ended.", "OK", NULL);
		return;
	}
	ending_pid = pid;
	ending_at = ms_now();
	snprintf(ending_name, sizeof ending_name, "%s", p->name);
}

static void
set_priority(int nice_value)
{
	const struct program *p = list.selected >= 0 ? &programs[listed[list.selected]] : NULL;

	if (p && setpriority(PRIO_PROCESS, (id_t)p->pid, nice_value) != 0)
		ask("error", "The priority could not be changed.", "OK", NULL);
}

static struct task *
chosen_task(void)
{
	return task_list.selected >= 0 && task_list.selected < task_count ? &tasks[task_list.selected] : NULL;
}

static void
switch_to(struct task *t)
{
	if (!t)
		return;
	if (t->hidden)
		GrMapWindow(t->frame);
	GrRaiseWindow(t->frame);
	GrSetFocus(t->client);
	windows_dirty = 1;
}

/* Whether a command's program is there to start: a path, or a name on PATH. */
static int
findable(const char *line)
{
	char name[128], path[256];
	const char *dirs = getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin:/sbin:/usr/sbin";
	int n = 0;

	while (*line == ' ')
		line++;
	while (line[n] && line[n] != ' ' && n < (int)sizeof name - 1) {
		name[n] = line[n];
		n++;
	}
	name[n] = 0;
	if (!n)
		return 0;
	if (strchr(name, '/'))
		return access(name, X_OK) == 0;
	while (*dirs) {
		int k = 0;

		while (dirs[k] && dirs[k] != ':')
			k++;
		snprintf(path, sizeof path, "%.*s/%s", k, dirs, name);
		if (access(path, X_OK) == 0)
			return 1;
		dirs += dirs[k] ? k + 1 : k;
	}
	return 0;
}

static void
start_task(const char *line)
{
	char text[200];
	pid_t child;

	if (!findable(line)) {
		snprintf(text, sizeof text, "The Monitor cannot find '%.60s'.\nMake sure the name is right, and try again.", line);
		ask("error", text, "OK", NULL);
		return;
	}
	/* (vfork: the child does nothing in our memory but become the shell) */
	child = real_vfork();
	if (child == 0) {
		signal(SIGCHLD, SIG_DFL);
		execl("/bin/sh", "sh", "-c", line, (char *)NULL);
		_exit(127);
	}
}

static GR_WINDOW_ID
event_window(const GR_EVENT *e)
{
	return e->type == GR_EVENT_TYPE_EXPOSURE ? e->exposure.wid : e->type == GR_EVENT_TYPE_BUTTON_DOWN ? e->button.wid :
		e->type == GR_EVENT_TYPE_KEY_DOWN ? e->keystroke.wid : e->type == GR_EVENT_TYPE_CLOSE_REQ ? e->general.wid : 0;
}

#define NEW_W 380
#define NEW_H 132

static void
draw_new_task(GR_WINDOW_ID d, const char *line)
{
	char shown_line[140];

	ui_fill(d, 0, 0, NEW_W, NEW_H, UI_FACE);
	if (ui_seven)
		ui_fill(d, 0, 0, NEW_W, NEW_H - 44, WHITE);
	ui_text(d, 14, 12, "Type the name of a program, and the Monitor", -1, BLACK, 0);
	ui_text(d, 14, 28, "will start it for you.", -1, BLACK, 0);
	ui_text(d, 14, 56, "Open:", -1, BLACK, 0);
	ui_fill(d, 60, 52, NEW_W - 74, 22, WHITE);
	ui_bevel(d, 60, 52, NEW_W - 74, 22, 1);
	snprintf(shown_line, sizeof shown_line, "%s|", line);
	ui_text_fit(d, 64, 56, NEW_W - 82, shown_line, BLACK);
	ui_button(d, NEW_W - 180, NEW_H - 34, 80, BUTTON_H, "OK", 0);
	ui_button(d, NEW_W - 92, NEW_H - 34, 80, BUTTON_H, "Cancel", 0);
}

/* File, New Task: a line to type, run by the shell. */
static void
new_task(void)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	char line[128] = "";
	int n = 0, done = 0, run = 0;
	GR_WINDOW_ID d;
	GR_EVENT e;

	finish_frame();
	d = GrNewWindowEx(UI_FIXED, "Create New Task", GR_ROOT_WINDOW_ID, (screen->cols - NEW_W) / 2, (screen->rows - NEW_H) / 3,
			  NEW_W, NEW_H, UI_FACE);
	GrSelectEvents(d, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(d);
	GrSetFocus(d);
	while (!done) {
		GrGetNextEvent(&e);
		if (event_window(&e) != d) {
			while_asking(&e);
			continue;
		}
		switch (e.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_new_task(d, line);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			done = 1;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_inside(e.button.x, e.button.y, NEW_W - 180, NEW_H - 34, 80, BUTTON_H))
				done = run = 1;
			else if (ui_inside(e.button.x, e.button.y, NEW_W - 92, NEW_H - 34, 80, BUTTON_H))
				done = 1;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (e.keystroke.ch == MWKEY_ENTER || e.keystroke.ch == '\r' || e.keystroke.ch == '\n')
				done = run = 1;
			else if (e.keystroke.ch == MWKEY_ESCAPE)
				done = 1;
			else if (e.keystroke.ch == MWKEY_BACKSPACE && n > 0)
				line[--n] = 0;
			else if (e.keystroke.ch >= ' ' && e.keystroke.ch < 127 && n < (int)sizeof line - 1)
				line[n++] = (char)e.keystroke.ch, line[n] = 0;
			else
				break;
			if (!done)
				draw_new_task(d, line);
			break;
		}
	}
	GrDestroyWindow(d);
	GrSetFocus(window);
	if (run && line[0])
		start_task(line);
}

/* The menus: built when one opens, its ticks as things are then. */
static struct ui_menu_item file_items[2], view_items[4], speed_items[4], history_items[2], help_items[1];
static struct ui_menu_item task_items[5], process_items[3], priority_items[5];
static const struct { struct ui_menu_item *items; int count; } menus[3] = { { file_items, 2 }, { view_items, 4 }, { help_items, 1 } };

static void
item(struct ui_menu_item *it, const char *label, int id, int flags)
{
	memset(it, 0, sizeof *it);
	it->label = label, it->icon = -1, it->id = id, it->flags = flags;
}

static void
make_menus(void)
{
	static const char *const speeds[4] = { "High", "Normal", "Low", "Paused" };
	static const char *const priorities[5] = { "High", "Above Normal", "Normal", "Below Normal", "Low" };
	static const int nices[5] = { -10, -5, 0, 5, 19 };
	const struct program *p = list.selected >= 0 ? &programs[listed[list.selected]] : NULL;
	const struct task *t = chosen_task();
	int i;

	item(&file_items[0], "New Task (Run...)", C_NEW, 0);
	item(&file_items[1], "Exit Monitor", C_EXIT, UI_MENU_LINE);
	for (i = 0; i < 4; i++)
		item(&speed_items[i], speeds[i], C_HIGH + i, speed == i ? UI_MENU_CHECK : 0);
	item(&history_items[0], "One Graph, All Cores", C_ONE_GRAPH, per_core ? 0 : UI_MENU_CHECK);
	item(&history_items[1], "A Cell Per Core", C_PER_CORE, per_core ? UI_MENU_CHECK : 0);
	item(&view_items[0], "Refresh Now   F5", C_REFRESH, 0);
	item(&view_items[1], "Update Speed", 0, 0);
	view_items[1].sub = speed_items, view_items[1].sub_count = 4;
	item(&view_items[2], "CPU History", 0, 0);
	view_items[2].sub = history_items, view_items[2].sub_count = 2;
	item(&view_items[3], "Show Kernel Threads", C_KERNEL, UI_MENU_LINE | (show_kernel ? UI_MENU_CHECK : 0));
	item(&help_items[0], "About Monitor", C_ABOUT, 0);
	item(&task_items[0], "Switch To", C_SWITCH, 0);
	item(&task_items[1], "Bring To Front", C_FRONT, 0);
	item(&task_items[2], "Minimise", C_MINIMISE, t && t->hidden ? UI_MENU_GREY : 0);
	item(&task_items[3], "End Task", C_END_TASK, UI_MENU_LINE);
	item(&task_items[4], "Go To Process", C_GO_PROCESS, UI_MENU_LINE);
	for (i = 0; i < 5; i++)
		item(&priority_items[i], priorities[i], C_PRI_HIGH + i, p && p->nice == nices[i] ? UI_MENU_CHECK : 0);
	item(&process_items[0], "End Process", C_END_PROCESS, 0);
	item(&process_items[1], "End Process Tree", C_END_TREE, 0);
	item(&process_items[2], "Set Priority", 0, UI_MENU_LINE);
	process_items[2].sub = priority_items, process_items[2].sub_count = 5;
}

static void
open_menu(int which, int keyboard)
{
	int wx, wy;

	finish_frame();
	make_menus();
	ui_window_origin(window, &wx, &wy);
	ui_menu_open(menus[which].items, menus[which].count, wx + menu_x[which], wy + MENU_H, 0, keyboard);
}

static void
remember(void)
{
	if (check)
		return;
	reg_set_int("nxmon.page", tab);
	reg_set_int("nxmon.cores", per_core);
	reg_set_int("nxmon.speed", speed);
	reg_set_int("nxmon.kernel", show_kernel);
}

static void
show_tab(int which)
{
	if (which == tab)
		return;
	finish_frame();
	tab = which;
	/* the window system tells of windows coming and going only while they are shown */
	GrSelectEvents(GR_ROOT_WINDOW_ID, tab == APPLICATIONS ? GR_EVENT_MASK_CHLD_UPDATE : 0);
	if (tab == PROCESSES) {
		sample_programs(1);
		list_programs();
	} else if (tab == APPLICATIONS)
		find_windows(1);
	else if (tab == NETWORKING) {
		/* (not read while not shown: its graph starts now) */
		memset(plots[NET].low, 0, sizeof plots[NET].low);
		memset(plots[NET].high, 0, sizeof plots[NET].high);
		sample_net(0, 1);
	}
	else if (tab == PERFORMANCE)
		memory_free_block = largest_free_block();
	remember();
	draw();
}

static unsigned due;		/* when the next sample is */
static int refresh;		/* one is taken at once */

/* A menu's item, or the same from a key or a button. */
static void
command(int id)
{
	struct task *t = chosen_task();

	switch (id) {
	case C_NEW: new_task(); break;
	case C_EXIT: GrClose(); exit(0);
	case C_REFRESH: refresh = 1; break;
	case C_HIGH: case C_NORMAL: case C_LOW: case C_PAUSED:
		speed = id - C_HIGH;
		due = ms_now() + (unsigned)speed_ms[speed];
		remember();
		break;
	case C_ONE_GRAPH: case C_PER_CORE:
		per_core = id == C_PER_CORE;
		remember();
		draw();
		break;
	case C_KERNEL:
		show_kernel = !show_kernel;
		remember();
		sample_programs(1);
		list_programs();
		draw();
		break;
	case C_ABOUT:
		ask("information", "Monitor, the machine's task manager.\nIts graphs are shaded by the machine's worker cores\n"
		    "straight into the window (docs/machine/multicore.md).", "OK", NULL);
		break;
	case C_SWITCH: case C_FRONT: switch_to(t); break;
	case C_MINIMISE:
		if (t)
			GrUnmapWindow(t->frame);
		windows_dirty = 1;
		break;
	case C_END_TASK:
		/* (asked to close, as its own close button does) */
		if (t)
			GrCloseWindow(t->client);
		windows_dirty = 1;
		break;
	case C_GO_PROCESS:
		if (t) {
			chosen_pid = t->pid;
			show_tab(PROCESSES);
			list_programs();
			ui_list_show(&list);
			draw_processes(1);
		}
		break;
	case C_END_PROCESS: end_process(0); break;
	case C_END_TREE: end_process(1); break;
	case C_PRI_HIGH: set_priority(-10); break;
	case C_PRI_ABOVE: set_priority(-5); break;
	case C_PRI_NORMAL: set_priority(0); break;
	case C_PRI_BELOW: set_priority(5); break;
	case C_PRI_LOW: set_priority(19); break;
	}
}

#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_UPDATE)

/* A letter typed in a list: the next row whose name starts with it. */
static int
type_ahead(struct ui_list *l, int ch, const char *(*name)(int n))
{
	int i;

	for (i = 1; i <= l->count; i++) {
		int n = (l->selected + i) % l->count;

		if (ui_starts(name(n), ch)) {
			l->selected = n;
			ui_list_show(l);
			return 1;
		}
	}
	return 0;
}

static const char *
process_name(int n)
{
	return programs[listed[n]].name;
}

static const char *
task_title(int n)
{
	return tasks[n].title;
}

/* An event in the window; true when the program is to close. */
static int
handle(GR_EVENT *event)
{
	int i, ch = event->type == GR_EVENT_TYPE_KEY_DOWN ? event->keystroke.ch : 0;

	switch (event->type) {
	case GR_EVENT_TYPE_CLOSE_REQ:
		return 1;
	case GR_EVENT_TYPE_EXPOSURE:
		if (event->exposure.wid == window) {
			hidden = 0;
			draw();
		}
		return 0;
	case GR_EVENT_TYPE_UPDATE:
		/* minimised (its frame unmapped), and shown again */
		if (event->update.wid == window && event->update.utype == GR_UPDATE_UNMAP)
			hidden = 1, need_frame = 0;
		else if (event->update.wid == window && event->update.utype == GR_UPDATE_MAP)
			hidden = 0;
		return 0;
	case GR_EVENT_TYPE_CHLD_UPDATE:
		if (event->update.utype != GR_UPDATE_MOVE && event->update.utype != GR_UPDATE_SIZE && event->update.utype != GR_UPDATE_UNMAPTEMP)
			windows_dirty = 1;
		return 0;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if (ui_wheel(event))
			break;
		if (event->button.y < MENU_H) {
			for (i = 0; i < 3; i++)
				if (event->button.x >= menu_x[i] && event->button.x < menu_x[i + 1])
					open_menu(i, 0);
			return 0;
		}
		if (event->button.y >= TAB_Y - 2 && event->button.y < PANEL_Y) {
			for (i = 0; i < tab_count(); i++)
				if (event->button.x >= tab_x[i] && event->button.x < tab_x[i + 1])
					show_tab(i);
			return 0;
		}
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		if (ch == MWKEY_ESCAPE)
			return 1;
		if (ch == MWKEY_TAB && (event->keystroke.modifiers & MWKMOD_CTRL)) {
			int n = tab_count();

			show_tab((tab + ((event->keystroke.modifiers & MWKMOD_SHIFT) ? n - 1 : 1)) % n);
			return 0;
		}
		if (ch == MWKEY_F5) {
			command(C_REFRESH);
			return 0;
		}
		if (ch == MWKEY_F10) {
			open_menu(0, 1);
			return 0;
		}
		break;
	}
	if (tab == APPLICATIONS) {
		int changed = 0, x = event->type == GR_EVENT_TYPE_BUTTON_DOWN ? event->button.x : 0, y = event->type == GR_EVENT_TYPE_BUTTON_DOWN ? event->button.y : 0;

		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(event))
			ui_moves(window, EVENTS, 1);
		else if (event->type == GR_EVENT_TYPE_MOUSE_MOTION && !event->mouse.buttons)
			ui_moves(window, EVENTS, 0);
		if (ch)
			changed = ui_list_key(&task_list, ch) || (ch > ' ' && ch < 127 && type_ahead(&task_list, ch, task_title));
		else
			changed = ui_list_event(&task_list, event);
		if (changed) {
			chosen_frame = chosen_task() ? chosen_task()->frame : 0;
			draw_applications(0);
		}
		if (changed == 2 || ch == MWKEY_ENTER || ch == '\r')
			switch_to(chosen_task());
		if (ch == MWKEY_DELETE)
			command(C_END_TASK);
		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && (event->button.changebuttons & GR_BUTTON_R) &&
		    ui_inside(x, y, task_list.x, task_list.y, task_list.width, task_list.height) && chosen_task()) {
			make_menus();
			ui_menu_open(task_items, 5, event->button.rootx, event->button.rooty, 0, 0);
		}
		for (i = 0; event->type == GR_EVENT_TYPE_BUTTON_DOWN && i < 3; i++)
			if (ui_inside(x, y, WIDTH - 12 - (3 - i) * (TASK_BUTTON_W + 6) + 6, BUTTON_Y, TASK_BUTTON_W, BUTTON_H))
				command(i == 0 ? C_END_TASK : i == 1 ? C_SWITCH : C_NEW);
	} else if (tab == PROCESSES) {
		int changed = 0, x = event->type == GR_EVENT_TYPE_BUTTON_DOWN ? event->button.x : 0, y = event->type == GR_EVENT_TYPE_BUTTON_DOWN ? event->button.y : 0;

		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && y >= LIST_Y - HEAD_H && y < LIST_Y) {
			for (i = COLUMNS - 1; i >= 0 && x < LIST_X + process_x[i] - 6; i--)
				;
			if (i >= 0) {
				sort_down = i == sort_by ? !sort_down : i != BY_NAME && i != BY_PID;
				sort_by = i;
				list_programs();
				draw_processes(1);
			}
			return 0;
		}
		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(event))
			ui_moves(window, EVENTS, 1);
		else if (event->type == GR_EVENT_TYPE_MOUSE_MOTION && !event->mouse.buttons)
			ui_moves(window, EVENTS, 0);
		if (ch)
			changed = ui_list_key(&list, ch) || (ch > ' ' && ch < 127 && type_ahead(&list, ch, process_name));
		else
			changed = ui_list_event(&list, event);
		if (changed) {
			chosen_pid = list.selected >= 0 ? programs[listed[list.selected]].pid : 0;
			draw_processes(0);
		}
		if (ch == MWKEY_DELETE)
			end_process(0);
		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && (event->button.changebuttons & GR_BUTTON_R) &&
		    ui_inside(x, y, list.x, list.y, list.width, list.height) && list.selected >= 0) {
			make_menus();
			ui_menu_open(process_items, 3, event->button.rootx, event->button.rooty, 0, 0);
		}
		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && ui_inside(x, y, WIDTH - 12 - END_W, BUTTON_Y, END_W, BUTTON_H))
			end_process(0);
		if (event->type == GR_EVENT_TYPE_BUTTON_DOWN && ui_inside(x, y, LIST_X, BUTTON_Y, KERNEL_BUTTON_W, BUTTON_H))
			command(C_KERNEL);
	}
	return 0;
}

/* The next event, or a timeout after `ms`. GrGetNextEventTimeout asks the window system anew
 * at every call: here it is asked once (GrPrepareSelect) and the wait is this program's, on
 * the socket that told. */
static GR_EVENT taken;
static int took;

static void
take(GR_EVENT *event)
{
	taken = *event;
	took = 1;
}

static void
next_event(GR_EVENT *event, int ms)
{
	static int asked, fd = -1;
	struct timeval limit;
	fd_set set;
	int most = 0;

	FD_ZERO(&set);
	took = 0;
	GrServiceSelect(&set, take);	/* one that came with an answer to something else */
	if (took)
		asked = 0;		/* (and was the answer to the asking, too) */
	if (!took && !asked) {
		GrPrepareSelect(&most, &set);
		for (fd = most; fd > 0 && !FD_ISSET(fd, &set); fd--)
			continue;
		asked = 1;
	}
	if (!took) {
		FD_ZERO(&set);
		FD_SET(fd, &set);
		limit.tv_sec = ms / 1000, limit.tv_usec = ms % 1000 * 1000;
		if (select(fd + 1, &set, NULL, NULL, &limit) > 0) {
			GrServiceSelect(&set, take);
			asked = 0;
		}
	}
	if (took)
		*event = taken;
	else
		event->type = GR_EVENT_TYPE_TIMEOUT;
}

/* Whether /proc/net/dev names an adapter besides the loopback. */
static int
network_there(void)
{
	return sample_net(0, 0);
}

int
main(void)
{
	GR_EVENT event;
	unsigned then;
	void *words = (gpu = open("/dev/gpu", O_RDWR)) < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000);

	if (words != MAP_FAILED)
		core_words = words;
	words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000 + 0x6ba000);
	if (words != MAP_FAILED)
		core_more = words;
	/* the workers' owner words (WORKERS_OWNER, 0x86c01400) */
	words = gpu < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x00c01000);
	if (words != MAP_FAILED)
		owners = words;
	if (GrOpen() < 0)
		return 1;
	ui_init();
	signal(SIGCHLD, SIG_IGN);	/* (what New Task started is nobody's to wait for) */
	{
		extern char __DATA_BEGIN__[], _end[];

		/* (NXMON_WORKERS=0: this core shades; NXMON_CHECK=1: it shades what the workers did again and
		 * compares, with the first view whatever the registry says) */
		no_workers = getenv("NXMON_WORKERS") && atoi(getenv("NXMON_WORKERS")) == 0;
		check = getenv("NXMON_CHECK") != NULL;
		print = getenv("NXMON_PRINT") != NULL;
		checked = check ? malloc(MAX_PLOT_PIXELS * sizeof checked[0]) : NULL;
		has_network = network_there();
		if (!check) {
			tab = reg_int("nxmon.page", PERFORMANCE);
			per_core = reg_int("nxmon.cores", 0) != 0;
			speed = reg_int("nxmon.speed", NORMAL);
			show_kernel = reg_int("nxmon.kernel", 0) != 0;
		}
		/* (NXMON_SHOW=applications, processes, cpu, cores, memory or networking: that view first) */
		if (getenv("NXMON_SHOW")) {
			const char *show = getenv("NXMON_SHOW");

			tab = !strcmp(show, "applications") ? APPLICATIONS : !strcmp(show, "processes") ? PROCESSES :
				!strcmp(show, "networking") ? NETWORKING : PERFORMANCE;
			per_core = !strcmp(show, "cores");
		}
		tab = tab < 0 || tab >= tab_count() ? PERFORMANCE : tab;
		speed = speed < 0 || speed > PAUSED ? NORMAL : speed;
		/* what a worker reads, there before it steps on it */
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
		atexit(mcw_close);
	}
	sample_cores(1000);
	sample_linux(1000);
	sample_memory(1);
	sample_programs(1);
	list_programs();
	window = GrNewWindowEx(UI_FIXED, "Monitor", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, EVENTS);
	if (tab == APPLICATIONS) {
		GrSelectEvents(GR_ROOT_WINDOW_ID, GR_EVENT_MASK_CHLD_UPDATE);
		find_windows(1);
	}
	GrMapWindow(window);
	then = ms_now();
	due = then + 1000;
	for (;;) {
		unsigned now = ms_now();
		int wait, id;

		wait = shading ? 15 : (need_frame && surface) || refresh ? 0 : speed == PAUSED ? 60000 : (int)(due - now) > 0 ? (int)(due - now) : 0;
		next_event(&event, wait);
		if (event.type != GR_EVENT_TYPE_TIMEOUT) {
			ui_origin_note(&event);
			finish_frame();
			switch (ui_menu_event(&event, &id)) {
			case UI_MENU_CHOSEN:
				GrSetFocus(window);
				command(id);
				continue;
			case UI_MENU_CLOSED:
				GrSetFocus(window);
				/* fall through */
			case UI_MENU_TOOK:
				continue;
			}
			if (handle(&event)) {
				GrClose();
				return 0;
			}
		}
		if (shading && frame_done()) {
			shading = 0;
			frames_shaded++;
			if (check)
				check_frame();
		}
		if (workers && mcw_asked())
			give_workers();
		now = ms_now();
		/* a process asked to end that is still there is made to */
		if (ending_pid && now - ending_at >= 2000) {
			struct program still = { .pid = ending_pid };

			if (read_stat(&still, now) && !strcmp(still.name, ending_name))
				kill(ending_pid, SIGKILL);
			ending_pid = 0;
		}
		/* windows that came, went or were named again, while they are shown */
		if (tab == APPLICATIONS && windows_dirty && !hidden) {
			find_windows(1);
			windows_dirty = 0;
			draw_applications(0);
		}
		if (!shading && (refresh || (speed != PAUSED && (int)(now - due) >= 0))) {
			sample((long)(now - then));
			then = now;
			due = now + (unsigned)speed_ms[speed];
			refresh = 0;
		}
		if (need_frame && !shading && surface && region_count) {
			GrFlush();
			settle();
			if (workers)
				post_frame();
			else
				shade_here();
			need_frame = 0;
		}
		GrFlush();
	}
}
