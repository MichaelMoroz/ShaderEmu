/*
 * The browser's page (docs/apps/web.md): HTML read into a tree, each element's style worked out
 * from the sheets (css.h), and the fonts text is measured with (../nanox/sef.h). web_layout.h
 * lays the tree out. No part of the window system is in here: a test on the host builds it too.
 * The program that includes this gives it web_file(), a font file's bytes by name.
 */
#ifndef NXWEB_WEB_H
#define NXWEB_WEB_H

#include <stdio.h>
#include "css.h"
#include "../nanox/sef.h"

static const void *web_file(const char *name, int *size);

#define T_TEXT 0xffff		/* a node that is text, not an element */
#define WEB_DEPTH 200

enum { N_SPACE = 2, N_CENTER = 4, N_OPEN = 8 };

struct web_node {
	struct css_node c;
	int first, last, next;		/* its children of any kind, and the one after it */
	int text, text_n;		/* a text node's 16-bit characters in web_u */
	int style;			/* an element's, in web_styles */
	int y;				/* where it was laid out */
	int minw, maxw;			/* its content's narrowest and widest, once measured (-1: not yet) */
	int last_element;		/* its last child that is an element, while the tree is made */
	unsigned char flags;
} __attribute__((aligned(16)));		/* (whole texels: two cores style neighbours) */

static struct web_node *web_nodes;
static int web_node_count, web_node_room;
static struct css_style *web_styles;
static int web_style_count, web_style_room;
static unsigned short *web_u;		/* the page's text */
static int web_u_n, web_u_room;
static const char *web_html;		/* the page as it came: tags' attributes are read where they lie */
static int web_html_n;
static char web_title[120];
static char web_address[256];		/* the page's own */
static char web_base[256];		/* what its addresses are made whole with */
static char web_refresh[256];		/* where the page sends its reader on at once (<meta http-equiv=refresh>), or "" */
static int web_latin;			/* its bytes are Latin-1, not UTF-8 */
static char web_http_charset[24];	/* what the answer's own head says of that ("": see the page) */
static int web_body = -1;

/* ---- fonts ---- */

#define WEB_SIZES 9
static const unsigned char web_sizes[WEB_SIZES] = { 10, 11, 13, 15, 17, 20, 24, 28, 32 };
#define FACE_UNI (3 * 2 * WEB_SIZES)	/* every other script, at its own size; the one after it at twice */
#define WEB_FACES (FACE_UNI + 2)

struct web_face {
	const unsigned char *file;	/* NULL: not in this image */
	unsigned char *adv[256];	/* a code point's advance, by its high byte: 255 for none */
	int height, ascent, twice, tried;
	char name[32];
};
#ifdef WEB_FACES_LATER
static struct web_face *web_faces;	/* (60 KB the program allocates before it lays anything out: nxedit's reader) */
#else
static struct web_face web_faces[WEB_FACES];
#endif
/* With no fonts in the image, or a window system that cannot draw them: every face is the
 * window system's own font, whose widths these are (Latin letters only, one size). */
static const unsigned char *web_plain;
static int web_plain_height, web_plain_ascent;

static struct web_face *
web_face(int n)
{
	static const char *const names[] = { "sans", "sans-b", "serif", "serif-b", "mono", "mono-b" };
	struct web_face *f = &web_faces[n];
	int size;

	if (f->tried)
		return f;
	f->tried = 1;
	if (web_plain) {
		f->height = web_plain_height;
		f->ascent = web_plain_ascent;
		return f;
	}
	if (n >= FACE_UNI)
		snprintf(f->name, sizeof f->name, "font-uni-16.sef");
	else
		snprintf(f->name, sizeof f->name, "font-%s-%d.sef", names[n / WEB_SIZES], web_sizes[n % WEB_SIZES]);
	f->file = web_file(f->name, &size);
	if (f->file && !sef_ok(f->file, size))
		f->file = NULL;
	if (f->file) {
		const struct sef_head *h = (const struct sef_head *)f->file;

		f->twice = n == FACE_UNI + 1;
		f->height = h->height << f->twice;
		f->ascent = h->ascent << f->twice;
	}
	return f;
}

/* The face for a family (0 sans, 1 serif, 2 fixed), a weight and a size in pixels. */
static int
web_face_for(int family, int bold, int px)
{
	int i, best = 0;

	for (i = 1; i < WEB_SIZES; i++)
		if (abs(web_sizes[i] - px) < abs(web_sizes[best] - px))
			best = i;
	return ((family > 2 ? 0 : family) * 2 + (bold ? 1 : 0)) * WEB_SIZES + best;
}

#define WEB_ADVANCE_LATER 254	/* in a block of advances: not looked up in the font yet */

/* The advances of the 256 code points that begin with one byte, made when first asked for. */
static const unsigned char *
web_advances(struct web_face *f, unsigned high)
{
	static const unsigned char none[256] = { [0 ... 255] = 255 };
	unsigned char *block;
	int i;

	if (!f->file && !web_plain)
		return none;
	block = f->adv[high] = malloc(256);
	if (web_plain) {
		/* (a character the font lacks is shown as a question mark, and is as wide) */
		for (i = 0; i < 256; i++)
			block[i] = !high && i >= 32 && i < 127 ? web_plain[i] : web_plain['?'];
		return block;
	}
	/* (each is looked up in the font when a text first has it: a page has some sixty of 256) */
	memset(block, WEB_ADVANCE_LATER, 256);
	return block;
}

/* The advance of a code point not looked up yet (WEB_ADVANCE_LATER in its block), kept. */
static int
web_advance_found(struct web_face *f, unsigned code)
{
	int g = sef_glyph(f->file, code & 0xffff), a = g < 0 ? 255 : sef_advance(f->file, g);

	if (a != WEB_ADVANCE_LATER)
		f->adv[code >> 8 & 255][code & 255] = (unsigned char)a;
	return a;
}

/* How far a character moves the line in a face, or -1 when the face has no glyph for it. */
static inline int
web_advance(int face, unsigned code)
{
	struct web_face *f = &web_faces[face];
	const unsigned char *block = f->adv[code >> 8 & 255];
	int a;

	if (!block)
		block = web_advances(f, code >> 8 & 255);
	a = block[code & 255];
	if (a == WEB_ADVANCE_LATER)
		a = web_advance_found(f, code);
	return a == 255 ? -1 : a << f->twice;
}

/* The face a character is drawn in, of a text whose own face is `face`: that one, or the
 * face of every script (twice its size in a large text). -1: nothing has it. */
static inline int
web_char_face(int face, unsigned code, int *advance)
{
	int other;

	if ((*advance = web_advance(face, code)) >= 0)
		return face;
	other = FACE_UNI + (web_faces[face].height >= 26);
	web_face(other);
	if ((*advance = web_advance(other, code)) >= 0)
		return other;
	return -1;
}

/* ---- addresses ---- */

/* An address a page gives (256 bytes of room), made whole with the page's own. 0 for what is
 * not an address to fetch: mailto: and the like, and pictures written into the page itself. */
