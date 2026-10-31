/*
 * The browser's own HTTP over Linux's sockets (docs/apps/web.md, "Addresses the machine reaches itself"):
 * where an address goes, a request that never blocks, an answer read as it comes (headers, a length,
 * chunks or the connection's end), and the cookies. Nothing of the window system: tools/web_net_test.c
 * tries it on the host.
 *
 * A fetch is a state moved on by net_poll() whenever the program has time. No call waits: connect,
 * send and read are the socket's non-blocking ones, and a read takes NET_READ bytes at most.
 */
#ifndef WEB_NET_H
#define WEB_NET_H
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NET_READ 16384		/* bytes a read takes: 16 KB is under 3 ms of the first core at 3 MIPS, copied twice */
#define NET_HEAD_MOST 8192
#define NET_CONNECT_MS 15000	/* SYN sent 1, 2, 4 and 8 s later: a machine that is there answers one of them */
#define NET_ANSWER_MS 30000	/* from the request to the first byte; and between two bytes of the answer */
#define NET_RETRY_MS 350	/* a refused connection is tried again this much later: a shell's `nc -l` loop is not */
#define NET_RETRIES 3		/* listening between two answers */

enum { NET_IDLE, NET_RESOLVE, NET_WAIT, NET_CONNECT, NET_SEND, NET_HEAD, NET_HEADED, NET_BODY, NET_DONE, NET_FAILED };
enum { ROUTE_HOST, ROUTE_DIRECT, ROUTE_NONE };
enum { NK_HTML, NK_TEXT, NK_IMAGE, NK_SAVE, NK_SNIFF };

struct net_fetch {
	int state, fd, out;	/* `out`: the file the body is written to, or -1: memory */
	char host[128], address[256];
	int port, tries;
	uint32_t ip;		/* network order; 0: its name is being asked of the name server */
	long long deadline, retry_at, head_at;	/* net_ms() */
	/* the request */
	char *request;
	int request_n, sent;
	/* the answer */
	char head[NET_HEAD_MOST];
	int head_n, code, simple;	/* simple: no header at all, as a shell's `echo | nc -l` may send */
	char reason[48], type[64], charset[24], location[256], disposition[96];
	int chunked, no_store;
	long long length;	/* -1: not said */
	long long got;		/* bytes of the body so far, kept or not */
	char *body;
	int n, room, most;
	int cut;		/* more came than `most`: it was left there */
	int short_ended;	/* the connection ended before the length or the last chunk said */
	int idle_ended;		/* nothing came for NET_ANSWER_MS and it was taken as the end */
	char why[160];		/* FAILED: what went wrong, for a person */
	int no_name;		/* FAILED because no name server knows it: the host's fetch may */
	/* what is being read */
	unsigned char *buf;
	int left_at, left_n;	/* bytes of `buf` after the head, until the caller has said where they go */
	int chunk_state;	/* 0: the size's line, 1: a chunk's bytes, 2: the line after them, 3: the trailer */
	long long chunk_left;
	int chunk_ext;		/* the size's line is past its digits */
	int line_blank;
};

static long long
net_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- addresses ---- */

/* An http:// address's host, port and where its path begins ("": the root): 0 if it is not one. */
static int
net_split(const char *address, char *host, int room, int *port, const char **path)
{
	const char *at, *end, *colon, *user;
	int n;

	if (strncasecmp(address, "http://", 7))
		return 0;
	at = address + 7;
	end = at + strcspn(at, "/?#");
	/* (name:password@host: the name is no part of where it goes) */
	for (user = at; user < end; user++)
		if (*user == '@')
			at = user + 1;
	colon = memchr(at, ':', end - at);
	n = (int)((colon ? colon : end) - at);
	if (n <= 0 || n >= room)
		return 0;
	memcpy(host, at, n);
	host[n] = 0;
	for (int i = 0; i < n; i++)
		host[i] = tolower((unsigned char)host[i]);
	*port = 80;
	if (colon && colon + 1 < end) {
		*port = atoi(colon + 1);
		if (*port <= 0 || *port > 65535)
			return 0;
	}
	*path = end;
	return 1;
}

/* Dotted numbers: the address in network order, or 0. */
static uint32_t
net_literal(const char *host)
{
	struct in_addr a;

	return inet_pton(AF_INET, host, &a) == 1 ? a.s_addr : 0;
}

