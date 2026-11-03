// DirectDraw and GDI over the machine's memory: a surface is 8-bit rows, the primary one the page
// the GPU draws into the window, a device context a surface and what is drawn with.
// Under AOE_NOWINDOW=1 (qemu, the host's counting) there is no window: AOE_DUMP=FILE writes the
// picture with its palette at each AOE_DUMP_EVERY'th Blt to the primary surface.
#include <stdarg.h>
#include <stdint.h>
#include "ddraw.h"
#include "dsound.h"
#include "aoe_port.h"
#undef open
#undef fopen
#include "aoe_fonts.h"

int aoe_w = 640, aoe_h = 480;
static PALETTEENTRY cur_pal[256];
static IDirectDrawSurface* primary;
static long blits;
// The surface the game draws on is the page itself (AOE_BACK=copy: a surface of its own, as on Windows): a
// frame's copy of up to 480 KB was a pass's stores many times over.
static IDirectDrawSurface* back_on_page;
static int back_copied = -1;

extern unsigned char* aoe_page(int w, int h);       // the window's page, or memory
extern void aoe_palette_set(const PALETTEENTRY* p); // the window's palette
extern void aoe_present(void);                      // the picture to the GPU
extern void aoe_before_blit(void);                  // what the GPU reads of the page is done with

// The script's "shot": the page and its palette as they are, kept in memory under a mark a host reads out of rvc_cpu's
// --save-ram (tools/aoe_shots.py): the machine has no way to give a picture to the host's files
extern "C" void aoe_shot(void) {
    if (!primary) return;
    static int n;
    unsigned bytes = 32 + 1024 + primary->w * primary->h;
    unsigned char* m = (unsigned char*) malloc(bytes);
    if (!m) return;
    memcpy(m, "AOESHOT!", 8);
    int hdr[4] = {n, primary->w, primary->h, 0};
    memcpy(m + 8, hdr, 16);
    memcpy(m + 32, cur_pal, 1024);
    for (int y = 0; y < primary->h; y++) memcpy(m + 1056 + y * primary->w, primary->bits + y * primary->pitch, primary->w);
    fprintf(stderr, "aoeshot: %d at %p, %dx%d\n", n++, (void*) m, primary->w, primary->h);
}

static void dump_picture(void) {
    static const char* name; static int read;
    if (!read) { read = 1; name = getenv("AOE_DUMP"); }
    if (!name || !primary) return;
    static int every = -1, count;
    if (every < 0) { const char* e = getenv("AOE_DUMP_EVERY"); every = e ? atoi(e) : 1; }
    if (++count % every) return;
    char path[256];
    snprintf(path, sizeof path, "%s.%05d", name, count / every);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int hdr[2] = {primary->w, primary->h};
    fwrite(hdr, 4, 2, f);
    fwrite(cur_pal, 4, 256, f);
    for (int y = 0; y < primary->h; y++) fwrite(primary->bits + y * primary->pitch, 1, primary->w, f);
    fclose(f);
}

HRESULT IDirectDrawPalette::SetEntries(DWORD, DWORD s, DWORD n, LPPALETTEENTRY p) {
    memcpy(entries + s, p, n * 4);
    memcpy(cur_pal + s, p, n * 4);
    aoe_palette_set(cur_pal);
    return DD_OK;
}
HRESULT IDirectDrawPalette::GetEntries(DWORD, DWORD s, DWORD n, LPPALETTEENTRY p) { memcpy(p, entries + s, n * 4); return DD_OK; }

