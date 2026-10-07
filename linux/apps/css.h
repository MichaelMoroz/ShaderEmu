/*
 * Style sheets for nxweb (docs/fetch.md): rules read from text, kept by the last part of their
 * selector, and found again for an element from its tag, id and classes. Lengths come out in
 * this machine's pixels; what is not understood is left out, never guessed.
 */
#ifndef NXWEB_CSS_H
#define NXWEB_CSS_H

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CSS_NONE (-2)		/* no colour given */
#define CSS_CLEAR (-1)		/* transparent */
#define CSS_AUTO (-30000)
#define CSS_UNSET (-30001)
#define CSS_EM 13		/* our font against the 16 pixels pages are written for */
#define CSS_ATOMS 8192

#define CSS_TAGS(X) \
	X(html) X(body) X(p) X(div) X(table) X(blockquote) X(section) X(article) X(header) X(footer) X(form) \
	X(tr) X(dt) X(dd) X(td) X(th) X(h1) X(h2) X(h3) X(h4) X(h5) X(h6) X(ul) X(ol) X(li) X(hr) X(pre) X(b) \
	X(strong) X(a) X(img) X(br) X(script) X(style) X(head) X(svg) X(noscript) X(template) X(title) X(main) \
	X(nav) X(aside) X(figure) X(figcaption) X(center) X(dl) X(address) X(details) X(summary) X(fieldset) \
	X(caption) X(input) X(meta) X(link) X(source) X(wbr) X(area) X(col) X(embed) X(param) X(track) X(base)
#define X(name) T_##name,
enum { T_NONE, CSS_TAGS(X) T_COUNT };
#undef X

enum { P_DISPLAY, P_COLOR, P_BG, P_BOLD, P_MT, P_MB, P_ML, P_MR, P_PT, P_PB, P_PL, P_PR, P_ALIGN, P_WIDTH, P_MAXW,
       P_UNDERLINE, P_BORDER, P_BORDER_T, P_BORDER_B, P_BULLET, P_POSITION, P_CLIP, P_PRE };
enum { U_PX, U_PERCENT, U_AUTO };
enum { D_INLINE, D_BLOCK, D_NONE };

struct css_decl { unsigned char prop, unit, important; int value; };
/* One step of a selector: `child` says the step to its left must be the parent itself. */
struct css_part { unsigned short tag, id, cls[3]; unsigned char classes, child; };
struct css_rule { int part, decl, order, weight, next; unsigned char parts, decls; };
struct css_elem { unsigned short tag, id, cls[8]; unsigned char classes; };

/* What an element looks like. The first group passes to what is inside it. */
struct css_style {
	int colour;
	unsigned char bold, align, underline, bullet, pre;
	unsigned char display, absolute, clipped;
	int bg, border, border_t, border_b;
	short mt, mb, ml, mr, pt, pb, pl, pr, width, maxw;
};

static char *css_names;			/* every atom's text */
static int css_names_length, css_names_room;
static int css_name_at[CSS_ATOMS];
static unsigned short css_hash[CSS_ATOMS * 2];
static int css_atoms;
static int css_by_id[CSS_ATOMS], css_by_class[CSS_ATOMS], css_by_tag[CSS_ATOMS], css_any;
static int css_var_at[CSS_ATOMS];	/* a custom property's value in css_text, or -1 */
static unsigned char css_var_root[CSS_ATOMS];
static char *css_text;
static int css_text_length, css_text_room;
static struct css_rule *css_rules;
static int css_rule_count, css_rule_room;
static struct css_part *css_parts;
static int css_part_count, css_part_room;
static struct css_decl *css_decls;
static int css_decl_count, css_decl_room;
static int css_view;			/* the width media queries are answered with */

static void *
css_grow(void *block, int *room, int need, int size)
{
	if (need <= *room)
		return block;
	*room = need * 2 + 256;
	return realloc(block, (size_t)*room * size);
}

/* The number of a name, 0 for one not seen when `add` is 0. `fold` is for tags: lower case. */
static int
css_atom(const char *text, int n, int add, int fold)
{
	char low[32];
	unsigned h = 2166136261u;
	int i, slot;

	if (n <= 0 || n > 200)
		return 0;
	if (fold) {
		if (n > (int)sizeof low)
			return 0;
		for (i = 0; i < n; i++)
			low[i] = tolower((unsigned char)text[i]);
		text = low;
	}
	for (i = 0; i < n; i++)
		h = (h ^ (unsigned char)text[i]) * 16777619u;
	for (slot = h % (CSS_ATOMS * 2); css_hash[slot]; slot = (slot + 1) % (CSS_ATOMS * 2)) {
		const char *have = css_names + css_name_at[css_hash[slot]];

		if (!strncmp(have, text, n) && !have[n])
			return css_hash[slot];
	}
	if (!add || css_atoms >= CSS_ATOMS - 1)
		return 0;
	css_names = css_grow(css_names, &css_names_room, css_names_length + n + 1, 1);
	memcpy(css_names + css_names_length, text, n);
	css_names[css_names_length + n] = 0;
	css_name_at[++css_atoms] = css_names_length;
	css_names_length += n + 1;
	css_hash[slot] = css_atoms;
	return css_atoms;
}