static int
net_hosts_lookup(const char *file, const char *host, uint32_t *ip)
{
	char text[8192], *line, *next;
	int fd = open(file, O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = (int)read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	for (line = text; line; line = next) {
		char *word, *keep;
		uint32_t a;

		if ((next = strchr(line, '\n')) != NULL)
			*next++ = 0;
		if ((word = strchr(line, '#')) != NULL)
			*word = 0;
		word = strtok_r(line, " \t\r", &keep);
		if (!word || !(a = net_literal(word)))
			continue;
		while ((word = strtok_r(NULL, " \t\r", &keep)) != NULL)
			if (!strcasecmp(word, host)) {
				*ip = a;
				return 1;
			}
	}
	return 0;
}

/* A name this machine knows without asking anybody: 127.0.0.1 for itself, and /etc/hosts. */
static uint32_t
net_name_here(const char *host)
{
	char me[64];
	uint32_t ip = 0;

	if (!strcasecmp(host, "localhost"))
		return htonl(0x7f000001);
	if (net_hosts_lookup("/etc/hosts", host, &ip))
		return ip;
	if (!gethostname(me, sizeof me) && !strcasecmp(me, host))
		return htonl(0x7f000001);
	return 0;
}

/* /etc/resolv.conf's first name server, or 0: the image has none unless somebody writes one. */
static uint32_t
net_name_server(void)
{
	char text[1024], *at;
	int fd = open("/etc/resolv.conf", O_RDONLY), n;

	if (fd < 0)
		return 0;
	n = (int)read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	for (at = text; (at = strstr(at, "nameserver")) != NULL; at += 10) {
		char word[40];

		if ((at == text || at[-1] == '\n') && sscanf(at + 10, " %39s", word) == 1 && net_literal(word))
			return net_literal(word);
	}
	return 0;
}

/* The addresses that are this machine's whatever a host could do: itself and its LAN (docs/system/lan.md). */
static int
net_own_range(uint32_t ip)
{
	uint32_t h = ntohl(ip);

	return (h >> 24) == 127 || (h >> 16) == 0x0a00;
}

/* Whether the kernel has a way to that address: asked with a socket that sends nothing. */
static int
net_has_route(uint32_t ip, int port)
{
	struct sockaddr_in to;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), fine;

	if (fd < 0)
		return 0;
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = ip;
	fine = !connect(fd, (struct sockaddr *)&to, sizeof to);
	close(fd);
	return fine;
}

/* Where an address goes. ROUTE_DIRECT: over this machine's own sockets (*ip the address, 0 when the
 * name server must be asked first). ROUTE_HOST: the host's fetch (docs/system/fetch.md): https, and
 * what the kernel has no way to. ROUTE_NONE: nothing can reach it, `why` says so. */
static int
net_route(const char *address, uint32_t *ip, char *why, int room)
{
	char host[128];
	const char *path;
	int port;

	*ip = 0;
	why[0] = 0;
	if (!strncasecmp(address, "https://", 8)) {
		char plain[256];

		/* (the host's fetch would open its own computer's localhost, or some real 10.0.x.x) */
		snprintf(plain, sizeof plain, "http://%s", address + 8);
		if (net_split(plain, host, sizeof host, &port, &path) && ((*ip = net_literal(host)) || (*ip = net_name_here(host))) && net_own_range(*ip)) {
			snprintf(why, room, "This machine has no TLS of its own, and %s is on its own network: open it with http://.", host);
			return ROUTE_NONE;
		}
		return ROUTE_HOST;
	}
	if (!net_split(address, host, sizeof host, &port, &path))
		return ROUTE_HOST;
	if (!(*ip = net_literal(host)) && !(*ip = net_name_here(host)))
		return net_name_server() ? ROUTE_DIRECT : ROUTE_HOST;
	if (net_has_route(*ip, port))
		return ROUTE_DIRECT;
	if (net_own_range(*ip)) {
		snprintf(why, room, "This machine has no way to %s: its network is not up (no link, docs/system/lan.md).", host);
		return ROUTE_NONE;
	}
	return ROUTE_HOST;
}

/* ---- cookies ---- */

#define COOKIES 64
#define COOKIE_FILE "/tmp/.nxweb-cookies"	/* so that the browser opened again in this session has them */

struct net_cookie {
	char host[64], path[64], name[48], value[200];
	long long expires;	/* time(); 0: until the machine stops */
	char domain, secure;	/* domain: the host's names under `host` too */
};
static struct net_cookie *net_cookies;	/* (made at the first request: the browser's start pays nothing) */
static int net_cookie_count;
static const char *net_cookie_file = COOKIE_FILE;

static void
net_cookies_read(void)
{
	FILE *file;
	char line[480];

	if (net_cookies)
		return;
	net_cookies = calloc(COOKIES, sizeof *net_cookies);
	if (!net_cookies || (file = fopen(net_cookie_file, "r")) == NULL)
		return;
	while (net_cookie_count < COOKIES && fgets(line, sizeof line, file)) {
		struct net_cookie *c = &net_cookies[net_cookie_count];
		int domain, secure;

		if (sscanf(line, "%63s %63s %47s %lld %d %d %199[^\n]", c->host, c->path, c->name, &c->expires, &domain, &secure, c->value) == 7) {
			c->domain = domain, c->secure = secure;
			net_cookie_count++;
		}
	}
	fclose(file);
}

static void
net_cookies_write(void)
{
	FILE *file = fopen(net_cookie_file, "w");
	int i;

	if (!file)
		return;
	for (i = 0; i < net_cookie_count; i++) {
		const struct net_cookie *c = &net_cookies[i];

		fprintf(file, "%s %s %s %lld %d %d %s\n", c->host, c->path, c->name, c->expires, c->domain, c->secure, c->value[0] ? c->value : "\"\"");
	}
	fclose(file);
}

