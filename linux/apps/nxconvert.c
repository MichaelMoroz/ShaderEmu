/*
 * nxconvert: pictures, sound and films made into other kinds (docs/apps/convert.md), with libmedia on the
 * worker cores (docs/system/media.md).
 *
 *	nxconvert [FILE]		the window: what the file is, what to make of it, Convert
 *	nxconvert --info FILE		what the file is, a line
 *	nxconvert --to FORMAT [options] IN OUT
 *		FORMAT: jpg png gif bmp ppm tga (a picture; of a film: every Nth picture, OUT-0001.jpg on),
 *		wav flac (a sound; of a film: its sound), avi (MJPEG and PCM sound), gif (of a film: animated)
 *		--quality Q (JPEG's and AVI's 1 to 100, 85; PNG's level 1 to 9, 6), --scale PERCENT, --size WxH,
 *		--every N (a film's pictures taken), --no-sound (an AVI without), --overwrite
 *	prints what it made and the FNV-1a sum of the bytes written (every picture's in turn for a film's pictures).
 *
 * NXCONVERT_PRINT=1 prints the window's buttons (where they are in the window) and what a Convert made;
 * NXCONVERT_WORKERS=N the workers (63 at most otherwise).
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "media.h"
#include "mcw.h"
#include "ui.h"
#include "ui_files.h"

/* ---------------------------------------------------------------- what is made */

enum { F_JPEG, F_PNG, F_GIF, F_BMP, F_PPM, F_TGA, F_WAV, F_FLAC, F_AVI, FORMATS };
static const char *const endings[FORMATS] = { ".jpg", ".png", ".gif", ".bmp", ".ppm", ".tga", ".wav", ".flac", ".avi" };
static const char *const format_names[FORMATS] = { "JPEG", "PNG", "GIF", "BMP", "PPM", "TGA", "WAV", "FLAC", "AVI" };

enum { S_NONE, S_PICTURE, S_SOUND, S_FILM };

struct source {
	int what;			/* S_* */
	uint8_t *data;
	uint32_t size;
	char kind[24];
	int width, height, frames, fps_milli, rate, channels, alpha;
	uint32_t sound_frames;
	char why[160];
};

struct job {
	int to;				/* F_* */
	int quality, level;		/* JPEG's and AVI's; PNG's */
	int scale, width, height;	/* a percent, or a size asked for (0: by the percent) */
	int every;			/* a film's pictures: every so many */
	int sound;			/* an AVI with the film's sound */
	int overwrite;
};

static struct source src;
static struct job job = { F_JPEG, 85, 6, 100, 0, 0, 1, 1, 0 };
static char in_path[600], out_path[700];
static int workers_wanted, workers_opened, cancelled;
static char made[400];			/* what the last conversion made, a line */
static uint32_t made_sum, made_bytes;

/* How far, for the window (NULL on the command line); `cancelled` is set when the window went. */
static void (*progress_hook)(unsigned done, unsigned total, const char *what);

static void
progress(unsigned done, unsigned total, const char *what)
{
	if (progress_hook)
		progress_hook(done, total, what);
}

static unsigned
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)t.tv_sec * 1000u + (unsigned)(t.tv_nsec / 1000000);
}

static int
workers(void)
{
	const char *asked = getenv("NXCONVERT_WORKERS");
	int n = asked ? atoi(asked) : mcw_most();

	return n > 63 && !asked ? 63 : n;
}

/* The workers are opened for a conversion and closed after: every call of the library lends them. */
static void
workers_begin(void)
{
	workers_wanted = workers();
	workers_opened = workers_wanted > 0 ? mcw_open(workers_wanted) : 0;
	if (workers_opened <= 0)
		workers_opened = 0;
}

static void
workers_end(void)
{
	if (workers_opened)
		mcw_close();
	workers_opened = 0;
}

static uint8_t *
read_file(const char *path, uint32_t *size)
{
	struct stat st;
	int fd = open(path, O_RDONLY);
	uint8_t *f;

	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size == 0) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	*size = (uint32_t)st.st_size;
	f = mmap(NULL, *size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	close(fd);
	return f == MAP_FAILED ? NULL : f;
}

/* Writes the bytes, and keeps their sum for the line that says what was made. */
static const char *
write_file(const char *path, const uint8_t *bytes, uint32_t size)
{
	uint32_t done = 0;
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0)
		return "the file cannot be written";
	while (done < size) {
		ssize_t n = write(fd, bytes + done, size - done);

		if (n <= 0)
			break;
		done += (uint32_t)n;
	}
	close(fd);
	if (done != size)
		return "the file could not be written whole (no room?)";
	made_sum = media_sum(bytes, size, made_sum);
	made_bytes += size;
	return NULL;
}