static int
web_resolve(const char *href, char *whole)
{
	char base[256], joined[600];
	char *slash, *host, *cut, *from, *to;
	int n;

	while (isspace((unsigned char)*href))
		href++;
	if (!href[0] || !strncasecmp(href, "data:", 5) || !strncasecmp(href, "javascript:", 11))
		return 0;
	for (n = 0; href[n] && (isalnum((unsigned char)href[n]) || href[n] == '+' || href[n] == '-' || href[n] == '.'); n++)
		;
	snprintf(base, sizeof base, "%s", web_base[0] ? web_base : web_address);
	if ((cut = strchr(base, '#')) != NULL)
		*cut = 0;
	if (href[n] == ':' && n > 1) {
		if (strncasecmp(href, "http:", 5) && strncasecmp(href, "https:", 6) && strncasecmp(href, "world:", 6) && strncasecmp(href, "file:", 5))
			return 0;
		snprintf(joined, sizeof joined, "%s", !strncasecmp(href, "file://", 7) ? href + 7 : href);
	} else {
		host = strstr(base, "://");
		if ((cut = strchr(host ? host + 3 : base, '?')) != NULL && href[0] != '?')
			*cut = 0;
		if (!strncmp(href, "//", 2) && host) {
			host[1] = 0;
			snprintf(joined, sizeof joined, "%s%s", base, href);
		} else if (href[0] == '/') {
			/* from the site's root, or from this computer's for a page that is a file */
			if (host && (slash = strchr(host + 3, '/')) != NULL)
				*slash = 0;
			snprintf(joined, sizeof joined, "%s%s", host ? base : "", href);
		} else if (href[0] == '?' || href[0] == '#') {
			if (href[0] == '?' && cut)
				*cut = 0;
			snprintf(joined, sizeof joined, "%s%s", base, href);
		} else {
			slash = strrchr(base, '/');
			if (slash && (!host || slash > host + 2))
				slash[1] = 0;
			else
				strcat(base, "/");
			snprintf(joined, sizeof joined, "%s%s", base, href);
		}
	}
	/* the path's ./ and ../ are taken out: a host is asked for what it will be given */
	host = strstr(joined, "://");
	from = to = host && (slash = strchr(host + 3, '/')) != NULL ? slash : joined[0] == '/' ? joined : joined + strlen(joined);
	slash = from;
	while (*from && *from != '?' && *from != '#') {
		if (from[0] == '/' && from[1] == '.' && (from[2] == '/' || !from[2] || from[2] == '?' || from[2] == '#')) {
			from += 2;
			if (*from != '/')
				*to++ = '/';
		} else if (from[0] == '/' && from[1] == '.' && from[2] == '.' && (from[3] == '/' || !from[3] || from[3] == '?' || from[3] == '#')) {
			from += 3;
			while (to > slash && *--to != '/')
				;
			if (*from != '/')
				*to++ = '/';
		} else {
			*to++ = *from++;
		}
	}
	memmove(to, from, strlen(from) + 1);
	if (host && !strchr(host + 3, '/') && strlen(joined) < sizeof joined - 2) {
		/* a site with no path is its root */
		cut = joined + strcspn(joined, "?#");
		memmove(cut + 1, cut, strlen(cut) + 1);
		*cut = '/';
	}
	for (n = 0; joined[n]; n++)
		if ((unsigned char)joined[n] <= ' ')
			return 0;
	if (n > 255)
		return 0;	/* (more than the host can be asked for) */
	strcpy(whole, joined);
	return 1;
}

/* ---- characters ---- */

static const struct { char name[10]; unsigned short code; } web_entities[] = {	/* (names in place: no pointers to relocate when a program starts) */
	{ "AElig", 0xc6 }, { "Aacute", 0xc1 }, { "Agrave", 0xc0 }, { "Aring", 0xc5 }, { "Auml", 0xc4 }, { "Ccedil", 0xc7 },
	{ "Dagger", 0x2021 }, { "Delta", 0x394 }, { "Eacute", 0xc9 }, { "Egrave", 0xc8 }, { "Iacute", 0xcd }, { "Ntilde", 0xd1 },
	{ "Oacute", 0xd3 }, { "Omega", 0x3a9 }, { "Oslash", 0xd8 }, { "Ouml", 0xd6 }, { "Prime", 0x2033 }, { "Sigma", 0x3a3 },
	{ "Uacute", 0xda }, { "Uuml", 0xdc }, { "aacute", 0xe1 }, { "acirc", 0xe2 }, { "acute", 0xb4 }, { "aelig", 0xe6 },
	{ "agrave", 0xe0 }, { "alpha", 0x3b1 }, { "amp", '&' }, { "apos", '\'' }, { "aring", 0xe5 }, { "asymp", 0x2248 },
	{ "atilde", 0xe3 }, { "auml", 0xe4 }, { "bdquo", 0x201e }, { "beta", 0x3b2 }, { "brvbar", 0xa6 }, { "bull", 0x2022 },
	{ "ccedil", 0xe7 }, { "cedil", 0xb8 }, { "cent", 0xa2 }, { "check", 0x2713 }, { "copy", 0xa9 }, { "crarr", 0x21b5 },
	{ "curren", 0xa4 }, { "dArr", 0x21d3 }, { "dagger", 0x2020 }, { "darr", 0x2193 }, { "deg", 0xb0 }, { "delta", 0x3b4 },
	{ "divide", 0xf7 }, { "eacute", 0xe9 }, { "ecirc", 0xea }, { "egrave", 0xe8 }, { "emsp", 0x2003 }, { "ensp", 0x2002 },
	{ "epsilon", 0x3b5 }, { "equiv", 0x2261 }, { "eth", 0xf0 }, { "euml", 0xeb }, { "euro", 0x20ac }, { "frac12", 0xbd },
	{ "frac14", 0xbc }, { "frac34", 0xbe }, { "gamma", 0x3b3 }, { "ge", 0x2265 }, { "gt", '>' }, { "hArr", 0x21d4 },
	{ "harr", 0x2194 }, { "hearts", 0x2665 }, { "hellip", 0x2026 }, { "iacute", 0xed }, { "icirc", 0xee }, { "iexcl", 0xa1 },
	{ "igrave", 0xec }, { "infin", 0x221e }, { "iquest", 0xbf }, { "iuml", 0xef }, { "lArr", 0x21d0 }, { "lambda", 0x3bb },
	{ "laquo", 0xab }, { "larr", 0x2190 }, { "ldquo", 0x201c }, { "le", 0x2264 }, { "lrm", 0x200e }, { "lsaquo", 0x2039 },
	{ "lsquo", 0x2018 }, { "lt", '<' }, { "macr", 0xaf }, { "mdash", 0x2014 }, { "micro", 0xb5 }, { "middot", 0xb7 },
	{ "minus", 0x2212 }, { "mu", 0x3bc }, { "nbsp", 0xa0 }, { "ndash", 0x2013 }, { "ne", 0x2260 }, { "not", 0xac },
	{ "ntilde", 0xf1 }, { "oacute", 0xf3 }, { "ocirc", 0xf4 }, { "ograve", 0xf2 }, { "omega", 0x3c9 }, { "ordf", 0xaa },
	{ "ordm", 0xba }, { "oslash", 0xf8 }, { "otilde", 0xf5 }, { "ouml", 0xf6 }, { "para", 0xb6 }, { "permil", 0x2030 },
	{ "pi", 0x3c0 }, { "plusmn", 0xb1 }, { "pound", 0xa3 }, { "prime", 0x2032 }, { "quot", '"' }, { "rArr", 0x21d2 },
	{ "raquo", 0xbb }, { "rarr", 0x2192 }, { "rdquo", 0x201d }, { "reg", 0xae }, { "rlm", 0x200f }, { "rsaquo", 0x203a },
	{ "rsquo", 0x2019 }, { "sbquo", 0x201a }, { "sect", 0xa7 }, { "shy", 0xad }, { "sigma", 0x3c3 }, { "sup1", 0xb9 },
	{ "sup2", 0xb2 }, { "sup3", 0xb3 }, { "szlig", 0xdf }, { "tau", 0x3c4 }, { "theta", 0x3b8 }, { "thinsp", 0x2009 },
	{ "thorn", 0xfe }, { "tilde", 0x2dc }, { "times", 0xd7 }, { "trade", 0x2122 }, { "uArr", 0x21d1 }, { "uacute", 0xfa },
	{ "uarr", 0x2191 }, { "ucirc", 0xfb }, { "ugrave", 0xf9 }, { "uml", 0xa8 }, { "uuml", 0xfc }, { "yacute", 0xfd },
	{ "yen", 0xa5 }, { "yuml", 0xff }, { "zwj", 0x200d }, { "zwnj", 0x200c },
};