/* Expires's date as time(), or -1 for what cannot be read as one. */
static long long
net_date(const char *text)
{
	static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
	char month[8];
	const char *at;
	int y, d, h, m, s;
	const char *comma = strchr(text, ',');
	long long days;

	if (sscanf(comma ? comma + 1 : text, " %d%*1[- ]%3s%*1[- ]%d %d:%d:%d", &d, month, &y, &h, &m, &s) != 6)
		return -1;
	for (int i = 0; i < 3; i++)
		month[i] = tolower((unsigned char)month[i]);
	month[3] = 0;
	if ((at = strstr(months, month)) == NULL || (at - months) % 3)
		return -1;
	y += y < 70 ? 2000 : y < 100 ? 1900 : 0;
	/* (days from 1970, as timegm has them: civil-from-days's inverse) */
	{
		int mo = (int)(at - months) / 3 + 1, a = (14 - mo) / 12, yy = y + 4800 - a, mm = mo + 12 * a - 3;

		days = d + (153 * mm + 2) / 5 + 365LL * yy + yy / 4 - yy / 100 + yy / 400 - 32045 - 2440588;
	}
	return days * 86400 + h * 3600 + m * 60 + s;
}

/* The path a cookie with no Path= is for: the request's folder. */
static void
net_default_path(const char *path, char *out, int room)
{
	int n = (int)strcspn(path, "?#");

	while (n > 1 && path[n - 1] != '/')
		n--;
	if (n > 1)
		n--;	/* (without its last slash) */
	if (n <= 0 || path[0] != '/') {
		snprintf(out, room, "/");
		return;
	}
	snprintf(out, room, "%.*s", n, path);
}

/* A Set-Cookie line of an answer from `host`, whose request was for `path`. */
static void
net_cookie_set(const char *host, const char *path, const char *line)
{
	struct net_cookie made;
	const char *eq, *end, *at;
	int i, n;

	net_cookies_read();
	if (!net_cookies)
		return;
	memset(&made, 0, sizeof made);
	end = line + strcspn(line, ";");
	eq = memchr(line, '=', end - line);
	if (!eq)
		return;
	for (at = line; at < eq && isspace((unsigned char)*at); at++)
		;
	n = (int)(eq - at);
	while (n > 0 && isspace((unsigned char)at[n - 1]))
		n--;
	if (n <= 0 || n >= (int)sizeof made.name)
		return;
	memcpy(made.name, at, n);
	for (at = eq + 1; at < end && isspace((unsigned char)*at); at++)
		;
	n = (int)(end - at);
	while (n > 0 && isspace((unsigned char)at[n - 1]))
		n--;
	if (n >= (int)sizeof made.value)
		return;	/* (not cut: half a session's key is no key) */
	memcpy(made.value, at, n);
	for (i = 0; i < n; i++)
		if ((unsigned char)made.value[i] < 32)
			return;
	snprintf(made.host, sizeof made.host, "%s", host);
	net_default_path(path, made.path, sizeof made.path);
	/* its attributes */
	for (at = end; *at == ';';) {
		char name[16], value[80];
		const char *stop, *is;
		int k;

		at++;
		while (isspace((unsigned char)*at))
			at++;
		stop = at + strcspn(at, ";");
		is = memchr(at, '=', stop - at);
		k = (int)((is ? is : stop) - at);
		while (k > 0 && isspace((unsigned char)at[k - 1]))
			k--;
		snprintf(name, sizeof name, "%.*s", k, at);
		value[0] = 0;
		if (is) {
			is++;
			while (is < stop && isspace((unsigned char)*is))
				is++;
			snprintf(value, sizeof value, "%.*s", (int)(stop - is), is);
		}
		if (!strcasecmp(name, "path") && value[0] == '/')
			snprintf(made.path, sizeof made.path, "%s", value);
		else if (!strcasecmp(name, "secure"))
			made.secure = 1;
		else if (!strcasecmp(name, "max-age"))
			made.expires = (long long)time(NULL) + (atoi(value) <= 0 ? -1000000 : atoi(value));
		else if (!strcasecmp(name, "expires") && !made.expires) {
			made.expires = net_date(value);
			made.expires = made.expires < 0 ? 0 : made.expires ? made.expires : 1;	/* (1970's first second: "forget it") */
		} else if (!strcasecmp(name, "domain") && value[0]) {
			const char *d = value + (value[0] == '.');
			int hl = (int)strlen(host), dl = (int)strlen(d);

			/* it must be this host or a name it is under, with a dot in it; never an address's digits */
			if (net_literal(host) || !strchr(d, '.') || dl > hl || strcasecmp(host + hl - dl, d) || (dl < hl && host[hl - dl - 1] != '.'))
				return;
			snprintf(made.host, sizeof made.host, "%s", d);
			made.domain = 1;
		}
		at = stop;
	}
	for (i = 0; i < net_cookie_count; i++)
		if (!strcasecmp(net_cookies[i].host, made.host) && !strcmp(net_cookies[i].path, made.path) && !strcmp(net_cookies[i].name, made.name))
			break;
	if (made.expires && made.expires < (long long)time(NULL)) {
		/* gone */
		if (i < net_cookie_count) {
			memmove(&net_cookies[i], &net_cookies[i + 1], sizeof *net_cookies * (--net_cookie_count - i));
			net_cookies_write();
		}
		return;
	}
	if (i == net_cookie_count) {
		if (net_cookie_count == COOKIES)
			memmove(&net_cookies[0], &net_cookies[1], sizeof *net_cookies * --net_cookie_count), i--;
		net_cookie_count++;
	}
	net_cookies[i] = made;
	net_cookies_write();
}