static const char *
leaf_of(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

/* ---------------------------------------------------------------- what a file is */

static void
source_close(void)
{
	if (src.data)
		munmap(src.data, src.size);
	memset(&src, 0, sizeof src);
}

static const char *
source_open(const char *path)
{
	struct media_info info;
	const char *why;

	source_close();
	snprintf(in_path, sizeof in_path, "%s", path);
	if (!(src.data = read_file(path, &src.size))) {
		snprintf(src.why, sizeof src.why, "%s cannot be read", leaf_of(path));
		return src.why;
	}
	why = media_probe(src.data, src.size, &info);
	snprintf(src.kind, sizeof src.kind, "%s", media_kind_name(info.kind));
	switch (why ? MEDIA_UNKNOWN : info.kind) {
	case MEDIA_PNG: case MEDIA_JPEG: case MEDIA_BMP: case MEDIA_PNM: case MEDIA_TGA:
		src.what = S_PICTURE;
		break;
	case MEDIA_GIF:
		src.what = info.frames > 1 ? S_FILM : S_PICTURE;
		break;
	case MEDIA_MPEG: case MEDIA_VQA: case MEDIA_AVI:
		src.what = S_FILM;
		break;
	default:
		src.what = S_SOUND;	/* (or nothing: the sound's opening says) */
		break;
	}
	src.width = info.width, src.height = info.height, src.frames = info.frames, src.alpha = info.alpha;
	if (src.what == S_SOUND) {
		struct media_audio_info a;
		struct media_audio *m = media_audio_open(src.data, src.size, &why);

		if (!m) {
			snprintf(src.why, sizeof src.why, "not a picture, sound or film this knows (%s)", why);
			src.what = S_NONE;
			return src.why;
		}
		media_audio_get_info(m, &a);
		snprintf(src.kind, sizeof src.kind, "%s", media_kind_name(a.kind));
		src.rate = a.rate, src.channels = a.channels, src.sound_frames = a.frames;
		media_audio_close(m);
	} else if (src.what == S_FILM) {
		struct media_video_info v;
		struct media_video *m = media_video_open(src.data, src.size, 0, &why);

		if (!m) {
			snprintf(src.why, sizeof src.why, "%s", why);
			src.what = S_NONE;
			return src.why;
		}
		media_video_get_info(m, &v);
		src.width = v.width, src.height = v.height, src.frames = v.frames, src.fps_milli = v.fps_milli;
		src.rate = v.rate, src.channels = v.channels, src.sound_frames = v.sound_frames;
		media_video_close(m);
	} else if (why) {
		snprintf(src.why, sizeof src.why, "%s", why);
		src.what = S_NONE;
		return src.why;
	}
	return NULL;
}

/* What the file is, a line: "JPEG picture, 640 x 480, 123,456 bytes". */
static const char *
source_line(void)
{
	static char line[200];
	char bytes[40];
	uint32_t s = src.size;

	if (s >= 1000000)
		snprintf(bytes, sizeof bytes, "%u,%03u,%03u bytes", s / 1000000, s / 1000 % 1000, s % 1000);
	else if (s >= 1000)
		snprintf(bytes, sizeof bytes, "%u,%03u bytes", s / 1000, s % 1000);
	else
		snprintf(bytes, sizeof bytes, "%u bytes", s);
	if (src.what == S_PICTURE)
		snprintf(line, sizeof line, "%s picture, %d x %d, %s", src.kind, src.width, src.height, bytes);
	else if (src.what == S_SOUND) {
		unsigned ms = src.rate && src.sound_frames ? (unsigned)((uint64_t)src.sound_frames * 1000u / (unsigned)src.rate) : 0;

		snprintf(line, sizeof line, "%s sound, %d Hz, %s, %u.%u s, %s", src.kind, src.rate, src.channels == 1 ? "mono" : "stereo",
			 ms / 1000, ms % 1000 / 100, bytes);
	} else if (src.what == S_FILM) {
		unsigned ms = src.fps_milli ? (unsigned)((uint64_t)src.frames * 1000000u / (unsigned)src.fps_milli) : 0;
		char sound[60] = ", no sound";

		if (src.rate)
			snprintf(sound, sizeof sound, ", sound %d Hz %s", src.rate, src.channels == 1 ? "mono" : "stereo");
		snprintf(line, sizeof line, "%s film, %d x %d, %d pictures at %d.%02d a second (%u.%u s)%s, %s", src.kind, src.width,
			 src.height, src.frames, src.fps_milli / 1000, src.fps_milli % 1000 / 10, ms / 1000, ms % 1000 / 100, sound, bytes);
	} else
		snprintf(line, sizeof line, "%s", src.why[0] ? src.why : "no file");
	return line;
}

/* Whether a format can be made of the source. */
static int
can_make(int to)
{
	if (src.what == S_PICTURE)
		return to <= F_TGA;
	if (src.what == S_SOUND)
		return to == F_WAV || to == F_FLAC;
	if (src.what == S_FILM)
		return to <= F_TGA || to == F_AVI || ((to == F_WAV || to == F_FLAC) && src.rate);
	return 0;
}

/* A film made into pictures: one file a picture taken. */
static int
many_pictures(void)
{
	return src.what == S_FILM && job.to != F_GIF && job.to <= F_TGA;
}

/* The size made, of the source's. */
static void
size_made(int *w, int *h)
{
	if (job.width > 0 && job.height > 0) {
		*w = job.width, *h = job.height;
		return;
	}
	*w = (src.width * job.scale + 50) / 100, *h = (src.height * job.scale + 50) / 100;
	if (*w < 1)
		*w = 1;
	if (*h < 1)
		*h = 1;
}

/* The name of picture n (from 1) of a film's pictures: OUT's name with -0001 before its ending. */
static void
numbered(char *to, int size, const char *out, int n)
{
	const char *dot = strrchr(out, '.'), *slash = strrchr(out, '/');

	if (!dot || (slash && dot < slash))
		dot = out + strlen(out);
	snprintf(to, size, "%.*s-%04d%s", (int)(dot - out), out, n, dot);
}

/* Beside the file, with the format's ending, never one that is there: name.png, name-2.png, ... */
static void
default_out(void)
{
	char stem[600], test[720];
	const char *dot = strrchr(in_path, '.'), *slash = strrchr(in_path, '/');
	int k;

	if (!dot || (slash && dot < slash))
		dot = in_path + strlen(in_path);
	snprintf(stem, sizeof stem, "%.*s", (int)(dot - in_path), in_path);
	for (k = 1; k < 1000; k++) {
		if (k == 1)
			snprintf(out_path, sizeof out_path, "%s%s", stem, endings[job.to]);
		else
			snprintf(out_path, sizeof out_path, "%s-%d%s", stem, k, endings[job.to]);
		if (many_pictures())
			numbered(test, sizeof test, out_path, 1);
		else
			snprintf(test, sizeof test, "%s", out_path);
		if (strcmp(test, in_path) && access(test, F_OK) != 0)
			return;
	}
}

/* ---------------------------------------------------------------- pictures */

static const char *library_doing = "decoding";	/* what the library's loops are at (their pieces tell how far) */
static unsigned stage_from, stage_to = 1000;	/* and the part of the bar that stage has, of 1000 (it never goes back) */

static void
stage(const char *what, unsigned from, unsigned to)
{
	library_doing = what, stage_from = from, stage_to = to;
	progress(from, 1000, what);
}

static void
told_by_library(uint32_t done, uint32_t total)
{
	progress(stage_from + (total ? (unsigned)((unsigned long long)done * (stage_to - stage_from) / total) : 0), 1000, library_doing);
}

/* Pixels of w x h, 3 or 4 channels, rows on whole 16 bytes. */
static int
image_alloc(struct media_image *im, int w, int h, int channels)
{
	memset(im, 0, sizeof *im);
	im->width = w, im->height = h, im->channels = channels;
	im->stride = (w * channels + 15) & ~15;
	im->background = 0xffffff;
	im->pixels = aligned_alloc(16, (size_t)im->stride * (size_t)h);
	return im->pixels != NULL;
}

/* A picture in memory into its format's bytes, written. */
static const char *
picture_write(const struct media_image *im, int to, const char *path, int w)
{
	uint32_t size = 0;
	uint8_t *f;
	const char *why;

	switch (to) {
	case F_JPEG: f = media_jpeg_encode(im, job.quality, &size, w); break;
	case F_PNG: f = media_png_encode(im, job.level, &size, w); break;
	case F_GIF: f = media_gif_encode(im, &size, w); break;
	case F_BMP: f = media_bmp_encode(im, &size); break;
	case F_TGA: f = media_tga_encode(im, &size); break;
	default: f = media_ppm_encode(im, &size); break;
	}
	if (!f)
		return "not enough memory to encode the picture";
	why = write_file(path, f, size);
	media_free(f, size);
	return why;
}

static const char *
convert_picture(void)
{
	struct media_image in, out;
	const char *why;
	/* (see-through pixels kept where the format has them, and only when the picture has any) */
	int w, h, channels = src.alpha && (job.to == F_PNG || job.to == F_GIF || job.to == F_TGA) ? 4 : 3;

	memset(&in, 0, sizeof in);
	in.channels = channels;
	in.background = 0xffffff;
	stage("decoding", 0, 500);
	media_progress = told_by_library;
	why = media_image_decode(src.data, src.size, &in, workers_opened);
	media_progress = NULL;
	if (why)
		return why;
	size_made(&w, &h);
	media_progress = told_by_library;	/* (the encoders' loops on the workers tell their pieces too) */
	if (w != in.width || h != in.height) {
		stage("resizing", 500, 600);
		if (!image_alloc(&out, w, h, channels)) {
			media_image_free(&in);
			return "not enough memory for the picture made";
		}
		media_image_fit(&in, &out, 0, workers_opened);
		media_image_free(&in);
		stage("encoding", 600, 1000);
		why = picture_write(&out, job.to, out_path, workers_opened);
		free(out.pixels);
	} else {
		stage("encoding", 500, 1000);
		why = picture_write(&in, job.to, out_path, workers_opened);
		media_image_free(&in);
	}
	media_progress = NULL;
	stage_from = 0, stage_to = 1000, library_doing = "decoding";
	if (!why)
		snprintf(made, sizeof made, "%s: %s picture, %d x %d", out_path, format_names[job.to], w, h);
	return why;
}

/* ---------------------------------------------------------------- sound */

static const char *
sound_write(const int16_t *pcm, uint32_t frames, int rate, int channels)
{
	uint32_t size = 0;
	uint8_t *f;
	const char *why;

	stage("encoding", 500, 1000);
	media_progress = told_by_library;
	f = job.to == F_FLAC ? media_flac_encode(pcm, frames, rate, channels, &size, workers_opened)
			      : media_wav_encode(pcm, frames, rate, channels, &size);
	media_progress = NULL;
	stage_from = 0, stage_to = 1000, library_doing = "decoding";
	if (!f)
		return "not enough memory to encode the sound";
	why = write_file(out_path, f, size);
	media_free(f, size);
	if (!why)
		snprintf(made, sizeof made, "%s: %s sound, %d Hz, %d ch, %u frames", out_path, format_names[job.to], rate, channels, frames);
	return why;
}

static const char *
convert_sound(void)
{
	struct media_audio_info info;
	struct media_audio *a;
	const char *why;
	int16_t *pcm;
	uint32_t frames, most;

	if (!(a = media_audio_open(src.data, src.size, &why)))
		return why;
	media_audio_get_info(a, &info);
	/* a tune that does not say how long it is (MOD): read in order, ten minutes at most */
	most = info.frames ? info.frames : (uint32_t)info.rate * 600u;
	if (!(pcm = aligned_alloc(16, ((size_t)most * (size_t)info.channels * 2 + 15) & ~(size_t)15))) {
		media_audio_close(a);
		return "not enough memory for the sound";
	}
	stage("decoding", 0, 500);
	if (info.frames && info.pieces) {
		media_progress = told_by_library;
		frames = media_audio_decode_all(a, pcm, most, workers_opened);
		media_progress = NULL;
	} else {
		int n;

		for (frames = 0; frames < most && !cancelled; frames += (uint32_t)n) {
			uint32_t want = most - frames < 4096 ? most - frames : 4096;

			if ((n = media_audio_read(a, pcm + frames * (uint32_t)info.channels, (int)want)) <= 0)
				break;
			progress((unsigned)((unsigned long long)frames * 500 / most), 1000, "decoding");
		}
	}
	media_audio_close(a);
	why = cancelled ? "stopped" : sound_write(pcm, frames, info.rate, info.channels);
	free(pcm);
	return why;
}

/* ---------------------------------------------------------------- films */

#define BATCH_MOST 96
#define BATCH_BYTES (24u << 20)

/* Waits for picture n of an open film. */
static const uint8_t *
film_frame(struct media_video *v, int n, const uint32_t **palette)
{
	const uint8_t *frame;

	while (!(frame = media_video_frame(v, n, palette)) && !cancelled) {
		media_video_pump(v);
		mcw_rest();	/* (the workers have it) */
	}
	return frame;
}

/* The film's whole sound, decoded ahead by the film itself: the frames had (a film whose sound stops coming for
 * 20 s has what came). */
static uint32_t
film_sound(struct media_video *v, const struct media_video_info *info, int16_t *pcm)
{
	uint32_t got = 0, more, since = now_ms();

	while (got < info->sound_frames && !cancelled) {
		more = media_video_sound(v, got, pcm + got * (uint32_t)info->channels, info->sound_frames - got);
		if (more) {
			got += more;
			since = now_ms();
			progress(got, info->sound_frames, "the sound");
			continue;
		}
		if (now_ms() - since > 20000)
			break;
		media_video_pump(v);
		mcw_rest();
	}
	return got;
}

static const char *
convert_film(void)
{
	struct media_video_info info;
	struct media_video *v;
	struct media_avi_out *avi = NULL;
	struct media_image full, out, *gif = NULL;
	const char *why = NULL;
	uint8_t *batch = NULL;
	uint32_t *batch_pal = NULL, *sound_counts = NULL, sound_got = 0, given = 0, frame_bytes;
	int16_t *pcm = NULL;
	int w, h, next = 0, taken = 0, gif_count = 0, gif_most = 0, slots, k, anim_delay;
	int *batch_n = NULL, reopens = 0, batches = 0;
	unsigned t_start = now_ms(), t_from = 0, t_decode = 0, t_reopen = 0, t_encode = 0;
	int pictures = job.to <= F_TGA, want_sound = job.to == F_WAV || job.to == F_FLAC || (job.to == F_AVI && job.sound);

	if (!(v = media_video_open(src.data, src.size, workers_opened, &why)))
		return why;
	media_video_get_info(v, &info);
	memset(&full, 0, sizeof full);
	memset(&out, 0, sizeof out);
	/* the sound first, whole: the film decodes it ahead by itself */
	if (want_sound && info.rate && info.sound_frames) {
		if (!(pcm = malloc((size_t)info.sound_frames * (size_t)info.channels * 2 + 16))) {
			why = "not enough memory for the film's sound";
			goto done;
		}
		sound_got = film_sound(v, &info, pcm);
	}
	if (job.to == F_WAV || job.to == F_FLAC) {
		media_video_close(v), v = NULL;
		why = cancelled ? "stopped" : !sound_got ? "the film's sound did not come" : sound_write(pcm, sound_got, info.rate, info.channels);
		goto done;
	}
	size_made(&w, &h);
	if (!image_alloc(&full, info.width, info.height, 3) || ((w != info.width || h != info.height) && !image_alloc(&out, w, h, 3))) {
		why = "not enough memory for the pictures";
		goto done;
	}
	if (!out.pixels)
		out = full;
	if (job.to == F_AVI && !(avi = media_avi_begin(w, h, info.fps_milli, sound_got ? info.rate : 0, info.channels))) {
		why = "not enough memory for the film";
		goto done;
	}
	if (job.to == F_GIF) {
		gif_most = (info.frames + job.every - 1) / job.every;
		if ((uint64_t)gif_most * (uint64_t)w * (uint64_t)h * 3 > 48u << 20) {
			why = "an animation that large does not fit: a smaller size, or fewer pictures (every Nth)";
			goto done;
		}
		if (!(gif = calloc((size_t)gif_most, sizeof *gif))) {
			why = "not enough memory for the animation";
			goto done;
		}
	}
	/*
	 * The pictures in batches: while a film is open its workers are its own (media.h), so a batch of pictures is
	 * copied out as the film gives them, the film closed, the batch encoded on every core, and the film opened
	 * again where it was (decoded from its group's first picture).
	 */
	frame_bytes = (info.frame_bytes + 15) & ~15u;
	slots = (int)(BATCH_BYTES / frame_bytes);
	slots = slots < 1 ? 1 : slots > BATCH_MOST ? BATCH_MOST : slots;
	if (!(batch = aligned_alloc(16, (size_t)slots * frame_bytes)) || !(batch_pal = malloc((size_t)slots * 1024)) ||
	    !(batch_n = malloc((size_t)slots * sizeof *batch_n)) || !(sound_counts = malloc((size_t)slots * sizeof *sound_counts))) {
		why = "not enough memory for the pictures";
		goto done;
	}
	anim_delay = (int)((uint64_t)job.every * 1000000u / (unsigned)(info.fps_milli > 0 ? info.fps_milli : 25000));
	t_from = now_ms();
	while (next < info.frames && !why && !cancelled) {
		int count = 0;
		unsigned t = now_ms();

		batches++;
		if (!v) {
			int from;

			if (!(v = media_video_open(src.data, src.size, workers_opened, &why)))
				break;
			for (from = media_video_seek(v, next); from < next && !cancelled; from++)
				film_frame(v, from, &(const uint32_t *){ NULL });
			t_reopen += now_ms() - t, t = now_ms(), reopens++;
		}
		while (count < slots && next < info.frames && !cancelled) {
			const uint32_t *pal = NULL;
			const uint8_t *frame = film_frame(v, next, &pal);

			if (!frame)
				break;
			if (job.to == F_AVI || next % job.every == 0) {
				memcpy(batch + (size_t)count * frame_bytes, frame, info.frame_bytes);
				if (pal)
					memcpy(batch_pal + count * 256, pal, 1024);
				batch_n[count++] = next;
			}
			next++;
			progress((unsigned)next * 2 - 1, (unsigned)info.frames * 2, "decoding");
		}
		media_video_close(v), v = NULL;
		t_decode += now_ms() - t, t = now_ms();
		if (avi && count && !cancelled && !getenv("NXCONVERT_ONE_BY_ONE")) {
			/* an AVI's pictures side by side, a core each (media_avi_film), each one's sound after it */
			const int16_t *from = pcm ? pcm + given * (uint32_t)info.channels : NULL;

			for (k = 0; k < count; k++) {
				uint32_t due = (uint32_t)((uint64_t)(batch_n[k] + 1) * 1000u * (uint32_t)info.rate / (uint32_t)info.fps_milli);

				if (batch_n[k] + 1 == info.frames || due > sound_got)
					due = sound_got;
				sound_counts[k] = sound_got && due > given ? due - given : 0;
				given += sound_counts[k];
			}
			why = media_avi_film(avi, &info, batch, frame_bytes, info.format == MEDIA_FRAME_PAL8 ? batch_pal : NULL, count, job.quality, from,
					     sound_counts, workers_opened);
			progress((unsigned)batch_n[count - 1] * 2 + 2, (unsigned)info.frames * 2, "encoding");
			count = 0;	/* (done: the loop below has none) */
		}
		for (k = 0; k < count && !why && !cancelled; k++) {
			const uint8_t *frame = batch + (size_t)k * frame_bytes;

			media_video_picture(&info, frame, info.format == MEDIA_FRAME_PAL8 ? batch_pal + k * 256 : NULL, &full, workers_opened);
			if (out.pixels != full.pixels)
				media_image_fit(&full, &out, 0, workers_opened);
			if (avi) {
				uint32_t due = (uint32_t)((uint64_t)(batch_n[k] + 1) * 1000u * (uint32_t)info.rate / (uint32_t)info.fps_milli);

				why = media_avi_picture(avi, &out, job.quality, workers_opened);
				if (batch_n[k] + 1 == info.frames || due > sound_got)
					due = sound_got;
				if (!why && sound_got && due > given)
					why = media_avi_sound(avi, pcm + given * (uint32_t)info.channels, due - given), given = due;
			} else if (gif) {
				if (gif_count < gif_most && image_alloc(&gif[gif_count], w, h, 3)) {
					for (int y = 0; y < h; y++)
						memcpy(gif[gif_count].pixels + (size_t)y * gif[gif_count].stride, out.pixels + (size_t)y * out.stride,
						       (size_t)w * 3);
					gif_count++;
				} else
					why = "not enough memory for the animation";
			} else if (pictures) {
				char name[720];

				numbered(name, sizeof name, out_path, ++taken);
				why = picture_write(&out, job.to, name, workers_opened);
			}
			progress((unsigned)batch_n[k] * 2 + 2, (unsigned)info.frames * 2, "encoding");
		}
		t_encode += now_ms() - t;
	}
	/* NXCONVERT_TIMES=1: where a film's time went (docs/apps/convert.md) */
	if (getenv("NXCONVERT_TIMES"))
		printf("nxconvert: the sound %u ms, the pictures in %d batches of %d: decoded %u ms, %d openings again %u ms, encoded %u ms\n",
		       t_from - t_start, batches, slots, t_decode, reopens, t_reopen, t_encode);
	if (cancelled && !why)
		why = "stopped";
	if (!why && avi) {
		uint32_t size;
		uint8_t *f = media_avi_end(avi, &size);

		avi = NULL;
		why = f ? write_file(out_path, f, size) : "not enough memory for the film";
		if (f)
			media_free(f, size);
		if (!why)
			snprintf(made, sizeof made, "%s: MJPEG AVI, %d x %d, %d pictures, quality %d, %s", out_path, w, h, info.frames, job.quality,
				 given ? "with sound" : "no sound");
	} else if (!why && gif) {
		uint32_t size;
		uint8_t *f;
		int *delays = malloc((size_t)(gif_count ? gif_count : 1) * sizeof *delays);

		progress(1, 1, "the animation's colours and codes");
		for (k = 0; delays && k < gif_count; k++)
			delays[k] = anim_delay;
		f = delays ? media_gif_anim_encode(gif, gif_count, delays, 0, &size, workers_opened) : NULL;
		free(delays);
		why = f ? write_file(out_path, f, size) : "not enough memory for the animation";
		if (f)
			media_free(f, size);
		if (!why)
			snprintf(made, sizeof made, "%s: animated GIF, %d x %d, %d pictures", out_path, w, h, gif_count);
	} else if (!why && pictures) {
		char first[720], last[720];

		numbered(first, sizeof first, out_path, 1);
		numbered(last, sizeof last, out_path, taken);
		snprintf(made, sizeof made, "%d %s pictures %s to %s, %d x %d", taken, format_names[job.to], first, leaf_of(last), w, h);
	}
done:
	if (v)
		media_video_close(v);
	if (avi) {
		uint32_t size;
		uint8_t *f = media_avi_end(avi, &size);

		if (f)
			media_free(f, size);
	}
	for (k = 0; k < gif_count; k++)
		free(gif[k].pixels);
	free(gif);
	free(batch), free(batch_pal), free(batch_n), free(sound_counts), free(pcm);
	if (out.pixels && out.pixels != full.pixels)
		free(out.pixels);
	free(full.pixels);
	return why;
}

/* ---------------------------------------------------------------- one conversion */

static const char *
convert(void)
{
	const char *why;
	char first[720];

	made[0] = 0;
	made_sum = 2166136261u, made_bytes = 0;
	cancelled = 0;
	if (!can_make(job.to))
		return src.what == S_NONE ? (src.why[0] ? src.why : "no file") : "that cannot be made of this file";
	if (many_pictures())
		numbered(first, sizeof first, out_path, 1);
	else
		snprintf(first, sizeof first, "%s", out_path);
	if (!strcmp(first, in_path))
		return "the file made would be the file itself";
	if (!job.overwrite && access(first, F_OK) == 0)
		return "a file of that name is there already";
	workers_begin();
	why = src.what == S_PICTURE ? convert_picture() : src.what == S_SOUND ? convert_sound() : convert_film();
	workers_end();
	return why;
}

/* ---------------------------------------------------------------- the command line */

static int
format_of(const char *name)
{
	int k;

	if (!strcmp(name, "jpeg"))
		return F_JPEG;
	for (k = 0; k < FORMATS; k++)
		if (!strcmp(name, endings[k] + 1))
			return k;
	return -1;
}

static int
command_line(int argc, char **argv)
{
	const char *why;
	unsigned t0;
	int i;

	for (i = 1; i < argc - 2; i++) {
		if (!strcmp(argv[i], "--to") && i + 1 < argc)
			job.to = format_of(argv[++i]);
		else if (!strcmp(argv[i], "--quality") && i + 1 < argc)
			job.quality = job.level = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
			job.scale = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &job.width, &job.height);
		else if (!strcmp(argv[i], "--every") && i + 1 < argc)
			job.every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-sound"))
			job.sound = 0;
		else if (!strcmp(argv[i], "--overwrite"))
			job.overwrite = 1;
		else {
			fprintf(stderr, "nxconvert: what is %s?\n", argv[i]);
			return 2;
		}
	}
	if (job.to < 0 || job.scale < 1 || job.scale > 800 || job.every < 1) {
		fprintf(stderr, "nxconvert: --to jpg|png|gif|bmp|ppm|tga|wav|flac|avi, --scale 1 to 800, --every 1 or more\n");
		return 2;
	}
	if (job.quality < 1 || job.quality > 100)
		job.quality = 85;
	if (job.level < 1 || job.level > 9)
		job.level = 6;
	if ((why = source_open(argv[argc - 2])) != NULL) {
		printf("nxconvert: %s: %s\n", argv[argc - 2], why);
		return 1;
	}
	printf("nxconvert: %s: %s\n", argv[argc - 2], source_line());
	snprintf(out_path, sizeof out_path, "%s", argv[argc - 1]);
	t0 = now_ms();
	if ((why = convert()) != NULL) {
		printf("nxconvert: not made: %s\n", why);
		return 1;
	}
	printf("nxconvert: made %s, %u bytes, sum %08x, %u ms\n", made, made_bytes, made_sum, now_ms() - t0);
	return 0;
}