ULONG IDirectDrawSurface::Release() {
    if (this == primary) primary = 0;
    else if (this == back_on_page) back_on_page = 0;
    else free(bits);
    delete this;
    return 0;
}
HRESULT IDirectDrawSurface::Lock(LPRECT r, LPDDSURFACEDESC d, DWORD, HANDLE) {
    if (this == primary || this == back_on_page) aoe_before_blit();
    d->dwWidth = w; d->dwHeight = h; d->lPitch = pitch;
    d->lpSurface = bits + (r ? r->top * pitch + r->left : 0);
    return DD_OK;
}
HRESULT IDirectDrawSurface::Unlock(LPVOID) { return DD_OK; }
HRESULT IDirectDrawSurface::Blt(LPRECT dr, IDirectDrawSurface* src, LPRECT sr, DWORD fl, LPDDBLTFX fx) {
    RECT all = {0, 0, w, h};
    if (!dr) dr = &all;
    static int trace = -1;
    if (trace < 0) trace = getenv("AOE_BLITS") != 0;
    if (trace) fprintf(stderr, "blt %p primary %p from %p fl %x %ld,%ld,%ld,%ld\n", this, primary, src, (unsigned) fl, (long) dr->left, (long) dr->top, (long) dr->right, (long) dr->bottom);
    if (this == primary || this == back_on_page) aoe_before_blit();
    if (this == primary && src && src == back_on_page) {
        blits++; dump_picture(); aoe_present();
        return DD_OK;
    }
    if (fl & DDBLT_COLORFILL) {
        for (int y = dr->top; y < dr->bottom; y++) memset(bits + y * pitch + dr->left, fx->dwFillColor, dr->right - dr->left);
    } else if (src) {
        RECT sall = {0, 0, src->w, src->h};
        if (!sr) sr = &sall;
        int n = sr->right - sr->left, rows = sr->bottom - sr->top;
        for (int y = 0; y < rows; y++) {
            unsigned char* d = bits + (dr->top + y) * pitch + dr->left;
            unsigned char* s = src->bits + (sr->top + y) * src->pitch + sr->left;
            if (fl & DDBLT_KEYSRC) {
                // Most of a keyed picture is the key (a message's text over the view, every frame): four of them at a time
                // are passed over by one word's test.
                unsigned char k = (unsigned char) src->key;
                unsigned k4 = k * 0x01010101u;
                static int bytes = -1;   // AOE_BLT=bytes: a byte at a time, the way before
                if (bytes < 0) { const char* e = getenv("AOE_BLT"); bytes = e && !strcmp(e, "bytes"); }
                int x = bytes ? n : 0;
                if (bytes) for (int i = 0; i < n; i++) if (s[i] != k) d[i] = s[i];
                for (; x < n && ((uintptr_t) (s + x) & 3); x++) if (s[x] != k) d[x] = s[x];
                for (; x + 4 <= n; x += 4) {
                    unsigned w = *(const unsigned*) (s + x);
                    if (w == k4) continue;
                    if ((w & 255) != k) d[x] = (unsigned char) w;
                    if (((w >> 8) & 255) != k) d[x + 1] = (unsigned char) (w >> 8);
                    if (((w >> 16) & 255) != k) d[x + 2] = (unsigned char) (w >> 16);
                    if ((w >> 24) != k) d[x + 3] = (unsigned char) (w >> 24);
                }
                for (; x < n; x++) if (s[x] != k) d[x] = s[x];
            } else memcpy(d, s, n);
        }
    }
    if (this == primary) { blits++; dump_picture(); aoe_present(); }
    return DD_OK;
}
HRESULT IDirectDrawSurface::BltFast(DWORD x, DWORD y, IDirectDrawSurface* src, LPRECT sr, DWORD fl) {
    RECT d = {(LONG) x, (LONG) y, (LONG) x + (sr ? sr->right - sr->left : src->w), (LONG) y + (sr ? sr->bottom - sr->top : src->h)};
    return Blt(&d, src, sr, (fl & DDBLTFAST_SRCCOLORKEY) ? DDBLT_KEYSRC : 0, 0);
}
HRESULT IDirectDrawSurface::Flip(IDirectDrawSurface*, DWORD) { return DD_OK; }
HRESULT IDirectDrawSurface::GetAttachedSurface(LPDDSCAPS, IDirectDrawSurface**) { return E_FAIL; }
HRESULT IDirectDrawSurface::GetSurfaceDesc(LPDDSURFACEDESC d) {
    d->dwWidth = w; d->dwHeight = h; d->lPitch = pitch;
    d->ddpfPixelFormat.dwRGBBitCount = 8;
    d->ddsCaps.dwCaps = primary == this ? DDSCAPS_PRIMARYSURFACE : DDSCAPS_SYSTEMMEMORY;
    return DD_OK;
}
HRESULT IDirectDrawSurface::GetCaps(LPDDSCAPS c) { c->dwCaps = primary == this ? DDSCAPS_PRIMARYSURFACE : DDSCAPS_SYSTEMMEMORY; return DD_OK; }
HRESULT IDirectDrawSurface::SetPalette(IDirectDrawPalette*) { return DD_OK; }
HRESULT IDirectDrawSurface::SetColorKey(DWORD, LPDDCOLORKEY k) { key = k ? k->dwColorSpaceLowValue : 0; has_key = k != 0; return DD_OK; }