static void
css_reset(int view)
{
	static const char *const tags[] = {
#define X(name) #name,
		CSS_TAGS(X)
#undef X
	};
	int i;

	css_atoms = css_names_length = css_text_length = css_rule_count = css_part_count = css_decl_count = 0;
	css_view = view;
	css_any = -1;
	memset(css_hash, 0, sizeof css_hash);
	for (i = 0; i < CSS_ATOMS; i++)
		css_by_id[i] = css_by_class[i] = css_by_tag[i] = css_var_at[i] = -1;
	memset(css_var_root, 0, sizeof css_var_root);
	for (i = 0; i < T_COUNT - 1; i++)
		css_atom(tags[i], strlen(tags[i]), 1, 0);	/* in the enum's order: T_p is "p" */
}

/* ---- values ---- */

/* A number in thousandths; moves past it. */
static int
css_number(const char **at, int *milli)
{
	const char *p = *at;
	int sign = 1, v = 0, digits = 0, scale = 100;

	if (*p == '-' || *p == '+')
		sign = *p++ == '-' ? -1 : 1;
	for (; isdigit((unsigned char)*p); p++, digits++)
		if (v < 100000)
			v = v * 10 + (*p - '0');
	v *= 1000;
	if (*p == '.')
		for (p++; isdigit((unsigned char)*p); p++, digits++, scale /= 10)
			v += (*p - '0') * scale;
	if (!digits)
		return 0;
	*milli = sign * v;
	*at = p;
	return 1;
}

/* A length: pixels of ours, a percentage, or auto. Moves past it. */
static int
css_length(const char **at, int *unit, int *value)
{
	const char *p = *at;
	int m;

	while (isspace((unsigned char)*p))
		p++;
	if (!strncasecmp(p, "auto", 4)) {
		*unit = U_AUTO;
		*value = 0;
		*at = p + 4;
		return 1;
	}
	if (!css_number(&p, &m))
		return 0;
	*unit = U_PX;
	if (!strncasecmp(p, "px", 2))
		*value = (m * CSS_EM / 16 + (m < 0 ? -500 : 500)) / 1000, p += 2;
	else if (!strncasecmp(p, "rem", 3))
		*value = m / 1000 * CSS_EM + m % 1000 * CSS_EM / 1000, p += 3;
	else if (!strncasecmp(p, "em", 2))
		*value = m / 1000 * CSS_EM + m % 1000 * CSS_EM / 1000, p += 2;
	else if (!strncasecmp(p, "pt", 2))
		*value = m * CSS_EM / 12 / 1000, p += 2;
	else if (!strncasecmp(p, "ch", 2) || !strncasecmp(p, "ex", 2))
		*value = m * 6 / 1000, p += 2;
	else if (*p == '%' || !strncasecmp(p, "vw", 2))
		*unit = U_PERCENT, *value = m / 1000, p += *p == '%' ? 1 : 2;
	else if (m == 0)
		*value = 0;
	else
		return 0;
	if (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')' && *p != '/')
		return 0;	/* calc() and the like */
	*at = p;
	return 1;
}

