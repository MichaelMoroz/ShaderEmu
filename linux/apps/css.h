/*
 * Style sheets for nxweb (docs/system/fetch.md): rules read from text, kept by the last part of their
 * selector, and found again for an element of the page's tree. Lengths come out in this
 * machine's pixels; what is not understood is left out, never guessed. No part of the window
 * system is in here: web.h and a test on the host include it too.
 */
#ifndef NXWEB_CSS_H
#define NXWEB_CSS_H

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CSS_NONE (-2)		/* no colour given */
#define CSS_CLEAR (-1)		/* transparent */
#define CSS_CURRENT (-3)	/* the text's colour */
#define CSS_AUTO (-30000)
#define CSS_UNSET (-30001)
#define CSS_PCT 100000		/* a length of CSS_PCT + n is n tenths of a percent */
#define CSS_EM 13		/* our font against the 16 pixels pages are written for */
#ifndef CSS_ATOMS
#define CSS_ATOMS 8192		/* (a program of few pages may have fewer: nxedit's reader) */
#endif

#define CSS_TAGS(X) \
	X(html) X(body) X(p) X(div) X(table) X(blockquote) X(section) X(article) X(header) X(footer) X(form) \
	X(tr) X(dt) X(dd) X(td) X(th) X(h1) X(h2) X(h3) X(h4) X(h5) X(h6) X(ul) X(ol) X(li) X(hr) X(pre) X(b) \
	X(strong) X(a) X(img) X(br) X(script) X(style) X(head) X(svg) X(noscript) X(template) X(title) X(main) \
	X(nav) X(aside) X(figure) X(figcaption) X(center) X(dl) X(address) X(details) X(summary) X(fieldset) \
	X(caption) X(input) X(meta) X(link) X(source) X(wbr) X(area) X(col) X(embed) X(param) X(track) X(base) \
	X(i) X(em) X(u) X(s) X(strike) X(del) X(ins) X(code) X(tt) X(kbd) X(samp) X(var) X(cite) X(dfn) X(small) \
	X(big) X(sub) X(sup) X(font) X(span) X(button) X(select) X(option) X(textarea) X(label) X(thead) X(tbody) \
	X(tfoot) X(colgroup) X(iframe) X(video) X(audio) X(canvas) X(object) X(picture) X(legend) X(menu) X(mark) \
	X(abbr) X(q) X(time) X(nobr) X(optgroup) X(datalist) X(dialog) X(math) X(map) X(hgroup) X(search)
#define X(name) T_##name,
enum { T_NONE, CSS_TAGS(X) T_COUNT };
#undef X

enum { P_DISPLAY, P_COLOR, P_BG, P_BOLD, P_ITALIC, P_FAMILY, P_FONT_SIZE, P_LINE_H, P_ALIGN, P_DECOR, P_BULLET,
       P_WHITE, P_TRANSFORM, P_VISIBLE, P_MT, P_MR, P_MB, P_ML, P_PT, P_PR, P_PB, P_PL, P_WIDTH, P_HEIGHT, P_MINW,
       P_MAXW, P_MINH, P_BWT, P_BWR, P_BWB, P_BWL, P_BCT, P_BCR, P_BCB, P_BCL, P_POSITION, P_CLIP, P_FLOAT,
       P_CLEAR, P_FLEX_DIR, P_FLEX_WRAP, P_JUSTIFY, P_ITEMS, P_GROW, P_BASIS, P_GAP, P_ROW_GAP, P_VALIGN, P_SIZING,
       P_GRID, P_COUNT };
enum { U_PX, U_PERCENT, U_AUTO, U_EM, U_REM, U_NUM };
enum { D_INLINE, D_BLOCK, D_NONE, D_INLINE_BLOCK, D_FLEX, D_INLINE_FLEX, D_GRID, D_TABLE, D_ROW, D_CELL,
       D_ROW_GROUP, D_LIST_ITEM, D_CAPTION };
enum { W_NORMAL, W_PRE, W_NOWRAP, W_PRE_WRAP };
enum { DECOR_UNDER = 1, DECOR_THROUGH = 2 };

/* A length kept in thousandths: of a pixel of ours, of an em, of a percent, or a plain number. */
struct css_decl { unsigned char prop, unit, important; int value; };
struct css_attr { int name, value; unsigned char op; };	/* [name op value]: op 0 is "has it" */
/* One step of a selector. `how` says what the step to its left is to it: 0 an element it is
 * inside, '>' its parent, '+' the one just before it, '~' any before it. */
struct css_part {
	unsigned short tag, id, cls[3];
	unsigned char classes, how, attrs, pseudo;	/* pseudo: PS_* bits */
	short nth_a, nth_b;
	struct css_attr attr[2];
	int negated;					/* a part this one must not match, or -1 */
};
enum { PS_FIRST = 1, PS_LAST = 2, PS_NTH = 4, PS_LINK = 8, PS_ROOT = 16, PS_EMPTY = 32 };
struct css_rule { int part, decl, order, weight, next; unsigned char parts, decls; };

/* An element as a selector sees it: the page's tree is made of these (web.h adds to them). */
struct css_node {
	unsigned short tag, id;
	int cls, classes;		/* its classes' atoms, in css_classes */
	int parent, prev;		/* elements; -1 for none */
	int index;			/* which element child of its parent it is, from 1 */
	int last;			/* true for its parent's last element child */
	const char *attrs;		/* its tag's text after the name */
	int attrs_n;
	int kids;			/* children of any kind */
	unsigned names;			/* a bit for each of its attributes' names (css_name_bit) */
};

/* What an element looks like. The first group passes to what is inside it. */
struct css_style {
	int colour;
	short font_size, line_h;	/* line_h: pixels, or minus 256ths of the font's size, or 0 for the font's own */
	unsigned char bold, italic, family, align, decor, bullet, white, transform, hidden;
	unsigned char face;		/* the font its text is in: worked out with the style (web.h) */
	unsigned char display, position, clipped, floated, clear, column, wrap, justify, items, valign, border_box;
	int bg, border_c[4];		/* sides: top, right, bottom, left */
	unsigned char border_w[4];
	int m[4], p[4], width, height, minw, maxw, minh, basis, gap, row_gap;	/* pixels, CSS_PCT + tenths, CSS_AUTO */
	short grow;			/* hundredths */
	int grid;			/* its columns' text in css_text, or -1 */
};

static char *css_names;			/* every atom's text */
static int css_names_length, css_names_room;
static int css_name_at[CSS_ATOMS];
static unsigned short css_hash[CSS_ATOMS * 2];
static unsigned short css_slot_of[CSS_ATOMS];	/* where an atom is in css_hash: a reset clears those, not the table */
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
static unsigned short *css_classes;	/* the elements' classes, one list after another */
static int css_class_count, css_class_room;
static int css_view, css_view_h;	/* the size media queries and vw, vh are answered with */
static int css_root_px = CSS_EM;	/* the root element's font size: what a rem is */
/* The tree is an array of something that begins with a css_node, this many bytes each. */
static int css_node_size = sizeof(struct css_node);
#define CSS_AT(nodes, i) ((const struct css_node *)((const char *)(nodes) + (size_t)(i) * css_node_size))

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
			low[i] = text[i] >= 'A' && text[i] <= 'Z' ? text[i] + 32 : text[i];
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
	css_slot_of[css_atoms] = (unsigned short)slot;
	/* (what is kept by atom begins here, for this atom: nothing is cleared for all of them) */
	css_by_id[css_atoms] = css_by_class[css_atoms] = css_by_tag[css_atoms] = css_var_at[css_atoms] = -1;
	css_var_root[css_atoms] = 0;
	return css_atoms;
}