/* ---------------------------------------------------------------- the window */

#define WIN_W 500
#define WIN_H 318
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ)

static GR_WINDOW_ID window;
static int closing, named;		/* the window goes; the name was chosen (not the default) */
static unsigned bar_share = 0, bar_drawn_at;
static char status[200] = "", bar_line[120] = "";
static int busy;

struct place { int x, y, w, h; };
enum { B_OPEN, B_F0, B_Q_LESS = B_F0 + 6, B_Q_MORE, B_S_LESS, B_S_MORE, B_E_LESS, B_E_MORE, B_SOUND, B_NAME, B_CONVERT, B_CLOSE, BUTTONS };
static struct place at[BUTTONS];
static const char *labels[BUTTONS] = { "Open...", "", "", "", "", "", "", "-", "+", "-", "+", "-", "+", "Sound", "Name...", "Convert", "Close" };
static int shown_formats[6], shown_count;
static const int scales[] = { 10, 25, 33, 50, 75, 100, 150, 200 };

static void
layout(void)
{
	static const int picture_formats[] = { F_JPEG, F_PNG, F_GIF, F_BMP, F_PPM, F_TGA };
	static const int sound_formats[] = { F_WAV, F_FLAC };
	static const int film_formats[] = { F_AVI, F_GIF, F_WAV, F_FLAC, F_JPEG, F_PNG };
	const int *list = src.what == S_SOUND ? sound_formats : src.what == S_FILM ? film_formats : picture_formats;
	int k;

	shown_count = src.what == S_NONE ? 0 : src.what == S_SOUND ? 2 : 6;
	for (k = 0; k < shown_count; k++)
		shown_formats[k] = list[k];
	at[B_OPEN] = (struct place){ 10, 8, 76, 22 };
	for (k = 0; k < 6; k++)
		at[B_F0 + k] = (struct place){ 70 + k * 70, 76, 64, 22 };
	at[B_Q_LESS] = (struct place){ 80, 110, 22, 20 }, at[B_Q_MORE] = (struct place){ 190, 110, 22, 20 };
	at[B_S_LESS] = (struct place){ 300, 110, 22, 20 }, at[B_S_MORE] = (struct place){ 466, 110, 22, 20 };
	at[B_E_LESS] = (struct place){ 80, 140, 22, 20 }, at[B_E_MORE] = (struct place){ 190, 140, 22, 20 };
	at[B_SOUND] = (struct place){ 252, 140, 100, 20 };
	at[B_NAME] = (struct place){ 414, 176, 76, 22 };
	at[B_CONVERT] = (struct place){ 300, 282, 94, 26 }, at[B_CLOSE] = (struct place){ 400, 282, 90, 26 };
}

