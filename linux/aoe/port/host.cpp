// The machine's side: the clock, the fonts' file, and (AOE_NOWINDOW=1 for now) what stands for a window.
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
extern "C" int mcw_on_worker(void);
#include <sys/time.h>
#include "ddraw.h"
#include "aoe_port.h"
#include "aoe_fonts.h"
#include "aoe_window.h"
#include "aoe_cores.h"
extern "C" void aoe_shot(void);
#undef open
#undef fopen

static int window_open;
extern "C" int aoe_start_width(void) {
    const char* e = getenv("AOE_SIZE");
    if (e) return atoi(e) == 800 ? 800 : 640;
    // started maximised (SE_MAXIMISED, linux/apps/ui_launch.h): the layout the window's size wants from the start (aoe_win_layout's)
    int w, h;
    if ((e = getenv("SE_MAXIMISED")) && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 760 && h >= 540) return 800;
    return 640;
}
extern "C" int aoe_scroll_edges(void) { const char* e = getenv("AOE_SCROLL"); return e && !strcmp(e, "edge"); }
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
// AOE_LAT=1: what became of each input, and every picture, kept in memory and written at the end (docs/games/aoe.md, "Playing").
// M a move came, B a button came (a x, b y), m a move's message taken, d a button's message taken (a the queue's length),
// e the message before was done, P a picture, U a world update, R a reply a panel drew (a what).
struct LatRecord { unsigned ms, instructions, frame; unsigned short kind, a; int b; };
enum { LAT_RECORDS = 1 << 17 };
static LatRecord* lat; static int lat_n, lat_on = -1;
static volatile sig_atomic_t lat_stop;
static void lat_term(int) { lat_stop = 1; }
void aoe_lat(int kind, int a, int b) {
    if (lat_on < 0) {
        lat_on = getenv("AOE_LAT") != 0;
        if (lat_on) { lat = (LatRecord*) malloc(sizeof(LatRecord) * LAT_RECORDS); signal(SIGTERM, lat_term); }
    }
    if (!lat_on || lat_n == LAT_RECORDS || mcw_on_worker()) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    unsigned count; __asm__ volatile("rdinstret %0" : "=r"(count));
    LatRecord* r = &lat[lat_n++];
    r->ms = t.tv_sec * 1000 + t.tv_nsec / 1000000; r->instructions = count; r->frame = frames; r->kind = kind; r->a = a; r->b = b;
}
static void lat_write(void) {
    aoe_win_settle();
    for (int i = 0; i < lat_n; i++) fprintf(stderr, "L %u %u %u %c %d %d\n", lat[i].ms, lat[i].instructions, lat[i].frame, lat[i].kind, lat[i].a, lat[i].b);
    fprintf(stderr, "aoe: mark over\n");
    aoe_window_close();
    _exit(0);
}
void aoe_present(void) {
    frames++;
    { extern void aoe_sound_look(void); aoe_sound_look(); }
    aoe_lat('P', 0, 0);
    // AOE_PICSUM=N: a sum of the picture every N frames, to compare two ways of drawing it
    static int picsum = -1; static long picdump = getenv("AOE_PICDUMP") ? atol(getenv("AOE_PICDUMP")) : 0;
    if (picsum < 0) { const char* e = getenv("AOE_PICSUM"); picsum = e ? atoi(e) : 0; }
    if (picsum && page && frames % picsum == 0) {
        if (window_open) aoe_win_settle();
        const unsigned* p = (const unsigned*) page; unsigned hs[3] = {2166136261u, 2166136261u, 2166136261u};
        for (int i = 0; i < aoe_w * aoe_h / 4; i++) { int row = i / (aoe_w / 4), b = row < 20 ? 0 : row < aoe_h - 126 ? 1 : 2; hs[b] = (hs[b] ^ p[i]) * 16777619u; }
        fprintf(stderr, "aoepic %ld %08x %08x %08x\n", frames, hs[0], hs[1], hs[2]);
        if (frames == picdump) for (int r = 0; r < 30; r++) { fprintf(stderr, "aoegrid %02d ", r); for (int c = 0; c < 40; c++) { unsigned g = 0; for (int y = 0; y < 20; y++) for (int x = 0; x < 20; x++) g = g * 31 + page[(r * 20 + y) * aoe_w + c * 20 + x]; fprintf(stderr, "%02x", (g ^ g >> 8 ^ g >> 16) & 255); } fprintf(stderr, "\n"); }
        if (frames == picdump && getenv("AOE_PICROWS")) for (int y = aoe_h - 126; y < aoe_h; y++) { fprintf(stderr, "aoerow "); for (int x = 0; x < 260; x++) fprintf(stderr, "%02x", page[y * aoe_w + x]); fprintf(stderr, "\n"); }
    }
    // the caption's rate counts pictures that show something new: a present between two slices with no view drawn is not one
    extern int aoe_view_new_picture(void);
    if (window_open) { if (!aoe_view_new_picture()) aoe_win_not_a_frame(); aoe_win_present(); }
}
void aoe_before_blit(void) { if (window_open) aoe_win_settle(); }
void aoe_window_make(int, int, const char*) {}
void aoe_window_close(void) { if (window_open) aoe_win_close(); window_open = 0; }