/* "a=1; b=2" for a request to that host and path (the longest paths first, as a server expects);
 * how many bytes it took. */
static int
net_cookie_header(const char *host, const char *path, char *out, int room)
{
	long long clock_now = (long long)time(NULL);
	int i, n = 0, done[COOKIES], most, pick;

	net_cookies_read();
	out[0] = 0;
	if (!net_cookies)
		return 0;
	memset(done, 0, sizeof done);
	for (;;) {
		for (i = 0, most = -1, pick = -1; i < net_cookie_count; i++) {
			const struct net_cookie *c = &net_cookies[i];
			int hl = (int)strlen(host), cl = (int)strlen(c->host), pl = (int)strlen(c->path);

			if (done[i] || c->secure || (c->expires && c->expires < clock_now))
				continue;
			if (strcasecmp(host, c->host) && !(c->domain && hl > cl && host[hl - cl - 1] == '.' && !strcasecmp(host + hl - cl, c->host)))
				continue;
			if (strncmp(path[0] ? path : "/", c->path, pl) || !(path[pl] == 0 || path[pl] == '/' || path[pl] == '?' || c->path[pl - 1] == '/' || (!path[0] && pl == 1)))
				continue;
			if (pl > most)
				most = pl, pick = i;
		}
		if (pick < 0)
			return n;
		done[pick] = 1;
		n += snprintf(out + n, room - n, "%s%s=%s", n ? "; " : "", net_cookies[pick].name, net_cookies[pick].value);
		if (n >= room) {
			out[n = room - 1] = 0;
			return n;
		}
	}
}

/* ---- an answer's bytes ---- */

static void
net_end(struct net_fetch *f, int state)
{
	if (f->fd >= 0)
		close(f->fd);
	f->fd = -1;
	if (f->out >= 0)
		close(f->out);
	f->out = -1;
	free(f->request);
	f->request = NULL;
	free(f->buf);
	f->buf = NULL;
	f->state = state;
}

static void
net_fail(struct net_fetch *f, const char *why)
{
	snprintf(f->why, sizeof f->why, "%s", why);
	net_end(f, NET_FAILED);
}

/* Bytes of the body as they are. */
static void
net_take(struct net_fetch *f, const unsigned char *p, int n)
{
	f->got += n;
	if (f->out >= 0) {
		while (n > 0) {
			int put = (int)write(f->out, p, n);

			if (put <= 0) {
				net_fail(f, "The file it was being saved to could not be written.");
				return;
			}
			p += put, n -= put;
		}
		return;
	}
	if (f->n + n > f->most) {
		f->cut = 1;
		n = f->most - f->n;
	}
	if (n <= 0)
		return;
	if (f->n + n + 1 > f->room) {
		int room = f->room ? f->room : 32768;
		char *more;

		while (room < f->n + n + 1)
			room *= 2;
		if (room > f->most + 1)
			room = f->most + 1;
		if ((more = realloc(f->body, room)) == NULL) {
			f->cut = 1;
			return;
		}
		f->body = more;
		f->room = room;
	}
	memcpy(f->body + f->n, p, n);
	f->n += n;
	f->body[f->n] = 0;
}

/* Bytes as they came: a length counted, or chunks taken apart, the end noticed. */
static void
net_feed(struct net_fetch *f, const unsigned char *p, int n)
{
	if (f->chunked) {
		while (n > 0 && f->state == NET_BODY) {
			if (f->chunk_state == 1) {
				int here = f->chunk_left < n ? (int)f->chunk_left : n;

				net_take(f, p, here);
				p += here, n -= here;
				if (!(f->chunk_left -= here))
					f->chunk_state = 2;
				continue;
			}
			{
				int c = *p++;

				n--;
				if (f->chunk_state == 2) {	/* the line ending a chunk */
					if (c == '\n')
						f->chunk_state = f->chunk_ext = 0, f->chunk_left = -1;
				} else if (f->chunk_state == 3) {	/* the trailer: up to an empty line */
					if (c == '\n') {
						if (f->line_blank) {
							net_end(f, NET_DONE);
							return;
						}
						f->line_blank = 1;
					} else if (c != '\r')
						f->line_blank = 0;
				} else if (c == '\n') {	/* a size's line ends */
					if (f->chunk_left <= 0) {
						f->chunk_state = 3;
						f->line_blank = 1;
					} else
						f->chunk_state = 1;
				} else if (!f->chunk_ext) {
					int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

					if (d < 0)
						f->chunk_ext = 1;	/* (the line's end, or an extension: skipped) */
					else if (f->chunk_left < (1LL << 40))
						f->chunk_left = (f->chunk_left < 0 ? 0 : f->chunk_left) * 16 + d;
				}
			}
		}
		return;
	}
	if (f->length >= 0 && f->got + n >= f->length) {
		net_take(f, p, (int)(f->length - f->got));
		if (f->state == NET_BODY)
			net_end(f, NET_DONE);
		return;
	}
	net_take(f, p, n);
}