static int
uses_quality(void)
{
	return job.to == F_JPEG || job.to == F_PNG || job.to == F_AVI;
}

static int
uses_size(void)
{
	return src.what == S_PICTURE || (src.what == S_FILM && job.to != F_WAV && job.to != F_FLAC);
}

static int
uses_every(void)
{
	return src.what == S_FILM && (job.to == F_GIF || job.to <= F_TGA);
}

static int
shown(int b)
{
	if (b >= B_F0 && b < B_F0 + 6)
		return b - B_F0 < shown_count;
	if (b == B_Q_LESS || b == B_Q_MORE)
		return uses_quality();
	if (b == B_S_LESS || b == B_S_MORE)
		return uses_size();
	if (b == B_E_LESS || b == B_E_MORE)
		return uses_every();
	if (b == B_SOUND)
		return src.what == S_FILM && job.to == F_AVI && src.rate;
	if (b == B_NAME || b == B_CONVERT)
		return src.what != S_NONE && !busy;
	return 1;
}

static void
draw_bar(void)
{
	int bw = WIN_W - 20;

	ui_bevel(window, 10, 214, bw, 18, 1);
	ui_fill(window, 11, 215, bw - 2, 16, WHITE);
	if (bar_share)
		ui_fill(window, 11, 215, (int)((bw - 2) * (unsigned long long)bar_share / 1000), 16, UI_SELECTED);
	ui_fill(window, 10, 238, WIN_W - 20, 36, UI_FACE);
	ui_text_fit(window, 10, 240, WIN_W - 20, busy ? bar_line : status, BLACK);
}