// AOE_INPUT_LOG=1: what the right button and the left one's box came to (docs/games/aoe.md, "Controls")
void aoe_input_log(const char* what, int a, int b, int c, int d) {
    static int on = -1;
    if (on < 0) on = getenv("AOE_INPUT_LOG") != 0;
    if (on) fprintf(stderr, "aoeinput: %s %d %d %d %d\n", what, a, b, c, d);
}

// A button held in a beam's hand: the machine's own words follow it past the window's sides and say when it comes up, where the window
// system's events would stop or be owed (docs/games/aoe.md, "Controls"). Only a press those words have shown: nxkey's presses are not in them.
static int held_seen[3], held_ox, held_oy;
static void held_by_words(void) {
    int bits = aoe_win_buttons();
    for (int c = 1; c <= 2; c++) {
        if (!aoe_keys[c]) { held_seen[c] = 0; continue; }
        if (bits & c) {
            if (!held_seen[c]) aoe_win_origin(&held_ox, &held_oy);
            held_seen[c] = 1;
            int x, y; aoe_win_pointer(&x, &y); x -= held_ox; y -= held_oy;
            aoe_win_map(&x, &y);   // (the picture is stretched over a resized window)
            if (x != aoe_mouse_x || y != aoe_mouse_y) {
                aoe_mouse_x = x; aoe_mouse_y = y;
                aoe_lat('M', x, y);
                aoe_post(WM_MOUSEMOVE, (aoe_keys[1] ? MK_LBUTTON : 0) | (aoe_keys[2] ? MK_RBUTTON : 0), MAKELONG(x, y));
            }
        } else if (held_seen[c]) {   // up in the words: the window's event, if it comes, finds it up already
            held_seen[c] = 0;
            aoe_keys[c] = 0;
            aoe_post(c == 1 ? WM_LBUTTONUP : WM_RBUTTONUP, 0, MAKELONG(aoe_mouse_x, aoe_mouse_y));
        }
    }
}

// After the script's "ready": /tmp/aoe-size says where the window is and how big, each time that changes (a test that resizes it).
static int size_told;
static void size_tell(void) {
    static int last_w, last_h;
    int w, h, ox, oy;
    aoe_win_size(&w, &h);
    if (w == last_w && h == last_h) return;
    last_w = w; last_h = h;
    aoe_win_origin(&ox, &oy);
    FILE* f = fopen("/tmp/aoe-size.part", "w");
    if (f) { fprintf(f, "%d %d %d %d\n", ox, oy, w, h); fclose(f); rename("/tmp/aoe-size.part", "/tmp/aoe-size"); }
    fprintf(stderr, "aoe: window at %d,%d, %d x %d\n", ox, oy, w, h);
}