/* Text kept for later (an attribute's name, a grid's columns): where it is in css_text. */
static int
css_keep(const char *text, int n)
{
	int at = css_text_length;

	css_text = css_grow(css_text, &css_text_room, css_text_length + n + 1, 1);
	memcpy(css_text + at, text, n);
	css_text[at + n] = 0;
	css_text_length += n + 1;
	return at;
}

static void
css_reset(int view, int view_h)
{
	static const char tags[][11] = {	/* (in place, as the tables below: no pointers to relocate at a start) */
#define X(name) #name,
		CSS_TAGS(X)
#undef X
	};
	int i;

	for (i = 1; i <= css_atoms; i++)
		css_hash[css_slot_of[i]] = 0;
	css_atoms = css_names_length = css_text_length = css_rule_count = css_part_count = css_decl_count = 0;
	css_view = view;
	css_view_h = view_h;
	css_any = -1;
	css_by_id[0] = css_by_class[0] = css_by_tag[0] = css_var_at[0] = -1;	/* (no atom: no rules) */
	css_var_root[0] = 0;
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

static int
css_scaled(int milli_px)
{
	return (int)((long long)milli_px * CSS_EM / 16);
}

/* One length, without calc(): its unit and thousandths. Moves past it. */
static int
css_length_plain(const char **at, int *unit, int *value)
{
	const char *p = *at;
	int m;

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
		*value = css_scaled(m), p += 2;
	else if (!strncasecmp(p, "rem", 3))
		*unit = U_REM, *value = m, p += 3;
	else if (!strncasecmp(p, "em", 2))
		*unit = U_EM, *value = m, p += 2;
	else if (!strncasecmp(p, "pt", 2))
		*value = css_scaled(m * 4 / 3), p += 2;
	else if (!strncasecmp(p, "ch", 2) || !strncasecmp(p, "ex", 2))
		*unit = U_EM, *value = m / 2, p += 2;
	else if (*p == '%')
		*unit = U_PERCENT, *value = m, p++;
	else if (!strncasecmp(p, "vw", 2) || !strncasecmp(p, "vh", 2) || !strncasecmp(p, "dvh", 3) || !strncasecmp(p, "svh", 3)) {
		int tall = tolower((unsigned char)p[0]) != 'v' || tolower((unsigned char)p[1]) == 'h';

		*value = (int)((long long)m * (tall ? css_view_h : css_view) / 100);
		p += tolower((unsigned char)p[0]) == 'v' ? 2 : 3;
	} else if (!strncasecmp(p, "in", 2))
		*value = css_scaled(m * 96), p += 2;
	else if (!strncasecmp(p, "cm", 2))
		*value = css_scaled(m * 38), p += 2;
	else if (!strncasecmp(p, "mm", 2))
		*value = css_scaled(m * 4), p += 2;
	else if (m == 0)
		*value = 0;
	else
		return 0;
	*at = p;
	return 1;
}

/* A length: one as above, or calc() of a sum of them (what is not in pixels counts for the
 * unit of the sum's first percentage or em, the pixels beside it dropped). Moves past it. */
static int
css_length(const char **at, int *unit, int *value)
{
	const char *p = *at;

	while (isspace((unsigned char)*p))
		p++;
	if (!strncasecmp(p, "calc(", 5) || !strncasecmp(p, "min(", 4) || !strncasecmp(p, "max(", 4) || !strncasecmp(p, "clamp(", 6)) {
		int px = 0, other = 0, other_unit = U_PX, sign = 1, depth = 1, u, v, terms = 0, sum = tolower((unsigned char)*p) == 'c' && tolower((unsigned char)p[1]) == 'a';

		for (p = strchr(p, '(') + 1; *p && depth; ) {
			if (isspace((unsigned char)*p) || *p == ',') {
				if (*p == ',' && !sum && terms)
					sign = 0;	/* min, max, clamp: the first one given */
				p++;
			} else if (*p == '(')
				depth++, p++;
			else if (*p == ')')
				depth--, p++;
			else if ((*p == '+' || *p == '-') && isspace((unsigned char)p[1]))
				sign = sign ? (*p == '-' ? -1 : 1) : 0, p++;
			else if (*p == '*' || *p == '/')
				return 0;
			else if (css_length_plain(&p, &u, &v)) {
				if (u == U_AUTO)
					return 0;
				if (u == U_PX)
					px += sign * v;
				else if (other_unit == U_PX || other_unit == u)
					other_unit = u, other += sign * v;
				terms++;
				if (sum)
					sign = 1;
			} else
				return 0;
		}
		if (depth || !terms)
			return 0;
		*unit = other_unit;
		*value = other_unit == U_PX ? px : other;
		*at = p;
		return 1;
	}
	if (!css_length_plain(&p, unit, value))
		return 0;
	if (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')' && *p != '/')
		return 0;
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

/* A colour as 0xRRGGBB (one that lets what is behind through is mixed with white), CSS_CLEAR,
 * CSS_CURRENT, or CSS_NONE for text that is not a colour. */
static int
css_colour(const char *v)
{
	static const struct { char name[15]; int rgb; } named[] = {
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
		{ "lightblue", 0xadd8e6 }, { "lightgreen", 0x90ee90 }, { "lightyellow", 0xffffe0 }, { "darkorange", 0xff8c00 },
		{ "firebrick", 0xb22222 }, { "midnightblue", 0x191970 }, { "linen", 0xfaf0e6 }, { "wheat", 0xf5deb3 },
		{ "tan", 0xd2b48c }, { "chocolate", 0xd2691e }, { "sienna", 0xa0522d }, { "orangered", 0xff4500 },
		{ "hotpink", 0xff69b4 }, { "deeppink", 0xff1493 }, { "darkcyan", 0x008b8b }, { "cornflowerblue", 0x6495ed },
		{ "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f }, { "lightslategray", 0x778899 },
		{ "mediumblue", 0x0000cd }, { "rebeccapurple", 0x663399 }, { "honeydew", 0xf0fff0 }, { "azure", 0xf0ffff },
		{ "canvas", 0xffffff }, { "canvastext", 0x000000 }, { "linktext", 0x0000c8 },
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
	} else if (!strncasecmp(v, "currentcolor", 12)) {
		return CSS_CURRENT;
	} else if (!strncasecmp(v, "light-dark(", 11)) {
		return css_colour(v + 11);	/* this machine's screen is the light one */
	} else {
		for (n = 0; isalpha((unsigned char)v[n]); n++)
			;
		for (i = 0; n > 2 && i < (int)(sizeof named / sizeof named[0]); i++)
			if (((unsigned char)*v | 32) == (unsigned char)named[i].name[0] && (int)strlen(named[i].name) == n &&
			    !strncasecmp(v, named[i].name, n))
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

static int
css_word(const char *v, const char *word)
{
	size_t n = strlen(word);
	const char *at = v;

	while ((at = strcasestr(at, word)) != NULL) {
		if ((at == v || !isalnum((unsigned char)at[-1])) && at[-(at > v)] != '-' && !isalnum((unsigned char)at[n]) && at[n] != '-')
			return 1;
		at += n;
	}
	return 0;
}

/* A font's size: a length, or one of the words for one. */
static int
css_font_size(const char *v, int *unit, int *value)
{
	static const struct { char name[10]; int unit, value; } words[] = {
		{ "xx-small", U_PX, 9000 }, { "x-small", U_PX, 10000 }, { "smaller", U_EM, 830 }, { "small", U_PX, 11000 },
		{ "medium", U_PX, 13000 }, { "larger", U_EM, 1200 }, { "large", U_PX, 15000 }, { "x-large", U_PX, 20000 },
		{ "xx-large", U_PX, 24000 }, { "xxx-large", U_PX, 32000 },
	};
	int i;

	for (i = 0; i < (int)(sizeof words / sizeof words[0]); i++)
		if (!strncasecmp(v, words[i].name, strlen(words[i].name))) {
			*unit = words[i].unit;
			*value = words[i].value;
			return 1;
		}
	if (!css_length(&v, unit, value) || *unit == U_AUTO || *value < 0)
		return 0;
	if (*unit == U_PERCENT)
		*unit = U_EM, *value /= 100;
	return 1;
}

/* Which of our three faces a list of font names asks for: 0 sans, 1 serif, 2 fixed. -1: none said. */
static int
css_family(const char *v)
{
	/* the first name of the list that begins like one of these says which */
	static const struct { char name[19]; unsigned char n, face; } known[] = {
		{ "monospace", 9, 2 }, { "courier", 7, 2 }, { "consolas", 8, 2 }, { "menlo", 5, 2 }, { "monaco", 6, 2 },
		{ "source code", 11, 2 }, { "dejavu sans mono", 16, 2 }, { "ui-monospace", 12, 2 }, { "sfmono", 6, 2 }, { "sf mono", 7, 2 },
		{ "lucida console", 14, 2 }, { "liberation mono", 15, 2 },
		{ "serif", 5, 1 }, { "times", 5, 1 }, { "georgia", 7, 1 }, { "garamond", 8, 1 }, { "palatino", 8, 1 }, { "charter", 7, 1 },
		{ "cambria", 7, 1 }, { "ui-serif", 8, 1 }, { "linux libertine", 15, 1 }, { "liberation serif", 16, 1 },
		{ "sans", 4, 0 }, { "arial", 5, 0 }, { "helvetica", 9, 0 }, { "verdana", 7, 0 }, { "tahoma", 6, 0 }, { "system-ui", 9, 0 },
		{ "segoe", 5, 0 }, { "roboto", 6, 0 }, { "inter", 5, 0 }, { "geneva", 6, 0 }, { "trebuchet", 9, 0 }, { "lucida", 6, 0 },
		{ "ubuntu", 6, 0 }, { "noto sans", 9, 0 }, { "open sans", 9, 0 }, { "lato", 4, 0 }, { "-apple-system", 13, 0 },
		{ "blinkmacsystemfont", 18, 0 }, { "ui-sans", 7, 0 }, { "liberation sans", 15, 0 }, { "dejavu sans", 11, 0 },
	};
	int i;

	while (*v) {
		while (isspace((unsigned char)*v) || *v == ',' || *v == '"' || *v == '\'')
			v++;
		for (i = 0; i < (int)(sizeof known / sizeof known[0]); i++)
			if (((unsigned char)*v | 32) == (unsigned char)known[i].name[0] && !strncasecmp(v, known[i].name, known[i].n))
				return known[i].face;
		while (*v && *v != ',')
			v++;
	}
	return -1;
}

/* One property and its value as what we keep of it: up to eight entries of `out`. */
static int
css_declaration(const char *name, int nn, const char *v, struct css_decl *out)
{
	static const struct { char name[21]; unsigned char first, sides; } boxes[] = {
		{ "margin", P_MT, 4 }, { "padding", P_PT, 4 }, { "margin-top", P_MT, 1 }, { "margin-bottom", P_MB, 1 },
		{ "margin-left", P_ML, 1 }, { "margin-right", P_MR, 1 }, { "padding-top", P_PT, 1 }, { "padding-bottom", P_PB, 1 },
		{ "padding-left", P_PL, 1 }, { "padding-right", P_PR, 1 }, { "margin-block", P_MT, 12 }, { "margin-inline", P_ML, 13 },
		{ "padding-block", P_PT, 12 }, { "padding-inline", P_PL, 13 }, { "margin-block-start", P_MT, 1 },
		{ "margin-block-end", P_MB, 1 }, { "margin-inline-start", P_ML, 1 }, { "margin-inline-end", P_MR, 1 },
		{ "padding-block-start", P_PT, 1 }, { "padding-block-end", P_PB, 1 }, { "padding-inline-start", P_PL, 1 },
		{ "padding-inline-end", P_PR, 1 }, { "width", P_WIDTH, 1 }, { "max-width", P_MAXW, 1 }, { "min-width", P_MINW, 1 },
		{ "height", P_HEIGHT, 1 }, { "min-height", P_MINH, 1 }, { "inline-size", P_WIDTH, 1 }, { "max-inline-size", P_MAXW, 1 },
		{ "flex-basis", P_BASIS, 1 }, { "column-gap", P_GAP, 1 }, { "row-gap", P_ROW_GAP, 1 }, { "gap", P_ROW_GAP, 14 },
		{ "grid-gap", P_ROW_GAP, 14 }, { "grid-column-gap", P_GAP, 1 }, { "grid-row-gap", P_ROW_GAP, 1 },
	};
	int n = 0, c, i, unit, value;

#define IS(text) (nn == (int)sizeof(text) - 1 && !strncasecmp(name, text, nn))
#define PUT(p, u, val) (out[n].prop = (p), out[n].unit = (u), out[n].important = 0, out[n++].value = (val))
	while (isspace((unsigned char)*v))
		v++;
	for (i = 0; i < (int)(sizeof boxes / sizeof boxes[0]); i++) {
		int units[4], values[4], count = 0, first = boxes[i].first;

		if ((name[0] | 32) != boxes[i].name[0] || nn != (int)strlen(boxes[i].name) || strncasecmp(name, boxes[i].name, nn))
			continue;
		if (first >= P_WIDTH && first <= P_MINH && (!strncasecmp(v, "none", 4) || strstr(v, "content"))) {
			PUT(first, U_AUTO, 0);		/* none, max-content, fit-content and the like */
			return n;
		}
		while (count < 4 && css_length(&v, &units[count], &values[count]))
			count++;
		while (isspace((unsigned char)*v))
			v++;
		if (!count || *v)
			return 0;
		if (boxes[i].sides == 4) {
			/* top, right, bottom, left: the ones not given are the ones across */
			static const unsigned char from[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };

			for (c = 0; c < 4; c++)
				PUT(first + c, units[from[count - 1][c]], values[from[count - 1][c]]);
		} else if (boxes[i].sides == 12) {		/* top and bottom */
			PUT(first, units[0], values[0]);
			PUT(first + 2, units[count > 1], values[count > 1]);
		} else if (boxes[i].sides == 13) {		/* left and right */
			PUT(first, units[0], values[0]);
			PUT(first - 2, units[count > 1], values[count > 1]);
		} else if (boxes[i].sides == 14) {		/* rows, then columns */
			PUT(P_ROW_GAP, units[0], values[0]);
			PUT(P_GAP, units[count > 1], values[count > 1]);
		} else {
			PUT(first, units[0], values[0]);
		}
		return n;
	}
	if (IS("display")) {
		static const struct { char name[19]; int display; } kinds[] = {
			{ "none", D_NONE }, { "inline-block", D_INLINE_BLOCK }, { "inline-flex", D_INLINE_FLEX },
			{ "inline-grid", D_INLINE_BLOCK }, { "inline-table", D_TABLE }, { "inline", D_INLINE }, { "contents", D_INLINE },
			{ "flex", D_FLEX }, { "grid", D_GRID }, { "table-row-group", D_ROW_GROUP }, { "table-header-group", D_ROW_GROUP },
			{ "table-footer-group", D_ROW_GROUP }, { "table-row", D_ROW }, { "table-cell", D_CELL },
			{ "table-caption", D_CAPTION }, { "table-column", D_NONE }, { "table", D_TABLE }, { "list-item", D_LIST_ITEM },
			{ "block", D_BLOCK }, { "flow-root", D_BLOCK },
		};

		for (i = 0; i < (int)(sizeof kinds / sizeof kinds[0]); i++)
			if (!strncasecmp(v, kinds[i].name, strlen(kinds[i].name))) {
				PUT(P_DISPLAY, 0, kinds[i].display);
				break;
			}
	} else if (IS("visibility")) {
		PUT(P_VISIBLE, 0, strncasecmp(v, "hidden", 6) && strncasecmp(v, "collapse", 8));
	} else if (IS("color")) {
		if ((c = css_colour(v)) >= 0)
			PUT(P_COLOR, 0, c);
	} else if (IS("background-color") || IS("background")) {
		if ((c = css_colour_in(v)) != CSS_NONE && c != CSS_CURRENT)
			PUT(P_BG, 0, c);
		else if (!strncasecmp(v, "none", 4))
			PUT(P_BG, 0, CSS_CLEAR);
	} else if (IS("font-weight")) {
		PUT(P_BOLD, 0, !strncasecmp(v, "bold", 4) || atoi(v) >= 600);
	} else if (IS("font-style")) {
		PUT(P_ITALIC, 0, !strncasecmp(v, "italic", 6) || !strncasecmp(v, "oblique", 7));
	} else if (IS("font-family")) {
		if ((c = css_family(v)) >= 0)
			PUT(P_FAMILY, 0, c);
	} else if (IS("font-size")) {
		if (css_font_size(v, &unit, &value))
			PUT(P_FONT_SIZE, unit, value);
	} else if (IS("line-height")) {
		const char *p = v;

		if (!strncasecmp(v, "normal", 6))
			PUT(P_LINE_H, U_AUTO, 0);
		else if (css_length(&p, &unit, &value) && unit != U_AUTO)
			PUT(P_LINE_H, unit, value);
		else if (css_number(&v, &value))
			PUT(P_LINE_H, U_NUM, value);
	} else if (IS("font")) {
		/* style and weight in any order, then size[/line-height], then the family */
		const char *p = v, *slash;

		PUT(P_BOLD, 0, css_word(v, "bold") || css_word(v, "700") || css_word(v, "600") || css_word(v, "800"));
		PUT(P_ITALIC, 0, css_word(v, "italic") || css_word(v, "oblique"));
		for (;;) {
			const char *word = p;

			while (*p && !isspace((unsigned char)*p) && *p != '/')
				p++;
			if (p == word)
				break;
			if (css_font_size(word, &unit, &value) && (isdigit((unsigned char)*word) || *word == '.' || !isdigit((unsigned char)*word))) {
				if (isdigit((unsigned char)*word) && unit == U_PX && value % 100000 == 0 && value >= 100000 && value <= 900000 &&
				    isspace((unsigned char)*p) && !strncasecmp(word + 3, " ", 1)) {
					/* a weight (400, 700), not a size */
				} else {
					PUT(P_FONT_SIZE, unit, value);
					PUT(P_LINE_H, U_AUTO, 0);
					if (*p == '/') {
						slash = p + 1;
						if (css_length(&slash, &unit, &value) && unit != U_AUTO)
							out[n - 1].unit = unit, out[n - 1].value = value;
						else if (css_number(&slash, &value))
							out[n - 1].unit = U_NUM, out[n - 1].value = value;
						while (*p && !isspace((unsigned char)*p))
							p++;
					}
					if ((c = css_family(p)) >= 0)
						PUT(P_FAMILY, 0, c);
					break;
				}
			}
			while (isspace((unsigned char)*p))
				p++;
		}
	} else if (IS("text-align")) {
		PUT(P_ALIGN, 0, !strncasecmp(v, "center", 6) || !strncasecmp(v, "-webkit-center", 14) ? 1 :
		    !strncasecmp(v, "right", 5) || !strncasecmp(v, "end", 3) ? 2 : 0);
	} else if (IS("text-decoration") || IS("text-decoration-line")) {
		if (strstr(v, "underline") || strstr(v, "line-through"))
			PUT(P_DECOR, 0, (strstr(v, "underline") ? DECOR_UNDER : 0) | (strstr(v, "line-through") ? DECOR_THROUGH : 0));
		else if (strstr(v, "none"))
			PUT(P_DECOR, 0, 0);
	} else if (IS("text-transform")) {
		PUT(P_TRANSFORM, 0, !strncasecmp(v, "upper", 5) ? 1 : !strncasecmp(v, "lower", 5) ? 2 : !strncasecmp(v, "capital", 7) ? 3 : 0);
	} else if (!strncasecmp(name, "border", 6) && nn <= 19) {
		/* border, border-SIDE, and either with -width, -color or -style */
		static const char *const side_names[] = { "-top", "-right", "-bottom", "-left" };
		const char *rest = name + 6;
		int rn = nn - 6, side = -1, what = 0, from = 0, to = 4;

		if (rn >= 6 && !strncasecmp(rest, "-block", 6))
			from = 0, to = 4, side = 12, rest += 6, rn -= 6;
		else if (rn >= 7 && !strncasecmp(rest, "-inline", 7))
			from = 1, to = 4, side = 13, rest += 7, rn -= 7;
		else
			for (i = 0; i < 4; i++)
				if (rn >= (int)strlen(side_names[i]) && !strncasecmp(rest, side_names[i], strlen(side_names[i])))
					side = i, rest += strlen(side_names[i]), rn -= (int)strlen(side_names[i]);
		if (rn == 6 && !strncasecmp(rest, "-width", 6))
			what = 1;
		else if (rn == 6 && !strncasecmp(rest, "-color", 6))
			what = 2;
		else if (rn == 6 && !strncasecmp(rest, "-style", 6))
			what = 3;
		else if (rn)
			return 0;	/* border-radius, border-collapse and the rest */
		if (side >= 0 && side < 4)
			from = side, to = side + 1;
		if (what == 0 || what == 3) {
			int gone = css_word(v, "none") || css_word(v, "hidden") || (what == 0 && (!strcmp(v, "0") || !strncmp(v, "0 ", 2)));
			int width = 2, colour = CSS_CURRENT;
			const char *p = v;

			if (what == 0 && !gone) {
				/* the width is the word that is a length; thin, medium and thick are 1, 2 and 3 */
				while (*p) {
					const char *word = p;

					if (css_length(&p, &unit, &value) && unit != U_AUTO && (isdigit((unsigned char)*word) || *word == '.')) {
						width = unit == U_PX ? (value + 999) / 1000 : value ? 1 : 0;
						if (unit == U_PX && value > 0 && width < 1)
							width = 1;
						break;
					}
					p = word;
					while (*p && !isspace((unsigned char)*p))
						p += *p == '(' ? (int)strcspn(p, ")") : 1;
					while (isspace((unsigned char)*p))
						p++;
				}
				if (css_word(v, "thin"))
					width = 1;
				if ((c = css_colour_in(v)) != CSS_NONE)
					colour = c;
			}
			for (i = from; i < to; i += side == 12 ? 2 : side == 13 ? 2 : 1) {
				if (what == 0 || gone)
					PUT(P_BWT + i, 0, gone ? 0 : width > 6 ? 6 : width);
				if (what == 0 && !gone)
					PUT(P_BCT + i, 0, colour);
			}
		} else if (what == 1) {
			int widths[4], count = 0;
			const char *p = v;

			while (count < 4 && css_length(&p, &unit, &value) && unit != U_AUTO)
				widths[count++] = unit == U_PX ? (value > 0 && value < 1000 ? 1 : (value + 500) / 1000) : value ? 1 : 0;
			if (count) {
				static const unsigned char pick[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };

				for (i = from; i < to; i++)
					PUT(P_BWT + i, 0, widths[side < 0 ? pick[count - 1][i] : 0] > 6 ? 6 : widths[side < 0 ? pick[count - 1][i] : 0]);
			}
		} else if ((c = css_colour_in(v)) != CSS_NONE) {
			for (i = from; i < to; i++)
				PUT(P_BCT + i, 0, c);
		}
	} else if (IS("list-style") || IS("list-style-type")) {
		PUT(P_BULLET, 0, strstr(v, "none") ? 0 : strstr(v, "decimal") || strstr(v, "roman") || strstr(v, "alpha") ? 2 : 1);
	} else if (IS("position")) {
		PUT(P_POSITION, 0, !strncasecmp(v, "absolute", 8) ? 1 : !strncasecmp(v, "fixed", 5) ? 2 : 0);
	} else if (IS("clip") || IS("clip-path")) {
		PUT(P_CLIP, 0, strncasecmp(v, "auto", 4) && strncasecmp(v, "none", 4));
	} else if (IS("white-space") || IS("white-space-collapse") || IS("text-wrap")) {
		PUT(P_WHITE, 0, !strncasecmp(v, "pre-wrap", 8) || !strncasecmp(v, "pre-line", 8) || !strncasecmp(v, "break-spaces", 12) ? W_PRE_WRAP :
		    !strncasecmp(v, "pre", 3) && !isalpha((unsigned char)v[3]) ? W_PRE : !strncasecmp(v, "nowrap", 6) ? W_NOWRAP : W_NORMAL);
	} else if (IS("float")) {
		PUT(P_FLOAT, 0, !strncasecmp(v, "left", 4) || !strncasecmp(v, "inline-start", 12) ? 1 :
		    !strncasecmp(v, "right", 5) || !strncasecmp(v, "inline-end", 10) ? 2 : 0);
	} else if (IS("clear")) {
		PUT(P_CLEAR, 0, !strncasecmp(v, "left", 4) ? 1 : !strncasecmp(v, "right", 5) ? 2 : !strncasecmp(v, "both", 4) ? 3 : 0);
	} else if (IS("flex-direction")) {
		PUT(P_FLEX_DIR, 0, !strncasecmp(v, "column", 6));
	} else if (IS("flex-wrap")) {
		PUT(P_FLEX_WRAP, 0, !strncasecmp(v, "wrap", 4));
	} else if (IS("flex-flow")) {
		PUT(P_FLEX_DIR, 0, strstr(v, "column") != NULL);
		PUT(P_FLEX_WRAP, 0, css_word(v, "wrap") || css_word(v, "wrap-reverse"));
	} else if (IS("justify-content")) {
		PUT(P_JUSTIFY, 0, strstr(v, "center") ? 1 : strstr(v, "end") || strstr(v, "right") ? 2 : strstr(v, "between") ? 3 :
		    strstr(v, "around") || strstr(v, "evenly") ? 4 : 0);
	} else if (IS("align-items")) {
		PUT(P_ITEMS, 0, strstr(v, "center") ? 1 : strstr(v, "end") ? 2 : 0);
	} else if (IS("flex-grow")) {
		if (css_number(&v, &value))
			PUT(P_GROW, 0, value / 10);
	} else if (IS("flex")) {
		/* grow [shrink] [basis], or one of the words */
		const char *p = v;

		if (!strncasecmp(v, "none", 4)) {
			PUT(P_GROW, 0, 0);
		} else if (!strncasecmp(v, "auto", 4)) {
			PUT(P_GROW, 0, 100);
		} else if (css_number(&p, &value) && (!*p || isspace((unsigned char)*p))) {
			int grow = value / 10;

			PUT(P_GROW, 0, grow);
			while (isspace((unsigned char)*p))
				p++;
			if (css_number(&p, &value) && (!*p || isspace((unsigned char)*p)))
				while (isspace((unsigned char)*p))
					p++;
			else
				p = *p ? p : "";
			if (*p && css_length(&p, &unit, &value))
				PUT(P_BASIS, unit, value);
			else if (grow)
				PUT(P_BASIS, U_PX, 0);
		} else if (css_length(&v, &unit, &value)) {
			PUT(P_BASIS, unit, value);
		}
	} else if (IS("vertical-align")) {
		PUT(P_VALIGN, 0, !strncasecmp(v, "middle", 6) ? 2 : !strncasecmp(v, "bottom", 6) ? 3 : !strncasecmp(v, "top", 3) ? 1 :
		    !strncasecmp(v, "sub", 3) ? 4 : !strncasecmp(v, "super", 5) ? 5 : 0);
	} else if (IS("box-sizing")) {
		PUT(P_SIZING, 0, !strncasecmp(v, "border-box", 10));
	} else if (IS("grid-template-columns") || IS("grid-template")) {
		const char *slash = strchr(v, '/');

		if (IS("grid-template-columns") || slash)
			PUT(P_GRID, 0, !strncasecmp(v, "none", 4) ? -1 : css_keep(slash ? slash + 1 : v, strlen(slash ? slash + 1 : v)));
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
				css_var_at[atom] = css_keep(s + value, value_n);
				css_var_root[atom] |= root;
			}
			continue;
		}
		if (vars || value_n <= 0 || value_n >= (int)sizeof buffer || count + 8 > room)
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

static int css_compound(const char *s, int n, int *at, struct css_part *part, int *weight, int inner);

/* One step of a selector, from s[*at] to the next combinator. 0: something we cannot test. */
static int
css_compound(const char *s, int n, int *at, struct css_part *part, int *weight, int inner)
{
	int i = *at, start, atom, used = 0;

	part->negated = -1;
	while (i < n) {
		unsigned char c = s[i];

		if (isspace(c) || c == '>' || c == '+' || c == '~' || c == ',' || (inner && c == ')'))
			break;
		used = 1;
		if (c == '*') {
			i++;
			continue;
		}
		if (c == '[') {
			struct css_attr *a = &part->attr[part->attrs];
			int name, name_n, end;

			for (name = ++i; i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_'); i++)
				;
			name_n = i - name;
			if (!name_n || part->attrs == 2)
				return 0;
			a->op = 0;
			a->value = -1;
			if (i < n && s[i] != ']') {
				char quote;

				if (s[i] != '=')
					a->op = s[i++];
				else
					a->op = '=';
				if (i >= n || s[i] != '=')
					return 0;
				i++;
				quote = i < n && (s[i] == '"' || s[i] == '\'') ? s[i++] : 0;
				for (end = i; end < n && (quote ? s[end] != quote : s[end] != ']'); end++)
					;
				a->value = css_keep(s + i, end - i);
				i = end + (quote ? 1 : 0);
				while (i < n && s[i] != ']')
					i++;	/* (a flag: i, s) */
			}
			if (i >= n)
				return 0;
			i++;
			a->name = css_keep(s + name, name_n);
			part->attrs++;
			*weight += 100;
			continue;
		}
		start = c == '.' || c == '#' ? i + 1 : c == ':' ? i + 1 + (i + 1 < n && s[i + 1] == ':') : i;
		for (i = start; i < n && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_' || (unsigned char)s[i] >= 0x80 || (s[i] == '\\' && ++i < n)); i++)
			;
		if (i == start)
			return 0;
		if (c == ':') {
			const char *name = s + start;
			int nn = i - start;

#define PSEUDO(text) (nn == (int)sizeof(text) - 1 && !strncasecmp(name, text, nn))
			*weight += 100;
			if (PSEUDO("root"))
				part->pseudo |= PS_ROOT;
			else if (PSEUDO("link") || PSEUDO("any-link"))
				part->pseudo |= PS_LINK;
			else if (PSEUDO("first-child"))
				part->pseudo |= PS_FIRST;
			else if (PSEUDO("last-child"))
				part->pseudo |= PS_LAST;
			else if (PSEUDO("only-child"))
				part->pseudo |= PS_FIRST | PS_LAST;
			else if (PSEUDO("empty"))
				part->pseudo |= PS_EMPTY;
			else if (PSEUDO("nth-child") && i < n && s[i] == '(') {
				const char *p = s + i + 1;
				int a = 0, b = 0, m;

				while (isspace((unsigned char)*p))
					p++;
				if (!strncasecmp(p, "odd", 3))
					a = 2, b = 1, p += 3;
				else if (!strncasecmp(p, "even", 4))
					a = 2, p += 4;
				else {
					if (*p == 'n' || ((*p == '-' || *p == '+') && p[1] == 'n'))
						a = *p == '-' ? -1 : 1, p += *p == 'n' ? 1 : 2;
					else if (css_number(&p, &m)) {
						if (*p == 'n')
							a = m / 1000, p++;
						else
							b = m / 1000;
					} else
						return 0;
					while (isspace((unsigned char)*p))
						p++;
					if (a && (*p == '+' || *p == '-')) {
						int sign = *p++ == '-' ? -1 : 1;

						while (isspace((unsigned char)*p))
							p++;
						if (!css_number(&p, &m))
							return 0;
						b = sign * m / 1000;
					}
				}
				while (isspace((unsigned char)*p))
					p++;
				if (*p != ')')
					return 0;	/* "of selector" and the like */
				part->pseudo |= PS_NTH;
				part->nth_a = a;
				part->nth_b = b;
				i = (int)(p - s) + 1;
			} else if (PSEUDO("not") && i < n && s[i] == '(' && !inner && part->negated < 0) {
				struct css_part inside;
				int k = i + 1, w = 0;

				memset(&inside, 0, sizeof inside);
				while (k < n && isspace((unsigned char)s[k]))
					k++;
				if (!css_compound(s, n, &k, &inside, &w, 1))
					return 0;
				while (k < n && isspace((unsigned char)s[k]))
					k++;
				if (k >= n || s[k] != ')')
					return 0;	/* a list, or steps: not tested */
				css_parts = css_grow(css_parts, &css_part_room, css_part_count + 1, sizeof *css_parts);
				css_parts[css_part_count] = inside;
				part->negated = css_part_count++;
				*weight += w;
				i = k + 1;
			} else
				return 0;	/* a state (:hover), or text that is not in the page (::before) */
#undef PSEUDO
			continue;
		}
		if ((atom = css_atom(s + start, i - start, 1, c != '.' && c != '#')) == 0)
			return 0;
		if (c == '.') {
			if (part->classes == 3)
				return 0;
			part->cls[part->classes++] = atom;
			*weight += 100;
		} else if (c == '#') {
			part->id = atom;
			*weight += 10000;
		} else {
			part->tag = atom;
			*weight += 1;
		}
	}
	*at = i;
	return used;
}

/* Adds a rule for one selector, unless it has something we cannot test (:hover, ::before). */
static void
css_selector(const char *s, int n, int decl, int decls, int order)
{
	struct css_part parts[8];
	struct css_rule *r;
	int k = 0, i = 0, weight = 0, *head, first = css_part_count;

	memset(parts, 0, sizeof parts);
	for (;;) {
		int how = 0;

		if (!css_compound(s, n, &i, &parts[k], &weight, 0)) {
			css_part_count = first;
			return;
		}
		for (; i < n && (isspace((unsigned char)s[i]) || s[i] == '>' || s[i] == '+' || s[i] == '~'); i++)
			if (!isspace((unsigned char)s[i]))
				how = s[i];
		if (i >= n) {
			if (how) {
				css_part_count = first;
				return;
			}
			break;
		}
		if (++k == 8) {
			css_part_count = first;
			return;
		}
		parts[k].how = how;
	}
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
						/* (the page's pixels: ours are 13 to its 16) */
						ok &= max ? css_view * 16 / CSS_EM <= m : css_view * 16 / CSS_EM >= m;
					} else {
						ok = 0;
					}
				} else {
					ok = 0;
				}
			} else if (!strncasecmp(p, "width", 5)) {
				/* width <= 600px, width >= 40em */
				const char *q = p + 5;
				int less;

				while (isspace((unsigned char)*q))
					q++;
				less = *q == '<';
				if ((*q == '<' || *q == '>') && (q++, 1)) {
					if (*q == '=')
						q++;
					while (isspace((unsigned char)*q))
						q++;
					if (css_number(&q, &m)) {
						m = !strncasecmp(q, "em", 2) || !strncasecmp(q, "rem", 3) ? m * 16 / 1000 : m / 1000;
						ok &= less ? css_view * 16 / CSS_EM <= m : css_view * 16 / CSS_EM >= m;
					} else
						ok = 0;
				} else
					ok = 0;
			} else if (!strncasecmp(p, "prefers-color-scheme", 20)) {
				ok &= memmem(p, s + end - p, "light", 5) != NULL;
			} else if (!strncasecmp(p, "orientation", 11)) {
				ok &= memmem(p, s + end - p, "landscape", 9) != NULL;
			} else if (!strncasecmp(p, "prefers-reduced-motion", 22)) {
				ok &= memmem(p, s + end - p, "reduce", 6) != NULL;
			} else if (!strncasecmp(p, "scripting", 9)) {
				ok &= memmem(p, s + end - p, "none", 4) != NULL;
			} else if (!strncasecmp(p, "hover", 5) || !strncasecmp(p, "any-hover", 9) || !strncasecmp(p, "pointer", 7)) {
				ok &= memmem(p, s + end - p, "none", 4) == NULL && memmem(p, s + end - p, "coarse", 6) == NULL;
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

/* The bit an attribute's name has in css_node.names: by its first letter and its length. Most
 * questions about an attribute end at that bit. */
static inline unsigned
css_name_bit(const char *name, int n)
{
	return 1u << ((((unsigned char)name[0] | 32) * 7 + n) & 31);
}

/* How many of the elements the one being styled is inside have each name (as tag, id or
 * class): a rule that wants one it is inside of that none has is not looked at further. A walk
 * over the tree on another core counts in an array of its own. */
static unsigned short css_inside[CSS_ATOMS];

/* Reads a sheet. With `vars`, only what custom properties it sets; call that for every sheet
 * before reading any for its rules, since a rule may use what a later sheet sets. */
static void
css_parse(const char *s, int n, int vars)
{
	struct css_decl found[96];
	int i = 0;

	while (i < n) {
		int start, pre_end, body, end, count, decl, root, depth, after;

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
		if (i + 2 < n && !strncmp(s + i, "-->", 3)) {
			i += 3;
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
		after = i = css_close(s, n, body);
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
		count = css_body(s + body, end - body, found, 96, vars, root);
		if (vars || !count)
			continue;
		css_decls = css_grow(css_decls, &css_decl_room, css_decl_count + count, sizeof *css_decls);
		memcpy(css_decls + css_decl_count, found, count * sizeof found[0]);
		decl = css_decl_count;
		css_decl_count += count;
		css_order++;
		/* one rule for each selector of the list */
		for (i = start, depth = 0; i <= pre_end; i++) {
			if (i < pre_end && (s[i] == '(' || s[i] == '['))
				depth++;
			else if (i < pre_end && (s[i] == ')' || s[i] == ']'))
				depth--;
			else if (i == pre_end || (s[i] == ',' && !depth)) {
				int a = start, b = i;

				while (a < b && isspace((unsigned char)s[a]))
					a++;
				while (b > a && isspace((unsigned char)s[b - 1]))
					b--;
				if (b > a)
					css_selector(s + a, b - a, decl, count > 255 ? 255 : count, css_order);
				start = i + 1;
			}
		}
		i = after;
	}
}

/* ---- finding an element's rules ---- */

/* The value of an attribute in a tag's text (after the name): where it is, its length; 0 if
 * the tag has none of that name. */
static int
css_attribute(const char *tag, int n, const char *name, const char **value, int *value_n)
{
	int m = strlen(name), i = 0;

	while (i < n) {
		int start, name_n, v = 0, vn = 0;

		while (i < n && (isspace((unsigned char)tag[i]) || tag[i] == '/'))
			i++;
		start = i;
		while (i < n && !isspace((unsigned char)tag[i]) && tag[i] != '=' && tag[i] != '/')
			i++;
		name_n = i - start;
		while (i < n && isspace((unsigned char)tag[i]))
			i++;
		if (i < n && tag[i] == '=') {
			for (i++; i < n && isspace((unsigned char)tag[i]); i++)
				;
			if (i < n && (tag[i] == '"' || tag[i] == '\'')) {
				char quote = tag[i++];

				for (v = i; i < n && tag[i] != quote; i++)
					;
				vn = i++ - v;
			} else {
				for (v = i; i < n && !isspace((unsigned char)tag[i]); i++)
					;
				vn = i - v;
			}
		} else if (!name_n) {
			i++;
		}
		if (name_n == m && !strncasecmp(tag + start, name, m)) {
			*value = tag + v;
			*value_n = vn;
			return 1;
		}
	}
	return 0;
}

static int
css_part_ok(const struct css_part *p, const struct css_node *nodes, int at)
{
	const struct css_node *e = CSS_AT(nodes, at);
	int i, j;

	if ((p->tag && p->tag != e->tag) || (p->id && p->id != e->id))
		return 0;
	for (i = 0; i < p->classes; i++) {
		for (j = 0; j < e->classes && css_classes[e->cls + j] != p->cls[i]; j++)
			;
		if (j == e->classes)
			return 0;
	}
	if (p->pseudo) {
		if ((p->pseudo & PS_ROOT) && e->parent >= 0)
			return 0;
		if ((p->pseudo & PS_FIRST) && e->index != 1)
			return 0;
		if ((p->pseudo & PS_LAST) && !e->last)
			return 0;
		if ((p->pseudo & PS_EMPTY) && e->kids)
			return 0;
		if (p->pseudo & PS_NTH) {
			int d = e->index - p->nth_b;

			if (p->nth_a == 0 ? d != 0 : (d % p->nth_a != 0 || d / p->nth_a < 0))
				return 0;
		}
	}
	for (i = 0; i < p->attrs; i++) {
		const struct css_attr *a = &p->attr[i];
		const char *want = a->value >= 0 ? css_text + a->value : "", *value, *found, *name = css_text + a->name;
		int n, m;

		if (!(e->names & css_name_bit(name, (int)strlen(name))) || !css_attribute(e->attrs, e->attrs_n, name, &value, &n))
			return 0;
		m = strlen(want);
		switch (a->op) {
		case 0: break;
		case '=': if (n != m || strncmp(value, want, m)) return 0; break;
		case '^': if (!m || n < m || strncmp(value, want, m)) return 0; break;
		case '$': if (!m || n < m || strncmp(value + n - m, want, m)) return 0; break;
		case '*': if (!m || !memmem(value, n, want, m)) return 0; break;
		case '|': if (n < m || strncmp(value, want, m) || (n > m && value[m] != '-')) return 0; break;
		case '~':
			for (found = value; m && (found = memmem(found, value + n - found, want, m)) != NULL; found++)
				if ((found == value || isspace((unsigned char)found[-1])) && (found + m == value + n || isspace((unsigned char)found[m])))
					break;
			if (!m || !found)
				return 0;
			break;
		default: return 0;
		}
	}
	if ((p->pseudo & PS_LINK) && (e->tag != T_a || !(e->names & css_name_bit("href", 4))))
		return 0;
	return p->negated < 0 || !css_part_ok(&css_parts[p->negated], nodes, at);
}

/* Whether the steps up to `j` hold for something that stands to element `at` as `how` says. */
static int
css_steps_ok(const struct css_rule *r, int j, int how, const struct css_node *nodes, int at)
{
	const struct css_part *p = css_parts + r->part + j;

	if (how == '>' || how == '+') {
		at = how == '>' ? CSS_AT(nodes, at)->parent : CSS_AT(nodes, at)->prev;
		return at >= 0 && css_part_ok(p, nodes, at) && (j == 0 || css_steps_ok(r, j - 1, p->how, nodes, at));
	}
	/* any element it is inside, or any before it: the nearest that the rest holds for too */
	for (at = how == '~' ? CSS_AT(nodes, at)->prev : CSS_AT(nodes, at)->parent; at >= 0;
	     at = how == '~' ? CSS_AT(nodes, at)->prev : CSS_AT(nodes, at)->parent)
		if (css_part_ok(p, nodes, at) && (j == 0 || css_steps_ok(r, j - 1, p->how, nodes, at)))
			return 1;
	return 0;
}

static int
css_rule_ok(const struct css_rule *r, const struct css_node *nodes, int at, const unsigned short *inside)
{
	const struct css_part *p = css_parts + r->part + r->parts - 1, *q;
	int how, k;

	if (!css_part_ok(p, nodes, at))
		return 0;
	if (r->parts == 1)
		return 1;
	/* what it must be inside: is there any such thing round it at all? */
	for (q = p - 1, how = p->how; q >= css_parts + r->part; how = q->how, q--) {
		if (how == '+' || how == '~')
			continue;
		if ((q->tag && !inside[q->tag]) || (q->id && !inside[q->id]))
			return 0;
		for (k = 0; k < q->classes; k++)
			if (!inside[q->cls[k]])
				return 0;
	}
	return css_steps_ok(r, r->parts - 2, p->how, nodes, at);
}

/* The rules for element `at` of the tree, the weakest first; `inside` as css_inside. */
static int
css_find(const struct css_node *nodes, int at, int *found, int room, const unsigned short *inside)
{
	const struct css_node *e = CSS_AT(nodes, at);
	int count = 0, list, r, i, j;

	for (list = -1; list <= e->classes + 1; list++) {
		r = list < 0 ? css_any : list == 0 ? css_by_tag[e->tag] : list == 1 ? css_by_id[e->id] :
		    css_by_class[css_classes[e->cls + list - 2]];
		if ((list == 0 && !e->tag) || (list == 1 && !e->id))
			continue;
		for (; r >= 0 && count < room; r = css_rules[r].next)
			if (css_rule_ok(&css_rules[r], nodes, at, inside))
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

/* A kept length as the style has it: pixels, a percentage, or auto. `em` is the element's font. */
static int
css_resolved(const struct css_decl *d, int em)
{
	switch (d->unit) {
	case U_AUTO: return CSS_AUTO;
	case U_PERCENT: return CSS_PCT + (d->value < 0 ? 0 : d->value / 100);
	case U_EM: return (int)(((long long)d->value * em + (d->value < 0 ? -500 : 500)) / 1000);
	case U_REM: return (int)(((long long)d->value * css_root_px + (d->value < 0 ? -500 : 500)) / 1000);
	default: return (d->value + (d->value < 0 ? -500 : 500)) / 1000;
	}
}

/* Lays declarations over a style: the font's size first (`sizes` true), since an em of the
 * others is that; then the rest. `locked` remembers which properties an !important has set. */
static void
css_apply(struct css_style *s, const struct css_decl *d, int n, int sizes, int parent_px, uint64_t *locked)
{
	for (; n-- > 0; d++) {
		uint64_t bit = 1ull << d->prop;
		int v = d->value, len;

		if (sizes != (d->prop == P_FONT_SIZE))
			continue;
		if ((*locked & bit) && !d->important)
			continue;
		if (d->important)
			*locked |= bit;
		if (d->prop == P_FONT_SIZE) {
			v = d->unit == U_EM ? (int)(((long long)v * parent_px + 500) / 1000) :
			    d->unit == U_REM ? (int)(((long long)v * css_root_px + 500) / 1000) : (v + 500) / 1000;
			s->font_size = v < 6 ? 6 : v > 64 ? 64 : v;
			continue;
		}
		len = css_resolved(d, s->font_size);
		switch (d->prop) {
		case P_DISPLAY: s->display = v; break;
		case P_COLOR: s->colour = v; break;
		case P_BG: s->bg = v; break;
		case P_BOLD: s->bold = v; break;
		case P_ITALIC: s->italic = v; break;
		case P_FAMILY: s->family = v; break;
		case P_LINE_H:
			s->line_h = d->unit == U_AUTO ? 0 : d->unit == U_NUM ? -(v * 256 / 1000) :
				    d->unit == U_PERCENT ? -(v * 256 / 100000) : len;
			break;
		case P_ALIGN: s->align = v; break;
		case P_DECOR: s->decor = v; break;
		case P_BULLET: s->bullet = v; break;
		case P_WHITE: s->white = v; break;
		case P_TRANSFORM: s->transform = v; break;
		case P_VISIBLE: s->hidden = !v; break;
		case P_POSITION: s->position = v; break;
		case P_CLIP: s->clipped = v; break;
		case P_FLOAT: s->floated = v; break;
		case P_CLEAR: s->clear = v; break;
		case P_FLEX_DIR: s->column = v; break;
		case P_FLEX_WRAP: s->wrap = v; break;
		case P_JUSTIFY: s->justify = v; break;
		case P_ITEMS: s->items = v; break;
		case P_GROW: s->grow = v > 30000 ? 30000 : v; break;
		case P_VALIGN: s->valign = v; break;
		case P_SIZING: s->border_box = v; break;
		case P_GRID: s->grid = v; break;
		case P_BWT: case P_BWR: case P_BWB: case P_BWL: s->border_w[d->prop - P_BWT] = v; break;
		case P_BCT: case P_BCR: case P_BCB: case P_BCL: s->border_c[d->prop - P_BCT] = v; break;
		case P_MT: case P_MR: case P_MB: case P_ML: s->m[d->prop - P_MT] = len; break;
		case P_PT: case P_PR: case P_PB: case P_PL: s->p[d->prop - P_PT] = len == CSS_AUTO || len < 0 ? 0 : len; break;
		case P_WIDTH: s->width = len; break;
		case P_HEIGHT: s->height = len; break;
		case P_MINW: s->minw = len == CSS_AUTO ? 0 : len; break;
		case P_MAXW: s->maxw = len == CSS_AUTO ? CSS_UNSET : len; break;
		case P_MINH: s->minh = len == CSS_AUTO ? 0 : len; break;
		case P_BASIS: s->basis = len; break;
		case P_GAP: s->gap = len == CSS_AUTO || len >= CSS_PCT ? 0 : len; break;
		case P_ROW_GAP: s->row_gap = len == CSS_AUTO || len >= CSS_PCT ? 0 : len; break;
		}
	}
}

/* A length of a style in pixels: a percentage is of `basis`; `none` is what auto comes to. */
static int
css_px(int len, int basis, int none)
{
	if (len == CSS_AUTO || len == CSS_UNSET)
		return none;
	if (len >= CSS_PCT / 2)
		return (int)((long long)basis * (len - CSS_PCT) / 1000);
	return len;
}

#endif