/* The header lines (not the status line) from `line` on: what the browser wants of them. */
static void
net_headers(struct net_fetch *f, char *line, const char *path)
{
	char *next;

	for (; line && *line; line = next) {
		char *colon, *value, *end;

		if ((next = strchr(line, '\n')) != NULL)
			*next++ = 0;
		end = line + strlen(line);
		while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
			*--end = 0;
		if ((colon = strchr(line, ':')) == NULL)
			continue;
		*colon = 0;
		value = colon + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		if (!strcasecmp(line, "content-type")) {
			char *cs = strcasestr(value, "charset=");

			snprintf(f->type, sizeof f->type, "%.*s", (int)strcspn(value, "; \t"), value);
			for (char *c = f->type; *c; c++)
				*c = tolower((unsigned char)*c);
			if (cs) {
				cs += 8 + (cs[8] == '"');
				snprintf(f->charset, sizeof f->charset, "%.*s", (int)strcspn(cs, "\"; \t"), cs);
			}
		} else if (!strcasecmp(line, "content-length")) {
			f->length = atoll(value);
		} else if (!strcasecmp(line, "transfer-encoding")) {
			f->chunked = strcasestr(value, "chunked") != NULL;
		} else if (!strcasecmp(line, "location")) {
			snprintf(f->location, sizeof f->location, "%s", value);
		} else if (!strcasecmp(line, "content-disposition")) {
			snprintf(f->disposition, sizeof f->disposition, "%s", value);
		} else if (!strcasecmp(line, "cache-control")) {
			f->no_store |= strcasestr(value, "no-store") != NULL || strcasestr(value, "no-cache") != NULL;
		} else if (!strcasecmp(line, "set-cookie")) {
			net_cookie_set(f->host, path, value);
		}
	}
	if (f->chunked)
		f->length = -1;
}

/* Bytes of the head. Its end leaves the state at NET_HEADED and what is after it in `left`. */
static void
net_head_feed(struct net_fetch *f, int from, int n, const char *path)
{
	unsigned char *p = f->buf + from;
	int i;

	for (i = 0; i < n; i++) {
		char *line;

		if (f->head_n == 5 && strncmp(f->head, "HTTP/", 5)) {
			/* no head at all: what came is the page, as HTTP/0.9 had it */
			break;
		}
		if (f->head_n == NET_HEAD_MOST - 1) {
			net_fail(f, "The server's headers are longer than this browser reads.");
			return;
		}
		f->head[f->head_n++] = p[i];
		if (p[i] != '\n' || (f->head_n >= 2 && f->head[f->head_n - 2] != '\n' && !(f->head_n >= 3 && f->head[f->head_n - 2] == '\r' && f->head[f->head_n - 3] == '\n')))
			continue;
		if (f->head_n < 3)
			continue;	/* (a line break before the status line) */
		f->head[f->head_n] = 0;
		line = strchr(f->head, ' ');
		f->code = line ? atoi(line + 1) : 0;
		if (f->code >= 100 && f->code < 200) {
			/* "go on": the real one is next */
			f->head_n = 0;
			continue;
		}
		if (line) {
			char *reason = strchr(line + 1, ' '), *eol = strchr(f->head, '\n');

			if (reason && eol && reason < eol)
				snprintf(f->reason, sizeof f->reason, "%.*s", (int)(eol - reason - 1 - (eol[-1] == '\r')), reason + 1);
			net_headers(f, eol ? eol + 1 : NULL, path);
		}
		f->left_at = from + i + 1;
		f->left_n = n - i - 1;
		f->head_at = net_ms();
		f->state = NET_HEADED;
		return;
	}
	if (f->head_n >= 5 && strncmp(f->head, "HTTP/", 5)) {
		/* the bytes kept so far and these go back together */
		int kept = f->head_n, rest = n - i;

		memmove(f->buf + kept, p + i, rest);
		memcpy(f->buf, f->head, kept);
		f->left_at = 0;
		f->left_n = kept + rest;
		f->simple = 1;
		f->code = 200;
		f->head_at = net_ms();
		f->state = NET_HEADED;
	}
}