static void events_of_window(void) {
    static unsigned last;
    unsigned now = aoe_win_clock();
    if (now - last < 10) return;   // a look at the window system is a call to another program
    last = now;
    struct aoe_event e;
    held_by_words();
    if (size_told) size_tell();
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
            aoe_lat('B', e.a, e.b);
            aoe_mouse_x = e.a; aoe_mouse_y = e.b;
            aoe_input_log("press", e.c, e.a, e.b, aoe_win_buttons());
            aoe_keys[e.c == 1 ? 1 : 2] = 1;
            aoe_post(e.c == 1 ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, e.c == 1 ? MK_LBUTTON : MK_RBUTTON, MAKELONG(e.a, e.b));
            break;
        case AOE_BUTTON_UP:
            if (!aoe_keys[e.c == 1 ? 1 : 2]) break;   // (the words said so first)
            aoe_mouse_x = e.a; aoe_mouse_y = e.b;
            aoe_keys[e.c == 1 ? 1 : 2] = 0;
            aoe_post(e.c == 1 ? WM_LBUTTONUP : WM_RBUTTONUP, 0, MAKELONG(e.a, e.b));
            break;
        case AOE_MOVE:
            if ((aoe_keys[1] && held_seen[1]) || (aoe_keys[2] && held_seen[2])) break;   // (the words are ahead of it)
            aoe_lat('M', e.a, e.b);
            aoe_mouse_x = e.a; aoe_mouse_y = e.b;
            aoe_post(WM_MOUSEMOVE, (aoe_keys[1] ? MK_LBUTTON : 0) | (aoe_keys[2] ? MK_RBUTTON : 0), MAKELONG(e.a, e.b));
            break;
        case AOE_CLOSE:
            fprintf(stderr, "aoe: the window was closed\n");
            aoe_post(WM_CLOSE, 0, 0);
            break;
        }
    }
    int layout = aoe_win_layout();
    if (layout) aoe_post(AOE_WM_LAYOUT, layout, 0);
}