static int
css_hsl(int h, int s, int l)
{
	int c[3], i, q, p;

	h = ((h % 360) + 360) % 360;
	q = l < 500 ? l * (1000 + s) / 1000 : l + s - l * s / 1000;
	p = 2 * l - q;
	for (i = 0; i < 3; i++) {
		int t = (h + 120 - 120 * i + 360) % 360;

		c[i] = t < 60 ? p + (q - p) * t / 60 : t < 180 ? q : t < 240 ? p + (q - p) * (240 - t) / 60 : p;
		c[i] = c[i] * 255 / 1000;
		c[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : c[i];
	}
	return c[0] << 16 | c[1] << 8 | c[2];
}

/* A colour as 0xRRGGBB (one that lets what is behind through is mixed with white), CSS_CLEAR, or
 * CSS_NONE for text that is not a colour. */
static int
css_colour(const char *v)
{
	static const struct { const char *name; int rgb; } named[] = {
		{ "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "blue", 0x0000ff },
		{ "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "maroon", 0x800000 }, { "navy", 0x000080 },
		{ "teal", 0x008080 }, { "olive", 0x808000 }, { "purple", 0x800080 }, { "orange", 0xffa500 }, { "yellow", 0xffff00 },
		{ "lime", 0x00ff00 }, { "aqua", 0x00ffff }, { "cyan", 0x00ffff }, { "fuchsia", 0xff00ff }, { "magenta", 0xff00ff },
		{ "lightgray", 0xd3d3d3 }, { "lightgrey", 0xd3d3d3 }, { "darkgray", 0xa9a9a9 }, { "darkgrey", 0xa9a9a9 },
		{ "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "whitesmoke", 0xf5f5f5 }, { "gainsboro", 0xdcdcdc },
		{ "pink", 0xffc0cb }, { "brown", 0xa52a2a }, { "gold", 0xffd700 }, { "tomato", 0xff6347 }, { "crimson", 0xdc143c },
		{ "indigo", 0x4b0082 }, { "violet", 0xee82ee }, { "coral", 0xff7f50 }, { "salmon", 0xfa8072 }, { "khaki", 0xf0e68c },
		{ "beige", 0xf5f5dc }, { "ivory", 0xfffff0 }, { "snow", 0xfffafa }, { "lavender", 0xe6e6fa }, { "steelblue", 0x4682b4 },
		{ "royalblue", 0x4169e1 }, { "skyblue", 0x87ceeb }, { "darkblue", 0x00008b }, { "darkred", 0x8b0000 },
		{ "darkgreen", 0x006400 }, { "forestgreen", 0x228b22 }, { "seagreen", 0x2e8b57 }, { "slategray", 0x708090 },
		{ "slategrey", 0x708090 }, { "aliceblue", 0xf0f8ff }, { "ghostwhite", 0xf8f8ff }, { "dodgerblue", 0x1e90ff },
	};
	int i, n, rgb, alpha = 1000;

	while (isspace((unsigned char)*v))
		v++;
	if (*v == '#') {
		unsigned long hex;
		char *end;

		hex = strtoul(v + 1, &end, 16);
		n = (int)(end - v - 1);
		if (n == 3 || n == 4) {
			if (n == 4)
				alpha = (int)(hex & 15) * 1000 / 15, hex >>= 4;
			rgb = (int)((hex >> 8 & 15) * 17 << 16 | (hex >> 4 & 15) * 17 << 8 | (hex & 15) * 17);
		} else if (n == 6 || n == 8) {
			if (n == 8)
				alpha = (int)(hex & 255) * 1000 / 255, hex >>= 8;
			rgb = (int)(hex & 0xffffff);
		} else {
			return CSS_NONE;
		}
	} else if (!strncasecmp(v, "rgb", 3) || !strncasecmp(v, "hsl", 3)) {
		int part[4] = { 0, 0, 0, 1000 }, percent[4] = { 0, 0, 0, 0 }, count = 0, hsl = tolower((unsigned char)*v) == 'h';
		const char *p = strchr(v, '(');

		if (!p)
			return CSS_NONE;
		for (p++; count < 4; count++) {
			while (isspace((unsigned char)*p) || *p == ',' || *p == '/')
				p++;
			if (!css_number(&p, &part[count]))
				break;
			if (*p == '%')
				percent[count] = 1, p++;
			else if (!strncasecmp(p, "deg", 3))
				p += 3;
		}
		if (count < 3)
			return CSS_NONE;	/* var() inside, or words */
		if (hsl) {
			rgb = css_hsl(part[0] / 1000, part[1] / 100, part[2] / 100);
		} else {
			for (i = 0, rgb = 0; i < 3; i++) {
				int c = percent[i] ? part[i] * 255 / 100000 : part[i] / 1000;

				rgb = rgb << 8 | (c < 0 ? 0 : c > 255 ? 255 : c);
			}
		}
		alpha = percent[3] ? part[3] / 100 : part[3];
	} else if (!strncasecmp(v, "transparent", 11)) {
		return CSS_CLEAR;
	} else if (!strncasecmp(v, "light-dark(", 11)) {
		return css_colour(v + 11);	/* this machine's screen is the light one */
	} else {
		for (n = 0; isalpha((unsigned char)v[n]); n++)
			;
		for (i = 0; i < (int)(sizeof named / sizeof named[0]); i++)
			if ((int)strlen(named[i].name) == n && !strncasecmp(v, named[i].name, n))
				return named[i].rgb;
		return CSS_NONE;
	}
	if (alpha <= 0)
		return CSS_CLEAR;
	if (alpha < 1000) {
		int out = 0;

		for (i = 16; i >= 0; i -= 8)
			out = out << 8 | ((rgb >> i & 255) * alpha + 255 * (1000 - alpha)) / 1000;
		rgb = out;
	}
	return rgb;
}

/* The first colour among a value's words ("1px solid #ccc"), or CSS_NONE. */
static int
css_colour_in(const char *v)
{
	char word[64];
	int depth, n, c;

	while (*v) {
		while (isspace((unsigned char)*v))
			v++;
		for (n = 0, depth = 0; *v && (depth || !isspace((unsigned char)*v)); v++) {
			depth += *v == '(' ? 1 : *v == ')' ? -1 : 0;
			if (n < (int)sizeof word - 1)
				word[n++] = *v;
		}
		word[n] = 0;
		if (n && (c = css_colour(word)) != CSS_NONE)
			return c;
	}
	return CSS_NONE;
}

/* ---- declarations ---- */

/* Writes `value` with every var() replaced by what it stands for; 0 if one stands for nothing. */
static int
css_substitute(const char *value, int n, char *out, int room, int depth)
{
	int i = 0, k = 0;

	while (i < n) {
		if (i + 4 < n && !strncmp(value + i, "var(", 4) && depth < 6) {
			int name = i + 4, name_n, end, level = 1, comma = -1, atom, got = 0;

			while (name < n && isspace((unsigned char)value[name]))
				name++;
			for (end = name; end < n && level; end++) {
				level += value[end] == '(' ? 1 : value[end] == ')' ? -1 : 0;
				if (value[end] == ',' && level == 1 && comma < 0)
					comma = end;
			}
			if (level)
				return 0;
			name_n = (comma < 0 ? end - 1 : comma) - name;
			while (name_n > 0 && isspace((unsigned char)value[name + name_n - 1]))
				name_n--;
			atom = css_atom(value + name, name_n, 0, 0);
			if (atom && css_var_at[atom] >= 0) {
				const char *is = css_text + css_var_at[atom];

				got = css_substitute(is, strlen(is), out + k, room - k, depth + 1);
			}
			if (!got && comma >= 0)
				got = css_substitute(value + comma + 1, end - 1 - comma - 1, out + k, room - k, depth + 1);
			if (!got)
				return 0;
			k += strlen(out + k);
			i = end;
			continue;
		}
		if (k >= room - 1)
			return 0;
		out[k++] = value[i++];
	}
	out[k] = 0;
	return k > 0;
}

/* One property and its value as what we keep of it: up to four entries of `out`. */
static int
css_declaration(const char *name, int nn, const char *v, struct css_decl *out)
{
	static const struct { const char *name; unsigned char first, sides; } boxes[] = {
		{ "margin", P_MT, 4 }, { "padding", P_PT, 4 }, { "margin-top", P_MT, 1 }, { "margin-bottom", P_MB, 1 },
		{ "margin-left", P_ML, 1 }, { "margin-right", P_MR, 1 }, { "padding-top", P_PT, 1 }, { "padding-bottom", P_PB, 1 },
		{ "padding-left", P_PL, 1 }, { "padding-right", P_PR, 1 }, { "margin-block", P_MT, 2 }, { "margin-inline", P_ML, 2 },
		{ "padding-block", P_PT, 2 }, { "padding-inline", P_PL, 2 }, { "margin-block-start", P_MT, 1 },
		{ "margin-block-end", P_MB, 1 }, { "margin-inline-start", P_ML, 1 }, { "margin-inline-end", P_MR, 1 },
		{ "padding-block-start", P_PT, 1 }, { "padding-block-end", P_PB, 1 }, { "padding-inline-start", P_PL, 1 },
		{ "padding-inline-end", P_PR, 1 }, { "width", P_WIDTH, 1 }, { "max-width", P_MAXW, 1 },
	};
	int n = 0, c, i;

#define IS(text) (nn == (int)sizeof(text) - 1 && !strncasecmp(name, text, nn))
#define PUT(p, u, val) (out[n].prop = (p), out[n].unit = (u), out[n].important = 0, out[n++].value = (val))
	for (i = 0; i < (int)(sizeof boxes / sizeof boxes[0]); i++) {
		int unit[4], value[4], count = 0;

		if (nn != (int)strlen(boxes[i].name) || strncasecmp(name, boxes[i].name, nn))
			continue;
		if (boxes[i].first == P_WIDTH || boxes[i].first == P_MAXW) {
			if (!strncasecmp(v, "none", 4)) {
				PUT(boxes[i].first, U_AUTO, 0);
				return n;
			}
		}
		while (count < 4 && css_length(&v, &unit[count], &value[count]))
			count++;
		while (isspace((unsigned char)*v))
			v++;
		if (!count || *v)
			return 0;
		if (boxes[i].sides == 4) {
			/* top, right, bottom, left: the ones not given are the ones across */
			static const unsigned char from[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
			static const unsigned char side[4] = { 0, 3, 1, 2 };	/* P_MT, P_MR, P_MB, P_ML from the first */

			for (c = 0; c < 4; c++)
				PUT(boxes[i].first + side[c], unit[from[count - 1][c]], value[from[count - 1][c]]);
		} else {
			PUT(boxes[i].first, unit[0], value[0]);
			if (boxes[i].sides == 2)
				PUT(boxes[i].first + 1, unit[count > 1], value[count > 1]);
		}
		return n;
	}
	if (IS("display")) {
		PUT(P_DISPLAY, 0, !strncasecmp(v, "none", 4) ? D_NONE :
		    !strncasecmp(v, "inline", 6) || !strncasecmp(v, "contents", 8) || !strncasecmp(v, "table-cell", 10) ? D_INLINE : D_BLOCK);
	} else if (IS("visibility")) {
		if (!strncasecmp(v, "hidden", 6) || !strncasecmp(v, "collapse", 8))
			PUT(P_DISPLAY, 0, D_NONE);
	} else if (IS("color")) {
		if ((c = css_colour(v)) >= 0)
			PUT(P_COLOR, 0, c);
	} else if (IS("background-color") || IS("background")) {
		if ((c = css_colour_in(v)) != CSS_NONE)
			PUT(P_BG, 0, c);
		else if (!strncasecmp(v, "none", 4))
			PUT(P_BG, 0, CSS_CLEAR);
	} else if (IS("font-weight")) {
		PUT(P_BOLD, 0, !strncasecmp(v, "bold", 4) || atoi(v) >= 600);
	} else if (IS("font")) {
		PUT(P_BOLD, 0, strstr(v, "bold") != NULL || strstr(v, " 700") != NULL);
	} else if (IS("text-align")) {
		PUT(P_ALIGN, 0, !strncasecmp(v, "center", 6) ? 1 : !strncasecmp(v, "right", 5) || !strncasecmp(v, "end", 3) ? 2 : 0);
	} else if (IS("text-decoration") || IS("text-decoration-line")) {
		if (strstr(v, "underline"))
			PUT(P_UNDERLINE, 0, 1);
		else if (strstr(v, "none"))
			PUT(P_UNDERLINE, 0, 0);
	} else if (IS("border") || IS("border-top") || IS("border-bottom")) {
		int prop = nn == 6 ? P_BORDER : name[7] == 't' || name[7] == 'T' ? P_BORDER_T : P_BORDER_B;

		if (!strncasecmp(v, "none", 4) || !strncmp(v, "0", 1))
			PUT(prop, 0, CSS_CLEAR);
		else if (!strstr(v, "none") && !strstr(v, "hidden"))
			PUT(prop, 0, (c = css_colour_in(v)) != CSS_NONE ? c : 0x999999);
	} else if (IS("list-style") || IS("list-style-type")) {
		PUT(P_BULLET, 0, strstr(v, "none") == NULL);
	} else if (IS("position")) {
		PUT(P_POSITION, 0, !strncasecmp(v, "absolute", 8) || !strncasecmp(v, "fixed", 5));
	} else if (IS("clip") || IS("clip-path")) {
		PUT(P_CLIP, 0, strncasecmp(v, "auto", 4) && strncasecmp(v, "none", 4));
	} else if (IS("white-space")) {
		PUT(P_PRE, 0, !strncasecmp(v, "pre", 3));
	}
#undef IS
#undef PUT
	return n;
}

/* The index just past the } that closes a block whose inside starts at `i`. */
static int
css_close(const char *s, int n, int i)
{
	int depth = 1;

	for (; i < n; i++) {
		if (s[i] == '"' || s[i] == '\'') {
			char quote = s[i];

			for (i++; i < n && s[i] != quote; i++)
				if (s[i] == '\\')
					i++;
		} else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
			for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); i++)
				;
			i++;
		} else if (s[i] == '{') {
			depth++;
		} else if (s[i] == '}' && --depth == 0) {
			return i + 1;
		}
	}
	return n;
}

/* The declarations between two braces (or of a style attribute). With `vars`, only custom
 * properties are taken, into the table; otherwise they are what is left out. */
static int
css_body(const char *s, int n, struct css_decl *out, int room, int vars, int root)
{
	char buffer[400];
	int i = 0, count = 0;

	while (i < n) {
		int name, name_n, value, value_n, depth = 0, important = 0, k;

		while (i < n && (isspace((unsigned char)s[i]) || s[i] == ';'))
			i++;
		if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
			for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); i++)
				;
			i += 2;
			continue;
		}
		name = i;
		while (i < n && s[i] != ':' && s[i] != ';' && s[i] != '{')
			i++;
		if (i >= n)
			break;
		if (s[i] == '{') {	/* a rule inside a rule: not ours */
			i = css_close(s, n, i + 1);
			continue;
		}
		if (s[i] == ';')
			continue;
		for (name_n = i - name; name_n > 0 && isspace((unsigned char)s[name + name_n - 1]); name_n--)
			;
		for (i++; i < n && isspace((unsigned char)s[i]); i++)
			;
		value = i;
		for (; i < n && (depth || s[i] != ';'); i++) {
			if (s[i] == '"' || s[i] == '\'') {
				char quote = s[i];

				for (i++; i < n && s[i] != quote; i++)
					;
			} else {
				depth += s[i] == '(' ? 1 : s[i] == ')' && depth ? -1 : 0;
			}
		}
		if (i > n)
			i = n;
		for (value_n = i - value; value_n > 0 && isspace((unsigned char)s[value + value_n - 1]); value_n--)
			;
		if (value_n >= 10 && !strncasecmp(s + value + value_n - 10, "!important", 10)) {
			important = 1;
			for (value_n -= 10; value_n > 0 && isspace((unsigned char)s[value + value_n - 1]); value_n--)
				;
		}
		if (name_n > 2 && s[name] == '-' && s[name + 1] == '-') {
			int atom;

			if (vars && value_n < 300 && (atom = css_atom(s + name, name_n, 1, 0)) != 0 &&
			    (root || !css_var_root[atom])) {
				css_text = css_grow(css_text, &css_text_room, css_text_length + value_n + 1, 1);
				memcpy(css_text + css_text_length, s + value, value_n);
				css_text[css_text_length + value_n] = 0;
				css_var_at[atom] = css_text_length;
				css_var_root[atom] |= root;
				css_text_length += value_n + 1;
			}
			continue;
		}
		if (vars || value_n <= 0 || value_n >= (int)sizeof buffer || count + 4 > room)
			continue;
		if (memmem(s + value, value_n, "var(", 4)) {
			if (!css_substitute(s + value, value_n, buffer, sizeof buffer, 0))
				continue;
		} else {
			memcpy(buffer, s + value, value_n);
			buffer[value_n] = 0;
		}
		k = css_declaration(s + name, name_n, buffer, out + count);
		while (k-- > 0)
			out[count++].important = important;
	}
	return count;
}