/* The character an entity such as &amp; or &#8217; stands for, and how much of the text it was. */
static int
web_entity(const char *at, int left, int *used)
{
	int lo = 0, hi = (int)(sizeof web_entities / sizeof web_entities[0]), n = 1, code;

	if (left > 2 && at[1] == '#') {
		int hex = at[2] == 'x' || at[2] == 'X';

		for (n = 2 + hex, code = 0; n < left && n < 12 && (hex ? isxdigit((unsigned char)at[n]) : isdigit((unsigned char)at[n])); n++)
			code = code * (hex ? 16 : 10) + (isdigit((unsigned char)at[n]) ? at[n] - '0' : (at[n] | 32) - 'a' + 10);
		if (n == 2 + hex)
			return 0;
		*used = n + (n < left && at[n] == ';');
		return code > 0xffff || code < 9 ? 0xfffd : code;
	}
	while (n < left && n < 10 && isalnum((unsigned char)at[n]))
		n++;
	if (n >= left || at[n] != ';')
		return 0;
	while (lo < hi) {
		int mid = (lo + hi) / 2, order = strncmp(at + 1, web_entities[mid].name, n - 1);

		if (!order && !web_entities[mid].name[n - 1]) {
			*used = n + 1;
			return web_entities[mid].code;
		}
		if (order < 0 || (!order && web_entities[mid].name[n - 1]))
			hi = mid;
		else
			lo = mid + 1;
	}
	return 0;
}

/* The character at `at` (UTF-8, or one byte of a Latin-1 page), and its length. */
static int
web_char(const unsigned char *at, int left, int *used)
{
	static const unsigned short high[32] = {	/* Windows' 0x80 to 0x9f */
		0x20ac, 0xfffd, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0xfffd, 0x17d, 0xfffd,
		0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0xfffd, 0x17e, 0x178,
	};
	int n = *at >= 0xf0 ? 4 : *at >= 0xe0 ? 3 : *at >= 0xc0 ? 2 : 1, code, i;

	*used = 1;
	if (*at < 0x80)
		return *at;
	if (!web_latin && n > 1 && n <= left) {
		code = *at & (0xff >> (n + 1));
		for (i = 1; i < n && (at[i] & 0xc0) == 0x80; i++)
			code = code << 6 | (at[i] & 0x3f);
		if (i == n) {
			*used = n;
			return code > 0xffff ? 0xfffd : code;
		}
	}
	return *at < 0xa0 ? high[*at - 0x80] : *at;
}

/* ---- the tree ---- */

static void *
web_grow(void *block, int *room, int need, int size)
{
	if (need <= *room)
		return block;
	*room = need * 2 + 256;
	return realloc(block, (size_t)*room * size);
}

static inline int
web_attr(int node, const char *name, const char **value, int *n)
{
	const struct web_node *e = &web_nodes[node];

	if (!(e->c.names & css_name_bit(name, (int)strlen(name))))
		return 0;
	return css_attribute(e->c.attrs, e->c.attrs_n, name, value, n);
}

/* Notes which names an element's tag has, and where its id and its classes are (one walk
 * over the tag for what every element is asked). */
static const char *web_seen_id, *web_seen_class;
static int web_seen_id_n, web_seen_class_n;

static void
web_note_names(struct web_node *e)
{
	const char *s = e->c.attrs;
	int n = e->c.attrs_n, i = 0;

	e->c.names = 0;
	web_seen_id = web_seen_class = NULL;
	if (n < 2)
		return;		/* (a tag with nothing after its name: most of them) */
	while (i < n) {
		int start, name_n, value;

		while (i < n && (isspace((unsigned char)s[i]) || s[i] == '/'))
			i++;
		start = i;
		while (i < n && !isspace((unsigned char)s[i]) && s[i] != '=' && s[i] != '/')
			i++;
		name_n = i - start;
		if (name_n)
			e->c.names |= css_name_bit(s + start, name_n);
		while (i < n && isspace((unsigned char)s[i]))
			i++;
		if (i < n && s[i] == '=') {
			for (i++; i < n && isspace((unsigned char)s[i]); i++)
				;
			if (i < n && (s[i] == '"' || s[i] == 39)) {
				char quote = s[i++];

				for (value = i; i < n && s[i] != quote; i++)
					;
				i++;
			} else
				for (value = i; i < n && !isspace((unsigned char)s[i]); i++)
					;
			if (name_n == 2 && (s[start] | 32) == 'i' && (s[start + 1] | 32) == 'd')
				web_seen_id = s + value, web_seen_id_n = (i > n ? n : i) - value - (i <= n && i > value && (s[i - 1] == '"' || s[i - 1] == 39) ? 1 : 0);
			else if (name_n == 5 && !strncasecmp(s + start, "class", 5))
				web_seen_class = s + value, web_seen_class_n = (i > n ? n : i) - value - (i <= n && i > value && (s[i - 1] == '"' || s[i - 1] == 39) ? 1 : 0);
		} else if (!name_n)
			i++;
	}
}

/* An attribute as text of its own, its &amp; and such as the characters they stand for
 * (UTF-8) and the white space round it gone. 0 when the element has none of that name. */
static int
web_attr_text(int node, const char *name, char *out, int room)
{
	const char *value;
	int n, i, k = 0, used, code;

	if (!web_attr(node, name, &value, &n))
		return 0;
	while (n > 0 && isspace((unsigned char)*value))
		value++, n--;
	while (n > 0 && isspace((unsigned char)value[n - 1]))
		n--;
	for (i = 0; i < n && k < room - 4; i += used) {
		used = 1;
		code = (unsigned char)value[i];
		if (code == '&' && (code = web_entity(value + i, n - i, &used)) == 0)
			code = '&', used = 1;
		else if (code == '\n' || code == '\r' || code == '\t')
			continue;
		if (code < 0x80 || (unsigned char)value[i] >= 0x80)
			out[k++] = code;
		else if (code < 0x800)
			out[k++] = 0xc0 | code >> 6, out[k++] = 0x80 | (code & 0x3f);
		else
			out[k++] = 0xe0 | code >> 12, out[k++] = 0x80 | (code >> 6 & 0x3f), out[k++] = 0x80 | (code & 0x3f);
	}
	out[k] = 0;
	return 1;
}

static int
web_attr_int(int node, const char *name, int none)
{
	const char *value;
	int n;

	if (!web_attr(node, name, &value, &n) || n <= 0 || !(isdigit((unsigned char)*value) || *value == '-' || *value == '+'))
		return none;
	return atoi(value);
}

static int
web_new_node(int parent, int tag)
{
	struct web_node *node, *p;

	web_nodes = web_grow(web_nodes, &web_node_room, web_node_count + 1, sizeof *web_nodes);
	node = &web_nodes[web_node_count];
	memset(node, 0, sizeof *node);
	node->c.tag = tag;
	node->c.parent = parent;
	node->c.prev = -1;
	node->first = node->last = node->next = node->style = node->last_element = -1;
	node->minw = node->maxw = -1;
	if (parent >= 0) {
		p = &web_nodes[parent];
		if (p->last >= 0)
			web_nodes[p->last].next = web_node_count;
		else
			p->first = web_node_count;
		p->last = web_node_count;
	}
	return web_node_count++;
}

/* Whether a tag ends a paragraph that was left open. */
static int
web_ends_p(int t)
{
	switch (t) {
	case T_p: case T_div: case T_ul: case T_ol: case T_table: case T_pre: case T_blockquote: case T_hr: case T_h1:
	case T_h2: case T_h3: case T_h4: case T_h5: case T_h6: case T_section: case T_article: case T_header:
	case T_footer: case T_form: case T_dl: case T_nav: case T_main: case T_aside: case T_figure: case T_address:
	case T_fieldset: case T_details: case T_center: case T_menu: case T_hgroup: case T_figcaption:
		return 1;
	}
	return 0;
}

static int
web_void(int t)
{
	switch (t) {
	case T_br: case T_img: case T_hr: case T_input: case T_meta: case T_link: case T_source: case T_wbr: case T_area:
	case T_col: case T_embed: case T_param: case T_track: case T_base:
		return 1;
	}
	return 0;
}

/* Reads the page's HTML into the tree. The sheets are read before it: an element's classes
 * are kept by the numbers the sheets gave their names, and those no sheet names are dropped. */
