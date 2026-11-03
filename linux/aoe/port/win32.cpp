#include "aoe_cores.h"
// Windows, as far as the game asks for it: files, the clock, strings, the registry, windows and
// messages (one real window, the rest records). docs/games/aoe.md, "What stands for Windows".
#include <dirent.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdarg.h>
#include "aoe_port.h"
#undef open
#undef fopen
#undef time

// ------------------------------------------------------------------ paths
// The trial's files all lie in one folder. A game's path is that folder's file of the same name in
// any case; what it writes lies under its home, in the folders it says.
static const char* data_dir(void) { const char* d = getenv("AOE_DATA"); return d ? d : "/usr/share/aoe"; }
static const char* home_dir(void) { const char* d = getenv("AOE_HOME"); return d ? d : "/tmp/aoe"; }

static void lower_path(char* out, const char* in, int size) {
    int i = 0;
    for (; in[i] && i < size - 1; i++) {
        char c = in[i] == '\\' ? '/' : in[i];
        out[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    out[i] = 0;
    // "C:/x" and "./x"
    if (out[0] && out[1] == ':') memmove(out, out + 2, strlen(out + 2) + 1);
    while (out[0] == '.' && out[1] == '/') memmove(out, out + 2, strlen(out + 2) + 1);
    while (out[0] == '/') memmove(out, out + 1, strlen(out + 1) + 1);
}

// names of the data folder, read once: a search goes through a table, not the ROM
static struct DataName { char lower[40]; char real[40]; } *data_names;
static int data_count;
static void data_read(void) {
    if (data_names) return;
    data_names = (DataName*) calloc(256, sizeof(DataName));
    DIR* d = opendir(data_dir());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || data_count == 256 || strlen(e->d_name) >= 40) continue;
        strcpy(data_names[data_count].real, e->d_name);
        lower_path(data_names[data_count].lower, e->d_name, 40);
        data_count++;
    }
    closedir(d);
}
static const char* data_file(const char* base) {
    data_read();
    for (int i = 0; i < data_count; i++) if (!strcmp(data_names[i].lower, base)) return data_names[i].real;
    return 0;
}

// the path a game's name is to be read from; 0 if there is none
static int resolve_read(const char* name, char* out, int size) {
    char l[300];
    lower_path(l, name, sizeof l);
    snprintf(out, size, "%s/%s", home_dir(), l);
    if (access(out, 0) == 0) return 1;
    const char* base = strrchr(l, '/');
    base = base ? base + 1 : l;
    // the expansion's folder has the same files' names, and the trial no expansion
    if (!strncmp(l, "data2/", 6) && strstr(base, ".drs")) return 0;
    const char* real = data_file(base);
    if (!real) return 0;
    snprintf(out, size, "%s/%s", data_dir(), real);
    return 1;
}
static void make_parents(const char* path) {
    char t[300];
    strncpy(t, path, sizeof t - 1);
    t[sizeof t - 1] = 0;
    for (char* p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
}
static void resolve_write(const char* name, char* out, int size) {
    char l[300];
    lower_path(l, name, sizeof l);
    snprintf(out, size, "%s/%s", home_dir(), l);
    make_parents(out);
}
extern "C" int aoe_open(const char* name, int flags, ...) {
    char path[320];
    int mode = 0666;
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) {
        if ((flags & O_CREAT) == 0 && !resolve_read(name, path, sizeof path)) { errno = ENOENT; return -1; }
        if (flags & O_CREAT) resolve_write(name, path, sizeof path);
    } else if (!resolve_read(name, path, sizeof path)) {
        if (getenv("AOE_FILES")) fprintf(stderr, "aoefile: no %s\n", name);
        errno = ENOENT;
        return -1;
    }
    if (getenv("AOE_FILES")) fprintf(stderr, "aoefile: %s -> %s\n", name, path);
    return open(path, flags, mode);
}
extern "C" FILE* aoe_fopen(const char* name, const char* how) {
    char path[320];
    if (how[0] == 'r' && !strchr(how, '+')) {
        if (!resolve_read(name, path, sizeof path)) return 0;
    } else resolve_write(name, path, sizeof path);
    return fopen(path, how);
}
extern "C" int aoe_unlink(const char* name) { char p[320]; resolve_write(name, p, sizeof p); return unlink(p); }
extern "C" int aoe_access(const char* name, int m) { char p[320]; return resolve_read(name, p, sizeof p) ? 0 : -1; }
extern "C" int aoe_chdir(const char*) { return 0; }
extern "C" int _chdrive(int) { return 0; }
extern "C" int _getdrive(void) { return 3; }
extern "C" long _filelength(int fd) { struct stat s; return fstat(fd, &s) ? -1 : (long) s.st_size; }
extern "C" char* _pgmptr_value;
char* _pgmptr = (char*) "empires.exe";