/* ---- rules ---- */

/* Adds a rule for one selector, unless it has something we cannot test (:hover, [type], +). */
static void
css_selector(const char *s, int n, int decl, int decls, int order)
{
	struct css_part parts[8];
	struct css_rule *r;
	int k = 0, used = 0, i = 0, weight = 0, atom, start, *head;

	memset(parts, 0, sizeof parts);
	while (i < n) {
		unsigned char c = s[i];

		if (isspace(c) || c == '>') {
			int child = 0;

			for (; i < n && (isspace((unsigned char)s[i]) || s[i] == '>'); i++)
				child |= s[i] == '>';
			if (i >= n)
				break;
			if (!used || ++k == 8)
				return;
			parts[k].child = child;
			used = 0;
			continue;
		}
		used = 1;
		if (c == '*') {
			i++;
			continue;
		}
		start = c == '.' || c == '#' ? i + 1 : c == ':' ? i + 1 + (i + 1 < n && s[i + 1] == ':') : i;
		for (i = start; i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_'); i++)
			;
		if (i == start)
			return;
		if (c == ':') {
			if (i - start == 4 && !strncmp(s + start, "root", 4))
				parts[k].tag = T_html, weight += 100;
			else if (!(i - start == 4 && !strncmp(s + start, "link", 4)) && !(i - start == 8 && !strncmp(s + start, "any-link", 8)))
				return;
			continue;
		}
		if ((atom = css_atom(s + start, i - start, 1, c != '.' && c != '#')) == 0)
			return;
		if (c == '.') {
			if (parts[k].classes == 3)
				return;
			parts[k].cls[parts[k].classes++] = atom;
			weight += 100;
		} else if (c == '#') {
			parts[k].id = atom;
			weight += 10000;
		} else {
			parts[k].tag = atom;
			weight += 1;
		}
	}
	if (!used)
		return;
	css_parts = css_grow(css_parts, &css_part_room, css_part_count + k + 1, sizeof *css_parts);
	css_rules = css_grow(css_rules, &css_rule_room, css_rule_count + 1, sizeof *css_rules);
	memcpy(css_parts + css_part_count, parts, (k + 1) * sizeof parts[0]);
	r = &css_rules[css_rule_count];
	r->part = css_part_count;
	r->parts = k + 1;
	r->decl = decl;
	r->decls = decls;
	r->order = order;
	r->weight = weight;
	head = parts[k].id ? &css_by_id[parts[k].id] : parts[k].classes ? &css_by_class[parts[k].cls[0]] :
	       parts[k].tag ? &css_by_tag[parts[k].tag] : &css_any;
	r->next = *head;
	*head = css_rule_count++;
	css_part_count += k + 1;
}

/* Whether a media query's list holds for this window. */
static int
css_media(const char *s, int n)
{
	int i = 0, any = 0, ok = 1, negate = 0, seen = 0;

	for (;; i++) {
		if (i >= n || s[i] == ',') {
			any |= seen && (negate ? !ok : ok);
			if (i >= n)
				break;
			ok = 1, negate = seen = 0;
			continue;
		}
		if (isspace((unsigned char)s[i]))
			continue;
		seen = 1;
		if (s[i] == '(') {
			const char *p = s + i + 1;
			int end = i, max, m;

			while (end < n && s[end] != ')')
				end++;
			while (isspace((unsigned char)*p))
				p++;
			max = !strncasecmp(p, "max-", 4);
			if ((max || !strncasecmp(p, "min-", 4)) && (!strncasecmp(p + 4, "width", 5) || !strncasecmp(p + 4, "device-width", 12))) {
				const char *colon = memchr(p, ':', s + end - p);

				if (colon) {
					for (colon++; isspace((unsigned char)*colon); colon++)
						;
					if (css_number(&colon, &m)) {
						m = !strncasecmp(colon, "em", 2) || !strncasecmp(colon, "rem", 3) ? m * 16 / 1000 : m / 1000;
						ok &= max ? css_view <= m : css_view >= m;
					} else {
						ok = 0;
					}
				} else {
					ok = 0;
				}
			} else if (!strncasecmp(p, "prefers-color-scheme", 20)) {
				ok &= memmem(p, s + end - p, "light", 5) != NULL;
			} else if (!strncasecmp(p, "orientation", 11)) {
				ok &= memmem(p, s + end - p, "landscape", 9) != NULL;
			} else {
				ok = 0;
			}
			i = end;
			continue;
		}
		if (!strncasecmp(s + i, "not", 3) && !isalnum((unsigned char)s[i + 3]))
			negate = 1, i += 2;
		else if (!strncasecmp(s + i, "print", 5) || !strncasecmp(s + i, "speech", 6))
			ok = 0;
		while (i + 1 < n && isalnum((unsigned char)s[i + 1]))
			i++;
	}
	return any || n == 0;
}

static int css_order;

/* Reads a sheet. With `vars`, only what custom properties it sets; call that for every sheet
 * before reading any for its rules, since a rule may use what a later sheet sets. */
static void
css_parse(const char *s, int n, int vars)
{
	struct css_decl found[64];
	int i = 0;

	while (i < n) {
		int start, pre_end, body, end, count, decl, root, depth;

		while (i < n && (isspace((unsigned char)s[i]) || s[i] == '}' || s[i] == ';'))
			i++;
		if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
			for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); i++)
				;
			i += 2;
			continue;
		}
		if (i + 3 < n && !strncmp(s + i, "<!--", 4)) {
			i += 4;
			continue;
		}
		start = i;
		while (i < n && s[i] != '{' && s[i] != ';' && s[i] != '}')
			i++;
		if (i >= n)
			break;
		if (s[i] != '{')
			continue;	/* @import and the like */
		pre_end = i;
		body = i + 1;
		i = css_close(s, n, body);
		end = i > body && s[i - 1] == '}' ? i - 1 : i;
		if (s[start] == '@') {
			if (!strncasecmp(s + start, "@media", 6)) {
				if (css_media(s + start + 6, pre_end - start - 6))
					css_parse(s + body, end - body, vars);
			} else if (!strncasecmp(s + start, "@supports", 9) || !strncasecmp(s + start, "@layer", 6)) {
				css_parse(s + body, end - body, vars);
			}
			continue;
		}
		root = memmem(s + start, pre_end - start, ":root", 5) != NULL || !strncasecmp(s + start, "html", 4) ||
		       !strncasecmp(s + start, "body", 4);
		count = css_body(s + body, end - body, found, 64, vars, root);
		if (vars || !count)
			continue;
		css_decls = css_grow(css_decls, &css_decl_room, css_decl_count + count, sizeof *css_decls);
		memcpy(css_decls + css_decl_count, found, count * sizeof found[0]);
		decl = css_decl_count;
		css_decl_count += count;
		css_order++;
		/* one rule for each selector of the list */
		for (i = start, depth = 0; i <= pre_end; i++) {
			if (i < pre_end && s[i] == '(')
				depth++;
			else if (i < pre_end && s[i] == ')')
				depth--;
			else if (i == pre_end || (s[i] == ',' && !depth)) {
				int a = start, b = i;

				while (a < b && isspace((unsigned char)s[a]))
					a++;
				while (b > a && isspace((unsigned char)s[b - 1]))
					b--;
				if (b > a)
					css_selector(s + a, b - a, decl, count, css_order);
				start = i + 1;
			}
		}
		i = css_close(s, n, body);
	}
}