static void
web_parse(const char *html, int n)
{
	int stack[WEB_DEPTH], depth = 0, i = 0, text_from, raw = 0, in_title = 0, title_n = 0, inked = 0, after_pre = 0;
	const char *charset;

	web_html = html;
	web_html_n = n;
	web_node_count = web_u_n = css_class_count = 0;
	web_title[0] = web_base[0] = web_refresh[0] = 0;
	web_body = -1;
	charset = memmem(html, n < 2048 ? n : 2048, "charset=", 8);
	if (!charset)
		charset = memmem(html, n < 2048 ? n : 2048, "CHARSET=", 8);
	charset = charset ? charset + 8 + (charset[8] == '"' || charset[8] == '\'') : "utf-8";
	if (web_http_charset[0])
		charset = web_http_charset;
	web_latin = !strncasecmp(charset, "iso-8859", 8) || !strncasecmp(charset, "windows-125", 11) || !strncasecmp(charset, "latin", 5) ||
		    !strncasecmp(charset, "us-ascii", 8);
	web_u = web_grow(web_u, &web_u_room, n + 64, sizeof *web_u);
	stack[depth++] = web_new_node(-1, T_html);	/* (the page's own <html> gives it its attributes) */
	web_nodes[0].c.index = 1;
	web_nodes[0].c.last = 1;
	web_nodes[0].c.attrs = "";
	text_from = 0;
#define TOP stack[depth - 1]
#define END_TEXT() do { \
		if (web_u_n > text_from) { \
			int only_space = !inked, pt = web_nodes[TOP].c.tag; \
			if (only_space && (pt == T_table || pt == T_tbody || pt == T_thead || pt == T_tfoot || pt == T_tr || pt == T_ul || \
					   pt == T_ol || pt == T_dl || pt == T_select || pt == T_html || pt == T_head || pt == T_colgroup)) { \
				web_u_n = text_from; \
			} else { \
				int t = web_new_node(TOP, T_TEXT); \
				web_nodes[t].text = text_from; \
				web_nodes[t].text_n = web_u_n - text_from; \
				web_nodes[t].flags = only_space ? N_SPACE : 0; \
				web_nodes[TOP].c.kids += !only_space; \
			} \
		} \
		web_u[web_u_n++] = 0;	/* (never two nodes' text end to end: a run does not go on into the next) */ \
		text_from = web_u_n; \
		inked = 0; \
	} while (0)
	while (i < n) {
		unsigned char c = html[i];
		int used = 1, ch;

		if (c > ' ' && c < 0x7f && c != '<' && c != '&' && !in_title && web_u_n + 8 < web_u_room) {
			/* a word's plain letters, as many as follow: most of a page, and nothing below is theirs */
			do
				web_u[web_u_n++] = c;
			while (++i < n && (c = html[i]) > ' ' && c < 0x7f && c != '<' && c != '&' && web_u_n + 8 < web_u_room);
			inked = 1;
			after_pre = 0;
			continue;
		}
		if (c == '<' && i + 1 < n && (isalpha((unsigned char)html[i + 1]) || html[i + 1] == '/' || html[i + 1] == '!' || html[i + 1] == '?')) {
			int end = i + 1, quote, closing, name, t, k, self;
			const char *tag;

			if (!strncmp(html + i, "<!--", 4)) {
				const char *close = memmem(html + i + 4, n - i - 4, "-->", 3);

				i = close ? (int)(close - html) + 3 : n;
				continue;
			}
			/* to the tag's end: a > inside a quoted value is not it */
			for (quote = 0; end < n && (quote || html[end] != '>'); end++)
				if ((html[end] == '"' || html[end] == '\'') && (quote || html[end - 1] == '='))
					quote = quote == html[end] ? 0 : quote ? quote : html[end];
			if (html[i + 1] == '!' || html[i + 1] == '?') {
				i = end + 1;
				continue;
			}
			closing = html[i + 1] == '/';
			tag = html + i + 1 + closing;
			for (name = 0; tag + name < html + end && (isalnum((unsigned char)tag[name]) || tag[name] == '-' || tag[name] == ':'); name++)
				;
			t = css_atom(tag, name, 1, 1);
			i = end + 1;
			if (in_title) {
				web_title[title_n] = 0;
				in_title = 0;
			}
			if (!t)
				continue;
			if (closing) {
				for (k = depth - 1; k > 0 && web_nodes[stack[k]].c.tag != t; k--)
					;
				if (k > 0) {
					END_TEXT();
					depth = k;
					raw = 0;
					for (k = 1; k < depth; k++)
						raw |= web_nodes[stack[k]].c.tag == T_pre || web_nodes[stack[k]].c.tag == T_textarea;
				} else if (t == T_p || t == T_br) {
					END_TEXT();
					k = web_new_node(TOP, t);	/* a stray </p> is an empty paragraph */
					web_nodes[k].c.attrs = "";
				}
				continue;
			}
			if (t == T_html || t == T_body) {
				/* the one there already takes this tag's attributes */
				k = t == T_html ? 0 : web_body;
				if (k < 0) {
					END_TEXT();
					depth = 1;
					k = web_body = web_new_node(0, T_body);
					stack[depth++] = k;
				}
				if (!web_nodes[k].c.attrs || !web_nodes[k].c.attrs_n) {
					web_nodes[k].c.attrs = tag + name;
					web_nodes[k].c.attrs_n = (int)(html + end - (tag + name));
					web_note_names(&web_nodes[k]);
				}
				continue;
			}
			END_TEXT();
			/* what a new element ends without saying so */
#define CLOSE_INSIDE(a, b, s1, s2) do { \
				for (k = depth - 1; k > 0 && web_nodes[stack[k]].c.tag != (s1) && web_nodes[stack[k]].c.tag != (s2); k--) \
					if (web_nodes[stack[k]].c.tag == (a) || web_nodes[stack[k]].c.tag == (b)) { depth = k; break; } \
			} while (0)
			switch (t) {
			case T_li: CLOSE_INSIDE(T_li, T_li, T_ul, T_ol); break;
			case T_dt: case T_dd: CLOSE_INSIDE(T_dt, T_dd, T_dl, T_dl); break;
			case T_tr: CLOSE_INSIDE(T_tr, T_tr, T_table, T_table); break;
			case T_td: case T_th: CLOSE_INSIDE(T_td, T_th, T_tr, T_table); break;
			case T_thead: case T_tbody: case T_tfoot:
				CLOSE_INSIDE(T_tr, T_tr, T_table, T_table);
				CLOSE_INSIDE(T_thead, T_tbody, T_table, T_table);
				CLOSE_INSIDE(T_tfoot, T_tfoot, T_table, T_table);
				break;
			case T_option: case T_optgroup: CLOSE_INSIDE(T_option, T_option, T_select, T_select); break;
			case T_a: CLOSE_INSIDE(T_a, T_a, T_table, T_td); break;
			}
			if (web_ends_p(t))
				CLOSE_INSIDE(T_p, T_p, T_table, T_td);
			if (web_body < 0 && t != T_head && t != T_title && t != T_meta && t != T_link && t != T_style && t != T_script &&
			    t != T_base && t != T_noscript) {
				/* the page's first content: a head that was never closed ends here */
				depth = 1;
				web_body = web_new_node(0, T_body);
				web_nodes[web_body].c.attrs = "";
				stack[depth++] = web_body;
			}
			k = web_new_node(TOP, t);
			web_nodes[TOP].c.kids++;
			web_nodes[k].c.attrs = tag + name;
			web_nodes[k].c.attrs_n = (int)(html + end - (tag + name));
			self = web_nodes[k].c.attrs_n > 0 && html[end - 1] == '/';
			if (self)
				web_nodes[k].c.attrs_n--;
			web_note_names(&web_nodes[k]);
			{
				/* which element child it is, the element before it, its id and classes */
				const char *value;
				int vn, a, b;

				web_nodes[k].c.prev = web_nodes[TOP].last_element;
				web_nodes[TOP].last_element = k;
				web_nodes[k].c.index = web_nodes[k].c.prev >= 0 ? web_nodes[web_nodes[k].c.prev].c.index + 1 : 1;
				if (css_rule_count && web_seen_id)
					web_nodes[k].c.id = css_atom(web_seen_id, web_seen_id_n, 0, 0);
				web_nodes[k].c.cls = css_class_count;
				value = web_seen_class;
				vn = web_seen_class_n;
				if (css_rule_count && value)
					for (a = 0; a < vn; a = b) {
						int atom;

						while (a < vn && isspace((unsigned char)value[a]))
							a++;
						for (b = a; b < vn && !isspace((unsigned char)value[b]); b++)
							;
						if (b > a && (atom = css_atom(value + a, b - a, 0, 0)) != 0) {
							css_classes = css_grow(css_classes, &css_class_room, css_class_count + 1, sizeof *css_classes);
							css_classes[css_class_count++] = atom;
						}
					}
				web_nodes[k].c.classes = css_class_count - web_nodes[k].c.cls;
			}
			if (t == T_meta && !web_refresh[0] && (web_nodes[k].c.names & css_name_bit("http-equiv", 10))) {
				/* "0;url=ADDRESS": a page for readers without scripts is often behind one */
				char how[24], content[400], whole[256];
				char *url;

				if (web_attr_text(k, "http-equiv", how, sizeof how) && !strcasecmp(how, "refresh") &&
				    web_attr_text(k, "content", content, sizeof content) && atoi(content) <= 10 &&
				    (url = strcasestr(content, "url=")) != NULL && web_resolve(url + 4 + (url[4] == '\'' || url[4] == '"'), whole)) {
					whole[strcspn(whole, "'\"")] = 0;
					if (strcmp(whole, web_address))
						strcpy(web_refresh, whole);
				}
			}
			if (t == T_base && !web_base[0]) {
				char href[256], whole[256];

				if (web_attr_text(k, "href", href, sizeof href) && web_resolve(href, whole))
					strcpy(web_base, whole);
			}
			if (t == T_script || t == T_style) {
				/* its text is not tags: a < in it would take the rest of the page */
				const char *close = t == T_script ? "</script" : "</style";
				int length = (int)strlen(close);

				for (;;) {
					const char *lt = i < n ? memchr(html + i, '<', (size_t)(n - i)) : NULL;

					i = lt ? (int)(lt - html) : n;
					if (i + length > n || !strncasecmp(html + i, close, length))
						break;
					i++;
				}
				i = i + length > n ? n : i;
				continue;
			}
			if (t == T_title && !web_title[0]) {
				in_title = 1;
				title_n = 0;
			}
			if (web_void(t) || self)
				continue;
			if (depth < WEB_DEPTH - 1)
				stack[depth++] = k;
			if (t == T_pre || t == T_textarea)
				raw = 1, after_pre = 1;
			continue;
		}
		ch = c;
		if (c == '&') {
			if ((ch = web_entity(html + i, n - i, &used)) == 0)
				ch = '&', used = 1;
		} else if (c >= 0x80) {
			ch = web_char((const unsigned char *)html + i, n - i, &used);
		}
		i += used;
		if (in_title) {
			if (title_n < (int)sizeof web_title - 1)
				web_title[title_n++] = ch < 32 ? ' ' : ch > 126 ? '?' : ch;
			continue;
		}
		if (web_u_n + 8 >= web_u_room)
			web_u = web_grow(web_u, &web_u_room, web_u_n + 8, sizeof *web_u);
		/* white space is kept as it is written: the style says later whether it is one space
		 * or the lines and columns of a listing. Not a line's end right after <pre>. */
		if (ch == '\r' || (ch == '\n' && after_pre && raw)) {
			after_pre = 0;
			continue;
		}
		after_pre = 0;
		if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\f') {
			if (ch == '\t' && raw)
				web_u[web_u_n++] = ' ', web_u[web_u_n++] = ' ', web_u[web_u_n++] = ' ';
			/* (a third space in a row is dropped where no listing can be: most of a page's bytes of them) */
			if (ch != '\n' && !raw && web_u_n > text_from + 1 && web_u[web_u_n - 1] == ' ' && web_u[web_u_n - 2] == ' ')
				continue;
			web_u[web_u_n++] = ch == '\n' ? '\n' : ' ';
			continue;
		}
		if (ch < 32 || ch == 0xad || ch == 0x200b || ch == 0x200c || ch == 0x200d || ch == 0x200e || ch == 0x200f || ch == 0xfeff)
			continue;
		web_u[web_u_n++] = ch;
		inked = 1;
	}
	END_TEXT();
	if (in_title)
		web_title[title_n] = 0;
	if (web_body < 0) {
		web_body = web_new_node(0, T_body);
		web_nodes[web_body].c.attrs = "";
	}
	/* an element's last element child knows it is that */
	for (i = 0; i < web_node_count; i++)
		if (web_nodes[i].c.tag != T_TEXT && web_nodes[i].last_element >= 0)
			web_nodes[web_nodes[i].last_element].c.last = 1;
