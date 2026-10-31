/* The registry: what every program of the desktop wants kept between sessions, a line a key
 * ("files.details=1") in one file of the folder the host keeps (docs/system/persist.md). A key is
 * the program's word, a dot, and its own name; a value is text without a line's end.
 * Any program may read any key; nxreg is the same for a script. */
#ifndef SHADEREMU_APPS_REGISTRY_H
#define SHADEREMU_APPS_REGISTRY_H

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define REG_FOLDER "/persistence"
#define REG_FILE REG_FOLDER "/registry"
#define REG_MOST 16384		/* bytes of it: more is not read, and a set that would pass it is not made */

static char reg_text[REG_MOST + 1];
static int reg_length = -1;	/* -1: not read yet */
static struct timespec reg_time;

static void
reg_load(void)
{
	struct stat info;
	int fd = open(REG_FILE, O_RDONLY), n = 0, got;

	memset(&reg_time, 0, sizeof reg_time);
	if (fd >= 0) {
		if (fstat(fd, &info) == 0)
			reg_time = info.st_mtim;
		while (n < REG_MOST && (got = (int)read(fd, reg_text + n, (size_t)(REG_MOST - n))) > 0)
			n += got;
		close(fd);
	}
	reg_text[n] = 0;
	reg_length = n;
}

/* True when another program has written the registry since this one last read it: it is read again. */
static int
reg_changed(void)
{
	struct stat info;

	if (reg_length >= 0 && stat(REG_FILE, &info) == 0 && info.st_mtim.tv_sec == reg_time.tv_sec && info.st_mtim.tv_nsec == reg_time.tv_nsec)
		return 0;
	if (reg_length >= 0 && stat(REG_FILE, &info) != 0 && reg_time.tv_sec == 0 && reg_time.tv_nsec == 0)
		return 0;
	reg_load();
	return 1;
}

/* Where a key's line begins in the text read, or NULL. */
static char *
reg_line(const char *key)
{
	size_t n = strlen(key);
	char *at;

	if (reg_length < 0)
		reg_load();
	for (at = reg_text; *at; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : at + strlen(at))
		if (!strncmp(at, key, n) && at[n] == '=')
			return at;
	return NULL;
}

/* A key's value, or `otherwise` when it has none. The text is good until the fourth call after. */
static const char *
reg_get(const char *key, const char *otherwise)
{
	static char values[4][256];
	static int turn;
	const char *at = reg_line(key);
	size_t n;

	if (!at)
		return otherwise;
	at += strlen(key) + 1;
	n = strcspn(at, "\n");
	turn = (turn + 1) & 3;
	memcpy(values[turn], at, n < 255 ? n : 255);
	values[turn][n < 255 ? n : 255] = 0;
	return values[turn];
}

static int
reg_int(const char *key, int otherwise)
{
	const char *value = reg_get(key, NULL);

	return value ? atoi(value) : otherwise;
}

/* Gives a key a value (NULL: the key is taken out), in the file at once: under a lock, from
 * the file as it is now, so that two programs' keys do not undo each other. False when the
 * folder cannot be written; the program goes on with what it has. */
static int
reg_set(const char *key, const char *value)
{
	static char out[REG_MOST + 1];
	int lock, fd, n = 0, fine = 0;
	char *at, *line;
	size_t k = strlen(key), v = value ? strcspn(value, "\n") : 0;

	mkdir(REG_FOLDER, 0777);
	lock = open(REG_FILE ".lock", O_CREAT | O_RDWR, 0666);
	if (lock < 0)
		return 0;
	flock(lock, LOCK_EX);
	reg_load();
	line = reg_line(key);
	for (at = reg_text; *at;) {
		size_t length = strcspn(at, "\n") + (strchr(at, '\n') != NULL);

		if (at != line)
			memcpy(out + n, at, length), n += (int)length;
		at += length;
	}
	if (n && out[n - 1] != '\n')
		out[n++] = '\n';
	if (value && n + k + v + 2 > REG_MOST) {	/* (the old value stays: nothing is lost by a set that does not fit) */
		flock(lock, LOCK_UN);
		close(lock);
		return 0;
	}
	if (value) {
		memcpy(out + n, key, k), n += (int)k;
		out[n++] = '=';
		memcpy(out + n, value, v), n += (int)v;
		out[n++] = '\n';
	}
	/* (written beside it and put in its place: no reader finds half a file) */
	fd = open(REG_FILE ".new", O_CREAT | O_TRUNC | O_WRONLY, 0666);
	if (fd >= 0) {
		fine = write(fd, out, (size_t)n) == n;
		close(fd);
		fine = fine && rename(REG_FILE ".new", REG_FILE) == 0;
	}
	reg_load();
	flock(lock, LOCK_UN);
	close(lock);
	return fine;
}

static int
reg_set_int(const char *key, int value)
{
	char text[16];

	snprintf(text, sizeof text, "%d", value);
	return reg_set(key, text);
}

#endif