// A script of input (AOE_SCRIPT="MS click X Y;MS key K"), each step MS after the screen last changed or the
// step before: for the tests. AOE_STATS_MS: a line of frames and the clock.
static const char* script;
static unsigned long step_from;
static int box_x, box_y, box_x2, box_y2, box_step = -1;   // the script's box
static unsigned long box_at;
static const char* chat_text;   // the script's chat: its text, typed once the line is up
static unsigned long chat_at;
extern unsigned long aoe_updates, aoe_world_ms;
extern "C" int aoe_layout_waiting(void);
extern "C" int aoe_layout_remakes(const char* name);
extern "C" int aoe_layout_height_wrong(const char* name);
void aoe_panel_changed(const char* n) {
    step_from = timeGetTime();
    if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: panel %s at %lu, frame %ld\n", n, step_from, frames);
    // a layout that waited at a menu (port/aoe_layout.cpp): made where the next message is handled
    int waiting = aoe_layout_waiting();
    if (waiting && (!strcmp(n, "Game Screen") || aoe_layout_remakes(n))) aoe_post(AOE_WM_LAYOUT, waiting, 0);
    // the game screen in the window's shape, a remade menu at 4:3 (aoe_layout_switch): the height changes going between them
    else if ((!strcmp(n, "Game Screen") || aoe_layout_remakes(n)) && aoe_layout_height_wrong(n)) aoe_post(AOE_WM_LAYOUT, aoe_w, 0);
}
static int mark_read, mark_a, mark_b;
void aoe_view_check_draw(int whole);
static unsigned checks, checks_bad;
static void aoe_view_check(int quiet) {
    if (!page) return;
    int top = 20, bottom = aoe_h - 126;   // (the view's rows at 640 x 480 in the window; 800: 20 to 474)
    unsigned bytes = aoe_w * (bottom - top);
    static unsigned char* drawn;
    if (!drawn) drawn = (unsigned char*) malloc(bytes);
    aoe_view_check_draw(0);
    memcpy(drawn, page + top * aoe_w, bytes);
    aoe_view_check_draw(1);
    unsigned differ = 0, first = 0;
    for (unsigned i = 0; i < bytes; i++) if (drawn[i] != page[top * aoe_w + i]) { if (!differ++) first = i; }
    checks++;
    if (differ) checks_bad++;
    fprintf(stderr, "aoecheck: %u pixels differ (first at %u,%u), %u of %u checks\n", differ, first % aoe_w, top + first / aoe_w, checks_bad, checks);
    if (differ && !quiet) {
        static unsigned char* whole;
        if (!whole) whole = (unsigned char*) malloc(bytes);
        memcpy(whole, page + top * aoe_w, bytes);
        memcpy(page + top * aoe_w, drawn, bytes); aoe_shot();
        memcpy(page + top * aoe_w, whole, bytes); aoe_shot();
    }
}
static void mark_from_script(int n) { mark_read = 1; mark_a = (int) aoe_updates + 1; mark_b = mark_a + n; }
void aoe_events(void) {
    if (lat_stop) lat_write();
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
    if (!mark_read) { const char* e = getenv("AOE_MARK"); mark_read = 1; if (e) sscanf(e, "%d,%d", &mark_a, &mark_b); }
    if (mark_a) {
        static unsigned long m_ms, m_world; static long m_frames; static unsigned long m_updates;
        if (mark_a > 0 && aoe_updates >= (unsigned long) mark_a) {
            mark_a = -2;
            m_ms = timeGetTime(); m_world = aoe_world_ms; m_frames = frames; m_updates = aoe_updates;
            aoe_phases_reset();
            fprintf(stderr, "aoe: mark begins\n");
        } else if (mark_a == -2 && aoe_updates >= (unsigned long) mark_b) {
            mark_a = 0;
            fprintf(stderr, "aoe: mark ends, %ld frames in %lu ms, updates %lu, game ms %lu\n", frames - m_frames, timeGetTime() - m_ms,
                aoe_updates - m_updates, aoe_world_ms - m_world);
            aoe_phases_print(frames - m_frames);
            fprintf(stderr, "aoe: mark over\n");   // (what a run stops at: the line before is whole by then)
        }
    }
    // "rsweep X Y": the right button pressed at X,Y and the pointer moved 20 pixels every 100 ms, 200 to the left and back, ten
    // times, then let go (game_par.py's aoe-drag-scroll: the map dragged all the time)
    static int sweep_x, sweep_y, sweep_step = -1;
    static unsigned long sweep_at;
    if (sweep_step >= 0 && timeGetTime() - sweep_at >= 100) {
        sweep_at = timeGetTime();
        if (++sweep_step > 200) {
            aoe_keys[2] = 0; sweep_step = -1;
            aoe_post(WM_RBUTTONUP, 0, MAKELONG(aoe_mouse_x, aoe_mouse_y));
        } else {
            int k = sweep_step % 20;
            aoe_mouse_x = sweep_x - 20 * (k <= 10 ? k : 20 - k); aoe_mouse_y = sweep_y;
            aoe_post(WM_MOUSEMOVE, MK_RBUTTON, MAKELONG(aoe_mouse_x, aoe_mouse_y));
        }
    }
    if (box_step >= 0 && timeGetTime() - box_at >= 100) {
        box_at = timeGetTime();
        if (++box_step > 5) {
            aoe_keys[1] = 0; box_step = -1;
            aoe_post(WM_LBUTTONUP, 0, MAKELONG(box_x2, box_y2));
        } else {
            aoe_mouse_x = box_x + (box_x2 - box_x) * box_step / 5; aoe_mouse_y = box_y + (box_y2 - box_y) * box_step / 5;
            aoe_post(WM_MOUSEMOVE, MK_LBUTTON, MAKELONG(aoe_mouse_x, aoe_mouse_y));
        }
    }
    if (chat_text && timeGetTime() - chat_at >= 500) {
        for (const char* t = chat_text; *t && *t != ';'; t++) aoe_post(WM_CHAR, (unsigned char) *t, 1);
        aoe_post(WM_KEYDOWN, 13, 1); aoe_post(WM_KEYUP, 13, 1);
        chat_text = 0;
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
        aoe_post(WM_MOUSEMOVE, (aoe_keys[1] ? MK_LBUTTON : 0) | (aoe_keys[2] ? MK_RBUTTON : 0), MAKELONG(x, y));
    } else if (!strcmp(what, "rsweep")) {
        aoe_mouse_x = sweep_x = x; aoe_mouse_y = sweep_y = y;
        aoe_keys[2] = 1; sweep_step = 0; sweep_at = timeGetTime();
        aoe_post(WM_MOUSEMOVE, 0, MAKELONG(x, y));
        aoe_post(WM_RBUTTONDOWN, MK_RBUTTON, MAKELONG(x, y));
    } else if (!strcmp(what, "box")) {
        // "box X1 Y1 X2 Y2": the left button pressed at one corner and dragged to the other over half a second, then let go
        // (a selection of many: a press held shorter is a click)
        box_x2 = x; box_y2 = y;
        sscanf(script, "%*d %*s %*d %*d %d %d", &box_x2, &box_y2);
        box_x = aoe_mouse_x = x; box_y = aoe_mouse_y = y;
        box_step = 0; box_at = timeGetTime();
        aoe_keys[1] = 1;
        aoe_post(WM_MOUSEMOVE, 0, MAKELONG(x, y));
        aoe_post(WM_LBUTTONDOWN, MK_LBUTTON, MAKELONG(x, y));
    } else if (!strcmp(what, "chat")) {
        // "chat TEXT": Enter, then the text and Enter half a second later, when the chat's line is up (the game's cheats:
        // game_par.py's busy scene)
        chat_text = strstr(script, "chat ") + 5;
        chat_at = timeGetTime();
        aoe_post(WM_KEYDOWN, 13, 1); aoe_post(WM_KEYUP, 13, 1);
    } else if (!strcmp(what, "mark")) {
        // "mark N": AOE_MARK's span from the next update, N updates long (a scene measured from a step of its script)
        mark_from_script(x);
    } else if (!strcmp(what, "check")) {
        // "check": the picture the view has drawn by its scrolls and redraws against one drawn whole from the same state of the
        // game (as OpenTTD_CHECK): "aoecheck: N pixels differ" in the view's rows, and both pictures as shots when they do
        aoe_view_check(x);
    } else if (!strcmp(what, "shot")) {
        aoe_view_join();   // (the view's job done: a picture whole, not one half drawn)
        aoe_shot();
    } else if (!strcmp(what, "close")) {
        aoe_post(WM_CLOSE, 0, 0);
    } else if (!strcmp(what, "key")) {
        aoe_post(WM_KEYDOWN, x, 1);
        aoe_post(WM_KEYUP, x, 1);
    } else if (!strcmp(what, "char")) {
        aoe_post(WM_CHAR, x, 1);
    } else if (!strcmp(what, "ready")) {
        // the game's up: says where the window's picture lies on the screen, for a script of real input
        int ox = 0, oy = 0; aoe_win_origin(&ox, &oy);
        FILE* f = fopen("/tmp/aoe-ready", "w");
        if (f) { fprintf(f, "%d %d\n", ox, oy); fclose(f); }
        size_told = 1;
    } else if (!strcmp(what, "view")) {
        // where the view stands, a hundredth of a tile, and how many are selected
        extern void aoe_view_place(float*, float*, int*);
        float vx, vy; int selected;
        aoe_view_place(&vx, &vy, &selected);
        fprintf(stderr, "aoeview: %d %d selected %d\n", (int) (vx * 100), (int) (vy * 100), selected);
    } else if (!strcmp(what, "quit")) {
        fprintf(stderr, "aoe: script's end, frames %ld\n", frames);
        aoe_window_close();
        _exit(0);
    }
    const char* e = strchr(script, ';');
    script = e ? e + 1 : "";
}