#undef TOP
#undef END_TEXT
#undef CLOSE_INSIDE
}

/* ---- styles ---- */

static int
web_scaled(int page_px)
{
	return (page_px * CSS_EM + 8) / 16;
}

/* A length an attribute gives ("85%", "120"): pixels of ours or a percentage; `none` otherwise. */
static int
web_attr_len(int node, const char *name, int none)
{
	const char *value;
	int n, v;

	if (!web_attr(node, name, &value, &n) || n <= 0 || !isdigit((unsigned char)*value))
		return none;
	v = atoi(value);
	return n > 1 && value[n - 1] == '%' ? CSS_PCT + v * 10 : web_scaled(v);
}

static int
web_attr_colour(int node, const char *name)
{
	char text[40];
	const char *value;
	int n, c;

	if (!web_attr(node, name, &value, &n) || n <= 0 || n > 30)
		return CSS_NONE;
	/* some write the colour without its # */
	snprintf(text, sizeof text, "#%.*s", n, value);
	if ((c = css_colour(text + 1)) >= 0 || (c = css_colour(text)) >= 0)
		return c;
	return CSS_NONE;
}

/* What a walk over the tree keeps while it styles: the elements the one being styled is inside
 * and their names counted (css_inside, or an array of another core's), and the table whose
 * cells it styled last. */
struct web_walk {
	unsigned short *inside;
	int stack[WEB_DEPTH], depth;
	int table_seen, table_padding, table_border;
};