HRESULT IDirectDraw::SetCooperativeLevel(HWND, DWORD) { return DD_OK; }
HRESULT IDirectDraw::SetDisplayMode(DWORD w, DWORD h, DWORD) { aoe_w = w; aoe_h = h; return DD_OK; }
HRESULT IDirectDraw::RestoreDisplayMode() { return DD_OK; }
HRESULT IDirectDraw::GetDisplayMode(LPDDSURFACEDESC d) {
    d->dwWidth = aoe_w; d->dwHeight = aoe_h; d->lPitch = aoe_w;
    d->ddpfPixelFormat.dwRGBBitCount = 8;
    return DD_OK;
}
HRESULT IDirectDraw::CreateSurface(LPDDSURFACEDESC d, IDirectDrawSurface** out, IUnknown*) {
    IDirectDrawSurface* s = new IDirectDrawSurface;
    s->gdi = 0; s->has_key = 0; s->key = 0; s->is_primary = 0;
    if (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
        s->w = aoe_w; s->h = aoe_h; s->pitch = aoe_w;
        s->is_primary = 1;
        s->bits = aoe_page(aoe_w, aoe_h);
        primary = s;
    } else {
        s->w = d->dwWidth; s->h = d->dwHeight; s->pitch = (s->w + 3) & ~3;
        if (back_copied < 0) { const char* e = getenv("AOE_BACK"); back_copied = e && !strcmp(e, "copy"); }
        if (primary && !back_on_page && !back_copied && s->w == primary->w && s->h == primary->h && s->pitch == primary->pitch) {
            s->bits = primary->bits;
            back_on_page = s;
        } else
        s->bits = (unsigned char*) calloc(s->pitch, s->h + 1);
    }
    *out = s;
    return DD_OK;
}
HRESULT IDirectDraw::CreatePalette(DWORD, LPPALETTEENTRY p, IDirectDrawPalette** out, IUnknown*) {
    IDirectDrawPalette* n = new IDirectDrawPalette;
    memcpy(n->entries, p, sizeof n->entries);
    *out = n;
    return DD_OK;
}
HRESULT IDirectDraw::CreateClipper(DWORD, IDirectDrawClipper** out, IUnknown*) { *out = new IDirectDrawClipper; return DD_OK; }
HRESULT IDirectDraw::GetCaps(LPDDCAPS a, LPDDCAPS) { DWORD n = a->dwSize; memset(a, 0, sizeof *a); a->dwSize = n; return DD_OK; }
HRESULT aoe_DirectDrawCreate(GUID*, IDirectDraw** out, IUnknown*) { *out = new IDirectDraw; return DD_OK; }


// ------------------------------------------------------------------ GDI
struct GdiObj { int kind; COLORREF color; int style; const AoeFont* font; RECT rect; PALETTEENTRY* pal; };
enum { K_PEN = 1, K_BRUSH, K_FONT, K_RGN, K_PAL, K_BMP };
struct Dc {
    IDirectDrawSurface* s;
    const AoeFont* font;
    COLORREF text, bk;
    int bkmode, align, rop;
    int has_clip;
    RECT clip;
    int px, py;
    GdiObj* pen;
    GdiObj* brush;
    HPALETTE pal;
};