static void
draw(void)
{
	char text[200];
	int k, w, h;

	layout();
	ui_fill(window, 0, 0, WIN_W, WIN_H, UI_FACE);
	for (k = 0; k < BUTTONS; k++) {
		const char *label = labels[k];
		int pressed = 0;

		if (!shown(k))
			continue;
		if (k >= B_F0 && k < B_F0 + 6) {
			int f = shown_formats[k - B_F0];

			label = src.what == S_FILM && f <= F_TGA && f != F_GIF ? (f == F_JPEG ? "JPEGs" : "PNGs") : format_names[f];
			pressed = f == job.to;
		}
		if (k == B_SOUND) {
			snprintf(text, sizeof text, "Sound: %s", job.sound ? "with" : "none");
			label = text;
		}
		ui_button(window, at[k].x, at[k].y, at[k].w, at[k].h, label, pressed);
	}
	ui_text_fit(window, 96, 12, WIN_W - 106, in_path[0] ? leaf_of(in_path) : "Open a picture, a sound or a film", BLACK);
	{
		/* what the file is, on two lines when one is too short: broken after the last comma that fits */
		char line[200], *cut = NULL, *comma;
		int whole, tw, th, tb;

		snprintf(line, sizeof line, "%s", source_line());
		GrSetGCFont(ui_gc, ui_var);
		GrGetGCTextSize(ui_gc, line, -1, GR_TFASCII, &whole, &th, &tb);
		for (comma = line; whole > WIN_W - 20 && (comma = strstr(comma, ", ")) != NULL; comma += 2) {
			GrGetGCTextSize(ui_gc, line, (int)(comma - line + 1), GR_TFASCII, &tw, &th, &tb);
			if (tw > WIN_W - 20)
				break;
			cut = comma;
		}
		if (cut) {
			*cut = 0;
			ui_text_fit(window, 10, 56, WIN_W - 20, cut + 2, BLACK);
		}
		ui_text_fit(window, 10, 38, WIN_W - 20, line, BLACK);
	}
	if (src.what == S_NONE)
		goto rest;
	ui_text(window, 10, 80, "Make", -1, BLACK, 0);
	if (uses_quality()) {
		ui_text(window, 10, 113, job.to == F_PNG ? "Level" : "Quality", -1, BLACK, 0);
		snprintf(text, sizeof text, "%d", job.to == F_PNG ? job.level : job.quality);
		ui_text(window, 120, 113, text, -1, BLACK, 0);
	}
	if (uses_size()) {
		size_made(&w, &h);
		ui_text(window, 250, 113, "Size", -1, BLACK, 0);
		snprintf(text, sizeof text, "%d%%: %d x %d", job.scale, w, h);
		ui_text(window, 330, 113, text, -1, BLACK, 0);
	}
	if (uses_every()) {
		ui_text(window, 10, 143, "Every", -1, BLACK, 0);
		snprintf(text, sizeof text, "%d%s", job.every, job.every == 1 ? " (all)" : "");
		ui_text(window, 112, 143, text, -1, BLACK, 0);
	}
	ui_text(window, 10, 180, "Save as", -1, BLACK, 0);
	ui_fill(window, 70, 176, 338, 22, WHITE);
	ui_bevel(window, 70, 176, 338, 22, 1);
	if (many_pictures())
		numbered(text, sizeof text, leaf_of(out_path), 1), strncat(text, " ...", sizeof text - strlen(text) - 1);
	else
		snprintf(text, sizeof text, "%s", out_path);
	ui_text_fit(window, 74, 180, 330, text, BLACK);
rest:
	draw_bar();
	GrFlush();
}