// _findfirst: *.ext (or a whole name) in a folder of the home and in the data folder
struct Find { char ext[16]; char names[64][64]; long sizes[64]; int n, at; };
static int match_pattern(const char* name, const char* pat) {
    if (pat[0] == '*') {
        const char* e = pat + 1;
        if (!strcmp(e, ".*") || !*e) return 1;
        int ln = strlen(name), le = strlen(e);
        return ln >= le && !strcasecmp(name + ln - le, e);
    }
    return !strcasecmp(name, pat);
}
static void find_scan(Find* f, const char* dir, const char* pat) {
    DIR* d = opendir(dir);
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || f->n == 64 || !match_pattern(e->d_name, pat)) continue;
        strncpy(f->names[f->n], e->d_name, 63);
        char p[320];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat s;
        f->sizes[f->n] = stat(p, &s) ? 0 : (long) s.st_size;
        f->n++;
    }
    closedir(d);
}
static void find_fill(Find* f, struct _finddata_t* o) {
    memset(o, 0, sizeof *o);
    strncpy(o->name, f->names[f->at], 259);
    o->size = f->sizes[f->at];
    o->attrib = _A_NORMAL;
    f->at++;
}
extern "C" long _findfirst(const char* spec, struct _finddata_t* o) {
    char l[300], dir[320];
    lower_path(l, spec, sizeof l);
    const char* pat = strrchr(l, '/');
    Find* f = (Find*) calloc(1, sizeof(Find));
    if (pat) {
        int n = pat - l;
        snprintf(dir, sizeof dir, "%s/%.*s", home_dir(), n, l);
        pat++;
    } else {
        snprintf(dir, sizeof dir, "%s", home_dir());
        pat = l;
    }
    find_scan(f, dir, pat);
    find_scan(f, data_dir(), pat);
    if (!f->n && !strcmp(pat, "empires.exe")) { strcpy(f->names[0], "Empires.exe"); f->n = 1; } // the game looks for itself
    if (!f->n) { free(f); return -1; }
    find_fill(f, o);
    return (long) f;
}
extern "C" int _findnext(long h, struct _finddata_t* o) {
    Find* f = (Find*) h;
    if (!f || f->at >= f->n) return -1;
    find_fill(f, o);
    return 0;
}
extern "C" int _findclose(long h) { free((void*) h); return 0; }

extern "C" void _splitpath(const char* p, char* drive, char* dir, char* fn, char* ext) {
    if (drive) drive[0] = 0;
    const char* s = strrchr(p, '\\');
    const char* s2 = strrchr(p, '/');
    if (s2 > s) s = s2;
    const char* base = s ? s + 1 : p;
    if (dir) { int n = base - p; memcpy(dir, p, n); dir[n] = 0; }
    const char* e = strrchr(base, '.');
    if (fn) { int n = e ? e - base : strlen(base); memcpy(fn, base, n); fn[n] = 0; }
    if (ext) strcpy(ext, e ? e : "");
}
extern "C" void _makepath(char* out, const char* d, const char* dir, const char* fn, const char* ext) {
    sprintf(out, "%s%s%s%s", d ? d : "", dir ? dir : "", fn ? fn : "", ext ? ext : "");
}

