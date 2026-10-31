// The machine's side: the clock, the fonts' file, and (AOE_NOWINDOW=1 for now) what stands for a window.
#include <sys/mman.h>
#include <sys/time.h>
#include "ddraw.h"
#include "aoe_port.h"
#include "aoe_fonts.h"
#include "aoe_window.h"
#undef open
#undef fopen

static int window_open;
unsigned long aoe_clock_ms(void) {
    if (window_open) return aoe_win_clock();
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
    static const char* last_face; static int last_size, last_bold; static const AoeFont* last;
    if (face == last_face && size == last_size && bold == last_bold) return last;
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
    last_face = face; last_size = size; last_bold = bold; last = best;
    return best;
}

// ------------------------------------------------------------------ the window, or under AOE_NOWINDOW=1 none
static unsigned char* page;
static int no_window = -1;
static int windowed(void) {
    if (no_window < 0) no_window = getenv("AOE_NOWINDOW") != 0;
    return !no_window;
}
static const char* wtitle = "Age of Empires";
unsigned char* aoe_page(int w, int h) {
    if (windowed()) { page = aoe_win_open(w, h, wtitle); window_open = 1; }
    else page = (unsigned char*) calloc(w, h + 1);
    return page;
}
void aoe_palette_set(const PALETTEENTRY* p) { if (window_open) aoe_win_palette((const unsigned char*) p); }
static long frames;
long aoe_frames(void) { return frames; }
void aoe_present(void) {
    frames++;
    if (window_open) aoe_win_present();
}
void aoe_before_blit(void) { if (window_open) aoe_win_settle(); }
void aoe_window_make(int, int, const char*) {}
void aoe_window_close(void) { if (window_open) aoe_win_close(); window_open = 0; }

static void events_of_window(void) {
    static unsigned last;
    unsigned now = aoe_win_clock();
    if (now - last < 10) return;   // a look at the window system is a call to another program
    last = now;
    struct aoe_event e;
    while (aoe_win_event(&e)) {
        switch (e.kind) {
        case AOE_KEY_DOWN:
            if (e.a) { aoe_keys[e.a] = 1; aoe_post(WM_KEYDOWN, e.a, 1); }
            if (e.b >= 32 || e.b == 8 || e.b == 13 || e.b == 27) aoe_post(WM_CHAR, e.b, 1);
            break;
        case AOE_KEY_UP:
            if (e.a) { aoe_keys[e.a] = 0; aoe_post(WM_KEYUP, e.a, 1); }
            break;
        case AOE_BUTTON_DOWN:
            aoe_mouse_x = e.a; aoe_mouse_y = e.b;
            aoe_keys[e.c == 1 ? 1 : 2] = 1;
            aoe_post(e.c == 1 ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, e.c == 1 ? MK_LBUTTON : MK_RBUTTON, MAKELONG(e.a, e.b));
            break;
        case AOE_BUTTON_UP:
            aoe_mouse_x = e.a; aoe_mouse_y = e.b;
            aoe_keys[e.c == 1 ? 1 : 2] = 0;
            aoe_post(e.c == 1 ? WM_LBUTTONUP : WM_RBUTTONUP, 0, MAKELONG(e.a, e.b));
            break;
        case AOE_MOVE:
            aoe_mouse_x = e.a; aoe_mouse_y = e.b;
            aoe_post(WM_MOUSEMOVE, (aoe_keys[1] ? MK_LBUTTON : 0) | (aoe_keys[2] ? MK_RBUTTON : 0), MAKELONG(e.a, e.b));
            break;
        case AOE_CLOSE:
            aoe_post(WM_CLOSE, 0, 0);
            break;
        }
    }
}

