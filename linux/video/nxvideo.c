/* nxvideo [FILE... | --none | --decode FILE]: plays an MPEG-1 film, a VQA, MJPEG in AVI or a GIF animation,
 * decoded ahead on the workers by libmedia into the GPU memory it is shown from; the picture
 * shown is the one the sound card has got to. Several files are a playlist. Keys: docs/apps/video.md. */
#define _GNU_SOURCE	/* strcasestr, for ui_files.h too */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include "media.h"
#include "mcw.h"

#define SAMPLE "/root/Videos/Big Buck Bunny.mpg"

static struct media_video *film;
static struct media_video_info info;

static unsigned
cycles(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static uint8_t *
load(const char *path, unsigned *bytes)
{
	int fd = open(path, O_RDONLY);
	long size = fd < 0 ? -1 : lseek(fd, 0, SEEK_END), got = 0, n;
	uint8_t *file;

	if (size <= 0 || size > 64 << 20 || (file = mcw_alloc((unsigned)size)) == NULL) {
		if (fd >= 0)
			close(fd);
		fprintf(stderr, "nxvideo: %s cannot be read\n", path);
		return NULL;
	}
	lseek(fd, 0, SEEK_SET);
	while (got < size && (n = read(fd, file + got, (size_t)(size - got))) > 0)
		got += n;
	close(fd);
	*bytes = (unsigned)got;
	return file;
}

/* The sum the tests compare (each four bytes rotated in), as the player before libmedia had it. */
static uint32_t
rotated(const uint8_t *bytes, unsigned length, uint32_t sum)
{
	uint32_t word;
	unsigned i;

	for (i = 0; i + 4 <= length; i += 4) {
		memcpy(&word, bytes + i, 4);
		sum = (sum << 5 | sum >> 27) ^ word;
	}
	return sum;
}

/* Every picture and all of the sound decoded here, one after another, with their sums:
 * what any other way of decoding the file must come to. */
static int
decode_all(const char *path)
{
	unsigned bytes, before, spent, sum = 0;
	uint8_t *file = load(path, &bytes);
	const char *wrong;
	const uint32_t *palette;
	const uint8_t *picture;
	int16_t *pcm;
	uint32_t got = 0, more;
	int n;

	if (!file)
		return 1;
	if ((film = media_video_open(file, bytes, 0, &wrong)) == NULL) {
		fprintf(stderr, "nxvideo: %s: %s\n", path, wrong);
		return 1;
	}
	media_video_get_info(film, &info);
	printf("nxvideo: %s %d x %d, %d.%03d pictures a second, %d pictures, sound %d Hz\n", media_kind_name(info.kind), info.width,
	       info.height, info.fps_milli / 1000, info.fps_milli % 1000, info.frames, info.rate);
	before = cycles();
	for (n = 0; n < info.frames; n++) {
		while ((picture = media_video_frame(film, n, &palette)) == NULL)
			media_video_pump(film);
		sum = rotated(picture, info.frame_bytes, sum);
		if (palette)
			sum = rotated((const uint8_t *)palette, 1024, sum);
	}
	spent = cycles() - before;
	printf("nxvideo: pictures' sum %08x, %u thousand instructions a picture (the sound's with them)\n", sum,
	       info.frames ? spent / 1000 / (unsigned)info.frames : 0);
	if (info.rate) {
		pcm = malloc((size_t)info.sound_frames * (size_t)info.channels * 2 + 16);
		while (pcm && got < info.sound_frames) {
			if (!(more = media_video_sound(film, got, pcm + got * (uint32_t)info.channels, info.sound_frames - got)))
				media_video_pump(film);
			got += more;
		}
		printf("nxvideo: sound's sum %08x\n", pcm ? rotated((const uint8_t *)pcm, got * (uint32_t)info.channels * 2, 0) : 0);
	}
	media_video_close(film);
	return 0;
}

#include <GLES/segl.h>
#include <dirent.h>
#include <strings.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_ask.h"
#include "ui_glyph.h"
#include "ui_menu.h"
#include "mc.h"
#include "shaderemu_sound.h"

#define ONE 65536
#define RING 65536u		/* the sound stream's ring, in frames */
#define FEED_MOST 1024u		/* frames put into the ring a turn of the loop, */
#define FEED_AHEAD 4096u	/* while the card has so many still to play */
#define AHEAD 36		/* pictures in hand before it plays, or plays on after it ran dry: groups come in bursts */
#define BAR 40			/* the strip of controls under the picture */
#define KEY_W 62
#define TOOL_W 28		/* a key with a sign: Play or Pause, Stop, the loudspeaker */
#define VOLUME_W 56
#define TIME_W 96
#define KINDS ".mpg .mpeg .vqa .avi .gif"
#define LIST_MOST 64		/* films of a playlist, or of the film's folder */
#define RECENT 8		/* files kept as recent, registry keys video.recent.1 ... */
#define MIN_W 240
#define MIN_H 100
#define EMPTY_W 480		/* the window with no film */
#define EMPTY_H 270
#define TWO_CLICKS 450		/* ms between the clicks of a double click */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN | 	GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE)	/* the pointer's moves only while a knob is held (ui_moves) */

static uint8_t *ring_a, *ring_b;	/* GPU memory for pictures: two pieces */
static int slots_a, slots;
static uint8_t **places;
static uint8_t *canvas[2];		/* an animation's pictures, as the GPU's words, in turn */
static int canvas_turn;
static const uint32_t *palette_shown;
static int shown = -1, dropped, summed = -1, summing;
static uint32_t picture_sum;
static int wait_for_groups = 1;	/* NXVIDEO_START=1: it plays as soon as it has pictures in hand, as it did */

static int sound_on, voice, sound_running;
static int still;		/* paused and gone elsewhere: the picture there is shown once it is decoded */
static int volume = 200, muted;	/* how loud, of 256; muted, the slider keeps its place */
static int kept_volume, kept_muted;	/* as the registry had them: written back only when changed */
static int repeat = 1, remaining;	/* the film again at its end; the time shown as what is left */
static int16_t *sound_ring;
static uint32_t sound_ring_at, written, sound_from;	/* frames given to the card, and the film's frame its first is */
static unsigned clock_base, clock_wall, clock_from;	/* the film's time at a moment, that moment, and the time playing began at */
static uint32_t clock_seen;
static int running, paused, ended, opened;

static GR_WINDOW_ID window, picture;	/* the program's window, and the part of it the GPU draws the film in */
static GLuint texture;
static int win_w, pic_x, pic_y, pic_w, pic_h;	/* the window's width; where in it the film is */
static int bar_y, strip = BAR, held = -1;	/* strip: the controls' height (0 in full window); held: the seek knob's picture */
static int full, full_x, full_y, full_w, full_h;	/* full window, and the frame's place and window's size to go back to */
static const char *name;
static char saying[80], notice[40];	/* the strip's status where the seek bar is; a passing word where the time is */
static unsigned notice_until;

static char *list[LIST_MOST];	/* the playlist: the files given, or the films of this one's folder */
static int list_n, list_at, list_given;
static char recent[RECENT][256];
static int recent_n;

/* Where the strip's controls are (-1: not shown at this width). */
static struct { int play, stop, prev, next, seek, seek_w, time, mute, vol, open, menu; } L;

/* The machine's control words once the GPU's memory is mapped: the buttons and the keys' count. */
static const volatile uint32_t *machine;

static unsigned
milliseconds(void)
{
	struct timespec now;

	/* (the kernel's clock, not the machine's clock word: between the sound card's steps the
	 * film goes by this, and by the host's clock it ran ahead of its pictures) */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* The last picture in its place with none missing before it, from where it plays. */
static int
ready(int *in_time)
{
	int have = media_video_ready(film, in_time);

	return have > shown ? have : shown;
}

/* The sound frame the card is at, of all it has been given since it last began. */
static uint32_t
card_at(void)
{
	return SND_STATE[voice].seen != SND_VOICE[voice].key ? 0 : SND_STATE[voice].position;
}

static void
sound_run(int on)
{
	sound_running = on;
	if (sound_on)
		SND_VOICE[voice].step = on ? snd_step((uint32_t)info.rate) : 0;
}

static int
sound_fed(void)
{
	return !sound_on || sound_from + written >= info.sound_frames;
}

/* Decoded sound into the card's ring (stereo: a film of one channel has it on both), as far as
 * that has room. */
static void
feed(void)
{
	static int16_t one[FEED_MOST];
	uint32_t room, at, n, got, i;
	int ahead;

	if (!sound_on)
		return;
	while (!sound_fed()) {
		room = RING - (written - card_at());
		at = written & (RING - 1);
		n = info.sound_frames - sound_from - written;
		n = n > room ? room : n;
		n = n > RING - at ? RING - at : n;
		/* 4 KB a turn while the card has an eighth of a second to go on with: more is more
		 * than a pass's stores, and the pass would end here with a picture still to show */
		ahead = written - card_at() >= FEED_AHEAD;
		if ((ahead || info.channels == 1) && n > FEED_MOST)
			n = FEED_MOST;
		if (!n)
			return;
		if (info.channels == 2) {
			got = media_video_sound(film, sound_from + written, sound_ring + at * 2, n);
		} else {
			got = media_video_sound(film, sound_from + written, one, n);
			for (i = 0; i < got; i++)
				sound_ring[(at + i) * 2] = sound_ring[(at + i) * 2 + 1] = one[i];
		}
		if (!got)
			return;
		written += got;
		SND_VOICE[voice].loop = written;
		if (ahead)
			return;
	}
}

/* The film's time in milliseconds: where the sound card is, and the clock from there on. */
static unsigned
film_time(void)
{
	unsigned now = milliseconds();
	uint32_t at;

	if (sound_on && sound_running && (at = card_at()) != clock_seen && at <= written) {
		clock_seen = at;
		/* (whole seconds and the rest apart: a 64-bit division is a long library call) */
		clock_base = clock_from + at / (unsigned)info.rate * 1000 + at % (unsigned)info.rate * 1000 / (unsigned)info.rate;
		clock_wall = now;
	}
	return clock_base + (running ? now - clock_wall : 0);
}

static void
run(int on)
{
	unsigned now = film_time();

	clock_base = now;
	clock_wall = milliseconds();
	running = on;
	sound_run(on);
}

static unsigned
time_of(int frame)
{
	return film ? media_video_time(film, frame < 0 ? 0 : frame) : 0;	/* (the strip is drawn before the file is open) */
}

/* ---- the playlist and the recent files ---- */

static int
is_film(const char *file)
{
	const char *dot = strrchr(file, '.');
	char ending[8];

	if (!dot || strlen(dot) >= sizeof ending - 1)
		return 0;
	snprintf(ending, sizeof ending, "%s ", dot);
	return strcasestr(KINDS " ", ending) != NULL;
}

static int
by_name(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* One film given: the playlist is its folder's films in name order, for Previous and Next. */
static void
list_of_folder(const char *path)
{
	char folder[PATH_MAX], whole[PATH_MAX + 260];
	const char *slash = strrchr(path, '/');
	struct dirent *entry;
	DIR *dir;
	int k;

	snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
	if (!folder[0])
		strcpy(folder, "/");
	list_n = 0;
	if ((dir = opendir(folder)) != NULL) {
		while (list_n < LIST_MOST && (entry = readdir(dir)) != NULL)
			if (entry->d_name[0] != '.' && is_film(entry->d_name)) {
				snprintf(whole, sizeof whole, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", entry->d_name);
				if ((list[list_n] = strdup(whole)) != NULL)
					list_n++;
			}
		closedir(dir);
	}
	qsort(list, (size_t)list_n, sizeof *list, by_name);
	for (k = 0; k < list_n; k++)
		if (!strcmp(strrchr(list[k], '/') + 1, slash ? slash + 1 : path))
			break;
	if (k == list_n) {	/* (a film not named as films are, or a folder not read) */
		for (k = 0; k < list_n; k++)
			free(list[k]);
		list[0] = strdup(path);
		list_n = 1, k = 0;
	} else {
		free(list[k]);
		list[k] = strdup(path);
	}
	list_at = k;
}

static void
recent_read(void)
{
	char key[32];
	int k;

	for (recent_n = 0, k = 1; k <= RECENT; k++) {
		snprintf(key, sizeof key, "video.recent.%d", k);
		snprintf(recent[recent_n], sizeof recent[0], "%s", reg_get(key, ""));
		if (recent[recent_n][0])
			recent_n++;
	}
}

/* A film that played goes first of the recent ones. */
static void
recent_add(const char *path)
{
	char whole[PATH_MAX], key[32];
	int k, at;

	if (!realpath(path, whole))
		snprintf(whole, sizeof whole, "%s", path);
	if (strlen(whole) >= sizeof recent[0] || (recent_n && !strcmp(recent[0], whole)))
		return;
	for (at = 0; at < recent_n && strcmp(recent[at], whole); at++)
		;
	if (at == recent_n)
		at = recent_n < RECENT ? recent_n++ : RECENT - 1;
	for (k = at; k > 0; k--)
		memcpy(recent[k], recent[k - 1], sizeof recent[0]);
	snprintf(recent[0], sizeof recent[0], "%s", whole);
	for (k = 0; k < RECENT; k++) {
		snprintf(key, sizeof key, "video.recent.%d", k + 1);
		reg_set(key, k < recent_n ? recent[k] : NULL);
	}
}

static void
recent_clear(void)
{
	char key[32];
	int k;

	for (k = 0; k < recent_n; k++) {
		snprintf(key, sizeof key, "video.recent.%d", k + 1);
		reg_set(key, NULL);
	}
	recent_n = 0;
}

/* The settings kept between sessions, when they changed. */
static void
keep_settings(void)
{
	if (volume != kept_volume)
		reg_set_int("video.volume", volume);
	if (muted != kept_muted)
		reg_set_int("video.muted", muted);
	kept_volume = volume, kept_muted = muted;
}

/* ---- the strip under the picture ---- */

static void
layout(void)
{
	int x = 6;

	L.play = x, x += TOOL_W + 2;
	L.stop = x, x += TOOL_W + 2;
	L.prev = L.next = -1;
	if (win_w >= 420)
		L.prev = x, L.next = x + TOOL_W + 2, x += 2 * (TOOL_W + 2);
	L.seek = x + 6;
	L.menu = win_w - 6 - TOOL_W;
	L.open = L.menu - 2 - KEY_W;
	L.vol = win_w >= 520 ? L.open - 6 - VOLUME_W : -1;
	L.mute = (L.vol >= 0 ? L.vol - 4 : L.open - 6) - TOOL_W;
	L.time = L.mute - 6 - TIME_W;
	L.seek_w = L.time - 6 - L.seek;
	if (L.time < L.seek)
		L.time = -1;
}

static void
set_volume(int to)
{
	volume = to < 0 ? 0 : to > 256 ? 256 : to;
	if (sound_on)
		SND_VOICE[voice].volume = muted ? 0 : SND_VOLUME(volume, volume);
	if (getenv("NXVIDEO_PRINT")) {	/* for a test: what the keys did */
		printf("nxvideo: volume %d%s\n", volume, muted ? ", muted" : "");
		fflush(stdout);
	}
}

static void
clock_text(char *to, size_t room, unsigned ms)
{
	unsigned s = ms / 1000;

	if (s >= 3600)
		snprintf(to, room, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(to, room, "%u:%02u", s / 60, s % 60);
}

/* The seek bar: ui_slider's track and knob, with the part played filled in. */
static void
seek_draw(int x, int y, int width, int value, int most)
{
	int at = x + 4 + (width - 16) * value / (most > 0 ? most : 1);

	ui_fill(window, x, y, width, 18, UI_FACE);
	if (ui_seven) {
		ui_fill(window, x + 4, y + 7, width - 8, 4, MWRGB(231, 234, 234));
		ui_edges(window, x + 4, y + 7, width - 8, 4, MWRGB(176, 176, 176), MWRGB(214, 214, 214));
	} else {
		ui_fill(window, x + 4, y + 7, width - 8, 4, UI_SHADOW);
		ui_bevel(window, x + 4, y + 7, width - 8, 4, 1);
	}
	if (opened && at > x + 5)
		ui_fill(window, x + 5, y + 8, at - x - 5, 2, UI_SELECTED);
	if (ui_seven) {
		ui_button_seven(window, at, y + 1, 8, 16, 0);
	} else {
		ui_fill(window, at, y + 1, 8, 16, UI_FACE);
		ui_bevel(window, at, y + 1, 8, 16, 0);
	}
}

static int
can_step(int by)
{
	if (list_n < 2)
		return 0;
	return !list_given || repeat || (list_at + by >= 0 && list_at + by < list_n);
}

static void
draw_bar(void)
{
	char text[48], t1[16], t2[16];
	unsigned at = time_of(held >= 0 ? held : shown), all = time_of(info.frames);
	int y = bar_y;

	if (!strip)
		return;
	ui_fill(window, 0, y, win_w, BAR, UI_FACE);
	if (ui_seven)
		ui_fill(window, 0, y, win_w, 1, MWRGB(214, 214, 214));
	ui_tool(window, L.play, y + 8, TOOL_W, 24, paused || ended || !opened ? UI_G_PLAY : UI_G_PAUSE, opened ? 0 : 2);
	ui_tool(window, L.stop, y + 8, TOOL_W, 24, UI_G_STOP, opened ? 0 : 2);
	if (L.prev >= 0) {
		ui_tool(window, L.prev, y + 8, TOOL_W, 24, UI_G_PREV, opened || can_step(-1) ? 0 : 2);
		ui_tool(window, L.next, y + 8, TOOL_W, 24, UI_G_NEXT, can_step(1) ? 0 : 2);
	}
	ui_tool(window, L.mute, y + 8, TOOL_W, 24, muted || !sound_on ? UI_G_MUTE : UI_G_SOUND, !sound_on ? 2 : muted);
	if (L.vol >= 0)
		ui_slider(window, L.vol, y + 11, VOLUME_W, volume, 256);
	ui_button(window, L.open, y + 8, KEY_W, 24, " Open...", 0);
	ui_tool(window, L.menu, y + 8, TOOL_W, 24, UI_G_LIST, 0);
	if (saying[0]) {
		ui_text_fit(window, L.seek + 4, y + 13, L.mute - L.seek - 10, saying, BLACK);
		return;
	}
	if (L.seek_w > 40)
		seek_draw(L.seek, y + 11, L.seek_w, held >= 0 ? held : shown < 0 ? 0 : shown, info.frames > 1 ? info.frames - 1 : 1);
	if (L.time < 0)
		return;
	/* after going elsewhere the film waits for a group of pictures: say so where the time is */
	if (notice[0]) {
		snprintf(text, sizeof text, "%s", notice);
	} else if (opened && !running && !paused && !ended) {
		snprintf(text, sizeof text, "decoding...");
	} else {
		clock_text(t1, sizeof t1, remaining ? all - (at < all ? at : all) : at);
		clock_text(t2, sizeof t2, all);
		snprintf(text, sizeof text, "%s%s / %s", remaining ? "-" : "", t1, t2);
	}
	ui_text_fit(window, L.time + 2, y + 13, TIME_W - 2, text, BLACK);
}

static void
say(const char *text)
{
	snprintf(saying, sizeof saying, "%s", text);
	draw_bar();
	GrFlush();
}

/* A word for a moment where the time is ("Volume 75%"). */
static void
note(const char *text)
{
	snprintf(notice, sizeof notice, "%s", text);
	notice_until = milliseconds() + 1500;
	draw_bar();
}

/* An animation's canvas (red, green, blue, alpha bytes) as the GPU's words, 0xTTRRGGBB. */
static uint8_t *
as_words(const uint8_t *in)
{
	uint32_t *out = (uint32_t *)canvas[canvas_turn ^= 1];
	unsigned i, n = (unsigned)(info.width * info.height);

	seglSettle();
	for (i = 0; i < n; i++, in += 4)
		out[i] = (uint32_t)(255 - in[3]) << 24 | (uint32_t)in[0] << 16 | (uint32_t)in[1] << 8 | in[2];
	return (uint8_t *)out;
}

static void
show(int frame)
{
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	const uint32_t *palette;
	const uint8_t *pixels = media_video_frame(film, frame, &palette);

	if (!pixels)
		return;
	glBindTexture(GL_TEXTURE_2D, texture);
	if (info.format == MEDIA_FRAME_YUV420) {
		seglTexturePointer(pixels, info.width, info.height, SEGL_YUV420);
	} else if (info.format == MEDIA_FRAME_PAL8) {
		if (palette && palette != palette_shown) {
			memcpy(seglPalette(), palette, 1024);
			palette_shown = palette;
		}
		seglTexturePointer(pixels, info.width, info.height, GL_COLOR_INDEX8_EXT);
	} else if (info.format == MEDIA_FRAME_WORDS) {
		seglTexturePointer(pixels, info.width, info.height, GL_RGBA);
	} else {
		seglTexturePointer(as_words(pixels), info.width, info.height, GL_RGBA);
	}
	seglQuad(corners, uv, texture, 255, 255, 0);
	/* not waited for (that is a pass's end): no picture is decoded into the shown one's place,
	 * and play_from() settles before the places are anybody's */
	seglSwapEarly();
	shown = frame;
}

/* Plays on from a picture's group: what was decoded is dropped, and the sound is kept as decoded. */
static void
play_from(int frame)
{
	seglSettle();
	if (shown >= 0)
		summing = 0;	/* (NXVIDEO_SUM: the pictures of a film played from its start once) */
	shown = media_video_seek(film, frame) - 1;
	summed = shown;
	clock_from = clock_base = time_of(shown + 1);
	sound_from = info.rate ? (uint32_t)((uint64_t)clock_from * (unsigned)info.rate / 1000) : 0;
	written = 0;
	clock_seen = 0;
	ended = 0;
	running = 0;
	still = 1;
	if (sound_on)
		snd_play(voice, SND_STREAM | SND_SMOOTH | SND_STEREO, sound_ring_at, RING, 0, 0, muted ? 0 : SND_VOLUME(volume, volume));
	sound_running = 0;
}

static void
seek(int frame)
{
	if (!opened)
		return;
	run(0);
	play_from(frame < 0 ? 0 : frame >= info.frames ? info.frames - 1 : frame);
	draw_bar();
}

static void
leave(void)
{
	if (sound_on)
		snd_stop(voice);
	keep_settings();
	if (film)
		media_video_close(film);	/* (the pictures being decoded end at their next one: the workers are waited for) */
	exit(0);
}

/* This program again with other arguments (a film's places are of its size): the GPU's file
 * is closed by the exec, and the frame's place is handed on in NXVIDEO_FRAME. */
static void
again(char **args)
{
	GR_WINDOW_INFO client, frame;
	char place[48];

	if (sound_on)
		snd_stop(voice);
	keep_settings();
	if (window) {
		GrGetWindowInfo(window, &client);
		if (client.parent && client.parent != GR_ROOT_WINDOW_ID) {
			GrGetWindowInfo(client.parent, &frame);
			snprintf(place, sizeof place, "%d,%d,%d", full ? full_x : frame.x, full ? full_y : frame.y, full);
			setenv("NXVIDEO_FRAME", place, 1);
		}
	}
	if (film)
		media_video_close(film);
	GrClose();
	execvp("nxvideo", args);
	exit(1);
}

static void
play_file(const char *path)
{
	char *args[] = { "nxvideo", (char *)path, NULL };

	again(args);
}

/* Closes the film: the player with nothing in it. */
static void
close_film(void)
{
	char *args[] = { "nxvideo", "--none", NULL };

	again(args);
}

/* Film k of the playlist: of the files given (all handed on), or of the folder. */
static void
play_item(int k)
{
	char *args[LIST_MOST + 4], number[12];
	int i;

	k = (k + list_n) % list_n;
	if (!list_given)
		play_file(list[k]);
	snprintf(number, sizeof number, "%d", k);
	args[0] = "nxvideo", args[1] = "--item", args[2] = number;
	for (i = 0; i < list_n; i++)
		args[3 + i] = list[i];
	args[3 + list_n] = NULL;
	again(args);
}

/* Previous: the film's start when it is past its first seconds, else the film before. */
static void
previous(void)
{
	if (opened && (time_of(shown) > 3000 || !can_step(-1)))
		seek(0);
	else if (can_step(-1))
		play_item(list_at - 1);
}

static void
next(void)
{
	if (can_step(1))
		play_item(list_at + 1);
}

static int closing;	/* the window's box was pressed while a file was being chosen */
static void draw_round(void);

static void
while_choosing(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window) {
		draw_round();
		draw_bar();
	}
	/* the player's own box while the file window is up: both go */
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		ui_files_stop = closing = 1;
}

static void
open_another(void)
{
	char path[1024], start[400];
	const char *slash = name && list_n ? strrchr(list[list_at], '/') : NULL;
	int was = running;

	if (opened)
		run(0);
	/* in the folder of the film shown, else in Videos */
	if (slash && slash != list[list_at])
		snprintf(start, sizeof start, "%.*s", (int)(slash - list[list_at]), list[list_at]);
	else
		snprintf(start, sizeof start, "%s", access("/root/Videos", R_OK) == 0 ? "/root/Videos" : "/root");
	if (!ui_choose_file("Open a video", start, KINDS, path, sizeof path, while_choosing)) {
		if (closing)
			leave();
		if (opened && was && !paused)
			run(1);
		GrSetFocus(window);
		return;
	}
	play_file(path);
}

/* A second start while a film plays gives this player its files (or, with none, has it come to
 * the front) through a pipe with a name: one player draws with the GPU at a time. */
#define HANDED "/tmp/nxvideo.fifo"
static int handed_fd = -1;

static int
hand_off(int count, char **paths)
{
	char whole[PATH_MAX], line[4096];
	int fd = open(HANDED, O_WRONLY | O_NONBLOCK | O_CLOEXEC), n = 0, k, ok;

	if (fd < 0)
		return 0;	/* (none, or nobody reading it: ENXIO) */
	/* a line a file, the lot in one write (a pipe takes 4 KB whole) */
	for (k = 0; k < count && n < (int)sizeof line - 2; k++) {
		if (!realpath(paths[k], whole))
			snprintf(whole, sizeof whole, "%s", paths[k]);
		n += snprintf(line + n, sizeof line - (size_t)n, "%s\n", whole);
	}
	if (!count)
		line[n++] = '\n';
	n = n < (int)sizeof line ? n : (int)sizeof line - 1;
	ok = write(fd, line, (size_t)n) == n;
	close(fd);
	if (ok)
		printf("nxvideo: given to the player that is playing\n");
	return ok;
}

static void
take_handed(void)
{
	char text[4096], *args[LIST_MOST + 4], *at, *end;
	ssize_t n;
	int count = 0;

	if (handed_fd < 0 || (n = read(handed_fd, text, sizeof text - 1)) <= 0)
		return;
	text[n] = 0;
	for (at = text; *at && count < LIST_MOST; at = end + 1) {
		if ((end = strchr(at, '\n')) == NULL)
			end = at + strlen(at) - 1;
		end[end[0] == '\n' ? 0 : 1] = 0;
		if (at[0])
			args[3 + count++] = at;
	}
	if (count == 1)
		play_file(args[3]);
	if (count > 1) {
		args[0] = "nxvideo", args[1] = "--item", args[2] = "0";
		args[3 + count] = NULL;
		again(args);
	}
	GrRaiseWindow(window);
	GrSetFocus(window);
}

/* The GPU drawing for another program, said before any worker is taken from that one. */
static const char *
gpu_taken(void)
{
	static char why[160];
	char where[40], name[64] = "another program";
	unsigned other = 0;
	int fd = open("/dev/gpu", O_RDWR | O_CLOEXEC), busy;
	FILE *f;

	if (fd < 0)
		return NULL;
	busy = ioctl(fd, 0x80044705u, &other) < 0 && errno == EBUSY;	/* SEGL_DRAW_CLAIM, given back at the close */
	close(fd);
	if (!busy)
		return NULL;
	snprintf(where, sizeof where, "/proc/%u/comm", other);
	if ((f = fopen(where, "r")) != NULL) {
		if (fgets(name, sizeof name, f))
			name[strcspn(name, "\n")] = 0;
		fclose(f);
	}
	snprintf(why, sizeof why, "The GPU is drawing for %s (process %u).\nOne program at a time draws with it: close %s first.", name, other, name);
	return why;
}

/* ---- the picture's part of the window ---- */

#define RECENT_ROW 18
static int recent_top, recent_x, recent_w, recent_rows;	/* the empty window's list of recent films */

static void
centred(int y, const char *text, GR_COLOR colour)
{
	int w = ui_width(&ui_var_widths, ui_var, text, -1);

	ui_text(window, (win_w - w) / 2 > 4 ? (win_w - w) / 2 : 4, y, text, -1, colour, 0);
}

/* No film: the player's icon, what to do, and the recent films to click. */
static void
draw_empty(void)
{
	int rows = recent_n, h, top, k, cell = ui_icon_of("nxvideo");
	const char *base;

	ui_fill(window, 0, 0, win_w, bar_y, MWRGB(16, 16, 16));
	while (rows > 0 && 48 + 52 + 22 + rows * RECENT_ROW > bar_y - 8)
		rows--;
	h = 48 + 52 + (rows ? 22 + rows * RECENT_ROW : 0);
	top = (bar_y - h) / 2 > 4 ? (bar_y - h) / 2 : 4;
	if (cell >= 0 && bar_y >= 120)
		ui_icon(window, ui_gc, win_w / 2 - 24, top, cell, 48);
	centred(top + 56, "No video is open", WHITE);
	centred(top + 76, "Open... (O) chooses a film; the menu key has recent ones", MWRGB(170, 170, 170));
	recent_rows = rows;
	recent_w = win_w * 2 / 3;
	recent_x = (win_w - recent_w) / 2;
	recent_top = top + 48 + 52 + 22;
	if (rows)
		ui_text(window, recent_x, recent_top - 20, "Recent:", -1, MWRGB(170, 170, 170), 0);
	for (k = 0; k < rows; k++) {
		base = strrchr(recent[k], '/') ? strrchr(recent[k], '/') + 1 : recent[k];
		ui_text_fit(window, recent_x + 12, recent_top + k * RECENT_ROW, recent_w - 12, base,
			access(recent[k], R_OK) == 0 ? MWRGB(110, 170, 255) : MWRGB(110, 110, 110));
	}
}

/* Black round the film, where the window is larger than it. */
static void
draw_round(void)
{
	if (!picture) {
		draw_empty();
		return;
	}
	ui_fill(window, 0, 0, win_w, pic_y, BLACK);
	ui_fill(window, 0, pic_y + pic_h, win_w, bar_y - pic_y - pic_h, BLACK);
	ui_fill(window, 0, pic_y, pic_x, pic_h, BLACK);
	ui_fill(window, pic_x + pic_w, pic_y, win_w - pic_x - pic_w, pic_h, BLACK);
}

/* The window is another size: the film as large as fits above the strip of controls, its
 * shape kept, in the middle, and the strip along the bottom. */
static void
fit(int w, int h)
{
	win_w = w;
	bar_y = h - strip;
	layout();
	pic_w = w, pic_h = bar_y;
	if (info.width > 0 && info.height > 0) {
		pic_h = info.height * w / info.width;
		if (pic_h > bar_y) {
			pic_h = bar_y > 16 ? bar_y : 16;
			pic_w = info.width * pic_h / info.height;
		}
	}
	pic_x = (w - pic_w) / 2;
	pic_y = (bar_y - pic_h) / 2;
	if (picture) {
		GrMoveWindow(picture, pic_x, pic_y);
		GrResizeWindow(picture, pic_w, pic_h);
	}
	draw_round();
	draw_bar();
	if (!texture)
		return;		/* (nothing is drawn by the GPU yet) */
	seglWindowChanged();
	glViewport(0, 0, pic_w, pic_h);
	if (shown >= 0)
		show(shown);
}

/* A size the user gave by the frame's corner: no smaller than the controls want, and the
 * window's height made the film's shape. True when another size was asked for. */
static int
reshape(int w, int h)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	GR_WINDOW_INFO client, frame;
	int want;

	if (w < MIN_W || h < MIN_H) {
		GrResizeWindow(window, w < MIN_W ? MIN_W : w, h < MIN_H ? MIN_H : h);
		return 1;
	}
	if (!opened || info.width <= 0)
		return 0;
	GrGetWindowInfo(window, &client);
	if (!client.parent || client.parent == GR_ROOT_WINDOW_ID)
		return 0;
	GrGetWindowInfo(client.parent, &frame);
	/* (maximised: as large as the screen, with black round the film) */
	if (frame.x <= 0 && frame.y <= 0 && frame.x + frame.width >= screen->cols)
		return 0;
	want = info.height * w / info.width + BAR;
	if (want - h <= 2 && h - want <= 2)
		return 0;
	if (frame.y + want + (frame.height - client.height) > screen->rows - UI_MENU_BAR)
		return 0;
	GrResizeWindow(window, w, want);
	return 1;
}

/* Full window: the frame moved so that the film covers the screen, no strip; or back. */
static void
full_window(int on)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	GR_WINDOW_INFO client, frame;

	if (on == full)
		return;
	GrGetWindowInfo(window, &client);
	if (!client.parent || client.parent == GR_ROOT_WINDOW_ID)
		return;
	GrGetWindowInfo(client.parent, &frame);
	full = on;
	if (on) {
		full_x = frame.x, full_y = frame.y, full_w = client.width, full_h = client.height;
		strip = 0;
		GrMoveWindow(client.parent, -client.x, -client.y);
		GrResizeWindow(window, screen->cols, screen->rows);
		GrRaiseWindow(client.parent);
		fit(screen->cols, screen->rows);
	} else {
		strip = BAR;
		GrMoveWindow(client.parent, full_x, full_y);
		GrResizeWindow(window, full_w, full_h);
		fit(full_w, full_h);
	}
	GrSetFocus(window);
}

static void
toggle(void)
{
	if (!opened)
		return;
	if (ended) {
		seek(0);
		paused = 0;
	} else {
		paused = !paused;	/* (it plays on by itself once it is let go: the loop below) */
		if (paused)
			run(0);
	}
	draw_bar();
}

/* Stop: the film waits at its start. */
static void
stop(void)
{
	if (!opened)
		return;
	if (!paused && !ended)
		toggle();
	seek(0);
	paused = 1;
	draw_bar();
}

static void
volume_by(int by)
{
	char text[24];

	set_volume(volume + by);
	snprintf(text, sizeof text, "Volume %d%%", volume * 100 / 256);
	note(text);
}

static void
mute(void)
{
	muted = !muted;
	set_volume(volume);
	note(muted ? "Muted" : "Sound on");
}

/* ---- the menu (the menu key, the right button, Shift+F10) ---- */

enum { M_OPEN = 1, M_CLOSE, M_PLAY, M_STOP, M_PREV, M_NEXT, M_FULL, M_REPEAT, M_MUTE, M_REMAIN, M_EXIT, M_CLEAR, M_RECENT = 100 };
static struct ui_menu_item menu_items[13], recent_items[RECENT + 1];
static char recent_labels[RECENT][72];

static void
menu_open(int x, int y, int up, int keyboard)
{
	int n = 0, k, grey = opened ? 0 : UI_MENU_GREY;
	const char *base;

#define ITEM(text, ident, how) (menu_items[n].label = (text), menu_items[n].icon = -1, menu_items[n].id = (ident), \
	menu_items[n].sub = NULL, menu_items[n].sub_count = 0, menu_items[n].flags = (how), menu_items[n].icon_file = NULL, n++)
	for (k = 0; k < recent_n; k++) {
		base = strrchr(recent[k], '/') ? strrchr(recent[k], '/') + 1 : recent[k];
		snprintf(recent_labels[k], sizeof recent_labels[k], "%d  %.64s", k + 1, base);
		recent_items[k] = (struct ui_menu_item){ recent_labels[k], -1, M_RECENT + k, NULL, 0, access(recent[k], R_OK) ? UI_MENU_GREY : 0, NULL };
	}
	recent_items[k] = (struct ui_menu_item){ "Clear the list", -1, M_CLEAR, NULL, 0, UI_MENU_LINE | (recent_n ? 0 : UI_MENU_GREY), NULL };
	ITEM("Open...    O", M_OPEN, 0);
	menu_items[n].label = "Recent", menu_items[n].icon = -1, menu_items[n].id = 0, menu_items[n].sub = recent_items;
	menu_items[n].sub_count = recent_n + 1, menu_items[n].flags = 0, menu_items[n].icon_file = NULL, n++;
	ITEM("Close    Ctrl+W", M_CLOSE, grey);
	ITEM(paused || ended || !opened ? "Play    Space" : "Pause    Space", M_PLAY, UI_MENU_LINE | grey);
	ITEM("Stop    S", M_STOP, grey);
	ITEM("Previous    P", M_PREV, opened || can_step(-1) ? 0 : UI_MENU_GREY);
	ITEM("Next    N", M_NEXT, can_step(1) ? 0 : UI_MENU_GREY);
	ITEM("Full window    F", M_FULL, UI_MENU_LINE | (full ? UI_MENU_CHECK : 0));
	ITEM("Repeat    R", M_REPEAT, repeat ? UI_MENU_CHECK : 0);
	ITEM("Mute    M", M_MUTE, (muted ? UI_MENU_CHECK : 0) | (sound_on ? 0 : UI_MENU_GREY));
	ITEM("Time left    T", M_REMAIN, remaining ? UI_MENU_CHECK : 0);
	ITEM("Exit    Q", M_EXIT, UI_MENU_LINE);
#undef ITEM
	ui_menu_open(menu_items, n, x, y, up, keyboard);
}

static void
command(int id)
{
	switch (id) {
	case M_OPEN: open_another(); break;
	case M_CLOSE: if (opened) close_film(); break;
	case M_PLAY: toggle(); break;
	case M_STOP: stop(); break;
	case M_PREV: previous(); break;
	case M_NEXT: next(); break;
	case M_FULL: full_window(!full); break;
	case M_REPEAT:
		repeat = !repeat;
		reg_set_int("video.repeat", repeat);
		note(repeat ? "Repeat on" : "Repeat off");
		break;
	case M_MUTE: mute(); break;
	case M_REMAIN: remaining = !remaining; draw_bar(); break;
	case M_EXIT: leave(); break;
	case M_CLEAR:
		recent_clear();
		if (!picture)
			draw_empty();
		break;
	default:
		if (id >= M_RECENT && id < M_RECENT + recent_n)
			play_file(recent[id - M_RECENT]);
	}
}

/* ---- events ---- */

static void
press(GR_EVENT_BUTTON *b)
{
	static unsigned last_at;
	static int last_x, last_y;
	unsigned now = milliseconds();
	int x = b->x, y = b->y, k;

	if (b->changebuttons & GR_BUTTON_R) {
		menu_open(b->rootx, b->rooty, 0, 0);
		return;
	}
	if (!(b->changebuttons & GR_BUTTON_L))
		return;
	if (b->wid != window || y < bar_y) {
		if (!picture) {	/* the empty window: a recent film */
			k = (y - recent_top) / RECENT_ROW;
			if (y >= recent_top && k < recent_rows && x >= recent_x && x < recent_x + recent_w && access(recent[k], R_OK) == 0)
				play_file(recent[k]);
			return;
		}
		/* a click on the film pauses or plays on; two (the first undone) are full window */
		toggle();
		if (now - last_at < TWO_CLICKS && abs(b->rootx - last_x) < 6 && abs(b->rooty - last_y) < 6) {
			toggle();
			full_window(!full);
			last_at = 0;
			return;
		}
		last_at = now, last_x = b->rootx, last_y = b->rooty;
		return;
	}
	if (x >= L.play && x < L.play + TOOL_W)
		toggle();
	else if (x >= L.stop && x < L.stop + TOOL_W)
		stop();
	else if (L.prev >= 0 && x >= L.prev && x < L.prev + TOOL_W)
		previous();
	else if (L.prev >= 0 && x >= L.next && x < L.next + TOOL_W)
		next();
	else if (x >= L.menu && x < L.menu + TOOL_W)
		menu_open(b->rootx - x + L.menu, b->rooty - y + bar_y, 1, 0);
	else if (x >= L.open && x < L.open + KEY_W)
		open_another();
	else if (x >= L.mute && x < L.mute + TOOL_W)
		mute();
	else if (L.vol >= 0 && x >= L.vol - 2 && x < L.vol + VOLUME_W + 2) {
		held = -2;	/* (the volume's knob) */
		ui_moves(window, EVENTS, 1);
		set_volume(ui_slider_value(x, L.vol, VOLUME_W, 256));
		draw_bar();
	} else if (L.time >= 0 && x >= L.time && x < L.time + TIME_W) {
		remaining = !remaining;
		draw_bar();
	} else if (opened && L.seek_w > 40 && x >= L.seek && x < L.seek + L.seek_w && !saying[0]) {
		held = ui_slider_value(x, L.seek, L.seek_w, info.frames - 1);
		ui_moves(window, EVENTS, 1);
		draw_bar();
	}
}

static void
key(GR_EVENT_KEYSTROKE *k)
{
	int ch = k->ch, ctrl = (k->modifiers & MWKMOD_CTRL) != 0, ox, oy;
	int step = info.fps_milli < 1000 ? 5 : 5 * info.fps_milli / 1000;	/* five seconds of pictures */

	if (ctrl && ch < 128 && isalpha(ch))
		ch = tolower(ch) - 'a' + 1;	/* (Ctrl and a letter: its control code, as some keyboards send it) */
	if (ch == MWKEY_F10 && (k->modifiers & MWKMOD_SHIFT))
		ch = MWKEY_MENU;
	switch (ch) {
	case 'q': leave(); break;
	case MWKEY_ESCAPE: if (full) full_window(0); else leave(); break;
	case ' ': toggle(); break;
	case 's': stop(); break;
	case 'm': mute(); break;
	case 'f': case 'F': full_window(!full); break;
	case 'r': command(M_REPEAT); break;
	case 't': remaining = !remaining; draw_bar(); break;
	case 'n': next(); break;
	case 'p': previous(); break;
	case 'o': case 15: open_another(); break;
	case 23: if (opened) close_film(); break;
	case MWKEY_UP: case '+': case '=': volume_by(16); break;
	case MWKEY_DOWN: case '-': volume_by(-16); break;
	case MWKEY_END: seek(info.frames - 1); break;
	case MWKEY_HOME: seek(0); break;
	case MWKEY_LEFT: seek(shown - step); break;
	case MWKEY_RIGHT: seek(shown + step); break;
	case MWKEY_PAGEUP: seek(shown - 12 * step); break;	/* a minute */
	case MWKEY_PAGEDOWN: seek(shown + 12 * step); break;
	case '.':	/* paused: the next picture, once it is decoded */
		if (opened && paused && shown + 1 < info.frames && media_video_frame(film, shown + 1, NULL)) {
			show(shown + 1);
			draw_bar();
		}
		break;
	case MWKEY_MENU:
		ui_window_origin(window, &ox, &oy);
		menu_open(ox + 20, oy + 20, 0, 1);
		break;
	default:
		if (ch >= '0' && ch <= '9' && opened)	/* a tenth of the film each */
			seek((ch - '0') * info.frames / 10);
	}
}

static void
handle(GR_EVENT *e)
{
	int id;

	if (ui_menu_is_open()) {
		switch (ui_menu_event(e, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id);
			return;
		case UI_MENU_CLOSED:
			GrSetFocus(window);
			return;
		case UI_MENU_TOOK:
			return;
		}
	}
	switch (e->type) {
	case GR_EVENT_TYPE_CLOSE_REQ:
		if (e->general.wid == window)
			leave();
		break;
	case GR_EVENT_TYPE_EXPOSURE:
		if (e->exposure.wid == window) {
			draw_round();
			draw_bar();
		}
		break;
	case GR_EVENT_TYPE_UPDATE:
		if (e->update.wid != window || e->update.utype != GR_UPDATE_SIZE ||
		    (e->update.width == win_w && e->update.height == bar_y + strip))
			break;
		if (!full && reshape(e->update.width, e->update.height))
			break;	/* (the size asked for comes as an event of its own) */
		fit(e->update.width, e->update.height);
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if (ui_wheel(e)) {	/* the wheel is the volume */
			volume_by(-16 * ui_wheel(e));
			break;
		}
		press(&e->button);
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		if (!(e->mouse.buttons & GR_BUTTON_L))
			break;
		if (held >= 0 && L.seek_w > 40) {
			held = ui_slider_value(e->mouse.x, L.seek, L.seek_w, info.frames - 1);
			draw_bar();
		} else if (held == -2 && L.vol >= 0) {
			set_volume(ui_slider_value(e->mouse.x, L.vol, VOLUME_W, 256));
			draw_bar();
		}
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		ui_moves(window, EVENTS, 0);
		if (held >= 0)
			seek(held);
		held = -1;
		draw_bar();
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		if (e->keystroke.wid == window || e->keystroke.wid == picture)
			key(&e->keystroke);
		break;
	}
}

static void
events(void)
{
	GR_EVENT e;

	take_handed();
	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return;
		handle(&e);
	}
}