/* ---- finding an element's rules ---- */

static int
css_part_ok(const struct css_part *p, const struct css_elem *e)
{
	int i, j;

	if ((p->tag && p->tag != e->tag) || (p->id && p->id != e->id))
		return 0;
	for (i = 0; i < p->classes; i++) {
		for (j = 0; j < e->classes && e->cls[j] != p->cls[i]; j++)
			;
		if (j == e->classes)
			return 0;
	}
	return 1;
}

static int
css_rule_ok(const struct css_rule *r, const struct css_elem *stack, int at)
{
	const struct css_part *p = css_parts + r->part + r->parts - 1;
	int j;

	if (!css_part_ok(p, &stack[at]))
		return 0;
	for (j = r->parts - 2; j >= 0; j--) {
		int child = p->child;

		p--;
		if (child) {
			if (--at < 0 || !css_part_ok(p, &stack[at]))
				return 0;
		} else {
			do
				at--;
			while (at >= 0 && !css_part_ok(p, &stack[at]));
			if (at < 0)
				return 0;
		}
	}
	return 1;
}

/* The rules for the element at stack[at] (the ones before it are what it is inside), the
 * weakest first. */
static int
css_find(const struct css_elem *stack, int at, int *found, int room)
{
	const struct css_elem *e = &stack[at];
	int count = 0, list, r, i, j;

	for (list = -1; list <= e->classes + 1; list++) {
		r = list < 0 ? css_any : list == 0 ? css_by_tag[e->tag] : list == 1 ? css_by_id[e->id] : css_by_class[e->cls[list - 2]];
		if ((list == 0 && !e->tag) || (list == 1 && !e->id))
			continue;
		for (; r >= 0 && count < room; r = css_rules[r].next)
			if (css_rule_ok(&css_rules[r], stack, at))
				found[count++] = r;
	}
	for (i = 1; i < count; i++) {
		int moved = found[i];
		const struct css_rule *m = &css_rules[moved];

		for (j = i; j > 0; j--) {
			const struct css_rule *o = &css_rules[found[j - 1]];

			if (o->weight < m->weight || (o->weight == m->weight && o->order <= m->order))
				break;
			found[j] = found[j - 1];
		}
		found[j] = moved;
	}
	return count;
}

