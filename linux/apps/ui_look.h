/* ui_look.h: the desktop's look as Settings chose it (docs/system/nanox.md, "Looks"): a style, classic
 * or Windows 7, and a font, the window system's bitmap one or Selawik. One word of the window
 * system's page of GPU memory holds it, so the window manager and every program read it with no
 * question asked; the registry keeps it for the next session. 0 there (nobody has said) is the
 * classic look with the smooth font (a bare `nano-X` for a test; the window manager's LOOK_SMOOTH
 * says the same): the bitmap font only when Settings chose it. */
#ifndef UI_LOOK_H
#define UI_LOOK_H
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "registry.h"

#define UI_LOOK_MAGIC	0x4c4b0000u	/* "LK" in the word's top half: a look was set */
#define UI_LOOK_PAGE	0x01001000u	/* /dev/gpu's offset of the page; the word is at WORD_AT in it */
#define UI_LOOK_WORD_AT	0x860		/* WM_OFFSET's word 24 (WM_LOOK in scr_shaderemu.c) */
#define UI_LOOK_STYLE	"desktop.style"	/* the registry's keys: "seven" or "classic", */
#define UI_LOOK_TEXT	"desktop.font"	/* "smooth" or "classic" */
#define UI_LOOK_FONT	"/usr/share/font-ui-12.sef"
#define UI_LOOK_FONT_BOLD "/usr/share/font-ui-b-12.sef"
#define UI_LOOK_FIXED	"/usr/share/font-mono-10.sef"	/* the fixed font's stand-in: 6 pixels a character, as X6x13 */
/* The windows' colour: the registry's "RRGGBB" (hex) or "default", and the next word of the page,
 * 0x01RRGGBB or 0 for the look's own (the window manager's frames: nx_tint in nxpaintnc.c). */
#define UI_TINT		"desktop.color"
#define UI_TINT_SET	0x01000000u

enum { UI_STYLE_CLASSIC, UI_STYLE_SEVEN };

/* The style and the font as this program started with them (ui_look_read) */
static int ui_seven, ui_smooth;

static volatile uint32_t *
ui_look_word(void)
{
	static volatile uint32_t *word;
	static int tried;

	if (!tried) {
		int fd = open("/dev/gpu", O_RDWR);
		void *page = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, UI_LOOK_PAGE);

		tried = 1;
		if (fd >= 0)
			close(fd);
		if (page != MAP_FAILED)
			word = (volatile uint32_t *)((char *)page + UI_LOOK_WORD_AT);
	}
	return word;
}

/* The word as it is now: 0 for no look set. */
static unsigned
ui_look_now(void)
{
	volatile uint32_t *word = ui_look_word();
	unsigned look = word ? *word : 0;

	return (look & 0xffff0000u) == UI_LOOK_MAGIC ? look : 0;
}

/* Reads the look into ui_seven and ui_smooth; true when it is another than they said. */
static int
ui_look_read(void)
{
	unsigned look = ui_look_now();
	int seven = (look & 0xff) == UI_STYLE_SEVEN, smooth = !look || (look >> 8 & 0xff) == 1;
	int changed = seven != ui_seven || smooth != ui_smooth;

	ui_seven = seven, ui_smooth = smooth;
	return changed;
}

static void
ui_look_set(int style, int smooth)
{
	volatile uint32_t *word = ui_look_word();

	if (word)
		*word = UI_LOOK_MAGIC | (unsigned)(smooth ? 1 : 0) << 8 | (unsigned)style;
}

/* The windows' colour as it is now: 0xRRGGBB | UI_TINT_SET, or 0 for the look's own. */
static unsigned
ui_tint_now(void)
{
	volatile uint32_t *word = ui_look_word();
	unsigned tint = word ? word[1] : 0;

	return (tint >> 24) == 1 ? tint : 0;
}

/* `tint`: 0xRRGGBB | UI_TINT_SET, or 0 for the look's own. */
static void
ui_tint_set(unsigned tint)
{
	volatile uint32_t *word = ui_look_word();

	if (word)
		word[1] = tint & UI_TINT_SET ? tint & 0x01ffffffu : 0;
}

/* The registry's text for a colour into the word's form ("default", or anything not six hex digits: 0). */
static unsigned
ui_tint_parse(const char *text)
{
	unsigned rgb = 0;
	int n;

	for (n = 0; n < 6; n++) {
		char c = text[n];

		if (c >= '0' && c <= '9')
			rgb = rgb << 4 | (unsigned)(c - '0');
		else if ((c | 32) >= 'a' && (c | 32) <= 'f')
			rgb = rgb << 4 | (unsigned)((c | 32) - 'a' + 10);
		else
			return 0;
	}
	return text[6] ? 0 : UI_TINT_SET | rgb;
}

/* The registry's look into the word, at the desktop's start (Windows 7 and Selawik unless it says otherwise). */
static void
ui_look_from_registry(void)
{
	ui_look_set(strcmp(reg_get(UI_LOOK_STYLE, "seven"), "classic") ? UI_STYLE_SEVEN : UI_STYLE_CLASSIC,
		strcmp(reg_get(UI_LOOK_TEXT, "smooth"), "classic") != 0);
	ui_tint_set(ui_tint_parse(reg_get(UI_TINT, "default")));
}
#endif