static void
print_buttons(void)
{
	int k;

	if (!getenv("NXCONVERT_PRINT"))
		return;
	layout();
	for (k = 0; k < BUTTONS; k++) {
		const char *label = labels[k];

		if (!shown(k))
			continue;
		if (k >= B_F0 && k < B_F0 + 6)
			label = format_names[shown_formats[k - B_F0]];
		if (k == B_Q_LESS || k == B_Q_MORE)
			label = k == B_Q_LESS ? "quality-" : "quality+";
		if (k == B_S_LESS || k == B_S_MORE)
			label = k == B_S_LESS ? "size-" : "size+";
		if (k == B_E_LESS || k == B_E_MORE)
			label = k == B_E_LESS ? "every-" : "every+";
		printf("nxconvert: button %s at %d,%d\n", label, at[k].x + at[k].w / 2, at[k].y + at[k].h / 2);
	}
	printf("nxconvert: %s: %s; out %s\n", in_path[0] ? in_path : "(none)", source_line(), out_path);
	fflush(stdout);
}

/* The window's events while the work goes on: drawn when uncovered; closed, the work stops. */
static void
gui_progress(unsigned done, unsigned total, const char *what)
{
	GR_EVENT event;
	unsigned share = total ? (unsigned)((unsigned long long)done * 1000 / total) : bar_share, t = now_ms();

	for (;;) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_NONE)
			break;
		if (event.type == GR_EVENT_TYPE_EXPOSURE)
			bar_drawn_at = t - 1000;
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			cancelled = closing = 1;
		if (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == MWKEY_ESCAPE)
			cancelled = 1;
	}
	if (share < bar_share && total)
		share = bar_share;	/* (stages after one another keep the bar where it was) */
	if (share == bar_share && t - bar_drawn_at < 500)
		return;
	if (t - bar_drawn_at < 100 && share - bar_share < 20)
		return;
	bar_share = share;
	snprintf(bar_line, sizeof bar_line, "%s: %u.%u%% (Esc stops)", what, share / 10, share % 10);
	if (t - bar_drawn_at >= 1000)
		draw();
	else
		draw_bar(), GrFlush();
	bar_drawn_at = t;
}