/* How a tag looks before any style sheet has its say. */
static void
web_plain_style(struct web_walk *w, int node, struct css_style *s, const struct css_style *parent)
{
	const struct web_node *e = &web_nodes[node];
	const char *value;
	int t = e->c.tag, em = s->font_size, n, c, up;

	switch (t) {
	case T_h1: s->font_size = em * 2; goto heading;
	case T_h2: s->font_size = em * 3 / 2; goto heading;
	case T_h3: s->font_size = em * 7 / 6; goto heading;
	case T_h4: goto heading;
	case T_h5: s->font_size = em * 5 / 6; goto heading;
	case T_h6: s->font_size = em * 3 / 4;
	heading:
		s->bold = 1;
		s->display = D_BLOCK;
		s->m[0] = s->m[2] = t == T_h1 ? s->font_size * 2 / 3 : t == T_h2 ? s->font_size * 5 / 6 : s->font_size;
		break;
	case T_p: case T_blockquote: case T_figure: case T_dl: case T_fieldset: case T_menu:
		s->display = D_BLOCK;
		s->m[0] = s->m[2] = em;
		if (t == T_blockquote || t == T_figure)
			s->m[1] = s->m[3] = web_scaled(40);
		if (t == T_fieldset) {
			s->border_w[0] = s->border_w[1] = s->border_w[2] = s->border_w[3] = 1;
			s->border_c[0] = s->border_c[1] = s->border_c[2] = s->border_c[3] = 0xa0a0a0;
			s->p[0] = s->p[1] = s->p[2] = s->p[3] = 6;
			s->m[1] = s->m[3] = 2;
		}
		if (t == T_menu)
			s->p[3] = web_scaled(40), s->bullet = 1;
		break;
	case T_pre:
		s->display = D_BLOCK;
		s->m[0] = s->m[2] = em;
		s->white = W_PRE;
		s->family = 2;
		break;
	case T_ul: case T_ol:
		s->display = D_BLOCK;
		for (up = e->c.parent; up >= 0 && web_nodes[up].c.tag != T_ul && web_nodes[up].c.tag != T_ol; up = web_nodes[up].c.parent)
			;
		s->m[0] = s->m[2] = up >= 0 ? 0 : em;
		s->p[3] = web_scaled(40);
		s->bullet = t == T_ol ? 2 : 1;
		break;
	case T_li:
		s->display = D_LIST_ITEM;
		break;
	case T_dd:
		s->display = D_BLOCK;
		s->m[3] = web_scaled(40);
		break;
	case T_center:
		s->align = 1;
		/* fall through */
	case T_html: case T_div: case T_dt: case T_main: case T_nav: case T_aside: case T_figcaption: case T_address:
	case T_details: case T_summary: case T_section: case T_article: case T_header: case T_footer: case T_form:
	case T_legend: case T_hgroup: case T_search: case T_dialog: case T_optgroup:
		s->display = D_BLOCK;
		if (t == T_address)
			s->italic = 1;
		break;
	case T_body:
		s->display = D_BLOCK;
		s->m[0] = s->m[1] = s->m[2] = s->m[3] = web_scaled(8);
		if ((c = web_attr_colour(node, "text")) >= 0)
			s->colour = c;
		break;
	case T_hr:
		s->display = D_BLOCK;
		s->m[0] = s->m[2] = em / 2;
		s->border_w[0] = 1;
		s->border_c[0] = 0x808080;
		break;
	case T_table:
		s->display = D_TABLE;
		s->white = W_NORMAL;
		s->align = 0;
		s->font_size = s->font_size;
		if (web_attr_int(node, "border", 0) > 0) {
			s->border_w[0] = s->border_w[1] = s->border_w[2] = s->border_w[3] = 1;
			s->border_c[0] = s->border_c[1] = s->border_c[2] = s->border_c[3] = 0x808080;
		}
		break;
	case T_thead: case T_tbody: case T_tfoot:
		s->display = D_ROW_GROUP;
		break;
	case T_tr:
		s->display = D_ROW;
		break;
	case T_th:
		s->bold = 1;
		s->align = 1;
		/* fall through */
	case T_td:
		s->display = D_CELL;
		s->valign = 2;
		for (up = e->c.parent; up >= 0 && web_nodes[up].c.tag != T_table; up = web_nodes[up].c.parent)
			if (web_nodes[up].c.tag == T_tr && web_attr(up, "valign", &value, &n))
				s->valign = n && tolower((unsigned char)*value) == 't' ? 1 : n && tolower((unsigned char)*value) == 'b' ? 3 : 2;
		if (up != w->table_seen) {
			/* (a table's cells ask the same of it one after another) */
			w->table_seen = up;
			w->table_padding = up >= 0 ? web_attr_int(up, "cellpadding", 1) : 1;
			w->table_border = up >= 0 && web_attr_int(up, "border", 0) > 0;
		}
		c = w->table_padding;
		s->p[0] = s->p[1] = s->p[2] = s->p[3] = c > 1 ? web_scaled(c) : c;
		if (w->table_border) {
			s->border_w[0] = s->border_w[1] = s->border_w[2] = s->border_w[3] = 1;
			s->border_c[0] = s->border_c[1] = s->border_c[2] = s->border_c[3] = 0x808080;
		}
		if (web_attr(node, "nowrap", &value, &n))
			s->white = W_NOWRAP;
		break;
	case T_caption:
		s->display = D_CAPTION;
		s->align = 1;
		break;
	case T_b: case T_strong:
		s->bold = 1;
		break;
	case T_i: case T_em: case T_var: case T_cite: case T_dfn:
		s->italic = 1;
		break;
	case T_u: case T_ins:
		s->decor |= DECOR_UNDER;
		break;
	case T_s: case T_strike: case T_del:
		s->decor |= DECOR_THROUGH;
		break;
	case T_code: case T_tt: case T_kbd: case T_samp:
		s->family = 2;
		break;
	case T_small:
		s->font_size = em * 5 / 6;
		break;
	case T_big:
		s->font_size = em * 6 / 5;
		break;
	case T_sub: case T_sup:
		s->font_size = em * 5 / 6;
		s->valign = t == T_sub ? 4 : 5;
		break;
	case T_mark:
		s->bg = 0xffff00;
		break;
	case T_nobr:
		s->white = W_NOWRAP;
		break;
	case T_a:
		if (e->c.names & css_name_bit("href", 4)) {
			s->colour = 0x0000c8;
			s->decor |= DECOR_UNDER;
		}
		break;
	case T_font:
		if ((c = web_attr_colour(node, "color")) >= 0)
			s->colour = c;
		if (web_attr(node, "size", &value, &n) && n > 0) {
			static const unsigned char sizes[8] = { 9, 9, 11, 13, 15, 20, 24, 32 };

			c = atoi(value);
			if (*value == '+' || *value == '-')
				c += 3;
			s->font_size = sizes[c < 1 ? 1 : c > 7 ? 7 : c];
		}
		if (web_attr(node, "face", &value, &n) && n < 120) {
			char face[128];

			snprintf(face, sizeof face, "%.*s", n, value);
			if ((c = css_family(face)) >= 0)
				s->family = c;
		}
		break;
	case T_button: case T_select: case T_textarea: case T_input:
		s->display = D_INLINE_BLOCK;
		s->font_size = CSS_EM;
		s->bold = s->italic = 0;
		s->colour = 0;
		s->align = t == T_button ? 1 : 0;
		s->white = t == T_textarea ? W_PRE_WRAP : W_NOWRAP;
		s->line_h = 0;
		s->decor = 0;
		if (t == T_button) {
			for (c = 0; c < 4; c++)
				s->border_w[c] = 1, s->border_c[c] = 0x767676, s->p[c] = c & 1 ? 6 : 2;
			s->bg = 0xefefef;
		}
		break;
	case T_img: case T_br: case T_wbr:
		break;
	case T_head: case T_script: case T_style: case T_title: case T_meta: case T_link: case T_template: case T_svg:
	case T_iframe: case T_video: case T_audio: case T_canvas: case T_object: case T_embed: case T_param: case T_source:
	case T_track: case T_area: case T_map: case T_col: case T_colgroup: case T_base: case T_datalist: case T_option:
	case T_math:
		s->display = D_NONE;
		break;
	}
	(void)parent;
	/* what the attributes of old say */
	if (t == T_td || t == T_th || t == T_table || t == T_tr || t == T_body || t == T_div || t == T_p || (t >= T_h1 && t <= T_h6) ||
	    t == T_img || t == T_hr || t == T_center || t == T_col) {
		if ((c = web_attr_colour(node, "bgcolor")) >= 0)
			s->bg = c;
		if (t != T_img && web_attr(node, "align", &value, &n) && n > 0) {
			int a = tolower((unsigned char)*value) == 'c' || tolower((unsigned char)*value) == 'm' ? 1 : tolower((unsigned char)*value) == 'r' ? 2 : 0;

			if (t == T_table) {
				if (a == 1)
					s->m[1] = s->m[3] = CSS_AUTO;
				else
					s->floated = a == 2 ? 2 : tolower((unsigned char)*value) == 'l' ? 1 : 0;
			} else
				s->align = a;
		}
		if (t == T_td || t == T_th) {
			if (web_attr(node, "valign", &value, &n) && n > 0)
				s->valign = tolower((unsigned char)*value) == 't' ? 1 : tolower((unsigned char)*value) == 'b' ? 3 : 2;
		}
		if (t == T_td || t == T_th || t == T_table || t == T_hr || t == T_col)
			s->width = web_attr_len(node, "width", CSS_UNSET) == CSS_UNSET ? s->width : web_attr_len(node, "width", CSS_UNSET);
		if (t == T_td || t == T_th || t == T_tr || t == T_table)
			if ((c = web_attr_len(node, "height", CSS_UNSET)) != CSS_UNSET && c < CSS_PCT / 2)
				s->minh = c;
	}
}

static const struct css_style *
web_style_of(int node)
{
	if (web_nodes[node].c.tag == T_TEXT)
		node = web_nodes[node].c.parent;
	return &web_styles[web_nodes[node].style];
}

/* An element is, or is no longer, one that the element being styled is inside. */
static void
web_inside(struct web_walk *w, int node, int by)
{
	const struct web_node *e = &web_nodes[node];
	int k;

	w->inside[e->c.tag] += by;
	if (e->c.id)
		w->inside[e->c.id] += by;
	for (k = 0; k < e->c.classes; k++)
		w->inside[css_classes[e->c.cls + k]] += by;
}