// A script of input (AOE_SCRIPT="MS click X Y;MS key K"), each step MS after the screen last changed or the
// step before: for the tests. AOE_STATS_MS: a line of frames and the clock.
static const char* script;
static unsigned long step_from;
extern unsigned long aoe_updates, aoe_world_ms;
void aoe_panel_changed(const char* n) {
    step_from = timeGetTime();
    if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: panel %s at %lu, frame %ld\n", n, step_from, frames);
}
void aoe_events(void) {
    if (window_open) {
        events_of_window();
        // nothing drawn since the last look: the rest of the pass is another program's
        static long frames_seen;
        if (frames == frames_seen) aoe_win_rest();
        frames_seen = frames;
    }
    static int read_env;
    if (!read_env) { read_env = 1; script = getenv("AOE_SCRIPT"); }
    static int stats = -1;
    static unsigned long stats_at; static long stats_frames;
    if (stats < 0) { const char* e = getenv("AOE_STATS_MS"); stats = e ? atoi(e) : 0; }
    if (stats && timeGetTime() - stats_at >= (unsigned long) stats) {
        static unsigned long stats_updates, stats_world;
        fprintf(stderr, "aoestat: %lu ms, frames %ld (+%ld), updates +%lu, game ms +%lu\n", timeGetTime(), frames, frames - stats_frames,
            aoe_updates - stats_updates, aoe_world_ms - stats_world);
        stats_at = timeGetTime(); stats_frames = frames; stats_updates = aoe_updates; stats_world = aoe_world_ms;
    }
    // AOE_MARK=A,B: the span of world updates A to B, for tools/game_par.py and tools/prof.py
    static int mark_read, mark_a, mark_b;
    if (!mark_read) { const char* e = getenv("AOE_MARK"); mark_read = 1; if (e) sscanf(e, "%d,%d", &mark_a, &mark_b); }
    if (mark_a) {
        static unsigned long m_ms, m_world; static long m_frames; static unsigned long m_updates;
        if (mark_a > 0 && aoe_updates >= (unsigned long) mark_a) {
            mark_a = -2;
            m_ms = timeGetTime(); m_world = aoe_world_ms; m_frames = frames; m_updates = aoe_updates;
            fprintf(stderr, "aoe: mark begins\n");
        } else if (mark_a == -2 && aoe_updates >= (unsigned long) mark_b) {
            mark_a = 0;
            fprintf(stderr, "aoe: mark ends, %ld frames in %lu ms, updates %lu, game ms %lu\n", frames - m_frames, timeGetTime() - m_ms,
                aoe_updates - m_updates, aoe_world_ms - m_world);
        }
    }
    if (!script || !*script) return;
    int at;
    if (sscanf(script, "%d", &at) != 1 || timeGetTime() - step_from < (unsigned long) at) return;
    step_from = timeGetTime();
    fprintf(stderr, "aoe: step %.30s at %lu\n", script, step_from);
    int x, y;
    char what[16];
    sscanf(script, "%d %15[^ ;] %d %d", &at, what, &x, &y);
    if (!strcmp(what, "click")) {
        aoe_mouse_x = x; aoe_mouse_y = y;
        aoe_post(WM_MOUSEMOVE, 0, MAKELONG(x, y));
        aoe_post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELONG(x, y));
        aoe_post(WM_LBUTTONUP, 0, MAKELONG(x, y));
    } else if (!strcmp(what, "rclick")) {
        aoe_mouse_x = x; aoe_mouse_y = y;
        aoe_post(WM_MOUSEMOVE, 0, MAKELONG(x, y));
        aoe_post(WM_RBUTTONDOWN, MK_RBUTTON, MAKELONG(x, y));
        aoe_post(WM_RBUTTONUP, 0, MAKELONG(x, y));
    } else if (!strcmp(what, "move")) {
        aoe_mouse_x = x; aoe_mouse_y = y;
        aoe_post(WM_MOUSEMOVE, 0, MAKELONG(x, y));
    } else if (!strcmp(what, "close")) {
        aoe_post(WM_CLOSE, 0, 0);
    } else if (!strcmp(what, "key")) {
        aoe_post(WM_KEYDOWN, x, 1);
        aoe_post(WM_KEYUP, x, 1);
    } else if (!strcmp(what, "char")) {
        aoe_post(WM_CHAR, x, 1);
    } else if (!strcmp(what, "quit")) {
        fprintf(stderr, "aoe: script's end, frames %ld\n", frames);
        aoe_window_close();
        _exit(0);
    }
    const char* e = strchr(script, ';');
    script = e ? e + 1 : "";
}