/* What the head says is the body's kind. */
static int
net_kind_of(const struct net_fetch *f)
{
	const char *t = f->type;

	if (!strncasecmp(f->disposition, "attachment", 10))
		return NK_SAVE;
	if (!t[0])
		return NK_SNIFF;
	if (!strcmp(t, "text/html") || !strcmp(t, "application/xhtml+xml"))
		return NK_HTML;
	if (!strncmp(t, "image/", 6))
		return strstr(t, "svg") ? NK_TEXT : NK_IMAGE;
	if (!strncmp(t, "text/", 5) || strstr(t, "json") || strstr(t, "xml") || strstr(t, "javascript") || !strcmp(t, "application/x-sh") ||
	    !strcmp(t, "application/x-www-form-urlencoded"))
		return NK_TEXT;
	return NK_SAVE;
}

/* The kind of bytes nobody said the kind of. */
static int
net_sniff(const char *p, int n)
{
	int i, odd = 0, look = n < 1024 ? n : 1024;

	for (i = 0; i < look && isspace((unsigned char)p[i]); i++)
		;
	if (i < look && p[i] == '<' && i + 1 < look && (isalpha((unsigned char)p[i + 1]) || p[i + 1] == '!' || p[i + 1] == '?'))
		return NK_HTML;
	for (i = 0; i < look; i++) {
		unsigned char c = p[i];

		if (!c)
			return NK_SAVE;
		odd += c < 32 && c != '\n' && c != '\r' && c != '\t' && c != 27 && c != 12 && c != 8;
	}
	return odd * 10 > look ? NK_SAVE : NK_TEXT;
}

/* ---- the request ---- */

static int
net_open(struct net_fetch *f)
{
	struct sockaddr_in to;

	f->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (f->fd < 0 || fcntl(f->fd, F_SETFL, O_NONBLOCK) < 0) {
		net_fail(f, "This machine could not make a socket.");
		return 0;
	}
	memset(&to, 0, sizeof to);
	to.sin_family = AF_INET;
	to.sin_port = htons(f->port);
	to.sin_addr.s_addr = f->ip;
	f->state = NET_CONNECT;
	f->deadline = net_ms() + NET_CONNECT_MS;
	if (connect(f->fd, (struct sockaddr *)&to, sizeof to) && errno != EINPROGRESS)
		return errno;
	return 0;
}

static void
net_connect_failed(struct net_fetch *f, int e)
{
	char why[160], ip[20];
	struct in_addr a;

	a.s_addr = f->ip;
	inet_ntop(AF_INET, &a, ip, sizeof ip);
	if (e == ECONNREFUSED && ++f->tries < NET_RETRIES) {
		close(f->fd);
		f->fd = -1;
		f->state = NET_WAIT;
		f->retry_at = net_ms() + NET_RETRY_MS;
		return;
	}
	if (e == ECONNREFUSED)
		snprintf(why, sizeof why, "%s (%s) refused the connection: nothing listens on its port %d.", f->host, ip, f->port);
	else if (e == ETIMEDOUT)
		snprintf(why, sizeof why, "No answer from %s (%s) on port %d in %d seconds: is a machine there?", f->host, ip, f->port, NET_CONNECT_MS / 1000);
	else if (e == ENETUNREACH || e == EHOSTUNREACH)
		snprintf(why, sizeof why, "This machine has no way to %s (%s).", f->host, ip);
	else
		snprintf(why, sizeof why, "The connection to %s (%s) on port %d failed: %s.", f->host, ip, f->port, strerror(e));
	net_fail(f, why);
}

/* A name server's question for `host`, type A. */
static int
net_dns_query(const char *host, unsigned char *q, int id)
{
	int n = 12;
	const char *at = host;

	memset(q, 0, 12);
	q[0] = id >> 8, q[1] = id;
	q[2] = 1;	/* recursion, please */
	q[5] = 1;
	while (*at) {
		int k = (int)strcspn(at, ".");

		if (k > 63 || n + k + 6 > 300)
			return 0;
		q[n++] = k;
		memcpy(q + n, at, k);
		n += k;
		at += k + (at[k] == '.');
	}
	q[n++] = 0;
	q[n++] = 0, q[n++] = 1, q[n++] = 0, q[n++] = 1;
	return n;
}

/* The first A record of an answer, or 0. */
static uint32_t
net_dns_answer(const unsigned char *a, int n, int id)
{
	int i = 12, count, k;

	if (n < 12 || a[0] != (id >> 8 & 255) || a[1] != (id & 255) || (a[3] & 15))
		return 0;
	count = a[6] << 8 | a[7];
	/* past the question */
	while (i < n && a[i])
		i += a[i] >= 192 ? 1 : a[i] + 1;
	i += 5;
	for (k = 0; k < count && i < n; k++) {
		int type, len;

		while (i < n && a[i]) {
			if (a[i] >= 192) {
				i++;
				break;
			}
			i += a[i] + 1;
		}
		i++;
		if (i + 10 > n)
			return 0;
		type = a[i] << 8 | a[i + 1];
		len = a[i + 8] << 8 | a[i + 9];
		i += 10;
		if (type == 1 && len == 4 && i + 4 <= n) {
			uint32_t ip;

			memcpy(&ip, a + i, 4);
			return ip;
		}
		i += len;
	}
	return 0;
}

