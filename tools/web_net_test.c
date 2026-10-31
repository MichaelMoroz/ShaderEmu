/*
 * The browser's HTTP on the host (linux/apps/web_net.h, docs/apps/web.md): no window system and no machine.
 * Each answer below is served by a child over a real socket on 127.0.0.1, in pieces of every size, and
 * must come out the same; then addresses, the kinds of bytes and the cookies' rules.
 *
 *   gcc -O1 -g -fsanitize=address,undefined -Ilinux/apps tools/web_net_test.c -o web_net_test && ./web_net_test
 */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/wait.h>
#include "web_net.h"

static int bad;

#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); bad++; } } while (0)

/* Serves `answer` once in pieces of `piece` bytes; the request it read goes to `seen`. */
static int
serve(int port_fd, const char *answer, int n, int piece, char *seen_file, int linger)
{
	pid_t child = fork();

	if (child == 0) {
		int fd = accept(port_fd, NULL, NULL), at = 0;
		char req[4096];
		int got = 0;

		/* (a server that closes with the request unread resets the connection, and what it had not sent yet is lost:
		 * curl sees the same) */
		{
			struct pollfd pf = { fd, POLLIN, 0 };

			while (poll(&pf, 1, seen_file ? 100 : 5) > 0) {
				int r = (int)read(fd, req + got, sizeof req - got);

				if (r <= 0)
					break;
				got += r;
			}
		}
		if (seen_file) {
			FILE *s = fopen(seen_file, "w");

			fwrite(req, 1, got, s);
			fclose(s);
		}
		while (at < n) {
			int here = n - at < piece ? n - at : piece;

			if (write(fd, answer + at, here) != here)
				break;
			at += here;
			usleep(200);
		}
		if (linger)
			usleep(linger * 1000);
		close(fd);
		_exit(0);
	}
	return child;
}

static void
fetch(struct net_fetch *f, int port, const char *method, const char *body, int most)
{
	char address[64];

	snprintf(address, sizeof address, "http://127.0.0.1:%d/a/b?c=d#e", port);
	net_start(f, address, net_literal("127.0.0.1"), method, body, body ? (int)strlen(body) : 0, NULL);
	while (f->state < NET_DONE) {
		if (net_poll(f) == NET_HEADED)
			net_body(f, NULL, most);
		usleep(100);
	}
}

