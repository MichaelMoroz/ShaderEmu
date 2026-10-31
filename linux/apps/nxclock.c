/*
 * nxclock: the clock, a tab each. Clock: an analogue face as large as the window. Digital: the
 * time in large figures and the date. Stopwatch: start, stop, laps. Timer: a countdown that
 * rings. Alarm: a time of day that rings. A ring is a notice and a beep through the sound card.
 * Settings are the registry's (nxclock.*).
 *
 *	nxclock [clock|digital|stopwatch|timer [SECONDS]|alarm [HH:MM]]
 *
 * It draws once a second, and the stopwatch ten times while it runs and is shown; the face is
 * drawn once a minute into a pixmap the GPU keeps, and a second is that copied and the hand.
 */
#include <sys/time.h>
#include <time.h>
#include "ui.h"
#include "ui_ask.h"
#include "shaderemu_sound.h"

#define TOP 36			/* where a tab's own part begins */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ)
#define BIG_FONT "/usr/share/font-ui-17.sef"	/* scaled for the large figures */
#define LAPS 99

enum { TAB_CLOCK, TAB_DIGITAL, TAB_WATCH, TAB_TIMER, TAB_ALARM, TABS };
static const char *tab_names[TABS] = { "Clock", "Digital", "Stopwatch", "Timer", "Alarm" };
static const char *tab_words[TABS] = { "clock", "digital", "stopwatch", "timer", "alarm" };

enum { B_24H, B_SECONDS, B_WATCH_GO, B_LAP, B_WATCH_RESET, B_TIMER_GO, B_TIMER_RESET, B_ALARM_ON,
	B_UP, B_DOWN = B_UP + 3, B_PRESET = B_DOWN + 3 };
static const int presets[4] = { 60, 300, 600, 1500 };

struct button { int x, y, w, h, id; char label[24]; };
static struct button buttons[16];
static int nbuttons, focus;	/* focus: 0 the row of tabs, then the buttons in order */

static GR_WINDOW_ID window;
static int width = 360, height = 380, tab, print;

/* settings (the registry's) */
static int h24 = 1, show_seconds = 1, alarm_on, alarm_hm = 7 * 60, timer_set = 300;

/* the stopwatch: time gathered before the last start, and when that was */
static int watch_running, nlaps;
static long long watch_base, watch_start, laps[LAPS];
static int laps_gone;		/* the oldest laps, gone from the list at LAPS */
static long long lap_before;	/* the time of the last of them */
static struct ui_list lap_list;

/* the timer: idle (showing timer_set), running to timer_end, or paused with timer_left */
static int timer_started, timer_running;
static long long timer_end, timer_left, timer_total;

static time_t snooze_until, rung_minute = -1;
static int ringing;
static long long ring_began;

/* what is on the screen now, to draw only what changed */
static time_t shown_sec;
static int shown_tenth = -1, shown_left = -1, shown_day = -1, base_min = -1;

/* the face: its centre and radius in the window, and the pixmap of it with the hour hands */
static int face_cx, face_cy, face_r;
static GR_WINDOW_ID face_pix;
static int face_pix_side;
static GR_FONT_ID num_font;
static int num_size, num_w[13], num_h;

/* the large figures: a font scaled to `height`, every digit in a cell of the widest one's width */
static struct { GR_FONT_ID font; int height, cell, w[128]; } big;
static int big_x, big_y;	/* where the figures of the tab shown begin */

static long long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct tm
local_now(time_t *at)
{
	struct tm tm;

	*at = time(NULL);
	if (!localtime_r(at, &tm))
		memset(&tm, 0, sizeof tm);
	return tm;
}

/* ---- settings ---- */