/* Starts a fetch. `method` NULL is GET; `body`, `type` what a POST sends. `ip` from net_route. */
static void
net_start(struct net_fetch *f, const char *address, uint32_t ip, const char *method, const char *body, int body_n, const char *type)
{
	const char *path;
	char cookie[1024], where[300];
	int room, n, i, k;

	free(f->body);
	memset(f, 0, offsetof(struct net_fetch, head));
	f->head_n = 0;
	f->fd = f->out = -1;
	f->length = -1;
	f->chunk_left = -1;
	f->code = f->simple = f->chunked = f->no_store = 0;
	f->reason[0] = f->type[0] = f->charset[0] = f->location[0] = f->disposition[0] = 0;
	f->got = 0;
	f->body = NULL;
	f->n = f->room = f->most = f->cut = f->short_ended = f->idle_ended = f->no_name = 0;
	f->why[0] = 0;
	f->left_at = f->left_n = f->chunk_state = f->chunk_ext = f->line_blank = 0;
	f->buf = NULL;
	f->request = NULL;
	snprintf(f->address, sizeof f->address, "%s", address);
	if (!net_split(address, f->host, sizeof f->host, &f->port, &path)) {
		net_fail(f, "That is not an address of the web.");
		return;
	}
	/* the path as a server wants it: no #fragment, no raw 8-bit bytes */
	for (i = k = 0; path[i] && path[i] != '#' && k < (int)sizeof where - 4; i++) {
		unsigned char c = path[i];

		if (c >= 0x7f || c <= ' ')
			k += sprintf(where + k, "%%%02X", c);
		else
			where[k++] = c;
	}
	where[k] = 0;
	if (where[0] != '/') {
		/* ("http://host?x=1") */
		memmove(where + 1, where, k + 1);
		where[0] = '/';
	}
	net_cookie_header(f->host, where, cookie, sizeof cookie);
	room = 512 + (int)strlen(where) + (int)strlen(cookie) + body_n + (type ? (int)strlen(type) : 0);
	f->request = malloc(room);
	f->buf = malloc(NET_READ + 16);
	if (!f->request || !f->buf) {
		net_fail(f, "This machine has no memory left for the request.");
		return;
	}
	n = snprintf(f->request, room, "%s %s HTTP/1.1\r\nHost: %s", method ? method : "GET", where, f->host);
	if (f->port != 80)
		n += snprintf(f->request + n, room - n, ":%d", f->port);
	n += snprintf(f->request + n, room - n, "\r\nUser-Agent: nxweb/1.0 (ShaderEmu)\r\nAccept: */*\r\nConnection: close\r\n");
	if (cookie[0])
		n += snprintf(f->request + n, room - n, "Cookie: %s\r\n", cookie);
	if (method && strcmp(method, "GET"))
		n += snprintf(f->request + n, room - n, "Content-Type: %s\r\nContent-Length: %d\r\n", type ? type : "application/x-www-form-urlencoded", body_n);
	n += snprintf(f->request + n, room - n, "\r\n");
	if (body_n > 0) {
		memcpy(f->request + n, body, body_n);
		n += body_n;
	}
	f->request_n = n;
	f->ip = ip;
	if (!ip) {
		/* a question to the name server, over a socket of its own that `fd` holds meanwhile */
		struct sockaddr_in to;
		unsigned char q[320];
		int id = (int)(net_ms() & 0x7fff) + 1, qn = net_dns_query(f->host, q, id);

		f->tries = id;
		f->fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		memset(&to, 0, sizeof to);
		to.sin_family = AF_INET;
		to.sin_port = htons(53);
		to.sin_addr.s_addr = net_name_server();
		if (f->fd < 0 || !qn || connect(f->fd, (struct sockaddr *)&to, sizeof to) || send(f->fd, q, qn, 0) != qn) {
			f->no_name = 1;
			net_fail(f, "The name server could not be asked.");
			return;
		}
		f->state = NET_RESOLVE;
		f->deadline = net_ms() + 2500;
		return;
	}
	if ((i = net_open(f)))
		net_connect_failed(f, i);
}