/* The passing word's time is up. */
static void
notice_over(unsigned now)
{
	if (notice[0] && (int)(now - notice_until) >= 0) {
		notice[0] = 0;
		draw_bar();
	}
}

/* The program's window, at the frame's place NXVIDEO_FRAME hands on (and full again with it). */
static void
make_window(const char *title, int w, int h)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	const char *place = getenv("NXVIDEO_FRAME");
	GR_WM_PROPS props = GR_WM_PROPS_APPWINDOW;
	int x = -1, y = -1, was_full = 0;

	if (place && sscanf(place, "%d,%d,%d", &x, &y, &was_full) >= 2) {
		props |= GR_WM_PROPS_NOAUTOMOVE;
		x = x < 0 ? 0 : x > screen->cols - 80 ? screen->cols - 80 : x;
		y = y < 0 ? 0 : y > screen->rows - 80 ? screen->rows - 80 : y;
	}
	unsetenv("NXVIDEO_FRAME");
	win_w = w, bar_y = h;
	layout();
	window = GrNewWindowEx(props, title, GR_ROOT_WINDOW_ID, x, y, w, h + BAR, UI_FACE);
	GrSelectEvents(window, EVENTS);
	if (picture != (GR_WINDOW_ID)-1) {
		picture = GrNewWindow(window, 0, 0, w, h, 0, BLACK, BLACK);
		GrMapWindow(picture);
	} else {
		picture = 0;
	}
	GrMapWindow(window);
	GrSetFocus(window);
	ui_framed();
	if (was_full)
		full_window(1);
}