static int nearest(COLORREF c, const PALETTEENTRY* pal) {
    if ((c >> 24) == 1) return c & 255;
    static COLORREF last_c[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    static int last_i[4], next_i;
    static const PALETTEENTRY* last_pal;
    static unsigned last_sum;
    unsigned pal_sum = pal[0].peRed + pal[7].peGreen * 3 + pal[64].peBlue * 5 + pal[255].peRed * 7;   // (a palette changed in place)
    if (pal != last_pal || pal_sum != last_sum) { last_pal = pal; last_sum = pal_sum; for (int i = 0; i < 4; i++) last_c[i] = 0xffffffff; }
    for (int i = 0; i < 4; i++) if (c == last_c[i]) return last_i[i];
    int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = r - pal[i].peRed, dg = g - pal[i].peGreen, db = b - pal[i].peBlue;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    last_c[next_i] = c; last_i[next_i] = best; next_i = (next_i + 1) & 3;
    return best;
}

static GdiObj stock_black_pen = {K_PEN, 0}, stock_white_pen = {K_PEN, 0xffffff}, stock_null_pen = {K_PEN, 0, PS_NULL};
static GdiObj stock_black_brush = {K_BRUSH, 0}, stock_white_brush = {K_BRUSH, 0xffffff}, stock_null_brush = {K_BRUSH, 0, 1};
static GdiObj stock_font = {K_FONT};

extern "C" {
HDC GetDC(HWND) {
    Dc* d = (Dc*) calloc(1, sizeof(Dc));
    d->s = primary; d->font = aoe_font_get("arial", 14, 0); d->bkmode = OPAQUE; d->bk = 0xffffff; d->pen = &stock_black_pen; d->brush = &stock_white_brush;
    return d;
}
int ReleaseDC(HWND, HDC d) { free(d); return 1; }
HDC CreateCompatibleDC(HDC) { return GetDC(0); }
BOOL DeleteDC(HDC d) { free(d); return 1; }
HDC CreateICA(LPCSTR, LPCSTR, LPCSTR, const void*) { return GetDC(0); }
HGDIOBJ GetStockObject(int i) {
    switch (i) {
        case BLACK_PEN: return &stock_black_pen;
        case WHITE_PEN: return &stock_white_pen;
        case NULL_PEN: return &stock_null_pen;
        case BLACK_BRUSH: return &stock_black_brush;
        case WHITE_BRUSH: return &stock_white_brush;
        case NULL_BRUSH: return &stock_null_brush;
    }
    return &stock_font;
}
HGDIOBJ SelectObject(HDC h, HGDIOBJ o) {
    Dc* d = (Dc*) h;
    GdiObj* g = (GdiObj*) o;
    if (!g) return 0;
    switch (g->kind) {
        case K_PEN: { GdiObj* old = d->pen; d->pen = g; return old; }
        case K_BRUSH: { GdiObj* old = d->brush; d->brush = g; return old; }
        case K_FONT: {
            static GdiObj holder[64];
            static int at;
            GdiObj* old = &holder[at++ & 63];
            old->kind = K_FONT; old->font = d->font;
            if (g->font) d->font = g->font;
            return old;
        }
    }
    return 0;
}
BOOL DeleteObject(HGDIOBJ o) {
    GdiObj* g = (GdiObj*) o;
    if (g && g != &stock_black_pen && g != &stock_white_pen && g != &stock_null_pen && g != &stock_black_brush && g != &stock_white_brush && g != &stock_null_brush && g != &stock_font && g->kind != K_FONT) free(g);
    return 1;
}
int GetObject(HGDIOBJ, int, LPVOID) { return 0; }
int GetDeviceCaps(HDC, int i) {
    switch (i) {
        case BITSPIXEL: return 8;
        case PLANES: return 1;
        case HORZRES: return aoe_w;
        case VERTRES: return aoe_h;
        case RASTERCAPS: return RC_PALETTE;
        case SIZEPALETTE: return 256;
        case NUMRESERVED: return 20;
        case LOGPIXELSX: case LOGPIXELSY: return 96;
    }
    return 0;
}
int SetBkMode(HDC h, int m) { Dc* d = (Dc*) h; int o = d->bkmode; d->bkmode = m; return o; }
COLORREF SetBkColor(HDC h, COLORREF c) { Dc* d = (Dc*) h; COLORREF o = d->bk; d->bk = c; return o; }
COLORREF SetTextColor(HDC h, COLORREF c) { Dc* d = (Dc*) h; COLORREF o = d->text; d->text = c; return o; }
UINT SetTextAlign(HDC h, UINT a) { Dc* d = (Dc*) h; UINT o = d->align; d->align = a; return o; }
int SetROP2(HDC h, int r) { Dc* d = (Dc*) h; int o = d->rop; d->rop = r; return o; }
int SetStretchBltMode(HDC, int) { return 1; }

static void hline(Dc* d, int x0, int x1, int y, int color) {
    if (!d->s) return;
    RECT c = {0, 0, d->s->w, d->s->h};
    if (d->has_clip) { if (d->clip.left > c.left) c.left = d->clip.left; if (d->clip.top > c.top) c.top = d->clip.top; if (d->clip.right < c.right) c.right = d->clip.right; if (d->clip.bottom < c.bottom) c.bottom = d->clip.bottom; }
    if (y < c.top || y >= c.bottom) return;
    if (x0 < c.left) x0 = c.left;
    if (x1 >= c.right) x1 = c.right - 1;
    if (x0 <= x1) memset(d->s->bits + y * d->s->pitch + x0, color, x1 - x0 + 1);
}
int aoe_text_width(const AoeFont* f, const char* s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) w += aoe_glyph_width(f, (unsigned char) s[i]);
    return w;
}
static void draw_text(Dc* d, int x, int y, const char* s, int n) {
    const AoeFont* f = d->font;
    if (!f || !d->s) return;
    if (n < 0) n = strlen(s);
    int w = aoe_text_width(f, s, n);
    if ((d->align & 6) == TA_RIGHT) x -= w;
    if ((d->align & 6) == TA_CENTER) x -= w / 2;
    if ((d->align & 24) == TA_BOTTOM) y -= f->height;
    if ((d->align & 24) == TA_BASELINE) y -= f->ascent;
    int color = nearest(d->text, cur_pal), bg = nearest(d->bk, cur_pal);
    RECT c = {0, 0, d->s->w, d->s->h};
    if (d->has_clip) { if (d->clip.left > c.left) c.left = d->clip.left; if (d->clip.top > c.top) c.top = d->clip.top; if (d->clip.right < c.right) c.right = d->clip.right; if (d->clip.bottom < c.bottom) c.bottom = d->clip.bottom; }
    if (d->bkmode == OPAQUE) for (int r = 0; r < f->height; r++) hline(d, x, x + w - 1, y + r, bg);
    for (int i = 0; i < n; i++) {
        unsigned char ch = s[i];
        int gw = aoe_glyph_width(f, ch);
        const unsigned char* bits = aoe_glyph_bits(f, ch);
        int stride = (gw + 7) / 8;
        for (int r = 0; r < f->height; r++, bits += stride) {
            int yy = y + r;
            if (yy < c.top || yy >= c.bottom) continue;
            unsigned char* row = d->s->bits + yy * d->s->pitch;
            for (int b = 0; b < gw; b++) {
                if (!(bits[b >> 3] & (0x80 >> (b & 7)))) continue;
                int xx = x + b;
                if (xx >= c.left && xx < c.right) row[xx] = color;
            }
        }
        x += gw;
    }
}
BOOL TextOutA(HDC h, int x, int y, LPCSTR s, int n) { draw_text((Dc*) h, x, y, s, n); return 1; }
BOOL ExtTextOutA(HDC h, int x, int y, UINT, const RECT*, LPCSTR s, UINT n, const int*) { draw_text((Dc*) h, x, y, s, n); return 1; }
BOOL GetTextExtentPoint32A(HDC h, LPCSTR s, int n, LPSIZE z) {
    Dc* d = (Dc*) h;
    z->cx = aoe_text_width(d->font, s, n); z->cy = d->font ? d->font->height : 12;
    return 1;
}
BOOL GetTextExtentPointA(HDC h, LPCSTR s, int n, LPSIZE z) { return GetTextExtentPoint32A(h, s, n, z); }
BOOL GetTextMetricsA(HDC h, LPTEXTMETRIC t) {
    Dc* d = (Dc*) h;
    memset(t, 0, sizeof *t);
    const AoeFont* f = d->font;
    t->tmHeight = f ? f->height : 12; t->tmAscent = f ? f->ascent : 10; t->tmDescent = t->tmHeight - t->tmAscent;
    t->tmAveCharWidth = f ? aoe_glyph_width(f, 'x') : 6; t->tmMaxCharWidth = f ? aoe_glyph_width(f, 'W') : 10;
    t->tmWeight = FW_NORMAL; t->tmLastChar = 255; t->tmDefaultChar = '?'; t->tmBreakChar = ' ';
    return 1;
}
int DrawTextA(HDC h, LPCSTR s, int n, LPRECT r, UINT fl) {
    Dc* d = (Dc*) h;
    const AoeFont* f = d->font;
    if (!f) return 0;
    if (n < 0) n = strlen(s);
    int lh = f->height, y = r->top, width = r->right - r->left, top = y;
    int save = d->align;
    d->align = 0;
    const char* p = s;
    const char* end = s + n;
    while (p < end) {
        // one line: to a line's end, or as many words as fit
        const char* q = p;
        const char* brk = 0;
        int w = 0;
        while (q < end && *q != '\n' && *q != '\r') {
            int g = aoe_glyph_width(f, (unsigned char) *q);
            if ((fl & DT_WORDBREAK) && w + g > width && (brk || q > p)) break;
            if (*q == ' ') brk = q;
            w += g;
            q++;
        }
        if ((fl & DT_WORDBREAK) && q < end && *q != '\n' && *q != '\r' && brk) q = brk;
        int lw = aoe_text_width(f, p, q - p);
        if (!(fl & DT_CALCRECT)) {
            int x = r->left;
            if (fl & DT_CENTER) x += (width - lw) / 2;
            else if (fl & DT_RIGHT) x += width - lw;
            int yy = y;
            if (fl & DT_SINGLELINE) {
                if (fl & DT_VCENTER) yy = r->top + (r->bottom - r->top - lh) / 2;
                else if (fl & DT_BOTTOM) yy = r->bottom - lh;
            }
            draw_text(d, x, yy, p, q - p);
        }
        y += lh;
        p = q;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
        else if (p < end && *p == ' ') p++;
    }
    d->align = save;
    if (fl & DT_CALCRECT) {
        if (fl & DT_SINGLELINE) r->right = r->left + aoe_text_width(f, s, n);
        r->bottom = r->top + (y - top);
    }
    return y - top;
}
int DrawTextExA(HDC h, LPSTR s, int n, LPRECT r, UINT fl, void*) { return DrawTextA(h, s, n, r, fl); }
HFONT CreateFontIndirectA(const LOGFONT* l) {
    GdiObj* g = (GdiObj*) calloc(1, sizeof(GdiObj));
    g->kind = K_FONT;
    int size = l->lfHeight < 0 ? -l->lfHeight : l->lfHeight;
    g->font = aoe_font_get(l->lfFaceName, size, l->lfWeight >= FW_BOLD);
    if (getenv("AOE_FILES")) fprintf(stderr, "aoefont: %s %d w%d\n", l->lfFaceName, (int) l->lfHeight, (int) l->lfWeight);
    return g;
}
HPEN CreatePen(int style, int, COLORREF c) { GdiObj* g = (GdiObj*) calloc(1, sizeof(GdiObj)); g->kind = K_PEN; g->color = c; g->style = style; return g; }
HBRUSH CreateSolidBrush(COLORREF c) { GdiObj* g = (GdiObj*) calloc(1, sizeof(GdiObj)); g->kind = K_BRUSH; g->color = c; return g; }
HRGN CreateRectRgn(int a, int b, int c, int e) { GdiObj* g = (GdiObj*) calloc(1, sizeof(GdiObj)); g->kind = K_RGN; g->rect.left = a; g->rect.top = b; g->rect.right = c; g->rect.bottom = e; return g; }
int SelectClipRgn(HDC h, HRGN r) {
    Dc* d = (Dc*) h;
    if (!r) { d->has_clip = 0; return 1; }
    d->has_clip = 1; d->clip = ((GdiObj*) r)->rect;
    return 2;
}
BOOL MoveToEx(HDC h, int x, int y, LPPOINT p) { Dc* d = (Dc*) h; if (p) { p->x = d->px; p->y = d->py; } d->px = x; d->py = y; return 1; }
BOOL LineTo(HDC h, int x, int y) {
    Dc* d = (Dc*) h;
    if (d->pen && d->pen->style == PS_NULL) { d->px = x; d->py = y; return 1; }
    int c = nearest(d->pen ? d->pen->color : 0, cur_pal), x0 = d->px, y0 = d->py;
    int dx = abs(x - x0), dy = abs(y - y0), sx = x0 < x ? 1 : -1, sy = y0 < y ? 1 : -1, e = dx - dy;
    while (x0 != x || y0 != y) {
        hline(d, x0, x0, y0, c);
        int e2 = e * 2;
        if (e2 > -dy) { e -= dy; x0 += sx; }
        if (e2 < dx) { e += dx; y0 += sy; }
    }
    d->px = x; d->py = y;
    return 1;
}
int FillRect(HDC h, const RECT* r, HBRUSH b) {
    Dc* d = (Dc*) h;
    int c = nearest(b ? ((GdiObj*) b)->color : 0, cur_pal);
    for (int y = r->top; y < r->bottom; y++) hline(d, r->left, r->right - 1, y, c);
    return 1;
}
BOOL SetRect(LPRECT r, int a, int b, int c, int e) { r->left = a; r->top = b; r->right = c; r->bottom = e; return 1; }
int StretchDIBits(HDC, int, int, int, int, int, int, int, int, const void*, const BITMAPINFO*, UINT, DWORD) { return 0; }
int GetDIBits(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT) { return 0; }
HPALETTE CreatePalette(const LOGPALETTE* l) {
    GdiObj* g = (GdiObj*) calloc(1, sizeof(GdiObj));
    g->kind = K_PAL;
    g->pal = (PALETTEENTRY*) calloc(256, 4);
    memcpy(g->pal, l->palPalEntry, l->palNumEntries * 4);
    return g;
}
HPALETTE CreateHalftonePalette(HDC) { static LOGPALETTE l = {0x300, 0}; return CreatePalette(&l); }
BOOL ResizePalette(HPALETTE, UINT) { return 1; }
UINT GetPaletteEntries(HPALETTE p, UINT s, UINT n, LPPALETTEENTRY o) { memcpy(o, ((GdiObj*) p)->pal + s, n * 4); return n; }
UINT SetPaletteEntries(HPALETTE p, UINT s, UINT n, const PALETTEENTRY* e) { memcpy(((GdiObj*) p)->pal + s, e, n * 4); return n; }
UINT GetNearestPaletteIndex(HPALETTE p, COLORREF c) {
    const PALETTEENTRY* pal = ((GdiObj*) p)->pal;
    if ((c >> 24) == 1) return c & 255;
    int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = r - pal[i].peRed, dg = g - pal[i].peGreen, db = b - pal[i].peBlue;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
HPALETTE SelectPalette(HDC h, HPALETTE p, BOOL) { Dc* d = (Dc*) h; HPALETTE o = d->pal; d->pal = p; return o; }
UINT RealizePalette(HDC) { return 256; }
UINT GetSystemPaletteEntries(HDC, UINT s, UINT n, LPPALETTEENTRY o) { memcpy(o, cur_pal + s, n * 4); return n; }
}
void aoe_gdi_surface(HDC h, IDirectDrawSurface* s) { ((Dc*) h)->s = s; }
HRESULT IDirectDrawSurface::GetDC(HDC* out) {
    if (this == primary || this == back_on_page) aoe_before_blit();
    *out = ::GetDC(0);
    aoe_gdi_surface(*out, this);
    return DD_OK;
}
HRESULT IDirectDrawSurface::ReleaseDC(HDC d) { ::ReleaseDC(0, d); return DD_OK; }

// the modes: 800 x 600, the size of the only full pictures the trial has, and 640 x 480, a window on a desktop (scr_game.cpp cuts them)
HRESULT IDirectDraw::EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID ctx, long (*cb)(_DDSURFACEDESC*, void*)) {
    DDSURFACEDESC d;
    memset(&d, 0, sizeof d);
    d.ddpfPixelFormat.dwRGBBitCount = 8;
    d.dwWidth = 800; d.dwHeight = 600;
    cb(&d, ctx);
    d.dwWidth = 640; d.dwHeight = 480; cb(&d, ctx);
    return DD_OK;
}