static void
read_settings(void)
{
	const char *alarm, *size;
	int h = 7, m = 0, w, ht;
	int i;

	h24 = reg_int("nxclock.24h", 1);
	show_seconds = reg_int("nxclock.seconds", 1);
	alarm_on = reg_int("nxclock.alarm_on", 0);
	timer_set = reg_int("nxclock.timer", 300);
	timer_set = timer_set < 1 || timer_set > 86399 ? 300 : timer_set;
	alarm = reg_get("nxclock.alarm", "07:00"), size = reg_get("nxclock.size", "360x380");	/* (after those: reg_get has four slots) */
	if (sscanf(alarm, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60)
		alarm_hm = h * 60 + m;
	if (sscanf(size, "%dx%d", &w, &ht) == 2 && w >= 200 && ht >= 200 && w <= 1920 && ht <= 1080)
		width = w, height = ht;
	for (i = 0; i < TABS; i++)
		if (!strcmp(reg_get("nxclock.tab", "clock"), tab_words[i]))
			tab = i;
}

static void
keep_alarm(void)
{
	char text[16];

	snprintf(text, sizeof text, "%02d:%02d", alarm_hm / 60, alarm_hm % 60);
	reg_set("nxclock.alarm", text);
	reg_set_int("nxclock.alarm_on", alarm_on);
}

/* ---- a beep through the sound card, looped until stopped ---- */

#define BEEP_RATE 22050
#define BEEP_N BEEP_RATE	/* a second: two short beeps and a rest */
static int beep_voice = -1;
static short *beep_mem;

static void
sound_start(void)
{
	uint32_t address;
	int i;

	if (beep_voice >= 0 || snd_open() != 0)
		return;
	if ((beep_voice = snd_claim()) < 0 || !(beep_mem = snd_memory(BEEP_N * 2, &address))) {
		beep_voice = -1;
		return;
	}
	for (i = 0; i < BEEP_N; i++) {
		int t = i % (BEEP_RATE * 15 / 100), on = i < BEEP_RATE * 30 / 100 && t < BEEP_RATE * 9 / 100;
		int phase = i * 880 % BEEP_RATE, up = phase * 4 / BEEP_RATE;	/* a triangle wave of 880 a second */
		int v = (up == 0 ? phase : up == 3 ? phase - BEEP_RATE : BEEP_RATE / 2 - phase) * 4;
		int edge = t < 200 ? t : BEEP_RATE * 9 / 100 - t < 200 ? BEEP_RATE * 9 / 100 - t : 200;

		beep_mem[i] = on ? (short)(v / 4 * edge / 200) : 0;
	}
	snd_play(beep_voice, SND_PCM16 | SND_LOOPED, address, BEEP_N, 0, snd_step(BEEP_RATE), SND_VOLUME(220, 220));
}

/* The card is let go: an open device keeps it on. */
static void
sound_stop(void)
{
	if (snd_fd < 0)
		return;
	if (beep_voice >= 0)
		snd_stop(beep_voice);
	if (beep_mem)
		munmap(beep_mem, BEEP_N * 2);
	munmap(snd_page, 4096);
	close(snd_fd);
	snd_fd = -1, snd_page = NULL, beep_mem = NULL, beep_voice = -1;
}

/* ---- the large figures ---- */

/* The font file nearest above `h` pixels: Liberation Sans goes to 32, Selawik to 17. */
static const char *
font_for(int h)
{
	static const int sizes[4] = { 20, 24, 28, 32 };
	static char path[40];
	int i;

	if (h > 17) {
		for (i = 0; i < 3 && sizes[i] < h; i++)
			continue;
		snprintf(path, sizeof path, "/usr/share/font-sans-%d.sef", sizes[i]);
		if (access(path, R_OK) == 0)
			return path;
	}
	return access(BIG_FONT, R_OK) == 0 ? BIG_FONT : UI_LOOK_FONT;
}

static void
big_set(int h)
{
	const char *chars = "0123456789:.";
	GR_SIZE w, hh, b;
	int i;

	h = h < 14 ? 14 : h > 200 ? 200 : h;
	if (big.font && big.height == h)
		return;
	if (big.font)
		GrDestroyFont(big.font);
	big.font = GrCreateFontEx(font_for(h), h, 0, NULL);
	big.height = h, big.cell = 1;
	GrSetGCFont(ui_gc, big.font);
	for (i = 0; chars[i]; i++) {
		GrGetGCTextSize(ui_gc, (void *)&chars[i], 1, GR_TFASCII, &w, &hh, &b);
		big.w[(int)chars[i]] = w;
		if (i < 10 && w > big.cell)
			big.cell = w;
	}
}

static int
big_width(const char *text)
{
	int w = 0;

	for (; *text; text++)
		w += isdigit((unsigned char)*text) ? big.cell : big.w[*text & 127];
	return w;
}

/* The figures at x, y over a cleared strip from `clear_x` `clear_w` wide (0: nothing cleared). */
static void
big_draw(int x, int y, const char *text, int clear_x, int clear_w)
{
	if (clear_w > 0)
		ui_fill(window, clear_x, y, clear_w, big.height + 2, UI_FACE);
	GrSetGCFont(ui_gc, big.font);
	GrSetGCForeground(ui_gc, ui_seven ? MWRGB(30, 30, 30) : BLACK);
	for (; *text; text++) {
		int digit = isdigit((unsigned char)*text), w = big.w[*text & 127];

		GrText(window, ui_gc, x + (digit ? (big.cell - w) / 2 : 0), y, (void *)text, 1, GR_TFASCII | GR_TFTOP);
		x += digit ? big.cell : w;
	}
}

/* Text centred on cx. */
static void
centred(int cx, int y, const char *text, GR_COLOR colour)
{
	ui_text(window, cx - ui_width(&ui_var_widths, ui_var, text, -1) / 2, y, text, -1, colour, 0);
}

/* ---- buttons ---- */

static void
add_button(int x, int y, int w, int h, int id, const char *label)
{
	struct button *b = &buttons[nbuttons];

	if (nbuttons == 16)
		return;
	b->x = x, b->y = y, b->w = w, b->h = h, b->id = id;
	snprintf(b->label, sizeof b->label, "%s", label);
	nbuttons++;
}

/* A row of buttons of one width, centred, at y. */
static void
button_row(int y, int w, int count, const int *ids, const char *const *labels)
{
	int x = (width - count * w - (count - 1) * 8) / 2, i;

	for (i = 0; i < count; i++)
		add_button(x + i * (w + 8), y, w, 24, ids[i], labels[i]);
}

static void
draw_button(int i)
{
	struct button *b = &buttons[i];
	int tw = ui_width(&ui_var_widths, ui_var, b->label, -1);

	ui_button(window, b->x, b->y, b->w, b->h, "", 0);
	ui_text(window, b->x + (b->w - tw) / 2, b->y + (b->h - 12) / 2, b->label, -1, BLACK, 0);
	if (focus == i + 1)
		ui_focus(window, b->x + 4, b->y + 4, b->w - 8, b->h - 8);
}

/* Tab i's place: each its label's width and an equal share of the room left over. */
static void
tab_place(int i, int *x, int *w)
{
	int lw[TABS], sum = 0, k, pad;

	for (k = 0; k < TABS; k++)
		sum += lw[k] = ui_width(&ui_var_widths, ui_var, tab_names[k], -1);
	pad = (width - 16 - 2 * (TABS - 1) - sum) / TABS;
	pad = pad < 6 ? 6 : pad;
	for (*x = 8, k = 0; k < i; k++)
		*x += lw[k] + pad + 2;
	*w = lw[i] + pad;
}

static void
draw_tabs(void)
{
	int i, x, tw;

	for (i = 0; i < TABS; i++) {
		int lw = ui_width(&ui_var_widths, ui_var, tab_names[i], -1);

		tab_place(i, &x, &tw);
		ui_button(window, x, 6, tw, 22, "", i == tab);
		ui_text(window, x + (tw - lw) / 2, 11, tab_names[i], -1, BLACK, 0);
		if (focus == 0 && i == tab)
			ui_focus(window, x + 4, 10, tw - 8, 14);
	}
}

/* ---- the analogue face ---- */

static const short sine[91] = {
	0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126,
	3406, 3686, 3964, 4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402,
	6664, 6924, 7182, 7438, 7692, 7943, 8192, 8438, 8682, 8923, 9162, 9397,
	9630, 9860, 10087, 10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982,
	12176, 12365, 12551, 12733, 12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044,
	14189, 14330, 14466, 14598, 14726, 14849, 14968, 15082, 15191, 15296, 15396, 15491,
	15582, 15668, 15749, 15826, 15897, 15964, 16026, 16083, 16135, 16182, 16225, 16262,
	16294, 16322, 16344, 16362, 16374, 16382, 16384
};

/* sin of whole degrees, times 16384 */
static int
isin(int deg)
{
	deg = ((deg % 360) + 360) % 360;
	return deg <= 90 ? sine[deg] : deg <= 180 ? sine[180 - deg] : deg <= 270 ? -sine[deg - 180] : -sine[360 - deg];
}

static int
scaled(int v)
{
	return (v + (v >= 0 ? 8192 : -8192)) / 16384;
}

/* The point `along` from the centre towards `deg` (0 is twelve, clockwise) and `aside` to its right. */
static GR_POINT
face_point(int cx, int cy, int deg, int along, int aside)
{
	int s = isin(deg), c = isin(deg + 90);
	GR_POINT p;

	p.x = cx + scaled(along * s + aside * c);
	p.y = cy + scaled(-along * c + aside * s);
	return p;
}

/* A hand, or a tick: from `back` behind the centre to `len`, `half` wide each side, narrowing to the tip. */
static void
hand(GR_DRAW_ID d, int cx, int cy, int deg, int back, int len, int half, int tip, GR_COLOR colour)
{
	GR_POINT p[4];

	p[0] = face_point(cx, cy, deg, -back, -half);
	p[1] = face_point(cx, cy, deg, len, -tip);
	p[2] = face_point(cx, cy, deg, len, tip);
	p[3] = face_point(cx, cy, deg, -back, half);
	GrSetGCForeground(ui_gc, colour);
	GrFillPoly(d, ui_gc, 4, p);
	if (half <= 1) {	/* (a polygon this thin may leave gaps: its spine as a line too) */
		GR_POINT a = face_point(cx, cy, deg, -back, 0), b = face_point(cx, cy, deg, len, 0);

		GrLine(d, ui_gc, a.x, a.y, b.x, b.y);
	}
}

static void
disc(GR_DRAW_ID d, int cx, int cy, int r, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	GrFillEllipse(d, ui_gc, cx, cy, r, r);
}

/* The numerals' font for a face of radius r: made again when the size changes. */
static void
numerals_for(int r)
{
	int size = r / 5, i;
	char text[4];
	GR_SIZE w, h, b;

	size = size < 11 ? 11 : size > 60 ? 60 : size;
	if (num_font && size == num_size)
		return;
	if (num_font)
		GrDestroyFont(num_font);
	num_font = GrCreateFontEx(font_for(size), size, 0, NULL);
	num_size = size;
	GrSetGCFont(ui_gc, num_font);
	for (i = 1; i <= 12; i++) {
		snprintf(text, sizeof text, "%d", i);
		GrGetGCTextSize(ui_gc, text, (int)strlen(text), GR_TFASCII, &w, &h, &b);
		num_w[i] = w, num_h = h;
	}
}

/* The dial, its numerals and the date, and the hour and minute hands: all but the second hand. */
static void
face_draw(GR_DRAW_ID d, int cx, int cy, int r, const struct tm *tm)
{
	GR_COLOR ink = ui_seven ? MWRGB(45, 45, 48) : BLACK;
	char text[4], date[16];
	int i, minute = tm->tm_min, hour = tm->tm_hour % 12;

	if (ui_seven) {
		disc(d, cx, cy, r, MWRGB(130, 133, 140));
		disc(d, cx, cy, r - 1, MWRGB(205, 208, 214));
		disc(d, cx, cy, r - 1 - r / 30, MWRGB(232, 234, 238));
		disc(d, cx, cy, r - 2 - r / 18, WHITE);
	} else {
		disc(d, cx, cy, r, BLACK);
		disc(d, cx, cy, r - 1, UI_SHADOW);
		disc(d, cx, cy, r - 1 - r / 30, BLACK);
		disc(d, cx, cy, r - 2 - r / 30, WHITE);
	}
	r -= 2 + r / 18;	/* the face inside the rim */
	for (i = 0; i < 60; i++) {
		if (i % 5 == 0)
			hand(d, cx, cy, i * 6, -(r * 82 / 100), r * 95 / 100, r / 70 + 1, r / 70 + 1, ink);
		else if (r >= 40) {
			GR_POINT a = face_point(cx, cy, i * 6, r * 90 / 100, 0), b = face_point(cx, cy, i * 6, r * 95 / 100, 0);

			GrSetGCForeground(ui_gc, UI_SHADOW);
			GrLine(d, ui_gc, a.x, a.y, b.x, b.y);
		}
	}
	if (r >= 50) {
		numerals_for(r);
		GrSetGCFont(ui_gc, num_font);
		GrSetGCForeground(ui_gc, ink);
		for (i = 1; i <= 12; i++) {
			GR_POINT p = face_point(cx, cy, i * 30, r * 68 / 100, 0);

			snprintf(text, sizeof text, "%d", i);
			GrText(d, ui_gc, p.x - num_w[i] / 2, p.y - num_h / 2, text, (int)strlen(text), GR_TFASCII | GR_TFTOP);
		}
	}
	if (r >= 70) {
		strftime(date, sizeof date, "%a %d", tm);
		ui_text(d, cx - ui_width(&ui_var_widths, ui_var, date, -1) / 2, cy + r * 32 / 100, date, -1, UI_SHADOW, 0);
	}
	hand(d, cx, cy, hour * 30 + minute / 2, r / 8, r * 52 / 100, r / 22 + 1, r / 60 + 1, ink);
	hand(d, cx, cy, minute * 6 + tm->tm_sec / 10, r / 8, r * 80 / 100, r / 30 + 1, r / 80 + 1, ink);
	disc(d, cx, cy, r / 20 + 2, ink);
}

static void
second_hand(int sec)
{
	GR_COLOR red = ui_seven ? MWRGB(214, 40, 40) : MWRGB(192, 0, 0);
	int r = face_r - 2 - face_r / 18;

	hand(window, face_cx, face_cy, sec * 6, r / 5, r * 88 / 100, r / 100 + 1, 1, red);
	disc(window, face_cx, face_cy, r / 28 + 2, red);
}

static void
face_drop(void)
{
	if (face_pix)
		GrDestroyWindow(face_pix);
	face_pix = 0, face_pix_side = 0, base_min = -1;
}

/* The face for the time: its pixmap drawn again when the minute (or the size) changed, then copied. */
static void
face_show(const struct tm *tm)
{
	int side = face_r * 2 + 3, x = face_cx - face_r - 1, y = face_cy - face_r - 1;
	int minute = tm->tm_hour * 60 + tm->tm_min;

	if (face_pix_side != side) {
		face_drop();
		face_pix = GrNewPixmap(side, side, NULL);
		face_pix_side = side;
	}
	if (!face_pix) {	/* (no pixmap: drawn on the window each time) */
		ui_fill(window, x, y, side, side, UI_FACE);
		face_draw(window, face_cx, face_cy, face_r, tm);
		base_min = minute;
	} else {
		if (minute != base_min) {
			ui_fill(face_pix, 0, 0, side, side, UI_FACE);
			face_draw(face_pix, face_r + 1, face_r + 1, face_r, tm);
			base_min = minute;
		}
		GrCopyArea(window, ui_gc, x, y, side, side, face_pix, 0, 0, MWROP_COPY);
	}
	if (show_seconds)
		second_hand(tm->tm_sec);
}

/* ---- each tab's layout: the buttons and where the figures go ---- */

static long long
watch_ms(void)
{
	return watch_base + (watch_running ? now_ms() - watch_start : 0);
}

static long long
timer_ms(void)
{
	return timer_running ? timer_end - now_ms() : timer_started ? timer_left : (long long)timer_set * 1000;
}

static void
format_watch(char *text, int room, long long ms)
{
	int t = (int)(ms / 100);

	if (t >= 36000)
		snprintf(text, room, "%d:%02d:%02d.%d", t / 36000, t / 600 % 60, t / 10 % 60, t % 10);
	else
		snprintf(text, room, "%02d:%02d.%d", t / 600, t / 10 % 60, t % 10);
}

static void
format_hms(char *text, int room, int seconds)
{
	snprintf(text, room, "%02d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
}

/* "5 min", "1 h 30 min", "45 s" */
static void
format_span(char *text, int room, int s)
{
	if (s >= 3600)
		snprintf(text, room, s % 3600 / 60 ? "%d h %d min" : "%d h", s / 3600, s % 3600 / 60);
	else if (s >= 60)
		snprintf(text, room, s % 60 ? "%d min %d s" : "%d min", s / 60, s % 60);
	else
		snprintf(text, room, "%d s", s);
}

/* A height for figures `chars` cells wide that fit `w` across and `h` down. */
static int
fit_height(int chars, int w, int h)
{
	int by_w = w * 10 / (chars * 6);

	return by_w < h ? by_w : h;
}

/* The +/- buttons over and under the groups of two digits of figures at big_x, big_y. */
static void
spin_buttons(int groups)
{
	int gw = 2 * big.cell + big.w[':'], k, bw = 2 * big.cell < 30 ? 30 : 2 * big.cell;

	for (k = 0; k < groups; k++) {
		int cx = big_x + k * gw + big.cell;

		add_button(cx - bw / 2, big_y - 28, bw, 24, B_UP + k, "+");
		add_button(cx - bw / 2, big_y + big.height + 6, bw, 24, B_DOWN + k, "-");
	}
}

static void
layout(void)
{
	int area = height - TOP - 8, w = width - 24, h;
	char text[24];

	nbuttons = 0;
	switch (tab) {
	case TAB_CLOCK:
		h = width - 16 < area ? width - 16 : area;
		face_r = h / 2 - 2;
		face_r = face_r < 20 ? 20 : face_r;
		face_cx = width / 2, face_cy = TOP + area / 2;
		break;
	case TAB_DIGITAL: {
		static const int ids[2] = { B_24H, B_SECONDS };
		const char *labels[2] = { h24 ? "24-hour: on" : "24-hour: off", show_seconds ? "Seconds: on" : "Seconds: off" };

		big_set(fit_height(show_seconds ? 7 : 5, w - (h24 ? 0 : 30), area - 90));
		strcpy(text, show_seconds ? "00:00:00" : "00:00");
		big_x = (width - big_width(text) - (h24 ? 0 : 30)) / 2;
		big_y = TOP + (area - 34 - big.height - 30) / 2;
		button_row(height - 8 - 24, 110, 2, ids, labels);
		break;
	}
	case TAB_WATCH: {
		static const int ids[3] = { B_WATCH_GO, B_LAP, B_WATCH_RESET };
		const char *labels[3] = { watch_running ? "Stop" : watch_base ? "Go on" : "Start", "Lap", "Reset" };

		big_set(fit_height(8, w, area / 3));
		big_y = TOP + 8;
		button_row(big_y + big.height + 12, 80, 3, ids, labels);
		lap_list.x = 8, lap_list.width = width - 16;
		lap_list.y = big_y + big.height + 48;
		lap_list.height = height - 8 - lap_list.y;
		lap_list.count = nlaps;
		break;
	}
	case TAB_TIMER: {
		static const int ids[2] = { B_TIMER_GO, B_TIMER_RESET };
		const char *labels[2] = { timer_running ? "Pause" : timer_started ? "Go on" : "Start", "Reset" };
		int i;

		big_set(fit_height(8, w, area - 150));
		big_x = (width - big_width("00:00:00")) / 2;
		big_y = TOP + 34 + (area - 150 - big.height) / 2;
		if (!timer_started)
			spin_buttons(3);
		button_row(height - 8 - 24 - (timer_started ? 0 : 32), 90, 2, ids, labels);
		for (i = 0; i < 4 && !timer_started; i++) {
			int pw = (width - 16 - 3 * 6) / 4;

			format_span(text, sizeof text, presets[i]);
			add_button(8 + i * (pw + 6), height - 8 - 24, pw, 24, B_PRESET + i, text);
		}
		break;
	}
	case TAB_ALARM: {
		static const int ids[1] = { B_ALARM_ON };
		const char *labels[1] = { alarm_on ? "Alarm: on" : "Alarm: off" };

		big_set(fit_height(5, w - 40, area - 130));
		big_x = (width - big_width("00:00") - (h24 ? 0 : 30)) / 2;
		big_y = TOP + 34 + (area - 130 - big.height) / 2;
		spin_buttons(2);
		button_row(height - 8 - 24, 120, 1, ids, labels);
		break;
	}
	}
	if (focus > nbuttons)
		focus = nbuttons;
	if (print) {
		int i;

		for (i = 0; i < nbuttons; i++)
			printf("nxclock: button %s at %d,%d\n", buttons[i].label, buttons[i].x + buttons[i].w / 2, buttons[i].y + buttons[i].h / 2);
		fflush(stdout);
	}
}

/* ---- each tab's changing part ---- */

static void
draw_digital(const struct tm *tm, int whole)
{
	char text[24], date[64];
	int hour = h24 ? tm->tm_hour : (tm->tm_hour + 11) % 12 + 1;

	snprintf(text, sizeof text, show_seconds ? "%02d:%02d:%02d" : "%02d:%02d", hour, tm->tm_min, tm->tm_sec);
	big_draw(big_x, big_y, text, 8, width - 16);
	if (!h24)
		ui_text(window, big_x + big_width(text) + 6, big_y + big.height / 6, tm->tm_hour < 12 ? "AM" : "PM", -1, BLACK, 0);
	if (whole || tm->tm_mday != shown_day) {
		strftime(date, sizeof date, "%A, %d %B %Y", tm);
		ui_fill(window, 8, big_y + big.height + 12, width - 16, 18, UI_FACE);
		centred(width / 2, big_y + big.height + 14, date, BLACK);
		shown_day = tm->tm_mday;
	}
}

static void
draw_watch(void)
{
	char text[24];
	long long ms = watch_ms();

	format_watch(text, sizeof text, ms);
	big_draw((width - big_width(text)) / 2, big_y, text, 8, width - 16);
	shown_tenth = (int)(ms / 100);
}

static const char *
lap_label(int row)
{
	static char text[80];
	char split[24], total[24];
	int n = nlaps - 1 - row;	/* the newest first */

	format_watch(split, sizeof split, laps[n] - (n ? laps[n - 1] : lap_before));
	format_watch(total, sizeof total, laps[n]);
	snprintf(text, sizeof text, "Lap %d:   %s    (%s in all)", n + 1 + laps_gone, split, total);
	return text;
}

static void
draw_timer(void)
{
	long long ms = timer_ms();
	int left = ms <= 0 ? 0 : (int)((ms + 999) / 1000), bw = width - 48, y = big_y + big.height + 40;
	char text[24];

	format_hms(text, sizeof text, left);
	big_draw(big_x, big_y, text, 8, width - 16);
	if (timer_started) {	/* how much is left, as a bar */
		int done = timer_total > 0 ? (int)((long long)(bw - 2) * (timer_total - (ms < 0 ? 0 : ms)) / timer_total) : 0;

		ui_fill(window, 24, y, bw, 12, WHITE);
		ui_fill(window, 25 + done, y + 1, bw - 2 - done, 10, UI_SELECTED);
		ui_bevel(window, 24, y, bw, 12, 1);
	}
	shown_left = left;
}

static void
draw_alarm_line(void)
{
	char text[96], span[32];
	time_t now;
	struct tm tm = local_now(&now);
	int until = (alarm_hm - (tm.tm_hour * 60 + tm.tm_min) + 1440) % 1440;

	format_span(span, sizeof span, (until ? until : 1440) * 60);
	if (snooze_until)
		snprintf(text, sizeof text, "Snoozed: it rings again in %d min.", (int)((snooze_until - now + 59) / 60));
	else if (alarm_on)
		snprintf(text, sizeof text, "It rings in %s.", span);
	else
		snprintf(text, sizeof text, "The alarm is off.");
	ui_fill(window, 8, big_y + big.height + 38, width - 16, 18, UI_FACE);
	centred(width / 2, big_y + big.height + 40, text, alarm_on || snooze_until ? BLACK : UI_SHADOW);
}

static void
draw_alarm(void)
{
	char text[16];
	int h = alarm_hm / 60;

	snprintf(text, sizeof text, "%02d:%02d", h24 ? h : (h + 11) % 12 + 1, alarm_hm % 60);
	big_draw(big_x, big_y, text, 8, width - 16);
	if (!h24)
		ui_text(window, big_x + big_width(text) + 6, big_y + big.height / 6, h < 12 ? "AM" : "PM", -1, BLACK, 0);
	draw_alarm_line();
}

/* Everything. */
static void
draw(void)
{
	time_t now;
	struct tm tm = local_now(&now);
	int i;

	layout();
	ui_fill(window, 0, 0, width, height, UI_FACE);
	draw_tabs();
	switch (tab) {
	case TAB_CLOCK:
		base_min = -1;
		face_show(&tm);
		break;
	case TAB_DIGITAL:
		draw_digital(&tm, 1);
		break;
	case TAB_WATCH:
		draw_watch();
		if (lap_list.height < 30)
			break;
		ui_list_draw(window, &lap_list, lap_label);
		if (!nlaps)
			centred(width / 2, lap_list.y + 8, "Laps are listed here.", UI_SHADOW);
		break;
	case TAB_TIMER:
		draw_timer();
		break;
	case TAB_ALARM:
		draw_alarm();
		break;
	}
	for (i = 0; i < nbuttons; i++)
		draw_button(i);
	shown_sec = now;
}

static void
show_tab(int which)
{
	tab = which;
	focus = 0;
	if (tab != TAB_CLOCK)
		face_drop();	/* (the GPU keeps eight pixmaps for everyone) */
	reg_set("nxclock.tab", tab_words[tab]);
	draw();
}

/* ---- ringing ---- */

static void tick(void);
static int kept_w, kept_h;
static int alarm_due;

/* The window's X: its size kept, the sound stopped, gone. */
static void
quit(void)
{
	if (width != kept_w || height != kept_h) {
		char size[24];

		snprintf(size, sizeof size, "%dx%d", width, height);
		reg_set("nxclock.size", size);
	}
	sound_stop();
	GrClose();
	exit(0);
}

static void
ask_others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		quit();
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw();
	else if (event->type == GR_EVENT_TYPE_UPDATE && event->update.wid == window && event->update.utype == GR_UPDATE_SIZE) {
		width = event->update.width, height = event->update.height;
		draw();
	}
}

/* Twice a second while a notice is up: the clock goes on, and the beep stops after a minute. */
static int
ask_tick(void)
{
	tick();
	if (now_ms() - ring_began > 60000)
		sound_stop();
	return 0;
}

static int
ring(const char *title, const char *text, const char *const *answers, int count)
{
	int answer;

	if (print) {
		printf("nxclock: ring %s: %s\n", title, text);
		fflush(stdout);
	}
	ringing = 1;
	ring_began = now_ms();
	sound_start();
	ui_ask_icon = "information";
	ui_ask_over = ask_tick;
	answer = ui_ask(title, text, answers, count, ask_others);
	ui_ask_icon = NULL;
	ui_ask_over = NULL;
	sound_stop();
	ringing = 0;
	return answer;
}

static void
timer_start(void)
{
	timer_started = timer_running = 1;
	timer_total = (long long)timer_set * 1000;
	timer_end = now_ms() + timer_total;
	reg_set_int("nxclock.timer", timer_set);
}

static void
timer_done(void)
{
	static const char *const answers[] = { "OK", "Again" };
	char text[96], span[32];

	timer_started = timer_running = 0;
	if (tab == TAB_TIMER)
		draw();
	format_span(span, sizeof span, timer_set);
	snprintf(text, sizeof text, "Time is up: the timer of %s has run out.", span);
	if (ring("Timer", text, answers, 2) == 1)
		timer_start();
	if (tab == TAB_TIMER)
		draw();
}

static void
alarm_ring(void)
{
	static const char *const answers[] = { "Stop", "Snooze 5 min" };
	char text[64];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	snprintf(text, sizeof text, "It is %02d:%02d: the alarm.", tm->tm_hour, tm->tm_min);	/* (after a snooze, not the alarm's time) */
	if (ring("Alarm", text, answers, 2) == 1)
		snooze_until = time(NULL) + 300;
	if (tab == TAB_ALARM)
		draw();
}

/* What time has changed: the part of the tab shown that says it, and whatever rings. */
static void
tick(void)
{
	time_t now;
	struct tm tm = local_now(&now);

	if (ui_restyle()) {	/* (Settings changed the look) */
		face_drop();
		draw();
	}
	if (now != shown_sec) {
		shown_sec = now;
		if (tab == TAB_CLOCK && (show_seconds || tm.tm_hour * 60 + tm.tm_min != base_min))
			face_show(&tm);
		else if (tab == TAB_DIGITAL)
			draw_digital(&tm, 0);
		else if (tab == TAB_ALARM && tm.tm_sec == 0)
			draw_alarm_line();
	}
	if (tab == TAB_WATCH && watch_running && watch_ms() / 100 != shown_tenth)
		draw_watch();
	if (tab == TAB_TIMER && timer_started && (timer_ms() + 999) / 1000 != shown_left)
		draw_timer();
	if (ringing) {	/* (an alarm whose minute comes under a notice rings after it) */
		if (alarm_on && tm.tm_hour * 60 + tm.tm_min == alarm_hm && rung_minute != now / 60) {
			rung_minute = now / 60;
			alarm_due = 1;
		}
		return;
	}
	if (timer_running && timer_ms() <= 0)
		timer_done();
	else if (alarm_due) {
		alarm_due = 0;
		alarm_ring();
	} else if (alarm_on && tm.tm_hour * 60 + tm.tm_min == alarm_hm && rung_minute != now / 60) {
		rung_minute = now / 60;
		alarm_ring();
	} else if (snooze_until && now >= snooze_until) {
		snooze_until = 0;
		alarm_ring();
	}
}

/* How long to sleep: to the next tenth of the stopwatch shown running, else the next second. */
static int
wait_ms(void)
{
	struct timeval tv;
	int wait;

	if (tab == TAB_WATCH && watch_running)
		return 100 - (int)(watch_ms() % 100) + 1;
	gettimeofday(&tv, NULL);
	wait = 1000 - (int)(tv.tv_usec / 1000) + 2;
	if (timer_running) {
		int to = (int)(timer_ms() % 1000) + 1;

		wait = to > 0 && to < wait ? to : wait;
	}
	return wait;
}

/* ---- what the buttons and keys do ---- */

static void
spin(int k, int by)
{
	if (tab == TAB_TIMER && !timer_started) {
		int h = timer_set / 3600, m = timer_set / 60 % 60, s = timer_set % 60;

		if (k == 0)
			h = (h + by + 24) % 24;
		else if (k == 1)
			m = (m + by + 60) % 60;
		else
			s = (s + by + 60) % 60;
		timer_set = h * 3600 + m * 60 + s;
		draw_timer();
	} else if (tab == TAB_ALARM && k < 2) {
		alarm_hm = (alarm_hm + (k == 0 ? by * 60 : by) + 1440) % 1440;
		snooze_until = 0;
		keep_alarm();
		draw_alarm();
	}
}

static void
act(int id)
{
	long long now = now_ms();

	switch (id) {
	case B_24H:
		h24 = !h24;
		reg_set_int("nxclock.24h", h24);
		break;
	case B_SECONDS:
		show_seconds = !show_seconds;
		reg_set_int("nxclock.seconds", show_seconds);
		break;
	case B_WATCH_GO:
		if (watch_running)
			watch_base += now - watch_start;
		else
			watch_start = now;
		watch_running = !watch_running;
		break;
	case B_LAP:
		if (!watch_running && !watch_base)
			return;
		if (nlaps == LAPS) {
			lap_before = laps[0], laps_gone++;
			memmove(laps, laps + 1, sizeof laps[0] * --nlaps);
		}
		laps[nlaps++] = watch_ms();
		lap_list.top = 0, lap_list.selected = -1;
		break;
	case B_WATCH_RESET:
		watch_running = 0, watch_base = 0, nlaps = 0, laps_gone = 0, lap_before = 0;
		lap_list.top = 0, lap_list.selected = -1;
		break;
	case B_TIMER_GO:
		if (!timer_started) {
			if (timer_set <= 0)
				return;
			timer_start();
		} else if (timer_running) {
			timer_left = timer_end - now;
			timer_running = 0;
		} else {
			timer_end = now + timer_left;
			timer_running = 1;
		}
		break;
	case B_TIMER_RESET:
		timer_started = timer_running = 0;
		break;
	case B_ALARM_ON:
		alarm_on = !alarm_on;
		snooze_until = 0;
		keep_alarm();
		break;
	default:
		if (id >= B_PRESET && id < B_PRESET + 4)
			timer_set = presets[id - B_PRESET];
		else if (id >= B_UP && id < B_DOWN + 3) {
			spin((id - B_UP) % 3, id < B_DOWN ? 1 : -1);
			return;
		}
	}
	draw();
}

static void
press(GR_EVENT *event)
{
	int x = event->button.x, y = event->button.y, turn = ui_wheel(event), tx, tw, i;

	/* the wheel over the figures of the timer or the alarm turns the group under it */
	if (turn) {
		int gw = 2 * big.cell + big.w[':'];

		if ((tab == TAB_TIMER || tab == TAB_ALARM) && y >= big_y && y < big_y + big.height && x >= big_x && gw > 0)
			spin((x - big_x) / gw, -ui_wheel_sum(event));
		else if (tab == TAB_WATCH && ui_list_event(&lap_list, event))
			ui_list_draw(window, &lap_list, lap_label);
		return;
	}
	for (i = 0; i < TABS; i++) {
		tab_place(i, &tx, &tw);
		if (ui_inside(x, y, tx, 6, tw + 2, 22)) {
			show_tab(i);
			return;
		}
	}
	for (i = 0; i < nbuttons; i++)
		if (ui_inside(x, y, buttons[i].x, buttons[i].y, buttons[i].w, buttons[i].h)) {
			focus = i + 1;
			act(buttons[i].id);
			return;
		}
	if (tab == TAB_WATCH && ui_list_event(&lap_list, event))
		ui_list_draw(window, &lap_list, lap_label);
}

/* Find a button by what it does; -1 when the tab shows none. */
static int
button_of(int id)
{
	int i;

	for (i = 0; i < nbuttons; i++)
		if (buttons[i].id == id)
			return i;
	return -1;
}

static void
key(const GR_EVENT *event)
{
	int ch = event->keystroke.ch;

	if (ui_tab(event, &focus, nbuttons + 1)) {
		draw();
		return;
	}
	if (ch >= '1' && ch < '1' + TABS) {
		show_tab(ch - '1');
		return;
	}
	if (focus == 0 && (ch == MWKEY_LEFT || ch == MWKEY_RIGHT)) {
		show_tab((tab + (ch == MWKEY_LEFT ? TABS - 1 : 1)) % TABS);
		return;
	}
	if (focus > 0 && ui_press_key(ch)) {
		act(buttons[focus - 1].id);
		return;
	}
	/* letters for the buttons, and the arrows for the figures */
	switch (tolower(ch)) {
	case ' ': case 's':
		if (button_of(B_WATCH_GO) >= 0)
			act(B_WATCH_GO);
		else if (button_of(B_TIMER_GO) >= 0)
			act(B_TIMER_GO);
		else if (button_of(B_ALARM_ON) >= 0)
			act(B_ALARM_ON);
		break;
	case 'l':
		if (button_of(B_LAP) >= 0)
			act(B_LAP);
		break;
	case 'r':
		if (button_of(B_WATCH_RESET) >= 0)
			act(B_WATCH_RESET);
		else if (button_of(B_TIMER_RESET) >= 0)
			act(B_TIMER_RESET);
		break;
	case MWKEY_UP: case MWKEY_DOWN:
		spin(1, ch == MWKEY_UP ? 1 : -1);
		break;
	case MWKEY_PAGEUP: case MWKEY_PAGEDOWN:
		spin(0, ch == MWKEY_PAGEUP ? 1 : -1);
		break;
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int i;

	print = getenv("NXCLOCK_PRINT") != NULL;
	read_settings();
	for (i = 0; argc > 1 && i < TABS; i++)
		if (!strcmp(argv[1], tab_words[i]))
			tab = i;
	if (GrOpen() < 0)
		return 1;
	ui_init();
	lap_list.selected = -1;
	if (argc > 2 && tab == TAB_TIMER && atoi(argv[2]) > 0) {
		timer_set = atoi(argv[2]) > 86399 ? 86399 : atoi(argv[2]);
		timer_start();
	}
	if (argc > 2 && tab == TAB_ALARM) {
		int h, m;

		if (sscanf(argv[2], "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
			alarm_hm = h * 60 + m, alarm_on = 1;
			keep_alarm();
		}
	}
	kept_w = width, kept_h = height;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Clock", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	for (;;) {
		GrGetNextEventTimeout(&event, wait_ms());
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && event.update.wid == window &&
			    (event.update.width != width || event.update.height != height)) {
				width = event.update.width, height = event.update.height;
				draw();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			press(&event);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(&event);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			quit();
		}
		tick();
	}
}