/* The walk at element i: of the elements it was inside, those that are not i's go. */
static void
web_walk_to(struct web_walk *w, int i)
{
	const struct web_node *e = &web_nodes[i];
	int k;

	for (k = w->depth - 1; k >= 0 && w->stack[k] != e->c.parent; k--)
		;
	if (k < 0 && e->c.parent >= 0) {
		/* its parent is the element before it, not counted yet: that one's parent is */
		for (k = w->depth - 1; k >= 0 && w->stack[k] != web_nodes[e->c.parent].c.parent; k--)
			;
		while (w->depth > k + 1)
			web_inside(w, w->stack[--w->depth], -1);
		if (w->depth < WEB_DEPTH)
			web_inside(w, w->stack[w->depth++] = e->c.parent, 1);
	} else {
		while (w->depth > k + 1)
			web_inside(w, w->stack[--w->depth], -1);
	}
}

/* Element i's style, its parent's made (its slot numbered: web_style_all). It writes the node
 * and its style, and reads the rest. */
static void
web_style_one(struct web_walk *w, int i)
{
	struct web_node *e = &web_nodes[i];
	struct css_style *s = &web_styles[e->style];
	struct css_decl own[64];
	const struct css_style *parent;
	const char *value;
	uint64_t locked = 0;
	int found[64], n, k, count, parent_px, t = e->c.tag, plain_size, plain_margin, plain_align;

	e->minw = e->maxw = -1;
	e->flags &= N_OPEN;
	if (e->c.parent >= 0) {
		parent = &web_styles[web_nodes[e->c.parent].style];
		*s = *parent;
	} else {
		memset(s, 0, sizeof *s);
		s->font_size = CSS_EM;
		s->family = 1;		/* a page that names no face is in the one with serifs */
		parent = NULL;
	}
	/* what is inside passes some on; the tag says some; then the sheets; then itself */
	s->display = D_INLINE;
	s->position = s->clipped = s->floated = s->clear = s->column = s->wrap = s->justify = s->items = s->valign = s->border_box = 0;
	s->bg = CSS_NONE;
	s->grid = -1;
	s->grow = 0;
	s->basis = CSS_AUTO;
	s->gap = s->row_gap = 0;
	s->width = s->height = s->maxw = CSS_UNSET;
	s->minw = s->minh = 0;
	for (k = 0; k < 4; k++) {
		s->m[k] = s->p[k] = 0;
		s->border_w[k] = 0;
		s->border_c[k] = CSS_CURRENT;
	}
	if (parent && (parent->decor & DECOR_UNDER) && t != T_a)
		;	/* (an underline goes through what is inside) */
	parent_px = s->font_size;
	web_plain_style(w, i, s, parent);
	plain_size = s->font_size;
	plain_margin = s->m[0];
	plain_align = s->align;
	count = css_rule_count ? css_find(&web_nodes[0].c, i, found, 64, w->inside) : 0;
	n = web_attr(i, "style", &value, &k) ? css_body(value, k, own, 64, 0, 0) : 0;
	/* (css_find walks the tree as an array of css_node: web_node begins with one) */
	for (k = 0; k < count; k++)
		css_apply(s, css_decls + css_rules[found[k]].decl, css_rules[found[k]].decls, 1, parent_px, &locked);
	css_apply(s, own, n, 1, parent_px, &locked);
	if (s->font_size != plain_size && (t >= T_h1 && t <= T_h6) && plain_margin == s->m[0])
		s->m[0] = s->m[2] = s->font_size * 2 / 3;
	for (k = 0; k < count; k++)
		css_apply(s, css_decls + css_rules[found[k]].decl, css_rules[found[k]].decls, 0, parent_px, &locked);
	css_apply(s, own, n, 0, parent_px, &locked);
	if (i == 0)
		css_root_px = s->font_size;
	if (web_attr(i, "hidden", &value, &k) || (s->position && s->clipped) || s->hidden)
		s->display = D_NONE;
	if (t == T_input && web_attr(i, "type", &value, &k) && k == 6 && !strncasecmp(value, "hidden", 6))
		s->display = D_NONE;
	if (t == T_dialog && !web_attr(i, "open", &value, &k))
		s->display = D_NONE;
	if (t == T_noscript && e->c.parent >= 0 && web_nodes[e->c.parent].c.tag == T_head)
		s->display = D_NONE;
	if (s->floated && (s->display == D_INLINE || s->display == D_INLINE_BLOCK))
		s->display = D_BLOCK;
	if (t == T_center || (t == T_div && s->align == 1 && web_attr(i, "align", &value, &k)) || (t == T_td && plain_align == 1))
		e->flags |= N_CENTER;
	if (t == T_br || t == T_img || t == T_wbr)
		s->display = s->display == D_NONE ? D_NONE : D_INLINE;
	/* margins that pull a box over its neighbours are none here, and a float that is
	 * placed by one (a side column) is a box under what is before it */
	for (k = 0; k < 4; k++)
		if (s->m[k] < 0 && s->m[k] != CSS_AUTO) {
			s->m[k] = 0;
			if (k & 1)
				s->floated = 0;
		}
	s->face = web_face_for(s->family, s->bold, s->font_size);
}

/* The elements inside `root` after it, styled by a walk of their own (`inside` all zeros, and so
 * again at the end; root's style made). Another core may run it meanwhile: it writes those alone. */
static void
web_style_range(unsigned short *inside, int root, int end)
{
	struct web_walk w;
	int chain[WEB_DEPTH], n = 0, i;

	w.inside = inside;
	w.depth = 0;
	w.table_seen = -2;
	for (i = root; i >= 0 && n < WEB_DEPTH; i = web_nodes[i].c.parent)
		chain[n++] = i;
	while (n > 0)
		web_inside(&w, w.stack[w.depth++] = chain[--n], 1);
	for (i = root + 1; i < end; i++)
		if (web_nodes[i].c.tag != T_TEXT) {
			web_walk_to(&w, i);
			web_style_one(&w, i);
		}
	while (w.depth > 0)
		web_inside(&w, w.stack[--w.depth], -1);
}

/* Whether an element inside root names a grid in its style attribute (its columns are kept in
 * css_text, which grows). */
static int
web_grid_inside(int root, int end)
{
	const char *value;
	int i, n;

	for (i = root + 1; i < end; i++)
		if (web_nodes[i].c.tag != T_TEXT && web_attr(i, "style", &value, &n) && memmem(value, n, "grid", 4))
			return 1;
	return 0;
}

/* A program with worker cores may style an element's inside elsewhere: given an element whose
 * style is made and where its inside ends, 1 when web_style_range(root, end) is, or will be,
 * run on another core; (-1, -1) when all of that must be done. NULL: all here. */
static int (*web_style_elsewhere)(int root, int end);
#define WEB_STYLE_LEAST 48	/* elements inside: fewer are styled here */

/* Works out every element's style, parents before what is inside them. */
static void
web_style_all(void)
{
	struct web_walk w;
	int *end = NULL, i, p;

	web_styles = web_grow(web_styles, &web_style_room, web_node_count + 1, sizeof *web_styles);
	css_node_size = sizeof(struct web_node);
	css_root_px = CSS_EM;
	/* every element's slot first, in the tree's order: the walks on other cores need them */
	for (i = 0, web_style_count = 0; i < web_node_count; i++)
		if (web_nodes[i].c.tag != T_TEXT)
			web_nodes[i].style = web_style_count++;
	if (web_style_elsewhere && (end = malloc(web_node_count * sizeof *end)) != NULL) {
		/* where each element's inside ends (its last descendant's next) */
		for (i = 0; i < web_node_count; i++)
			end[i] = i + 1;
		for (i = web_node_count - 1; i > 0; i--)
			if ((p = web_nodes[i].c.parent) >= 0 && end[i] > end[p])
				end[p] = end[i];
	}
	w.inside = css_inside;
	w.depth = 0;
	w.table_seen = -2;
	for (i = 0; i < web_node_count; i++) {
		if (web_nodes[i].c.tag == T_TEXT)
			continue;
		web_walk_to(&w, i);
		web_style_one(&w, i);
		/* (an inside with a grid of its own written in a style attribute grows css_text: here) */
		if (end && i > 0 && end[i] - i > WEB_STYLE_LEAST && w.depth < WEB_DEPTH - 8 && !web_grid_inside(i, end[i]) &&
		    web_style_elsewhere(i, end[i]))
			i = end[i] - 1;
	}
	if (end)
		web_style_elsewhere(-1, -1);
	free(end);
	while (w.depth > 0)
		web_inside(&w, w.stack[--w.depth], -1);
}