int
main(void)
{
	static struct net_fetch f;
	struct sockaddr_in at;
	socklen_t size = sizeof at;
	int port_fd = socket(AF_INET, SOCK_STREAM, 0), port, one = 1;
	static const struct { const char *answer, *body; int code; const char *type; } cases[] = {
		/* the issue's shell server */
		{ "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhello from the shell\n", "hello from the shell\n", 200, "text/plain" },
		/* its lines ended by \n alone */
		{ "HTTP/1.0 200 OK\nContent-Type: text/html; charset=latin1\n\n<p>hi", "<p>hi", 200, "text/html" },
		{ "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n12345", "12345", 200, "" },
		{ "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n7;x=y\r\n, world\r\n0\r\nA: b\r\n\r\n", "hello, world", 200, "" },
		{ "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 404 Not Found\r\nContent-Length: 2\r\n\r\nno", "no", 404, "" },
		{ "no head at all, only text\n", "no head at all, only text\n", 200, "" },
		{ "hi\n", "hi\n", 200, "" },
	};
	char seen[4096];
	int i, piece;
	FILE *file;

	signal(SIGPIPE, SIG_IGN);
	net_cookie_file = "/tmp/web_net_test_cookies";
	unlink(net_cookie_file);
	setsockopt(port_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&at, 0, sizeof at);
	at.sin_family = AF_INET;
	at.sin_addr.s_addr = htonl(0x7f000001);
	CHECK(!bind(port_fd, (struct sockaddr *)&at, sizeof at) && !listen(port_fd, 8) && !getsockname(port_fd, (struct sockaddr *)&at, &size));
	port = ntohs(at.sin_port);
	for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++)
		for (piece = 1; piece < 40; piece += 3) {
			pid_t child = serve(port_fd, cases[i].answer, (int)strlen(cases[i].answer), piece, NULL, 0);

			fetch(&f, port, NULL, NULL, 1000);
			CHECK(f.state == NET_DONE);
			CHECK(f.code == cases[i].code);
			CHECK(!strcmp(f.type, cases[i].type));
			if (f.n != (int)strlen(cases[i].body) || memcmp(f.body, cases[i].body, f.n)) {
				printf("case %d, pieces of %d: %d bytes \"%.*s\"\n", i, piece, f.n, f.n, f.body);
				bad++;
			}
			waitpid(child, NULL, 0);
			net_forget(&f);
		}
	/* a length not met, a chunk not ended: what came, said to be short */
	{
		pid_t child = serve(port_fd, "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort", 44, 100, NULL, 0);

		fetch(&f, port, NULL, NULL, 1000);
		CHECK(f.state == NET_DONE && f.short_ended && f.n == 5);
		waitpid(child, NULL, 0);
		net_forget(&f);
	}
	/* more than is read */
	{
		static char big[3000];
		int n = sprintf(big, "HTTP/1.1 200 OK\r\n\r\n");
		pid_t child;

		memset(big + n, 'x', sizeof big - n);
		child = serve(port_fd, big, sizeof big, 500, NULL, 0);
		fetch(&f, port, NULL, NULL, 100);
		CHECK(f.state == NET_DONE && f.cut && f.n == 100);
		waitpid(child, NULL, 0);
		net_forget(&f);
	}
	/* closed with nothing said */
	{
		pid_t child = serve(port_fd, "", 0, 1, NULL, 0);

		fetch(&f, port, NULL, NULL, 100);
		CHECK(f.state == NET_FAILED && strstr(f.why, "without an answer"));
		waitpid(child, NULL, 0);
		net_forget(&f);
	}
	/* a POST, and a cookie that comes back on the next request */
	{
		const char *a = "HTTP/1.1 303 See Other\r\nLocation: /next\r\nSet-Cookie: sid=abc; Path=/; HttpOnly\r\nSet-Cookie: old=1; Max-Age=0\r\nContent-Length: 0\r\n\r\n";
		pid_t child = serve(port_fd, a, (int)strlen(a), 1000, "/tmp/web_net_test_seen", 0);
		int n;

		fetch(&f, port, "POST", "a=1&b=2", 100);
		CHECK(f.code == 303 && !strcmp(f.location, "/next"));
		waitpid(child, NULL, 0);
		file = fopen("/tmp/web_net_test_seen", "r");
		n = (int)fread(seen, 1, sizeof seen - 1, file);
		seen[n] = 0;
		fclose(file);
		CHECK(!strncmp(seen, "POST /a/b?c=d HTTP/1.1\r\n", 24));
		CHECK(strstr(seen, "\r\nContent-Length: 7\r\n") && strstr(seen, "\r\n\r\na=1&b=2"));
		net_forget(&f);
		child = serve(port_fd, "HTTP/1.0 200 OK\r\n\r\nx", 20, 100, "/tmp/web_net_test_seen", 0);
		fetch(&f, port, NULL, NULL, 100);
		waitpid(child, NULL, 0);
		file = fopen("/tmp/web_net_test_seen", "r");
		n = (int)fread(seen, 1, sizeof seen - 1, file);
		seen[n] = 0;
		fclose(file);
		CHECK(strstr(seen, "\r\nCookie: sid=abc\r\n") != NULL);
		net_forget(&f);
	}
	close(port_fd);
	/* nothing listens: refused, after its tries */
	fetch(&f, port, NULL, NULL, 100);
	CHECK(f.state == NET_FAILED && strstr(f.why, "refused") && f.tries == NET_RETRIES);
	net_forget(&f);
	/* addresses */
	{
		char host[128], why[200];
		const char *path;
		uint32_t ip;
		int p;

		CHECK(net_split("http://10.0.0.11", host, sizeof host, &p, &path) && !strcmp(host, "10.0.0.11") && p == 80 && !*path);
		CHECK(net_split("HTTP://Foo.Bar:8080/x?y", host, sizeof host, &p, &path) && !strcmp(host, "foo.bar") && p == 8080 && !strcmp(path, "/x?y"));
		CHECK(net_split("http://me:pw@host/x", host, sizeof host, &p, &path) && !strcmp(host, "host"));
		CHECK(!net_split("https://x/", host, sizeof host, &p, &path) && !net_split("http://:80/", host, sizeof host, &p, &path));
		CHECK(!net_split("http://a:99999/", host, sizeof host, &p, &path));
		CHECK(net_route("http://127.0.0.1:81/", &ip, why, sizeof why) == ROUTE_DIRECT && ip == htonl(0x7f000001));
		CHECK(net_route("http://localhost/", &ip, why, sizeof why) == ROUTE_DIRECT);
		CHECK(net_route("https://example.com/", &ip, why, sizeof why) == ROUTE_HOST);
		CHECK(net_route("https://localhost/", &ip, why, sizeof why) == ROUTE_NONE && strstr(why, "TLS"));
		CHECK(net_route("file.html", &ip, why, sizeof why) == ROUTE_HOST);
		CHECK(net_typed_is_local("10.0.0.11", 0) && net_typed_is_local("x", 0) && net_typed_is_local("a.b", 1) && !net_typed_is_local("example.com", 0));
	}
	/* cookies' rules */
	{
		char out[400];

		unlink(net_cookie_file);
		net_cookie_count = 0;
		net_cookie_set("a.example.com", "/x/y", "one=1");
		net_cookie_header("a.example.com", "/x/z", out, sizeof out);
		CHECK(!strcmp(out, "one=1"));
		net_cookie_header("a.example.com", "/other", out, sizeof out);
		CHECK(!out[0]);	/* (one's path is /x) */
		net_cookie_header("b.example.com", "/x/z", out, sizeof out);
		CHECK(!out[0]);
		net_cookie_set("a.example.com", "/", "dom=2; Domain=.example.com; Path=/");
		net_cookie_header("b.example.com", "/", out, sizeof out);
		CHECK(!strcmp(out, "dom=2"));
		net_cookie_set("a.example.com", "/", "evil=3; Domain=other.com");
		net_cookie_set("a.example.com", "/", "evil=3; Domain=com");
		net_cookie_set("10.0.0.1", "/", "evil=3; Domain=0.0.1");
		CHECK(net_cookie_count == 2);
		net_cookie_set("a.example.com", "/", "sec=4; Secure");
		net_cookie_header("a.example.com", "/", out, sizeof out);
		CHECK(!strstr(out, "sec"));	/* (no TLS here) */
		net_cookie_set("a.example.com", "/", "dom=gone; Domain=example.com; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
		net_cookie_header("b.example.com", "/", out, sizeof out);
		CHECK(!out[0]);
		CHECK(net_date("Wed, 09 Jun 2021 10:18:14 GMT") == 1623233894);
		CHECK(net_date("Wednesday, 09-Jun-21 10:18:14 GMT") == 1623233894);
		CHECK(net_date("nonsense") == -1);
		net_cookie_set("h", "/longer/path", "deep=1");
		net_cookie_set("h", "/", "root=1");
		net_cookie_header("h", "/longer/path/x", out, sizeof out);
		CHECK(!strcmp(out, "deep=1; root=1"));
		/* read back */
		free(net_cookies);
		net_cookies = NULL;
		net_cookie_count = 0;
		net_cookie_header("h", "/longer/path/x", out, sizeof out);
		CHECK(!strcmp(out, "deep=1; root=1"));
	}
	/* kinds of bytes */
	CHECK(net_sniff("<!doctype html>", 15) == NK_HTML);
	CHECK(net_sniff("  \n<p>x", 7) == NK_HTML);
	CHECK(net_sniff("plain text\n", 11) == NK_TEXT);
	CHECK(net_sniff("a<b and c\n", 10) == NK_TEXT);
	CHECK(net_sniff("PK\3\4\0\0", 6) == NK_SAVE);
	printf("web_net_test: %s\n", bad ? "FAILED" : "PASS");
	return bad != 0;
}