static void
run_convert(void)
{
	const char *why;
	unsigned t0 = now_ms();

	busy = 1;
	bar_share = 0;
	bar_drawn_at = 0;
	snprintf(bar_line, sizeof bar_line, "starting");
	draw();
	progress_hook = gui_progress;
	why = convert();
	progress_hook = NULL;
	busy = 0;
	if (why) {
		bar_share = 0;
		snprintf(status, sizeof status, "Not made: %s", why);
	} else {
		unsigned ms = now_ms() - t0;

		bar_share = 1000;
		if (many_pictures())
			snprintf(status, sizeof status, "Made %d pictures, %u bytes in %u.%u s", atoi(made), made_bytes, ms / 1000, ms % 1000 / 100);
		else
			snprintf(status, sizeof status, "Made %s, %u bytes in %u.%u s", leaf_of(out_path), made_bytes, ms / 1000, ms % 1000 / 100);
		if (getenv("NXCONVERT_PRINT"))
			printf("nxconvert: made %s, %u bytes, sum %08x\n", made, made_bytes, made_sum), fflush(stdout);
	}
	if (!why || !named)
		default_out(), named = 0;	/* (a second Convert makes another file, never this one again) */
	draw();
}

/* The first format the file can be made into, when the one chosen cannot. */
static void
format_fits(int kind_was)
{
	layout();
	/* another kind of file: the first it can make (an AVI of a film, a PNG of a JPEG, a JPEG of another picture) */
	if (shown_count && (!can_make(job.to) || src.what != kind_was))
		job.to = shown_formats[0];
	if (src.what == S_PICTURE && job.to == F_JPEG && !strcmp(src.kind, "JPEG"))
		job.to = F_PNG;
	if (src.what == S_SOUND && !can_make(job.to))
		job.to = F_WAV;
}