/* Lays declarations over a style. `basis` is what a percentage is of; `locked` remembers which
 * properties an !important has set, across the calls for one element. */
static void
css_apply(struct css_style *s, const struct css_decl *d, int n, int basis, unsigned *locked)
{
	for (; n-- > 0; d++) {
		unsigned bit = 1u << d->prop;
		int v = d->unit == U_PERCENT ? basis * d->value / 100 : d->value;
		short *box = NULL;

		if ((*locked & bit) && !d->important)
			continue;
		if (d->important)
			*locked |= bit;
		switch (d->prop) {
		case P_DISPLAY: s->display = v; break;
		case P_COLOR: s->colour = v; break;
		case P_BG: s->bg = v; break;
		case P_BOLD: s->bold = v; break;
		case P_ALIGN: s->align = v; break;
		case P_UNDERLINE: s->underline = v; break;
		case P_BORDER: s->border = v; break;
		case P_BORDER_T: s->border_t = v; break;
		case P_BORDER_B: s->border_b = v; break;
		case P_BULLET: s->bullet = v; break;
		case P_POSITION: s->absolute = v; break;
		case P_CLIP: s->clipped = v; break;
		case P_PRE: s->pre = v; break;
		case P_MT: box = &s->mt; break;
		case P_MB: box = &s->mb; break;
		case P_ML: box = &s->ml; break;
		case P_MR: box = &s->mr; break;
		case P_PT: box = &s->pt; break;
		case P_PB: box = &s->pb; break;
		case P_PL: box = &s->pl; break;
		case P_PR: box = &s->pr; break;
		case P_WIDTH: box = &s->width; break;
		case P_MAXW: box = &s->maxw; break;
		}
		if (box && d->unit == U_AUTO)
			*box = d->prop == P_ML || d->prop == P_MR ? CSS_AUTO : d->prop >= P_WIDTH ? CSS_UNSET : 0;
		else if (box)
			*box = v < 0 ? 0 : v > 20000 ? 20000 : v;
	}
}

#endif