static int
web_block_level(int display)
{
	return display == D_BLOCK || display == D_FLEX || display == D_GRID || display == D_TABLE || display == D_LIST_ITEM ||
	       display == D_ROW || display == D_ROW_GROUP || display == D_CELL || display == D_CAPTION;
}

/* ---- the page's style sheets ---- */

#define WEB_SHEETS 16
enum { SHEET_WANTED, SHEET_HERE, SHEET_FAILED, SHEET_BY_HAND };
/* A sheet of the page, in its order: text of its own, or a file somebody has to bring. */
struct web_sheet { char address[256]; char *text; int length, state, own; long asked; };
static struct web_sheet web_sheets[WEB_SHEETS];
static int web_sheet_count;

/* Finds the page's sheets: text between style tags, and the files it links to (WANTED until
 * the program has brought them). Comments are passed over, and what only script-less pages
 * of the head would have is taken: there are no scripts here. */
static void
web_find_sheets(const char *html, int n, int view)
{
	int i, end;

	for (i = 0; i < web_sheet_count; i++)
		if (web_sheets[i].own)
			free(web_sheets[i].text);
	web_sheet_count = 0;
	web_base[0] = 0;
	css_view = view;
	for (i = 0; i + 7 < n && web_sheet_count < WEB_SHEETS; i++) {
		struct web_sheet *sh = &web_sheets[web_sheet_count];
		const char *tag = memchr(html + i, '<', (size_t)(n - i)), *value;
		char href[600], whole[256];
		int vn;

		if (!tag)
			break;
		i = (int)(tag - html);
		if (i + 7 >= n)
			break;
		if (!strncmp(html + i, "<!--", 4)) {
			const char *close = memmem(html + i, n - i, "-->", 3);

			if (!close)
				break;
			i = (int)(close - html) + 2;
			continue;
		}
		/* (by the tag's first letter before anything is compared: a page is thousands of tags) */
		if (((html[i + 1] | 32) != 's' && (html[i + 1] | 32) != 'l' && (html[i + 1] | 32) != 'b') ||
		    (strncasecmp(html + i, "<style", 6) && strncasecmp(html + i, "<link", 5) && strncasecmp(html + i, "<base", 5)))
			continue;
		for (end = i; end < n && html[end] != '>'; end++)
			;
		memset(sh, 0, sizeof *sh);
		if (tolower((unsigned char)html[i + 1]) == 'b') {
			struct web_node fake;

			/* (web_attr_text reads a node: one made for the purpose) */
			memset(&fake, 0, sizeof fake);
			fake.c.attrs = html + i + 5;
			fake.c.attrs_n = end - i - 5;
			if (!web_base[0] && css_attribute(fake.c.attrs, fake.c.attrs_n, "href", &value, &vn) && vn < 250) {
				snprintf(href, sizeof href, "%.*s", vn, value);
				if (web_resolve(href, whole))
					strcpy(web_base, whole);
			}
			i = end;
			continue;
		}
		if (tolower((unsigned char)html[i + 1]) == 's') {
			const char *close = NULL;
			int k;

			for (k = end; k + 7 < n && !close; k++)
				if (html[k] == '<' && !strncasecmp(html + k, "</style", 7))
					close = html + k;
			if (!close)
				break;
			if (!css_attribute(html + i + 6, end - i - 6, "media", &value, &vn) || css_media(value, vn)) {
				sh->text = (char *)html + end + 1;
				sh->length = (int)(close - sh->text);
				sh->state = SHEET_HERE;
				web_sheet_count++;
			}
			i = (int)(close - html);
			continue;
		}
		if (css_attribute(html + i + 5, end - i - 5, "rel", &value, &vn) && memmem(value, vn, "stylesheet", 10) &&
		    !memmem(value, vn, "alternate", 9) &&
		    (!css_attribute(html + i + 5, end - i - 5, "media", &value, &vn) || css_media(value, vn)) &&
		    css_attribute(html + i + 5, end - i - 5, "href", &value, &vn) && vn < 590) {
			int k = 0, j;

			for (j = 0; j < vn; j++) {
				if (j + 4 < vn && !strncmp(value + j, "&amp;", 5))
					j += 4, href[k++] = '&';
				else
					href[k++] = value[j];
			}
			href[k] = 0;
			if (web_resolve(href, whole)) {
				strcpy(sh->address, whole);
				sh->state = SHEET_WANTED;
				web_sheet_count++;
			}
		}
		i = end;
	}
}

/* Reads every sheet that is here, again from the first: a rule may use a custom property that
 * a sheet which came later sets. Not when they are, byte for byte, the sheets read last for
 * this width: a site's next page has the same ones, and their rules are still there. */
static void
web_read_sheets(int view, int view_h)
{
	static unsigned read_last;
	unsigned sum = 2166136261u ^ (unsigned)view;
	int pass, i, k;

	for (i = 0; i < web_sheet_count; i++) {
		const struct web_sheet *sh = &web_sheets[i];
		const uint32_t *word = (const uint32_t *)(sh->text + (-(uintptr_t)sh->text & 3));
		int words = sh->state == SHEET_HERE && sh->length > 8 ? (sh->length - 4) / 4 : 0;

		if (sh->state != SHEET_HERE)
			continue;
		/* (a word at a time, from the first whole one: every byte but a few at the ends) */
		sum = (sum ^ (unsigned)sh->length) * 16777619u;
		for (k = 0; k < words; k++)
			sum = (sum ^ word[k]) * 16777619u;
	}
	if (sum == read_last && css_rule_count)
		return;
	read_last = sum;
	css_reset(view, view_h);
	css_order = 0;
	for (pass = 1; pass >= 0; pass--)
		for (i = 0; i < web_sheet_count; i++)
			if (web_sheets[i].state == SHEET_HERE && (!pass || memmem(web_sheets[i].text, web_sheets[i].length, "--", 2)))
				css_parse(web_sheets[i].text, web_sheets[i].length, pass);	/* (a sheet with no custom property is read once) */
}

/* ---- what the image has of the network already (web/preload.py) ---- */

#ifndef WEB_CACHE_DIR
#define WEB_CACHE_DIR "/usr/share"
#endif

static char *web_cache_text;
static int web_cache_size = -1;

/* The file the image keeps for an address (a page P, a sheet S, a picture I), and a
 * picture's size. 0: it keeps none. */
static int
web_cache_find(const char *address, int kind, char *file, int room, int *w, int *h)
{
	int lo = 0, hi;

	if (web_cache_size < 0) {
		char path[400];
		FILE *idx;
		long size;

		web_cache_size = 0;
		snprintf(path, sizeof path, "%s/web-cache.idx", WEB_CACHE_DIR);
		if ((idx = fopen(path, "rb")) == NULL)
			return 0;
		fseek(idx, 0, SEEK_END);
		size = ftell(idx);
		rewind(idx);
		web_cache_text = malloc(size + 1);
		web_cache_size = (int)fread(web_cache_text, 1, size, idx);
		fclose(idx);
		web_cache_text[web_cache_size] = 0;
	}
	/* its lines are in the order of their addresses: halved until the line is found */
	hi = web_cache_size;
	while (lo < hi) {
		int mid = (lo + hi) / 2, start, end, tabs = 0, order, at;
		const char *text = web_cache_text;

		for (start = mid; start > lo && text[start - 1] != '\n'; start--)
			;
		for (end = start; end < hi && text[end] != '\n'; end++)
			;
		if (text[start] == '#') {
			lo = end + 1;
			continue;
		}
		for (at = start; at < end && tabs < 4; at++)
			tabs += text[at] == '\t';
		order = strncmp(address, text + at, end - at);
		if (!order && address[end - at])
			order = 1;
		if (!order) {
			char name[64];
			int pw = 0, ph = 0;

			if (text[start] != kind || sscanf(text + start + 2, "%63s %d %d", name, &pw, &ph) != 3)
				return 0;
			snprintf(file, room, "%s/%s", WEB_CACHE_DIR, name);
			if (w)
				*w = pw, *h = ph;
			return 1;
		}
		if (order < 0)
			hi = start;
		else
			lo = end + 1;
	}
	return 0;
}

#endif