static void
take_file(const char *path)
{
	int kind_was = src.what;
	const char *why = source_open(path);

	status[0] = 0;
	if (why)
		snprintf(status, sizeof status, "%s", why);
	format_fits(kind_was);
	named = 0;
	default_out();
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw();
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		ui_files_stop = closing = 1;
}

static void
open_file(void)
{
	char chosen[600];
	const char *start = "/root";

	if (ui_choose_file("Open a file to convert", start,
			   ".jpg .jpeg .png .gif .bmp .tga .ppm .pgm .pbm .pnm .wav .flac .mp3 .ogg .oga .mod .mpg .mpeg .avi .vqa", chosen,
			   sizeof chosen, others))
		take_file(chosen);
	draw();
	print_buttons();
}

static void
choose_name(void)
{
	char chosen[700], folder[600];
	const char *slash = strrchr(out_path, '/');

	snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - out_path) : 1, slash ? out_path : "/");
	if (ui_save_file("Save the converted file as", folder[0] ? folder : "/", endings[job.to], leaf_of(out_path), chosen, sizeof chosen,
			 others)) {
		ui_files_ending(chosen, sizeof chosen, endings[job.to]);
		snprintf(out_path, sizeof out_path, "%s", chosen);
		named = 1;
		job.overwrite = 1;	/* (the save window asked about a file that is there) */
	}
	draw();
}

static void
press(int b)
{
	int k;

	status[0] = 0;
	if (b >= B_F0 && b < B_F0 + 6) {
		job.to = shown_formats[b - B_F0];
		if (src.what == S_FILM && job.to == F_GIF && job.scale == 100 && job.every == 1)
			job.scale = 50, job.every = 2;	/* (an animation of every picture at full size is seldom wanted, and large) */
		job.overwrite = 0;
		if (!named)
			default_out();
		else
			ui_files_ending(out_path, sizeof out_path, endings[job.to]);
	} else if (b == B_Q_LESS || b == B_Q_MORE) {
		int d = b == B_Q_MORE ? 1 : -1;

		if (job.to == F_PNG)
			job.level = job.level + d < 1 ? 1 : job.level + d > 9 ? 9 : job.level + d;
		else
			job.quality = job.quality + 5 * d < 5 ? 5 : job.quality + 5 * d > 100 ? 100 : job.quality + 5 * d;
	} else if (b == B_S_LESS || b == B_S_MORE) {
		for (k = 0; k < (int)(sizeof scales / sizeof scales[0]) - 1 && scales[k] < job.scale; k++)
			;
		k += b == B_S_MORE ? (scales[k] == job.scale) : -1;
		k = k < 0 ? 0 : k >= (int)(sizeof scales / sizeof scales[0]) ? (int)(sizeof scales / sizeof scales[0]) - 1 : k;
		job.scale = scales[k];
	} else if (b == B_E_LESS || b == B_E_MORE) {
		job.every += b == B_E_MORE ? 1 : -1;
		job.every = job.every < 1 ? 1 : job.every > 100 ? 100 : job.every;
	} else if (b == B_SOUND) {
		job.sound = !job.sound;
	} else if (b == B_OPEN) {
		open_file();
		return;
	} else if (b == B_NAME) {
		choose_name();
		return;
	} else if (b == B_CONVERT) {
		run_convert();
		return;
	} else if (b == B_CLOSE) {
		closing = 1;
		return;
	}
	draw();
}

static int
window_main(int argc, char **argv)
{
	GR_EVENT event;
	int k;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxconvert: no window system (nxconvert --to FORMAT IN OUT works without one)\n");
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);
	ui_init();
	window = GrNewWindowEx(UI_FIXED, "Converter", GR_ROOT_WINDOW_ID, -1, -1, WIN_W, WIN_H, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	if (argc > 1) {
		/* what the file is, read with the window framed and saying so (docs/system/nanox.md) */
		ui_framed();
		snprintf(in_path, sizeof in_path, "%s", argv[1]);
		snprintf(status, sizeof status, "Reading %s", leaf_of(argv[1]));
		draw();
		take_file(argv[1]);
	}
	draw();
	print_buttons();
	while (!closing) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window) {
				ui_restyle();
				draw();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid != window || !(event.button.changebuttons & GR_BUTTON_L))
				break;
			layout();
			for (k = 0; k < BUTTONS; k++)
				if (shown(k) && ui_inside(event.button.x, event.button.y, at[k].x, at[k].y, at[k].w, at[k].h)) {
					press(k);
					break;
				}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.ch == MWKEY_ESCAPE)
				closing = 1;
			else if (event.keystroke.ch == MWKEY_ENTER && shown(B_CONVERT))
				press(B_CONVERT);
			else if (event.keystroke.ch == 'o' || event.keystroke.ch == 'O')
				press(B_OPEN);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			closing = 1;
			break;
		}
	}
	GrClose();
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc > 2 && !strcmp(argv[1], "--info")) {
		const char *why = source_open(argv[2]);

		printf("nxconvert: %s: %s\n", argv[2], why ? why : source_line());
		return why != NULL;
	}
	if (argc > 3 && !strncmp(argv[1], "--", 2))
		return command_line(argc, argv);
	return window_main(argc, argv);
}