/* Moves a fetch on as far as it goes without waiting; its state. */
static int
net_poll(struct net_fetch *f)
{
	long long clock_now = net_ms();
	struct pollfd pf;
	int e, n;

	switch (f->state) {
	case NET_RESOLVE:
		n = (int)recv(f->fd, f->buf, NET_READ, 0);
		if (n > 0 && (f->ip = net_dns_answer(f->buf, n, f->tries))) {
			close(f->fd);
			f->fd = -1;
			f->tries = 0;
			if ((e = net_open(f)))
				net_connect_failed(f, e);
			return f->state;
		}
		if (n > 0 || (n < 0 && errno != EAGAIN) || clock_now > f->deadline) {
			f->no_name = 1;
			net_fail(f, "The name server does not know that name.");
		}
		return f->state;
	case NET_WAIT:
		if (clock_now >= f->retry_at && (e = net_open(f)))
			net_connect_failed(f, e);
		return f->state;
	case NET_CONNECT:
		pf.fd = f->fd, pf.events = POLLOUT, pf.revents = 0;
		if (poll(&pf, 1, 0) <= 0) {
			if (clock_now > f->deadline)
				net_connect_failed(f, ETIMEDOUT);
			return f->state;
		}
		{
			socklen_t size = sizeof e;

			e = 0;
			getsockopt(f->fd, SOL_SOCKET, SO_ERROR, &e, &size);
			if (e) {
				net_connect_failed(f, e);
				return f->state;
			}
		}
		f->state = NET_SEND;
		/* fall through */
	case NET_SEND:
		n = (int)send(f->fd, f->request + f->sent, f->request_n - f->sent, MSG_NOSIGNAL);
		if (n < 0 && errno != EAGAIN) {
			/* (a server that answered without reading, closed already: its answer is still to be read) */
			f->sent = f->request_n;
		} else if (n > 0)
			f->sent += n;
		if (f->sent == f->request_n) {
			free(f->request);
			f->request = NULL;
			f->state = NET_HEAD;
			f->deadline = clock_now + NET_ANSWER_MS;
		}
		if (f->state != NET_HEAD)
			return f->state;
		/* fall through */
	case NET_HEAD:
		n = (int)read(f->fd, f->buf, NET_READ);
		if (n > 0) {
			const char *path;
			char host[128];
			int port;

			f->deadline = clock_now + NET_ANSWER_MS;
			if (net_split(f->address, host, sizeof host, &port, &path))
				net_head_feed(f, 0, n, path);
			return f->state;
		}
		if (n == 0 || errno != EAGAIN) {
			if (f->head_n >= 1 && f->head_n < NET_HEAD_MOST && strncmp(f->head, "HTTP/", f->head_n < 5 ? f->head_n : 5)) {
				/* a few bytes, and the end */
				int kept = f->head_n;

				memcpy(f->buf, f->head, kept);
				f->left_at = 0, f->left_n = kept;
				f->simple = 1, f->code = 200, f->head_at = clock_now;
				f->state = NET_HEADED;
				return f->state;
			}
			{
				char why[160];

				snprintf(why, sizeof why, "%s closed the connection without an answer.", f->host);
				net_fail(f, why);
			}
		} else if (clock_now > f->deadline) {
			char why[160];

			snprintf(why, sizeof why, "%s took the request and said nothing in %d seconds.", f->host, NET_ANSWER_MS / 1000);
			net_fail(f, why);
		}
		return f->state;
	case NET_BODY:
		n = (int)read(f->fd, f->buf, NET_READ);
		if (n > 0) {
			f->deadline = clock_now + NET_ANSWER_MS;
			net_feed(f, f->buf, n);
			if (f->state == NET_BODY && f->cut)
				net_end(f, NET_DONE);	/* (as much as is read: the rest is left there) */
			return f->state;
		}
		if (n == 0 || errno != EAGAIN) {
			f->short_ended = f->chunked || (f->length >= 0 && f->got < f->length);
			net_end(f, NET_DONE);
		} else if (clock_now > f->deadline) {
			f->idle_ended = 1;
			f->short_ended = f->chunked || f->length >= 0;
			net_end(f, NET_DONE);
		}
		return f->state;
	}
	return f->state;
}

/* After NET_HEADED: says where the body goes (`file`, or memory up to `most` bytes), and goes on. */
static void
net_body(struct net_fetch *f, const char *file, int most)
{
	if (f->state != NET_HEADED)
		return;
	f->most = most;
	if (file && (f->out = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
		net_fail(f, "The file for it could not be made.");
		return;
	}
	if (!file && f->length > 0) {
		/* (its size is known: no growing) */
		f->room = (f->length < most ? (int)f->length : most) + 1;
		f->body = malloc(f->room);
		if (!f->body)
			f->room = 0;
	}
	f->state = NET_BODY;
	if ((f->code == 204 || f->code == 304 || f->length == 0)) {
		net_end(f, NET_DONE);
		return;
	}
	if (f->left_n > 0) {
		int n = f->left_n;

		f->left_n = 0;
		net_feed(f, f->buf + f->left_at, n);
	}
	f->deadline = net_ms() + NET_ANSWER_MS;
}

static void
net_abort(struct net_fetch *f)
{
	if (f->state && f->state < NET_DONE)
		net_end(f, NET_IDLE);
	f->state = NET_IDLE;
}

/* The answer's end has been handled: its bytes go. */
static void
net_forget(struct net_fetch *f)
{
	net_abort(f);
	free(f->body);
	f->body = NULL;
	f->n = f->room = 0;
}

/* What a typed host looks like it wants: this machine's own http, not the host's https. */
static int
net_typed_is_local(const char *host, int has_port)
{
	return has_port || !strchr(host, '.') || net_literal(host) || net_name_here(host);
}

#endif