// old-style files
extern "C" int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) {
    fprintf(stderr, "aoe: message box: %s: %s\n", title ? title : "", text ? text : "");
    return 1;
}
extern "C" HFILE _lopen(LPCSTR n, int m) { return aoe_open(n, m & 3); }
extern "C" HFILE _lcreat(LPCSTR n, int) { return aoe_open(n, O_CREAT | O_WRONLY | O_TRUNC); }
extern "C" HFILE _lclose(HFILE f) { return close(f); }
extern "C" UINT _lread(HFILE f, LPVOID b, UINT n) { return read(f, b, n); }
extern "C" UINT _lwrite(HFILE f, LPCSTR b, UINT n) { return write(f, b, n); }
extern "C" LONG _llseek(HFILE f, LONG o, int w) { return lseek(f, o, w); }
extern "C" LONG _hread(HFILE f, LPVOID b, LONG n) { return read(f, b, n); }
extern "C" LONG _hwrite(HFILE f, LPCSTR b, LONG n) { return write(f, b, n); }
extern "C" HFILE OpenFile(LPCSTR n, LPOFSTRUCT o, UINT how) {
    if (how & OF_EXIST) return aoe_access(n, 0) == 0 ? 1 : -1;
    if (how & OF_CREATE) return aoe_open(n, O_CREAT | O_RDWR | O_TRUNC);
    return aoe_open(n, how & 3);
}
extern "C" HANDLE CreateFileA(LPCSTR n, DWORD acc, DWORD, void*, DWORD how, DWORD, HANDLE) {
    int fl = (acc & GENERIC_WRITE) ? ((acc & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (how == CREATE_ALWAYS) fl |= O_CREAT | O_TRUNC;
    if (how == OPEN_ALWAYS && (acc & GENERIC_WRITE)) fl |= O_CREAT;
    int fd = aoe_open(n, fl);
    return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE) (long) (fd + 1);
}
extern "C" HANDLE FindFirstFileA(LPCSTR, LPWIN32_FIND_DATA) { return INVALID_HANDLE_VALUE; }
extern "C" BOOL FindClose(HANDLE) { return 1; }
extern "C" DWORD GetTempPathA(DWORD n, LPSTR b) { snprintf(b, n, "%s/", home_dir()); return strlen(b); }
extern "C" UINT GetTempFileNameA(LPCSTR, LPCSTR pre, UINT u, LPSTR out) { static int k; sprintf(out, "tmp%d.tmp", ++k); return k; }
extern "C" DWORD GetCurrentDirectoryA(DWORD n, LPSTR b) { snprintf(b, n, "C:\\"); return 3; }
extern "C" UINT GetDriveTypeA(LPCSTR) { return DRIVE_FIXED; }
extern "C" BOOL GetVolumeInformationA(LPCSTR, LPSTR v, DWORD n, LPDWORD ser, LPDWORD, LPDWORD, LPSTR, DWORD) {
    if (v && n) snprintf(v, n, "AOE");
    if (ser) *ser = 0;
    return 1;
}
// a mapped file: where it lies, when it is a file of the ROM
extern "C" HANDLE CreateFileMappingA(HANDLE f, void*, DWORD, DWORD, DWORD, LPCSTR) {
    int fd = dup((int) (long) f - 1);
    return fd < 0 ? (HANDLE) 0 : (HANDLE) (long) (fd + 1);
}
extern "C" LPVOID MapViewOfFile(HANDLE m, DWORD, DWORD, DWORD, DWORD) {
    int fd = (int) (long) m - 1;
    long n = lseek(fd, 0, SEEK_END);
    void* p = mmap(0, n, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) return 0;
    aoe_sound_mapped(p, n, fd);
    return p;
}
extern "C" BOOL UnmapViewOfFile(LPCVOID) { return 1; }
extern "C" BOOL FileTimeToSystemTime(const FILETIME*, LPSYSTEMTIME t) { memset(t, 0, sizeof *t); t->wYear = 1997; t->wMonth = 10; t->wDay = 1; return 1; }

// ------------------------------------------------------------------ strings of the C library of Visual C++
extern "C" {
size_t _mbslen(const unsigned char* s) { return strlen((const char*) s); }
unsigned char* _mbsinc(const unsigned char* s) { return (unsigned char*) s + 1; }
unsigned char* _mbsdec(const unsigned char* b, const unsigned char* s) { return s > b ? (unsigned char*) s - 1 : 0; }
unsigned char* _mbsninc(const unsigned char* s, size_t n) { return (unsigned char*) s + n; }
unsigned char* _mbscpy(unsigned char* d, const unsigned char* s) { return (unsigned char*) strcpy((char*) d, (const char*) s); }
unsigned char* _mbscat(unsigned char* d, const unsigned char* s) { return (unsigned char*) strcat((char*) d, (const char*) s); }
unsigned char* _mbsncpy(unsigned char* d, const unsigned char* s, size_t n) { return (unsigned char*) strncpy((char*) d, (const char*) s, n); }
unsigned char* _mbsncat(unsigned char* d, const unsigned char* s, size_t n) { return (unsigned char*) strncat((char*) d, (const char*) s, n); }
unsigned char* _mbschr(const unsigned char* s, unsigned c) { return (unsigned char*) strchr((const char*) s, c); }
unsigned char* _mbsrchr(const unsigned char* s, unsigned c) { return (unsigned char*) strrchr((const char*) s, c); }
unsigned char* _mbsstr(const unsigned char* a, const unsigned char* b) { return (unsigned char*) strstr((const char*) a, (const char*) b); }
int _mbscmp(const unsigned char* a, const unsigned char* b) { return strcmp((const char*) a, (const char*) b); }
int _mbsicmp(const unsigned char* a, const unsigned char* b) { return strcasecmp((const char*) a, (const char*) b); }
char* strupr(char* s) { for (char* p = s; *p; p++) *p = toupper((unsigned char) *p); return s; }
char* strlwr(char* s) { for (char* p = s; *p; p++) *p = tolower((unsigned char) *p); return s; }
unsigned char* _mbsupr(unsigned char* s) { return (unsigned char*) strupr((char*) s); }
int _ismbcspace(unsigned c) { return isspace(c); }
int _ismbcdigit(unsigned c) { return isdigit(c); }
int _ismbblead(unsigned) { return 0; }
char* aoe_strupr(char* s) { return strupr(s); }
char* itoa(int v, char* b, int r) { if (r == 16) sprintf(b, "%x", v); else sprintf(b, "%d", v); return b; }
char* ltoa(long v, char* b, int r) { if (r == 16) sprintf(b, "%lx", v); else sprintf(b, "%ld", v); return b; }
char* ultoa(unsigned long v, char* b, int r) { if (r == 16) sprintf(b, "%lx", v); else sprintf(b, "%lu", v); return b; }
LPSTR CharUpperA(LPSTR s) { if ((unsigned long) s < 256) return (LPSTR) (long) toupper((int) (long) s); return strupr(s); }
int CompareStringA(DWORD, DWORD fl, LPCSTR a, int na, LPCSTR b, int nb) {
    if (na < 0) na = strlen(a);
    if (nb < 0) nb = strlen(b);
    int n = na < nb ? na : nb;
    int r = (fl & NORM_IGNORECASE) ? strncasecmp(a, b, n) : strncmp(a, b, n);
    if (!r) r = na - nb;
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}
BOOL IsDBCSLeadByte(BYTE) { return 0; }
int MulDiv(int a, int b, int c) { return c ? (int) ((long long) a * b / c) : -1; }
_PNH _set_new_handler(_PNH) { return 0; }
int _set_new_mode(int) { return 0; }
}

// ------------------------------------------------------------------ the clock
// A fixed step for runs that must come out the same (AOE_TICK_MS): the clock moves at each look for messages.
static unsigned long virtual_now;
static int tick_ms = -1;
int aoe_clock_still;   // (the game told of a song's end: when that comes is the card's, and must not move this clock)
extern "C" DWORD timeGetTime(void) {
    if (tick_ms < 0) { const char* e = getenv("AOE_TICK_MS"); tick_ms = e ? atoi(e) : 0; }
    if (tick_ms) return aoe_view_in_job() || aoe_clock_still ? virtual_now : virtual_now++;   // and a wait on the clock itself ends
    return aoe_clock_ms();
}
extern "C" DWORD GetTickCount(void) { return timeGetTime(); }
extern "C" void Sleep(DWORD ms) { if (tick_ms > 0) virtual_now += ms; else aoe_idle(ms); }
extern "C" void GlobalMemoryStatus(LPMEMORYSTATUS m) {
    memset(m, 0, sizeof *m);
    m->dwTotalPhys = m->dwAvailPhys = 200u << 20;
    m->dwTotalVirtual = m->dwAvailVirtual = 200u << 20;
    m->dwTotalPageFile = m->dwAvailPageFile = 200u << 20;
}
extern "C" DWORD GetLastError(void) { return 0; }
extern "C" void OutputDebugString(LPCSTR s) { if (getenv("AOE_DEBUG")) fprintf(stderr, "%s", s); }

// ------------------------------------------------------------------ memory handles: the pointer itself
extern "C" {
HGLOBAL GlobalAlloc(UINT f, DWORD n) { return calloc(1, n ? n : 1); }
HGLOBAL GlobalFree(HGLOBAL h) { free(h); return 0; }
LPVOID GlobalLock(HGLOBAL h) { return h; }
BOOL GlobalUnlock(HGLOBAL) { return 1; }
HGLOBAL GlobalHandle(LPCVOID p) { return (HGLOBAL) p; }
HGLOBAL GlobalReAlloc(HGLOBAL h, DWORD n, UINT) { return realloc(h, n); }
HGLOBAL LocalAlloc(UINT, DWORD n) { return calloc(1, n ? n : 1); }
HGLOBAL LocalFree(HGLOBAL h) { free(h); return 0; }
LPVOID VirtualAlloc(LPVOID, DWORD n, DWORD, DWORD) { return calloc(1, n); }
BOOL VirtualFree(LPVOID p, DWORD, DWORD t) { if (t & MEM_RELEASE) free(p); return 1; }
HANDLE CreateMutex(void*, BOOL, LPCSTR) { return (HANDLE) 1; }
BOOL ReleaseMutex(HANDLE) { return 1; }
BOOL CloseHandle(HANDLE h) { if ((long) h > 1) close((long) h - 1); return 1; }
HINSTANCE LoadLibrary(LPCSTR) { return (HINSTANCE) 1; }
BOOL FreeLibrary(HMODULE) { return 1; }
HMODULE GetModuleHandle(LPCSTR) { return (HMODULE) 1; }
HRSRC FindResource(HMODULE, LPCSTR, LPCSTR) { return 0; }
HGLOBAL LoadResource(HMODULE, HRSRC) { return 0; }
LPVOID LockResource(HGLOBAL) { return 0; }
HICON LoadIcon(HINSTANCE, LPCSTR) { return (HICON) 1; }
HCURSOR LoadCursor(HINSTANCE, LPCSTR) { return (HCURSOR) 1; }
BOOL WinHelpA(HWND, LPCSTR, UINT, DWORD) { return 1; }
UINT WinExec(LPCSTR, UINT) { return 0; }
BOOL ImmSetOpenStatus(HIMC, BOOL) { return 1; }
HIMC ImmAssociateContext(HWND, HIMC) { return 0; }
HIMC ImmGetContext(HWND) { return 0; }
BOOL ImmReleaseContext(HWND, HIMC) { return 1; }
BOOL ImmNotifyIME(HIMC, DWORD, DWORD, DWORD) { return 1; }
BOOL OpenClipboard(HWND) { return 0; }
BOOL CloseClipboard(void) { return 1; }
BOOL IsClipboardFormatAvailable(UINT) { return 0; }
HANDLE GetClipboardData(UINT) { return 0; }
UINT GetCaretBlinkTime(void) { return 500; }
BOOL SystemParametersInfoA(UINT, UINT, PVOID p, UINT) { if (p) *(int*) p = 0; return 1; }
BOOL SetSysColors(int, const int*, const COLORREF*) { return 1; }
DWORD GetSysColor(int) { return 0; }
BOOL MessageBeep(UINT) { return 1; }
BOOL ICInfo_(void) { return 0; }
int ICInfo(int, int, void*) { return 0; }
int GetSystemMetrics(int i) {
    switch (i) {
        case SM_CXSCREEN: case SM_CXFULLSCREEN: return aoe_w;
        case SM_CYSCREEN: case SM_CYFULLSCREEN: return aoe_h;
        case SM_MOUSEPRESENT: return 1;
        case SM_CMOUSEBUTTONS: return 3;
    }
    return 0;
}
// no networks, no CD; MCI and the mixer are aoe_music.cpp's
HRESULT DirectPlayCreate(void*, void**, void*) { return E_FAIL; }
HRESULT DirectPlayLobbyCreateA(void*, void**, void*, void*, DWORD) { return E_FAIL; }
HRESULT DirectPlayEnumerateA(void*, void*) { return E_FAIL; }
HWND MCIWndCreateA(HWND, HINSTANCE, DWORD, LPCSTR) { return 0; }
void ds_stream_init(void) {}
void ds_stream_exit(void) {}
void ds_stream_messages(void) {}
int ds_stream_file(void) { return 0; }
void ds_stream_stop(void) {}
void ds_stream_pause(void) {}
void ds_stream_resume(void) {}
void ds_stream_volume(void) {}
}
void GetDXVersion(unsigned long* v, unsigned long* p) { *v = 0x7fffffff; *p = 2; }

// ------------------------------------------------------------------ the registry: in memory; settings come later
struct RegValue { char key[140]; char name[40]; DWORD type, len; unsigned char data[64]; };
static RegValue reg_values[64];
static int reg_count;
extern "C" {
LONG RegOpenKeyExA(HKEY, LPCSTR k, DWORD, DWORD, PHKEY out) { *out = (HKEY) strdup(k ? k : ""); return 0; }
LONG RegCreateKeyExA(HKEY, LPCSTR k, DWORD, LPSTR, DWORD, DWORD, void*, PHKEY out, LPDWORD d) { *out = (HKEY) strdup(k ? k : ""); if (d) *d = 2; return 0; }
LONG RegCloseKey(HKEY k) { free(k); return 0; }
LONG RegQueryValueExA(HKEY k, LPCSTR n, LPDWORD, LPDWORD t, LPBYTE b, LPDWORD l) {
    for (int i = 0; i < reg_count; i++) {
        if (strcmp(reg_values[i].key, (char*) k) || strcasecmp(reg_values[i].name, n)) continue;
        if (t) *t = reg_values[i].type;
        if (b && l) memcpy(b, reg_values[i].data, *l < reg_values[i].len ? *l : reg_values[i].len);
        if (l) *l = reg_values[i].len;
        return 0;
    }
    return 2;
}
LONG RegSetValueExA(HKEY k, LPCSTR n, DWORD, DWORD t, const BYTE* b, DWORD l) {
    int i = 0;
    for (; i < reg_count; i++) if (!strcmp(reg_values[i].key, (char*) k) && !strcasecmp(reg_values[i].name, n)) break;
    if (i == 64 || l > 64) return 1;
    if (i == reg_count) reg_count++;
    strncpy(reg_values[i].key, (char*) k, 139);
    strncpy(reg_values[i].name, n, 39);
    reg_values[i].type = t;
    reg_values[i].len = l;
    memcpy(reg_values[i].data, b, l);
    return 0;
}
}

// ------------------------------------------------------------------ strings of language.dll, taken out when the image is made
struct Str { int id; char* s; };
static Str* strs;
static int str_count;
static void strs_read(void) {
    static int done;
    if (done) return;
    done = 1;
    char p[320];
    snprintf(p, sizeof p, "%s/language.txt", data_dir());
    FILE* f = fopen(p, "r");
    if (!f) { fprintf(stderr, "aoe: no %s\n", p); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* all = (char*) malloc(n + 1);
    fread(all, 1, n, f);
    all[n] = 0;
    fclose(f);
    int cap = 0;
    for (char* q = all; *q; q++) if (*q == '\n') cap++;
    strs = (Str*) malloc((cap + 1) * sizeof(Str));
    // lines id, a tab, the string with \n written so; in the order of the id
    for (char* q = all; *q;) {
        char* e = strchr(q, '\n');
        if (e) *e = 0;
        char* t = strchr(q, '\t');
        if (t) {
            strs[str_count].id = atoi(q);
            char* w = t + 1;
            strs[str_count].s = w;
            for (char* r = t + 1; *r; r++) {
                if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
                else if (r[0] == '\\' && r[1] == 'r') { *w++ = '\r'; r++; }
                else if (r[0] == '\\' && r[1] == '\\') { *w++ = '\\'; r++; }
                else *w++ = *r;
            }
            *w = 0;
            str_count++;
        }
        if (!e) break;
        q = e + 1;
    }
}
extern "C" int LoadString(HINSTANCE, UINT id, LPSTR b, int n) {
    strs_read();
    int lo = 0, hi = str_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (strs[m].id == (int) id) {
            strncpy(b, strs[m].s, n - 1);
            b[n - 1] = 0;
            return strlen(b);
        }
        if (strs[m].id < (int) id) lo = m + 1; else hi = m - 1;
    }
    if (n) b[0] = 0;
    return 0;
}

// ------------------------------------------------------------------ windows
struct Wnd {
    WNDPROC proc;
    char cls[32];
    char text[512];
    DWORD style, exstyle;
    LONG user;
    int x, y, w, h, visible;
    Wnd* parent;
    int sel0, sel1;
};
static struct { char name[32]; WNDPROC proc; } classes[8];
static int class_count;
static Wnd* main_wnd;
static Wnd* focus_wnd;
static Wnd* capture_wnd;
static Wnd* windows[32];
static int window_count;

#define QUEUE 256
static MSG queue[QUEUE];
static int q_head, q_tail;
static int need_paint;
static int quitting;

static LRESULT send_to(Wnd* w, UINT m, WPARAM a, LPARAM b) {
    if (!w) return 0;
    return w->proc ? w->proc((HWND) w, m, a, b) : DefWindowProc((HWND) w, m, a, b);
}
// an edit control: what its keys do, as the game's panels ask (pnl_edit.cpp)
static LRESULT edit_proc(HWND h, UINT m, WPARAM a, LPARAM b) {
    Wnd* w = (Wnd*) h;
    switch (m) {
        case WM_SETTEXT: strncpy(w->text, b ? (char*) b : "", sizeof w->text - 1); return 1;
        case WM_GETTEXT: strncpy((char*) b, w->text, a); if (a) ((char*) b)[a - 1] = 0; return strlen((char*) b);
        case WM_GETTEXTLENGTH: return strlen(w->text);
        case WM_CHAR: {
            int n = strlen(w->text);
            if (a == 8) { if (n) w->text[n - 1] = 0; }
            else if (a >= 32 && n < (int) sizeof w->text - 1) { w->text[n] = a; w->text[n + 1] = 0; }
            return 0;
        }
        case EM_GETSEL: return MAKELONG(strlen(w->text), strlen(w->text));
    }
    return DefWindowProc(h, m, a, b);
}
extern "C" {
ATOM RegisterClass(const WNDCLASS* c) {
    if (class_count == 8) return 0;
    strncpy(classes[class_count].name, c->lpszClassName, 31);
    classes[class_count].proc = c->lpfnWndProc;
    return ++class_count;
}
HWND CreateWindowEx(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent, HMENU, HINSTANCE, LPVOID param) {
    Wnd* n = (Wnd*) calloc(1, sizeof(Wnd));
    strncpy(n->cls, cls ? cls : "", 31);
    n->style = style;
    n->exstyle = ex;
    n->x = x == CW_USEDEFAULT ? 0 : x;
    n->y = y == CW_USEDEFAULT ? 0 : y;
    n->w = w; n->h = h;
    n->parent = (Wnd*) parent;
    n->visible = (style & WS_VISIBLE) != 0;
    if (title) strncpy(n->text, title, sizeof n->text - 1);
    for (int i = 0; i < class_count; i++) if (!strcasecmp(classes[i].name, n->cls)) n->proc = classes[i].proc;
    if (!n->proc && !strcasecmp(n->cls, "edit")) n->proc = edit_proc;
    if (window_count < 32) windows[window_count++] = n;
    if (!parent && !main_wnd && n->proc != edit_proc) {
        main_wnd = n;
        aoe_window_make(n->w, n->h, title);
    }
    CREATESTRUCT cs;
    memset(&cs, 0, sizeof cs);
    cs.lpCreateParams = param; cs.x = x; cs.y = y; cs.cx = w; cs.cy = h; cs.style = style; cs.lpszName = title; cs.lpszClass = cls;
    send_to(n, WM_CREATE, 0, (LPARAM) &cs);
    return n;
}
BOOL DestroyWindow(HWND h) {
    Wnd* w = (Wnd*) h;
    if (!w) return 0;
    send_to(w, WM_DESTROY, 0, 0);
    for (int i = 0; i < window_count; i++) if (windows[i] == w) { windows[i] = windows[--window_count]; break; }
    if (w == main_wnd) main_wnd = 0;
    if (w == focus_wnd) focus_wnd = 0;
    if (w == capture_wnd) capture_wnd = 0;
    free(w);
    return 1;
}
LRESULT DefWindowProc(HWND h, UINT m, WPARAM a, LPARAM b) {
    Wnd* w = (Wnd*) h;
    switch (m) {
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_PAINT: need_paint = 0; return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SETTEXT: if (w) strncpy(w->text, b ? (char*) b : "", sizeof w->text - 1); return 1;
        case WM_GETTEXT: if (w) { strncpy((char*) b, w->text, a); if (a) ((char*) b)[a - 1] = 0; return strlen((char*) b); } return 0;
        case WM_GETTEXTLENGTH: return w ? strlen(w->text) : 0;
    }
    return 0;
}
LRESULT CallWindowProc(WNDPROC p, HWND h, UINT m, WPARAM a, LPARAM b) {
    return p ? p(h, m, a, b) : DefWindowProc(h, m, a, b);
}
BOOL ShowWindow(HWND h, int c) { Wnd* w = (Wnd*) h; if (w) { w->visible = c != SW_HIDE; if (w == main_wnd) need_paint = 1; } return 1; }
BOOL UpdateWindow(HWND h) { if ((Wnd*) h == main_wnd && need_paint) { need_paint = 0; send_to(main_wnd, WM_PAINT, 0, 0); } return 1; }
BOOL IsWindow(HWND h) { for (int i = 0; i < window_count; i++) if (windows[i] == (Wnd*) h) return 1; return 0; }
BOOL IsIconic(HWND) { return 0; }
BOOL EnableWindow(HWND, BOOL) { return 1; }
BOOL MoveWindow(HWND h, int x, int y, int w, int hh, BOOL) { Wnd* p = (Wnd*) h; if (p) { p->x = x; p->y = y; p->w = w; p->h = hh; } return 1; }
BOOL SetWindowPos(HWND h, HWND, int x, int y, int w, int hh, UINT f) {
    Wnd* p = (Wnd*) h;
    if (!p) return 0;
    if (!(f & SWP_NOMOVE)) { p->x = x; p->y = y; }
    if (!(f & SWP_NOSIZE)) { p->w = w; p->h = hh; }
    return 1;
}
HWND SetFocus(HWND h) { HWND o = (HWND) focus_wnd; focus_wnd = (Wnd*) h; return o; }
HWND GetFocus(void) { return (HWND) focus_wnd; }
HWND GetActiveWindow(void) { return (HWND) main_wnd; }
HWND GetForegroundWindow(void) { return (HWND) main_wnd; }
BOOL SetForegroundWindow(HWND) { return 1; }
BOOL BringWindowToTop(HWND) { return 1; }
HWND GetParent(HWND h) { return h ? (HWND) ((Wnd*) h)->parent : 0; }
HWND GetLastActivePopup(HWND h) { return h; }
HWND FindWindow(LPCSTR, LPCSTR) { return 0; }
DWORD GetWindowThreadProcessId(HWND, LPDWORD p) { if (p) *p = 1; return 1; }
HWND SetCapture(HWND h) { HWND o = (HWND) capture_wnd; capture_wnd = (Wnd*) h; return o; }
HWND GetCapture(void) { return (HWND) capture_wnd; }
BOOL ReleaseCapture(void) { capture_wnd = 0; return 1; }
LONG GetWindowLong(HWND h, int i) {
    Wnd* w = (Wnd*) h;
    if (!w) return 0;
    switch (i) {
        case GWL_STYLE: return w->style;
        case GWL_EXSTYLE: return w->exstyle;
        case GWL_WNDPROC: return (LONG) w->proc;
        case GWL_USERDATA: return w->user;
    }
    return 0;
}
LONG SetWindowLong(HWND h, int i, LONG v) {
    Wnd* w = (Wnd*) h;
    LONG o = GetWindowLong(h, i);
    if (!w) return 0;
    switch (i) {
        case GWL_STYLE: w->style = v; break;
        case GWL_EXSTYLE: w->exstyle = v; break;
        case GWL_WNDPROC: w->proc = (WNDPROC) v; break;
        case GWL_USERDATA: w->user = v; break;
    }
    return o;
}
DWORD SetClassLong(HWND, int, LONG) { return 0; }
BOOL GetClientRect(HWND h, LPRECT r) {
    Wnd* w = (Wnd*) h;
    r->left = r->top = 0;
    r->right = w ? w->w : aoe_w;
    r->bottom = w ? w->h : aoe_h;
    return 1;
}
BOOL GetWindowRect(HWND h, LPRECT r) {
    Wnd* w = (Wnd*) h;
    r->left = w ? w->x : 0;
    r->top = w ? w->y : 0;
    r->right = r->left + (w ? w->w : aoe_w);
    r->bottom = r->top + (w ? w->h : aoe_h);
    return 1;
}
BOOL ClientToScreen(HWND, LPPOINT) { return 1; }
BOOL ScreenToClient(HWND, LPPOINT) { return 1; }
BOOL InvalidateRect(HWND h, const RECT*, BOOL) { if (!h || (Wnd*) h == main_wnd) need_paint = 1; return 1; }
BOOL ValidateRect(HWND h, const RECT*) { if (!h || (Wnd*) h == main_wnd) need_paint = 0; return 1; }
BOOL GetUpdateRect(HWND, LPRECT r, BOOL) { if (r) { r->left = r->top = 0; r->right = aoe_w; r->bottom = aoe_h; } return need_paint; }
HDC BeginPaint(HWND h, LPPAINTSTRUCT p) { memset(p, 0, sizeof *p); p->rcPaint.right = aoe_w; p->rcPaint.bottom = aoe_h; need_paint = 0; p->hdc = GetDC(h); return p->hdc; }
BOOL EndPaint(HWND h, const PAINTSTRUCT* p) { ReleaseDC(h, p->hdc); return 1; }
BOOL SetWindowText(HWND h, LPCSTR s) { if (h) strncpy(((Wnd*) h)->text, s, 511); return 1; }
int GetWindowText(HWND h, LPSTR b, int n) { if (!h || !n) return 0; strncpy(b, ((Wnd*) h)->text, n); b[n - 1] = 0; return strlen(b); }
BOOL TranslateMessage(const MSG*) { return 1; }
LRESULT DispatchMessage(const MSG* m) {
    static int msgs = -1;
    if (msgs < 0) msgs = getenv("AOE_MSGS") != 0;
    if (msgs) fprintf(stderr, "aoemsg %x %x %lx at %lu\n", m->message, (unsigned) m->wParam, (long) m->lParam, (unsigned long) timeGetTime());
    if (m->message == WM_PAINT) need_paint = 0;
    Wnd* w = (Wnd*) m->hwnd;
    if (!w) w = main_wnd;
    if (m->message == WM_CHAR && focus_wnd && focus_wnd->proc == edit_proc) return send_to(focus_wnd, m->message, m->wParam, m->lParam);
    return send_to(w, m->message, m->wParam, m->lParam);
}
void PostQuitMessage(int) { fprintf(stderr, "aoe: quit asked from %p\n", __builtin_return_address(0)); quitting = 1; }
BOOL PostMessage(HWND h, UINT m, WPARAM a, LPARAM b) {
    // As Windows does: moves not yet taken are one move. Each message is a look at the game's idle work, a whole frame.
    static int moves_all = -1;
    if (moves_all < 0) { const char* e = getenv("AOE_MOVES"); moves_all = e && !strcmp(e, "all"); }
    if (m == WM_MOUSEMOVE && !moves_all && q_head != q_tail) {
        MSG* last = &queue[(q_tail + QUEUE - 1) % QUEUE];
        if (last->message == WM_MOUSEMOVE) { last->wParam = a; last->lParam = b; last->time = timeGetTime(); return 1; }
    }
    int t = (q_tail + 1) % QUEUE;
    if (t == q_head) return 0;
    MSG* q = &queue[q_tail];
    q->hwnd = h; q->message = m; q->wParam = a; q->lParam = b; q->time = timeGetTime();
    q_tail = t;
    return 1;
}
LRESULT SendMessage(HWND h, UINT m, WPARAM a, LPARAM b) {
    if (h == HWND_BROADCAST) return 0;
    return send_to((Wnd*) h, m, a, b);
}
static int take(LPMSG m, UINT remove) {
    if (q_head != q_tail) {
        *m = queue[q_head];
        if (remove & PM_REMOVE) {
            int waiting = (q_tail - q_head + QUEUE) % QUEUE;
            q_head = (q_head + 1) % QUEUE;
            if (m->message == WM_MOUSEMOVE) aoe_lat('m', waiting, 0);
            else if (m->message == WM_LBUTTONDOWN || m->message == WM_RBUTTONDOWN || m->message == WM_KEYDOWN) aoe_lat('d', waiting, m->message);
        }
        return 1;
    }
    if (quitting) {
        memset(m, 0, sizeof *m);
        m->message = WM_QUIT;
        return 1;
    }
    if (need_paint && main_wnd) {
        memset(m, 0, sizeof *m);
        m->hwnd = (HWND) main_wnd;
        m->message = WM_PAINT;
        if (remove & PM_REMOVE) need_paint = 0;
        return 1;
    }
    return 0;
}
BOOL PeekMessage(LPMSG m, HWND, UINT, UINT, UINT remove) {
    aoe_lat('e', 0, 0);
    aoe_music_look();
    if (q_head == q_tail) { if (tick_ms > 0) virtual_now += tick_ms; aoe_events(); }
    return take(m, remove);
}
BOOL GetMessage(LPMSG m, HWND, UINT, UINT) {
    while (!take(m, PM_REMOVE)) { aoe_music_look(); if (tick_ms > 0) virtual_now += tick_ms; aoe_events(); if (q_head == q_tail) aoe_idle(10); }
    return m->message != WM_QUIT;
}
UINT SetTimer(HWND, UINT, UINT, void*) { return 1; }
BOOL KillTimer(HWND, UINT) { return 1; }
}
HWND aoe_main_window(void) { return (HWND) main_wnd; }
// the machine's events as messages
void aoe_post(UINT m, WPARAM a, LPARAM b) { PostMessage((HWND) main_wnd, m, a, b); }

// ------------------------------------------------------------------ input as the game asks for it
unsigned char aoe_keys[256];
int aoe_mouse_x, aoe_mouse_y;
extern "C" {
SHORT GetKeyState(int k) { return aoe_keys[k & 255] ? (SHORT) 0x8000 : 0; }
SHORT GetAsyncKeyState(int k) { return aoe_keys[k & 255] ? (SHORT) 0x8000 : 0; }
BOOL GetKeyboardState(LPBYTE b) { for (int i = 0; i < 256; i++) b[i] = aoe_keys[i] ? 0x80 : 0; return 1; }
BOOL GetCursorPos(LPPOINT p) { p->x = aoe_mouse_x; p->y = aoe_mouse_y; return 1; }
BOOL SetCursorPos(int x, int y) { aoe_mouse_x = x; aoe_mouse_y = y; return 1; }
HCURSOR SetCursor(HCURSOR c) { return c; }
}

extern "C" int WinMain(HINSTANCE, HINSTANCE, char*, int);
static char cmd_line[256];
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#include <sys/mman.h>
static void on_fault(int s, siginfo_t* i, void* c) {
    ucontext_t* u = (ucontext_t*) c;
    fprintf(stderr, "aoe: signal %d at pc %lx ra %lx addr %lx\n", s, (long) u->uc_mcontext.__gregs[0], (long) u->uc_mcontext.__gregs[1], (long) i->si_addr);
    if (s != SIGALRM) _exit(139);
}
int main(int argc, char** argv) {
    setvbuf(stderr, 0, _IOLBF, 0);
    if (getenv("AOE_NOWINDOW")) {
        struct sigaction a;
        memset(&a, 0, sizeof a);
        a.sa_sigaction = on_fault;
        a.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &a, 0);
        sigaction(SIGILL, &a, 0);
        sigaction(SIGBUS, &a, 0);
        sigaction(SIGABRT, &a, 0);
        if (getenv("AOE_SAMPLE")) {
            a.sa_flags |= SA_RESTART;
            sigaction(SIGALRM, &a, 0);
            struct itimerval t = {{atoi(getenv("AOE_SAMPLE")), 0}, {atoi(getenv("AOE_SAMPLE")), 0}};
            setitimer(ITIMER_REAL, &t, 0);
        }
    }
    for (int i = 1; i < argc; i++) { strncat(cmd_line, argv[i], 200); strcat(cmd_line, " "); }
    int r = WinMain((HINSTANCE) 1, 0, cmd_line, SW_SHOW);
    aoe_window_close();
    fflush(0);
    _exit(r);
}

static unsigned rand_state = 1;
extern "C" int aoe_rand(void) { rand_state = rand_state * 214013 + 2531011; return (rand_state >> 16) & 0x7fff; }
// AOE_SEED=N: every seeding takes N, not the clock's: the same map and the same start in a run on the real clock (game_par.py)
extern "C" void aoe_srand(unsigned s) {
    static int fixed = -1; static unsigned seed;
    if (fixed < 0) { const char* e = getenv("AOE_SEED"); fixed = e != 0; seed = e ? (unsigned) atol(e) : 0; }
    rand_state = fixed ? seed : s;
}
unsigned aoe_rand_state(void) { return rand_state; }