static void
open_fifo(void)
{
	mkfifo(HANDED, 0600);
	handed_fd = open(HANDED, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

static void
settings_read(void)
{
	kept_volume = volume = reg_int("video.volume", 200);
	kept_muted = muted = reg_int("video.muted", 0) != 0;
	volume = volume < 0 ? 0 : volume > 256 ? 256 : volume;
	repeat = reg_int("video.repeat", 1) != 0;
	recent_read();
}

/* The player with no film: Open..., the recent films, the menu; a film handed to it plays. */
static void __attribute__((noreturn))
empty_player(void)
{
	GR_EVENT e;

	settings_read();
	picture = (GR_WINDOW_ID)-1;	/* (none) */
	info.width = info.height = 0;
	info.frames = 0;
	make_window("Video", EMPTY_W, EMPTY_H);
	open_fifo();
	say("No video");
	draw_round();
	for (;;) {
		GrGetNextEventTimeout(&e, notice[0] ? 300 : 1000);	/* (the fifo is looked at meanwhile) */
		take_handed();
		notice_over(milliseconds());
		if (e.type != GR_EVENT_TYPE_TIMEOUT && e.type != GR_EVENT_TYPE_NONE)
			handle(&e);
	}
}

/* Why a file is not played, on the console and (on a desktop) in a window; then the player
 * stays, with nothing in it, unless `stay` is 0 (NXVIDEO_ONCE: a test). Returns 1, for main. */
static int
refuse(const char *path, const char *why, int opened, int stay)
{
	static const char *const answers[] = { "OK" };
	char text[300];

	fprintf(stderr, "nxvideo: %s: %s\n", path, why);
	if (!opened && GrOpen() < 0)
		return 1;
	if (!opened)
		ui_init();
	snprintf(text, sizeof text, "%.150s\n%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path, why);
	ui_ask_icon = "error";
	ui_ask("Video", text, answers, 1, opened ? while_choosing : NULL);
	if (!stay || getenv("NXVIDEO_ONCE")) {
		GrClose();
		return 1;
	}
	if (opened)
		close_film();
	empty_player();
}

/* The pictures' size by a look at the file's start (an MPEG-1 sequence header, a VQA's, an
 * AVI's main header or a GIF's own), for the window to be there before the file is taken apart. */
static int
peek(const uint8_t *file, unsigned bytes, int *width, int *height)
{
	struct media_info about;
	unsigned at;

	if (bytes >= 30 && !memcmp(file, "FORM", 4) && !memcmp(file + 8, "WVQAVQHD", 8)) {
		*width = file[26] | file[27] << 8;
		*height = file[28] | file[29] << 8;
		return *width > 0 && *height > 0;
	}
	if (bytes >= 72 && !memcmp(file, "RIFF", 4) && !memcmp(file + 8, "AVI LIST", 8) && !memcmp(file + 20, "hdrlavih", 8)) {
		*width = file[64] | file[65] << 8;
		*height = file[68] | file[69] << 8;
		return *width > 0 && *height > 0;
	}
	if (!media_probe(file, bytes, &about) && about.kind == MEDIA_GIF) {
		*width = about.width, *height = about.height;
		return *width > 0 && *height > 0;
	}
	/* 00 00 01 B3, then twelve bits of width and twelve of height */
	for (at = 0; at + 8 <= bytes && at < 65536; at++)
		if (file[at] == 0 && file[at + 1] == 0 && file[at + 2] == 1 && file[at + 3] == 0xb3) {
			*width = file[at + 4] << 4 | file[at + 5] >> 4;
			*height = (file[at + 5] & 15) << 8 | file[at + 6];
			return *width > 0 && *height > 0 && !((*width | *height) & 15);
		}
	return 0;
}

int
main(int argc, char **argv)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	extern char __DATA_BEGIN__[], _end[];
	const char *path, *text, *wrong;
	char title[160];
	int most_w, most_h, first = 1, none = 0;
	unsigned bytes, spare = 0, left, stat_at = 0, bar_at = 0, now, input = 0, hot_at = 0, asked_at = 0;
	unsigned loading_since = 0, loading_said = 0;	/* while the first pictures are got ready */
	uint8_t *file;
	int wanted, k, hold = -1, was_shown = -2, ahead, once = getenv("NXVIDEO_ONCE") != NULL;

	if (argc > 2 && !strcmp(argv[1], "--decode"))
		return decode_all(argv[2]);
	/* nxvideo [--none | --item K] [FILE...]: K is the film of the files given that plays */
	if (argc > 1 && !strcmp(argv[1], "--none"))
		none = 1, first = 2;
	else if (argc > 2 && !strcmp(argv[1], "--item"))
		list_at = atoi(argv[2]), first = 3;
	if (!none && hand_off(argc - first, argv + first))
		return 0;
	if ((wrong = gpu_taken()) != NULL)
		return refuse(argc > first ? argv[first] : SAMPLE, wrong, 0, 0);
	if (argc - first > 1) {
		for (k = first; k < argc && list_n < LIST_MOST; k++)
			list[list_n++] = argv[k];
		list_given = 1;
		list_at = list_at < 0 || list_at >= list_n ? 0 : list_at;
	} else if (argc > first) {
		list_of_folder(argv[first]);
	} else if (!none && access(SAMPLE, R_OK) == 0) {
		list_of_folder(SAMPLE);	/* (with no file, the image's sample, again and again) */
	}
	if (none || !list_n) {
		if (GrOpen() < 0) {
			fprintf(stderr, "nxvideo: no Nano-X server (start one: nano-X -p &)\n");
			return 1;
		}
		ui_init();
		empty_player();
	}
	path = list[list_at];
	if ((file = load(path, &bytes)) == NULL)
		return refuse(path, "cannot be read", 0, 1);
	/* the pictures' size by a look at the file's start, and the window at once: taking the file
	 * apart and starting the workers is seconds, and a desktop with nothing new on it looks stuck */
	if (!peek(file, bytes, &info.width, &info.height))
		return refuse(path, "is not a film this plays: MPEG-1, VQA, MJPEG in AVI or GIF (docs/apps/video.md)", 0, 1);
	info.frames = 1;
	info.fps_milli = 24000;
	summing = getenv("NXVIDEO_SUM") != NULL;
	if ((text = getenv("NXVIDEO_HOLD")) != NULL)
		hold = atoi(text);
	wait_for_groups = getenv("NXVIDEO_START") == NULL;
	if (GrOpen() < 0) {
		fprintf(stderr, "nxvideo: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	ui_init();
	settings_read();
	most_w = ui_screen()->cols - 20, most_h = ui_screen()->rows - 60 - BAR;
	if ((text = getenv("NXVIDEO_SCREEN")) != NULL)	/* for a test: a smaller desktop, WIDTHxHEIGHT */
		sscanf(text, "%dx%d", &most_w, &most_h), most_w -= 20, most_h -= 60 + BAR;
	/* three times, twice or once as large; a film larger than the desktop as large as fits */
	for (k = 3; k > 1 && (info.width * k > most_w || info.height * k > most_h); k--)
		;
	pic_w = info.width * k, pic_h = info.height * k;
	if (pic_w > most_w)
		pic_h = pic_h * most_w / pic_w, pic_w = most_w;
	if (pic_h > most_h)
		pic_w = pic_w * most_h / pic_h, pic_h = most_h;
	if (getenv("NXVIDEO_PRINT"))
		printf("nxvideo: %d x %d shown at %d x %d, the window %d x %d\n", info.width, info.height, pic_w, pic_h, pic_w, pic_h + BAR);
	name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	if (list_n > 1)
		snprintf(title, sizeof title, "%.120s (%d of %d) - Video", name, list_at + 1, list_n);
	else
		snprintf(title, sizeof title, "%.120s - Video", name);
	make_window(title, pic_w, pic_h);
	say("opening it...");

	/* the workers: an MPEG-1 film takes what it needs of them (a decoder each for 16 at the
	 * sample's size, more for larger pictures, and 8 for the sound), a VQA's frame rows 16 */
	wanted = (text = getenv("NXVIDEO_WORKERS")) != NULL ? atoi(text) : file[0] == 'F' ? 16 : file[0] == 'G' ? 8 : 64;
	if ((film = media_video_open(file, bytes, wanted, &wrong)) == NULL) {
		say(wrong);
		return refuse(path, wrong, 1, 1);	/* (and in a window over the player's, until OK) */
	}
	media_video_get_info(film, &info);
	if (seglInit(picture) < 0) {
		return refuse(path, "this machine has no GPU to show it with", 1, 0);
	}
	machine = (const volatile uint32_t *)((const char *)seglPalette() - 0x400);
	/* pictures in GPU memory: what is left to a program, and the display's spare framebuffer */
	left = seglMemoryLeft() & ~15u;
	ring_a = seglMemory(left);
	ring_b = seglMemorySpare(&spare);
	if (info.format == MEDIA_FRAME_RGBA) {
		/* an animation's canvas, as the GPU's words, in two places in turn */
		if (!ring_a || left < 2 * info.frame_bytes)
			return refuse(path, "no GPU memory for pictures of this size", 1, 1);
		canvas[0] = ring_a, canvas[1] = ring_a + info.frame_bytes;
		slots = 2;
	} else {
		slots_a = ring_a ? (int)(left / info.frame_bytes) : 0;
		slots = slots_a + (ring_b ? (int)(spare / info.frame_bytes) : 0);
		places = malloc(sizeof *places * (size_t)(slots > 0 ? slots : 1));
		for (k = 0; k < slots; k++)
			places[k] = k < slots_a ? ring_a + (unsigned)k * info.frame_bytes : ring_b + (unsigned)(k - slots_a) * info.frame_bytes;
		if (slots < info.least_places || media_video_places(film, places, slots) != NULL)
			return refuse(path, "no GPU memory for pictures of this size", 1, 1);
	}
	glGenTextures(1, &texture);
	glViewport(0, 0, pic_w, pic_h);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);

	sound_on = info.rate && snd_open() == 0 && (voice = snd_claim()) >= 0 && (sound_ring = snd_memory(RING * 4, &sound_ring_at)) != NULL;
	if (info.workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	printf("nxvideo: %s, %s %d x %d, %d.%03d pictures a second, %d of them; %d workers decode; room for %d pictures\n", name,
	       media_kind_name(info.kind), info.width, info.height, info.fps_milli / 1000, info.fps_milli % 1000, info.frames, info.workers,
	       slots);
	fflush(stdout);
	if (!once && !summing)
		recent_add(path);
	/* (pictures in hand before it plays: as many as there are places for, and a group's worth) */
	ahead = AHEAD < slots - 1 ? AHEAD : slots - 1;
	opened = 1;
	open_fifo();
	play_from(0);
	say("getting the first pictures ready...");
	loading_since = milliseconds();

	for (;;) {
		int have, due, target, drew = 0, in_time;

		/* The window system is asked when the button or key words change, for a while
		 * after (the event is on its way), while a button is down or a menu is open; else a
		 * few times a second: a question to it is more than a pass of this core. */
		now = milliseconds();
		if (input != (machine[0x28 / 4] ^ machine[0x2c / 4] << 8)) {
			input = machine[0x28 / 4] ^ machine[0x2c / 4] << 8;
			hot_at = now;
		}
		if (now - asked_at >= 150 || now - hot_at < 120 || (machine[0x28 / 4] & 7) || ui_menu_is_open()) {
			asked_at = now;
			events();
		}
		media_video_pump(film);
		feed();
		have = ready(&in_time);
		/* how far the first pictures are, with the time, while it waits for them */
		if (!running && !ended && saying[0] == 'g' && now - loading_said >= 250 && ahead > 0) {
			int part = (have - shown) * 100 / ahead;
			unsigned tenths = (now - loading_since) / 100;
			char text[80];

			loading_said = now;
			snprintf(text, sizeof text, "getting the first pictures ready: %d%%, %u.%u s",
				 part < 0 ? 0 : part > 99 ? 99 : part, tenths / 10, tenths % 10);
			say(text);
		}
		/* NXVIDEO_SUM: every picture where it is shown from, once, as it comes there */
		if (summing && info.format != MEDIA_FRAME_RGBA) {
			while (summed < have) {
				summed++;
				picture_sum = rotated(places[summed % slots], info.frame_bytes, picture_sum);
			}
			if (summed == info.frames - 1) {
				printf("nxvideo: sum %08x of %d pictures\n", picture_sum, info.frames);
				fflush(stdout);
				summing = 0;
			}
		}
		if (!running && !paused && !ended && (have >= shown + ahead || have == info.frames - 1) &&
		    (!sound_on || written > 0 || sound_fed()) && (have == info.frames - 1 || !wait_for_groups || in_time)) {
			saying[0] = 0;
			run(1);
			draw_bar();
		}
		if (paused && still && have > shown) {
			still = 0;
			show(shown + 1);
			draw_bar();
		}
		if (running) {
			due = media_video_at(film, film_time());
			target = due < have ? due : have;
			if (hold >= 0 && target > hold)
				target = hold;
			if (target > shown) {
				dropped += target - shown - 1;
				show(target);
				drew = 1;
			}
			if (hold >= 0 && shown == hold) {
				run(0);
				draw_bar();
				GrFlush();
				printf("nxvideo: holding picture %d\n", hold);
				fflush(stdout);
				for (;;)
					mc_next_pass();
			}
			if (shown == info.frames - 1) {
				running = 0;	/* (the sound card plays what it still has) */
				ended = 1;
			} else if (due > have + 1) {
				run(0);	/* it has run dry: the sound waits for the pictures */
			}
		}
		if (ended && sound_fed() && (!sound_on || card_at() >= written)) {
			if (once)
				leave();
			/* the next of the files given, the film again, or stopped at its start */
			if (list_given && (list_at + 1 < list_n || repeat))
				play_item(list_at + 1);
			else if (repeat)
				play_from(0);
			else
				stop();
		}
		now = milliseconds();
		notice_over(now);
		/* the bar follows the film four times a second, and at once when it stops or starts */
		if (now - bar_at >= 250 && shown != was_shown && held < 0) {
			draw_bar();
			was_shown = shown;
			bar_at = now;
		}
		if (now - stat_at >= 1000) {
			printf("nxvideostat: picture %d of %d, %d left out, %d ready, %d workers, film at %u ms\n", shown, info.frames, dropped, have,
			       info.workers, film_time());
			fflush(stdout);
			stat_at = now;
		}
		/* nothing to show: the kernel has the core until the next pass (docs/machine/multicore.md) */
		if (!drew)
			mcw_rest();
	}
}
