// The machine's side: the clock, the fonts' file, and (AOE_NOWINDOW=1 for now) what stands for a window.
#include <sys/mman.h>
#include <sys/time.h>
#include "ddraw.h"
#include "aoe_port.h"
#include "aoe_fonts.h"
#undef open
#undef fopen

unsigned long aoe_clock_ms(void) {
    struct timeval t;
    gettimeofday(&t, 0);
    static long start;
    if (!start) start = t.tv_sec - 1;   // from the program's start: it stays in 32 bits
    return (t.tv_sec - start) * 1000 + t.tv_usec / 1000;
}
void aoe_idle(unsigned ms) { usleep(ms * 1000); }

const unsigned char* aoe_font_file;
static int font_count;
static void fonts_read(void) {
    static int done;
    if (done) return;
    done = 1;
    const char* dir = getenv("AOE_DATA");
    char p[300];
    snprintf(p, sizeof p, "%s/aoe-fonts.bin", dir ? dir : "/usr/share/aoe");
    int fd = open(p, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "aoe: no %s\n", p); return; }
    long n = lseek(fd, 0, SEEK_END);
    aoe_font_file = (const unsigned char*) mmap(0, n, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (aoe_font_file == MAP_FAILED) { aoe_font_file = 0; return; }
    font_count = *(const int*) aoe_font_file;
}
const AoeFont* aoe_font_get(const char* face, int size, int bold) {
    fonts_read();
    if (!aoe_font_file) return 0;
    const AoeFont* fonts = (const AoeFont*) (aoe_font_file + 16);
    const AoeFont* best = 0;
    int bd = 1 << 30;
    for (int i = 0; i < font_count; i++) {
        const AoeFont* f = &fonts[i];
        int d = abs((int) f->size - size) * 4 + (f->bold != (bold != 0));
        if (strncasecmp(f->face, face, 4)) d += 1000;   // "Arial", "Copperplate", "Comic Sans"
        if (d < bd) { bd = d; best = f; }
    }
    return best;
}

// ------------------------------------------------------------------ with no window
static unsigned char* page;
unsigned char* aoe_page(int w, int h) { page = (unsigned char*) calloc(w, h + 1); return page; }
void aoe_palette_set(const PALETTEENTRY*) {}
static long frames;
void aoe_present(void) { frames++; }
void aoe_window_make(int, int, const char*) {}
void aoe_window_close(void) {}

// A script of input (AOE_SCRIPT="MS click X Y;MS key K"): for the tests and for first light
static const char* script;
void aoe_events(void) {
    static int read_env;
    if (!read_env) { read_env = 1; script = getenv("AOE_SCRIPT"); }
    if (!script || !*script) return;
    int at;
    if (sscanf(script, "%d", &at) != 1 || (long) timeGetTime() < at) return;
    int x, y;
    char what[16];
    sscanf(script, "%d %15s %d %d", &at, what, &x, &y);
    if (!strcmp(what, "click")) {
        aoe_mouse_x = x; aoe_mouse_y = y;
        aoe_post(WM_MOUSEMOVE, 0, MAKELONG(x, y));
        aoe_post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELONG(x, y));
        aoe_post(WM_LBUTTONUP, 0, MAKELONG(x, y));
    } else if (!strcmp(what, "key")) {
        aoe_post(WM_KEYDOWN, x, 1);
        aoe_post(WM_KEYUP, x, 1);
    } else if (!strcmp(what, "char")) {
        aoe_post(WM_CHAR, x, 1);
    } else if (!strcmp(what, "quit")) {
        fprintf(stderr, "aoe: script's end, frames %ld\n", frames);
        exit(0);
    }
    const char* e = strchr(script, ';');
    script = e ? e + 1 : "";
}
